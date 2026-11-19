#include "harken/param/EmitterTable.h"
#include "harken/core/LogicAllocCheckpoint.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace harken {

EmitterTable::EmitterTable(const EngineConfig& config, std::uint32_t generationFloor)
    : m_initialGeneration(generationFloor + 1u), m_initialLocalSlots(config.emitterVariableSlots) {
    if (generationFloor >= UINT32_MAX - 1u)
        throw std::invalid_argument("emitter handle generation exhausted");
    m_default.live = true;
    m_default.stateOnly = true;
    m_default.locals.resize(m_initialLocalSlots);
    m_default.projectionLocals.resize(m_initialLocalSlots);
}

std::uint32_t EmitterTable::GenerationHighWater() const noexcept {
    std::uint32_t maximum = m_initialGeneration;
    for (std::size_t i = 0; i < m_emitters.size(); ++i)
        maximum = std::max(maximum, m_emitters[i].generation);
    return maximum;
}

bool EmitterTable::BeginVariableProjection() noexcept {
    if (m_projectionActive) return false;
    for (std::size_t i = 0; i < m_emitters.size(); ++i) {
        const auto& locals = m_emitters[i].locals;
        std::copy(locals.begin(), locals.end(), m_emitters[i].projectionLocals.begin());
    }
    std::copy(m_default.locals.begin(), m_default.locals.end(), m_default.projectionLocals.begin());
    m_projectionActive = true;
    return true;
}

void EmitterTable::RestoreVariableProjection() noexcept {
    if (!m_projectionActive) return;
    const auto restore = [this](Emitter& emitter, VnEmitterHandle handle) {
        auto& locals = emitter.locals;
        const auto begin = emitter.projectionLocals.begin();
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
    };
    for (std::uint32_t i = 0; i < m_emitters.size(); ++i)
        restore(m_emitters[i], MakeHandle(PublicSlot(i), m_emitters[i].generation));
    restore(m_default, DefaultState());
    m_projectionActive = false;
}

std::uint64_t EmitterTable::VariableProjectionStorageBytes() const noexcept {
    std::uint64_t bytes = m_default.projectionLocals.capacity() * sizeof(LocalVar);
    for (const auto& emitter : m_emitters)
        bytes += emitter.projectionLocals.capacity() * sizeof(LocalVar);
    return bytes;
}

VnResult EmitterTable::PrepareCreate(VnEmitterHandle& candidate) noexcept {
    candidate = 0;
    auto index = m_firstFree;
    while (index < m_emitters.size() &&
           (m_emitters[index].live || m_emitters[index].generation == UINT32_MAX)) ++index;
    if (index >= UINT32_MAX - 1u) return VN_NO_EMITTER;
    const bool fresh = index == m_emitters.size();
    if (!m_emitters.ConstructThrough(index + 1u)) return VN_OUT_OF_MEMORY;
    auto& emitter = m_emitters[index];
    if (fresh) emitter.generation = m_initialGeneration;
    try {
        if (emitter.generation != m_initialGeneration) {
            candidate = MakeHandle(PublicSlot(index), emitter.generation);
            return VN_OK;
        }
        emitter.locals.reserve(m_initialLocalSlots);
        emitter.projectionLocals.reserve(m_initialLocalSlots);
        emitter.locals.resize(m_initialLocalSlots);
        emitter.projectionLocals.resize(m_initialLocalSlots);
    } catch (const std::bad_alloc&) {
        detail::RecordCurrentLogicAllocFailure(detail::LogicAllocSite::VariableValueStorage);
        return VN_OUT_OF_MEMORY;
    }
    candidate = MakeHandle(PublicSlot(index), emitter.generation);
    return VN_OK;
}

void EmitterTable::PublishCreate(VnEmitterHandle candidate, bool stateOnly) noexcept {
    const auto index = StorageIndex(SlotOf(candidate));
    auto& emitter = m_emitters[index];
    assert(!emitter.live && emitter.generation == candidate >> 32);
    emitter.stateOnly = stateOnly;
    emitter.spatial = {};
    emitter.live = true;
    m_firstFree = index + 1u;
    ++m_activeCount;
    m_highWater = std::max<std::uint64_t>(m_highWater, m_activeCount);
}

bool EmitterTable::Create(VnEmitterHandle* outHandle, bool stateOnly) {
    if (PrepareCreate(*outHandle) != VN_OK) return false;
    PublishCreate(*outHandle, stateOnly);
    return true;
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
    m_firstFree = std::min(m_firstFree, StorageIndex(SlotOf(handle)));
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
    return MakeHandle(kDefaultSlot, 1);
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
    return const_cast<Emitter*>(std::as_const(*this).Live(handle));
}

const EmitterTable::Emitter* EmitterTable::Live(VnEmitterHandle handle) const noexcept {
    if (handle == DefaultState()) return &m_default;
    const auto slot = SlotOf(handle);
    if (handle == 0 || slot == kDefaultSlot) return nullptr;
    const auto* emitter = m_emitters.TryAt(StorageIndex(slot));
    return emitter != nullptr && emitter->live && emitter->generation == handle >> 32
        ? emitter : nullptr;
}

} // namespace harken
