#pragma once

#include "ThemeColorCatalog.h"

namespace harken::studio {

bool resolveThemeColors(ThemeSnapshot& theme, const QVector<ThemeColorRole>& roles,
                        const ThemeColorOverrides& colors);

}
