#include "Core/BuiltinTheme.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeAssetResolver.h"
#include "Core/ThemeManager.h"
#include "Widgets/Meter/PeakMeterRenderer.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <thread>

namespace {

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

bool writeImage(const QString& path, const QColor& color,
                const QSize& size = QSize(8, 8))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    return image.save(path);
}

bool writePackage(const QString& themesRoot,
                  const QString& directoryName,
                  const QString& id,
                  const QByteArray& assets,
                  const std::optional<QString>& baseThemeId = std::nullopt)
{
    const QString packageRoot = QDir(themesRoot).filePath(directoryName);
    const QByteArray base = baseThemeId
        ? QByteArray(",\"baseThemeId\":\"") + baseThemeId->toUtf8() + '"'
        : QByteArray();
    const QByteArray manifest = QByteArray("{")
        + "\"schemaVersion\":1,\"id\":\"" + id.toUtf8()
        + "\",\"displayName\":\"Test " + id.toUtf8()
        + "\",\"version\":\"1.0.0\"" + base
        + ",\"compatibility\":{\"harkenVersion\":\">=0.1.0 <1.0.0\","
          "\"capabilities\":[\"curve-v1\",\"meter-v1\",\"waveform-v1\"]},"
          "\"entries\":{\"tokens\":\"tokens.json\",\"visuals\":{"
          "\"curve\":\"visuals/curve.json\","
          "\"meter\":\"visuals/meter.json\","
          "\"waveform\":\"visuals/waveform.json\"},"
          "\"assetMap\":\"assets.json\"}}";
    return writeFile(QDir(packageRoot).filePath(QStringLiteral("manifest.json")),
                     manifest)
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("tokens.json")),
                     R"json({"colors":{"accent.primary":"#ff00f0ff"}})json")
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/curve.json")),
                     R"json({"colors":{"curve.line":"#ff112233"}})json")
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/meter.json")),
                     R"json({"colors":{"meter.green":"#ff22aa44"}})json")
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/waveform.json")),
                     R"json({"colors":{"waveform.line":"#ff334455"}})json")
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("assets.json")),
                     assets);
}

std::unique_ptr<VNX::ISettingsStorage> memoryStorage()
{
    return std::make_unique<VNX::MemorySettingsStorage>();
}

}

TEST(ThemeAssetResolver, EveryCatalogImageHasStableBuiltinFallback)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(root.path(), memoryStorage());

    for (const auto& entry : VNX::themeAssetCatalog()) {
        if (entry.kind != VNX::ThemeAssetKind::Image)
            continue;
        const auto result = manager.resolveImage(
            entry.logicalId, VNX::ThemeAssetVariant{QSize(20, 20)});
        EXPECT_TRUE(result.ok()) << entry.logicalId.toStdString();
        if (result.resource)
            EXPECT_TRUE(result.resource->builtinFallback);
    }
}

TEST(ThemeAssetResolver, IconEngineUsesPaletteModesAndToggleStates)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(root.path(), memoryStorage());

    const QPalette originalPalette = qApp->palette();
    QPalette palette = originalPalette;
    palette.setColor(QPalette::Active, QPalette::ButtonText, Qt::red);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, Qt::green);
    qApp->setPalette(palette);

    const auto icon = manager.resolveIcon(
        QString::fromLatin1(VNX::ThemeAssetIds::AuditionPlay));
    const auto toggle = manager.resolveToggleIcon(
        QString::fromLatin1(VNX::ThemeAssetIds::TreeFolderOpen),
        QString::fromLatin1(VNX::ThemeAssetIds::TreeFolderClosed));

    ASSERT_TRUE(icon.ok());
    EXPECT_FALSE(icon.icon.pixmap(QSize(20, 20), QIcon::Normal).isNull());
    EXPECT_FALSE(icon.icon.pixmap(QSize(20, 20), QIcon::Disabled).isNull());
    ASSERT_TRUE(toggle.ok());
    EXPECT_NE(toggle.icon.pixmap(QSize(20, 20), QIcon::Normal, QIcon::On)
                  .cacheKey(),
              toggle.icon.pixmap(QSize(20, 20), QIcon::Normal, QIcon::Off)
                  .cacheKey());
    qApp->setPalette(originalPalette);
}

