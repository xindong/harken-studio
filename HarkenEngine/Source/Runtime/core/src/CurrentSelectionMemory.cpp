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
    if (config.maxChildren == 0 ||
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
        std::vector<SelectionChildV1> childScratch;
        std::vector<std::uint32_t> remainingScratch;
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
        childScratch_.swap(childScratch);
        remainingScratch_.swap(remainingScratch);
    } catch (...) {
        return false;
    }
    config_ = config;
    rowCount_ = 0;
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
        if (!deltas_[i].used && !deltas_[i].prepared &&
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

void SelectionMemoryTableV1::LinkDelta(Row& row, std::uint32_t rowIndex,
                                       std::uint32_t deltaIndex) noexcept {
    DeltaRow& delta = deltas_[deltaIndex];
    delta.row = rowIndex;
    delta.previous = row.tail;
    delta.next = UINT32_MAX;
    if (row.tail != UINT32_MAX) deltas_[row.tail].next = deltaIndex;
    if (row.head == UINT32_MAX) row.head = deltaIndex;
    row.tail = deltaIndex;
    ++row.pendingCount;
    ++pendingCount_;
}

void SelectionMemoryTableV1::LinkScheduled(Row& row, std::uint32_t rowIndex,
                                           std::uint32_t deltaIndex) noexcept {
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
}

void SelectionMemoryTableV1::UnlinkScheduledHead(
    Row& row, std::uint32_t deltaIndex) noexcept {
    const std::uint32_t next = deltas_[deltaIndex].next;
    row.scheduledHead = next;
    if (next != UINT32_MAX) {
        deltas_[next].previous = UINT32_MAX;
    } else {
        row.scheduledTail = UINT32_MAX;
    }
    --row.scheduledCount;
}

void SelectionMemoryTableV1::DropScheduledThrough(
    Row& row, std::uint64_t executeFrame, std::uint64_t ticket,
    std::uint32_t eventIndex) noexcept {
    std::uint32_t index = row.scheduledHead;
    while (index != UINT32_MAX) {
        const std::uint32_t next = deltas_[index].next;
        const DeltaRow& delta = deltas_[index];
        if (AppliesBefore(delta.executeFrame, delta.ticket, delta.eventIndex,
                          executeFrame, ticket, eventIndex) ||
            (delta.executeFrame == executeFrame && delta.ticket == ticket &&
             delta.eventIndex == eventIndex)) {
            if (delta.previous == UINT32_MAX) {
                row.scheduledHead = next;
            } else {
                deltas_[delta.previous].next = next;
            }
            if (next == UINT32_MAX) {
                row.scheduledTail = delta.previous;
            } else {
                deltas_[next].previous = delta.previous;
            }
            --row.scheduledCount;
            RecycleDelta(index);
        }
        index = next;
    }
}

void SelectionMemoryTableV1::ApplyDue(std::uint64_t now) noexcept {
    for (std::uint32_t rowIndex = 0; rowIndex < rows_.size(); ++rowIndex) {
        Row& row = rows_[rowIndex];
        if (!row.used) {
            continue;
        }
        while (row.scheduledHead != UINT32_MAX) {
            DeltaRow& delta = deltas_[row.scheduledHead];
            if (delta.executeFrame > now) {
                break;
            }
            if (delta.status == SelectionMemoryStatusV1::Ok &&
                !CopyState(delta.overlay, row.state)) {
                break;
            }
            row.committed = true;
            if (!row.counted) {
                row.counted = true;
                ++rowCount_;
            }
            const std::uint32_t slot = row.scheduledHead;
            UnlinkScheduledHead(row, slot);
            RecycleDelta(slot);
        }
    }
}

SelectionAppendResultV1 SelectionMemoryTableV1::AppendPlay(
    const SelectionPlayRequestV1& request) noexcept {
    SelectionAppendResultV1 result{};
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
    if (row.pendingCount >= perRowLimit ||
        (!newRow &&
         !PendingOrderValid(row, request.ticket, request.eventIndex)) ||
        (newRow && request.ticket == 0)) {
        result.status = row.pendingCount >= perRowLimit
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
    delta.status = ComputePlay(*computationRow, delta.request, *actualBefore,
                               delta.overlay,
                               &delta.selectedChildOrdinal, &delta.seedProbe);
    if (delta.status != SelectionMemoryStatusV1::Ok) {
        result.status = delta.status;
        delta.path.clear();
        return result;
    }
    if (newRow) {
        row.used = true;
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
    result.delta = {deltaIndex, delta.generation};
    return result;
}

SelectionAppendResultV1 SelectionMemoryTableV1::AppendReset(
    const SelectionMemoryKeyV1& key, std::uint64_t ticket,
    std::uint32_t eventIndex) noexcept {
    SelectionAppendResultV1 result{};
    if (!ValidKey(key) || ticket == 0) return result;
    const std::uint32_t rowIndex = FindRow(key);
    if (rowIndex == UINT32_MAX) {
        result.status = SelectionMemoryStatusV1::Ok;
        result.idempotent = true;
        return result;
    }
    Row& row = rows_[rowIndex];
    if (row.preparedCount != 0) {
        result.status = SelectionMemoryStatusV1::Capacity;
        return result;
    }
    const std::uint32_t perRowLimit =
        key.memoryScope == MemoryScopeV1::Global
            ? config_.maxPendingGlobalPerRow
            : config_.maxPendingEmitterPerRow;
    const std::uint32_t deltaIndex = FindFreeDelta();
    if (row.pendingCount >= perRowLimit || deltaIndex == UINT32_MAX) {
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
    result.delta = {deltaIndex, delta.generation};
    return result;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::PreparePlay(
    const SelectionPlayRequestV1& request,
    PreparedSelectionPlayV1* out,
    std::uint32_t* outSelectedChildOrdinal) noexcept {
    if (outSelectedChildOrdinal != nullptr) {
        *outSelectedChildOrdinal = 0;
    }
    if (out == nullptr || out->Valid() || outSelectedChildOrdinal == nullptr ||
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
    const std::uint32_t deltaIndex = FindFreeDelta();
    if (rowIndex == UINT32_MAX || deltaIndex == UINT32_MAX) {
        return SelectionMemoryStatusV1::Capacity;
    }
    Row& row = rows_[rowIndex];
    const std::uint32_t perRowLimit =
        request.key.memoryScope == MemoryScopeV1::Global
            ? config_.maxPendingGlobalPerRow
            : config_.maxPendingEmitterPerRow;
    if (row.pendingCount != 0 ||
        row.preparedCount + row.scheduledCount >= perRowLimit ||
        (!newRow && row.preparedTail != UINT32_MAX &&
         !Later(request.ticket, request.eventIndex,
                deltas_[row.preparedTail].ticket,
                deltas_[row.preparedTail].eventIndex))) {
        return row.pendingCount != 0 ||
                       row.preparedCount + row.scheduledCount >= perRowLimit
                   ? SelectionMemoryStatusV1::Capacity
                   : SelectionMemoryStatusV1::InvalidInput;
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
    const MemoryState* before = &row.state;
    for (std::uint32_t index = row.scheduledHead; index != UINT32_MAX;
         index = deltas_[index].next) {
        const DeltaRow& prior = deltas_[index];
        if (AppliesBefore(prior.executeFrame, prior.ticket, prior.eventIndex,
                          request.executeFrame, request.ticket,
                          request.eventIndex)) {
            before = &prior.overlay;
        }
    }
    for (std::uint32_t index = row.preparedHead; index != UINT32_MAX;
         index = deltas_[index].next) {
        const DeltaRow& prior = deltas_[index];
        if (AppliesBefore(prior.executeFrame, prior.ticket, prior.eventIndex,
                          request.executeFrame, request.ticket,
                          request.eventIndex)) {
            before = &prior.overlay;
        }
    }
    delta.status = ComputePlay(row, delta.request, *before,
                               delta.overlay,
                               &delta.selectedChildOrdinal, &delta.seedProbe);
    if (delta.status != SelectionMemoryStatusV1::Ok) {
        const SelectionMemoryStatusV1 status = delta.status;
        delta.request = {};
        delta.path.clear();
        delta.ticket = 0;
        delta.eventIndex = 0;
        delta.selectedChildOrdinal = 0;
        delta.seedProbe = {};
        return status;
    }

    if (newRow) {
        row.used = true;
        row.committed = false;
        row.counted = false;
    }
    delta.row = rowIndex;
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
    out->prepared_ = true;
    *outSelectedChildOrdinal = delta.selectedChildOrdinal;
    return SelectionMemoryStatusV1::Ok;
}

SelectionSeedProbeCopyV1 SelectionMemoryTableV1::PreparedPlaySeedProbe(
    const PreparedSelectionPlayV1& prepared) const noexcept {
    if (prepared.owner_ != this || !prepared.prepared_ ||
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
    const SelectionDeltaRefV1 ref{prepared->slot_, delta.generation};
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
    const SelectionDeltaRefV1 ref{prepared->slot_, delta.generation};
    LinkDelta(row, rowIndex, prepared->slot_);
    if (!row.counted) {
        row.counted = true;
        ++rowCount_;
    }
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->generation_ = 0;
    prepared->prepared_ = false;
    *out = ref;
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1
SelectionMemoryTableV1::CommitPreparedPlayTransactional(
    PreparedSelectionPlayV1* prepared,
    std::uint64_t ticket) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
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
    DropScheduledThrough(row, playFrame, playTicket, playEvent);
    RecycleDelta(committed->slot_);
    committed->owner_ = nullptr;
    committed->slot_ = UINT32_MAX;
    committed->generation_ = 0;
    committed->committed_ = false;
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::RollbackCommittedPlay(
    PreparedSelectionPlayV1* committed) noexcept {
    if (committed == nullptr || committed->owner_ != this ||
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
    committed->committed_ = false;
    RecomputePrepared(rowIndex);
    if (row.preparedCount == 0 && row.pendingCount == 0 &&
        !row.committed && !row.counted) {
        ReleaseRow(rowIndex);
    }
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CancelPreparedPlay(
    PreparedSelectionPlayV1* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
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
    delta.prepared = false;
    delta.row = UINT32_MAX;
    delta.next = UINT32_MAX;
    delta.previous = UINT32_MAX;
    delta.status = SelectionMemoryStatusV1::InvalidInput;
    delta.request = {};
    delta.path.clear();
    delta.ticket = 0;
    delta.eventIndex = 0;
    delta.selectedChildOrdinal = 0;
    delta.seedProbe = {};
    delta.overlay.resetEpoch = 0;
    delta.overlay.selectionOrdinal = 0;
    delta.overlay.sequenceCursor = 0;
    delta.overlay.sequenceStopped = false;
    delta.overlay.rotationCursor = 0;
    delta.overlay.recent.clear();
    delta.overlay.permutation.clear();
    delta.rollback.resetEpoch = 0;
    delta.rollback.selectionOrdinal = 0;
    delta.rollback.sequenceCursor = 0;
    delta.rollback.sequenceStopped = false;
    delta.rollback.rotationCursor = 0;
    delta.rollback.recent.clear();
    delta.rollback.permutation.clear();
    delta.rollbackCommitted = false;
    delta.rollbackCounted = false;
    delta.rollbackable = false;
    RecomputePrepared(rowIndex);
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->generation_ = 0;
    prepared->prepared_ = false;
    if (row.preparedCount == 0 && row.pendingCount == 0 &&
        !row.committed && !row.counted) {
        ReleaseRow(rowIndex);
    }
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
    const std::uint32_t rowIndex = FindRow(key);
    if (rowIndex == UINT32_MAX) {
        out->owner_ = this;
        out->prepared_ = true;
        out->idempotent_ = true;
        return SelectionMemoryStatusV1::Ok;
    }
    Row& row = rows_[rowIndex];
    const std::uint32_t perRowLimit =
        key.memoryScope == MemoryScopeV1::Global
            ? config_.maxPendingGlobalPerRow
            : config_.maxPendingEmitterPerRow;
    const std::uint32_t deltaIndex = FindFreeDelta();
    if (row.pendingCount != 0 ||
        row.preparedCount + row.scheduledCount >=
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
    const MemoryState& before =
        row.preparedTail == UINT32_MAX
            ? row.state
            : deltas_[row.preparedTail].overlay;
    delta.status = ComputeReset(row, before, delta.overlay);
    if (delta.status != SelectionMemoryStatusV1::Ok) {
        return delta.status;
    }
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
    out->prepared_ = true;
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CommitPreparedReset(
    PreparedSelectionResetV1* prepared, std::uint64_t ticket) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_ || ticket == 0) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    if (prepared->idempotent_) {
        prepared->owner_ = nullptr;
        prepared->prepared_ = false;
        prepared->idempotent_ = false;
        return SelectionMemoryStatusV1::Ok;
    }
    if (prepared->slot_ >= deltas_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    DeltaRow& delta = deltas_[prepared->slot_];
    if (!delta.prepared || delta.generation != prepared->generation_ ||
        delta.ticket != ticket || delta.row >= rows_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
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
    SelectionMemoryStatusV1 status = SelectionMemoryStatusV1::Ok;
    if (delta.executeFrame == 0) {
        const SelectionDeltaRefV1 ref{prepared->slot_, delta.generation};
        LinkDelta(row, delta.row, prepared->slot_);
        status = CommitHead(ref);
    } else {
        LinkScheduled(row, delta.row, prepared->slot_);
    }
    if (status == SelectionMemoryStatusV1::Ok) {
        prepared->owner_ = nullptr;
        prepared->slot_ = UINT32_MAX;
        prepared->generation_ = 0;
        prepared->prepared_ = false;
    }
    return status;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CancelPreparedReset(
    PreparedSelectionResetV1* prepared) noexcept {
    if (prepared == nullptr || prepared->owner_ != this ||
        !prepared->prepared_) {
        return SelectionMemoryStatusV1::NotFound;
    }
    if (prepared->idempotent_) {
        prepared->owner_ = nullptr;
        prepared->prepared_ = false;
        prepared->idempotent_ = false;
        return SelectionMemoryStatusV1::Ok;
    }
    if (prepared->slot_ >= deltas_.size()) {
        return SelectionMemoryStatusV1::NotFound;
    }
    DeltaRow& delta = deltas_[prepared->slot_];
    if (!delta.prepared || delta.generation != prepared->generation_ ||
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
    delta.prepared = false;
    delta.row = UINT32_MAX;
    delta.next = UINT32_MAX;
    delta.previous = UINT32_MAX;
    delta.status = SelectionMemoryStatusV1::InvalidInput;
    delta.ticket = 0;
    delta.eventIndex = 0;
    delta.overlay.resetEpoch = 0;
    delta.overlay.selectionOrdinal = 0;
    delta.overlay.sequenceCursor = 0;
    delta.overlay.sequenceStopped = false;
    delta.overlay.rotationCursor = 0;
    delta.overlay.recent.clear();
    delta.overlay.permutation.clear();
    RecomputePrepared(rowIndex);
    prepared->owner_ = nullptr;
    prepared->slot_ = UINT32_MAX;
    prepared->generation_ = 0;
    prepared->prepared_ = false;
    if (row.preparedCount == 0 && row.pendingCount == 0 &&
        !row.committed && !row.counted) {
        ReleaseRow(rowIndex);
    }
    return SelectionMemoryStatusV1::Ok;
}

void SelectionMemoryTableV1::UnlinkHead(Row& row,
                                        std::uint32_t deltaIndex) noexcept {
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
    delta.used = false;
    delta.prepared = false;
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
    delta.rollback.resetEpoch = 0;
    delta.rollback.selectionOrdinal = 0;
    delta.rollback.sequenceCursor = 0;
    delta.rollback.sequenceStopped = false;
    delta.rollback.rotationCursor = 0;
    delta.rollback.recent.clear();
    delta.rollback.permutation.clear();
    delta.rollbackCommitted = false;
    delta.rollbackCounted = false;
    delta.rollbackable = false;
    if (delta.generation == UINT32_MAX) {
        delta.quarantined = true;
        ++quarantinedDeltaCount_;
    } else {
        ++delta.generation;
    }
}

void SelectionMemoryTableV1::ReleaseRow(std::uint32_t rowIndex) noexcept {
    Row& row = rows_[rowIndex];
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
        deltas_[delta].prepared = false;
        deltas_[delta].row = UINT32_MAX;
        deltas_[delta].next = UINT32_MAX;
        deltas_[delta].previous = UINT32_MAX;
        delta = next;
    }
    delta = row.scheduledHead;
    while (delta != UINT32_MAX) {
        const std::uint32_t next = deltas_[delta].next;
        RecycleDelta(delta);
        delta = next;
    }
    row.used = false;
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
    row.head = UINT32_MAX;
    row.tail = UINT32_MAX;
    row.pendingCount = 0;
    row.preparedHead = UINT32_MAX;
    row.preparedTail = UINT32_MAX;
    row.preparedCount = 0;
    row.scheduledHead = UINT32_MAX;
    row.scheduledTail = UINT32_MAX;
    row.scheduledCount = 0;
    if (counted) {
        --rowCount_;
    }
}

void SelectionMemoryTableV1::Recompute(std::uint32_t rowIndex) noexcept {
    Row& row = rows_[rowIndex];
    const MemoryState* before = &row.state;
    for (std::uint32_t index = row.head; index != UINT32_MAX;
         index = deltas_[index].next) {
        DeltaRow& delta = deltas_[index];
        if (delta.kind == SelectionDeltaKindV1::Reset) {
            delta.status = ComputeReset(row, *before, delta.overlay);
            delta.selectedChildOrdinal = 0;
        } else {
            delta.status = ComputePlay(row, delta.request, *before,
                                       delta.overlay,
                                       &delta.selectedChildOrdinal,
                                       &delta.seedProbe);
        }
        before = delta.status == SelectionMemoryStatusV1::Ok
                     ? &delta.overlay
                     : before;
    }
}

void SelectionMemoryTableV1::RecomputePrepared(
    std::uint32_t rowIndex) noexcept {
    Row& row = rows_[rowIndex];
    const MemoryState* before = &row.state;
    for (std::uint32_t index = row.preparedHead; index != UINT32_MAX;
         index = deltas_[index].next) {
        DeltaRow& delta = deltas_[index];
        if (delta.kind == SelectionDeltaKindV1::Reset) {
            delta.status = ComputeReset(row, *before, delta.overlay);
            delta.selectedChildOrdinal = 0;
        } else {
            delta.status = ComputePlay(row, delta.request, *before,
                                       delta.overlay,
                                       &delta.selectedChildOrdinal,
                                       &delta.seedProbe);
        }
        before = delta.status == SelectionMemoryStatusV1::Ok
                     ? &delta.overlay
                     : before;
    }
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CommitHead(
    SelectionDeltaRefV1 ref) noexcept {
    DeltaRow* delta = ResolveDelta(ref);
    if (delta == nullptr) return SelectionMemoryStatusV1::NotFound;
    Row& row = rows_[delta->row];
    if (row.head != ref.slot) return SelectionMemoryStatusV1::NotHead;
    if (delta->status != SelectionMemoryStatusV1::Ok) return delta->status;
    if (!CopyState(delta->overlay, row.state)) {
        return SelectionMemoryStatusV1::InvalidInput;
    }
    row.committed = true;
    UnlinkHead(row, ref.slot);
    RecycleDelta(ref.slot);
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::FailHead(
    SelectionDeltaRefV1 ref) noexcept {
    DeltaRow* delta = ResolveDelta(ref);
    if (delta == nullptr) return SelectionMemoryStatusV1::NotFound;
    const std::uint32_t rowIndex = delta->row;
    Row& row = rows_[rowIndex];
    if (row.head != ref.slot) return SelectionMemoryStatusV1::NotHead;
    UnlinkHead(row, ref.slot);
    RecycleDelta(ref.slot);
    if (row.pendingCount == 0 && !row.committed) {
        ReleaseRow(rowIndex);
    } else {
        Recompute(rowIndex);
    }
    return SelectionMemoryStatusV1::Ok;
}

SelectionMemoryStatusV1 SelectionMemoryTableV1::CancelDelta(
    SelectionDeltaRefV1 ref) noexcept {
    DeltaRow* delta = ResolveDelta(ref);
    if (delta == nullptr) return SelectionMemoryStatusV1::NotFound;
    const std::uint32_t rowIndex = delta->row;
    Row& row = rows_[rowIndex];
    UnlinkDelta(row, ref.slot);
    RecycleDelta(ref.slot);
    if (row.pendingCount == 0 && !row.committed) {
        ReleaseRow(rowIndex);
    } else {
        Recompute(rowIndex);
    }
    return SelectionMemoryStatusV1::Ok;
}

const SelectionMemoryTableV1::DeltaRow*
SelectionMemoryTableV1::ResolveDelta(SelectionDeltaRefV1 ref) const noexcept {
    if (!ref || ref.slot >= deltas_.size()) return nullptr;
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
    for (std::uint32_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].used && rows_[i].key.world == world) ReleaseRow(i);
    }
}

void SelectionMemoryTableV1::RetireUnusedWorldsBefore(WorldGeneration current,
    bool (*hasReaders)(const void*, WorldGeneration) noexcept,
    const void* context) noexcept {
    if (current == 0 || hasReaders == nullptr) return;
    for (std::uint32_t i = 0; i < rows_.size(); ++i) {
        const auto& row = rows_[i];
        if (!row.used || row.key.world >= current || row.pendingCount != 0 ||
            row.preparedCount != 0 || row.scheduledCount != 0 ||
            hasReaders(context, row.key.world)) continue;
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
        count += !delta.used && !delta.prepared && !delta.quarantined ? 1u
                                                                      : 0u;
    }
    return count;
}

bool SelectionMemoryTableV1::SetDeltaGenerationForTests(
    std::uint32_t slot, std::uint32_t generation) noexcept {
    if (slot >= deltas_.size() || deltas_[slot].used ||
        deltas_[slot].prepared ||
        deltas_[slot].quarantined || generation == 0) {
        return false;
    }
    deltas_[slot].generation = generation;
    return true;
}

}
