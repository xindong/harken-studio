#include "ComponentRecipeRegistry.h"

#include "ParameterPresenterRegistry.h"

namespace VNX {

QString componentRecipeHostName(ComponentRecipeHostId host)
{
    switch (host)
    {
        case ComponentRecipeHostId::TrackHeader:
            return QStringLiteral("track.header");
    }
    return {};
}

QString componentRecipeBreakpointName(ComponentRecipeBreakpoint breakpoint)
{
    switch (breakpoint)
    {
        case ComponentRecipeBreakpoint::Default:
            return QStringLiteral("default");
        case ComponentRecipeBreakpoint::Compact:
            return QStringLiteral("compact");
        case ComponentRecipeBreakpoint::Wide:
            return QStringLiteral("wide");
    }
    return {};
}

QString componentRecipeVariantName(ComponentRecipeVariant variant)
{
    switch (variant)
    {
        case ComponentRecipeVariant::TrackCompact:
            return QStringLiteral("track.compact");
        case ComponentRecipeVariant::TrackExpanded:
            return QStringLiteral("track.expanded");
    }
    return {};
}

QString componentRecipeSlotName(ComponentRecipeSlotId slot)
{
    switch (slot)
    {
        case ComponentRecipeSlotId::Color:
            return QStringLiteral("color");
        case ComponentRecipeSlotId::Identity:
            return QStringLiteral("identity");
        case ComponentRecipeSlotId::Input:
            return QStringLiteral("input");
        case ComponentRecipeSlotId::Inserts:
            return QStringLiteral("inserts");
        case ComponentRecipeSlotId::Sends:
            return QStringLiteral("sends");
        case ComponentRecipeSlotId::Pan:
            return QStringLiteral("pan");
        case ComponentRecipeSlotId::Volume:
            return QStringLiteral("volume");
        case ComponentRecipeSlotId::Meter:
            return QStringLiteral("meter");
        case ComponentRecipeSlotId::Controls:
            return QStringLiteral("controls");
        case ComponentRecipeSlotId::Routing:
            return QStringLiteral("routing");
        case ComponentRecipeSlotId::Params:
            return QStringLiteral("params");
    }
    return {};
}

QString componentRecipeAlignmentName(ComponentRecipeAlignment alignment)
{
    switch (alignment)
    {
        case ComponentRecipeAlignment::Start:
            return QStringLiteral("start");
        case ComponentRecipeAlignment::Center:
            return QStringLiteral("center");
        case ComponentRecipeAlignment::End:
            return QStringLiteral("end");
        case ComponentRecipeAlignment::Fill:
            return QStringLiteral("fill");
    }
    return {};
}

std::optional<ComponentRecipeHostId> parseComponentRecipeHost(
    const QString& host)
{
    if (host == QStringLiteral("track.header"))
        return ComponentRecipeHostId::TrackHeader;
    return std::nullopt;
}

std::optional<ComponentRecipeBreakpoint> parseComponentRecipeBreakpoint(
    const QString& breakpoint)
{
    if (breakpoint == QStringLiteral("default"))
        return ComponentRecipeBreakpoint::Default;
    if (breakpoint == QStringLiteral("compact"))
        return ComponentRecipeBreakpoint::Compact;
    if (breakpoint == QStringLiteral("wide"))
        return ComponentRecipeBreakpoint::Wide;
    return std::nullopt;
}

std::optional<ComponentRecipeVariant> parseComponentRecipeVariant(
    const QString& variant)
{
    if (variant == QStringLiteral("track.compact"))
        return ComponentRecipeVariant::TrackCompact;
    if (variant == QStringLiteral("track.expanded"))
        return ComponentRecipeVariant::TrackExpanded;
    return std::nullopt;
}

std::optional<ComponentRecipeSlotId> parseComponentRecipeSlot(
    const QString& slot)
{
    if (slot == QStringLiteral("color"))
        return ComponentRecipeSlotId::Color;
    if (slot == QStringLiteral("identity"))
        return ComponentRecipeSlotId::Identity;
    if (slot == QStringLiteral("input"))
        return ComponentRecipeSlotId::Input;
    if (slot == QStringLiteral("inserts"))
        return ComponentRecipeSlotId::Inserts;
    if (slot == QStringLiteral("sends"))
        return ComponentRecipeSlotId::Sends;
    if (slot == QStringLiteral("pan"))
        return ComponentRecipeSlotId::Pan;
    if (slot == QStringLiteral("volume"))
        return ComponentRecipeSlotId::Volume;
    if (slot == QStringLiteral("meter"))
        return ComponentRecipeSlotId::Meter;
    if (slot == QStringLiteral("controls"))
        return ComponentRecipeSlotId::Controls;
    if (slot == QStringLiteral("routing"))
        return ComponentRecipeSlotId::Routing;
    if (slot == QStringLiteral("params"))
        return ComponentRecipeSlotId::Params;
    return std::nullopt;
}

std::optional<ComponentRecipeAlignment> parseComponentRecipeAlignment(
    const QString& alignment)
{
    if (alignment == QStringLiteral("start"))
        return ComponentRecipeAlignment::Start;
    if (alignment == QStringLiteral("center"))
        return ComponentRecipeAlignment::Center;
    if (alignment == QStringLiteral("end"))
        return ComponentRecipeAlignment::End;
    if (alignment == QStringLiteral("fill"))
        return ComponentRecipeAlignment::Fill;
    return std::nullopt;
}

bool componentRecipeVariantSupportsHost(ComponentRecipeVariant variant,
                                        ComponentRecipeHostId host)
{
    switch (host)
    {
        case ComponentRecipeHostId::TrackHeader:
            return variant == ComponentRecipeVariant::TrackCompact
                || variant == ComponentRecipeVariant::TrackExpanded;
    }
    return false;
}

bool componentRecipeHostSupportsSlot(ComponentRecipeHostId host,
                                     ComponentRecipeSlotId slot)
{
    switch (host)
    {
        case ComponentRecipeHostId::TrackHeader:
            return slot == ComponentRecipeSlotId::Color
                || slot == ComponentRecipeSlotId::Identity
                || slot == ComponentRecipeSlotId::Pan
                || slot == ComponentRecipeSlotId::Volume
                || slot == ComponentRecipeSlotId::Meter
                || slot == ComponentRecipeSlotId::Controls
                || slot == ComponentRecipeSlotId::Routing
                || slot == ComponentRecipeSlotId::Params;
    }
    return false;
}

std::optional<ParameterPresenterContext> componentRecipePresenterContext(
    ComponentRecipeHostId host,
    ComponentRecipeSlotId slot,
    ComponentRecipeVariant)
{
    if (slot != ComponentRecipeSlotId::Volume)
        return std::nullopt;
    if (host == ComponentRecipeHostId::TrackHeader)
        return ParameterPresenterContext::InspectorFloat;
    return std::nullopt;
}

bool componentRecipeVariantIsVertical(ComponentRecipeVariant)
{
    return false;
}

QString componentRecipeKey(ComponentRecipeHostId host,
                           ComponentRecipeBreakpoint breakpoint)
{
    return componentRecipeHostName(host) + QLatin1Char('@')
        + componentRecipeBreakpointName(breakpoint);
}

}
