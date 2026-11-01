#include "ThemeAssetResolver.h"

#include "ThemeAssetCatalog.h"
#include "Utils/SvgIconEngine.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPixmap>
#include <QSet>
#include <QThread>
#include <QSvgRenderer>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>

namespace VNX {
namespace {

ThemePackageDiagnostic diagnostic(ThemePackageErrorCode code,
                                  const QString& file,
                                  const QString& path,
                                  const QString& message)
{
    return ThemePackageDiagnostic{code, file, path, message};
}

bool isContained(const QString& parent, const QString& child)
{
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    QString prefix = QDir::fromNativeSeparators(parent);
    const QString normalizedChild = QDir::fromNativeSeparators(child);
    if (QString::compare(prefix, normalizedChild, sensitivity) == 0)
        return true;
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix += QLatin1Char('/');
    return normalizedChild.startsWith(prefix, sensitivity);
}

QString variantKey(const ThemeAssetVariant& variant)
{
    return QStringLiteral("%1x%2@%3:%4:%5")
        .arg(variant.logicalSize.width())
        .arg(variant.logicalSize.height())
        .arg(QString::number(variant.devicePixelRatio, 'f', 3))
        .arg(variant.frameIndex)
        .arg(variant.tint.isValid()
                 ? QString::number(variant.tint.rgba(), 16)
                 : QStringLiteral("none"));
}

bool imageKind(ThemeAssetKind kind)
{
    return kind == ThemeAssetKind::Image
        || kind == ThemeAssetKind::NinePatch
        || kind == ThemeAssetKind::FrameSequence;
}

bool imageExtension(const QString& suffix)
{
    static const QSet<QString> extensions = {
        QStringLiteral("png"), QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("webp"),
        QStringLiteral("bmp"), QStringLiteral("gif"),
        QStringLiteral("svg")};
    return extensions.contains(suffix.toLower());
}

bool fontExtension(const QString& suffix)
{
    return suffix.compare(QStringLiteral("ttf"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("otf"), Qt::CaseInsensitive) == 0;
}

QImage tinted(QImage image, const QColor& tint)
{
    if (!tint.isValid() || image.isNull())
        return image;
    QImage result(image.size(), QImage::Format_ARGB32_Premultiplied);
    result.setDevicePixelRatio(image.devicePixelRatio());
    result.fill(0);
    {
        QPainter painter(&result);
        painter.drawImage(0, 0, image);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(result.rect(), tint);
    }
    return result;
}

}

class ThemeAssetResolver::Impl
{
public:
    struct Catalog
    {
        std::weak_ptr<const ThemeSnapshot> snapshot;
        ThemeAssetMapDto assets;
        QMap<QString, QString> roots;
        quint64 generation = 0;
    };

    explicit Impl(ThemeAssetResolverLimits configuredLimits)
        : limits(configuredLimits)
    {
    }

    std::optional<Catalog> catalogFor(const ThemeSnapshotPtr& snapshot)
    {
        QMutexLocker lock(&mutex);
        const auto it = catalogs.constFind(snapshot->generation);
        if (it == catalogs.constEnd() || it->snapshot.lock() != snapshot)
            return std::nullopt;
        return *it;
    }

