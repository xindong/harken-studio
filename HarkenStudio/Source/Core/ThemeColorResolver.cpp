#include "ThemeColorResolver.h"

#include "ThemePackageLoader.h"

namespace VNX {

bool resolveThemeColors(ThemeSnapshot& theme, const QVector<ThemeColorRole>& roles,
                        const ThemeColorOverrides& colors)
{
    for (const auto& role : roles)
    {
        const auto source = colors.constFind(role.key);
        if (source == colors.cend() || !source->isValid())
            return false;
        for (const auto& binding : role.bindings)
        {
            QColor color = *source;
            if (binding.alpha)
            {
                if (*binding.alpha < 0 || *binding.alpha > 255)
                    return false;
                color.setAlpha(*binding.alpha);
            }
            if (!applyThemeColor(theme, binding.target, color))
                return false;
        }
    }
    return true;
}

}
