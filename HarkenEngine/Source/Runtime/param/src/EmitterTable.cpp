#include "harken/param/EmitterTable.h"
#include "harken/core/LogicAllocCheckpoint.h"

#include <algorithm>
#include <stdexcept>

namespace harken {

EmitterTable::EmitterTable(const EngineConfig& config, std::uint32_t generationFloor)
    : m_emitters(static_cast<std::size_t>(config.maxEmitters) + 1u) {
    if (generationFloor >= UINT32_MAX - 1u)
        throw std::invalid_argument("emitter handle generation exhausted");
    for (Emitter& emitter : m_emitters) {
        emitter.generation = generationFloor + 1;
        emitter.locals.assign(config.emitterVariableSlots, LocalVar{});
        emitter.projectionLocals.resize(config.emitterVariableSlots);
    }
    m_emitters.back().generation = 1;
    m_emitters.back().live = true;
    m_emitters.back().stateOnly = true;
}

std::uint32_t EmitterTable::GenerationHighWater() const noexcept {
    std::uint32_t maximum = 0;
    for (std::size_t i = 0; i + 1 < m_emitters.size(); ++i)
        maximum = std::max(maximum, m_emitters[i].generation);
    return maximum;
}

bool EmitterTable::BeginVariableProjection() noexcept {
    if (m_projectionActive) return false;
    for (std::size_t i = 0; i < m_emitters.size(); ++i) {
        const auto& locals = m_emitters[i].locals;
        std::copy(locals.begin(), locals.end(), m_emitters[i].projectionLocals.begin());
    }
    m_projectionActive = true;
    return true;
}

void EmitterTable::RestoreVariableProjection() noexcept {
    if (!m_projectionActive) return;
    for (std::size_t i = 0; i < m_emitters.size(); ++i) {
        auto& locals = m_emitters[i].locals;
        const auto begin = m_emitters[i].projectionLocals.begin();
        const auto handle = MakeHandle(static_cast<std::uint32_t>(i), m_emitters[i].generation);
        for (std::size_t slot = 0; slot < locals.size(); ++slot) {
            const auto previous = locals[slot];
            const auto restored = begin[slot];
            locals[slot] = restored;
            const bool sameVariable = previous.inUse && restored.inUse &&
                                      previous.variableId == restored.variableId;
            if (previous.inUse && (!sameVariable || previous.value != restored.value))
                NotifyVariableChange(handle, previous.variableId);
            if (restored.inUse && !sameVariable)
                NotifyVariableChange(handle, restored.variableId);
        }
    }
    m_projectionActive = false;
}

std::uint64_t EmitterTable::VariableProjectionStorageBytes() const noexcept {
    std::uint64_t bytes = 0;
    for (const auto& emitter : m_emitters)
        bytes += emitter.projectionLocals.capacity() * sizeof(LocalVar);
    return bytes;
}

bool EmitterTable::Create(VnEmitterHandle* outHandle, bool stateOnly) {
    for (std::uint32_t slot = 0; slot + 1u < m_emitters.size(); ++slot) {
        Emitter& emitter = m_emitters[slot];
        if (emitter.live || emitter.generation == UINT32_MAX) {
            continue;
        }
        emitter.live = true;
        emitter.stateOnly = stateOnly;
        emitter.spatial = SpatialSourceState{};
        for (LocalVar& local : emitter.locals) {
            local = LocalVar{};
        }
        *outHandle = MakeHandle(slot, emitter.generation);
        ++m_activeCount;
        if (m_activeCount > m_highWater) {
            m_highWater = m_activeCount;
        }
        return true;
    }
    return false; // every slot live: the caller maps this to VN_NO_EMITTER
}

bool EmitterTable::Destroy(VnEmitterHandle handle) {
    if (!IsPublicLive(handle)) return false;
    Emitter* emitter = Live(handle);
    if (emitter == nullptr) {
        return false;
    }
    emitter->live = false;
    // A wrapped generation would make a future handle alias an old one. Keep
    // the slot permanently unavailable instead of cycling through generation 1.
    if (emitter->generation != UINT32_MAX) {
        ++emitter->generation;
    }
    for (LocalVar& local : emitter->locals) {
        const auto previous = local;
        local = LocalVar{};
        if (previous.inUse) NotifyVariableChange(handle, previous.variableId);
    }
    std::vector<LocalVar>().swap(emitter->locals);
    std::vector<LocalVar>().swap(emitter->projectionLocals);
    --m_activeCount;
    return true;
}

bool EmitterTable::IsLive(VnEmitterHandle handle) const noexcept {
    return Live(handle) != nullptr;
}

bool EmitterTable::IsPublicLive(VnEmitterHandle handle) const noexcept {
    return handle != DefaultState() && IsLive(handle);
}

VnEmitterHandle EmitterTable::DefaultState() const noexcept {
    return MakeHandle(static_cast<std::uint32_t>(m_emitters.size() - 1u), 1);
}

PlaybackExecutionContext EmitterTable::Context(VnEmitterHandle state) const noexcept {
    const auto* emitter = Live(state);
    return {state, emitter != nullptr && !emitter->stateOnly ? state : 0};
}

std::uint64_t EmitterTable::StorageBytesPerEmitter(std::uint32_t localSlots) noexcept {
    return sizeof(Emitter) + static_cast<std::uint64_t>(localSlots) * sizeof(LocalVar) * 2u;
}

std::size_t EmitterTable::LocalValueCapacity(VnEmitterHandle handle) const noexcept {
    const auto* emitter = Live(handle);
    return emitter != nullptr ? emitter->locals.size() : 0;
}

VnResult EmitterTable::PrepareLocalValueStorage(VnEmitterHandle handle, std::size_t count) noexcept {
    auto* emitter = Live(handle);
    if (emitter == nullptr) return VN_STALE_HANDLE;
    if (count <= emitter->locals.size()) return VN_OK;
    if (m_projectionActive) return VN_BACKPRESSURE;
    try {
        const auto size = std::max(count, emitter->locals.size() * 2u);
        // Reserve both copies before exposing slots; failure preserves all values.
        detail::CheckedGrow(emitter->locals, size, detail::LogicAllocSite::VariableValueStorage);
        emitter->locals.reserve(size);
        detail::CheckedGrow(emitter->projectionLocals, size, detail::LogicAllocSite::VariableValueStorage);
        emitter->projectionLocals.reserve(size);
        emitter->locals.resize(size);
        emitter->projectionLocals.resize(size);
        return VN_OK;
    } catch (const std::bad_alloc&) {
        detail::RecordCurrentLogicAllocFailure(detail::LogicAllocSite::VariableValueStorage);
        return VN_OUT_OF_MEMORY;
    } catch (const std::length_error&) {
        return VN_BACKPRESSURE;
    }
}

bool EmitterTable::SetPosition(VnEmitterHandle handle, float x, float y,
                               float z) {
    Emitter* emitter = Live(handle);
    if (emitter == nullptr || emitter->stateOnly) {
        return false;
    }
    emitter->spatial.posX = x;
    emitter->spatial.posY = y;
    emitter->spatial.posZ = z;
    return true;
}

bool EmitterTable::SetOrientation(VnEmitterHandle handle, float fx, float fy,
                                  float fz) {
    Emitter* emitter = Live(handle);
    if (emitter == nullptr || emitter->stateOnly) {
        return false;
    }
    emitter->spatial.fwdX = fx;
    emitter->spatial.fwdY = fy;
    emitter->spatial.fwdZ = fz;
    return true;
}

bool EmitterTable::SetDistanceScale(VnEmitterHandle handle, float scale) {
    Emitter* emitter = Live(handle);
    if (emitter == nullptr || emitter->stateOnly) {
        return false;
    }
    emitter->spatial.emitterDistanceScale = scale;
    return true;
}

bool EmitterTable::Pose(VnEmitterHandle handle, SpatialSourceState* out) const {
    const Emitter* emitter = Live(handle);
    if (emitter == nullptr || emitter->stateOnly) {
        return false;
    }
    *out = emitter->spatial;
    return true;
}

VnResult EmitterTable::SetLocalValue(VnEmitterHandle handle,
                                     VnVariableId variableId, float value) noexcept {
    Emitter* emitter = Live(handle);
    if (emitter == nullptr) {
        return VN_STALE_HANDLE;
    }
    LocalVar* free = nullptr;
    for (LocalVar& local : emitter->locals) {
        if (local.inUse && local.variableId == variableId) {
            const bool changed = local.value != value;
            local.value = value; // update in place
            if (changed) NotifyVariableChange(handle, variableId);
            return VN_OK;
        }
        if (!local.inUse && free == nullptr) {
            free = &local; // remember the first free slot for a new id
        }
    }
    if (free == nullptr) {
        const auto slot = emitter->locals.size();
        const auto result = PrepareLocalValueStorage(handle, slot + 1u);
        if (result != VN_OK) return result;
        free = &emitter->locals[slot];
    }
    free->inUse = true;
    free->variableId = variableId;
    free->value = value;
    NotifyVariableChange(handle, variableId);
    return VN_OK;
}

VnResult EmitterTable::ClearLocalValue(VnEmitterHandle handle,
                                       VnVariableId variableId) {
    Emitter* emitter = Live(handle);
    if (emitter == nullptr) {
        return VN_STALE_HANDLE;
    }
    for (LocalVar& local : emitter->locals) {
        if (local.inUse && local.variableId == variableId) {
            local = LocalVar{};
            NotifyVariableChange(handle, variableId);
            return VN_OK;
        }
    }
    return VN_OK;
}

VnResult EmitterTable::ClearLocalValues(VnEmitterHandle handle) noexcept {
    Emitter* emitter = Live(handle);
    if (emitter == nullptr) {
        return VN_STALE_HANDLE;
    }
    for (LocalVar& local : emitter->locals) {
        const auto previous = local;
        local = LocalVar{};
        if (previous.inUse) NotifyVariableChange(handle, previous.variableId);
    }
    return VN_OK;
}

bool EmitterTable::LocalValue(VnEmitterHandle handle, VnVariableId variableId,
                              float* out) const {
    const Emitter* emitter = Live(handle);
    if (emitter == nullptr) {
        return false;
    }
    for (const LocalVar& local : emitter->locals) {
        if (local.inUse && local.variableId == variableId) {
            *out = local.value;
            return true;
        }
    }
    return false;
}

void EmitterTable::NotifyVariableChange(VnEmitterHandle handle, VnVariableId variableId) const noexcept {
    if (m_variableObserver != nullptr)
        m_variableObserver(m_variableObserverContext, handle, variableId);
}

EmitterTable::Emitter* EmitterTable::Live(VnEmitterHandle handle) noexcept {
    const std::uint32_t slot = SlotOf(handle);
    if (handle == 0 || slot >= m_emitters.size() || !m_emitters[slot].live ||
        m_emitters[slot].generation !=
            static_cast<std::uint32_t>(handle >> 32)) {
        return nullptr;
    }
    return &m_emitters[slot];
}

const EmitterTable::Emitter* EmitterTable::Live(VnEmitterHandle handle) const noexcept {
    const std::uint32_t slot = SlotOf(handle);
    if (handle == 0 || slot >= m_emitters.size() || !m_emitters[slot].live ||
        m_emitters[slot].generation !=
            static_cast<std::uint32_t>(handle >> 32)) {
        return nullptr;
    }
    return &m_emitters[slot];
}

} // namespace harken
