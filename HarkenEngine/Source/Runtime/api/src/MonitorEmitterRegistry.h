#ifndef HARKEN_API_MONITOR_EMITTER_REGISTRY_H
#define HARKEN_API_MONITOR_EMITTER_REGISTRY_H

#include "harken/core/MonitorEmitterIdentity.h"
#include "MonitorHostDescription.h"
#include "harken/param/EmitterTable.h"

#include <vector>

namespace harken {

// Logic-owned diagnostic values, bounded by the actual public emitter table.
// No playback owner or SDK object is retained. Capture does not control storage.
class MonitorEmitterRegistry {
public:
    enum class Change { Created, BindingChanged, Destroyed };
    using ChangeSink = void (*)(Change, const MonitorEmitterOrigin&) noexcept;

    MonitorEmitterRegistry(const EmitterTable& emitters, std::uint32_t capacity,
        std::uint64_t engineInstance, std::uint64_t lastSession = 0)
        : emitters_(emitters), slots_(capacity), engineInstance_(engineInstance),
          lastSession_(lastSession) {}

    std::uint32_t Capacity() const noexcept { return static_cast<std::uint32_t>(slots_.size()); }
    std::uint64_t EngineInstance() const noexcept { return engineInstance_; }
    std::uint64_t Session() const noexcept { return session_; }
    std::uint64_t UnavailableCount() const noexcept { return unavailable_; }
    std::uint64_t StorageBytes() const noexcept { return slots_.size() * sizeof(Slot); }
    void SetChangeSink(ChangeSink sink) noexcept { changeSink_ = sink; }

    std::uint64_t BeginSession() noexcept {
        ClearHosts();
        session_ = 0;
        lastLocalSequence_ = 0;
        if (engineInstance_ == 0 || lastSession_ == UINT64_MAX) {
            NoteUnavailable();
            return 0;
        }
        session_ = ++lastSession_;
        return session_;
    }

    bool EndSession(std::uint64_t engineInstance, std::uint64_t session) noexcept {
        if (engineInstance != engineInstance_ || session == 0 || session != session_) return false;
        ClearHosts();
        session_ = 0;
        return true;
    }

    void NoteCreated(std::uint64_t emitter) noexcept {
        const auto index = static_cast<std::uint32_t>(emitter);
        if (index < slots_.size() && emitters_.IsPublicLive(emitter)) {
            auto& slot = slots_[index];
            slot = {emitter};
            slot.kind = emitters_.Context(emitter).spatialEmitter == 0
                ? MonitorEmitterKind::ExplicitStateOnly : MonitorEmitterKind::ExplicitSpatial;
            Changed(Change::Created, slot);
        }
    }

    void NoteDestroyed(std::uint64_t emitter) noexcept {
        const auto index = static_cast<std::uint32_t>(emitter);
        if (index < slots_.size() && slots_[index].emitter == emitter) {
            // The authoritative table has already advanced its generation.
            // Preserve the last known identity before clearing this value slot.
            const auto retired = slots_[index];
            slots_[index] = {};
            Changed(Change::Destroyed, retired);
        }
    }

    bool Register(std::uint64_t engineInstance, std::uint64_t session, std::uint64_t emitter,
        std::uint64_t localSequence, MonitorHostOwnership ownership,
        std::uint64_t revision, const MonitorHostDescription* description = nullptr) noexcept {
        auto* slot = Find(emitter);
        if (engineInstance != engineInstance_ || slot == nullptr || session == 0 || session != session_ ||
            localSequence == 0 || revision == 0 ||
            (ownership != MonitorHostOwnership::UnityEmitter &&
             ownership != MonitorHostOwnership::UnitySpatialSource &&
             ownership != MonitorHostOwnership::StudioDefaultEmitter)) return false;
        if (ownership == MonitorHostOwnership::StudioDefaultEmitter) {
            for (const auto& other : slots_) {
                if (other.emitter != emitter && other.host.Valid() &&
                    other.host.sdkSession == session &&
                    other.ownership == MonitorHostOwnership::StudioDefaultEmitter &&
                    emitters_.IsPublicLive(other.emitter)) return false;
            }
        }
        const MonitorHostKey key{engineInstance_, session, localSequence};
        if (slot->host.Valid()) {
            if (!(slot->host == key) || slot->ownership != ownership ||
                revision < slot->revision) return false;
            if (description != nullptr && slot->hasDescription && revision == slot->revision &&
                !slot->description.Same(*description)) return false;
        } else {
            // The SDK allocates monotonically within its own registration
            // session. A retired key cannot be assigned to a different object.
            if (localSequence <= lastLocalSequence_) return false;
            lastLocalSequence_ = localSequence;
        }
        const bool changed = !(slot->host == key) || slot->ownership != ownership || slot->revision != revision;
        // Description and identity become visible at the same logical boundary.
        // A V1 revision update cannot relabel an old description as a new value.
        if (description != nullptr) {
            slot->description = *description;
            slot->hasDescription = true;
        } else if (slot->revision != revision) {
            slot->description = {};
            slot->hasDescription = false;
        }
        slot->host = key;
        slot->ownership = ownership;
        slot->revision = revision;
        if (changed) Changed(Change::BindingChanged, *slot);
        return true;
    }