TEST(ThemeAssetResolver, SvgTintPreservesFixedColors)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"audition.play":{"type":"image","sources":[{"path":"multicolor.svg"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("svg"),
                             QStringLiteral("theme.svg"), assets));
    ASSERT_TRUE(writeFile(
        QDir(root.path()).filePath(QStringLiteral("svg/multicolor.svg")),
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10"><rect width="10" height="10" fill="currentColor"/><rect x="10" width="10" height="10" fill="#ff0000"/></svg>)svg"));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.svg")).ok());

    const auto result = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("audition.play"),
        VNX::ThemeAssetVariant{QSize(20, 10), 1.0, 0, Qt::green});

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.resource->image.pixelColor(5, 5), QColor(Qt::green));
    EXPECT_EQ(result.resource->image.pixelColor(15, 5), QColor(Qt::red));
}

TEST(ThemeAssetResolver, ComboArrowFollowsThemeOverrideAndReloadWithoutStaleSvg)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"navigation.chevron.down":{"type":"image","sources":[{"path":"arrow.svg"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("arrow"),
                             QStringLiteral("theme.arrow"), assets));
    const QString sourcePath = QDir(root.path()).filePath(QStringLiteral("arrow/arrow.svg"));
    const QByteArray source = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect x="3" y="4" width="10" height="8" fill="currentColor"/></svg>)svg";
    ASSERT_TRUE(writeFile(sourcePath, source));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.arrow")).ok());

    const auto arrowPath = [] {
        const QRegularExpression rule(QStringLiteral(
            R"qss(QComboBox::down-arrow\s*\{\s*image: url\("([^"]+)"\);)qss"));
        return rule.match(qApp->styleSheet()).captured(1);
    };
    const QString oldPath = arrowPath();
    ASSERT_TRUE(oldPath.endsWith(QStringLiteral(".svg")));
    QFile oldFile(oldPath);
    ASSERT_TRUE(oldFile.open(QIODevice::ReadOnly));
    const QByteArray oldBytes = oldFile.readAll();
    EXPECT_EQ(oldBytes, QByteArray(source).replace(
        "currentColor", manager.colors().textSecondary.name(QColor::HexRgb).toLatin1()));

    ASSERT_TRUE(writeFile(sourcePath, QByteArray(source).replace("width=\"10\"", "width=\"14\"")));
    ASSERT_TRUE(manager.reloadTheme().ok());
    const QString newPath = arrowPath();
    EXPECT_NE(newPath, oldPath);
    QFile newFile(newPath);
    ASSERT_TRUE(newFile.open(QIODevice::ReadOnly));
    EXPECT_TRUE(newFile.readAll().contains("width=\"14\""));
    ASSERT_TRUE(oldFile.seek(0));
    EXPECT_EQ(oldFile.readAll(), oldBytes);
    ASSERT_TRUE(manager.selectTheme(VNX::builtinThemeId()).ok());
}

TEST(ThemeAssetResolver, ResolvesInheritedAssetAndClosestDpiVariant)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray baseAssets = R"json({"assets":{"meter.peak.cap":{"type":"image","sources":[{"path":"red.png"}]}}})json";
    const QByteArray childAssets = R"json({"assets":{"audition.play":{"type":"image","sources":[{"path":"one.png","scale":1},{"path":"two.png","scale":2}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("base"),
                             QStringLiteral("theme.base"), baseAssets));
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("child"),
                             QStringLiteral("theme.child"), childAssets,
                             QStringLiteral("theme.base")));
    ASSERT_TRUE(writeImage(QDir(root.path()).filePath(
        QStringLiteral("base/red.png")), Qt::red));
    ASSERT_TRUE(writeImage(QDir(root.path()).filePath(
        QStringLiteral("child/one.png")), Qt::green));
    ASSERT_TRUE(writeImage(QDir(root.path()).filePath(
        QStringLiteral("child/two.png")), Qt::blue));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.child")).ok());

    const auto inherited = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("meter.peak.cap"));
    const auto dpi = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("audition.play"),
        VNX::ThemeAssetVariant{QSize(6, 6), 1.8});

    ASSERT_TRUE(inherited.ok());
    ASSERT_TRUE(dpi.ok());
    EXPECT_EQ(inherited.resource->image.pixelColor(0, 0), QColor(Qt::red));
    EXPECT_EQ(dpi.resource->image.pixelColor(0, 0), QColor(Qt::blue));
    EXPECT_EQ(dpi.resource->image.size(), QSize(11, 11));
    EXPECT_DOUBLE_EQ(dpi.resource->image.devicePixelRatio(), 1.8);
}

