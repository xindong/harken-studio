#ifndef HARKEN_ENGINE_NATIVE_BACKING_ENVELOPE_H
#define HARKEN_ENGINE_NATIVE_BACKING_ENVELOPE_H

// Count under the retained content authority before retiring original sources.
// This helper allocates nothing and owns no native rights.
#include <harken/core/PlayablePropertyCut.h>
#include <harken/core/PackPlaybackView.h>
#include <harken/core/CurrentSelection.h>
#include <harken/core/EngineConfig.h>
#include <harken/core/TimelineProgram.h>
#include <harken/render/generated/harken_effect_registry.g.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace harken::native_backing {

inline constexpr std::uint32_t kLeafCapacity = kMaxKnownScheduleLeaves;
inline constexpr std::uint32_t kPathCapacity = kSelectionPathNodeCapacity;
inline constexpr std::uint32_t kControlSideCapacity = 2u * (kLeafCapacity + kPathCapacity + 1u);
static_assert(kLeafCapacity == 64 && kPathCapacity == 64);

enum class Status : std::uint8_t {
    Ok,
    InvalidInput,
    MissingContent,
    FingerprintMismatch,
    UnsupportedKind,
    UnsupportedMode,
    WorkspaceExhausted,
    AuthorityMismatch,
};

enum Flag : std::uint32_t {
    UnprunedSchedule = 1u << 0,
    ExistingDynamicPath = 1u << 1,
    ExistingDirectRefusal = 1u << 2,
    SaturatedLegalBound = 1u << 3,
    OmittedDepthBranch = 1u << 4,
    OmittedPathBranch = 1u << 5,
    OmittedSeedBranch = 1u << 6,
    IndependentFutureEnvelope = 1u << 7,
};

struct VoiceColumns {
    std::uint32_t bindings = 0;
    std::uint32_t path = 0;
    std::uint32_t sends = 0;
    std::uint32_t effects = 0;
    std::array<std::uint32_t, kEffectRegistryRowCount> effectInstances{};

    void Include(const VoiceColumns& other) noexcept {
        bindings = std::max(bindings, other.bindings);
        path = std::max(path, other.path);
        sends = std::max(sends, other.sends);
        effects = std::max(effects, other.effects);
        for (std::size_t type = 0; type < effectInstances.size(); ++type)
            effectInstances[type] = std::max(effectInstances[type], other.effectInstances[type]);
    }
    void AddPathPrefix(std::uint32_t rows) noexcept {
        if (path != 0) path = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            kPlaybackPathRowCapacity, static_cast<std::uint64_t>(path) + rows));
    }
};

struct QueueColumns {
    std::uint32_t programs = 0;
    std::uint32_t events = 0;

    void Include(const QueueColumns& other) noexcept {
        programs = std::max(programs, other.programs);
        events = std::max(events, other.events);
    }
};

struct Profile {
    std::uint32_t current = 0;
    std::uint32_t referenceRows = 0;
    std::array<std::uint32_t, kLeafCapacity + 1u> H{};
    std::array<std::uint32_t, kLeafCapacity + 1u> controllerWeights{};
    std::uint32_t rootContinuousMembers = 0;
    std::uint32_t Q = 0;
    std::uint32_t controlSides = 0;
    VoiceColumns voice{};
    QueueColumns queue{};
};

struct Result {
    Status status = Status::Ok;
    std::uint32_t flags = 0;
    Profile profile{};
    explicit operator bool() const noexcept { return status == Status::Ok; }
};

struct Limits {
    std::uint32_t immediateFutureSides = 0;
    std::uint32_t queueRoleCapacity = 0;
    std::uint32_t currentScheduleDepth = 0;
    std::uint32_t directFutureScheduleDepth = 0;
    std::uint32_t propertyPrefixCount = 0;
    std::uint32_t seedPrefixCount = 0;
};

struct AuthorityStamp {
    std::uint64_t generation = 0;
    std::uint64_t semanticFingerprint = 0;
    bool operator==(const AuthorityStamp& other) const noexcept {
        return generation == other.generation && semanticFingerprint == other.semanticFingerprint;
    }
};

inline bool SameRef(const PlayableRefV1& a, const PlayableRefV1& b) noexcept {
    return a.kind == b.kind && a.reserved == b.reserved && a.stableId == b.stableId &&
        a.expectedProgramSemanticFingerprint == b.expectedProgramSemanticFingerprint &&
        a.contentRevision == b.contentRevision && a.routeStableId == b.routeStableId &&
        a.compatibilityFingerprint == b.compatibilityFingerprint;
}

