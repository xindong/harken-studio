#include "ThemeStyleTokens.h"

#include <cmath>

namespace harken::studio {

const std::array<ThemeMetricToken, 18>& themeMetricTokens()
{
    using Metrics = ThemeMetrics;
    using Control = ThemeMetrics::Control;
    constexpr auto spacing = QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Spacing");
    constexpr auto radius = QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Corner Radius");
    constexpr auto border = QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Border Width");
    constexpr auto control = QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Control Height");
    constexpr auto icon = QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Icon Size");
    static constexpr std::array<ThemeMetricToken, 18> tokens{{
        {"spacing.compact", spacing, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Compact"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets the gap between grouped tool buttons and the space above and below table-row content."),
         0, &Metrics::spacingCompact},
        {"spacing.xSmall", spacing, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Extra Small"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets the gap between form controls and the thickness of slider grooves and panel splitters."),
         0, &Metrics::spacingXSmall},
        {"spacing.small", spacing, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Small"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets panel padding, the space between sections, and horizontal padding inside text fields."),
         0, &Metrics::spacingSmall},
        {"spacing.medium", spacing, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Medium"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets Blueprint node padding and the size of slider handles and timeline marker handles."),
         0, &Metrics::spacingMedium},
        {"spacing.large", spacing, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Large"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets the vertical gap between automatically arranged Blueprint nodes and the side padding in meter readouts."),
         0, &Metrics::spacingLarge},
        {"spacing.xLarge", spacing, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Extra Large"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets the left and right margins around content at the bottom of the startup window."),
         0, &Metrics::spacingXLarge},
        {"radius.compact", radius, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Compact"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Rounds flat icon buttons and slider grooves."),
         0, &Metrics::radiusCompact},
        {"radius.small", radius, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Small"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Rounds tool buttons, menu-bar items, and the top corners of tabs."),
         0, &Metrics::radiusSmall},
        {"radius.medium", radius, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Medium"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Rounds playback controls, slider handles, and the lower corners of tab panes."),
         0, &Metrics::radiusMedium},
        {"radius.large", radius, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Large"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Rounds drop zones, Assistant message bubbles, and the Assistant input box."),
         0, &Metrics::radiusLarge},
        {"border.width", border, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Standard"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets the thickness of standard control borders, table dividers, and plot grid lines."),
         0, &Metrics::borderWidth},
        {"border.accentWidth", border, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Accent"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets the thickness of highlighted section edges and selected Blueprint node outlines."),
         0, &Metrics::accentBorderWidth},
        {"control.compactHeight", control, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Compact"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets the height of Inspector slot buttons, list footer buttons, and the Blueprint status bar."),
         1, &Control::compactHeight},
        {"control.height", control, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Standard"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets the base height of buttons and input fields and contributes to table row height."),
         1, &Metrics::controlHeight},
        {"control.largeHeight", control, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Large"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets Blueprint node header height and contributes to title-bar and toolbar height."),
         1, &Control::largeHeight},
        {"icon.small", icon, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Small"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets the size of checkbox indicators and arrows inside input fields."),
         1, &Control::smallIconExtent},
        {"icon.regular", icon, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Standard"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets the size of dock-tab icons and toolbar icons."),
         1, &Metrics::iconExtent},
        {"icon.large", icon, QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Large"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Sets the size of Blueprint node badges and the icon column in popup menus."),
         1, &Control::largeIconExtent},
    }};
    return tokens;
}

int ThemeMetricToken::value(const ThemeMetrics& metrics) const
{
    if (const auto root = std::get_if<int ThemeMetrics::*>(&member))
        return metrics.*(*root);
    return metrics.control.*std::get<int ThemeMetrics::Control::*>(member);
}

void ThemeMetricToken::setValue(ThemeMetrics& metrics, int value) const
{
    if (const auto root = std::get_if<int ThemeMetrics::*>(&member))
        metrics.*(*root) = value;
    else
        metrics.control.*std::get<int ThemeMetrics::Control::*>(member) = value;
}

bool ThemeMetricToken::accepts(double value) const
{
    return value >= minimum && value <= maximum && std::floor(value) == value;
}

const std::array<ThemeFontToken, 4>& themeFontTokens()
{
    static constexpr std::array<ThemeFontToken, 4> tokens{{
        {"body", QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Body"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Field labels and values, menus, buttons, and tables."),
         FontRole::Body, &ThemeTypography::body},
        {"title", QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Title"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Page titles and headings in Assistant content."),
         FontRole::Title, &ThemeTypography::title},
        {"caption", QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Caption"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Axis labels, metadata, subtitles, and compact status text."),
         FontRole::Caption, &ThemeTypography::caption},
        {"mono", QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Monospace"),
         QT_TRANSLATE_NOOP("harken::studio::AppearancePage", "Code, commands, and raw logs with aligned columns."),
         FontRole::Mono, &ThemeTypography::mono},
    }};
    return tokens;
}

const ThemeMetricToken* findThemeMetricToken(const QString& key)
{
    for (const auto& token : themeMetricTokens())
        if (key == QLatin1StringView(token.key))
            return &token;
    return nullptr;
}

const ThemeFontToken& themeFontToken(FontRole role)
{
    for (const auto& token : themeFontTokens())
        if (token.role == role)
            return token;
    Q_UNREACHABLE();
}

const ThemeFontToken* findThemeFontToken(const QString& key)
{
    for (const auto& token : themeFontTokens())
        if (key == QLatin1StringView(token.key))
            return &token;
    return nullptr;
}

bool ThemeFontOverride::acceptsFamily(const QString& name, ThemeFontSource source)
{
    return name.size() <= maximumFamilyLength && (source == ThemeFontSource::System || !name.isEmpty());
}

const char* ThemeFontOverride::invalidField() const
{
    if (family && !acceptsFamily(*family, source.value_or(ThemeFontSource::System)))
        return "family";
    if (pointSize && (*pointSize < minimumPointSize || *pointSize > maximumPointSize))
        return "pointSize";
    if (weight && (*weight < minimumWeight || *weight > maximumWeight))
        return "weight";
    return nullptr;
}

void ThemeFontOverride::applyTo(ThemeFontStyle& style, const QStringList& fallbackFamilies) const
{
    if (family) {
        style.family = *family;
        style.source = source;
        style.fallbackFamilies = fallbackFamilies;
    }
    if (pointSize) style.pointSize = *pointSize;
    if (weight) style.weight = *weight;
}

}