TEST(ThemeAssetResolver, RejectsZeroAreaRequestedVariant)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(root.path(), memoryStorage());

    const auto result = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("audition.play"),
        VNX::ThemeAssetVariant{QSize(0, 8)});

    EXPECT_FALSE(result.ok());
    ASSERT_FALSE(result.diagnostics.isEmpty());
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::InvalidValue);
}

TEST(ThemeAssetResolver, UsesExplicitBuiltinFallbackAndReportsUnknownId)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(root.path(), memoryStorage());

    const auto builtin = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("audition.play"),
        VNX::ThemeAssetVariant{QSize(20, 20), 1.0});
    const auto unknown = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("unknown.asset"));

    ASSERT_TRUE(builtin.ok());
    EXPECT_TRUE(builtin.resource->builtinFallback);
    EXPECT_FALSE(unknown.ok());
    ASSERT_FALSE(unknown.diagnostics.isEmpty());
    EXPECT_EQ(unknown.diagnostics.back().code,
              VNX::ThemePackageErrorCode::AssetNotFound);
}

TEST(ThemeAssetResolver, CorruptOverrideReportsDiagnosticAndFallsBack)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"audition.play":{"type":"image","sources":[{"path":"broken.png"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("broken"),
                             QStringLiteral("theme.broken"), assets));
    ASSERT_TRUE(writeFile(QDir(root.path()).filePath(
        QStringLiteral("broken/broken.png")), QByteArrayLiteral("not-png")));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.broken")).ok());

    const auto result = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("audition.play"),
        VNX::ThemeAssetVariant{QSize(20, 20)});

    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.resource->builtinFallback);
    ASSERT_FALSE(result.diagnostics.isEmpty());
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::AssetDecodeFailed);
}

TEST(ThemeAssetResolver, MissingSourceReportsDiagnosticAndFallsBack)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"audition.stop":{"type":"image","sources":[{"path":"stop.png"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("missing"),
                             QStringLiteral("theme.missing"), assets));
    const QString imagePath = QDir(root.path()).filePath(
        QStringLiteral("missing/stop.png"));
    ASSERT_TRUE(writeImage(imagePath, Qt::red));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.missing")).ok());
    ASSERT_TRUE(QFile::remove(imagePath));

    const auto result = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("audition.stop"),
        VNX::ThemeAssetVariant{QSize(20, 20)});

    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.resource->builtinFallback);
    ASSERT_FALSE(result.diagnostics.isEmpty());
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::AssetNotFound);
}

TEST(ThemeAssetResolver, RejectsDecodedDimensionsAboveLimit)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"meter.peak.cap":{"type":"image","sources":[{"path":"wide.png"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("wide"),
                             QStringLiteral("theme.wide"), assets));
    ASSERT_TRUE(writeImage(QDir(root.path()).filePath(
        QStringLiteral("wide/wide.png")), Qt::red, QSize(4097, 1)));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.wide")).ok());

    const auto result = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("meter.peak.cap"));

    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.resource->builtinFallback);
    ASSERT_FALSE(result.diagnostics.isEmpty());
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::AssetDimensionsExceeded);
}

