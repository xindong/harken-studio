#include "harken/param/EmitterTable.h"

#include <algorithm>

namespace harken {

EmitterTable::EmitterTable(const EngineConfig& config)
    : m_emitters(static_cast<std::size_t>(config.maxEmitters) + 1u) {
    for (Emitter& emitter : m_emitters) {
        emitter.locals.assign(config.emitterVariableSlots, LocalVar{});
    }
    m_emitters.back().live = true;
    m_emitters.back().stateOnly = true;
    m_projectionStride = config.emitterVariableSlots;
    m_projectionLocals.resize(m_emitters.size() * m_projectionStride);
}

bool EmitterTable::BeginVariableProjection() noexcept {
    if (m_projectionActive) return false;
    for (std::size_t i = 0; i < m_emitters.size(); ++i) {
        const auto& locals = m_emitters[i].locals;
        std::copy(locals.begin(), locals.end(), m_projectionLocals.begin() + i * m_projectionStride);
    }
    m_projectionActive = true;
    return true;
}

void EmitterTable::RestoreVariableProjection() noexcept {
    if (!m_projectionActive) return;
    for (std::size_t i = 0; i < m_emitters.size(); ++i) {
        auto& locals = m_emitters[i].locals;
        const auto begin = m_projectionLocals.begin() + i * m_projectionStride;
        std::copy(begin, begin + m_projectionStride, locals.begin());
    }
    m_projectionActive = false;
}

std::uint64_t EmitterTable::VariableProjectionStorageBytes() const noexcept {
    return m_projectionLocals.size() * sizeof(LocalVar);
}

bool EmitterTable::Create(VnEmitterHandle* outHandle, bool stateOnly) {
    for (std::uint32_t slot = 0; slot + 1u < m_emitters.size(); ++slot) {
        Emitter& emitter = m_emitters[slot];
        if (emitter.live || emitter.generation == UINT32_MAX) {
            continue;
        }
        emitter.live = true;
        emitter.stateOnly = stateOnly;
        emitter.posX = emitter.posY = emitter.posZ = 0.0f;
        emitter.fwdX = 0.0f;
        emitter.fwdY = 0.0f;
        emitter.fwdZ = -1.0f;
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
        local = LocalVar{};
    }
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
    return sizeof(Emitter) + static_cast<std::uint64_t>(localSlots) * sizeof(LocalVar);
}

std::size_t EmitterTable::LocalValueCapacity(VnEmitterHandle handle) const noexcept {
    const auto* emitter = Live(handle);
    return emitter != nullptr ? emitter->locals.size() : 0;
}

bool EmitterTable::SetPosition(VnEmitterHandle handle, float x, float y,
                               float z) {
    Emitter* emitter = Live(handle);
    if (emitter == nullptr || emitter->stateOnly) {
        return false;
    }
    emitter->posX = x;
    emitter->posY = y;
    emitter->posZ = z;
    return true;
}

bool EmitterTable::SetOrientation(VnEmitterHandle handle, float fx, float fy,
                                  float fz) {
    Emitter* emitter = Live(handle);
    if (emitter == nullptr || emitter->stateOnly) {
        return false;
    }
    emitter->fwdX = fx;
    emitter->fwdY = fy;
    emitter->fwdZ = fz;
    return true;
}

bool EmitterTable::Pose(VnEmitterHandle handle, float* x, float* y, float* z,
                        float* fx, float* fy, float* fz) const {
    const Emitter* emitter = Live(handle);
    if (emitter == nullptr || emitter->stateOnly) {
        return false;
    }
    *x = emitter->posX;
    *y = emitter->posY;
    *z = emitter->posZ;
    *fx = emitter->fwdX;
    *fy = emitter->fwdY;
    *fz = emitter->fwdZ;
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
            local.value = value; // update in place
            return VN_OK;
        }
        if (!local.inUse && free == nullptr) {
            free = &local; // remember the first free slot for a new id
        }
    }
    if (free == nullptr) {
        return VN_NO_VARIABLE; // this emitter's local table is full
    }
    free->inUse = true;
    free->variableId = variableId;
    free->value = value;
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
        local = LocalVar{};
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
