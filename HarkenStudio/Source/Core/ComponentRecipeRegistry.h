#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace VNX {

enum class ComponentRecipeHostId
{
    TrackHeader
};

enum class ComponentRecipeBreakpoint
{
    Default,
    Compact,
    Wide
};

enum class ComponentRecipeVariant
{
    TrackCompact,
    TrackExpanded
};

enum class ComponentRecipeSlotId
{
    Color,
    Identity,
    Input,
    Inserts,
    Sends,
    Pan,
    Volume,
    Meter,
    Controls,
    Routing,
    Params
};

enum class ComponentRecipeAlignment
{
    Start,
    Center,
    End,
    Fill
};

enum class ParameterPresenterContext;

QString componentRecipeHostName(ComponentRecipeHostId host);
QString componentRecipeBreakpointName(ComponentRecipeBreakpoint breakpoint);
QString componentRecipeVariantName(ComponentRecipeVariant variant);
QString componentRecipeSlotName(ComponentRecipeSlotId slot);
QString componentRecipeAlignmentName(ComponentRecipeAlignment alignment);

std::optional<ComponentRecipeHostId> parseComponentRecipeHost(
    const QString& host);
std::optional<ComponentRecipeBreakpoint> parseComponentRecipeBreakpoint(
    const QString& breakpoint);
std::optional<ComponentRecipeVariant> parseComponentRecipeVariant(
    const QString& variant);
std::optional<ComponentRecipeSlotId> parseComponentRecipeSlot(
    const QString& slot);
std::optional<ComponentRecipeAlignment> parseComponentRecipeAlignment(
    const QString& alignment);

bool componentRecipeVariantSupportsHost(ComponentRecipeVariant variant,
                                        ComponentRecipeHostId host);
bool componentRecipeHostSupportsSlot(ComponentRecipeHostId host,
                                     ComponentRecipeSlotId slot);
std::optional<ParameterPresenterContext> componentRecipePresenterContext(
    ComponentRecipeHostId host,
    ComponentRecipeSlotId slot,
    ComponentRecipeVariant variant);
bool componentRecipeVariantIsVertical(ComponentRecipeVariant variant);
QString componentRecipeKey(ComponentRecipeHostId host,
                           ComponentRecipeBreakpoint breakpoint);

}
