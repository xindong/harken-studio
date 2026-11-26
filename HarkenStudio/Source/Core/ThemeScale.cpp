#include "ThemeScale.h"

#include "ThemeMetricsDerivation.h"
#include "ThemeStyleTokens.h"

namespace harken::studio {
namespace {

int scalePixels(int value, qreal factor)
{
    return value > 0 ? qMax(1, qRound(value * factor)) : qRound(value * factor);
}

}

ThemeSnapshot scaledThemeSnapshot(const ThemeSnapshot& base, qreal factor)
{
    Q_ASSERT(qIsFinite(factor) && factor > 0.0);
    ThemeSnapshot scaled = base;
    scaled.uiScale = factor;
    auto& metrics = scaled.ui.metrics;
    for (const auto& token : themeMetricTokens())
        token.setValue(metrics, scalePixels(token.value(metrics), factor));
    deriveThemeMetrics(scaled, base.ui.metrics);
    for (auto& recipe : scaled.ui.recipes.entries)
    {
        for (auto& slot : recipe.items)
        {
            if (slot.minimum >= 0) slot.minimum = scalePixels(slot.minimum, factor);
            if (slot.preferred >= 0) slot.preferred = scalePixels(slot.preferred, factor);
            if (slot.maximum >= 0) slot.maximum = scalePixels(slot.maximum, factor);
        }
    }
    return scaled;
}

}