class WorldContent {
public:
    explicit WorldContent(const PlayableWorldViewV1& view,
        std::uint32_t liveBindingCapacity = kMaxVoiceBindingInstanceSlots) noexcept
        : view_(view), liveBindingCapacity_(liveBindingCapacity) {}
    AuthorityStamp Authority() const noexcept {
        return {view_.generation, view_.identity.runtimeSemanticFingerprint};
    }
    static constexpr bool kDirectNestedContainer = true;
    std::uint32_t QueueResetScheduleDepth() const noexcept { return view_.maxTrackPieceScheduleDepth; }
    const PlayablePropertyProgramViewV1* Property(const PlayableRefV1& ref) const noexcept {
        for (std::uint32_t i = 0; view_.properties != nullptr && i < view_.propertyCount; ++i)
            if (view_.properties[i].stableId == ref.stableId &&
                view_.properties[i].programSemanticFingerprint == ref.expectedProgramSemanticFingerprint)
                return &view_.properties[i];
        return nullptr;
    }
    std::uint32_t BindingCount(VnObjectId) const noexcept { return liveBindingCapacity_; }
    const ContainerProgramViewV3* Container(const PlayableRefV1& ref) const noexcept {
        if (view_.containers == nullptr) return nullptr;
        for (std::uint32_t i = 0; i < view_.containerCount; ++i) {
            const auto& item = view_.containers[i];
            if (item.containerStableId == ref.stableId &&
                item.programSemanticFingerprint == ref.expectedProgramSemanticFingerprint) return &item;
        }
        return nullptr;
    }
    const TrackScheduleProgramViewV1* Track(const PlayableRefV1& ref) const noexcept {
        if (view_.trackSchedules == nullptr) return nullptr;
        for (std::uint32_t i = 0; i < view_.trackScheduleCount; ++i)
            if (SameRef(view_.trackSchedules[i].rootDefinition, ref)) return &view_.trackSchedules[i];
        return nullptr;
    }
    const PieceScheduleProgramViewV1* Piece(const PlayableRefV1& ref) const noexcept {
        if (view_.pieceSchedules == nullptr) return nullptr;
        for (std::uint32_t i = 0; i < view_.pieceScheduleCount; ++i)
            if (SameRef(view_.pieceSchedules[i].rootDefinition, ref)) return &view_.pieceSchedules[i];
        return nullptr;
    }
private:
    const PlayableWorldViewV1& view_;
    std::uint32_t liveBindingCapacity_ = 0;
};

class PackContent {
public:
    explicit PackContent(const PackPlaybackView& view) noexcept : view_(view) {}
    AuthorityStamp Authority() const noexcept {
        return {view_.CompositeGeneration(), view_.WorldSemanticFingerprint()};
    }
    static constexpr bool kDirectNestedContainer = false;
    std::uint32_t QueueResetScheduleDepth() const noexcept { return kPathCapacity; }
    const PlayablePropertyProgramViewV1* Property(const PlayableRefV1& ref) const noexcept {
        using Kind = playableproperties::ContentPropertyKindV1;
        const auto kind = ref.kind == PlayableKindV1::Sound ? Kind::Sound
            : ref.kind == PlayableKindV1::Container ? Kind::Container
            : ref.kind == PlayableKindV1::Track ? Kind::Track : Kind::Piece;
        return view_.ResolveProperty(kind, ref.stableId);
    }
    std::uint32_t BindingCount(VnObjectId object) const noexcept {
        return view_.ResolveVoiceBindings(object, nullptr, 0);
    }
    const ContainerProgramViewV3* Container(const PlayableRefV1& ref) const noexcept {
        return view_.ResolveContainer(ref.stableId, ref.expectedProgramSemanticFingerprint);
    }
    const TrackScheduleProgramViewV1* Track(const PlayableRefV1& ref) const noexcept {
        const auto* item = view_.ResolveTrack(ref.stableId, 0);
        return item != nullptr && SameRef(item->rootDefinition, ref) ? item : nullptr;
    }
    const PieceScheduleProgramViewV1* Piece(const PlayableRefV1& ref) const noexcept {
        const auto* item = view_.ResolvePiece(ref.stableId, 0);
        return item != nullptr && SameRef(item->rootDefinition, ref) ? item : nullptr;
    }
private:
    const PackPlaybackView& view_;
};

inline Result Failure(Status status) noexcept {
    Result result{};
    result.status = status;
    return result;
}

inline Result Omitted(std::uint32_t flags) noexcept {
    Result result{};
    result.flags = flags;
    return result;
}

