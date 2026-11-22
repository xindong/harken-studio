// Fixed-memory acknowledgement channel for parking a streamed Render Voice.
// One mailbox belongs to each Render slot. Logic reserves it before publishing
// the park command; cancellation can atomically claim a fixed tombstone while
// Reserved; audio publishes exactly one outcome after retiring the stream slot
// and releasing the Render slot; logic consumes it before the slot may be
// reserved again.
#ifndef HARKEN_CORE_STREAM_PARK_MAILBOX_H
#define HARKEN_CORE_STREAM_PARK_MAILBOX_H

#include <harken/Harken.h>

#include "harken/core/StreamParkToken.h"
#include "harken/core/StreamRing.h"

#include <atomic>
#include <cstdint>

namespace harken {

enum class StreamParkMailboxState : std::uint32_t {
    Empty = 0,
    Reserved = 1,
    Cancelled = 2,
    Published = 3
};

enum class StreamParkDisposition : std::uint32_t {
    ResumeReady = 0,
    NaturalEnd = 1,
    IntegrityAbort = 2,
    StaleRender = 3,
    // The streaming thread died while this park was in flight. Distinct from
    // StaleRender, which means a benign race lost to a newer generation, and
    // from the two content outcomes: nothing is wrong with the asset.
    ProducerFailure = 4,
    // Keep last. Same tripwire discipline as the two enumerations it is mapped
    // from: appending a disposition and bumping this fails the assertion below,
    // so the mapping is revisited deliberately.
    Count = 5
};

static_assert(static_cast<std::uint32_t>(StreamParkDisposition::Count) == 5,
              "a park disposition was added: extend "
              "ParkDispositionForTerminalReason below before bumping this");

// A losing park's terminal reason becomes its disposition here, in one place.
// Two ternary chains in the voice pool used to do this independently, each
// ending in StaleRender -- so a reason neither chain named was reported as a
// benign lost race. Exhaustive switch with no default, and the reason
// enumeration carries a Count tripwire, so adding one lands here deliberately.
inline StreamParkDisposition ParkDispositionForTerminalReason(
    StreamTerminalReason reason) {
    switch (reason) {
    case StreamTerminalReason::IntegrityAbort:
        return StreamParkDisposition::IntegrityAbort;
    case StreamTerminalReason::NaturalEnd:
        return StreamParkDisposition::NaturalEnd;
    case StreamTerminalReason::ProducerFailure:
        return StreamParkDisposition::ProducerFailure;
    case StreamTerminalReason::None:
    case StreamTerminalReason::Count:
        return StreamParkDisposition::StaleRender;
    }
    return StreamParkDisposition::StaleRender;
}

enum class StreamParkCancelResult : std::uint32_t {
    Missing = 0,
    Cancelled = 1,
    Published = 2
};

struct StreamParkOutcome {
    VnVoiceHandle renderHandle = 0;
    VnVoiceHandle publicHandle = 0;
    std::uint64_t requestSequence = 0;
    std::uint64_t sourceFrame = 0;
    std::uint32_t loopsRemaining = 0;
    StreamParkTokenRef token{};
    StreamParkDisposition disposition = StreamParkDisposition::StaleRender;
    bool cancelled = false;
};

struct StreamParkMailbox {
    std::atomic<StreamParkMailboxState> state{
        StreamParkMailboxState::Empty};
    StreamParkOutcome outcome{};
};

static_assert(std::atomic<StreamParkMailboxState>::is_always_lock_free,
              "stream park mailbox state must be lock-free");

} // namespace harken

#endif // HARKEN_CORE_STREAM_PARK_MAILBOX_H
