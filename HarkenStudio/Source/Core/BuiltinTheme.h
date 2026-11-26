#pragma once

#include "ThemeSnapshot.h"

namespace harken::studio {

const ThemeTypography& builtinThemeTypography();
ThemeSnapshot createBuiltinDarkTheme();
ThemeSnapshot createBuiltinLightTheme();

}
