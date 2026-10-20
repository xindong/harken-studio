// Emitter table, logic-thread side: the game objects many voices follow.
// An emitter carries a shared position/orientation and a small local
// Variable table; attached voices derive their spatial geometry from the
// emitter each tick and resolve Variable reads at emitter scope first.
//
// Identity is the same generational scheme as voices and packs: a u64
// handle of { 32-bit slot | 32-bit generation }, 0 is never valid, and the
// generation advances on destroy so a stale handle never matches a reused
// slot. Every operation on a stale handle is a benign no-op — the caller's
// API layer maps that to VN_STALE_HANDLE.
//
// Logic thread only. All storage (the emitter array and each emitter's local
// Variable slots) is preallocated at construction from the resolved config;
// create, destroy, and lookups touch no heap. The audio thread never sees an
// emitter — position/orientation reach voices through the ordinary per-voice
// spatial path, and Variable scope is resolved before curve evaluation.
#ifndef HARKEN_PARAM_EMITTER_TABLE_H
#define HARKEN_PARAM_EMITTER_TABLE_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/EngineConfig.h"

#include <cstdint>
#include <vector>

namespace harken {

struct PlaybackExecutionContext {
    VnEmitterHandle stateEmitter = 0;
    VnEmitterHandle spatialEmitter = 0;
};

class EmitterTable {
public:
    explicit EmitterTable(const EngineConfig& config);
    bool BeginVariableProjection() noexcept;
    void RestoreVariableProjection() noexcept;
    std::uint64_t VariableProjectionStorageBytes() const noexcept;
    using VariableChangeObserver = void (*)(void*, VnEmitterHandle, VnVariableId) noexcept;
    void SetVariableChangeObserver(void* context, VariableChangeObserver observer) noexcept {
        m_variableObserverContext = context;
        m_variableObserver = observer;
    }

    // Mints a fresh emitter at a free slot, forming its handle. Returns false
    // when every slot is live (the caller maps this to VN_NO_EMITTER). A new
    // emitter starts at the origin, facing (0,0,-1), with no local overrides.
    bool Create(VnEmitterHandle* outHandle, bool stateOnly = false);
    bool IsPublicLive(VnEmitterHandle handle) const noexcept;
    VnEmitterHandle DefaultState() const noexcept;
    PlaybackExecutionContext Context(VnEmitterHandle state) const noexcept;
    static std::uint64_t StorageBytesPerEmitter(std::uint32_t localSlots) noexcept;

    // Ends the emitter: its slot is freed and its generation advances so the
    // handle is now stale, and its local Variable overrides are dropped.
    // Returns false for an already-stale handle (a benign no-op). Attached
    // voices are not touched here — they detach lazily (the spatializer
    // freezes their last emitter-derived state, and Variable reads fall back
    // to global) because the handle no longer resolves live.
    bool Destroy(VnEmitterHandle handle);

    // Whether a handle names a currently live emitter (slot in range, live,
    // generation matches).
    bool IsLive(VnEmitterHandle handle) const noexcept;
    std::size_t LocalValueCapacity(VnEmitterHandle handle) const noexcept;

    // Sets the shared position / forward orientation. False for a stale
    // handle. The forward vector is stored as given (the API layer validated
    // finiteness and non-zero length and normalized it).
    bool SetPosition(VnEmitterHandle handle, float x, float y, float z);
    bool SetOrientation(VnEmitterHandle handle, float fx, float fy, float fz);

    // Reads the emitter's pose for the spatializer's per-tick fan-out to
    // attached voices. False for a stale handle (the caller keeps the voice's
    // frozen last-known state).
    bool Pose(VnEmitterHandle handle, float* x, float* y, float* z, float* fx,
              float* fy, float* fz) const;

    // Emitter-local Variable overrides. SetLocalValue creates or updates the
    // override (VN_NO_VARIABLE when the emitter's local table is full and the
    // id is new; VN_STALE_HANDLE for a dead handle). ClearLocalValue removes
    // an override (a no-op if absent), reverting that Variable to global scope
    // for the emitter's voices.
    VnResult SetLocalValue(VnEmitterHandle handle, VnVariableId variableId,
                           float value) noexcept;
    VnResult ClearLocalValue(VnEmitterHandle handle, VnVariableId variableId);
    VnResult ClearLocalValues(VnEmitterHandle handle) noexcept;

    // Resolves an emitter-local override: true (and writes *out) only when the
    // handle is live and holds an override for the id. A stale handle or an
    // absent override returns false, so the caller falls back to global scope.
    bool LocalValue(VnEmitterHandle handle, VnVariableId variableId,
                    float* out) const;

    // Observability: emitters currently live, and the most ever live at once.
    std::uint32_t ActiveCount() const { return m_activeCount; }
    std::uint64_t HighWater() const { return m_highWater; }

private:
    struct LocalVar {
        VnVariableId variableId{};
        float value = 0.0f;
        bool inUse = false;
    };

    struct Emitter {
        std::uint32_t generation = 1; // wrap skips 0 (handle 0 never valid)
        bool live = false;
        bool stateOnly = false;
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float fwdX = 0.0f, fwdY = 0.0f, fwdZ = -1.0f;
        std::vector<LocalVar> locals; // fixed size emitterVariableSlots
    };

    static VnEmitterHandle MakeHandle(std::uint32_t slot,
                                      std::uint32_t generation) {
        return (static_cast<std::uint64_t>(generation) << 32) | slot;
    }
    static std::uint32_t SlotOf(VnEmitterHandle handle) noexcept {
        return static_cast<std::uint32_t>(handle);
    }

    Emitter* Live(VnEmitterHandle handle) noexcept;
    const Emitter* Live(VnEmitterHandle handle) const noexcept;
    void NotifyVariableChange(VnEmitterHandle handle, VnVariableId variableId) const noexcept;

    std::vector<Emitter> m_emitters;
    std::vector<LocalVar> m_projectionLocals;
    bool m_projectionActive = false;
    std::uint32_t m_projectionStride = 0;
    std::uint32_t m_activeCount = 0;
    std::uint64_t m_highWater = 0;
    void* m_variableObserverContext = nullptr;
    VariableChangeObserver m_variableObserver = nullptr;
};

} // namespace harken

#endif // HARKEN_PARAM_EMITTER_TABLE_H