inline std::uint32_t SaturatingAdd(std::uint32_t a, std::uint32_t b,
    std::uint32_t limit, std::uint32_t& flags) noexcept {
    if (a > limit || b > limit - a) {
        flags |= SaturatedLegalBound;
        return limit;
    }
    return a + b;
}

inline Result Concurrent(Result a, const Result& b, const Limits& limits) noexcept {
    if (!a) return a;
    if (!b) return b;
    a.flags |= b.flags;
    a.profile.current = SaturatingAdd(a.profile.current, b.profile.current, kLeafCapacity, a.flags);
    a.profile.referenceRows = SaturatingAdd(a.profile.referenceRows, b.profile.referenceRows, UINT32_MAX, a.flags);
    a.profile.Q = SaturatingAdd(a.profile.Q, b.profile.Q, limits.queueRoleCapacity, a.flags);
    a.profile.controlSides = SaturatingAdd(a.profile.controlSides, b.profile.controlSides,
        kControlSideCapacity, a.flags);
    a.profile.voice.Include(b.profile.voice);
    a.profile.queue.Include(b.profile.queue);
    a.profile.rootContinuousMembers = 0;
    for (std::uint32_t t = 1; t <= kLeafCapacity; ++t) {
        a.profile.H[t] = SaturatingAdd(a.profile.H[t], b.profile.H[t], limits.immediateFutureSides, a.flags);
        a.profile.controllerWeights[t] = SaturatingAdd(a.profile.controllerWeights[t], b.profile.controllerWeights[t],
            kControlSideCapacity, a.flags);
    }
    return a;
}

inline Result Alternative(Result a, const Result& b) noexcept {
    if (!a) return a;
    if (!b) return b;
    a.flags |= b.flags;
    a.profile.current = std::max(a.profile.current, b.profile.current);
    a.profile.referenceRows = std::max(a.profile.referenceRows, b.profile.referenceRows);
    a.profile.Q = std::max(a.profile.Q, b.profile.Q);
    a.profile.controlSides = std::max(a.profile.controlSides, b.profile.controlSides);
    a.profile.voice.Include(b.profile.voice);
    a.profile.queue.Include(b.profile.queue);
    a.profile.rootContinuousMembers = 0;
    for (std::uint32_t t = 1; t <= kLeafCapacity; ++t) {
        a.profile.H[t] = std::max(a.profile.H[t], b.profile.H[t]);
        a.profile.controllerWeights[t] = std::max(a.profile.controllerWeights[t], b.profile.controllerWeights[t]);
    }
    return a;
}

inline void AddFuture(Result& result, std::uint32_t capacity, const Limits& limits) noexcept {
    for (std::uint32_t t = 1; t <= capacity; ++t)
        result.profile.H[t] = SaturatingAdd(result.profile.H[t], 1, limits.immediateFutureSides, result.flags);
    if (capacity != 0) result.flags |= IndependentFutureEnvelope;
}

inline std::uint64_t ReservedRows(const Profile& profile) noexcept {
    std::uint64_t rows = profile.current;
    for (std::uint32_t t = 1; t <= kLeafCapacity; ++t) rows += profile.H[t];
    return rows;
}

template<class Emit>
bool EmitDescendingCapacities(const std::array<std::uint32_t, kLeafCapacity + 1u>& capacities,
    Emit&& emit) noexcept {
    for (std::uint32_t t = 1; t < kLeafCapacity; ++t)
        if (capacities[t] < capacities[t + 1u]) return false;
    std::uint32_t next = 0;
    for (std::uint32_t t = kLeafCapacity; t != 0; --t) {
        const auto count = capacities[t] - next;
        for (std::uint32_t i = 0; i < count; ++i) if (!emit(t)) return false;
        next = capacities[t];
    }
    return true;
}

template<class Emit>
bool EmitDescendingFutureCapacities(const Profile& profile, Emit&& emit) noexcept {
    return EmitDescendingCapacities(profile.H, std::forward<Emit>(emit));
}

enum class Mode : std::uint8_t { Audit, Frontier, StaticSchedule, DirectNested };
enum class Domain : std::uint8_t { Current, DirectFuture };

struct Budget {
    std::uint32_t containers = 0;
    std::uint32_t schedules = 0;
    std::uint32_t materializedPath = 0;
    std::uint32_t frontierDepth = 0;
    Domain domain = Domain::Current;
    bool operator==(const Budget& other) const noexcept {
        return containers == other.containers && schedules == other.schedules &&
            materializedPath == other.materializedPath && frontierDepth == other.frontierDepth &&
            domain == other.domain;
    }
};

