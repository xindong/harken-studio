#pragma once

#include "ThemeColorCatalog.h"

#include <QList>

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

void copyThemeColors(ThemeSnapshot& target, const ThemeSnapshot& source);
QList<ColorPalette> createStudioDefaultPalettes();

}
