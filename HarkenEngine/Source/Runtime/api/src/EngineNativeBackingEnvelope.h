#ifndef HARKEN_ENGINE_NATIVE_BACKING_ENVELOPE_H
#define HARKEN_ENGINE_NATIVE_BACKING_ENVELOPE_H

// Count under the retained content authority before retiring original sources.
// This helper allocates nothing and owns no native rights.
#include <harken/core/PlayablePropertyCut.h>
#include <harken/core/PackPlaybackView.h>
#include <harken/core/CurrentSelection.h>
#include <harken/core/EngineConfig.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace harken::native_backing {

inline constexpr std::uint32_t kLeafCapacity = kMaxKnownScheduleLeaves;
inline constexpr std::uint32_t kPathCapacity = kSelectionPathNodeCapacity;
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

struct Profile {
    std::uint32_t current = 0;
    std::array<std::uint32_t, kLeafCapacity + 1u> H{};
};

struct Result {
    Status status = Status::Ok;
    std::uint32_t flags = 0;
    Profile profile{};
    explicit operator bool() const noexcept { return status == Status::Ok; }
};

struct Limits {
    std::uint32_t immediateFutureSides = 0;
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
    explicit WorldContent(const PlayableWorldViewV1& view) noexcept : view_(view) {}
    AuthorityStamp Authority() const noexcept {
        return {view_.generation, view_.identity.runtimeSemanticFingerprint};
    }
    static constexpr bool kDirectNestedContainer = true;
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
};

class PackContent {
public:
    explicit PackContent(const PackPlaybackView& view) noexcept : view_(view) {}
    AuthorityStamp Authority() const noexcept {
        return {view_.CompositeGeneration(), view_.WorldSemanticFingerprint()};
    }
    static constexpr bool kDirectNestedContainer = false;
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
    for (std::uint32_t t = 1; t <= kLeafCapacity; ++t)
        a.profile.H[t] = SaturatingAdd(a.profile.H[t], b.profile.H[t], limits.immediateFutureSides, a.flags);
    return a;
}

inline Result Alternative(Result a, const Result& b) noexcept {
    if (!a) return a;
    if (!b) return b;
    a.flags |= b.flags;
    a.profile.current = std::max(a.profile.current, b.profile.current);
    for (std::uint32_t t = 1; t <= kLeafCapacity; ++t) a.profile.H[t] = std::max(a.profile.H[t], b.profile.H[t]);
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
bool EmitDescendingFutureCapacities(const Profile& profile, Emit&& emit) noexcept {
    for (std::uint32_t t = 1; t < kLeafCapacity; ++t)
        if (profile.H[t] < profile.H[t + 1u]) return false;
    std::uint32_t next = 0;
    for (std::uint32_t t = kLeafCapacity; t != 0; --t) {
        const auto count = profile.H[t] - next;
        for (std::uint32_t i = 0; i < count; ++i) if (!emit(t)) return false;
        next = profile.H[t];
    }
    return true;
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
        entry->state = MemoState::Ready;
        return entry->result;
    }

    Result Compute(const PlayableRefV1& ref, Mode mode, Budget budget) noexcept {
        if (ref.kind == PlayableKindV1::Sound) {
            Result result{};
            result.profile.current = 1;
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
        return Concurrent(discrete, continuous, limits_);
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

    Result Queue(const ContainerProgramViewV3& program, Budget budget) noexcept {
        if (program.queueCycle != QueueCycleV1::Once && program.queueCycle != QueueCycleV1::Loop)
            return Failure(Status::UnsupportedMode);
        Result result{};
        for (std::uint32_t i = 0; i < program.childCount; ++i) {
            auto current = Visit(program.children[i].playable, Mode::Frontier, budget);
            if (!current) return current;
            std::uint32_t capacity = 0;
            for (std::uint32_t j = 0; j < program.childCount; ++j) {
                const bool possible = program.mode == ContainerModeV1::SequenceQueue
                    ? j > i || (j == 0 && CanWrap(program)) : j != i || CanWrap(program);
                if (!possible) continue;
                const auto next = DirectFuture(program.children[j]);
                if (!next) return next;
                capacity = std::max(capacity, next.profile.current);
                current.flags |= next.flags;
            }
            AddFuture(current, capacity, limits_);
            result = Alternative(result, current);
            if (!result) return result;
        }
        return result;
    }

    const Content& content_;
    Limits limits_{};
    MemoWorkspace& memo_;
};

} // namespace harken::native_backing
#endif