    std::optional<QByteArray> readSource(
        const Catalog& catalog,
        const QString& logicalPath,
        bool expectImage,
        QList<ThemePackageDiagnostic>& diagnostics) const
    {
        const qsizetype separator = logicalPath.indexOf(QLatin1Char('/'));
        if (separator <= 0)
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::PathEscape, logicalPath,
                QStringLiteral("$"),
                QStringLiteral("Asset source has no package identity.")));
            return std::nullopt;
        }
        const QString packageId = logicalPath.left(separator);
        const QString relative = logicalPath.mid(separator + 1);
        const QString root = catalog.roots.value(packageId);
        const QString rootCanonical = QFileInfo(root).canonicalFilePath();
        const QFileInfo fileInfo(QDir(rootCanonical).filePath(relative));
        const QString fileAbsolute = QDir::cleanPath(fileInfo.absoluteFilePath());
        if (rootCanonical.isEmpty()
            || !isContained(rootCanonical, fileAbsolute))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::PathEscape, logicalPath,
                QStringLiteral("$"),
                QStringLiteral("Asset source resolves outside its package root.")));
            return std::nullopt;
        }
        if (!fileInfo.isFile())
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::AssetNotFound, logicalPath,
                QStringLiteral("$"),
                QStringLiteral("Asset source does not exist.")));
            return std::nullopt;
        }
        const QString fileCanonical = fileInfo.canonicalFilePath();
        if (fileCanonical.isEmpty()
            || !isContained(rootCanonical, fileCanonical))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::PathEscape, logicalPath,
                QStringLiteral("$"),
                QStringLiteral("Asset source resolves outside its package root.")));
            return std::nullopt;
        }
        const QString suffix = fileInfo.suffix();
        if ((expectImage && !imageExtension(suffix))
            || (!expectImage && !fontExtension(suffix)))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::AssetTypeMismatch, logicalPath,
                QStringLiteral("$"),
                QStringLiteral("Asset file type does not match its declaration.")));
            return std::nullopt;
        }
        if (fileInfo.size() < 0 || fileInfo.size() > limits.maximumEncodedBytes)
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::ResourceLimit, logicalPath,
                QStringLiteral("$"),
                QStringLiteral("Encoded asset exceeds the read budget.")));
            return std::nullopt;
        }
        QFile file(fileCanonical);
        if (!file.open(QIODevice::ReadOnly))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::AssetNotFound, logicalPath,
                QStringLiteral("$"),
                QStringLiteral("Asset source cannot be read.")));
            return std::nullopt;
        }
        return file.readAll();
    }

    std::optional<QImage> decodeImage(
        QByteArray bytes,
        const QString& sourcePath,
        const ThemeAssetVariant& variant,
        QByteArray& svgData,
        QList<ThemePackageDiagnostic>& diagnostics) const
    {
        svgData.clear();
        if (QFileInfo(sourcePath).suffix().compare(
                QStringLiteral("svg"), Qt::CaseInsensitive) == 0)
        {
            if (variant.tint.isValid()) {
                bytes.replace("currentColor", variant.tint.name(QColor::HexRgb).toLatin1());
                if (variant.tint.alpha() < 255) {
                    QByteArray composed;
                    QXmlStreamReader reader(bytes);
                    QXmlStreamWriter writer(&composed);
                    int depth = 0;
                    while (!reader.atEnd()) {
                        reader.readNext();
                        if (reader.isEndElement() && --depth == 0)
                            writer.writeEndElement();
                        writer.writeCurrentToken(reader);
                        if (reader.isStartElement() && depth++ == 0) {
                            writer.writeStartElement(QStringLiteral("g"));
                            writer.writeAttribute(QStringLiteral("opacity"),
                                QString::number(variant.tint.alphaF(), 'g', 8));
                        }
                    }
                    if (reader.hasError()) {
                        diagnostics.append(diagnostic(ThemePackageErrorCode::AssetDecodeFailed,
                            sourcePath, QStringLiteral("$"), reader.errorString()));
                        return std::nullopt;
                    }
                    bytes = std::move(composed);
                }
            }
            QSvgRenderer renderer(bytes);
            if (!renderer.isValid())
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::AssetDecodeFailed, sourcePath,
                    QStringLiteral("$"),
                    QStringLiteral("Asset image cannot be decoded.")));
                return std::nullopt;
            }
            QSize pixels = renderer.defaultSize();
            if (variant.logicalSize.isValid())
                pixels = QSize(
                    qRound(variant.logicalSize.width()
                           * variant.devicePixelRatio),
                    qRound(variant.logicalSize.height()
                           * variant.devicePixelRatio));
            if (!pixels.isValid() || pixels.width() > limits.maximumDimension
                || pixels.height() > limits.maximumDimension
                || static_cast<qint64>(pixels.width()) * pixels.height()
                    > limits.maximumPixels)
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::AssetDimensionsExceeded, sourcePath,
                    QStringLiteral("$"),
                    QStringLiteral("Decoded asset dimensions exceed the limit.")));
                return std::nullopt;
            }
            QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
            image.fill(0);
            {
                QPainter painter(&image);
                renderer.render(&painter);
            }
            image.setDevicePixelRatio(
                std::max<qreal>(1.0, variant.devicePixelRatio));
            svgData = std::move(bytes);
            return image;
        }
        QBuffer buffer;
        buffer.setData(bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, QFileInfo(sourcePath).suffix().toLatin1());
        reader.setDecideFormatFromContent(true);
        const QSize declared = reader.size();
        if (declared.isValid()
            && (declared.width() > limits.maximumDimension
                || declared.height() > limits.maximumDimension
                || static_cast<qint64>(declared.width()) * declared.height()
                    > limits.maximumPixels))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::AssetDimensionsExceeded, sourcePath,
                QStringLiteral("$"),
                QStringLiteral("Decoded asset dimensions exceed the limit.")));
            return std::nullopt;
        }
        QImage image = reader.read();
        if (image.isNull())
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::AssetDecodeFailed, sourcePath,
                QStringLiteral("$"),
                QStringLiteral("Asset image cannot be decoded.")));
            return std::nullopt;
        }
        if (image.width() > limits.maximumDimension
            || image.height() > limits.maximumDimension
            || static_cast<qint64>(image.width()) * image.height()
                > limits.maximumPixels)
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::AssetDimensionsExceeded, sourcePath,
                QStringLiteral("$"),
                QStringLiteral("Decoded asset dimensions exceed the limit.")));
            return std::nullopt;
        }
        if (variant.logicalSize.isValid())
        {
            const QSize pixels(
                qRound(variant.logicalSize.width() * variant.devicePixelRatio),
                qRound(variant.logicalSize.height() * variant.devicePixelRatio));
            if (pixels.width() <= 0 || pixels.height() <= 0
                || pixels.width() > limits.maximumDimension
                || pixels.height() > limits.maximumDimension
                || static_cast<qint64>(pixels.width()) * pixels.height()
                    > limits.maximumPixels)
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::AssetDimensionsExceeded, sourcePath,
                    QStringLiteral("$"),
                    QStringLiteral("Requested asset variant exceeds the limit.")));
                return std::nullopt;
            }
            image = image.scaled(pixels, Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
        }
        image.setDevicePixelRatio(std::max<qreal>(1.0, variant.devicePixelRatio));
        return tinted(std::move(image), variant.tint);
    }

    ThemeAssetResolverLimits limits;
    QMutex mutex;
    QMap<quint64, Catalog> catalogs;
    QHash<QString, std::weak_ptr<const ThemeImageResource>> imageCache;
    QHash<QString, std::weak_ptr<const ThemeFontResource>> fontCache;
};

