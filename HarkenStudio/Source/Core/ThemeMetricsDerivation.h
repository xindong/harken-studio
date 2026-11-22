#pragma once

#include "ThemeSnapshot.h"

namespace harken::studio {

void deriveThemeMetrics(ThemeSnapshot& theme);
void deriveThemeMetrics(ThemeSnapshot& theme, const ThemeMetrics& baseMetrics);

}
