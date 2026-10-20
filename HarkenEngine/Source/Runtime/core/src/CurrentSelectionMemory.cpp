#include "harken/core/CurrentSelection.h"

#include <algorithm>
#include <limits>

namespace harken {
namespace {

bool Later(std::uint64_t ticket, std::uint32_t eventIndex,
           std::uint64_t priorTicket, std::uint32_t priorEventIndex) noexcept {
    return ticket > priorTicket ||
           (ticket == priorTicket && eventIndex > priorEventIndex);
}

bool AppliesBefore(std::uint64_t executeFrame, std::uint64_t ticket,
                   std::uint32_t eventIndex, std::uint64_t laterFrame,
                   std::uint64_t laterTicket,
                   std::uint32_t laterEventIndex) noexcept {
    if (executeFrame != laterFrame) {
        return executeFrame < laterFrame;
    }
    return Later(laterTicket, laterEventIndex, ticket, eventIndex);
}

SelectionMemoryStatusV1 MapStatus(SelectionStatusV1 status) noexcept {
    switch (status) {
    case SelectionStatusV1::Ok:
        return SelectionMemoryStatusV1::Ok;
    case SelectionStatusV1::SequenceComplete:
        return SelectionMemoryStatusV1::SequenceComplete;
    case SelectionStatusV1::InvalidInput:
    case SelectionStatusV1::InvalidAvoid:
    case SelectionStatusV1::WeightOverflow:
    case SelectionStatusV1::OutputCapacity:
        return SelectionMemoryStatusV1::InvalidInput;
    }
    return SelectionMemoryStatusV1::InvalidInput;
}

}

std::uint64_t SelectionMemoryTableV1::EmitterStorageBytes(
    const SelectionMemoryConfigV1& config) noexcept {
    const std::uint64_t rowBytes = sizeof(Row) +
        (static_cast<std::uint64_t>(config.maxAvoidLastN) + config.maxChildren) *
            sizeof(std::uint32_t);
    return config.maxEmitterRowsPerEmitter * rowBytes;
}

bool SelectionMemoryTableV1::Reset(
    const SelectionMemoryConfigV1& config) noexcept {
    if (tableLifetime_ == UINT64_MAX || config.maxChildren == 0 ||
        config.maxChildren > kHardPlayableOccurrencePropertyCapacity ||
        config.maxAvoidLastN > config.maxChildren ||
        config.maxPathNodes > kSelectionPathNodeCapacity) {
        return false;
    }
    const std::uint64_t emitterRows =
        static_cast<std::uint64_t>(config.maxEmitters) *
        config.maxEmitterRowsPerEmitter;
    const std::uint64_t rowCapacity64 =
        static_cast<std::uint64_t>(config.maxGlobalRows) + emitterRows;
    if (rowCapacity64 > UINT32_MAX) return false;
    try {
        std::vector<Row> rows(static_cast<std::size_t>(rowCapacity64));
        std::vector<DeltaRow> deltas(config.maxPendingDeltasTotal);
        std::vector<BatchRow> batches(config.maxPendingDeltasTotal);
        std::vector<std::uint8_t> replacementAffectedRows(static_cast<std::size_t>(rowCapacity64), 0);
        std::vector<BatchDependencyEdge> replacementBatchEdges(config.maxPendingDeltasTotal);
        std::vector<std::uint8_t> replacementBatchReachable(config.maxPendingDeltasTotal, 0);
        std::vector<std::uint32_t> pendingCommitHeads(static_cast<std::size_t>(rowCapacity64), UINT32_MAX);
        std::vector<SelectionChildV1> childScratch;
        std::vector<std::uint32_t> remainingScratch;
        MemoryState pendingScratch[2];
        MemoryState replacementScratch[2];
        for (auto& state : pendingScratch) {
            state.recent.reserve(config.maxAvoidLastN);
            state.permutation.reserve(config.maxChildren);
        }
        for (auto& state : replacementScratch) {
            state.recent.reserve(config.maxAvoidLastN);
            state.permutation.reserve(config.maxChildren);
        }
        childScratch.reserve(config.maxChildren);
        remainingScratch.resize(config.maxChildren);
        for (Row& row : rows) {
            row.state.recent.reserve(config.maxAvoidLastN);
            row.state.permutation.reserve(config.maxChildren);
        }
        for (DeltaRow& delta : deltas) {
            delta.path.reserve(config.maxPathNodes);
            delta.overlay.recent.reserve(config.maxAvoidLastN);
            delta.overlay.permutation.reserve(config.maxChildren);
            delta.rollback.recent.reserve(config.maxAvoidLastN);
            delta.rollback.permutation.reserve(config.maxChildren);
        }
        rows_.swap(rows);
        deltas_.swap(deltas);
        batches_.swap(batches);
        replacementAffectedRows_.swap(replacementAffectedRows);
        replacementBatchEdges_.swap(replacementBatchEdges);
        replacementBatchReachable_.swap(replacementBatchReachable);
        pendingCommitHeads_.swap(pendingCommitHeads);
        for (std::uint32_t index = 0; index < 2; ++index) {
            std::swap(pendingScratch_[index], pendingScratch[index]);
            std::swap(replacementScratch_[index], replacementScratch[index]);
        }
        childScratch_.swap(childScratch);
        remainingScratch_.swap(remainingScratch);
    } catch (...) {
        return false;
    }
    config_ = config;
    ++tableLifetime_;
    mutationEpoch_ = 1;
    rowCount_ = 0;
    occupiedRowCount_ = 0;
    pendingCount_ = 0;
    quarantinedDeltaCount_ = 0;
    return true;
}

bool SelectionMemoryTableV1::SameKey(const SelectionMemoryKeyV1& a,
                                     const SelectionMemoryKeyV1& b) noexcept {
    return a.world == b.world &&
           a.containerStableId == b.containerStableId &&
           a.programSemanticFingerprint == b.programSemanticFingerprint &&
           a.memoryScope == b.memoryScope && a.emitterSlot == b.emitterSlot &&
           a.emitterGeneration == b.emitterGeneration;
}

bool SelectionMemoryTableV1::ValidKey(
    const SelectionMemoryKeyV1& key) const noexcept {
    if (key.world == 0 || key.containerStableId == VnObjectId{} ||
        key.programSemanticFingerprint == 0) {
        return false;
    }
    if (key.memoryScope == MemoryScopeV1::Global) {
        return key.emitterSlot == UINT32_MAX && key.emitterGeneration == 0;
    }
    return key.memoryScope == MemoryScopeV1::Emitter &&
           key.emitterSlot < config_.maxEmitters &&
           key.emitterGeneration != 0;
}

std::uint32_t SelectionMemoryTableV1::FindRow(
    const SelectionMemoryKeyV1& key) const noexcept {
    for (std::uint32_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].used && SameKey(rows_[i].key, key)) return i;
    }
    return UINT32_MAX;
}

std::uint32_t SelectionMemoryTableV1::FindFreeRow(
    const SelectionMemoryKeyV1& key) const noexcept {
    std::uint32_t scopedCount = 0;
    for (const Row& row : rows_) {
        if (!row.used) continue;
        if (key.memoryScope == MemoryScopeV1::Global) {
            scopedCount += row.key.memoryScope == MemoryScopeV1::Global ? 1u : 0u;
        } else {
            scopedCount += row.key.memoryScope == MemoryScopeV1::Emitter &&
                                   row.key.emitterSlot == key.emitterSlot
                               ? 1u
                               : 0u;
        }
    }
    const std::uint32_t limit =
        key.memoryScope == MemoryScopeV1::Global
            ? config_.maxGlobalRows
            : config_.maxEmitterRowsPerEmitter;
    if (scopedCount >= limit) return UINT32_MAX;
    for (std::uint32_t i = 0; i < rows_.size(); ++i) {
        if (!rows_[i].used) return i;
    }
    return UINT32_MAX;
}