ThemeAssetResolver::ThemeAssetResolver(ThemeAssetResolverLimits limits)
    : impl_(std::make_unique<Impl>(limits))
{
}

ThemeAssetResolver::~ThemeAssetResolver() = default;

void ThemeAssetResolver::registerSnapshot(
    const ThemeSnapshotPtr& snapshot,
    ThemeAssetMapDto assets,
    QMap<QString, QString> packageRoots,
    const ThemeSnapshotPtr& sharedAssets)
{
    static std::atomic<quint64> nextGeneration{0};
    QMutexLocker lock(&impl_->mutex);
    const auto generation = sharedAssets
        ? impl_->catalogs.value(sharedAssets->generation).generation
        : ++nextGeneration;
    Q_ASSERT(generation != 0);
    for (auto it = impl_->catalogs.begin(); it != impl_->catalogs.end();)
    {
        if (it->snapshot.expired())
            it = impl_->catalogs.erase(it);
        else
            ++it;
    }
    impl_->catalogs.insert(snapshot->generation, Impl::Catalog{
        snapshot, std::move(assets), std::move(packageRoots), generation});
}

quint64 ThemeAssetResolver::assetGeneration(const ThemeSnapshotPtr& snapshot) const
{
    const auto catalog = snapshot ? impl_->catalogFor(snapshot) : std::nullopt;
    return catalog ? catalog->generation : 0;
}

