#include "ThemeScale.h"

#include "ThemeMetricsDerivation.h"

namespace VNX {
namespace {

template<typename... Values>
void scalePixels(qreal factor, Values&... values)
{
    ((values = values > 0 ? qMax(1, qRound(values * factor)) : qRound(values * factor)), ...);
}

}

ThemeSnapshot scaledThemeSnapshot(const ThemeSnapshot& base, qreal factor)
{
    Q_ASSERT(qIsFinite(factor) && factor > 0.0);
    ThemeSnapshot scaled = base;
    scaled.uiScale = factor;
    auto& metrics = scaled.ui.metrics;
    scalePixels(factor,
        metrics.spacingCompact, metrics.spacingXSmall, metrics.spacingSmall,
        metrics.spacingMedium, metrics.spacingLarge, metrics.spacingXLarge,
        metrics.radiusCompact, metrics.radiusSmall, metrics.radiusMedium,
        metrics.radiusLarge, metrics.borderWidth, metrics.accentBorderWidth,
        metrics.controlHeight, metrics.iconExtent,
        metrics.control.compactHeight, metrics.control.largeHeight,
        metrics.control.smallIconExtent, metrics.control.largeIconExtent);
    deriveThemeMetrics(scaled, base.ui.metrics);
    for (auto& recipe : scaled.ui.recipes.entries)
    {
        for (auto& slot : recipe.items)
        {
            if (slot.minimum >= 0) scalePixels(factor, slot.minimum);
            if (slot.preferred >= 0) scalePixels(factor, slot.preferred);
            if (slot.maximum >= 0) scalePixels(factor, slot.maximum);
        }
    }
    return scaled;
}

}
