#pragma once

#include "ThemeColorCatalog.h"

namespace VNX {

bool resolveThemeColors(ThemeSnapshot& theme, const QVector<ThemeColorRole>& roles,
                        const ThemeColorOverrides& colors);

}