ThemeImageResolution ThemeAssetResolver::resolveImage(
    const ThemeSnapshotPtr& snapshot,
    const QString& logicalId,
    const ThemeAssetVariant& variant)
{
    ThemeImageResolution result;
    if (!snapshot)
        return result;
    const bool hasLogicalSize = variant.logicalSize != QSize();
    if (!std::isfinite(variant.devicePixelRatio)
        || variant.devicePixelRatio <= 0.0
        || (hasLogicalSize
            && (!variant.logicalSize.isValid()
                || variant.logicalSize.isEmpty()))
        || variant.frameIndex < 0)
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::InvalidValue, logicalId,
            QStringLiteral("$.variant"),
            QStringLiteral("Asset variant values are invalid.")));
        return result;
    }
    const QString cacheKey = QStringLiteral("%1:%2:%3")
        .arg(assetGeneration(snapshot)).arg(logicalId, variantKey(variant));
    {
        QMutexLocker lock(&impl_->mutex);
        if (const auto cached = impl_->imageCache.value(cacheKey).lock())
        {
            result.resource = cached;
            return result;
        }
    }

    const auto catalog = impl_->catalogFor(snapshot);
    if (!catalog)
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::AssetNotFound, logicalId,
            QStringLiteral("$"),
            QStringLiteral("Snapshot asset catalog is unavailable.")));
        return result;
    }

    std::optional<ThemeAssetDto> asset;
    const auto assetIt = catalog->assets.assets.constFind(logicalId);
    if (assetIt != catalog->assets.assets.constEnd())
        asset = *assetIt;
    QImage image;
    QByteArray svgData;
    std::optional<ThemeAssetInsetsDto> insets;
    const ThemeAssetCatalogEntry* builtin = findThemeAssetCatalogEntry(logicalId);
    if (asset && builtin && !builtin->themeOverridable)
        asset.reset();
    if (asset)
    {
        if (!imageKind(asset->kind))
        {
            result.diagnostics.append(diagnostic(
                ThemePackageErrorCode::AssetTypeMismatch, logicalId,
                QStringLiteral("$.type"),
                QStringLiteral("Requested asset is not an image resource.")));
        }
        else if (asset->sources.isEmpty())
        {
            result.diagnostics.append(diagnostic(
                ThemePackageErrorCode::InvalidValue, logicalId,
                QStringLiteral("$.sources"),
                QStringLiteral("Image asset has no sources.")));
        }
        else
        {
            qsizetype sourceIndex = 0;
            if (asset->kind == ThemeAssetKind::FrameSequence)
            {
                if (variant.frameIndex < 0
                    || variant.frameIndex >= asset->sources.size())
                    result.diagnostics.append(diagnostic(
                        ThemePackageErrorCode::InvalidValue, logicalId,
                        QStringLiteral("$.frameIndex"),
                        QStringLiteral("Frame index is outside the sequence.")));
                else
                    sourceIndex = variant.frameIndex;
            }
            else
            {
                qreal bestDistance = std::numeric_limits<qreal>::max();
                for (qsizetype index = 0; index < asset->sources.size(); ++index)
                {
                    const qreal distance = std::abs(
                        asset->sources.at(index).scale - variant.devicePixelRatio);
                    if (distance < bestDistance)
                    {
                        bestDistance = distance;
                        sourceIndex = index;
                    }
                }
            }
            if (result.diagnostics.isEmpty())
            {
                const auto& source = asset->sources.at(sourceIndex);
                const auto bytes = impl_->readSource(
                    *catalog, source.path, true, result.diagnostics);
                if (bytes)
                {
                    const auto decoded = impl_->decodeImage(
                        *bytes, source.path, variant, svgData, result.diagnostics);
                    if (decoded)
                    {
                        const qreal decodedWidth = decoded->width()
                            / decoded->devicePixelRatio();
                        const qreal decodedHeight = decoded->height()
                            / decoded->devicePixelRatio();
                        if (asset->kind == ThemeAssetKind::NinePatch
                            && asset->insets
                            && (asset->insets->left + asset->insets->right
                                    > decodedWidth
                                || asset->insets->top + asset->insets->bottom
                                    > decodedHeight))
                        {
                            result.diagnostics.append(diagnostic(
                                ThemePackageErrorCode::AssetDimensionsExceeded,
                                logicalId, QStringLiteral("$.insets"),
                                QStringLiteral("Nine-patch insets exceed decoded dimensions.")));
                        }
                        else
                            image = *decoded;
                    }
                }
                insets = asset->insets;
            }
        }
    }

    bool builtinFallback = false;
    if (image.isNull() && builtin)
    {
        svgData.clear();
        if (builtin->fallback == ThemeBuiltinAssetFallback::TransparentPixel)
        {
            image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
            image.fill(0);
            image.setDevicePixelRatio(
                std::max<qreal>(1.0, variant.devicePixelRatio));
        }
        else
        {
            QFile file(builtin->builtinPath);
            if (file.open(QIODevice::ReadOnly))
            {
                const auto decoded = impl_->decodeImage(
                    file.readAll(), builtin->builtinPath, variant,
                    svgData, result.diagnostics);
                if (decoded)
                    image = *decoded;
            }
        }
        builtinFallback = !image.isNull();
    }
    if (image.isNull())
    {
        if (!asset)
            result.diagnostics.append(diagnostic(
                ThemePackageErrorCode::AssetNotFound, logicalId,
                QStringLiteral("$"),
                QStringLiteral("Logical asset ID has no theme or built-in resource.")));
        return result;
    }

    auto resource = std::make_shared<ThemeImageResource>();
    resource->snapshot = snapshot;
    resource->logicalId = logicalId;
    resource->variant = variant;
    resource->image = std::move(image);
    resource->svgData = std::move(svgData);
    resource->insets = insets;
    resource->builtinFallback = builtinFallback;
    result.resource = std::move(resource);
    {
        QMutexLocker lock(&impl_->mutex);
        impl_->imageCache.insert(cacheKey, result.resource);
    }
    return result;
}

