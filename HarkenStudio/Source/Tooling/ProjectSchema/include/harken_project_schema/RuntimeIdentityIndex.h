#ifndef HARKEN_PROJECT_SCHEMA_RUNTIME_IDENTITY_INDEX_H
#define HARKEN_PROJECT_SCHEMA_RUNTIME_IDENTITY_INDEX_H

#include "harken_project_schema/RuntimeIdentity.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace harkenprojectschema {

struct RuntimeIdentityIndexBuildRowV1 {
    std::uint64_t sourceHandle = 0;
    RuntimeIdentityClaimV1 claim;
};

struct RuntimeIdentityIndexBuildOutcomeV1;

enum class RuntimeIdentityIndexDiagnosticCodeV1 : std::uint32_t {
    InvalidClaim = 1,
    DuplicateStableId = 2,
    DuplicateGuid = 3,
    DuplicateSourceKey = 4,
};

struct RuntimeIdentityIndexDiagnosticV1 {
    RuntimeIdentityIndexDiagnosticCodeV1 code = RuntimeIdentityIndexDiagnosticCodeV1::InvalidClaim;
    StableId offendingId = kUnmintedStableId;
    RuntimeIdentityKindV1 kind = RuntimeIdentityKindV1::Invalid;
    RuntimeIdentitySourceKeyV1 sourceKey;
    RuntimeIdentityKindV1 conflictingKind = RuntimeIdentityKindV1::Invalid;
    RuntimeIdentitySourceKeyV1 conflictingSourceKey{};
};

class RuntimeIdentityIndexV1 {
public:
    const RuntimeIdentityClaimV1* find(StableId id) const noexcept;
    const RuntimeIdentityClaimV1* find(
        const RuntimeIdentitySourceKeyV1& sourceKey) const noexcept;
    // Source handles are editor-only object/tree handles.  This lookup is the
    // single sanctioned conversion point when an editor selection has not yet
    // been rewritten to its persisted stable identity.
    const RuntimeIdentityClaimV1* findBySourceHandle(
        RuntimeIdentityKindV1 kind, std::uint64_t sourceHandle) const noexcept;
    const std::vector<RuntimeIdentityClaimV1>& claims() const noexcept { return claims_; }

private:
    friend RuntimeIdentityIndexBuildOutcomeV1 BuildRuntimeIdentityIndexV1(
        const std::vector<RuntimeIdentityIndexBuildRowV1>& rows);
    std::vector<RuntimeIdentityClaimV1> claims_;
    std::map<StableId, std::size_t> byStableId_;
    std::map<RuntimeIdentitySourceKeyV1, std::size_t> bySourceKey_;
    std::map<std::pair<RuntimeIdentityKindV1, std::uint64_t>, std::size_t>
        bySourceHandle_;
};

struct RuntimeIdentityIndexBuildOutcomeV1 {
    std::shared_ptr<const RuntimeIdentityIndexV1> index;
    std::vector<RuntimeIdentityIndexDiagnosticV1> diagnostics;

    bool ok() const noexcept { return index != nullptr && diagnostics.empty(); }
};

RuntimeIdentityIndexBuildOutcomeV1 BuildRuntimeIdentityIndexV1(
    const std::vector<RuntimeIdentityIndexBuildRowV1>& rows);

} // namespace harkenprojectschema

#endif // HARKEN_PROJECT_SCHEMA_RUNTIME_IDENTITY_INDEX_H
