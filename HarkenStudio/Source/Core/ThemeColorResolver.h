#pragma once

#include "ThemeColorCatalog.h"

namespace harken::studio {

void normalizeOpaqueThemeColors(const QVector<ThemeColorRole>& roles, ThemeColorOverrides& colors,
                               const QColor& baseBackground = {});
bool resolveThemeColors(ThemeSnapshot& theme, const QVector<ThemeColorRole>& roles,
                        const ThemeColorOverrides& colors);

}