ThemeIconResolution ThemeAssetResolver::resolveIcon(
    const ThemeSnapshotPtr& snapshot,
    const QString& logicalId,
    const ThemeAssetVariant& variant)
{
    ThemeIconResolution result;
    QCoreApplication* application = QCoreApplication::instance();
    if (!qobject_cast<QGuiApplication*>(application)
        || QThread::currentThread() != application->thread())
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::AssetThreadViolation, logicalId,
            QStringLiteral("$"),
            QStringLiteral("QIcon creation must run on the GUI thread.")));
        return result;
    }
    ThemeImageResolution image = resolveImage(snapshot, logicalId, variant);
    result.imageResource = image.resource;
    result.diagnostics = std::move(image.diagnostics);
    if (result.imageResource)
        result.icon = QIcon(new SvgIconEngine(
            this, snapshot, logicalId, QString(), variant.tint, result.imageResource));
    return result;
}

ThemeIconResolution ThemeAssetResolver::resolveToggleIcon(
    const ThemeSnapshotPtr& snapshot,
    const QString& onLogicalId,
    const QString& offLogicalId,
    const ThemeAssetVariant& variant)
{
    ThemeIconResolution result;
    QCoreApplication* application = QCoreApplication::instance();
    if (!qobject_cast<QGuiApplication*>(application)
        || QThread::currentThread() != application->thread())
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::AssetThreadViolation, onLogicalId,
            QStringLiteral("$"),
            QStringLiteral("QIcon creation must run on the GUI thread.")));
        return result;
    }
    ThemeImageResolution on = resolveImage(snapshot, onLogicalId, variant);
    ThemeImageResolution off = resolveImage(snapshot, offLogicalId, variant);
    result.imageResource = on.resource;
    result.diagnostics = std::move(on.diagnostics);
    result.diagnostics.append(off.diagnostics);
    if (on.resource && off.resource)
        result.icon = QIcon(new SvgIconEngine(
            this, snapshot, onLogicalId, offLogicalId, variant.tint, on.resource, off.resource));
    return result;
}

