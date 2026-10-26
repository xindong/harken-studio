#pragma once

#include "SignalPathGraph.h"
#include "Core/PlacementContext.h"

namespace VNX {

struct SignalPathScope
{
    QUuid objectGuid;
    std::vector<PlayablePropertyLocatorV1> occurrences;

    bool operator==(const SignalPathScope&) const = default;
};

SignalPathGraph buildProjectSignalPathGraph(const ProjectData& project, FieldGroupScope platform = {},
    std::shared_ptr<const FieldGroupEffectiveSnapshot> groups = {});

SignalPathGraph buildSignalPathGraphForScope(
    const ProjectData& project, const SignalPathScope& scope,
    FieldGroupScope platform = {});

SignalPathGraph buildSignalPathGraphForScopes(
    const ProjectData& project, const std::vector<SignalPathScope>& scopes,
    FieldGroupScope platform = {});

PlacementContext signalPathPlacementContext(
    const std::vector<PlayablePropertyLocatorV1>& occurrences);

}
