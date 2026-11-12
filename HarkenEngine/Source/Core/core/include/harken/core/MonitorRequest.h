#ifndef HARKEN_CORE_MONITOR_REQUEST_H
#define HARKEN_CORE_MONITOR_REQUEST_H

#include <atomic>
#include <cstdint>

namespace harken {

// Playback operations not represented by the legacy EventDetail vocabulary.
inline constexpr std::uint32_t kMonitorOperationResetPreview = 17;
inline constexpr std::uint32_t kMonitorOperationPlayRawPreview = 18;

struct MonitorRequestKey {
    std::uint64_t engineInstance = 0;
    std::uint64_t sequence = 0;

    bool Valid() const noexcept { return engineInstance != 0 && sequence != 0; }
};

inline bool operator==(const MonitorRequestKey& left, const MonitorRequestKey& right) noexcept {
    return left.engineInstance == right.engineInstance && left.sequence == right.sequence;
}

enum class MonitorRequestOrigin : std::uint32_t {
    Unknown = 0,
    NativeAPI = 1,
    StudioPreview = 2,
};

struct MonitorRequestContext {
    MonitorRequestKey key{};
    MonitorRequestOrigin origin = MonitorRequestOrigin::Unknown;
    std::uint32_t operation = 0;
};

struct MonitorControlCause {
    MonitorRequestContext request{};
    std::uint64_t actionFiring = 0;
    std::uint64_t parentPlayable = 0;
    std::uint32_t executionOrdinal = UINT32_MAX;
};

// Called only during engine construction. A zero result disables identity,
// never engine initialization. This identity is independent of capture sessions.
std::uint64_t AllocateMonitorEngineInstance() noexcept;

class MonitorRequestSource {
public:
    explicit MonitorRequestSource(std::uint64_t engineInstance,
                                  std::uint64_t lastSequence = 0) noexcept
        : engineInstance_(engineInstance), lastSequence_(lastSequence) {}

    MonitorRequestContext Begin(MonitorRequestOrigin origin, std::uint32_t operation) noexcept;
    std::uint64_t EngineInstance() const noexcept { return engineInstance_; }
    std::uint64_t LastSequence() const noexcept { return lastSequence_.load(std::memory_order_relaxed); }
    std::uint64_t UnavailableCount() const noexcept { return unavailable_.load(std::memory_order_relaxed); }

private:
    const std::uint64_t engineInstance_;
    std::atomic<std::uint64_t> lastSequence_;
    std::atomic<std::uint64_t> unavailable_{0};
};

} // namespace harken

#endif
