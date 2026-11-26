#include "ThemeFontAssets.h"

#include "ThemeAssetResolver.h"
#include "ThemeFontAlias.h"
#include "ThemePackageLoader.h"
#include "ThemeStyleTokens.h"

#include <QApplicationStatic>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <utility>

namespace harken::studio {

struct PreparedThemeFonts
{
    struct Source
    {
        QString logicalPath;
        QString canonicalPath;
        qint64 size;
        std::filesystem::file_time_type modified;
        bool operator==(const Source&) const = default;
    };
    QList<Source> sources;
    QList<std::shared_ptr<const ThemeFontAssetFamily>> families;
};

namespace {

struct FontFiles
{
    QString name;
    QMap<QByteArray, QByteArray> faces;
};

using FamilyPtr = std::shared_ptr<const ThemeFontAssetFamily>;

struct FontRegistry
{
    QMap<QByteArray, std::weak_ptr<const ThemeFontAssetFamily>> families;
    QMutex mutex;
    QHash<QString, int> aliasCounts;
};

Q_APPLICATION_STATIC(FontRegistry, registry)

struct RegisteredFamily : ThemeFontAssetFamily
{
    QPointer<QGuiApplication> owner;
    QList<int> registrations;
    QSet<QString> aliases;

    void hideNames(const QStringList& names)
    {
        QMutexLocker lock(&registry->mutex);
        for (const auto& name : names) {
            if (!aliases.contains(name)) {
                aliases.insert(name);
                ++registry->aliasCounts[name];
            }
        }
    }

    ~RegisteredFamily()
    {
        if (!owner || QCoreApplication::closingDown())
            return;
        QMetaObject::invokeMethod(owner, [ids = std::move(registrations), names = std::move(aliases)] {
            if (QCoreApplication::closingDown())
                return;
            for (int id : ids)
                QFontDatabase::removeApplicationFont(id);
            QMutexLocker lock(&registry->mutex);
            for (const auto& name : names) {
                auto it = registry->aliasCounts.find(name);
                if (--it.value() == 0)
                    registry->aliasCounts.erase(it);
            }
        }, Qt::AutoConnection);
    }
};

FamilyPtr registerFamily(const FontFiles& files, QList<ThemePackageDiagnostic>& diagnostics)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto& [digest, bytes] : files.faces.asKeyValueRange())
        hash.addData(digest);
    const auto digest = hash.result();
    if (auto existing = registry->families.value(digest).lock())
        return existing;
    auto family = std::make_shared<RegisteredFamily>();
    family->owner = qGuiApp;
    family->name = files.name;
    family->renderFamily = QStringLiteral("Harken") + QString::fromLatin1(digest.toHex().left(24));
    family->hideNames({family->renderFamily});
    for (const auto& face : files.faces) {
        const auto aliased = aliasThemeFont(face, family->renderFamily);
        const int id = aliased.isEmpty() ? -1 : QFontDatabase::addApplicationFontFromData(aliased);
        if (id >= 0) {
            family->registrations.append(id);
            const auto names = QFontDatabase::applicationFontFamilies(id);
            if (names.contains(family->renderFamily)) {
                family->hideNames(names);
                continue;
            }
        }
        diagnostics.append({ThemePackageErrorCode::AssetDecodeFailed, files.name, QStringLiteral("$.assets"),
            QStringLiteral("The theme font family could not be registered.")});
        return {};
    }
    registry->families.removeIf([](auto it) { return it.value().expired(); });
    registry->families.insert(digest, family);
    return family;
}

}

QStringList installedSystemFontFamilies()
{
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance()) || QCoreApplication::closingDown())
        return {};
    auto result = QFontDatabase::families();
    QHash<QString, int> aliases;
    {
        QMutexLocker lock(&registry->mutex);
        aliases = registry->aliasCounts;
    }
    result.removeIf([&aliases](const QString& name) {
        return name.size() > ThemeFontOverride::maximumFamilyLength
            || aliases.contains(name) || QFontDatabase::isPrivateFamily(name);
    });
    return result;
}

