#include "PlatformFieldMerge.h"
#include "Core/PropertyDescriptor.h"

namespace VNX {

PlatformFieldView platformFieldView(ProjectItem& owner,
                                   const PlayablePropertyLocatorV1& locator,
                                   uint8_t propertyId,
                                   const PlatformContext& context)
{
    PlatformFieldView view;
    if (context.selected.isEmpty())
        return view;

    int overlayCount = 0;
    bool haveValue = false;
    bool uniform = true;
    PlayablePropertyStateV1 shared;
    view.perPlatform.reserve(context.selected.size());
    for (ProjectPlatformId platform : context.selected) {
        const auto state = PlayablePropertyAuthoringV1::readProperty(
            owner, locator, propertyId, platform);
        const PlayablePropertyStateV1 resolved = state.value_or(PlayablePropertyStateV1{});
        view.perPlatform.append({platform, resolved});
        if (!haveValue) {
            shared = resolved;
            haveValue = true;
        } else if (!(resolved == shared)) {
            uniform = false;
        }
        const auto overlay = PlayablePropertyAuthoringV1::readPlatformOverlay(
            owner, locator, propertyId, platform);
        if (overlay && overlay->present)
            ++overlayCount;
    }

    view.uniform = uniform;
    if (uniform)
        view.value = shared;
    if (overlayCount == 0)
        view.coverage = PlatformFieldView::Coverage::None;
    else if (overlayCount == context.selected.size())
        view.coverage = PlatformFieldView::Coverage::All;
    else
        view.coverage = PlatformFieldView::Coverage::Some;
    return view;
}

QString platformFieldMixedTooltip(const PlatformFieldView& view,
                                   const QVector<ProjectPlatform>& platforms,
                                   const PropertyDescriptor* descriptor)
{
    QStringList lines;
    lines.reserve(view.perPlatform.size());
    for (const auto& entry : view.perPlatform) {
        const auto* platform = ProjectPlatformSemantics::find(platforms, entry.first);
        if (!platform) continue;
        QString valueText = entry.second.value.isNull()
            ? QString()
            : entry.second.value.toVariant().toString();
        if (descriptor && descriptor->displayScale != 1.0
            && entry.second.value.isDouble()) {
            valueText = QString::number(
                entry.second.value.toDouble() * descriptor->displayScale,
                'f', descriptor->decimals) + descriptor->unit;
        }
        lines.append(QStringLiteral("%1: %2")
                         .arg(platform->name, valueText));
    }
    return lines.join(QLatin1Char('\n'));
}

}