struct MemoKey {
    PlayableRefV1 ref{};
    Mode mode = Mode::Audit;
    Budget budget{};
    bool operator==(const MemoKey& other) const noexcept {
        return SameRef(ref, other.ref) && mode == other.mode && budget == other.budget;
    }
};

enum class MemoState : std::uint8_t { Empty, Visiting, Ready };
struct MemoEntry {
    MemoKey key{};
    Result result{};
    std::size_t next = SIZE_MAX;
    MemoState state = MemoState::Empty;
};

inline std::uint64_t Hash(const MemoKey& key) noexcept {
    std::uint64_t value = UINT64_C(14695981039346656037);
    const auto mix = [&](std::uint64_t part) { value = (value ^ part) * UINT64_C(1099511628211); };
    mix(static_cast<std::uint32_t>(key.ref.kind)); mix(key.ref.reserved);
    mix(key.ref.stableId.high); mix(key.ref.stableId.low);
    mix(key.ref.expectedProgramSemanticFingerprint); mix(key.ref.contentRevision);
    mix(key.ref.routeStableId.high); mix(key.ref.routeStableId.low); mix(key.ref.compatibilityFingerprint);
    mix(static_cast<std::uint32_t>(key.mode)); mix(key.budget.containers); mix(key.budget.schedules);
    mix(key.budget.materializedPath); mix(key.budget.frontierDepth);
    mix(static_cast<std::uint32_t>(key.budget.domain));
    return value;
}

class MemoWorkspace {
public:
    MemoWorkspace(MemoEntry* entries, std::size_t capacity) noexcept
        : entries_(entries), capacity_(entries != nullptr && capacity <= SIZE_MAX / sizeof(MemoEntry) ? capacity : 0) {}
    void Clear() noexcept {
        used_ = 0;
        head_ = tail_ = SIZE_MAX;
        for (std::size_t i = 0; i < capacity_; ++i) entries_[i].state = MemoState::Empty;
    }
    MemoEntry* FindOrInsert(const MemoKey& key, bool& inserted) noexcept {
        inserted = false;
        if (capacity_ == 0) return nullptr;
        auto slot = static_cast<std::size_t>(Hash(key) % capacity_);
        for (std::size_t probe = 0; probe < capacity_; ++probe) {
            auto& entry = entries_[slot];
            if (entry.state == MemoState::Empty) {
                entry.key = key;
                entry.result = {};
                entry.next = SIZE_MAX;
                entry.state = MemoState::Visiting;
                if (tail_ == SIZE_MAX) head_ = slot;
                else entries_[tail_].next = slot;
                tail_ = slot;
                ++used_;
                highWater_ = std::max(highWater_, used_);
                inserted = true;
                return &entry;
            }
            if (entry.key == key) return &entry;
            slot = slot + 1u == capacity_ ? 0 : slot + 1u;
        }
        return nullptr;
    }
    MemoEntry* First() noexcept { return head_ == SIZE_MAX ? nullptr : &entries_[head_]; }
    MemoEntry* Next(const MemoEntry& entry) noexcept {
        return entry.next == SIZE_MAX ? nullptr : &entries_[entry.next];
    }
    std::size_t Used() const noexcept { return used_; }
    std::size_t HighWater() const noexcept { return highWater_; }
    std::size_t CapacityBytes() const noexcept { return capacity_ * sizeof(MemoEntry); }
private:
    MemoEntry* entries_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t used_ = 0;
    std::size_t highWater_ = 0;
    std::size_t head_ = SIZE_MAX;
    std::size_t tail_ = SIZE_MAX;
};

inline bool KnownMode(ContainerModeV1 mode) noexcept {
    switch (mode) {
    case ContainerModeV1::Random:
    case ContainerModeV1::Sequence:
    case ContainerModeV1::RandomQueue:
    case ContainerModeV1::SequenceQueue:
    case ContainerModeV1::VariableControl: return true;
    }
    return false;
}