bool prepareThemeFonts(ThemePackageCandidate& candidate, QList<ThemePackageDiagnostic>& diagnostics,
                       const ThemePackageCandidate* previous)
{
    if (candidate.preparedFonts)
        return true;
    ThemeAssetResolver resolver;
    constexpr qint64 maximumPackageFontBytes = 128 * 1024 * 1024;
    qint64 totalBytes = 0;
    bool canReuseSources = true;
    QSet<QString> readPaths;
    QList<PreparedThemeFonts::Source> sources;
    for (const auto& asset : std::as_const(candidate.assets.assets)) {
        if (asset.kind != ThemeAssetKind::Font)
            continue;
        const auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
        if (!app || QThread::currentThread() != app->thread()) {
            diagnostics.append({ThemePackageErrorCode::AssetThreadViolation, candidate.manifest.id,
                QStringLiteral("$.assets"), QStringLiteral("Theme font registration requires the GUI thread.")});
            return false;
        }
        for (const auto& source : asset.sources) {
            if (readPaths.contains(source.path))
                continue;
            readPaths.insert(source.path);
            const auto file = resolver.fontSourceFile(candidate.assetRoots, source.path, diagnostics);
            if (!file)
                return false;
            if (file->size > maximumPackageFontBytes - totalBytes) {
                diagnostics.append({ThemePackageErrorCode::ResourceLimit, source.path, QStringLiteral("$"),
                    QStringLiteral("Theme font assets exceed the package read budget.")});
                return false;
            }
            totalBytes += file->size;
            std::error_code error;
            const auto modified = std::filesystem::last_write_time(QFileInfo(file->path).filesystemFilePath(), error);
            if (error)
                canReuseSources = false;
            sources.append({source.path, file->path, file->size, modified});
        }
    }

    std::shared_ptr<const PreparedThemeFonts> prepared;
    if (canReuseSources && previous && previous->manifest.id == candidate.manifest.id
        && previous->preparedFonts && previous->preparedFonts->sources == sources)
        prepared = previous->preparedFonts;
    if (!sources.isEmpty() && !prepared) {
        auto next = std::make_shared<PreparedThemeFonts>();
        next->sources = std::move(sources);
        QList<FontFiles> families;
        for (const auto& source : std::as_const(next->sources)) {
            auto bytes = resolver.readFontSource(candidate.assetRoots, source.logicalPath, diagnostics);
            if (!bytes)
                return false;
            if (bytes->size() != source.size) {
                diagnostics.append({ThemePackageErrorCode::AssetDecodeFailed, source.logicalPath, QStringLiteral("$"),
                    QStringLiteral("The font source changed while preparing the theme.")});
                return false;
            }
            QRawFont raw(*bytes, 12, QFont::PreferDefaultHinting);
            const QString name = raw.familyName();
            if (!raw.isValid() || name.isEmpty()) {
                diagnostics.append({ThemePackageErrorCode::AssetDecodeFailed, source.logicalPath, QStringLiteral("$"),
                    QStringLiteral("The theme font has no readable family name.")});
                return false;
            }
            if (name.size() > ThemeFontOverride::maximumFamilyLength) {
                diagnostics.append({ThemePackageErrorCode::ResourceLimit, source.logicalPath, QStringLiteral("$"),
                    QStringLiteral("The font family name exceeds the supported length.")});
                return false;
            }
            auto family = std::find_if(families.begin(), families.end(), [&name](const FontFiles& entry) {
                return entry.name.compare(name, Qt::CaseInsensitive) == 0;
            });
            if (family == families.end()) {
                families.append({name, {}});
                family = std::prev(families.end());
            }
            const auto digest = QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256);
            family->faces.insert(digest, std::move(*bytes));
        }
        for (const auto& files : families) {
            auto family = registerFamily(files, diagnostics);
            if (!family)
                return false;
            next->families.append(std::move(family));
        }
        prepared = std::move(next);
    }

    for (const auto& token : themeFontTokens()) {
        const auto& style = candidate.snapshot->ui.typography.*token.member;
        if (style.source != ThemeFontSource::Theme)
            continue;
        const bool supplied = prepared && std::any_of(prepared->families.cbegin(), prepared->families.cend(),
            [&style](const FamilyPtr& family) { return family->name.compare(style.family, Qt::CaseInsensitive) == 0; });
        if (!supplied) {
            diagnostics.append({ThemePackageErrorCode::AssetNotFound, candidate.manifest.id,
                QStringLiteral("$.fonts.%1.family").arg(QLatin1StringView(token.key)),
                QStringLiteral("The theme does not supply the font family '%1'.").arg(style.family)});
            return false;
        }
    }
    if (!prepared)
        return true;
    candidate.preparedFonts = std::move(prepared);
    for (auto& palette : candidate.palettes) {
        auto snapshot = std::make_shared<ThemeSnapshot>(*palette.snapshot);
        snapshot->fontAssets = candidate.preparedFonts->families;
        palette.snapshot = std::move(snapshot);
        if (palette.id == candidate.defaultPaletteId)
            candidate.snapshot = palette.snapshot;
    }
    return true;
}

}