ThemeFontResolution ThemeAssetResolver::resolveFont(
    const ThemeSnapshotPtr& snapshot,
    const QString& logicalId,
    qreal pixelSize)
{
    ThemeFontResolution result;
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::AssetThreadViolation, logicalId,
            QStringLiteral("$"),
            QStringLiteral("Font decoding requires a QGuiApplication instance.")));
        return result;
    }
    if (!snapshot)
        return result;
    if (!std::isfinite(pixelSize) || pixelSize <= 0.0)
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::InvalidValue, logicalId,
            QStringLiteral("$.pixelSize"),
            QStringLiteral("Font pixel size must be positive and finite.")));
        return result;
    }
    const QString cacheKey = QStringLiteral("%1:%2:%3")
        .arg(assetGeneration(snapshot))
        .arg(logicalId)
        .arg(QString::number(pixelSize, 'f', 3));
    {
        QMutexLocker lock(&impl_->mutex);
        if (const auto cached = impl_->fontCache.value(cacheKey).lock())
        {
            result.resource = cached;
            return result;
        }
    }
    const auto catalog = impl_->catalogFor(snapshot);
    if (!catalog)
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::AssetNotFound, logicalId,
            QStringLiteral("$"),
            QStringLiteral("Snapshot asset catalog is unavailable.")));
        return result;
    }
    const auto assetIt = catalog->assets.assets.constFind(logicalId);
    if (assetIt == catalog->assets.assets.constEnd())
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::AssetNotFound, logicalId,
            QStringLiteral("$"), QStringLiteral("Logical font ID was not found.")));
        return result;
    }
    if (assetIt->kind != ThemeAssetKind::Font)
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::AssetTypeMismatch, logicalId,
            QStringLiteral("$.type"),
            QStringLiteral("Requested asset is not a font resource.")));
        return result;
    }
    if (assetIt->sources.isEmpty())
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::InvalidValue, logicalId,
            QStringLiteral("$.sources"),
            QStringLiteral("Font asset has no sources.")));
        return result;
    }
    const auto bytes = impl_->readSource(
        *catalog, assetIt->sources.front().path, false, result.diagnostics);
    if (!bytes)
        return result;
    QRawFont font(*bytes, pixelSize, QFont::PreferDefaultHinting);
    if (!font.isValid())
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::AssetDecodeFailed, logicalId,
            QStringLiteral("$"), QStringLiteral("Font cannot be decoded.")));
        return result;
    }
    auto resource = std::make_shared<ThemeFontResource>();
    resource->snapshot = snapshot;
    resource->logicalId = logicalId;
    resource->data = *bytes;
    resource->font = std::move(font);
    result.resource = std::move(resource);
    {
        QMutexLocker lock(&impl_->mutex);
        impl_->fontCache.insert(cacheKey, result.resource);
    }
    return result;
}

}
