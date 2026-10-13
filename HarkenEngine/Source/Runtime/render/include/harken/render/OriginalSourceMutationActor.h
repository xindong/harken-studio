#ifndef HARKEN_RENDER_ORIGINAL_SOURCE_MUTATION_ACTOR_H
#define HARKEN_RENDER_ORIGINAL_SOURCE_MUTATION_ACTOR_H

#include "harken/core/TimelineArmTable.h"
#include "harken/core/OriginalCatalogStageLoan.h"
#include <cstdint>

namespace harken {
class OriginalSourceSubmissionRegistry;
class OriginalSourceInstallationLoan;

// A structural loan issued by the actual joint Source factory.
class OriginalSourceLifetimePin {
public:
    OriginalSourceLifetimePin() noexcept = default;
    OriginalSourceLifetimePin(const OriginalSourceLifetimePin&) = delete;
    OriginalSourceLifetimePin& operator=(const OriginalSourceLifetimePin&) = delete;
    OriginalSourceLifetimePin(OriginalSourceLifetimePin&& other) noexcept;
    OriginalSourceLifetimePin& operator=(OriginalSourceLifetimePin&& other) noexcept;
    ~OriginalSourceLifetimePin() { Release(); }
    bool Valid() const noexcept { return owner_ != nullptr; }
    bool Borrow(OriginalSourceLifetimePin* out) const noexcept;
    void Release() noexcept;
private:
    friend class OriginalSourceSubmissionRegistry;
    friend class OriginalSourceMutationActor;
    OriginalSourceSubmissionRegistry* owner_ = nullptr;
    void* row_ = nullptr;
    TimelineArmToken token_{};
    std::uint64_t marker_ = 0;
};

// Only a native frozen range or its installed self-loan issues a root actor.
// The actor preserves the Source episode and records its actual first write.
// It grants neither target metadata access nor Catalog selection authority.
class OriginalSourceMutationActor {
public:
    OriginalSourceMutationActor() noexcept = default;
    OriginalSourceMutationActor(const OriginalSourceMutationActor&) = delete;
    OriginalSourceMutationActor& operator=(const OriginalSourceMutationActor&) = delete;
    OriginalSourceMutationActor(OriginalSourceMutationActor&& other) noexcept;
    OriginalSourceMutationActor& operator=(OriginalSourceMutationActor&& other) noexcept;
    ~OriginalSourceMutationActor() { Release(); }
    bool Valid() const noexcept { return lifetime_.Valid(); }
    bool Borrow(OriginalSourceMutationActor* out) const noexcept;
    bool BeforeFirstWrite() noexcept;
    void Release() noexcept;
private:
    friend class OriginalSourceSubmissionRegistry;
    OriginalSourceLifetimePin lifetime_;
    OriginalCatalogOldSiblingLoan catalogSibling_;
    bool catalogFallback_ = false;
    // Normal is retained across saturation; Return does not require Redirect.
    std::uint8_t borrowedMode_ = 0;
    bool wrote_ = false;
    bool callbackStanding_ = false;
};
} // namespace harken
#endif
