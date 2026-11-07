#pragma once

#include "ThemeSnapshot.h"

namespace VNX {

void deriveThemeMetrics(ThemeSnapshot& theme);
void deriveThemeMetrics(ThemeSnapshot& theme, const ThemeMetrics& worldTokens);

}