template<class Content>
class Counter {
public:
    Counter(const Content& content, Limits limits, MemoWorkspace& memo) noexcept
        : content_(content), limits_(limits), memo_(memo) {}
    Result Count(const PlayableRefV1& root, AuthorityStamp expected) noexcept {
        if (limits_.currentScheduleDepth == 0 || limits_.currentScheduleDepth > kPathCapacity ||
            limits_.directFutureScheduleDepth == 0 || limits_.directFutureScheduleDepth > kPathCapacity ||
            limits_.propertyPrefixCount > kPathCapacity || limits_.seedPrefixCount > kPathCapacity)
            return Failure(Status::InvalidInput);
        if (expected.generation == 0 || !(content_.Authority() == expected))
            return Failure(Status::AuthorityMismatch);
        memo_.Clear();
        auto result = Audit(root);
        if (!(content_.Authority() == expected)) return Failure(Status::AuthorityMismatch);
        if (!result) return result;
        memo_.Clear();
        result = Visit(root, Mode::Frontier, {kPathCapacity, limits_.currentScheduleDepth,
            kPathCapacity - limits_.propertyPrefixCount, 0, Domain::Current});
        if (result && result.profile.voice.path != 0) {
            const auto* property = content_.Property(root);
            if (property != nullptr) {
                const bool structural = property->structuralAncestorCount != 0;
                const auto count = structural ? property->structuralAncestorCount : property->organizationLayerAncestorCount;
                result.profile.voice.AddPathPrefix(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    if ((structural && property->structuralAncestors == nullptr) ||
                        (!structural && property->organizationLayerAncestors == nullptr))
                        return Failure(Status::InvalidInput);
                    const auto id = structural ? property->structuralAncestors[i].stableId
                        : property->organizationLayerAncestors[i].layerStableId;
                    result.profile.voice.bindings = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                        kMaxVoiceBindingInstanceSlots,
                        static_cast<std::uint64_t>(result.profile.voice.bindings) + content_.BindingCount(id)));
                }
            }
        }
        if (!(content_.Authority() == expected)) return Failure(Status::AuthorityMismatch);
        return result;
    }
