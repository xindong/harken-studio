#include "ThemeColorResolver.h"

#include "ThemePackageLoader.h"

#include <QImage>
#include <QPainter>

namespace harken::studio {

void normalizeOpaqueThemeColors(const QVector<ThemeColorRole>& roles, ThemeColorOverrides& colors,
                               const QColor& baseBackground)
{
    for (const auto& role : roles)
    {
        if (!role.opaque)
            continue;
        auto value = colors.find(role.key);
        if (value == colors.end() || !value->isValid() || value->alphaF() == 1.0f)
            continue;
        if (!baseBackground.isValid())
        {
            value->setAlpha(255);
            continue;
        }
        QImage pixel(1, 1, QImage::Format_RGB32);
        pixel.fill(baseBackground.rgb());
        QPainter painter(&pixel);
        painter.fillRect(pixel.rect(), colors.value(QStringLiteral("background.primary"), baseBackground));
        painter.fillRect(pixel.rect(), *value);
        painter.end();
        *value = pixel.pixelColor(0, 0);
    }
}

bool resolveThemeColors(ThemeSnapshot& theme, const QVector<ThemeColorRole>& roles,
                        const ThemeColorOverrides& colors)
{
    for (const auto& role : roles)
    {
        const auto source = colors.constFind(role.key);
        if (source == colors.cend() || !source->isValid()
            || (role.opaque && source->alphaF() != 1.0f))
            return false;
        for (const auto& binding : role.bindings)
        {
            QColor color = *source;
            if (binding.alpha)
            {
                if (*binding.alpha < 0 || *binding.alpha > 255)
                    return false;
                color.setAlphaF(color.alphaF() * *binding.alpha / 255.0);
            }
            if (!applyThemeColor(theme, binding.target, color))
                return false;
        }
    }
    for (const auto& [target, source] : theme.inheritedColorSources.asKeyValueRange())
    {
        QColor color = readThemeColor(theme, source);
        if (target == QStringLiteral("qtAds.activeTabAccent") && source == QStringLiteral("accent.primary"))
            color.setAlpha(static_cast<int>(color.alpha() * theme.thirdParty.qtAds.activeTabAccentOpacity));
        if (!color.isValid() || !applyThemeColor(theme, target, color))
            return false;
    }
    return true;
}

}