TEST(ThemeAssetResolver, ReloadSeparatesCacheAndKeepsOldHandleAlive)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"meter.peak.cap":{"type":"image","sources":[{"path":"cap.png"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("reload"),
                             QStringLiteral("theme.reload"), assets));
    const QString imagePath = QDir(root.path()).filePath(
        QStringLiteral("reload/cap.png"));
    ASSERT_TRUE(writeImage(imagePath, Qt::red));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.reload")).ok());
    const auto oldSnapshot = manager.snapshot();
    const auto oldResource = manager.assetResolver().resolveImage(
        oldSnapshot, QStringLiteral("meter.peak.cap")).resource;
    ASSERT_NE(oldResource, nullptr);
    ASSERT_TRUE(writeImage(imagePath, Qt::blue));

    ASSERT_TRUE(manager.reloadTheme().ok());
    const auto currentResource = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("meter.peak.cap")).resource;

    ASSERT_NE(currentResource, nullptr);
    EXPECT_NE(currentResource, oldResource);
    EXPECT_NE(currentResource->snapshot->generation,
              oldResource->snapshot->generation);
    EXPECT_EQ(oldResource->image.pixelColor(0, 0), QColor(Qt::red));
    EXPECT_EQ(currentResource->image.pixelColor(0, 0), QColor(Qt::blue));
}

TEST(ThemeAssetResolver, RejectsRuntimePathEscape)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString packageRoot = QDir(root.path()).filePath(QStringLiteral("theme"));
    QDir().mkpath(packageRoot);
    ASSERT_TRUE(writeImage(QDir(root.path()).filePath(
        QStringLiteral("outside.png")), Qt::red));
    VNX::ThemeSnapshot snapshotValue = VNX::createBuiltinDarkTheme();
    snapshotValue.generation = 42;
    const auto snapshot = std::make_shared<const VNX::ThemeSnapshot>(snapshotValue);
    VNX::ThemeAssetMapDto assets;
    VNX::ThemeAssetDto asset;
    asset.sources.append({QStringLiteral("theme.test/../outside.png"), 1.0});
    assets.assets.insert(QStringLiteral("audition.play"), asset);
    VNX::ThemeAssetResolver resolver;
    resolver.registerSnapshot(snapshot, assets,
                              {{QStringLiteral("theme.test"), packageRoot}});

    const auto result = resolver.resolveImage(
        snapshot, QStringLiteral("audition.play"),
        VNX::ThemeAssetVariant{QSize(20, 20)});

    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.resource->builtinFallback);
    ASSERT_FALSE(result.diagnostics.isEmpty());
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::PathEscape);
}

TEST(ThemeAssetResolver, CreatesQtIconOnlyOnGuiThread)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(root.path(), memoryStorage());

    const auto result = manager.assetResolver().resolveIcon(
        manager.snapshot(), QStringLiteral("audition.play"),
        VNX::ThemeAssetVariant{QSize(20, 20), 1.0, 0, Qt::cyan});

    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.icon.isNull());

    const auto snapshot = manager.snapshot();
    std::optional<VNX::ThemeIconResolution> workerResult;
    std::thread worker([&] {
        workerResult = manager.assetResolver().resolveIcon(
            snapshot, QStringLiteral("audition.stop"),
            VNX::ThemeAssetVariant{QSize(20, 20)});
    });
    worker.join();

    ASSERT_TRUE(workerResult.has_value());
    EXPECT_FALSE(workerResult->ok());
    ASSERT_FALSE(workerResult->diagnostics.isEmpty());
    EXPECT_EQ(workerResult->diagnostics.back().code,
              VNX::ThemePackageErrorCode::AssetThreadViolation);
}

TEST(ThemeAssetResolver, ResolvesNinePatchAndFrameSequenceResources)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"panel.background":{"type":"ninePatch","insets":{"left":2,"top":1,"right":2,"bottom":1},"sources":[{"path":"panel.png"}]},"parameter.knob.frame":{"type":"frameSequence","sources":[{"path":"frame0.png"},{"path":"frame1.png"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("composite"),
                             QStringLiteral("theme.composite"), assets));
    const QDir package(QDir(root.path()).filePath(QStringLiteral("composite")));
    ASSERT_TRUE(writeImage(package.filePath(QStringLiteral("panel.png")),
                           Qt::yellow, QSize(8, 6)));
    ASSERT_TRUE(writeImage(package.filePath(QStringLiteral("frame0.png")),
                           Qt::red));
    ASSERT_TRUE(writeImage(package.filePath(QStringLiteral("frame1.png")),
                           Qt::blue));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.composite")).ok());

    const auto panel = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("panel.background"));
    const auto frame = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("parameter.knob.frame"),
        VNX::ThemeAssetVariant{QSize(), 1.0, 1});

    ASSERT_TRUE(panel.ok());
    ASSERT_TRUE(panel.resource->insets.has_value());
    EXPECT_EQ(panel.resource->insets->left, 2);
    EXPECT_EQ(panel.resource->insets->bottom, 1);
    ASSERT_TRUE(frame.ok());
    EXPECT_EQ(frame.resource->image.pixelColor(0, 0), QColor(Qt::blue));
}

