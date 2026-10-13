#ifndef HARKEN_CORE_COMPLETION_MUTATION_BINDING_H
#define HARKEN_CORE_COMPLETION_MUTATION_BINDING_H

#include "harken/core/CompletionOwner.h"
#include "harken/core/OccurrenceToken.h"

#include <cstdint>

namespace harken {

struct CompletionMutationBinding {
    OccurrenceToken occurrence{};
    CompletionOwnerRef owner{};
};

enum class CompletionMutationGuardResult : std::uint8_t {
    Allowed = 0,
    OwnerPinned = 1,
    StaleBinding = 2,
};

bool IsCanonicalUnboundCompletionMutation(
    const CompletionMutationBinding& binding) noexcept;
bool IsValidCompletionMutationBinding(
    const CompletionMutationBinding& binding) noexcept;
bool SameCompletionMutationBinding(
    const CompletionMutationBinding& a,
    const CompletionMutationBinding& b) noexcept;
CompletionMutationGuardResult EvaluateCompletionMutationBinding(
    const CompletionMutationBinding& binding,
    const CompletionOwnerRef* capturedAuthority) noexcept;

} // namespace harken

#endif // HARKEN_CORE_COMPLETION_MUTATION_BINDING_H
