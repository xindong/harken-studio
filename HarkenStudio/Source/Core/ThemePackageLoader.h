#pragma once

#include "ColorPalette.h"
#include "ThemePackageDto.h"
#include "ThemePackageParser.h"
#include "ThemeSnapshot.h"

#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

namespace VNX {

struct ThemePackageLoadLimits
{
    int maximumThemeCount = 64;
    int maximumInheritanceDepth = 8;
    qsizetype maximumFileBytes = 1024 * 1024;
    qsizetype maximumTotalBytes = 8 * 1024 * 1024;
    int maximumJsonDepth = 32;
    int maximumJsonNodes = 20000;
};

struct ThemePackageLoadOptions
{
    ThemePackageLoadLimits limits;
    QString harkenVersion = QStringLiteral("0.1.9");
    QSet<QString> supportedCapabilities{
        QStringLiteral("curve-v1"),
        QStringLiteral("meter-v1"),
        QStringLiteral("waveform-v1")};
};

struct ThemePackageCandidate
{
    ThemeManifestDto manifest;
    ThemeSnapshotPtr snapshot;
    ThemeAssetMapDto assets;
    QMap<QString, QString> assetRoots;
    QStringList inheritanceOrder;
    QVector<ThemeColorRole> colorRoles;
    QList<ColorPalette> palettes;
    QString defaultPaletteId;
};

struct ThemePackageLoadResult
{
    std::optional<ThemePackageCandidate> candidate;
    QList<ThemePackageDiagnostic> diagnostics;

    bool ok() const
    {
        return candidate.has_value() && diagnostics.isEmpty();
    }
};

struct ThemePackageDescriptor
{
    QString id;
    QString displayName;
    QString version;
    bool builtin = false;
};

struct ThemePackageDiscoveryResult
{
    QList<ThemePackageDescriptor> themes;
    QList<ThemePackageDiagnostic> diagnostics;
};

QString builtinThemeId();

bool applyThemeColor(ThemeSnapshot& theme, const QString& key, const QColor& color);
QColor readThemeColor(const ThemeSnapshot& theme, const QString& key);

ThemePackageDiscoveryResult discoverThemePackages(
    const QString& themesRoot,
    const ThemePackageLoadOptions& options = {});

ThemePackageLoadResult loadThemePackageCandidate(
    const QString& themesRoot,
    const QString& themeId,
    const ThemePackageLoadOptions& options = {});

}
