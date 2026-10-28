#ifndef HARKEN_CORE_PROGRAM_HANDLE_H
#define HARKEN_CORE_PROGRAM_HANDLE_H

#include "harken/core/CurrentWorld.h"

#include <cstddef>

namespace harken {

enum class ProgramKindV1 : std::uint32_t {
    Sound = 0, Container = 1, Track = 2, Segment = 3, Action = 4,
};

struct ProgramSourceIdentityV1 {
    WorldGeneration worldGeneration = 0;
    std::uint64_t ownerStableId = 0;
    std::uint64_t ownerRevision = 0;
    std::uint64_t leaseIdentity = 0;
    std::uint32_t sourceBodyVintage = kCurrentWorldShippingBodyVintage;
    bool operator==(const ProgramSourceIdentityV1&) const = default;
};

inline constexpr std::uint32_t kProgramCapabilityEntrySchedulingV1 = 1u;

struct ProgramCapabilitySummaryV1 {
    VnObjectId stableId{};
    std::uint64_t semanticFingerprint = 0;
    PlayableKindV1 kind = PlayableKindV1::Sound;
    std::uint32_t initialClosureCapabilities = 0;
};
static_assert(sizeof(ProgramCapabilitySummaryV1) == 32);

// Borrowed from an installed world or a frozen Pack lease. This is not a pin.
template<class Program>
struct ProgramHandleV1 {
    ProgramKindV1 kind = ProgramKindV1::Sound;
    VnObjectId stableId{};
    std::uint64_t semanticFingerprint = 0;
    ProgramSourceIdentityV1 source{};
    std::uint32_t initialClosureCapabilities = 0;
    const Program* programView = nullptr;

    explicit operator bool() const noexcept { return programView != nullptr; }
    const Program* operator->() const noexcept { return programView; }
    const Program& operator*() const noexcept { return *programView; }
    bool operator==(std::nullptr_t) const noexcept { return programView == nullptr; }
    bool operator==(const ProgramHandleV1&) const = default;
};

using ContainerProgramHandleV1 = ProgramHandleV1<ContainerProgramViewV3>;
using TrackProgramHandleV1 = ProgramHandleV1<TrackScheduleProgramViewV1>;
using SegmentProgramHandleV1 = ProgramHandleV1<SegmentScheduleProgramViewV1>;
using ActionProgramHandleV1 = ProgramHandleV1<ActionProgramViewV1>;

} // namespace harken

#endif
