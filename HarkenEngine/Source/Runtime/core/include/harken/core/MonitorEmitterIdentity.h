#ifndef HARKEN_CORE_MONITOR_EMITTER_IDENTITY_H
#define HARKEN_CORE_MONITOR_EMITTER_IDENTITY_H

#include <cstdint>
#include <type_traits>

namespace harken {

enum class MonitorEmitterKind : std::uint32_t {
    Unknown = 0,
    None = 1,
    DefaultState = 2,
    ExplicitStateOnly = 3,
    ExplicitSpatial = 4,
};

enum class MonitorHostOwnership : std::uint32_t {
    Unknown = 0,
    UnityEmitter = 1,
    UnitySpatialSource = 2,
    StudioDefaultEmitter = 3,
};

struct MonitorHostKey {
    std::uint64_t engineInstance = 0;
    std::uint64_t sdkSession = 0;
    std::uint64_t localSequence = 0;

    bool Valid() const noexcept {
        return engineInstance != 0 && sdkSession != 0 && localSequence != 0;
    }
};

inline bool operator==(const MonitorHostKey& a, const MonitorHostKey& b) noexcept {
    return a.engineInstance == b.engineInstance && a.sdkSession == b.sdkSession &&
        a.localSequence == b.localSequence;
}

// Copied at acceptance. Current binding and later host registration cannot
// rewrite this historical value, including after emitter slot reuse.
struct MonitorEmitterOrigin {
    std::uint64_t emitter = 0;
    MonitorHostKey host{};
    std::uint64_t registrationRevision = 0;
    MonitorEmitterKind kind = MonitorEmitterKind::Unknown;
    MonitorHostOwnership ownership = MonitorHostOwnership::Unknown;
};

enum class MonitorBindingState : std::uint32_t {
    None = 0,
    Bound = 1,
    Detached = 2,
};

struct MonitorEmitterBinding {
    std::uint64_t stateEmitter = 0;
    std::uint64_t spatialEmitter = 0;
    MonitorBindingState state = MonitorBindingState::None;
    MonitorBindingState spatial = MonitorBindingState::None;
};

static_assert(std::is_trivially_copyable_v<MonitorEmitterOrigin>);
static_assert(sizeof(MonitorEmitterOrigin) == 48);

} // namespace harken

#endif
