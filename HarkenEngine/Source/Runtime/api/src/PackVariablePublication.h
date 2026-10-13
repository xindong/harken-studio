#pragma once

#include "harken/core/PackComposition.h"
#include "harken/param/VariableService.h"

namespace harken {

struct Engine;

// Logic thread only. The candidate must come from successful PrepareCommit;
// composition owns identity/conflict validation and the overall transaction.
VnResult PreparePackVariables(
    Engine& engine, const PackComposition::PublishedView& next,
    VariableServiceV1::PreparedPackDefinitions* out);
VnResult PreparePackVariables(
    Engine& engine, const PackComposition::PublishedView& next,
    PackVariablePublicationReason reason,
    VariableServiceV1::PreparedPackDefinitions* out);
void CommitPackVariables(
    Engine& engine,
    VariableServiceV1::PreparedPackDefinitions* prepared) noexcept;

} // namespace harken