    bool Unregister(std::uint64_t engineInstance, std::uint64_t session, std::uint64_t emitter,
        std::uint64_t localSequence) noexcept {
        auto* slot = Find(emitter);
        if (engineInstance != engineInstance_ || slot == nullptr || session == 0 || session != session_ ||
            !(slot->host == MonitorHostKey{engineInstance_, session, localSequence})) return false;
        ClearHost(*slot);
        Changed(Change::BindingChanged, *slot);
        return true;
    }

    bool CaptureAt(std::uint32_t slot, MonitorEmitterOrigin* out) const noexcept {
        if (out == nullptr) return false;
        *out = {};
        if (slot >= slots_.size()) return false;
        const auto emitter = slots_[slot].emitter;
        if (emitter == 0 || !emitters_.IsPublicLive(emitter)) return false;
        *out = CaptureOrigin(emitter);
        return true;
    }

    bool CaptureDescriptionAt(std::uint32_t index, MonitorEmitterOrigin& origin,
        MonitorHostDescription& description) const noexcept {
        origin = {};
        description = {};
        if (index >= slots_.size()) return false;
        const auto& slot = slots_[index];
        if (!slot.hasDescription || !slot.host.Valid() || !emitters_.IsPublicLive(slot.emitter)) return false;
        origin = CaptureOrigin(slot.emitter);
        description = slot.description;
        return true;
    }

    MonitorEmitterOrigin CaptureOrigin(std::uint64_t emitter) const noexcept {
        MonitorEmitterOrigin result{};
        result.emitter = emitter;
        if (emitter == 0) {
            result.kind = MonitorEmitterKind::None;
        } else if (emitter == emitters_.DefaultState()) {
            result.kind = MonitorEmitterKind::DefaultState;
        } else if (emitters_.IsPublicLive(emitter)) {
            result.kind = emitters_.Context(emitter).spatialEmitter == 0
                ? MonitorEmitterKind::ExplicitStateOnly : MonitorEmitterKind::ExplicitSpatial;
            const auto index = static_cast<std::uint32_t>(emitter);
            if (index < slots_.size() && slots_[index].emitter == emitter) {
                const auto& slot = slots_[index];
                result.host = slot.host;
                result.ownership = slot.ownership;
                result.registrationRevision = slot.revision;
            }
        }
        return result;
    }

    MonitorEmitterBinding CaptureBinding(const MonitorEmitterOrigin& origin,
        std::uint64_t state, std::uint64_t spatial) const noexcept {
        MonitorEmitterBinding result{};
        if (state != 0 && emitters_.IsLive(state)) {
            result.stateEmitter = state;
            result.state = MonitorBindingState::Bound;
        } else if (state != 0 || origin.emitter != 0) {
            result.state = MonitorBindingState::Detached;
        }
        if (spatial != 0 && emitters_.IsPublicLive(spatial) &&
            emitters_.Context(spatial).spatialEmitter == spatial) {
            result.spatialEmitter = spatial;
            result.spatial = MonitorBindingState::Bound;
        } else if (spatial != 0 || origin.kind == MonitorEmitterKind::ExplicitSpatial) {
            result.spatial = MonitorBindingState::Detached;
        }
        return result;
    }

private:
    struct Slot {
        std::uint64_t emitter = 0;
        MonitorHostKey host{};
        std::uint64_t revision = 0;
        MonitorHostOwnership ownership = MonitorHostOwnership::Unknown;
        MonitorEmitterKind kind = MonitorEmitterKind::Unknown;
        bool hasDescription = false;
        MonitorHostDescription description{};
    };
    void Changed(Change change, const Slot& slot) const noexcept {
        if (changeSink_ != nullptr)
            changeSink_(change, {slot.emitter, slot.host, slot.revision, slot.kind, slot.ownership});
    }
    static void ClearHost(Slot& slot) noexcept {
        slot.host = {};
        slot.revision = 0;
        slot.ownership = MonitorHostOwnership::Unknown;
        slot.hasDescription = false;
        slot.description = {};
    }
    Slot* Find(std::uint64_t emitter) noexcept {
        const auto index = static_cast<std::uint32_t>(emitter);
        return index < slots_.size() && slots_[index].emitter == emitter &&
            emitters_.IsPublicLive(emitter) ? &slots_[index] : nullptr;
    }
    void ClearHosts() noexcept {
        for (auto& slot : slots_) {
            const bool changed = slot.host.Valid();
            ClearHost(slot);
            if (changed) Changed(Change::BindingChanged, slot);
        }
    }
    void NoteUnavailable() noexcept { if (unavailable_ != UINT64_MAX) ++unavailable_; }

    const EmitterTable& emitters_;
    std::vector<Slot> slots_;
    std::uint64_t engineInstance_ = 0;
    std::uint64_t lastSession_ = 0;
    std::uint64_t session_ = 0;
    std::uint64_t lastLocalSequence_ = 0;
    std::uint64_t unavailable_ = 0;
    ChangeSink changeSink_ = nullptr;
};

} // namespace harken

#endif