std::uint32_t SelectionMemoryTableV1::FindFreeDelta() const noexcept {
    for (std::uint32_t i = 0; i < deltas_.size(); ++i) {
        if (!deltas_[i].used && !deltas_[i].prepared && !deltas_[i].replacementCandidate &&
            !deltas_[i].quarantined) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool SelectionMemoryTableV1::PendingOrderValid(
    const Row& row, std::uint64_t ticket, std::uint32_t eventIndex) const noexcept {
    if (ticket == 0) return false;
    if (row.tail == UINT32_MAX) return true;
    const DeltaRow& tail = deltas_[row.tail];
    return Later(ticket, eventIndex, tail.ticket, tail.eventIndex);
}

bool SelectionMemoryTableV1::CopyState(const MemoryState& source,
                                       MemoryState& target) noexcept {
    if (source.recent.size() > target.recent.capacity() ||
        source.permutation.size() > target.permutation.capacity()) {
        return false;
    }
    target.resetEpoch = source.resetEpoch;
    target.selectionOrdinal = source.selectionOrdinal;
    target.sequenceCursor = source.sequenceCursor;
    target.sequenceStopped = source.sequenceStopped;
    target.rotationCursor = source.rotationCursor;
    target.hasScheduledFrontier = source.hasScheduledFrontier;
    target.scheduledFrame = source.scheduledFrame;
    target.scheduledTicket = source.scheduledTicket;
    target.scheduledEvent = source.scheduledEvent;
    target.hasNativeScheduledFrontier = source.hasNativeScheduledFrontier;
    target.nativeScheduledFrame = source.nativeScheduledFrame;
    target.nativeScheduledTicket = source.nativeScheduledTicket;
    target.nativeScheduledEvent = source.nativeScheduledEvent;
    target.hasImmediateFrontier = source.hasImmediateFrontier;
    target.immediateTicket = source.immediateTicket;
    target.immediateEvent = source.immediateEvent;
    target.hasPrivateScheduledFrontier = source.hasPrivateScheduledFrontier;
    target.privateScheduledFrame = source.privateScheduledFrame;
    target.privateScheduledTicket = source.privateScheduledTicket;
    target.privateScheduledEvent = source.privateScheduledEvent;
    target.hasPrivateImmediateFrontier = source.hasPrivateImmediateFrontier;
    target.privateImmediateTicket = source.privateImmediateTicket;
    target.privateImmediateEvent = source.privateImmediateEvent;
    target.recent.assign(source.recent.begin(), source.recent.end());
    target.permutation.assign(source.permutation.begin(),
                              source.permutation.end());
    return true;
}

void SelectionMemoryTableV1::InitializeState(const Row& row,
                                             MemoryState& state) noexcept {
    state.resetEpoch = 0;
    state.selectionOrdinal = 0;
    state.sequenceCursor = row.initialSequenceCursor;
    state.sequenceStopped = false;
    state.rotationCursor = 0;
    state.recent.clear();
    state.permutation.clear();
    ClearResetFrontiers(state);
}

void SelectionMemoryTableV1::AppendRecent(MemoryState& state,
                                          std::uint32_t child,
                                          std::uint32_t avoidLastN) noexcept {
    if (avoidLastN == 0) {
        state.recent.clear();
        return;
    }
    if (state.recent.size() == avoidLastN) {
        for (std::size_t i = 1; i < state.recent.size(); ++i) {
            state.recent[i - 1] = state.recent[i];
        }
        state.recent.back() = child;
    } else {
        state.recent.push_back(child);
    }
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::ComputePlay(
    const Row& row, const SelectionPlayRequestV1& request,
    const MemoryState& before, MemoryState& after,
    std::uint32_t* selectedChildOrdinal,
    SelectionSeedProbeCopyV1* seedProbe) noexcept {
    if (seedProbe != nullptr) {
        *seedProbe = {};
    }
    if (request.program == nullptr || selectedChildOrdinal == nullptr ||
        request.program->containerStableId != row.key.containerStableId ||
        request.program->programSemanticFingerprint !=
            row.key.programSemanticFingerprint ||
        request.program->mode != row.mode ||
        request.program->randomDiscipline != row.randomDiscipline ||
        request.program->sequenceEndPolicy != row.sequenceEndPolicy ||
        request.program->avoidLastN != row.avoidLastN ||
        request.program->childCount == 0 ||
        request.program->childCount > config_.maxChildren ||
        request.program->children == nullptr ||
        request.worldSemanticFingerprint == 0 || request.rootStableId == VnObjectId{} ||
        request.acceptedPostOrdinal == 0 || request.ticket == 0 ||
        request.pathCount > config_.maxPathNodes ||
        (request.pathCount != 0 && request.path == nullptr) ||
        before.selectionOrdinal == UINT64_MAX ||
        !CopyState(before, after)) {
        return before.selectionOrdinal == UINT64_MAX
                   ? SelectionMemoryStatusV1::RuntimeRecreateRequired
                   : SelectionMemoryStatusV1::InvalidInput;
    }
    childScratch_.clear();
    for (std::uint32_t i = 0; i < request.program->childCount; ++i) {
        const ContainerChildProgramViewV2& child =
            request.program->children[i];
        childScratch_.push_back({child.stableChildOrdinal,
                                 child.authoredStableOrder,
                                 child.frequencyWeight,
                                 child.orderWeight});
    }
    const std::uint64_t nextOrdinal = before.selectionOrdinal + 1;
    SelectionSeedInputV1 seedInput{};
    seedInput.worldSemanticFingerprint = request.worldSemanticFingerprint;
    seedInput.programStableId = row.key.containerStableId;
    seedInput.programSemanticFingerprint = row.key.programSemanticFingerprint;
    seedInput.memoryScope = row.key.memoryScope;
    seedInput.emitterSlot = row.key.emitterSlot;
    seedInput.emitterGeneration = row.key.emitterGeneration;
    seedInput.resetEpoch = before.resetEpoch;
    seedInput.rootStableId = request.rootStableId;
    seedInput.callerSeed = request.callerSeed;
    seedInput.acceptedPostOrdinal = request.acceptedPostOrdinal;
    seedInput.pathCount = request.pathCount;
    seedInput.path = request.path;
    seedInput.occurrenceOrdinal = request.occurrenceOrdinal;
    seedInput.loopIteration = request.loopIteration;
    seedInput.selectionOrdinal = nextOrdinal;
    if (seedProbe != nullptr) {
        seedProbe->captured = true;
        seedProbe->derivedCallerSeed = seedInput.callerSeed;
        seedProbe->acceptedPostOrdinal = seedInput.acceptedPostOrdinal;
        seedProbe->selectionOrdinal = seedInput.selectionOrdinal;
        seedProbe->resetEpoch = seedInput.resetEpoch;
    }

    SelectionStatusV1 status = SelectionStatusV1::InvalidInput;
    if (row.mode == ContainerModeV1::Random &&
        row.randomDiscipline == RandomDisciplineV1::Draw) {
        seedInput.discipline = SelectionDisciplineV1::Draw;
        std::uint64_t seed = 0;
        if (!ComputeSelectionSeedV1(seedInput, &seed)) {
            if (seedProbe != nullptr) {
                *seedProbe = {};
            }
            return SelectionMemoryStatusV1::InvalidInput;
        }
        SplitMix64 stream(seed);
        status = SelectRandomDrawV1(
            childScratch_.data(),
            static_cast<std::uint32_t>(childScratch_.size()),
            before.recent.data(),
            static_cast<std::uint32_t>(before.recent.size()), &stream,
            selectedChildOrdinal);
    } else if (row.mode == ContainerModeV1::Random &&
               row.randomDiscipline == RandomDisciplineV1::Rotation) {
        seedInput.discipline = SelectionDisciplineV1::Rotation;
        if (after.rotationCursor >= after.permutation.size()) {
            std::uint64_t seed = 0;
            if (!ComputeSelectionSeedV1(seedInput, &seed)) {
                if (seedProbe != nullptr) {
                    *seedProbe = {};
                }
                return SelectionMemoryStatusV1::InvalidInput;
            }
            SplitMix64 stream(seed);
            after.permutation.resize(childScratch_.size());
            status = BuildRandomRotationV1(
                childScratch_.data(),
                static_cast<std::uint32_t>(childScratch_.size()),
                before.recent.data(),
                static_cast<std::uint32_t>(before.recent.size()), &stream,
                remainingScratch_.data(),
                static_cast<std::uint32_t>(remainingScratch_.size()),
                after.permutation.data(),
                static_cast<std::uint32_t>(after.permutation.size()));
            if (status == SelectionStatusV1::Ok) after.rotationCursor = 0;
        } else {
            status = SelectionStatusV1::Ok;
        }
        if (status == SelectionStatusV1::Ok) {
            *selectedChildOrdinal =
                after.permutation[after.rotationCursor++];
        }
    } else if (row.mode == ContainerModeV1::Sequence) {
        seedInput.discipline = SelectionDisciplineV1::Sequence;
        SequenceSelectionV1 sequence{};
        status = SelectSequenceV1(
            childScratch_.data(),
            static_cast<std::uint32_t>(childScratch_.size()),
            before.sequenceCursor, before.sequenceStopped,
            row.sequenceEndPolicy, &sequence);
        if (status == SelectionStatusV1::Ok) {
            *selectedChildOrdinal = sequence.selectedChildOrdinal;
            after.sequenceCursor = sequence.nextCursor;
            after.sequenceStopped = sequence.stopped;
        }
    }
    if (status != SelectionStatusV1::Ok) {
        if (seedProbe != nullptr) {
            *seedProbe = {};
        }
        return MapStatus(status);
    }
    after.selectionOrdinal = nextOrdinal;
    AppendRecent(after, *selectedChildOrdinal, row.avoidLastN);
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::ComputeReset(
    const Row& row, const MemoryState& before, MemoryState& after) noexcept {
    if (before.resetEpoch == UINT64_MAX || !CopyState(before, after)) {
        return before.resetEpoch == UINT64_MAX
                   ? SelectionMemoryStatusV1::RuntimeRecreateRequired
                   : SelectionMemoryStatusV1::InvalidInput;
    }
    ++after.resetEpoch;
    after.selectionOrdinal = 0;
    after.sequenceCursor = row.initialSequenceCursor;
    after.sequenceStopped = false;
    after.rotationCursor = 0;
    after.recent.clear();
    after.permutation.clear();
    return SelectionMemoryStatusV1::Ok;
}

void SelectionMemoryTableV1::MarkScheduledFrontier(MemoryState& state, const DeltaRow& reset) noexcept {
    if (reset.pendingReset && reset.prepared) {
        if (reset.executeFrame == 0) {
            state.hasPrivateImmediateFrontier = true;
            state.privateImmediateTicket = reset.ticket;
            state.privateImmediateEvent = reset.eventIndex;
        } else {
            state.hasPrivateScheduledFrontier = true;
            state.privateScheduledFrame = reset.executeFrame;
            state.privateScheduledTicket = reset.ticket;
            state.privateScheduledEvent = reset.eventIndex;
        }
        return;
    }
    if (reset.pendingReset) {
        if (reset.executeFrame == 0) {
            state.hasImmediateFrontier = true;
            state.immediateTicket = reset.ticket;
            state.immediateEvent = reset.eventIndex;
        } else {
            state.hasNativeScheduledFrontier = true;
            state.nativeScheduledFrame = reset.executeFrame;
            state.nativeScheduledTicket = reset.ticket;
            state.nativeScheduledEvent = reset.eventIndex;
        }
        return;
    }
    state.hasScheduledFrontier = true;
    state.scheduledFrame = reset.executeFrame;
    state.scheduledTicket = reset.ticket;
    state.scheduledEvent = reset.eventIndex;
}

void SelectionMemoryTableV1::ClearResetFrontiers(MemoryState& state) noexcept {
    state.hasScheduledFrontier = false;
    state.scheduledFrame = state.scheduledTicket = 0;
    state.scheduledEvent = 0;
    state.hasNativeScheduledFrontier = false;
    state.nativeScheduledFrame = state.nativeScheduledTicket = 0;
    state.nativeScheduledEvent = 0;
    state.hasImmediateFrontier = false;
    state.immediateTicket = 0;
    state.immediateEvent = 0;
    state.hasPrivateScheduledFrontier = false;
    state.privateScheduledFrame = state.privateScheduledTicket = 0;
    state.privateScheduledEvent = 0;
    state.hasPrivateImmediateFrontier = false;
    state.privateImmediateTicket = 0;
    state.privateImmediateEvent = 0;
}

bool SelectionMemoryTableV1::ResetConsumed(const MemoryState& state, const DeltaRow& reset) noexcept {
    if (reset.pendingReset && reset.prepared) {
        if (reset.executeFrame == 0)
            return state.hasPrivateImmediateFrontier && !Later(reset.ticket, reset.eventIndex,
                state.privateImmediateTicket, state.privateImmediateEvent);
        return state.hasPrivateScheduledFrontier && !AppliesBefore(state.privateScheduledFrame,
            state.privateScheduledTicket, state.privateScheduledEvent,
            reset.executeFrame, reset.ticket, reset.eventIndex);
    }
    if (reset.pendingReset) {
        if (reset.executeFrame == 0)
            return state.hasImmediateFrontier && !Later(reset.ticket, reset.eventIndex,
                state.immediateTicket, state.immediateEvent);
        return state.hasNativeScheduledFrontier && !AppliesBefore(state.nativeScheduledFrame,
            state.nativeScheduledTicket, state.nativeScheduledEvent,
            reset.executeFrame, reset.ticket, reset.eventIndex);
    }
    return state.hasScheduledFrontier && !AppliesBefore(state.scheduledFrame,
        state.scheduledTicket, state.scheduledEvent, reset.executeFrame, reset.ticket, reset.eventIndex);
}

bool SelectionMemoryTableV1::ResetAppliesBefore(const DeltaRow& reset, std::uint64_t frame,
    std::uint64_t ticket, std::uint32_t eventIndex) noexcept {
    return reset.pendingReset && reset.executeFrame == 0
        ? Later(ticket, eventIndex, reset.ticket, reset.eventIndex)
        : AppliesBefore(reset.executeFrame, reset.ticket, reset.eventIndex, frame, ticket, eventIndex);
}

std::uint32_t SelectionMemoryTableV1::PrivateResetCount(const SelectionMemoryKeyV1& key) const noexcept {
    std::uint32_t count = 0;
    for (const auto& delta : deltas_)
        if (delta.prepared && delta.pendingReset && SameKey(delta.request.key, key)) ++count;
    return count;
}

bool SelectionMemoryTableV1::PrivateResetConflicts(const SelectionMemoryKeyV1& key,
    std::uint64_t ticket) const noexcept {
    for (const auto& delta : deltas_)
        if (delta.prepared && delta.pendingReset && SameKey(delta.request.key, key) &&
            (ticket == 0 || ticket != delta.ticket)) return true;
    return false;
}

bool SelectionMemoryTableV1::PendingResetHasHistory(const Row& row, const DeltaRow& reset) const noexcept {
    if (row.committed || row.scheduledCount != 0) return true;
    for (auto index = row.head; index != UINT32_MAX; index = deltas_[index].next)
        if (Later(reset.ticket, reset.eventIndex, deltas_[index].ticket, deltas_[index].eventIndex)) return true;
    for (auto index = row.preparedHead; index != UINT32_MAX; index = deltas_[index].next)
        if (Later(reset.ticket, reset.eventIndex, deltas_[index].ticket, deltas_[index].eventIndex)) return true;
    return false;
}

bool SelectionMemoryTableV1::PendingResetBlocked(const Row& row, const DeltaRow& reset) const noexcept {
    const auto preceding = [&](const DeltaRow& play) noexcept {
        return ResetAppliesBefore(reset, play.executeFrame, play.ticket, play.eventIndex) == false;
    };
    for (auto index = row.head; index != UINT32_MAX; index = deltas_[index].next)
        if (preceding(deltas_[index])) return true;
    for (auto index = row.preparedHead; index != UINT32_MAX; index = deltas_[index].next)
        if (preceding(deltas_[index])) return true;
    for (const auto& delta : deltas_)
        if (delta.used && delta.rollbackable && delta.row < rows_.size() &&
            &rows_[delta.row] == &row) return true;
    return false;
}

void SelectionMemoryTableV1::RefreshResetJournal(std::uint32_t rowIndex) noexcept {
    auto& row = rows_[rowIndex];
    // Actual history never retains consumption coordinates for journal records already removed.
    ClearResetFrontiers(row.state);
    const MemoryState* before = &row.state;
    for (auto index = row.scheduledHead; index != UINT32_MAX; index = deltas_[index].next) {
        auto& reset = deltas_[index];
        if (reset.pendingReset) continue;
        reset.status = ComputeReset(row, *before, reset.overlay);
        if (reset.status == SelectionMemoryStatusV1::Ok) {
            MarkScheduledFrontier(reset.overlay, reset);
            before = &reset.overlay;
        }
    }
    Recompute(rowIndex);
    RecomputePrepared(rowIndex);
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::PendingPredecessor(const Row& row,
    const MemoryState& before, const SelectionPlayRequestV1& request, const MemoryState** out,
    bool includePrivate) noexcept {
    *out = &before;
    std::uint32_t scratch = 0;
    // Keep the accepted FIFO prefix, then add only scheduled resets it has not consumed.
    for (auto index = row.scheduledHead; index != UINT32_MAX; index = deltas_[index].next) {
        const auto& reset = deltas_[index];
        const auto& prefix = **out;
        if (reset.status != SelectionMemoryStatusV1::Ok ||
            !ResetAppliesBefore(reset, request.executeFrame, request.ticket, request.eventIndex) ||
            ResetConsumed(prefix, reset)) continue;
        const auto status = ComputeReset(row, prefix, pendingScratch_[scratch]);
        if (status != SelectionMemoryStatusV1::Ok) return status;
        MarkScheduledFrontier(pendingScratch_[scratch], reset);
        *out = &pendingScratch_[scratch];
        scratch ^= 1u;
    }
    if (includePrivate) {
        // Reservations have no row links; find each next coordinate in the bounded pool.
        const DeltaRow* prior = nullptr;
        for (std::size_t visited = 0; visited < deltas_.size(); ++visited) {
            const DeltaRow* next = nullptr;
            for (const auto& reset : deltas_) {
                if (!reset.prepared || !reset.pendingReset || reset.ticket != request.ticket ||
                    !SameKey(reset.request.key, row.key) || !PendingResetHasHistory(row, reset) ||
                    !ResetAppliesBefore(reset, request.executeFrame, request.ticket, request.eventIndex) ||
                    (prior != nullptr && !AppliesBefore(prior->executeFrame, prior->ticket, prior->eventIndex,
                        reset.executeFrame, reset.ticket, reset.eventIndex))) continue;
                if (next == nullptr || AppliesBefore(reset.executeFrame, reset.ticket, reset.eventIndex,
                    next->executeFrame, next->ticket, next->eventIndex)) next = &reset;
            }
            if (next == nullptr) break;
            prior = next;
            if (ResetConsumed(**out, *next)) continue;
            const auto status = ComputeReset(row, **out, pendingScratch_[scratch]);
            if (status != SelectionMemoryStatusV1::Ok) return status;
            MarkScheduledFrontier(pendingScratch_[scratch], *next);
            *out = &pendingScratch_[scratch];
            scratch ^= 1u;
        }
    }
    return SelectionMemoryStatusV1::Ok;
}

void SelectionMemoryTableV1::LinkDelta(Row& row, std::uint32_t rowIndex,
                                       std::uint32_t deltaIndex) noexcept {
    AdvanceMutationEpoch();
    DeltaRow& delta = deltas_[deltaIndex];
    delta.row = rowIndex;
    delta.previous = row.tail;
    delta.next = UINT32_MAX;
    if (row.tail != UINT32_MAX) deltas_[row.tail].next = deltaIndex;
    if (row.head == UINT32_MAX) row.head = deltaIndex;
    row.tail = deltaIndex;
    ++row.pendingCount;
    ++pendingCount_;
    if (!row.counted) {
        row.counted = true;
        ++rowCount_;
    }
}

void SelectionMemoryTableV1::LinkScheduled(Row& row, std::uint32_t rowIndex,
                                           std::uint32_t deltaIndex) noexcept {
    AdvanceMutationEpoch();
    DeltaRow& delta = deltas_[deltaIndex];
    delta.row = rowIndex;
    std::uint32_t insertAfter = UINT32_MAX;
    for (std::uint32_t index = row.scheduledHead; index != UINT32_MAX;
         index = deltas_[index].next) {
        const DeltaRow& prior = deltas_[index];
        if (AppliesBefore(prior.executeFrame, prior.ticket, prior.eventIndex,
                          delta.executeFrame, delta.ticket,
                          delta.eventIndex) ||
            (prior.executeFrame == delta.executeFrame &&
             prior.ticket == delta.ticket &&
             prior.eventIndex == delta.eventIndex)) {
            insertAfter = index;
        } else {
            break;
        }
    }
    if (insertAfter == UINT32_MAX) {
        delta.previous = UINT32_MAX;
        delta.next = row.scheduledHead;
        if (row.scheduledHead != UINT32_MAX) {
            deltas_[row.scheduledHead].previous = deltaIndex;
        } else {
            row.scheduledTail = deltaIndex;
        }
        row.scheduledHead = deltaIndex;
    } else {
        delta.previous = insertAfter;
        delta.next = deltas_[insertAfter].next;
        deltas_[insertAfter].next = deltaIndex;
        if (delta.next != UINT32_MAX) {
            deltas_[delta.next].previous = deltaIndex;
        } else {
            row.scheduledTail = deltaIndex;
        }
    }
    ++row.scheduledCount;
    if (delta.pendingReset) {
        RefreshResetJournal(rowIndex);
        return;
    }
    // The inserted reset changes every later reset's cumulative epoch.
    const MemoryState* before = &delta.overlay;
    for (auto index = delta.next; index != UINT32_MAX; index = deltas_[index].next) {
        auto& following = deltas_[index];
        if (following.pendingReset) continue;
        following.status = ComputeReset(row, *before, following.overlay);
        if (following.status == SelectionMemoryStatusV1::Ok) {
            MarkScheduledFrontier(following.overlay, following);
            before = &following.overlay;
        }
    }
    RecomputePrepared(rowIndex);
}

void SelectionMemoryTableV1::UnlinkScheduledHead(
    Row& row, std::uint32_t deltaIndex) noexcept {
    UnlinkScheduled(row, deltaIndex);
}

void SelectionMemoryTableV1::UnlinkScheduled(Row& row, std::uint32_t deltaIndex) noexcept {
    AdvanceMutationEpoch();
    const auto previous = deltas_[deltaIndex].previous;
    const std::uint32_t next = deltas_[deltaIndex].next;
    if (previous == UINT32_MAX) row.scheduledHead = next;
    else deltas_[previous].next = next;
    if (next != UINT32_MAX) {
        deltas_[next].previous = previous;
    } else {
        row.scheduledTail = previous;
    }
    --row.scheduledCount;
}

bool SelectionMemoryTableV1::DropScheduledThrough(
    Row& row, std::uint64_t executeFrame, std::uint64_t ticket,
    std::uint32_t eventIndex) noexcept {
    std::uint32_t index = row.scheduledHead;
    bool nativeRemoved = false;
    while (index != UINT32_MAX) {
        const std::uint32_t next = deltas_[index].next;
        const DeltaRow& delta = deltas_[index];
        if (delta.pendingReset ? ResetConsumed(row.state, delta) :
            (AppliesBefore(delta.executeFrame, delta.ticket, delta.eventIndex,
                          executeFrame, ticket, eventIndex) ||
            (delta.executeFrame == executeFrame && delta.ticket == ticket &&
             delta.eventIndex == eventIndex))) {
            nativeRemoved |= delta.pendingReset;
            UnlinkScheduled(row, index);
            RecycleDelta(index);
        }
        index = next;
    }
    return nativeRemoved;
}

void SelectionMemoryTableV1::ApplyDue(std::uint64_t now) noexcept {
    if (occupiedRowCount_ == 0) return;
    for (std::uint32_t rowIndex = 0; rowIndex < rows_.size(); ++rowIndex) {
        Row& row = rows_[rowIndex];
        if (!row.used) {
            continue;
        }
        bool changed = false;
        bool nativeChanged = false;
        bool mixedChanged = false;
        for (auto slot = row.scheduledHead; slot != UINT32_MAX;) {
            DeltaRow& delta = deltas_[slot];
            const auto next = delta.next;
            if (delta.executeFrame > now) {
                break;
            }
            if (delta.pendingReset) {
                if (PendingResetBlocked(row, delta)) { slot = next; continue; }
                const auto status = ComputeReset(row, row.state, pendingScratch_[0]);
                if (status != SelectionMemoryStatusV1::Ok) { slot = next; continue; }
                (void)CopyState(pendingScratch_[0], row.state);
                nativeChanged = true;
            } else {
                // Rebuild against actual history if native records share this journal.
                const bool mixed = nativeChanged || std::any_of(deltas_.begin(), deltas_.end(),
                    [rowIndex](const auto& reset) { return reset.used && reset.pendingReset && reset.row == rowIndex; });
                if (mixed) {
                    if (ComputeReset(row, row.state, pendingScratch_[0]) != SelectionMemoryStatusV1::Ok) break;
                    (void)CopyState(pendingScratch_[0], row.state);
                    mixedChanged = true;
                } else if (delta.status == SelectionMemoryStatusV1::Ok && !CopyState(delta.overlay, row.state)) break;
            }
            row.committed = true;
            if (!row.counted) {
                row.counted = true;
                ++rowCount_;
            }
            UnlinkScheduled(row, slot);
            RecycleDelta(slot);
            changed = true;
            slot = next;
        }
        if (changed) {
            if (nativeChanged || mixedChanged) RefreshResetJournal(rowIndex);
            else { Recompute(rowIndex); RecomputePrepared(rowIndex); }
        }
    }
}

SelectionAppendResultV1 SelectionMemoryTableV1::AppendPlay(
    const SelectionPlayRequestV1& request) noexcept {
    SelectionAppendResultV1 result{};
    if (PrivateResetConflicts(request.key)) {
        result.status = SelectionMemoryStatusV1::NotHead;
        return result;
    }
    if (!ValidKey(request.key) || request.program == nullptr ||
        request.program->containerStableId != request.key.containerStableId ||
        request.program->programSemanticFingerprint !=
            request.key.programSemanticFingerprint ||
        request.program->memoryScope != request.key.memoryScope ||
        request.program->childCount == 0 ||
        request.program->childCount > config_.maxChildren ||
        request.program->avoidLastN > config_.maxAvoidLastN ||
        request.pathCount > config_.maxPathNodes ||
        (request.pathCount != 0 && request.path == nullptr) ||
        (request.program->mode != ContainerModeV1::Random &&
         request.program->mode != ContainerModeV1::Sequence)) {
        return result;
    }
    std::uint32_t rowIndex = FindRow(request.key);
    const bool newRow = rowIndex == UINT32_MAX;
    if (newRow) rowIndex = FindFreeRow(request.key);
    const std::uint32_t deltaIndex = FindFreeDelta();
    if (rowIndex == UINT32_MAX || deltaIndex == UINT32_MAX) {
        result.status = SelectionMemoryStatusV1::Capacity;
        return result;
    }
    Row& row = rows_[rowIndex];
    const std::uint32_t perRowLimit =
        request.key.memoryScope == MemoryScopeV1::Global
            ? config_.maxPendingGlobalPerRow
            : config_.maxPendingEmitterPerRow;
    if (!newRow && row.preparedCount != 0) {
        result.status = SelectionMemoryStatusV1::Capacity;
        return result;
    }
    if (!newRow && row.tail != UINT32_MAX &&
        deltas_[row.tail].status != SelectionMemoryStatusV1::Ok) {
        result.status = deltas_[row.tail].status;
        return result;
    }
    const auto occupancy = row.pendingCount + row.scheduledCount + row.candidateCount;
    if (occupancy >= perRowLimit ||
        (!newRow &&
         !PendingOrderValid(row, request.ticket, request.eventIndex)) ||
        (newRow && request.ticket == 0)) {
        result.status = occupancy >= perRowLimit
                            ? SelectionMemoryStatusV1::Capacity
                            : SelectionMemoryStatusV1::InvalidInput;
        return result;
    }
    DeltaRow& delta = deltas_[deltaIndex];
    delta.kind = SelectionDeltaKindV1::Play;
    delta.request = request;
    delta.path.clear();
    if (request.pathCount != 0) {
        delta.path.assign(request.path, request.path + request.pathCount);
    }
    delta.request.path = delta.path.empty() ? nullptr : delta.path.data();
    delta.ticket = request.ticket;
    delta.eventIndex = request.eventIndex;
    delta.executeFrame = request.executeFrame;
    Row preparedRow = {};
    const Row* computationRow = &row;
    if (newRow) {
        preparedRow.key = request.key;
        preparedRow.mode = request.program->mode;
        preparedRow.randomDiscipline = request.program->randomDiscipline;
        preparedRow.sequenceEndPolicy = request.program->sequenceEndPolicy;
        preparedRow.initialSequenceCursor =
            request.program->initialSequenceCursor;
        preparedRow.avoidLastN = request.program->avoidLastN;
        computationRow = &preparedRow;
        InitializeState(preparedRow, delta.overlay);
    }
    const MemoryState* actualBefore = nullptr;
    if (newRow) {
        InitializeState(preparedRow, row.state);
        actualBefore = &row.state;
    } else {
        actualBefore = row.tail == UINT32_MAX ? &row.state
                                              : &deltas_[row.tail].overlay;
    }
    delta.status = PendingPredecessor(*computationRow, *actualBefore, delta.request, &actualBefore);
    if (delta.status == SelectionMemoryStatusV1::Ok)
        delta.status = ComputePlay(*computationRow, delta.request, *actualBefore,
            delta.overlay, &delta.selectedChildOrdinal, &delta.seedProbe);
    if (delta.status != SelectionMemoryStatusV1::Ok) {
        result.status = delta.status;
        delta.path.clear();
        return result;
    }
    if (newRow) {
        row.used = true;
        ++occupiedRowCount_;
        row.committed = false;
        row.counted = true;
        row.key = preparedRow.key;
        row.mode = preparedRow.mode;
        row.randomDiscipline = preparedRow.randomDiscipline;
        row.sequenceEndPolicy = preparedRow.sequenceEndPolicy;
        row.initialSequenceCursor = preparedRow.initialSequenceCursor;
        row.avoidLastN = preparedRow.avoidLastN;
        InitializeState(row, row.state);
        ++rowCount_;
    }
    delta.used = true;
    LinkDelta(row, rowIndex, deltaIndex);
    result.status = SelectionMemoryStatusV1::Ok;
    result.delta = {deltaIndex, delta.generation, tableLifetime_};
    return result;
}

SelectionAppendResultV1 SelectionMemoryTableV1::AppendReset(
    const SelectionMemoryKeyV1& key, std::uint64_t ticket,
    std::uint32_t eventIndex) noexcept {
    SelectionAppendResultV1 result{};
    if (!ValidKey(key) || ticket == 0) return result;
    if (PrivateResetConflicts(key)) { result.status = SelectionMemoryStatusV1::NotHead; return result; }
    const std::uint32_t rowIndex = FindRow(key);
    if (rowIndex == UINT32_MAX) {
        result.status = SelectionMemoryStatusV1::Ok;
        result.idempotent = true;
        return result;
    }
    Row& row = rows_[rowIndex];
    if (!row.counted && !row.committed && row.pendingCount == 0 && row.preparedCount == 0 && row.scheduledCount == 0) {
        result.status = SelectionMemoryStatusV1::Ok;
        result.idempotent = true;
        return result;
    }
    if (row.preparedCount != 0) {
        result.status = SelectionMemoryStatusV1::Capacity;
        return result;
    }
    const std::uint32_t perRowLimit =
        key.memoryScope == MemoryScopeV1::Global
            ? config_.maxPendingGlobalPerRow
            : config_.maxPendingEmitterPerRow;
    const std::uint32_t deltaIndex = FindFreeDelta();
    if (row.pendingCount + row.scheduledCount + row.candidateCount >= perRowLimit || deltaIndex == UINT32_MAX) {
        result.status = SelectionMemoryStatusV1::Capacity;
        return result;
    }
    if (!PendingOrderValid(row, ticket, eventIndex)) return result;
    if (row.tail != UINT32_MAX &&
        deltas_[row.tail].status != SelectionMemoryStatusV1::Ok) {
        result.status = deltas_[row.tail].status;
        return result;
    }
    DeltaRow& delta = deltas_[deltaIndex];
    delta.kind = SelectionDeltaKindV1::Reset;
    delta.ticket = ticket;
    delta.eventIndex = eventIndex;
    const MemoryState& before = row.tail == UINT32_MAX
                                    ? row.state
                                    : deltas_[row.tail].overlay;
    delta.status = ComputeReset(row, before, delta.overlay);
    if (delta.status != SelectionMemoryStatusV1::Ok) {
        result.status = delta.status;
        return result;
    }
    delta.used = true;
    LinkDelta(row, rowIndex, deltaIndex);
    result.status = SelectionMemoryStatusV1::Ok;
    result.delta = {deltaIndex, delta.generation, tableLifetime_};
    return result;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::PreparePlayForPendingBatch(
    const SelectionPlayRequestV1& request, PreparedSelectionPlayV1* out,
    std::uint32_t* outSelectedChildOrdinal) noexcept {
    return PreparePlayImpl(request, out, outSelectedChildOrdinal, true);
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::PreparePlay(
    const SelectionPlayRequestV1& request,
    PreparedSelectionPlayV1* out,
    std::uint32_t* outSelectedChildOrdinal) noexcept {
    return PreparePlayImpl(request, out, outSelectedChildOrdinal, false);
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::PreparePlayImpl(
    const SelectionPlayRequestV1& request, PreparedSelectionPlayV1* out,
    std::uint32_t* outSelectedChildOrdinal, bool nativePendingPreparation) noexcept {
    if (outSelectedChildOrdinal != nullptr) {
        *outSelectedChildOrdinal = 0;
    }
    if (out == nullptr || out->Valid() || out->Committed() || outSelectedChildOrdinal == nullptr ||
        !ValidKey(request.key) || request.program == nullptr ||
        request.program->containerStableId != request.key.containerStableId ||
        request.program->programSemanticFingerprint !=
            request.key.programSemanticFingerprint ||
        request.program->memoryScope != request.key.memoryScope ||
        request.program->childCount == 0 ||
        request.program->childCount > config_.maxChildren ||
        request.program->avoidLastN > config_.maxAvoidLastN ||
        request.pathCount > config_.maxPathNodes ||
        (request.pathCount != 0 && request.path == nullptr) ||
        (request.program->mode != ContainerModeV1::Random &&
         request.program->mode != ContainerModeV1::Sequence) ||
        request.ticket == 0) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    std::uint32_t rowIndex = FindRow(request.key);
    const bool newRow = rowIndex == UINT32_MAX;
    if (newRow) {
        rowIndex = FindFreeRow(request.key);
    }
    if (PrivateResetConflicts(request.key, nativePendingPreparation ? request.ticket : 0))
        return SelectionMemoryStatusV1::NotHead;
    if (nativePendingPreparation) {
        for (const auto& reset : deltas_)
            if (reset.pendingReset && (reset.used || reset.prepared) && SameKey(reset.request.key, request.key) &&
                reset.ticket == request.ticket && reset.eventIndex == request.eventIndex)
                return SelectionMemoryStatusV1::InvalidInput;
    }
    const std::uint32_t deltaIndex = FindFreeDelta();
    if (rowIndex == UINT32_MAX || deltaIndex == UINT32_MAX) {
        return SelectionMemoryStatusV1::Capacity;
    }
    Row& row = rows_[rowIndex];
    const std::uint32_t perRowLimit =
        request.key.memoryScope == MemoryScopeV1::Global
            ? config_.maxPendingGlobalPerRow
            : config_.maxPendingEmitterPerRow;
    if (row.preparedHead != UINT32_MAX &&
        deltas_[row.preparedHead].nativePendingPreparation != nativePendingPreparation) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    if ((!nativePendingPreparation && row.pendingCount != 0) ||
        static_cast<std::uint64_t>(row.pendingCount) + row.preparedCount + row.scheduledCount +
            row.candidateCount + PrivateResetCount(request.key) >= perRowLimit) {
        return SelectionMemoryStatusV1::Capacity;
    }
    if (nativePendingPreparation) {
        for (const auto& transaction : deltas_)
            if (transaction.used && transaction.rollbackable && transaction.row == rowIndex)
                return SelectionMemoryStatusV1::NotHead;
        if (row.tail != UINT32_MAX && deltas_[row.tail].status != SelectionMemoryStatusV1::Ok)
            return deltas_[row.tail].status;
        if (!PendingOrderValid(row, request.ticket, request.eventIndex))
            return SelectionMemoryStatusV1::InvalidInput;
    }
    if (row.preparedTail != UINT32_MAX) {
        const auto& prior = deltas_[row.preparedTail];
        if (nativePendingPreparation && prior.status != SelectionMemoryStatusV1::Ok) return prior.status;
        if (!Later(request.ticket, request.eventIndex, prior.ticket, prior.eventIndex))
            return SelectionMemoryStatusV1::InvalidInput;
    }

    if (newRow) {
        row.key = request.key;
        row.mode = request.program->mode;
        row.randomDiscipline = request.program->randomDiscipline;
        row.sequenceEndPolicy = request.program->sequenceEndPolicy;
        row.initialSequenceCursor =
            request.program->initialSequenceCursor;
        row.avoidLastN = request.program->avoidLastN;
        InitializeState(row, row.state);
    }

    DeltaRow& delta = deltas_[deltaIndex];
    delta.kind = SelectionDeltaKindV1::Play;
    delta.request = request;
    delta.path.clear();
    if (request.pathCount != 0) {
        delta.path.assign(request.path, request.path + request.pathCount);
    }
    delta.request.path = delta.path.empty() ? nullptr : delta.path.data();
    delta.ticket = request.ticket;
    delta.eventIndex = request.eventIndex;
    delta.executeFrame = request.executeFrame;
    const MemoryState* before = nullptr;
    if (nativePendingPreparation) {
        before = row.preparedTail != UINT32_MAX ? &deltas_[row.preparedTail].overlay
            : row.tail != UINT32_MAX ? &deltas_[row.tail].overlay : &row.state;
        delta.status = PendingPredecessor(row, *before, delta.request, &before, true);
    } else {
        before = &PreparedPredecessor(row, request.executeFrame, request.ticket, request.eventIndex);
        delta.status = PendingPredecessor(row, *before, delta.request, &before);
    }
    if (delta.status == SelectionMemoryStatusV1::Ok)
        delta.status = ComputePlay(row, delta.request, *before, delta.overlay,
            &delta.selectedChildOrdinal, &delta.seedProbe);
    if (delta.status != SelectionMemoryStatusV1::Ok) {
        const SelectionMemoryStatusV1 status = delta.status;
        delta.request = {};
        delta.path.clear();
        delta.ticket = 0;
        delta.eventIndex = 0;
        delta.selectedChildOrdinal = 0;
        delta.seedProbe = {};
        delta.nativePendingPreparation = false;
        return status;
    }

    if (newRow) {
        row.used = true;
        ++occupiedRowCount_;
        row.committed = false;
        row.counted = false;
    }
    delta.row = rowIndex;
    AdvanceMutationEpoch();
    delta.prepared = true;
    delta.nativePendingPreparation = nativePendingPreparation;
    delta.previous = row.preparedTail;
    delta.next = UINT32_MAX;
    if (row.preparedTail != UINT32_MAX) {
        deltas_[row.preparedTail].next = deltaIndex;
    } else {
        row.preparedHead = deltaIndex;
    }
    row.preparedTail = deltaIndex;
    ++row.preparedCount;
    out->owner_ = this;
    out->slot_ = deltaIndex;
    out->generation_ = delta.generation;
    out->lifetime_ = tableLifetime_;
    out->prepared_ = true;
    *outSelectedChildOrdinal = delta.selectedChildOrdinal;
    return SelectionMemoryStatusV1::Ok;
}

SelectionSeedProbeCopyV1 SelectionMemoryTableV1::PreparedPlaySeedProbe(
    const PreparedSelectionPlayV1& prepared) const noexcept {
    if (prepared.owner_ != this || prepared.lifetime_ != tableLifetime_ || !prepared.prepared_ ||
        prepared.slot_ >= deltas_.size()) {
        return {};
    }
    const DeltaRow& delta = deltas_[prepared.slot_];
    if (!delta.prepared || delta.generation != prepared.generation_ ||
        delta.kind != SelectionDeltaKindV1::Play) {
        return {};
    }
    return delta.seedProbe;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CommitPreparedPlay(
    PreparedSelectionPlayV1* prepared,
    std::uint64_t ticket) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        prepared->lifetime_ != tableLifetime_ ||
        !prepared->prepared_ || ticket == 0 ||
        prepared->slot_ >= deltas_.size()) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    DeltaRow& delta = deltas_[prepared->slot_];
    if (!delta.prepared || delta.generation != prepared->generation_ ||
        delta.kind != SelectionDeltaKindV1::Play ||
        delta.ticket != ticket || delta.row >= rows_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    if (delta.nativePendingPreparation) return SelectionMemoryStatusV1::InvalidInput;
    const std::uint32_t rowIndex = delta.row;
    Row& row = rows_[rowIndex];
    if (row.pendingCount != 0 || row.preparedHead != prepared->slot_) {
        return SelectionMemoryStatusV1::NotHead;
    }
    row.preparedHead = delta.next;
    if (delta.next != UINT32_MAX) {
        deltas_[delta.next].previous = UINT32_MAX;
    } else {
        row.preparedTail = UINT32_MAX;
    }
    --row.preparedCount;
    delta.prepared = false;
    delta.used = true;
    delta.previous = UINT32_MAX;
    delta.next = UINT32_MAX;
    const std::uint64_t playFrame = delta.request.executeFrame;
    const std::uint64_t playTicket = delta.ticket;
    const std::uint32_t playEvent = delta.eventIndex;
    const SelectionDeltaRefV1 ref{prepared->slot_, delta.generation, tableLifetime_};
    LinkDelta(row, delta.row, prepared->slot_);
    const SelectionMemoryStatusV1 status = CommitHead(ref);
    if (status == SelectionMemoryStatusV1::Ok) {
        if (!row.counted) {
            row.counted = true;
            ++rowCount_;
        }
        DropScheduledThrough(row, playFrame, playTicket, playEvent);
        prepared->owner_ = nullptr;
        prepared->slot_ = UINT32_MAX;
        prepared->generation_ = 0;
        prepared->lifetime_ = 0;
        prepared->prepared_ = false;
    }
    return status;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::MovePreparedPlayToPending(
    PreparedSelectionPlayV1* prepared, std::uint64_t ticket,
    SelectionDeltaRefV1* out) noexcept {
    if (out != nullptr) {
        *out = {};
    }
    if (prepared == nullptr || prepared->owner_ != this ||
        prepared->lifetime_ != tableLifetime_ ||
        !prepared->prepared_ || ticket == 0 || out == nullptr ||
        prepared->slot_ >= deltas_.size()) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    DeltaRow& delta = deltas_[prepared->slot_];
    if (!delta.prepared || delta.generation != prepared->generation_ ||
        delta.kind != SelectionDeltaKindV1::Play ||
        delta.ticket != ticket || delta.row >= rows_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    if (delta.nativePendingPreparation) return SelectionMemoryStatusV1::InvalidInput;
    const std::uint32_t rowIndex = delta.row;
    Row& row = rows_[rowIndex];
    if (row.pendingCount != 0 || row.preparedHead != prepared->slot_) {
        return SelectionMemoryStatusV1::NotHead;
    }
    row.preparedHead = delta.next;
    if (delta.next != UINT32_MAX) {
        deltas_[delta.next].previous = UINT32_MAX;
    } else {
        row.preparedTail = UINT32_MAX;
    }
    --row.preparedCount;
    delta.prepared = false;
    delta.used = true;
    delta.previous = UINT32_MAX;
    delta.next = UINT32_MAX;
    const SelectionDeltaRefV1 ref{prepared->slot_, delta.generation, tableLifetime_};
    LinkDelta(row, rowIndex, prepared->slot_);
    if (!row.counted) {
        row.counted = true;
        ++rowCount_;
    }
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->generation_ = 0;
    prepared->lifetime_ = 0;
    prepared->prepared_ = false;
    *out = ref;
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::MovePreparedPlaysToPending(
    PreparedSelectionPlayV1* const* prepared, std::uint32_t count,
    std::uint64_t ticket, PendingSelectionBatchV1* out) noexcept {
    if (prepared == nullptr || count == 0 || count > deltas_.size() || ticket == 0 ||
        out == nullptr || out->Valid()) return SelectionMemoryStatusV1::InvalidInput;
    std::uint32_t batchSlot = UINT32_MAX;
    for (std::uint32_t slot = 0; slot < batches_.size(); ++slot)
        if (!batches_[slot].used && !batches_[slot].quarantined) { batchSlot = slot; break; }
    if (batchSlot == UINT32_MAX) return SelectionMemoryStatusV1::Capacity;
    WorldGeneration world = 0;
    std::uint64_t frame = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto* token = prepared[index];
        if (token == nullptr || token->owner_ != this || token->lifetime_ != tableLifetime_ ||
            !token->prepared_ || token->committed_ ||
            token->slot_ >= deltas_.size()) return SelectionMemoryStatusV1::InvalidInput;
        const auto& delta = deltas_[token->slot_];
        if (!delta.prepared || delta.used || delta.generation != token->generation_ ||
            delta.kind != SelectionDeltaKindV1::Play || delta.ticket != ticket ||
            delta.row >= rows_.size() || delta.batch.slot != UINT32_MAX)
            return SelectionMemoryStatusV1::NotFound;
        if (delta.status != SelectionMemoryStatusV1::Ok) return delta.status;
        const auto& row = rows_[delta.row];
        if (PrivateResetConflicts(row.key, delta.nativePendingPreparation ? ticket : 0))
            return SelectionMemoryStatusV1::NotHead;
        if (!row.used || (!delta.nativePendingPreparation && row.pendingCount != 0))
            return SelectionMemoryStatusV1::NotHead;
        if (delta.nativePendingPreparation) {
            if (row.tail != UINT32_MAX && deltas_[row.tail].status != SelectionMemoryStatusV1::Ok)
                return deltas_[row.tail].status;
            if (!PendingOrderValid(row, delta.ticket, delta.eventIndex))
                return SelectionMemoryStatusV1::InvalidInput;
        }
        for (const auto& transaction : deltas_)
            if (transaction.used && transaction.rollbackable && transaction.row == delta.row)
                return SelectionMemoryStatusV1::NotHead;
        if (index == 0) { world = row.key.world; frame = delta.executeFrame; }
        if (row.key.world != world || delta.executeFrame != frame)
            return SelectionMemoryStatusV1::InvalidInput;
        auto expected = row.preparedHead;
        for (std::uint32_t prior = 0; prior < index; ++prior) {
            if (prepared[prior]->slot_ == token->slot_) return SelectionMemoryStatusV1::InvalidInput;
            const auto& previous = deltas_[prepared[prior]->slot_];
            if (previous.row == delta.row) {
                if (expected != prepared[prior]->slot_) return SelectionMemoryStatusV1::NotHead;
                expected = previous.next;
            }
        }
        if (expected != token->slot_) return SelectionMemoryStatusV1::NotHead;
    }
    auto& batch = batches_[batchSlot];
    batch.used = true;
    batch.head = prepared[0]->slot_;
    batch.count = count;
    batch.revision = 1;
    for (std::uint32_t index = 0; index < count; ++index) {
        auto& delta = deltas_[prepared[index]->slot_];
        auto& row = rows_[delta.row];
        row.preparedHead = delta.next;
        if (delta.next != UINT32_MAX) deltas_[delta.next].previous = UINT32_MAX;
        else row.preparedTail = UINT32_MAX;
        --row.preparedCount;
        delta.prepared = false;
        delta.used = true;
        delta.batch = {batchSlot, batch.generation,
            index + 1u == count ? UINT32_MAX : prepared[index + 1u]->slot_, index};
        LinkDelta(row, delta.row, prepared[index]->slot_);
        if (!row.counted) { row.counted = true; ++rowCount_; }
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        auto& token = *prepared[index];
        token.owner_ = nullptr;
        token.slot_ = UINT32_MAX;
        token.generation_ = 0;
        token.lifetime_ = 0;
        token.prepared_ = false;
    }
    out->owner_ = this;
    out->slot_ = batchSlot;
    out->generation_ = batch.generation;
    out->lifetime_ = tableLifetime_;
    return SelectionMemoryStatusV1::Ok;
}

void SelectionMemoryTableV1::AdvanceMutationEpoch() noexcept {
    if (mutationEpoch_ != UINT64_MAX) ++mutationEpoch_;
}

bool SelectionMemoryTableV1::ValidBatchIdentity(const SelectionPendingBatchIdentityV1& identity) const noexcept {
    return identity.world != 0 && identity.worldSemanticFingerprint != 0 &&
        identity.rootStableId != VnObjectId{} && identity.acceptedPostOrdinal != 0 && identity.ticket != 0 &&
        identity.eventCount != 0 && identity.eventCount - 1u <= UINT32_MAX - identity.eventBegin &&
        ((identity.emitterSlot == UINT32_MAX && identity.emitterGeneration == 0) ||
         (identity.emitterSlot < config_.maxEmitters && identity.emitterGeneration != 0));
}

bool SelectionMemoryTableV1::MatchesBatchIdentity(const SelectionPendingBatchIdentityV1& identity,
    const SelectionPlayRequestV1& request) const noexcept {
    return request.key.world == identity.world && request.worldSemanticFingerprint == identity.worldSemanticFingerprint &&
        request.rootStableId == identity.rootStableId && request.callerSeed == identity.callerSeed &&
        request.acceptedPostOrdinal == identity.acceptedPostOrdinal && request.ticket == identity.ticket &&
        request.eventIndex >= identity.eventBegin && request.eventIndex - identity.eventBegin < identity.eventCount &&
        (request.key.memoryScope == MemoryScopeV1::Global ||
         (request.key.emitterSlot == identity.emitterSlot && request.key.emitterGeneration == identity.emitterGeneration));
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::IssueProducerPendingSelectionBatch(
    const SelectionPendingBatchIdentityV1& identity, PreparedSelectionPlayV1* const* prepared,
    std::uint32_t count, PendingSelectionBatchV1* out) noexcept {
    if (!ValidBatchIdentity(identity) || out == nullptr || out->Valid() || count > deltas_.size() ||
        (count != 0 && prepared == nullptr)) return SelectionMemoryStatusV1::InvalidInput;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto* token = prepared[index];
        if (token == nullptr || token->owner_ != this || token->lifetime_ != tableLifetime_ ||
            !token->prepared_ || token->slot_ >= deltas_.size()) return SelectionMemoryStatusV1::InvalidInput;
        const auto& delta = deltas_[token->slot_];
        if (!delta.prepared || delta.generation != token->generation_ || !delta.nativePendingPreparation ||
            !MatchesBatchIdentity(identity, delta.request)) return SelectionMemoryStatusV1::InvalidInput;
    }
    if (count != 0) {
        const auto result = MovePreparedPlaysToPending(prepared, count, identity.ticket, out);
        if (result != SelectionMemoryStatusV1::Ok) return result;
    } else {
        std::uint32_t slot = UINT32_MAX;
        for (std::uint32_t index = 0; index < batches_.size(); ++index)
            if (!batches_[index].used && !batches_[index].quarantined) { slot = index; break; }
        if (slot == UINT32_MAX) return SelectionMemoryStatusV1::Capacity;
        AdvanceMutationEpoch();
        auto& native = batches_[slot];
        native.used = true;
        native.head = UINT32_MAX;
        native.count = 0;
        native.revision = 1;
        out->owner_ = this;
        out->slot_ = slot;
        out->generation_ = native.generation;
        out->lifetime_ = tableLifetime_;
    }
    batches_[out->slot_].producer = true;
    batches_[out->slot_].identity = identity;
    return SelectionMemoryStatusV1::Ok;
}

const SelectionMemoryTableV1::BatchRow* SelectionMemoryTableV1::ResolveReplacement(
    const PendingSelectionReplacementV1& replacement) const noexcept {
    if (replacement.owner_ != this || replacement.lifetime_ != tableLifetime_ ||
        replacement.slot_ >= batches_.size()) return nullptr;
    const auto& batch = batches_[replacement.slot_];
    return batch.used && batch.producer && batch.generation == replacement.generation_ &&
        batch.replacementActive && batch.replacementSession == replacement.session_ ? &batch : nullptr;
}

bool SelectionMemoryTableV1::ValidateReplacementMembers(std::uint32_t slot) const noexcept {
    if (slot >= batches_.size()) return false;
    const auto& batch = batches_[slot];
    if (!batch.replacementActive || batch.replacementCount > deltas_.size()) return false;
    auto member = batch.replacementHead;
    for (std::uint32_t ordinal = 0; ordinal < batch.replacementCount; ++ordinal) {
        if (member >= deltas_.size()) return false;
        const auto& delta = deltas_[member];
        if (!delta.replacementCandidate || delta.used || delta.prepared || delta.quarantined ||
            delta.row >= rows_.size() || !rows_[delta.row].used || rows_[delta.row].candidateCount == 0 ||
            !SameKey(rows_[delta.row].key, delta.request.key) || delta.batch.slot != slot ||
            delta.batch.generation != batch.generation || delta.batch.ordinal != ordinal) return false;
        if (ordinal + 1u == batch.replacementCount && member != batch.replacementTail) return false;
        member = delta.batch.next;
    }
    return member == UINT32_MAX && (batch.replacementCount != 0 || batch.replacementTail == UINT32_MAX);
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CheckReplacementCurrent(
    const PendingSelectionReplacementV1& replacement) const noexcept {
    const auto* batch = ResolveReplacement(replacement);
    if (batch == nullptr || !ValidateBatchMembers(replacement.slot_) ||
        !ValidateReplacementMembers(replacement.slot_)) return SelectionMemoryStatusV1::NotFound;
    if (mutationEpoch_ == UINT64_MAX || batch->revision == UINT64_MAX)
        return SelectionMemoryStatusV1::RuntimeRecreateRequired;
    if (batch->replacementEpoch != mutationEpoch_ || batch->replacementRevision != batch->revision)
        return SelectionMemoryStatusV1::InvalidInput;
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::BeginPendingBatchReplacement(
    const PendingSelectionBatchV1& batch, std::uint64_t revision, PendingSelectionReplacementV1* out) noexcept {
    const auto* native = ResolveBatch(batch);
    if (native == nullptr || !ValidateBatchMembers(batch.slot_)) return SelectionMemoryStatusV1::NotFound;
    if (out == nullptr || out->Valid() || !native->producer || native->replacementActive ||
        revision == 0 || revision != native->revision) return SelectionMemoryStatusV1::InvalidInput;
    if (mutationEpoch_ == UINT64_MAX || native->revision == UINT64_MAX || native->replacementSession == UINT64_MAX)
        return SelectionMemoryStatusV1::RuntimeRecreateRequired;
    auto& target = batches_[batch.slot_];
    target.replacementActive = true;
    target.replacementSealed = false;
    ++target.replacementSession;
    target.replacementEpoch = mutationEpoch_;
    target.replacementRevision = revision;
    target.replacementHead = target.replacementTail = UINT32_MAX;
    target.replacementCount = 0;
    out->owner_ = this;
    out->slot_ = batch.slot_;
    out->generation_ = batch.generation_;
    out->lifetime_ = tableLifetime_;
    out->session_ = target.replacementSession;
    return SelectionMemoryStatusV1::Ok;
}

bool SelectionMemoryTableV1::ReplacementRowCompatible(std::uint32_t rowIndex) const noexcept {
    const auto& row = rows_[rowIndex];
    if (PrivateResetConflicts(row.key)) return false;
    if (row.preparedHead != UINT32_MAX && !deltas_[row.preparedHead].nativePendingPreparation) return false;
    return std::none_of(deltas_.begin(), deltas_.end(), [rowIndex](const auto& delta) {
        return delta.used && delta.rollbackable && delta.row == rowIndex;
    });
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CheckReplacementPlayOrder(std::uint32_t batchSlot,
    std::uint32_t rowIndex, const SelectionPlayRequestV1& request) const noexcept {
    const auto& row = rows_[rowIndex];
    for (auto member = row.head; member != UINT32_MAX; member = deltas_[member].next) {
        const auto& prior = deltas_[member];
        if (prior.ticket == request.ticket && prior.eventIndex == request.eventIndex &&
            (prior.batch.slot != batchSlot || prior.batch.generation != batches_[batchSlot].generation))
            return SelectionMemoryStatusV1::InvalidInput;
    }
    for (auto member = row.scheduledHead; member != UINT32_MAX; member = deltas_[member].next)
        if (deltas_[member].ticket == request.ticket && deltas_[member].eventIndex == request.eventIndex)
            return SelectionMemoryStatusV1::InvalidInput;
    for (auto member = row.preparedHead; member != UINT32_MAX; member = deltas_[member].next) {
        const auto& prior = deltas_[member];
        if (prior.ticket == request.ticket && prior.eventIndex == request.eventIndex)
            return SelectionMemoryStatusV1::InvalidInput;
        // Hidden preparations must remain a transferable suffix after the installed candidate.
        if (!Later(prior.ticket, prior.eventIndex, request.ticket, request.eventIndex))
            return SelectionMemoryStatusV1::NotHead;
    }
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CheckReplacementBatchOrder(std::uint32_t batchSlot) const noexcept {
    const auto& batch = batches_[batchSlot];
    if (batch.replacementCount == 0) return SelectionMemoryStatusV1::Ok;
    std::uint32_t edgeCount = 0;
    // Build the post-splice wait graph without changing any real row or batch.
    for (std::uint32_t rowIndex = 0; rowIndex < rows_.size(); ++rowIndex) {
        const auto& row = rows_[rowIndex];
        if (!row.used) continue;
        auto pending = row.head;
        auto candidate = batch.replacementHead;
        auto previousBatch = UINT32_MAX;
        bool ordinaryGap = false;
        for (;;) {
            while (pending != UINT32_MAX && deltas_[pending].batch.slot == batchSlot &&
                deltas_[pending].batch.generation == batch.generation) pending = deltas_[pending].next;
            while (candidate != UINT32_MAX && deltas_[candidate].row != rowIndex)
                candidate = deltas_[candidate].batch.next;
            if (pending == UINT32_MAX && candidate == UINT32_MAX) break;
            const bool takeCandidate = candidate != UINT32_MAX && (pending == UINT32_MAX ||
                Later(deltas_[pending].ticket, deltas_[pending].eventIndex,
                    deltas_[candidate].ticket, deltas_[candidate].eventIndex));
            const auto& delta = deltas_[takeCandidate ? candidate : pending];
            const auto currentBatch = delta.batch.slot;
            if (currentBatch == UINT32_MAX) {
                ordinaryGap = previousBatch != UINT32_MAX;
            } else {
                if (currentBatch >= batches_.size() || !batches_[currentBatch].used ||
                    delta.batch.generation != batches_[currentBatch].generation)
                    return SelectionMemoryStatusV1::InvalidInput;
                if (previousBatch != UINT32_MAX && (previousBatch != currentBatch || ordinaryGap)) {
                    if (edgeCount == replacementBatchEdges_.size()) return SelectionMemoryStatusV1::InvalidInput;
                    replacementBatchEdges_[edgeCount++] = {previousBatch, currentBatch};
                }
                previousBatch = currentBatch;
                ordinaryGap = false;
            }
            if (takeCandidate) candidate = delta.batch.next;
            else pending = delta.next;
        }
    }
    // Only the replaced batch can introduce a new cycle; ordinary gaps retain self-edges.
    std::fill(replacementBatchReachable_.begin(), replacementBatchReachable_.end(), 0);
    replacementBatchReachable_[batchSlot] = 1;
    for (std::size_t pass = 0; pass < batches_.size(); ++pass) {
        bool changed = false;
        for (std::uint32_t index = 0; index < edgeCount; ++index) {
            const auto& edge = replacementBatchEdges_[index];
            if (replacementBatchReachable_[edge.before] == 0) continue;
            if (edge.after == batchSlot) return SelectionMemoryStatusV1::NotHead;
            if (replacementBatchReachable_[edge.after] == 0) {
                replacementBatchReachable_[edge.after] = 1;
                changed = true;
            }
        }
        if (!changed) return SelectionMemoryStatusV1::Ok;
    }
    return SelectionMemoryStatusV1::InvalidInput;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::ComputeReplacementPlay(
    std::uint32_t batchSlot, std::uint32_t rowIndex, const SelectionPlayRequestV1& request,
    MemoryState& after, std::uint32_t* selected, SelectionSeedProbeCopyV1* seed) noexcept {
    const auto& row = rows_[rowIndex];
    const auto& batch = batches_[batchSlot];
    const MemoryState* before = &row.state;
    std::uint32_t scratch = 0;
    auto pending = row.head;
    auto candidate = batch.replacementHead;
    const auto skipPending = [&]() {
        while (pending != UINT32_MAX && deltas_[pending].batch.slot == batchSlot &&
            deltas_[pending].batch.generation == batch.generation) pending = deltas_[pending].next;
    };
    const auto skipCandidate = [&]() {
        while (candidate != UINT32_MAX && deltas_[candidate].row != rowIndex) candidate = deltas_[candidate].batch.next;
    };
    // Rebuild from actual committed history; old overlays contain the very members being replaced.
    for (;;) {
        skipPending();
        skipCandidate();
        const bool hasPending = pending != UINT32_MAX && Later(request.ticket, request.eventIndex,
            deltas_[pending].ticket, deltas_[pending].eventIndex);
        const bool hasCandidate = candidate != UINT32_MAX && Later(request.ticket, request.eventIndex,
            deltas_[candidate].ticket, deltas_[candidate].eventIndex);
        if (!hasPending && !hasCandidate) break;
        const bool takeCandidate = hasCandidate && (!hasPending || Later(deltas_[pending].ticket,
            deltas_[pending].eventIndex, deltas_[candidate].ticket, deltas_[candidate].eventIndex));
        const auto& prior = deltas_[takeCandidate ? candidate : pending];
        auto& next = replacementScratch_[scratch];
        auto status = SelectionMemoryStatusV1::Ok;
        if (prior.kind == SelectionDeltaKindV1::Reset) status = ComputeReset(row, *before, next);
        else {
            const MemoryState* applicable = before;
            status = PendingPredecessor(row, *before, prior.request, &applicable);
            std::uint32_t ignored = 0;
            if (status == SelectionMemoryStatusV1::Ok)
                status = ComputePlay(row, prior.request, *applicable, next, &ignored, nullptr);
        }
        if (status != SelectionMemoryStatusV1::Ok) return status;
        before = &next;
        scratch ^= 1u;
        if (takeCandidate) candidate = prior.batch.next;
        else pending = prior.next;
    }
    const auto status = PendingPredecessor(row, *before, request, &before);
    return status == SelectionMemoryStatusV1::Ok ? ComputePlay(row, request, *before, after, selected, seed) : status;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::PrepareReplacementPlay(
    PendingSelectionReplacementV1* replacement, const SelectionPlayRequestV1& request,
    std::uint32_t* outSelectedChildOrdinal) noexcept {
    if (outSelectedChildOrdinal != nullptr) *outSelectedChildOrdinal = 0;
    if (replacement == nullptr || outSelectedChildOrdinal == nullptr) return SelectionMemoryStatusV1::InvalidInput;
    const auto current = CheckReplacementCurrent(*replacement);
    if (current != SelectionMemoryStatusV1::Ok) return current;
    auto& batch = batches_[replacement->slot_];
    if (batch.replacementSealed || !MatchesBatchIdentity(batch.identity, request) || !ValidKey(request.key) ||
        request.program == nullptr || request.program->containerStableId != request.key.containerStableId ||
        request.program->programSemanticFingerprint != request.key.programSemanticFingerprint ||
        request.program->memoryScope != request.key.memoryScope || request.program->avoidLastN > config_.maxAvoidLastN ||
        request.pathCount > config_.maxPathNodes || (request.pathCount != 0 && request.path == nullptr) ||
        (request.program->mode != ContainerModeV1::Random && request.program->mode != ContainerModeV1::Sequence))
        return SelectionMemoryStatusV1::InvalidInput;
    if (batch.replacementCount != 0 && deltas_[batch.replacementHead].executeFrame != request.executeFrame)
        return SelectionMemoryStatusV1::InvalidInput;
    auto rowIndex = FindRow(request.key);
    const bool newRow = rowIndex == UINT32_MAX;
    if (newRow) rowIndex = FindFreeRow(request.key);
    const auto deltaIndex = FindFreeDelta();
    if (rowIndex == UINT32_MAX || deltaIndex == UINT32_MAX) return SelectionMemoryStatusV1::Capacity;
    auto& row = rows_[rowIndex];
    if (!ReplacementRowCompatible(rowIndex)) return SelectionMemoryStatusV1::NotHead;
    const auto limit = request.key.memoryScope == MemoryScopeV1::Global ? config_.maxPendingGlobalPerRow : config_.maxPendingEmitterPerRow;
    if (row.pendingCount + row.preparedCount + row.scheduledCount + row.candidateCount >= limit)
        return SelectionMemoryStatusV1::Capacity;
    const auto order = CheckReplacementPlayOrder(replacement->slot_, rowIndex, request);
    if (order != SelectionMemoryStatusV1::Ok) return order;
    for (auto member = batch.replacementHead; member != UINT32_MAX; member = deltas_[member].batch.next)
        if (deltas_[member].row == rowIndex && !Later(request.ticket, request.eventIndex,
                deltas_[member].ticket, deltas_[member].eventIndex)) return SelectionMemoryStatusV1::InvalidInput;
    if (newRow) {
        row.key = request.key;
        row.mode = request.program->mode;
        row.randomDiscipline = request.program->randomDiscipline;
        row.sequenceEndPolicy = request.program->sequenceEndPolicy;
        row.initialSequenceCursor = request.program->initialSequenceCursor;
        row.avoidLastN = request.program->avoidLastN;
        InitializeState(row, row.state);
    }
    auto& delta = deltas_[deltaIndex];
    delta.kind = SelectionDeltaKindV1::Play;
    delta.request = request;
    delta.path.clear();
    if (request.pathCount != 0) delta.path.assign(request.path, request.path + request.pathCount);
    delta.request.path = delta.path.empty() ? nullptr : delta.path.data();
    delta.status = ComputeReplacementPlay(replacement->slot_, rowIndex, delta.request,
        delta.overlay, &delta.selectedChildOrdinal, &delta.seedProbe);
    if (delta.status != SelectionMemoryStatusV1::Ok) {
        const auto status = delta.status;
        delta.request = {};
        delta.path.clear();
        delta.seedProbe = {};
        return status;
    }
    if (!row.used) {
        row.used = true;
        ++occupiedRowCount_;
    }
    ++row.candidateCount;
    delta.replacementCandidate = true;
    delta.nativePendingPreparation = true;
    delta.row = rowIndex;
    delta.ticket = request.ticket;
    delta.eventIndex = request.eventIndex;
    delta.executeFrame = request.executeFrame;
    delta.batch = {replacement->slot_, batch.generation, UINT32_MAX, batch.replacementCount};
    if (batch.replacementTail == UINT32_MAX) batch.replacementHead = deltaIndex;
    else deltas_[batch.replacementTail].batch.next = deltaIndex;
    batch.replacementTail = deltaIndex;
    ++batch.replacementCount;
    *outSelectedChildOrdinal = delta.selectedChildOrdinal;
    return SelectionMemoryStatusV1::Ok;
}

SelectionReplacementPlayViewV1 SelectionMemoryTableV1::ReadReplacementPlay(
    const PendingSelectionReplacementV1& replacement, std::uint32_t ordinal) const noexcept {
    const auto status = CheckReplacementCurrent(replacement);
    if (status != SelectionMemoryStatusV1::Ok) return {status};
    const auto& batch = batches_[replacement.slot_];
    if (ordinal >= batch.replacementCount) return {};
    auto member = batch.replacementHead;
    for (std::uint32_t index = 0; index < ordinal; ++index) member = deltas_[member].batch.next;
    const auto& delta = deltas_[member];
    return {delta.status, delta.selectedChildOrdinal, delta.seedProbe};
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::SealReplacement(
    PendingSelectionReplacementV1* replacement, std::uint32_t count, std::uint64_t frame) noexcept {
    if (replacement == nullptr) return SelectionMemoryStatusV1::InvalidInput;
    const auto current = CheckReplacementCurrent(*replacement);
    if (current != SelectionMemoryStatusV1::Ok) return current;
    auto& batch = batches_[replacement->slot_];
    if (batch.replacementSealed || count != batch.replacementCount ||
        (count != 0 && deltas_[batch.replacementHead].executeFrame != frame)) return SelectionMemoryStatusV1::InvalidInput;
    batch.replacementSealed = true;
    batch.replacementFrame = frame;
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CanInstallReplacement(
    const PendingSelectionReplacementV1& replacement) const noexcept {
    const auto current = CheckReplacementCurrent(replacement);
    if (current != SelectionMemoryStatusV1::Ok) return current;
    const auto& batch = batches_[replacement.slot_];
    if (!batch.replacementSealed) return SelectionMemoryStatusV1::InvalidInput;
    // Reserve epoch/revision space for the entire no-fail splice and its suffix recomputation.
    const auto budget = 8u * static_cast<std::uint64_t>(deltas_.size()) + 4u * rows_.size() + 4u;
    if (budget >= UINT64_MAX - mutationEpoch_ || budget >= UINT64_MAX - batch.revision)
        return SelectionMemoryStatusV1::RuntimeRecreateRequired;
    for (auto member = batch.replacementHead; member != UINT32_MAX; member = deltas_[member].batch.next) {
        const auto& delta = deltas_[member];
        const auto& row = rows_[delta.row];
        const auto limit = row.key.memoryScope == MemoryScopeV1::Global ? config_.maxPendingGlobalPerRow : config_.maxPendingEmitterPerRow;
        if (delta.status != SelectionMemoryStatusV1::Ok) return delta.status;
        if (!MatchesBatchIdentity(batch.identity, delta.request) || delta.executeFrame != batch.replacementFrame ||
            !ReplacementRowCompatible(delta.row)) return SelectionMemoryStatusV1::NotHead;
        const auto order = CheckReplacementPlayOrder(replacement.slot_, delta.row, delta.request);
        if (order != SelectionMemoryStatusV1::Ok) return order;
        if (row.pendingCount + row.preparedCount + row.scheduledCount + row.candidateCount > limit)
            return SelectionMemoryStatusV1::Capacity;
    }
    for (const auto& follower : batches_)
        if (follower.used && budget >= UINT64_MAX - follower.revision) return SelectionMemoryStatusV1::RuntimeRecreateRequired;
    return CheckReplacementBatchOrder(replacement.slot_);
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::InstallReplacement(
    PendingSelectionReplacementV1* replacement, PendingSelectionBatchV1* batch) noexcept {
    if (replacement == nullptr || batch == nullptr || ResolveBatch(*batch) == nullptr ||
        batch->slot_ != replacement->slot_ || batch->generation_ != replacement->generation_)
        return SelectionMemoryStatusV1::InvalidInput;
    const auto ready = CanInstallReplacement(*replacement);
    if (ready != SelectionMemoryStatusV1::Ok) return ready;
    auto& native = batches_[batch->slot_];
    std::fill(replacementAffectedRows_.begin(), replacementAffectedRows_.end(), 0);
    // Every operation below uses already-owned storage and cannot fail on this control thread.
    for (auto member = native.head; member != UINT32_MAX;) {
        const auto next = deltas_[member].batch.next;
        replacementAffectedRows_[deltas_[member].row] = 1;
        UnlinkDelta(rows_[deltas_[member].row], member);
        RecycleDelta(member);
        member = next;
    }
    native.head = native.replacementHead;
    native.count = native.replacementCount;
    for (auto member = native.head; member != UINT32_MAX; member = deltas_[member].batch.next) {
        auto& delta = deltas_[member];
        auto& row = rows_[delta.row];
        --row.candidateCount;
        replacementAffectedRows_[delta.row] = 1;
        delta.replacementCandidate = false;
        delta.used = true;
        auto after = UINT32_MAX;
        auto before = row.head;
        while (before != UINT32_MAX && Later(delta.ticket, delta.eventIndex, deltas_[before].ticket, deltas_[before].eventIndex)) {
            after = before;
            before = deltas_[before].next;
        }
        delta.previous = after;
        delta.next = before;
        if (after == UINT32_MAX) row.head = member;
        else deltas_[after].next = member;
        if (before == UINT32_MAX) row.tail = member;
        else deltas_[before].previous = member;
        ++row.pendingCount;
        ++pendingCount_;
        if (!row.counted) { row.counted = true; ++rowCount_; }
    }
    native.replacementActive = native.replacementSealed = false;
    native.replacementHead = native.replacementTail = UINT32_MAX;
    native.replacementCount = 0;
    ++native.revision;
    AdvanceMutationEpoch();
    for (std::uint32_t index = 0; index < rows_.size(); ++index) {
        auto& row = rows_[index];
        if (!row.used || replacementAffectedRows_[index] == 0) continue;
        if (row.pendingCount == 0 && !row.committed && row.counted) { row.counted = false; --rowCount_; }
        if (row.pendingCount == 0 && row.preparedCount == 0 && row.scheduledCount == 0 &&
            row.candidateCount == 0 && !row.committed) ReleaseRow(index);
        else { Recompute(index); RecomputePrepared(index); }
    }
    replacement->Clear();
    return SelectionMemoryStatusV1::Ok;
}

void SelectionMemoryTableV1::CancelReplacementSlot(std::uint32_t slot) noexcept {
    auto& batch = batches_[slot];
    if (!batch.replacementActive) return;
    for (auto member = batch.replacementHead; member != UINT32_MAX;) {
        const auto next = deltas_[member].batch.next;
        auto& row = rows_[deltas_[member].row];
        --row.candidateCount;
        RecycleDelta(member);
        member = next;
    }
    batch.replacementActive = batch.replacementSealed = false;
    batch.replacementHead = batch.replacementTail = UINT32_MAX;
    batch.replacementCount = 0;
    for (std::uint32_t index = 0; index < rows_.size(); ++index) {
        const auto& row = rows_[index];
        if (row.used && !row.committed && !row.counted && row.pendingCount == 0 &&
            row.preparedCount == 0 && row.scheduledCount == 0 && row.candidateCount == 0) ReleaseRow(index);
    }
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CancelReplacement(PendingSelectionReplacementV1* replacement) noexcept {
    if (replacement == nullptr || ResolveReplacement(*replacement) == nullptr ||
        !ValidateReplacementMembers(replacement->slot_)) return SelectionMemoryStatusV1::NotFound;
    CancelReplacementSlot(replacement->slot_);
    replacement->Clear();
    return SelectionMemoryStatusV1::Ok;
}

const SelectionMemoryTableV1::BatchRow* SelectionMemoryTableV1::ResolveBatch(
    const PendingSelectionBatchV1& batch) const noexcept {
    if (batch.owner_ != this || batch.lifetime_ != tableLifetime_ || batch.slot_ >= batches_.size()) return nullptr;
    const auto& row = batches_[batch.slot_];
    return row.used && !row.quarantined && row.generation == batch.generation_ ? &row : nullptr;
}

bool SelectionMemoryTableV1::ValidateBatchMembers(std::uint32_t slot) const noexcept {
    if (slot >= batches_.size()) return false;
    const auto& batch = batches_[slot];
    if (!batch.used || batch.quarantined || batch.count > deltas_.size()) return false;
    if (batch.count == 0) return batch.producer && batch.head == UINT32_MAX;
    auto member = batch.head;
    for (std::uint32_t ordinal = 0; ordinal < batch.count; ++ordinal) {
        if (member >= deltas_.size()) return false;
        const auto& delta = deltas_[member];
        if (!delta.used || delta.prepared || delta.quarantined || delta.row >= rows_.size() ||
            !rows_[delta.row].used || delta.kind != SelectionDeltaKindV1::Play ||
            delta.batch.slot != slot || delta.batch.generation != batch.generation ||
            delta.batch.ordinal != ordinal) return false;
        member = delta.batch.next;
    }
    return member == UINT32_MAX;
}

PendingSelectionBatchViewV1 SelectionMemoryTableV1::ReadPendingBatch(
    const PendingSelectionBatchV1& batch) const noexcept {
    const auto* row = ResolveBatch(batch);
    if (row == nullptr || !ValidateBatchMembers(batch.slot_)) return {};
    return {true, row->count, row->revision};
}

PendingSelectionBatchMemberViewV1 SelectionMemoryTableV1::ReadPendingBatchMember(
    const PendingSelectionBatchV1& batch, std::uint32_t ordinal) const noexcept {
    const auto* row = ResolveBatch(batch);
    if (row == nullptr || ordinal >= row->count || !ValidateBatchMembers(batch.slot_)) return {};
    auto member = row->head;
    for (std::uint32_t index = 0; index < ordinal; ++index) member = deltas_[member].batch.next;
    const auto& delta = deltas_[member];
    const SelectionDeltaRefV1 ref{member, delta.generation, tableLifetime_};
    return {ref, Delta(ref), delta.seedProbe, row->revision};
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CanCommitPendingBatch(
    const PendingSelectionBatchV1& batch, std::uint64_t revision) const noexcept {
    const auto* native = ResolveBatch(batch);
    if (native == nullptr || !ValidateBatchMembers(batch.slot_)) return SelectionMemoryStatusV1::NotFound;
    if (native->replacementActive && !ValidateReplacementMembers(batch.slot_)) return SelectionMemoryStatusV1::NotFound;
    if (native->revision == UINT64_MAX) return SelectionMemoryStatusV1::RuntimeRecreateRequired;
    if (revision == 0 || revision != native->revision) return SelectionMemoryStatusV1::InvalidInput;
    for (auto member = native->head; member != UINT32_MAX; member = deltas_[member].batch.next) {
        const auto& delta = deltas_[member];
        if (delta.status != SelectionMemoryStatusV1::Ok) return delta.status;
        const auto& row = rows_[delta.row];
        auto expected = row.head;
        for (auto prior = native->head; prior != member; prior = deltas_[prior].batch.next) {
            if (deltas_[prior].row == delta.row) {
                if (prior != expected) return SelectionMemoryStatusV1::NotHead;
                expected = deltas_[prior].next;
            }
        }
        if (expected != member) return SelectionMemoryStatusV1::NotHead;
        if (delta.overlay.recent.size() > row.state.recent.capacity() ||
            delta.overlay.permutation.size() > row.state.permutation.capacity())
            return SelectionMemoryStatusV1::InvalidInput;
    }
    return SelectionMemoryStatusV1::Ok;
}

void SelectionMemoryTableV1::RecycleBatch(std::uint32_t slot) noexcept {
    AdvanceMutationEpoch();
    auto& batch = batches_[slot];
    batch.used = false;
    batch.head = UINT32_MAX;
    batch.count = 0;
    batch.revision = 1;
    batch.producer = false;
    batch.identity = {};
    batch.replacementActive = false;
    batch.replacementSealed = false;
    batch.replacementSession = 0;
    batch.replacementHead = batch.replacementTail = UINT32_MAX;
    batch.replacementCount = 0;
    if (batch.generation == UINT32_MAX) batch.quarantined = true;
    else ++batch.generation;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CommitPendingBatch(
    PendingSelectionBatchV1* batch, std::uint64_t revision) noexcept {
    if (batch == nullptr) return SelectionMemoryStatusV1::InvalidInput;
    const auto ready = CanCommitPendingBatch(*batch, revision);
    if (ready != SelectionMemoryStatusV1::Ok) return ready;
    CancelReplacementSlot(batch->slot_);
    const auto slot = batch->slot_;
    const auto generation = batch->generation_;
    // All fallible checks precede this single-threaded publication.
    RecycleBatch(slot);
    for (std::uint32_t index = 0; index < rows_.size(); ++index) {
        auto& row = rows_[index];
        if (!row.used || row.head == UINT32_MAX || deltas_[row.head].batch.slot != slot ||
            deltas_[row.head].batch.generation != generation) continue;
        auto last = row.head;
        while (deltas_[last].next != UINT32_MAX) {
            const auto next = deltas_[last].next;
            if (deltas_[next].batch.slot != slot || deltas_[next].batch.generation != generation) break;
            last = next;
        }
        const auto& final = deltas_[last];
        (void)CopyState(final.overlay, row.state);
        row.committed = true;
        const bool nativeRemoved = DropScheduledThrough(row, final.executeFrame, final.ticket, final.eventIndex);
        while (row.head != UINT32_MAX && deltas_[row.head].batch.slot == slot &&
            deltas_[row.head].batch.generation == generation) {
            const auto member = row.head;
            UnlinkHead(row, member);
            RecycleDelta(member);
        }
        if (nativeRemoved) RefreshResetJournal(index);
        else RecomputePrepared(index);
    }
    batch->Clear();
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CanCommitPendingBatches(
    const PendingSelectionBatchCommitV1* inputs, std::uint32_t count) const noexcept {
    if (count > batches_.size() || (count != 0 && inputs == nullptr))
        return SelectionMemoryStatusV1::InvalidInput;
    if (count == 0) return SelectionMemoryStatusV1::Ok;
    const std::uint64_t epochBudget = static_cast<std::uint64_t>(count) + 2u * static_cast<std::uint64_t>(deltas_.size());
    if (mutationEpoch_ >= UINT64_MAX - epochBudget)
        return SelectionMemoryStatusV1::RuntimeRecreateRequired;
    for (std::uint32_t row = 0; row < rows_.size(); ++row) pendingCommitHeads_[row] = rows_[row].head;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto* token = inputs[index].batch;
        if (token == nullptr) return SelectionMemoryStatusV1::InvalidInput;
        const auto* batch = ResolveBatch(*token);
        if (batch == nullptr || !ValidateBatchMembers(token->slot_)) return SelectionMemoryStatusV1::NotFound;
        if (!batch->producer || !ValidBatchIdentity(batch->identity)) return SelectionMemoryStatusV1::InvalidInput;
        if (batch->replacementActive && !ValidateReplacementMembers(token->slot_))
            return SelectionMemoryStatusV1::NotFound;
        if (batch->revision == UINT64_MAX) return SelectionMemoryStatusV1::RuntimeRecreateRequired;
        if (inputs[index].revision == 0 || inputs[index].revision != batch->revision)
            return SelectionMemoryStatusV1::InvalidInput;
        for (std::uint32_t prior = 0; prior < index; ++prior)
            if (inputs[prior].batch->slot_ == token->slot_ && inputs[prior].batch->generation_ == token->generation_)
                return SelectionMemoryStatusV1::InvalidInput;
        for (auto member = batch->head; member != UINT32_MAX; member = deltas_[member].batch.next) {
            const auto& delta = deltas_[member];
            const auto& row = rows_[delta.row];
            if (!delta.nativePendingPreparation || !MatchesBatchIdentity(batch->identity, delta.request))
                return SelectionMemoryStatusV1::InvalidInput;
            if (delta.status != SelectionMemoryStatusV1::Ok) return delta.status;
            if (pendingCommitHeads_[delta.row] != member) return SelectionMemoryStatusV1::NotHead;
            if (PrivateResetConflicts(row.key)) return SelectionMemoryStatusV1::NotHead;
            if (delta.overlay.recent.size() > row.state.recent.capacity() ||
                delta.overlay.permutation.size() > row.state.permutation.capacity())
                return SelectionMemoryStatusV1::InvalidInput;
            pendingCommitHeads_[delta.row] = delta.next;
        }
    }
    for (const auto& delta : deltas_)
        if (delta.used && delta.rollbackable && delta.row < rows_.size() &&
            pendingCommitHeads_[delta.row] != rows_[delta.row].head)
            return SelectionMemoryStatusV1::NotHead;
    // Each surviving member can advance its batch revision once during the final journal refresh.
    for (std::uint32_t slot = 0; slot < batches_.size(); ++slot) {
        const auto& batch = batches_[slot];
        if (!batch.used) continue;
        std::uint64_t increments = 0;
        for (auto member = batch.head; member != UINT32_MAX; member = deltas_[member].batch.next) {
            const auto row = deltas_[member].row;
            if (pendingCommitHeads_[row] == rows_[row].head) continue;
            for (auto suffix = pendingCommitHeads_[row]; suffix != UINT32_MAX; suffix = deltas_[suffix].next)
                if (suffix == member) { ++increments; break; }
        }
        if (increments != 0 && batch.revision >= UINT64_MAX - increments)
            return SelectionMemoryStatusV1::RuntimeRecreateRequired;
    }
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CommitPendingBatches(
    const PendingSelectionBatchCommitV1* inputs, std::uint32_t count) noexcept {
    const auto ready = CanCommitPendingBatches(inputs, count);
    if (ready != SelectionMemoryStatusV1::Ok || count == 0) return ready;
    // No selection mutation or fallible publication is allowed between the engine preflight and this commit.
    for (std::uint32_t index = 0; index < count; ++index) CancelReplacementSlot(inputs[index].batch->slot_);
    for (std::uint32_t rowIndex = 0; rowIndex < rows_.size(); ++rowIndex) {
        auto& row = rows_[rowIndex];
        replacementAffectedRows_[rowIndex] = pendingCommitHeads_[rowIndex] != row.head ? 1 : 0;
        if (replacementAffectedRows_[rowIndex] == 0) continue;
        auto last = row.head;
        while (deltas_[last].next != pendingCommitHeads_[rowIndex]) last = deltas_[last].next;
        const auto& final = deltas_[last];
        (void)CopyState(final.overlay, row.state);
        row.committed = true;
        (void)DropScheduledThrough(row, final.executeFrame, final.ticket, final.eventIndex);
        while (row.head != pendingCommitHeads_[rowIndex]) {
            const auto member = row.head;
            UnlinkHead(row, member);
            RecycleDelta(member);
        }
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        RecycleBatch(inputs[index].batch->slot_);
        inputs[index].batch->Clear();
    }
    for (std::uint32_t row = 0; row < rows_.size(); ++row)
        if (replacementAffectedRows_[row] != 0) RefreshResetJournal(row);
    return SelectionMemoryStatusV1::Ok;
}

void SelectionMemoryTableV1::CancelBatchSlot(std::uint32_t slot) noexcept {
    CancelReplacementSlot(slot);
    const auto generation = batches_[slot].generation;
    RecycleBatch(slot);
    for (std::uint32_t index = 0; index < rows_.size(); ++index) {
        auto& row = rows_[index];
        bool changed = false;
        for (auto member = row.head; member != UINT32_MAX;) {
            const auto next = deltas_[member].next;
            if (deltas_[member].batch.slot == slot && deltas_[member].batch.generation == generation) {
                UnlinkDelta(row, member);
                RecycleDelta(member);
                changed = true;
            }
            member = next;
        }
        if (!changed) continue;
        if (row.pendingCount == 0 && !row.committed && row.counted) {
            row.counted = false;
            --rowCount_;
        }
        if (row.pendingCount == 0 && row.preparedCount == 0 && row.scheduledCount == 0 &&
            row.candidateCount == 0 && !row.committed)
            ReleaseRow(index);
        else {
            Recompute(index);
            RecomputePrepared(index);
        }
    }
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CancelPendingBatch(PendingSelectionBatchV1* batch) noexcept {
    if (batch == nullptr) return SelectionMemoryStatusV1::InvalidInput;
    if (ResolveBatch(*batch) == nullptr || !ValidateBatchMembers(batch->slot_))
        return SelectionMemoryStatusV1::NotFound;
    CancelBatchSlot(batch->slot_);
    batch->Clear();
    return SelectionMemoryStatusV1::Ok;
}

std::uint32_t SelectionMemoryTableV1::FreePendingBatchCount() const noexcept {
    return static_cast<std::uint32_t>(std::count_if(batches_.begin(), batches_.end(),
        [](const auto& batch) { return !batch.used && !batch.quarantined; }));
}

std::size_t SelectionMemoryTableV1::PendingBatchStorageBytes() const noexcept {
    std::size_t bytes = batches_.capacity() * sizeof(BatchRow) +
        deltas_.capacity() * (sizeof(DeltaRow::BatchMembership) + sizeof(DeltaRow::replacementCandidate)) +
        rows_.capacity() * sizeof(Row::candidateCount) + sizeof(mutationEpoch_) +
        sizeof(pendingScratch_) + sizeof(replacementScratch_) + replacementAffectedRows_.capacity() +
        replacementBatchEdges_.capacity() * sizeof(BatchDependencyEdge) + replacementBatchReachable_.capacity() +
        pendingCommitHeads_.capacity() * sizeof(std::uint32_t);
    for (const auto& state : pendingScratch_)
        bytes += (state.recent.capacity() + state.permutation.capacity()) * sizeof(std::uint32_t);
    for (const auto& state : replacementScratch_)
        bytes += (state.recent.capacity() + state.permutation.capacity()) * sizeof(std::uint32_t);
    return bytes;
}

SelectionMemoryStatusV1
SelectionMemoryTableV1::CommitPreparedPlayTransactional(
    PreparedSelectionPlayV1* prepared,
    std::uint64_t ticket) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        prepared->lifetime_ != tableLifetime_ ||
        !prepared->prepared_ || prepared->committed_ || ticket == 0 ||
        prepared->slot_ >= deltas_.size()) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    DeltaRow& delta = deltas_[prepared->slot_];
    if (!delta.prepared || delta.generation != prepared->generation_ ||
        delta.kind != SelectionDeltaKindV1::Play ||
        delta.ticket != ticket || delta.row >= rows_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    if (delta.nativePendingPreparation) return SelectionMemoryStatusV1::InvalidInput;
    const std::uint32_t rowIndex = delta.row;
    Row& row = rows_[rowIndex];
    if (row.pendingCount != 0 || row.preparedHead != prepared->slot_) {
        return SelectionMemoryStatusV1::NotHead;
    }
    if (!CopyState(row.state, delta.rollback) ||
        !CopyState(delta.overlay, row.state)) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    delta.rollbackCommitted = row.committed;
    AdvanceMutationEpoch();
    delta.rollbackCounted = row.counted;
    delta.rollbackable = true;
    row.preparedHead = delta.next;
    if (delta.next != UINT32_MAX) {
        deltas_[delta.next].previous = UINT32_MAX;
    } else {
        row.preparedTail = UINT32_MAX;
    }
    --row.preparedCount;
    delta.prepared = false;
    delta.used = true;
    delta.previous = UINT32_MAX;
    delta.next = UINT32_MAX;
    row.committed = true;
    if (!row.counted) {
        row.counted = true;
        ++rowCount_;
    }
    prepared->prepared_ = false;
    prepared->committed_ = true;
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::FinalizeCommittedPlay(
    PreparedSelectionPlayV1* committed) noexcept {
    if (committed == nullptr || committed->owner_ != this ||
        committed->lifetime_ != tableLifetime_ ||
        !committed->committed_ || committed->prepared_ ||
        committed->slot_ >= deltas_.size()) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    DeltaRow& delta = deltas_[committed->slot_];
    if (!delta.used || delta.prepared || !delta.rollbackable ||
        delta.generation != committed->generation_ ||
        delta.row >= rows_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    Row& row = rows_[delta.row];
    const std::uint64_t playFrame = delta.request.executeFrame;
    const std::uint64_t playTicket = delta.ticket;
    const std::uint32_t playEvent = delta.eventIndex;
    const auto rowIndex = delta.row;
    const bool nativeRemoved = DropScheduledThrough(row, playFrame, playTicket, playEvent);
    RecycleDelta(committed->slot_);
    if (nativeRemoved) RefreshResetJournal(rowIndex);
    committed->owner_ = nullptr;
    committed->slot_ = UINT32_MAX;
    committed->generation_ = 0;
    committed->lifetime_ = 0;
    committed->committed_ = false;
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::RollbackCommittedPlay(
    PreparedSelectionPlayV1* committed) noexcept {
    if (committed == nullptr || committed->owner_ != this ||
        committed->lifetime_ != tableLifetime_ ||
        !committed->committed_ || committed->prepared_ ||
        committed->slot_ >= deltas_.size()) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    DeltaRow& delta = deltas_[committed->slot_];
    if (!delta.used || delta.prepared || !delta.rollbackable ||
        delta.generation != committed->generation_ ||
        delta.row >= rows_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    const std::uint32_t rowIndex = delta.row;
    Row& row = rows_[rowIndex];
    if (!CopyState(delta.rollback, row.state)) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    row.committed = delta.rollbackCommitted;
    if (row.counted && !delta.rollbackCounted) {
        row.counted = false;
        --rowCount_;
    } else {
        row.counted = delta.rollbackCounted;
    }
    RecycleDelta(committed->slot_);
    committed->owner_ = nullptr;
    committed->slot_ = UINT32_MAX;
    committed->generation_ = 0;
    committed->lifetime_ = 0;
    committed->committed_ = false;
    RecomputePrepared(rowIndex);
    if (row.preparedCount == 0 && row.pendingCount == 0 && row.scheduledCount == 0 &&
        row.candidateCount == 0 && !row.committed && !row.counted) {
        ReleaseRow(rowIndex);
    }
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CancelPreparedPlay(
    PreparedSelectionPlayV1* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        prepared->lifetime_ != tableLifetime_ ||
        !prepared->prepared_ || prepared->slot_ >= deltas_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    DeltaRow& delta = deltas_[prepared->slot_];
    if (!delta.prepared || delta.generation != prepared->generation_ ||
        delta.kind != SelectionDeltaKindV1::Play ||
        delta.row >= rows_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    const std::uint32_t rowIndex = delta.row;
    Row& row = rows_[rowIndex];
    if (delta.previous == UINT32_MAX) {
        row.preparedHead = delta.next;
    } else {
        deltas_[delta.previous].next = delta.next;
    }
    if (delta.next == UINT32_MAX) {
        row.preparedTail = delta.previous;
    } else {
        deltas_[delta.next].previous = delta.previous;
    }
    --row.preparedCount;
    RecycleDelta(prepared->slot_);
    RecomputePrepared(rowIndex);
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->generation_ = 0;
    prepared->lifetime_ = 0;
    prepared->prepared_ = false;
    if (row.preparedCount == 0 && row.pendingCount == 0 && row.scheduledCount == 0 &&
        row.candidateCount == 0 && !row.committed && !row.counted) {
        ReleaseRow(rowIndex);
    }
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::PreparePendingReset(
    const SelectionMemoryKeyV1& key, std::uint64_t expectedTicket,
    std::uint32_t eventIndex, PreparedSelectionResetV1* out,
    std::uint64_t executeFrame) noexcept {
    if (out == nullptr || out->Valid() || !ValidKey(key) || expectedTicket == 0)
        return SelectionMemoryStatusV1::InvalidInput;
    if (PrivateResetConflicts(key, expectedTicket)) return SelectionMemoryStatusV1::NotHead;
    const auto rowIndex = FindRow(key);
    std::uint64_t occupancy = PrivateResetCount(key);
    if (rowIndex != UINT32_MAX) {
        const auto& row = rows_[rowIndex];
        occupancy += static_cast<std::uint64_t>(row.pendingCount) + row.preparedCount +
            row.scheduledCount + row.candidateCount;
        for (auto index = row.preparedHead; index != UINT32_MAX; index = deltas_[index].next)
            if (!deltas_[index].nativePendingPreparation || deltas_[index].ticket != expectedTicket)
                return SelectionMemoryStatusV1::NotHead;
        for (const auto& delta : deltas_)
            if (delta.used && delta.rollbackable && delta.row == rowIndex)
                return SelectionMemoryStatusV1::NotHead;
    }
    for (const auto& delta : deltas_)
        if ((delta.used || delta.prepared || delta.replacementCandidate) &&
            (delta.pendingReset ? SameKey(delta.request.key, key) :
                delta.row < rows_.size() && SameKey(rows_[delta.row].key, key)) &&
            delta.ticket == expectedTicket && delta.eventIndex == eventIndex)
            return SelectionMemoryStatusV1::InvalidInput;
    const auto limit = key.memoryScope == MemoryScopeV1::Global
        ? config_.maxPendingGlobalPerRow : config_.maxPendingEmitterPerRow;
    const auto slot = FindFreeDelta();
    if (occupancy >= limit || slot == UINT32_MAX) return SelectionMemoryStatusV1::Capacity;
    auto& delta = deltas_[slot];
    delta.prepared = true;
    delta.pendingReset = true;
    delta.kind = SelectionDeltaKindV1::Reset;
    delta.request.key = key;
    delta.ticket = expectedTicket;
    delta.eventIndex = eventIndex;
    delta.executeFrame = executeFrame;
    delta.status = SelectionMemoryStatusV1::Ok;
    out->owner_ = this;
    out->slot_ = slot;
    out->generation_ = delta.generation;
    out->lifetime_ = tableLifetime_;
    out->prepared_ = true;
    out->pending_ = true;
    AdvanceMutationEpoch();
    if (rowIndex != UINT32_MAX) RecomputePrepared(rowIndex);
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::AcceptPreparedPendingReset(
    PreparedSelectionResetV1* prepared, std::uint64_t ticket) noexcept {
    if (prepared == nullptr) return SelectionMemoryStatusV1::InvalidInput;
    bool noOp = false;
    const auto ready = CanAcceptPreparedPendingReset(*prepared, ticket, &noOp);
    if (ready != SelectionMemoryStatusV1::Ok) return ready;
    auto& delta = deltas_[prepared->slot_];
    const auto rowIndex = FindRow(delta.request.key);
    if (noOp) RecycleDelta(prepared->slot_);
    else {
        delta.prepared = false;
        delta.used = true;
        LinkScheduled(rows_[rowIndex], rowIndex, prepared->slot_);
    }
    if (noOp && rowIndex != UINT32_MAX) RecomputePrepared(rowIndex);
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->generation_ = 0;
    prepared->lifetime_ = 0;
    prepared->prepared_ = prepared->pending_ = prepared->idempotent_ = false;
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CanAcceptPreparedPendingReset(
    const PreparedSelectionResetV1& prepared, std::uint64_t ticket,
    bool* outIdempotent) const noexcept {
    if (outIdempotent != nullptr) *outIdempotent = false;
    if (prepared.owner_ != this || prepared.lifetime_ != tableLifetime_ || !prepared.prepared_ ||
        !prepared.pending_ || ticket == 0) return SelectionMemoryStatusV1::InvalidInput;
    if (prepared.slot_ >= deltas_.size()) return SelectionMemoryStatusV1::NotFound;
    const auto& delta = deltas_[prepared.slot_];
    if (!delta.prepared || !delta.pendingReset || delta.generation != prepared.generation_ ||
        delta.kind != SelectionDeltaKindV1::Reset || delta.ticket != ticket)
        return SelectionMemoryStatusV1::NotFound;
    const auto rowIndex = FindRow(delta.request.key);
    const bool noOp = rowIndex == UINT32_MAX || !PendingResetHasHistory(rows_[rowIndex], delta);
    if (outIdempotent != nullptr) *outIdempotent = noOp;
    if (noOp) return SelectionMemoryStatusV1::Ok;
    const auto& row = rows_[rowIndex];
    const auto limit = row.key.memoryScope == MemoryScopeV1::Global
        ? config_.maxPendingGlobalPerRow : config_.maxPendingEmitterPerRow;
    if (static_cast<std::uint64_t>(row.pendingCount) + row.preparedCount + row.scheduledCount +
        row.candidateCount + PrivateResetCount(row.key) > limit) return SelectionMemoryStatusV1::Capacity;
    const auto budget = 4u * static_cast<std::uint64_t>(deltas_.size()) + 4u;
    if (budget >= UINT64_MAX - mutationEpoch_ || deltas_.size() >= UINT64_MAX - row.state.resetEpoch)
        return SelectionMemoryStatusV1::RuntimeRecreateRequired;
    for (const auto& transaction : deltas_)
        if (transaction.used && transaction.rollbackable && transaction.row == rowIndex)
            return SelectionMemoryStatusV1::NotHead;
    for (const auto& batch : batches_)
        if (batch.used && budget >= UINT64_MAX - batch.revision)
            return SelectionMemoryStatusV1::RuntimeRecreateRequired;
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::PrepareReset(
    const SelectionMemoryKeyV1& key, std::uint64_t expectedTicket,
    std::uint32_t eventIndex, PreparedSelectionResetV1* out,
    std::uint64_t executeFrame) noexcept {
    if (out == nullptr || out->Valid() || !ValidKey(key) ||
        expectedTicket == 0) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    if (PrivateResetConflicts(key)) return SelectionMemoryStatusV1::NotHead;
    const std::uint32_t rowIndex = FindRow(key);
    if (rowIndex == UINT32_MAX || (!rows_[rowIndex].counted && !rows_[rowIndex].committed &&
        rows_[rowIndex].pendingCount == 0 && rows_[rowIndex].preparedCount == 0 && rows_[rowIndex].scheduledCount == 0)) {
        out->owner_ = this;
        out->lifetime_ = tableLifetime_;
        out->prepared_ = true;
        out->idempotent_ = true;
        return SelectionMemoryStatusV1::Ok;
    }
    Row& row = rows_[rowIndex];
    if (row.preparedHead != UINT32_MAX && deltas_[row.preparedHead].nativePendingPreparation)
        return SelectionMemoryStatusV1::InvalidInput;
    const std::uint32_t perRowLimit =
        key.memoryScope == MemoryScopeV1::Global
            ? config_.maxPendingGlobalPerRow
            : config_.maxPendingEmitterPerRow;
    const std::uint32_t deltaIndex = FindFreeDelta();
    if (row.pendingCount != 0 ||
        row.preparedCount + row.scheduledCount + row.candidateCount >=
            perRowLimit - row.pendingCount ||
        deltaIndex == UINT32_MAX) {
        return SelectionMemoryStatusV1::Capacity;
    }
    if (row.preparedTail != UINT32_MAX) {
        const DeltaRow& prior = deltas_[row.preparedTail];
        if (prior.status != SelectionMemoryStatusV1::Ok ||
            !Later(expectedTicket, eventIndex, prior.ticket,
                   prior.eventIndex)) {
            return prior.status == SelectionMemoryStatusV1::Ok
                       ? SelectionMemoryStatusV1::InvalidInput
                       : prior.status;
        }
    }
    DeltaRow& delta = deltas_[deltaIndex];
    delta.kind = SelectionDeltaKindV1::Reset;
    delta.row = rowIndex;
    delta.ticket = expectedTicket;
    delta.eventIndex = eventIndex;
    delta.executeFrame = executeFrame;
    const auto* before = &PreparedPredecessor(row, delta.executeFrame, delta.ticket, delta.eventIndex);
    SelectionPlayRequestV1 resetBoundary{};
    resetBoundary.executeFrame = delta.executeFrame;
    resetBoundary.ticket = delta.ticket;
    resetBoundary.eventIndex = delta.eventIndex;
    delta.status = PendingPredecessor(row, *before, resetBoundary, &before);
    if (delta.status == SelectionMemoryStatusV1::Ok) delta.status = ComputeReset(row, *before, delta.overlay);
    if (delta.status != SelectionMemoryStatusV1::Ok) {
        return delta.status;
    }
    if (executeFrame != 0) MarkScheduledFrontier(delta.overlay, delta);
    AdvanceMutationEpoch();
    delta.prepared = true;
    delta.previous = row.preparedTail;
    delta.next = UINT32_MAX;
    if (row.preparedTail != UINT32_MAX) {
        deltas_[row.preparedTail].next = deltaIndex;
    } else {
        row.preparedHead = deltaIndex;
    }
    row.preparedTail = deltaIndex;
    ++row.preparedCount;
    out->owner_ = this;
    out->slot_ = deltaIndex;
    out->generation_ = delta.generation;
    out->lifetime_ = tableLifetime_;
    out->prepared_ = true;
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CommitPreparedReset(
    PreparedSelectionResetV1* prepared, std::uint64_t ticket) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        prepared->lifetime_ != tableLifetime_ ||
        !prepared->prepared_ || ticket == 0) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    if (prepared->pending_) return SelectionMemoryStatusV1::InvalidInput;
    if (prepared->idempotent_) {
        prepared->owner_ = nullptr;
        prepared->lifetime_ = 0;
        prepared->prepared_ = false;
        prepared->idempotent_ = false;
        return SelectionMemoryStatusV1::Ok;
    }
    if (prepared->slot_ >= deltas_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    DeltaRow& delta = deltas_[prepared->slot_];
    if (!delta.prepared || delta.generation != prepared->generation_ ||
        delta.kind != SelectionDeltaKindV1::Reset ||
        delta.ticket != ticket || delta.row >= rows_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    const std::uint32_t rowIndex = delta.row;
    Row& row = rows_[rowIndex];
    if (row.pendingCount != 0 || row.preparedHead != prepared->slot_) {
        return SelectionMemoryStatusV1::NotHead;
    }
    if (delta.executeFrame != 0) {
        std::uint64_t followingCount = 0;
        for (auto index = row.scheduledHead; index != UINT32_MAX; index = deltas_[index].next) {
            const auto& following = deltas_[index];
            if (AppliesBefore(delta.executeFrame, delta.ticket, delta.eventIndex,
                    following.executeFrame, following.ticket, following.eventIndex)) ++followingCount;
        }
        if (followingCount > UINT64_MAX - delta.overlay.resetEpoch)
            return SelectionMemoryStatusV1::RuntimeRecreateRequired;
    }
    row.preparedHead = delta.next;
    if (delta.next != UINT32_MAX) {
        deltas_[delta.next].previous = UINT32_MAX;
    } else {
        row.preparedTail = UINT32_MAX;
    }
    --row.preparedCount;
    delta.prepared = false;
    delta.used = true;
    delta.previous = UINT32_MAX;
    delta.next = UINT32_MAX;
    SelectionMemoryStatusV1 status = SelectionMemoryStatusV1::Ok;
    if (delta.executeFrame == 0) {
        const SelectionDeltaRefV1 ref{prepared->slot_, delta.generation, tableLifetime_};
        LinkDelta(row, delta.row, prepared->slot_);
        status = CommitHead(ref);
    } else {
        LinkScheduled(row, delta.row, prepared->slot_);
    }
    if (status == SelectionMemoryStatusV1::Ok) {
        prepared->owner_ = nullptr;
        prepared->slot_ = UINT32_MAX;
        prepared->generation_ = 0;
        prepared->lifetime_ = 0;
        prepared->prepared_ = false;
    }
    return status;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CancelPreparedReset(
    PreparedSelectionResetV1* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        prepared->lifetime_ != tableLifetime_ ||
        !prepared->prepared_) {
        return SelectionMemoryStatusV1::NotFound;
    }
    if (prepared->pending_) {
        if (prepared->slot_ >= deltas_.size()) return SelectionMemoryStatusV1::NotFound;
        const auto& delta = deltas_[prepared->slot_];
        if (!delta.prepared || !delta.pendingReset || delta.generation != prepared->generation_)
            return SelectionMemoryStatusV1::NotFound;
        const auto rowIndex = FindRow(delta.request.key);
        RecycleDelta(prepared->slot_);
        if (rowIndex != UINT32_MAX) RecomputePrepared(rowIndex);
        prepared->owner_ = nullptr;
        prepared->slot_ = UINT32_MAX;
        prepared->generation_ = 0;
        prepared->lifetime_ = 0;
        prepared->prepared_ = prepared->pending_ = prepared->idempotent_ = false;
        return SelectionMemoryStatusV1::Ok;
    }
    if (prepared->idempotent_) {
        prepared->owner_ = nullptr;
        prepared->lifetime_ = 0;
        prepared->prepared_ = false;
        prepared->idempotent_ = false;
        return SelectionMemoryStatusV1::Ok;
    }
    if (prepared->slot_ >= deltas_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    DeltaRow& delta = deltas_[prepared->slot_];
    if (!delta.prepared || delta.generation != prepared->generation_ ||
        delta.kind != SelectionDeltaKindV1::Reset ||
        delta.row >= rows_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    const std::uint32_t rowIndex = delta.row;
    Row& row = rows_[rowIndex];
    if (delta.previous == UINT32_MAX) {
        row.preparedHead = delta.next;
    } else {
        deltas_[delta.previous].next = delta.next;
    }
    if (delta.next == UINT32_MAX) {
        row.preparedTail = delta.previous;
    } else {
        deltas_[delta.next].previous = delta.previous;
    }
    --row.preparedCount;
    RecycleDelta(prepared->slot_);
    RecomputePrepared(rowIndex);
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->generation_ = 0;
    prepared->lifetime_ = 0;
    prepared->prepared_ = false;
    if (row.preparedCount == 0 && row.pendingCount == 0 && row.scheduledCount == 0 &&
        row.candidateCount == 0 && !row.committed && !row.counted) {
        ReleaseRow(rowIndex);
    }
    return SelectionMemoryStatusV1::Ok;
}

void SelectionMemoryTableV1::UnlinkHead(Row& row,
                                        std::uint32_t deltaIndex) noexcept {
    AdvanceMutationEpoch();
    const std::uint32_t next = deltas_[deltaIndex].next;
    row.head = next;
    if (next != UINT32_MAX) {
        deltas_[next].previous = UINT32_MAX;
    } else {
        row.tail = UINT32_MAX;
    }
    --row.pendingCount;
    --pendingCount_;
}

void SelectionMemoryTableV1::UnlinkDelta(Row& row,
                                         std::uint32_t deltaIndex) noexcept {
    AdvanceMutationEpoch();
    DeltaRow& delta = deltas_[deltaIndex];
    if (delta.previous == UINT32_MAX) {
        row.head = delta.next;
    } else {
        deltas_[delta.previous].next = delta.next;
    }
    if (delta.next == UINT32_MAX) {
        row.tail = delta.previous;
    } else {
        deltas_[delta.next].previous = delta.previous;
    }
    --row.pendingCount;
    --pendingCount_;
}

void SelectionMemoryTableV1::RecycleDelta(
    std::uint32_t deltaIndex) noexcept {
    DeltaRow& delta = deltas_[deltaIndex];
    if (!delta.replacementCandidate && (delta.used || delta.prepared)) AdvanceMutationEpoch();
    delta.used = false;
    delta.prepared = false;
    delta.nativePendingPreparation = false;
    delta.pendingReset = false;
    delta.replacementCandidate = false;
    delta.row = UINT32_MAX;
    delta.next = UINT32_MAX;
    delta.previous = UINT32_MAX;
    delta.status = SelectionMemoryStatusV1::InvalidInput;
    delta.request = {};
    delta.path.clear();
    delta.ticket = 0;
    delta.eventIndex = 0;
    delta.executeFrame = 0;
    delta.selectedChildOrdinal = 0;
    delta.seedProbe = {};
    delta.overlay.resetEpoch = 0;
    delta.overlay.selectionOrdinal = 0;
    delta.overlay.sequenceCursor = 0;
    delta.overlay.sequenceStopped = false;
    delta.overlay.rotationCursor = 0;
    delta.overlay.recent.clear();
    delta.overlay.permutation.clear();
    ClearResetFrontiers(delta.overlay);
    delta.rollback.resetEpoch = 0;
    delta.rollback.selectionOrdinal = 0;
    delta.rollback.sequenceCursor = 0;
    delta.rollback.sequenceStopped = false;
    delta.rollback.rotationCursor = 0;
    delta.rollback.recent.clear();
    delta.rollback.permutation.clear();
    ClearResetFrontiers(delta.rollback);
    delta.rollbackCommitted = false;
    delta.rollbackCounted = false;
    delta.rollbackable = false;
    delta.batch = {};
    if (delta.generation == UINT32_MAX) {
        delta.quarantined = true;
        ++quarantinedDeltaCount_;
    } else {
        ++delta.generation;
    }
}

void SelectionMemoryTableV1::ReleaseRow(std::uint32_t rowIndex) noexcept {
    Row& row = rows_[rowIndex];
    if (!row.used) return;
    if (row.counted || row.committed || row.pendingCount != 0 || row.preparedCount != 0 ||
        row.scheduledCount != 0) AdvanceMutationEpoch();
    for (auto member = row.head; member != UINT32_MAX;) {
        const auto slot = deltas_[member].batch.slot;
        if (slot != UINT32_MAX && slot < batches_.size() && batches_[slot].used) {
            CancelBatchSlot(slot);
            if (!row.used) return;
            member = row.head;
        } else member = deltas_[member].next;
    }
    const bool counted = row.counted;
    std::uint32_t delta = row.head;
    while (delta != UINT32_MAX) {
        const std::uint32_t next = deltas_[delta].next;
        --pendingCount_;
        RecycleDelta(delta);
        delta = next;
    }
    delta = row.preparedHead;
    while (delta != UINT32_MAX) {
        const std::uint32_t next = deltas_[delta].next;
        RecycleDelta(delta);
        delta = next;
    }
    delta = row.scheduledHead;
    while (delta != UINT32_MAX) {
        const std::uint32_t next = deltas_[delta].next;
        RecycleDelta(delta);
        delta = next;
    }
    // Transactional owners are retained outside the pending/prepared chains.
    for (std::uint32_t index = 0; index < deltas_.size(); ++index)
        if (deltas_[index].row == rowIndex && (deltas_[index].used || deltas_[index].prepared))
            RecycleDelta(index);
    row.used = false;
    --occupiedRowCount_;
    row.committed = false;
    row.counted = false;
    row.key = {};
    row.state.resetEpoch = 0;
    row.state.selectionOrdinal = 0;
    row.state.sequenceCursor = 0;
    row.state.sequenceStopped = false;
    row.state.rotationCursor = 0;
    row.state.recent.clear();
    row.state.permutation.clear();
    ClearResetFrontiers(row.state);
    row.head = UINT32_MAX;
    row.tail = UINT32_MAX;
    row.pendingCount = 0;
    row.preparedHead = UINT32_MAX;
    row.preparedTail = UINT32_MAX;
    row.preparedCount = 0;
    row.scheduledHead = UINT32_MAX;
    row.scheduledTail = UINT32_MAX;
    row.scheduledCount = 0;
    row.candidateCount = 0;
    if (counted) {
        --rowCount_;
    }
}

void SelectionMemoryTableV1::Recompute(std::uint32_t rowIndex) noexcept {
    Row& row = rows_[rowIndex];
    const MemoryState* before = &row.state;
    auto predecessorStatus = SelectionMemoryStatusV1::Ok;
    for (std::uint32_t index = row.head; index != UINT32_MAX;
         index = deltas_[index].next) {
        DeltaRow& delta = deltas_[index];
        if (delta.nativePendingPreparation && predecessorStatus != SelectionMemoryStatusV1::Ok) {
            delta.status = predecessorStatus;
            delta.selectedChildOrdinal = 0;
            delta.seedProbe = {};
        } else if (delta.kind == SelectionDeltaKindV1::Reset) {
            delta.status = ComputeReset(row, *before, delta.overlay);
            delta.selectedChildOrdinal = 0;
        } else {
            const MemoryState* selectedBefore = before;
            delta.status = PendingPredecessor(row, *before, delta.request, &selectedBefore);
            if (delta.status == SelectionMemoryStatusV1::Ok)
                delta.status = ComputePlay(row, delta.request, *selectedBefore,
                    delta.overlay, &delta.selectedChildOrdinal, &delta.seedProbe);
        }
        before = delta.status == SelectionMemoryStatusV1::Ok
                     ? &delta.overlay
                     : before;
        predecessorStatus = delta.status;
        if (delta.batch.slot < batches_.size()) {
            auto& batch = batches_[delta.batch.slot];
            if (batch.used && batch.generation == delta.batch.generation && batch.revision != UINT64_MAX)
                ++batch.revision;
        }
    }
}

const SelectionMemoryTableV1::MemoryState& SelectionMemoryTableV1::PreparedPredecessor(
    const Row& row, std::uint64_t executeFrame, std::uint64_t ticket,
    std::uint32_t eventIndex, std::uint32_t stopBefore) const noexcept {
    const DeltaRow* latest = nullptr;
    const auto consider = [&](const DeltaRow& prior) noexcept {
        if (prior.pendingReset || prior.status != SelectionMemoryStatusV1::Ok ||
            !AppliesBefore(prior.executeFrame, prior.ticket, prior.eventIndex,
                executeFrame, ticket, eventIndex)) return;
        if (latest == nullptr || AppliesBefore(latest->executeFrame, latest->ticket,
                latest->eventIndex, prior.executeFrame, prior.ticket, prior.eventIndex))
            latest = &prior;
    };
    for (auto index = row.scheduledHead; index != UINT32_MAX; index = deltas_[index].next)
        consider(deltas_[index]);
    // Prepared requests are ticket ordered, but their frames can straddle a scheduled reset.
    for (auto index = row.preparedHead; index != UINT32_MAX && index != stopBefore;
         index = deltas_[index].next)
        consider(deltas_[index]);
    return latest == nullptr ? row.state : latest->overlay;
}

void SelectionMemoryTableV1::RecomputePrepared(
    std::uint32_t rowIndex) noexcept {
    Row& row = rows_[rowIndex];
    const MemoryState* nativeBefore = row.tail == UINT32_MAX ? &row.state : &deltas_[row.tail].overlay;
    auto nativeStatus = row.tail == UINT32_MAX ? SelectionMemoryStatusV1::Ok : deltas_[row.tail].status;
    for (std::uint32_t index = row.preparedHead; index != UINT32_MAX;
         index = deltas_[index].next) {
        DeltaRow& delta = deltas_[index];
        if (delta.nativePendingPreparation) {
            delta.status = nativeStatus;
            if (delta.status == SelectionMemoryStatusV1::Ok)
                delta.status = PendingPredecessor(row, *nativeBefore, delta.request, &nativeBefore, true);
            if (delta.status == SelectionMemoryStatusV1::Ok)
                delta.status = ComputePlay(row, delta.request, *nativeBefore, delta.overlay,
                    &delta.selectedChildOrdinal, &delta.seedProbe);
            if (delta.status == SelectionMemoryStatusV1::Ok) nativeBefore = &delta.overlay;
            else {
                delta.selectedChildOrdinal = 0;
                delta.seedProbe = {};
            }
            nativeStatus = delta.status;
            continue;
        }
        const auto* before = &PreparedPredecessor(row, delta.executeFrame, delta.ticket, delta.eventIndex, index);
        SelectionPlayRequestV1 boundary{};
        boundary.executeFrame = delta.executeFrame;
        boundary.ticket = delta.ticket;
        boundary.eventIndex = delta.eventIndex;
        delta.status = PendingPredecessor(row, *before, boundary, &before);
        if (delta.status != SelectionMemoryStatusV1::Ok) {
            delta.selectedChildOrdinal = 0;
            delta.seedProbe = {};
            continue;
        }
        if (delta.kind == SelectionDeltaKindV1::Reset) {
            delta.status = ComputeReset(row, *before, delta.overlay);
            delta.selectedChildOrdinal = 0;
            if (delta.status == SelectionMemoryStatusV1::Ok && delta.executeFrame != 0)
                MarkScheduledFrontier(delta.overlay, delta);
        } else {
            delta.status = ComputePlay(row, delta.request, *before,
                                       delta.overlay,
                                       &delta.selectedChildOrdinal,
                                       &delta.seedProbe);
        }
    }
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CommitHead(
    SelectionDeltaRefV1 ref) noexcept {
    DeltaRow* delta = ResolveDelta(ref);
    if (delta == nullptr) return SelectionMemoryStatusV1::NotFound;
    if (delta->batch.slot != UINT32_MAX) return SelectionMemoryStatusV1::InvalidInput;
    Row& row = rows_[delta->row];
    if (row.head != ref.slot) return SelectionMemoryStatusV1::NotHead;
    if (delta->status != SelectionMemoryStatusV1::Ok) return delta->status;
    if (!CopyState(delta->overlay, row.state)) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    row.committed = true;
    const auto rowIndex = delta->row;
    bool nativeRemoved = false;
    if (delta->kind == SelectionDeltaKindV1::Play)
        nativeRemoved = DropScheduledThrough(row, delta->executeFrame, delta->ticket, delta->eventIndex);
    else {
        // A legacy Reset can consume native operations too; legacy scheduled removal keeps its existing boundary.
        for (auto index = row.scheduledHead; index != UINT32_MAX;) {
            const auto next = deltas_[index].next;
            if (deltas_[index].pendingReset && ResetConsumed(row.state, deltas_[index])) {
                UnlinkScheduled(row, index);
                RecycleDelta(index);
                nativeRemoved = true;
            }
            index = next;
        }
    }
    UnlinkHead(row, ref.slot);
    RecycleDelta(ref.slot);
    if (nativeRemoved) RefreshResetJournal(rowIndex);
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::FailHead(
    SelectionDeltaRefV1 ref) noexcept {
    DeltaRow* delta = ResolveDelta(ref);
    if (delta == nullptr) return SelectionMemoryStatusV1::NotFound;
    if (delta->batch.slot != UINT32_MAX) return SelectionMemoryStatusV1::InvalidInput;
    const std::uint32_t rowIndex = delta->row;
    Row& row = rows_[rowIndex];
    if (row.head != ref.slot) return SelectionMemoryStatusV1::NotHead;
    UnlinkHead(row, ref.slot);
    RecycleDelta(ref.slot);
    if (row.pendingCount == 0 && !row.committed && row.counted) {
        row.counted = false;
        --rowCount_;
    }
    if (row.pendingCount == 0 && row.preparedCount == 0 && row.scheduledCount == 0 &&
        row.candidateCount == 0 && !row.committed) {
        ReleaseRow(rowIndex);
    } else {
        Recompute(rowIndex);
        RecomputePrepared(rowIndex);
    }
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CancelDelta(
    SelectionDeltaRefV1 ref) noexcept {
    DeltaRow* delta = ResolveDelta(ref);
    if (delta == nullptr) return SelectionMemoryStatusV1::NotFound;
    if (delta->batch.slot != UINT32_MAX) return SelectionMemoryStatusV1::InvalidInput;
    const std::uint32_t rowIndex = delta->row;
    Row& row = rows_[rowIndex];
    UnlinkDelta(row, ref.slot);
    RecycleDelta(ref.slot);
    if (row.pendingCount == 0 && !row.committed && row.counted) {
        row.counted = false;
        --rowCount_;
    }
    if (row.pendingCount == 0 && row.preparedCount == 0 && row.scheduledCount == 0 &&
        row.candidateCount == 0 && !row.committed) {
        ReleaseRow(rowIndex);
    } else {
        Recompute(rowIndex);
        RecomputePrepared(rowIndex);
    }
    return SelectionMemoryStatusV1::Ok;
}

const SelectionMemoryTableV1::DeltaRow*
SelectionMemoryTableV1::ResolveDelta(SelectionDeltaRefV1 ref) const noexcept {
    if (!ref || ref.lifetime != tableLifetime_ || ref.slot >= deltas_.size()) return nullptr;
    const DeltaRow& delta = deltas_[ref.slot];
    return delta.used && !delta.quarantined &&
                   delta.generation == ref.generation
               ? &delta
               : nullptr;
}

SelectionMemoryTableV1::DeltaRow* SelectionMemoryTableV1::ResolveDelta(
    SelectionDeltaRefV1 ref) noexcept {
    return const_cast<DeltaRow*>(
        static_cast<const SelectionMemoryTableV1*>(this)->ResolveDelta(ref));
}

SelectionDeltaViewV1 SelectionMemoryTableV1::Delta(
    SelectionDeltaRefV1 ref) const noexcept {
    SelectionDeltaViewV1 view{};
    const DeltaRow* delta = ResolveDelta(ref);
    if (delta == nullptr) return view;
    const Row& row = rows_[delta->row];
    view.live = true;
    view.head = row.head == ref.slot;
    view.kind = delta->kind;
    view.status = delta->status;
    view.key = row.key;
    view.ticket = delta->ticket;
    view.eventIndex = delta->eventIndex;
    view.selectedChildOrdinal = delta->selectedChildOrdinal;
    view.resetEpochAfter = delta->overlay.resetEpoch;
    view.selectionOrdinalAfter = delta->overlay.selectionOrdinal;
    return view;
}

SelectionMemorySnapshotV1 SelectionMemoryTableV1::Snapshot(
    const SelectionMemoryKeyV1& key) const noexcept {
    SelectionMemorySnapshotV1 snapshot{};
    const std::uint32_t index = FindRow(key);
    if (index == UINT32_MAX) return snapshot;
    const Row& row = rows_[index];
    if (!row.counted) return snapshot;
    snapshot.found = true;
    snapshot.resetEpoch = row.state.resetEpoch;
    snapshot.selectionOrdinal = row.state.selectionOrdinal;
    snapshot.sequenceCursor = row.state.sequenceCursor;
    snapshot.sequenceStopped = row.state.sequenceStopped;
    snapshot.rotationCursor = row.state.rotationCursor;
    snapshot.recentCount = static_cast<std::uint32_t>(row.state.recent.size());
    snapshot.permutationCount =
        static_cast<std::uint32_t>(row.state.permutation.size());
    snapshot.pendingCount = row.pendingCount;
    return snapshot;
}

std::uint32_t SelectionMemoryTableV1::CopyRecent(
    const SelectionMemoryKeyV1& key, std::uint32_t* output,
    std::uint32_t capacity) const noexcept {
    const std::uint32_t index = FindRow(key);
    if (index == UINT32_MAX || !rows_[index].counted) return 0;
    const auto& values = rows_[index].state.recent;
    if (values.size() > capacity || (!values.empty() && output == nullptr)) {
        return static_cast<std::uint32_t>(values.size());
    }
    std::copy(values.begin(), values.end(), output);
    return static_cast<std::uint32_t>(values.size());
}

std::uint32_t SelectionMemoryTableV1::CopyPermutation(
    const SelectionMemoryKeyV1& key, std::uint32_t* output,
    std::uint32_t capacity) const noexcept {
    const std::uint32_t index = FindRow(key);
    if (index == UINT32_MAX || !rows_[index].counted) return 0;
    const auto& values = rows_[index].state.permutation;
    if (values.size() > capacity || (!values.empty() && output == nullptr)) {
        return static_cast<std::uint32_t>(values.size());
    }
    std::copy(values.begin(), values.end(), output);
    return static_cast<std::uint32_t>(values.size());
}

void SelectionMemoryTableV1::RetireEmitter(
    std::uint32_t emitterSlot, std::uint32_t emitterGeneration) noexcept {
    for (std::uint32_t index = 0; index < deltas_.size(); ++index) {
        const auto& delta = deltas_[index];
        if (delta.prepared && delta.pendingReset && delta.request.key.memoryScope == MemoryScopeV1::Emitter &&
            delta.request.key.emitterSlot == emitterSlot && delta.request.key.emitterGeneration == emitterGeneration)
            RecycleDelta(index);
    }
    for (std::uint32_t slot = 0; slot < batches_.size(); ++slot) {
        if (!batches_[slot].used) continue;
        bool affected = batches_[slot].producer && batches_[slot].identity.emitterSlot == emitterSlot &&
            batches_[slot].identity.emitterGeneration == emitterGeneration;
        for (auto member = batches_[slot].head; member != UINT32_MAX; member = deltas_[member].batch.next) {
            const auto& key = rows_[deltas_[member].row].key;
            affected |= key.memoryScope == MemoryScopeV1::Emitter &&
                key.emitterSlot == emitterSlot && key.emitterGeneration == emitterGeneration;
        }
        if (affected) CancelBatchSlot(slot);
    }
    for (std::uint32_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (row.used && row.key.memoryScope == MemoryScopeV1::Emitter &&
            row.key.emitterSlot == emitterSlot &&
            row.key.emitterGeneration == emitterGeneration) {
            ReleaseRow(i);
        }
    }
}

void SelectionMemoryTableV1::RetireWorld(WorldGeneration world) noexcept {
    for (std::uint32_t index = 0; index < deltas_.size(); ++index)
        if (deltas_[index].prepared && deltas_[index].pendingReset && deltas_[index].request.key.world == world)
            RecycleDelta(index);
    for (std::uint32_t slot = 0; slot < batches_.size(); ++slot) {
        if (batches_[slot].used && (batches_[slot].producer ? batches_[slot].identity.world == world :
            rows_[deltas_[batches_[slot].head].row].key.world == world))
            CancelBatchSlot(slot);
    }
    for (std::uint32_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].used && rows_[i].key.world == world) ReleaseRow(i);
    }
}

void SelectionMemoryTableV1::RetireUnusedWorldsBefore(WorldGeneration current,
    bool (*hasReaders)(const void*, WorldGeneration) noexcept,
    const void* context) noexcept {
    if (current == 0 || hasReaders == nullptr || occupiedRowCount_ == 0) return;
    for (std::uint32_t i = 0; i < rows_.size(); ++i) {
        const auto& row = rows_[i];
        if (!row.used || row.key.world >= current || row.pendingCount != 0 ||
            row.preparedCount != 0 || row.scheduledCount != 0 || row.candidateCount != 0 ||
            PrivateResetCount(row.key) != 0 ||
            hasReaders(context, row.key.world)) continue;
        if (std::any_of(batches_.begin(), batches_.end(), [&](const auto& batch) {
                return batch.used && batch.producer && batch.identity.world == row.key.world;
            })) continue;
        // Transactional commits retain rollback ownership outside the row lists.
        if (std::any_of(deltas_.begin(), deltas_.end(), [i](const auto& delta) {
                return delta.row == i && (delta.used || delta.prepared);
            })) continue;
        ReleaseRow(i);
    }
}

std::uint32_t SelectionMemoryTableV1::FreeDeltaCount() const noexcept {
    std::uint32_t count = 0;
    for (const DeltaRow& delta : deltas_) {
        count += !delta.used && !delta.prepared && !delta.replacementCandidate && !delta.quarantined ? 1u
                                                                      : 0u;
    }
    return count;
}

bool SelectionMemoryTableV1::SetDeltaGenerationForTests(
    std::uint32_t slot, std::uint32_t generation) noexcept {
    if (slot >= deltas_.size() || deltas_[slot].used ||
        deltas_[slot].prepared || deltas_[slot].replacementCandidate ||
        deltas_[slot].quarantined || generation == 0) {
        return false;
    }
    deltas_[slot].generation = generation;
    AdvanceMutationEpoch();
    return true;
}

}