TEST(ThemeAssetResolver, ValidatesDeclaredFileTypeAndFontDecode)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"ui.body":{"type":"font","sources":[{"path":"body.ttf"}]},"bad.image":{"type":"image","sources":[{"path":"body.ttf"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("font"),
                             QStringLiteral("theme.font"), assets));
    ASSERT_TRUE(writeFile(QDir(root.path()).filePath(
        QStringLiteral("font/body.ttf")), QByteArrayLiteral("not-a-font")));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.font")).ok());

    const auto font = manager.assetResolver().resolveFont(
        manager.snapshot(), QStringLiteral("ui.body"));
    const auto image = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("bad.image"));

    EXPECT_FALSE(font.ok());
    ASSERT_FALSE(font.diagnostics.isEmpty());
    EXPECT_EQ(font.diagnostics.back().code,
              VNX::ThemePackageErrorCode::AssetDecodeFailed);
    EXPECT_FALSE(image.ok());
    ASSERT_FALSE(image.diagnostics.isEmpty());
    EXPECT_EQ(image.diagnostics.back().code,
              VNX::ThemePackageErrorCode::AssetTypeMismatch);
}

TEST(ThemeAssetResolver, PeakRendererConsumesResolvedLogicalAsset)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"meter.peak.cap":{"type":"image","sources":[{"path":"cap.png"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("renderer"),
                             QStringLiteral("theme.renderer"), assets));
    ASSERT_TRUE(writeImage(QDir(root.path()).filePath(
        QStringLiteral("renderer/cap.png")), QColor(QStringLiteral("#ffff00ff")),
        QSize(4, 2)));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.renderer")).ok());
    VNX::PeakMeterRenderFrame frame;
    frame.canvasBounds = QRect(0, 0, 40, 40);
    frame.bars[0].bounds = QRect(0, 0, 15, 40);
    frame.bars[0].showHold = true;
    frame.bars[0].holdY = 10.0;
    frame.bars[1].bounds = QRect(20, 0, 15, 40);
    QImage baseline(40, 40, QImage::Format_ARGB32_Premultiplied);
    baseline.fill(Qt::transparent);
    {
        QPainter painter(&baseline);
        VNX::PeakMeterRenderer::render(
            painter, manager.snapshot()->domains.meter, frame);
    }
    frame.peakCap = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("meter.peak.cap")).resource;
    QImage themed(40, 40, QImage::Format_ARGB32_Premultiplied);
    themed.fill(Qt::transparent);
    {
        QPainter painter(&themed);
        VNX::PeakMeterRenderer::render(
            painter, manager.snapshot()->domains.meter, frame);
    }

    EXPECT_NE(themed, baseline);
    EXPECT_EQ(themed.pixelColor(5, 10), QColor(QStringLiteral("#ffff00ff")));
}

TEST(ThemeAssetResolver, ParsesNinePatchAndFrameSequenceContracts)
{
    const auto result = VNX::parseThemeAssetMap(R"json({"assets":{
        "panel.bg":{"type":"ninePatch","insets":{"left":2,"top":3,"right":4,"bottom":5},"sources":[{"path":"panel.png"}]},
        "parameter.knob.frame":{"type":"frameSequence","sources":[{"path":"0.png"},{"path":"1.png"}]}
    }})json");

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->assets.value(QStringLiteral("panel.bg")).kind,
              VNX::ThemeAssetKind::NinePatch);
    EXPECT_EQ(result.value->assets.value(QStringLiteral("panel.bg"))
                  .insets->left, 2);
    EXPECT_EQ(result.value->assets.value(
                  QStringLiteral("parameter.knob.frame")).kind,
              VNX::ThemeAssetKind::FrameSequence);
}
