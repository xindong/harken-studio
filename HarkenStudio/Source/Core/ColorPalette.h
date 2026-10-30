#pragma once

#include "ThemeColorCatalog.h"
#include "ThemePackageParser.h"

#include <optional>

namespace VNX {

struct ColorPaletteDescriptor
{
    QString id;
    QString displayName;
};

struct ColorPalette
{
    QString id;
    QString displayName;
    ThemeColorOverrides colors;
    ThemeSnapshotPtr snapshot;
};

struct ColorPaletteLoadResult
{
    std::optional<ColorPalette> palette;
    QList<ThemePackageDiagnostic> diagnostics;

    bool ok() const { return palette.has_value() && diagnostics.isEmpty(); }
};

void applyColorPalette(ThemeSnapshot& target, const ColorPalette& palette);

QString defaultPaletteId(const QString& themeId);
QString builtinAmberPaletteId();
ColorPalette createBuiltinAmberPalette();
ColorPalette colorPaletteFromTheme(const QString& themeId,
                                   const QString& displayName,
                                   const ThemeSnapshot& theme);

}