private:
    Result EnqueueAudit(const PlayableRefV1& ref) noexcept {
        bool inserted = false;
        return memo_.FindOrInsert({ref, Mode::Audit, {}}, inserted) == nullptr
            ? Failure(Status::WorkspaceExhausted) : Result{};
    }
    Result Audit(const PlayableRefV1& root) noexcept {
        auto result = EnqueueAudit(root);
        if (!result) return result;
        // The insertion chain is a caller-backed work list, not a recursive
        // validation stack. Cycles are visited once and counted with budgets.
        for (auto* entry = memo_.First(); entry != nullptr; entry = memo_.Next(*entry)) {
            const auto ref = entry->key.ref;
            switch (ref.kind) {
            case PlayableKindV1::Sound: break;
            case PlayableKindV1::Track: {
                const auto* track = content_.Track(ref);
                if (track == nullptr) return Failure(Status::MissingContent);
                if (track->placementCount != 0 && track->placements == nullptr) return Failure(Status::InvalidInput);
                for (std::uint32_t i = 0; i < track->placementCount; ++i) {
                    const auto child = track->placements[i].content;
                    if (child.kind != PlayableKindV1::Sound && child.kind != PlayableKindV1::Piece &&
                        child.kind != PlayableKindV1::Container) return Failure(Status::UnsupportedKind);
                    result = EnqueueAudit(child);
                    if (!result) return result;
                }
                break;
            }
            case PlayableKindV1::Piece: {
                const auto* piece = content_.Piece(ref);
                if (piece == nullptr) return Failure(Status::MissingContent);
                if (piece->trackRefCount != 0 && piece->trackRefs == nullptr) return Failure(Status::InvalidInput);
                for (std::uint32_t i = 0; i < piece->trackRefCount; ++i) {
                    const auto& child = piece->trackRefs[i];
                    if (child.trackDefinition.kind != PlayableKindV1::Track) return Failure(Status::UnsupportedKind);
                    const auto* track = content_.Track(child.trackDefinition);
                    if (track == nullptr) return Failure(Status::MissingContent);
                    if (track->scheduleSemanticFingerprint != child.trackScheduleSemanticFingerprint)
                        return Failure(Status::FingerprintMismatch);
                    result = EnqueueAudit(child.trackDefinition);
                    if (!result) return result;
                }
                break;
            }
            case PlayableKindV1::Container: {
                const auto* program = content_.Container(ref);
                if (program == nullptr) return Failure(Status::MissingContent);
                if (!KnownMode(program->mode)) return Failure(Status::UnsupportedMode);
                if ((program->mode == ContainerModeV1::RandomQueue || program->mode == ContainerModeV1::SequenceQueue) &&
                    program->queueCycle != QueueCycleV1::Once && program->queueCycle != QueueCycleV1::Loop)
                    return Failure(Status::UnsupportedMode);
                if ((program->childCount != 0 && program->children == nullptr) ||
                    (program->discreteRowCount != 0 && program->discreteRows == nullptr) ||
                    (program->continuousRowCount != 0 && program->continuousRows == nullptr))
                    return Failure(Status::InvalidInput);
                for (std::uint32_t i = 0; i < program->childCount; ++i) {
                    result = EnqueueAudit(program->children[i].playable);
                    if (!result) return result;
                }
                if (program->mode == ContainerModeV1::VariableControl) {
                    for (std::uint32_t i = 0; i < program->discreteRowCount; ++i)
                        if (Child(*program, program->discreteRows[i].childStableOrdinal) == nullptr)
                            return Failure(Status::MissingContent);
                    for (std::uint32_t i = 0; i < program->continuousRowCount; ++i)
                        if (Child(*program, program->continuousRows[i].childStableOrdinal) == nullptr)
                            return Failure(Status::MissingContent);
                }
                break;
            }
            default: return Failure(Status::UnsupportedKind);
            }
            entry->state = MemoState::Ready;
        }
        return {};
    }

    Result Visit(const PlayableRefV1& ref, Mode mode, Budget budget) noexcept {
        bool inserted = false;
        auto* entry = memo_.FindOrInsert({ref, mode, budget}, inserted);
        if (entry == nullptr) return Failure(Status::WorkspaceExhausted);
        if (!inserted) return entry->state == MemoState::Visiting
            ? Failure(Status::InvalidInput) : entry->result;
        entry->result = Compute(ref, mode, budget);
        if (entry->result && ref.kind == PlayableKindV1::Container)
            entry->result.profile.referenceRows = SaturatingAdd(entry->result.profile.referenceRows,
                entry->result.profile.current, UINT32_MAX, entry->result.flags);
        if (entry->result && entry->result.profile.voice.path != 0) {
            auto& voice = entry->result.profile.voice;
            if (ref.kind != PlayableKindV1::Sound)
                voice.AddPathPrefix(ref.kind == PlayableKindV1::Piece ? 1u : 2u);
            voice.bindings = static_cast<std::uint32_t>(std::min<std::uint64_t>(kMaxVoiceBindingInstanceSlots,
                static_cast<std::uint64_t>(voice.bindings) + content_.BindingCount(ref.stableId)));
        }
        entry->state = MemoState::Ready;
        return entry->result;
    }

    Result Compute(const PlayableRefV1& ref, Mode mode, Budget budget) noexcept {
        if (ref.kind == PlayableKindV1::Sound) {
            Result result{};
            result.profile.current = 1;
            result.profile.referenceRows = 1;
            result.profile.voice.path = 1;
            const auto* property = content_.Property(ref);
            if (property != nullptr) {
                result.profile.voice.sends = property->payload.sendCount;
                result.profile.voice.effects = property->payload.effectNodeCount;
                if (property->payload.effectNodeCount != 0 && property->payload.effectNodes == nullptr)
                    return Failure(Status::InvalidInput);
                for (std::uint32_t index = 0; index < std::min(property->payload.effectNodeCount,
                        kTimelineMaxVoiceEffects); ++index) {
                    for (std::uint32_t type = 0; type < kEffectRegistryRowCount; ++type)
                        if (kEffectRegistry[type].effectType == property->payload.effectNodes[index].effectType)
                            ++result.profile.voice.effectInstances[type];
                }
            }
            return result;
        }
        if (ref.kind == PlayableKindV1::Track || ref.kind == PlayableKindV1::Piece) return Schedule(ref, budget);
        if (ref.kind != PlayableKindV1::Container) return Failure(Status::UnsupportedKind);
        const auto* program = content_.Container(ref);
        if (program == nullptr) return Failure(Status::MissingContent);
        if (!KnownMode(program->mode)) return Failure(Status::UnsupportedMode);
        if (mode == Mode::StaticSchedule) return Omitted(ExistingDynamicPath);
        if (budget.containers == 0) return Omitted(OmittedDepthBranch);
        if (budget.materializedPath == 0) return Omitted(OmittedPathBranch);
        if (mode == Mode::Frontier &&
            (program->mode == ContainerModeV1::Random || program->mode == ContainerModeV1::Sequence) &&
            budget.frontierDepth > kPathCapacity - limits_.seedPrefixCount)
            return Omitted(OmittedSeedBranch);
        --budget.containers;
        --budget.materializedPath;
        ++budget.frontierDepth;
        if (mode == Mode::DirectNested) {
            if (program->childCount != 1u) return Omitted(ExistingDynamicPath);
            return Visit(program->children[0].playable, Mode::DirectNested, budget);
        }
        switch (program->mode) {
        case ContainerModeV1::Random:
        case ContainerModeV1::Sequence: return Choices(*program, budget);
        case ContainerModeV1::VariableControl: return Variable(*program, budget);
        case ContainerModeV1::RandomQueue:
        case ContainerModeV1::SequenceQueue: return Queue(*program, budget);
        }
        return Failure(Status::UnsupportedMode);
    }

    Result Schedule(const PlayableRefV1& ref, Budget budget) noexcept {
        if (budget.schedules == 0) return Omitted(UnprunedSchedule | OmittedDepthBranch);
        --budget.schedules;
        Result result{};
        result.flags = UnprunedSchedule;
        if (ref.kind == PlayableKindV1::Piece) {
            const auto* piece = content_.Piece(ref);
            if (piece == nullptr) return Failure(Status::MissingContent);
            for (std::uint32_t i = 0; i < piece->trackRefCount; ++i) {
                result = Concurrent(result, Visit(piece->trackRefs[i].trackDefinition, Mode::StaticSchedule, budget), limits_);
                if (!result) return result;
            }
            return result;
        }
        const auto* track = content_.Track(ref);
        if (track == nullptr) return Failure(Status::MissingContent);
        if (track->placementCount != 0 && budget.materializedPath == 0)
            return Omitted(UnprunedSchedule | OmittedPathBranch);
        if (track->placementCount != 0) --budget.materializedPath;
        for (std::uint32_t i = 0; i < track->placementCount; ++i) {
            const auto child = track->placements[i].content;
            if (child.kind == PlayableKindV1::Container) result.flags |= ExistingDynamicPath;
            else {
                result = Concurrent(result, Visit(child, Mode::StaticSchedule, budget), limits_);
                if (!result) return result;
            }
        }
        return result;
    }

    const ContainerChildProgramViewV2* Child(const ContainerProgramViewV3& program,
        std::uint32_t ordinal) const noexcept {
        for (std::uint32_t i = 0; i < program.childCount; ++i)
            if (program.children[i].stableChildOrdinal == ordinal) return &program.children[i];
        return nullptr;
    }

    Result Choices(const ContainerProgramViewV3& program, Budget budget) noexcept {
        Result result{};
        for (std::uint32_t i = 0; i < program.childCount; ++i) {
            result = Alternative(result, Visit(program.children[i].playable, Mode::Frontier, budget));
            if (!result) return result;
        }
        return result;
    }

    Result Variable(const ContainerProgramViewV3& program, Budget budget) noexcept {
        Result discrete{};
        for (std::uint32_t i = 0; i < program.discreteRowCount; ++i) {
            const auto* child = Child(program, program.discreteRows[i].childStableOrdinal);
            if (child == nullptr) return Failure(Status::MissingContent);
            discrete = Alternative(discrete, Visit(child->playable, Mode::Frontier, budget));
            if (!discrete) return discrete;
        }
        Result continuous{};
        for (std::uint32_t i = 0; i < program.continuousRowCount; ++i) {
            const auto ordinal = program.continuousRows[i].childStableOrdinal;
            bool first = true;
            for (std::uint32_t j = 0; j < i; ++j)
                if (program.continuousRows[j].childStableOrdinal == ordinal) first = false;
            if (!first) continue;
            const auto* child = Child(program, ordinal);
            if (child == nullptr) return Failure(Status::MissingContent);
            continuous = Concurrent(continuous, Visit(child->playable, Mode::Frontier, budget), limits_);
            if (!continuous) return continuous;
        }
        auto result = Concurrent(discrete, continuous, limits_);
        result.profile.rootContinuousMembers = continuous.profile.current;
        for (std::uint32_t t = 1; t <= continuous.profile.current; ++t)
            result.profile.controllerWeights[t] = SaturatingAdd(result.profile.controllerWeights[t], 1u,
                kControlSideCapacity, result.flags);
        result.profile.controlSides = SaturatingAdd(result.profile.controlSides, 2u,
            kControlSideCapacity, result.flags);
        return result;
    }

    bool CanWrap(const ContainerProgramViewV3& program) const noexcept {
        return program.queueCycle == QueueCycleV1::Loop && (program.queueCycleCount == 0 || program.queueCycleCount > 1u);
    }

    Result DirectFuture(const ContainerChildProgramViewV2& child) noexcept {
        const Budget fresh{kPathCapacity, limits_.directFutureScheduleDepth,
            kPathCapacity - 1u, 0, Domain::DirectFuture};
        const auto kind = child.playable.kind;
        const auto hint = child.timingHint.kind;
        if (kind == PlayableKindV1::Sound &&
            (hint == ChildTimingHintKindV2::Unknown || hint == ChildTimingHintKindV2::InlineSound))
            return Visit(child.playable, Mode::StaticSchedule, fresh);
        if ((kind == PlayableKindV1::Track || kind == PlayableKindV1::Piece) && hint == ChildTimingHintKindV2::ScheduleReference) {
            const auto& key = child.timingHint.payload.schedule;
            if (key.kind != kind || key.stableId != child.playable.stableId) return Failure(Status::FingerprintMismatch);
            const auto* track = kind == PlayableKindV1::Track ? content_.Track(child.playable) : nullptr;
            const auto* piece = kind == PlayableKindV1::Piece ? content_.Piece(child.playable) : nullptr;
            if (track == nullptr && piece == nullptr) return Failure(Status::MissingContent);
            const auto fingerprint = track != nullptr ? track->scheduleSemanticFingerprint : piece->scheduleSemanticFingerprint;
            if (key.scheduleSemanticFingerprint != fingerprint) return Failure(Status::FingerprintMismatch);
            return Visit(child.playable, Mode::StaticSchedule, fresh);
        }
        if (kind == PlayableKindV1::Container && hint == ChildTimingHintKindV2::Unknown) {
            if constexpr (Content::kDirectNestedContainer) return Visit(child.playable, Mode::DirectNested, fresh);
            else return Omitted(ExistingDirectRefusal);
        }
        if (kind != PlayableKindV1::Sound && kind != PlayableKindV1::Track &&
            kind != PlayableKindV1::Piece && kind != PlayableKindV1::Container) return Failure(Status::UnsupportedKind);
        return Omitted(ExistingDirectRefusal);
    }

    Result QueueResetShape(const ContainerChildProgramViewV2& child) noexcept {
        const auto depth = content_.QueueResetScheduleDepth();
        if (depth == 0 || depth > kPathCapacity) return {};
        const Budget fresh{kPathCapacity, depth, kPathCapacity - 1u, 0, Domain::DirectFuture};
        if (child.playable.kind == PlayableKindV1::Container) {
            if constexpr (Content::kDirectNestedContainer) return Visit(child.playable, Mode::DirectNested, fresh);
            else return {};
        }
        return Visit(child.playable, Mode::StaticSchedule, fresh);
    }

    Result Queue(const ContainerProgramViewV3& program, Budget budget) noexcept {
        if (program.queueCycle != QueueCycleV1::Once && program.queueCycle != QueueCycleV1::Loop)
            return Failure(Status::UnsupportedMode);
        Result result{};
        std::uint32_t currentCapacity = 0;
        std::uint32_t targetCapacity = 0;
        for (std::uint32_t i = 0; i < program.childCount; ++i) {
            auto current = Visit(program.children[i].playable, Mode::Frontier, budget);
            if (!current) return current;
            currentCapacity = std::max(currentCapacity, current.profile.current);
            current.profile.Q = SaturatingAdd(current.profile.Q, 1, limits_.queueRoleCapacity, current.flags);
            std::uint32_t capacity = 0;
            std::uint32_t futureReferences = 0;
            for (std::uint32_t j = 0; j < program.childCount; ++j) {
                const bool possible = program.mode == ContainerModeV1::SequenceQueue
                    ? j > i || (j == 0 && CanWrap(program)) : j != i || CanWrap(program);
                if (!possible) continue;
                const auto next = DirectFuture(program.children[j]);
                if (!next) return next;
                capacity = std::max(capacity, next.profile.current);
                futureReferences = std::max(futureReferences, SaturatingAdd(next.profile.referenceRows,
                    next.profile.current, UINT32_MAX, current.flags));
                current.flags |= next.flags;
            }
            targetCapacity = std::max(targetCapacity, capacity);
            AddFuture(current, capacity, limits_);
            current.profile.referenceRows = SaturatingAdd(current.profile.referenceRows,
                futureReferences, UINT32_MAX, current.flags);
            result = Alternative(result, current);
            if (!result) return result;
        }
        for (std::uint32_t i = 0; i < program.childCount; ++i) {
            const auto reset = QueueResetShape(program.children[i]);
            if (reset.status == Status::WorkspaceExhausted) return reset;
            if (reset) targetCapacity = std::max(targetCapacity, reset.profile.current);
        }
        currentCapacity = std::max(currentCapacity, targetCapacity);
        const auto programs = currentCapacity + targetCapacity;
        result.profile.queue.Include({programs, 2u * programs});
        return result;
    }

    const Content& content_;
    Limits limits_{};
    MemoWorkspace& memo_;
};

} // namespace harken::native_backing
#endif
