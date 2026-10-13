#include "harken_project_schema/RuntimeIdentityIndex.h"

#include <algorithm>
#include <map>
#include <set>

namespace harkenprojectschema {
namespace {

bool validClaimShape(const RuntimeIdentityIndexBuildRowV1& row) noexcept
{
    const auto& claim = row.claim;
    if (claim.stableId == kUnmintedStableId
        || claim.kind == RuntimeIdentityKindV1::Invalid
        || claim.sourceKey.kind != claim.kind
        || claim.sourceKey.reserved != 0
        || claim.sourceKey.stableId != claim.stableId
        || IsNull(claim.sourceKey.owningGuid))
        return false;
    if (isObjectRuntimeIdentityKindV1(claim.kind)) {
        return claim.stableId == claim.authoringGuid
            && claim.sourceKey.owningGuid == claim.authoringGuid
            && row.sourceHandle != 0;
    }
    if (isNestedRuntimeIdentityKindV1(claim.kind))
        return claim.authoringGuid == claim.stableId && row.sourceHandle == 0;
    return false;
}

}

const RuntimeIdentityClaimV1* RuntimeIdentityIndexV1::find(
    StableId id) const noexcept
{
    const auto it = byStableId_.find(id);
    return it == byStableId_.end() ? nullptr : &claims_[it->second];
}

const RuntimeIdentityClaimV1* RuntimeIdentityIndexV1::find(
    const RuntimeIdentitySourceKeyV1& sourceKey) const noexcept
{
    const auto it = bySourceKey_.find(sourceKey);
    return it == bySourceKey_.end() ? nullptr : &claims_[it->second];
}

const RuntimeIdentityClaimV1* RuntimeIdentityIndexV1::findBySourceHandle(
    RuntimeIdentityKindV1 kind, std::uint64_t sourceHandle) const noexcept
{
    const auto it = bySourceHandle_.find({kind, sourceHandle});
    return it == bySourceHandle_.end() ? nullptr : &claims_[it->second];
}

RuntimeIdentityIndexBuildOutcomeV1 BuildRuntimeIdentityIndexV1(
    const std::vector<RuntimeIdentityIndexBuildRowV1>& rows)
{
    RuntimeIdentityIndexBuildOutcomeV1 outcome;
    auto index = std::make_shared<RuntimeIdentityIndexV1>();
    index->claims_.reserve(rows.size());
    std::map<StableId, RuntimeIdentityClaimV1> identities;
    std::set<RuntimeIdentitySourceKeyV1> sourceKeys;
    std::set<std::pair<RuntimeIdentityKindV1, std::uint64_t>> sourceHandles;
    std::map<StableId, std::uint64_t> handleById;

    for (const auto& row : rows) {
        const auto& claim = row.claim;
        if (!validClaimShape(row)) {
            outcome.diagnostics.push_back({
                RuntimeIdentityIndexDiagnosticCodeV1::InvalidClaim,
                claim.stableId, claim.kind, claim.sourceKey});
            continue;
        }
        if (!sourceKeys.insert(claim.sourceKey).second) {
            outcome.diagnostics.push_back({
                RuntimeIdentityIndexDiagnosticCodeV1::DuplicateSourceKey,
                claim.stableId, claim.kind, claim.sourceKey});
            continue;
        }
        if (row.sourceHandle != 0
            && !sourceHandles.insert({claim.kind, row.sourceHandle}).second) {
            outcome.diagnostics.push_back({
                RuntimeIdentityIndexDiagnosticCodeV1::InvalidClaim,
                claim.stableId, claim.kind, claim.sourceKey});
            continue;
        }

        const auto [existing, inserted] = identities.emplace(claim.stableId, claim);
        if (!inserted) {
            outcome.diagnostics.push_back({
                RuntimeIdentityIndexDiagnosticCodeV1::DuplicateStableId,
                claim.stableId, claim.kind, claim.sourceKey,
                existing->second.kind,
                existing->second.sourceKey});
            continue;
        }

        index->claims_.push_back(claim);
        handleById.emplace(claim.stableId, row.sourceHandle);
    }

    if (!outcome.diagnostics.empty()) return outcome;

    std::sort(index->claims_.begin(), index->claims_.end(),
              [](const auto& left, const auto& right) {
                  return left.stableId < right.stableId;
              });
    for (std::size_t position = 0; position < index->claims_.size(); ++position) {
        const auto& claim = index->claims_[position];
        index->byStableId_.emplace(claim.stableId, position);
        index->bySourceKey_.emplace(claim.sourceKey, position);
        const auto handle = handleById.find(claim.stableId);
        if (handle != handleById.end() && handle->second != 0)
            index->bySourceHandle_.emplace(
                std::make_pair(claim.kind, handle->second), position);
    }
    outcome.index = std::move(index);
    return outcome;
}

}
