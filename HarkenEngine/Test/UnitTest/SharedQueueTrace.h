#pragma once

#include "harken/core/PlayableInstanceTable.h"

#include <array>
#include <cstdint>

struct SharedQueueTrace {
    bool valid = false;
    std::array<std::uint32_t, 2> selectedOrdinals{};
    std::int64_t onsetRelativeFrame = 0;
    std::uint64_t parentEpoch = 0;
    std::uint64_t parentPathHash = 0;
    std::uint32_t parentPathDepth = 0;
    bool parentLinked = false;
    bool reclaimed = false;
    bool nestedReclaimed = false;
    bool leafReclaimed = false;
    bool successorObserved = false;
    std::uint32_t successorOrdinal = 0;
    std::uint32_t typedError = 0;
};

inline bool CaptureNestedRuntimeParentPath(
    harken::PlayableInstanceHandle rootHandle,
    harken::PlayableInstanceHandle nestedHandle,
    harken::PlayableInstanceHandle leafHandle,
    const harken::PlayableInstanceView& root,
    const harken::PlayableInstanceView& nested,
    const harken::PlayableInstanceView& leaf,
    std::uint64_t rootExpectedEpoch,
    std::uint64_t rootActiveChildEpoch,
    std::uint64_t nestedExpectedEpoch,
    std::uint64_t nestedActiveChildEpoch,
    std::uint64_t* outHash,
    std::uint32_t* outDepth) noexcept {
    if (outHash == nullptr || outDepth == nullptr ||
        !root.live || !nested.live || !leaf.live ||
        root.kind != harken::PlayableKind::Container ||
        nested.kind != harken::PlayableKind::Container ||
        leaf.kind != harken::PlayableKind::Sound || root.parent != 0 ||
        root.firstChild != nestedHandle || nested.parent != rootHandle ||
        nested.firstChild != leafHandle || leaf.parent != nestedHandle ||
        root.scheduleEpoch == 0 || nested.scheduleEpoch == 0 ||
        leaf.scheduleEpoch == 0 || rootExpectedEpoch != root.scheduleEpoch ||
        rootActiveChildEpoch != nested.scheduleEpoch ||
        nestedExpectedEpoch != nested.scheduleEpoch ||
        nestedActiveChildEpoch != leaf.scheduleEpoch) {
        return false;
    }
    std::uint64_t hash = 1469598103934665603ull;
    const auto add = [&hash](std::uint64_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    add(static_cast<std::uint8_t>(root.kind));
    add(static_cast<std::uint8_t>(nested.kind));
    add(static_cast<std::uint8_t>(leaf.kind));
    add(1u); // root -> nested first-child edge verified
    add(1u); // nested -> leaf first-child edge verified
    add(1u); // playback owner/active-child epochs verified
    *outHash = hash;
    *outDepth = 2;
    return true;
}
