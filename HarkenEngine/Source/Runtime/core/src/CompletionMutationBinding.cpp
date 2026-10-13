#include "harken/core/CompletionMutationBinding.h"

namespace harken {
namespace {

bool IsCanonicalNone(const CompletionOwnerRef& owner) noexcept {
    return owner.kind == CompletionOwnerKind::None &&
           !owner.composite.Valid() && !owner.singleDecision.Valid() &&
           owner.factsRevision == 0;
}

bool IsValidOwner(const CompletionOwnerRef& owner) noexcept {
    if (owner.factsRevision == 0) {
        return false;
    }
    if (owner.kind == CompletionOwnerKind::SingleQueue) {
        return owner.singleDecision.Valid() && !owner.composite.Valid();
    }
    if (owner.kind == CompletionOwnerKind::Composite) {
        return owner.composite.Valid() && !owner.singleDecision.Valid();
    }
    return false;
}

} // namespace

bool IsCanonicalUnboundCompletionMutation(
    const CompletionMutationBinding& binding) noexcept {
    return binding.occurrence.generation == 0 &&
           binding.occurrence.slot == 0 &&
           IsCanonicalNone(binding.owner);
}

bool IsValidCompletionMutationBinding(
    const CompletionMutationBinding& binding) noexcept {
    return binding.occurrence.generation != 0 && IsValidOwner(binding.owner);
}

bool SameCompletionMutationBinding(
    const CompletionMutationBinding& a,
    const CompletionMutationBinding& b) noexcept {
    return a.occurrence.slot == b.occurrence.slot &&
           a.occurrence.generation == b.occurrence.generation &&
           SameCompletionOwner(a.owner, b.owner);
}

CompletionMutationGuardResult EvaluateCompletionMutationBinding(
    const CompletionMutationBinding& binding,
    const CompletionOwnerRef* capturedAuthority) noexcept {
    if (IsCanonicalUnboundCompletionMutation(binding)) {
        return capturedAuthority == nullptr
                   ? CompletionMutationGuardResult::Allowed
                   : CompletionMutationGuardResult::StaleBinding;
    }
    if (!IsValidCompletionMutationBinding(binding) ||
        capturedAuthority == nullptr ||
        !SameCompletionOwner(binding.owner, *capturedAuthority)) {
        return CompletionMutationGuardResult::StaleBinding;
    }
    return CompletionMutationGuardResult::OwnerPinned;
}

} // namespace harken
