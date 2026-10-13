#include "harken/core/TimelineExecutor.h"
#include "harken/core/VariableControlRuntime.h"

#include <algorithm>

namespace harken {
namespace {

void HashByte(std::uint64_t* hash, std::uint8_t value) noexcept {
    *hash ^= value;
    *hash *= 1099511628211ull;
}

template <typename T>
void HashLE(std::uint64_t* hash, T value) noexcept {
    for (std::uint32_t i = 0; i < sizeof(T); ++i) {
        HashByte(hash, static_cast<std::uint8_t>(value >> (8u * i)));
    }
}

void HashLE(std::uint64_t* hash, VnObjectId value) noexcept {
    for (const auto byte : ObjectIdBytes(value)) HashByte(hash, byte);
}

bool SameTarget(const TimelineSemanticTarget& a,
                const TimelineSemanticTarget& b) noexcept {
    return a.programSemanticFingerprint == b.programSemanticFingerprint &&
           a.clipGuidHigh == b.clipGuidHigh &&
           a.clipGuidLow == b.clipGuidLow &&
           a.subjectGuidHigh == b.subjectGuidHigh &&
           a.subjectGuidLow == b.subjectGuidLow &&
           a.trackId == b.trackId && a.clipId == b.clipId &&
           a.subjectId == b.subjectId &&
           a.loopIteration == b.loopIteration &&
           a.occurrenceOrdinal == b.occurrenceOrdinal;
}

std::uint8_t DispositionForArmState(ArmState state) noexcept {
    if (state == ArmState::PendingApply || state == ArmState::Armed) {
        return 2;
    }
    return 3;
}

} // namespace

bool TimelineExecutor::Reset(std::uint32_t quantumFrames,
                             std::uint32_t maxEventsPerQuantum,
                             ScheduledTransactionPool* pool,
                             TimelineArmTable* armTable,
                             TimelineExecutorSink* sink,
                             DecisionTable* decisionTable) {
    if (quantumFrames == 0 || maxEventsPerQuantum == 0 || pool == nullptr ||
        !pool->Valid() || armTable == nullptr || sink == nullptr) {
        return false;
    }
    quantumFrames_ = quantumFrames;
    pool_ = pool;
    armTable_ = armTable;
    sink_ = sink;
    decisionTable_ = decisionTable;
    const std::size_t decodedCapacity =
        static_cast<std::size_t>(maxEventsPerQuantum) * 2;
    decoded_.assign(decodedCapacity, ConsumedEvent{});
    deferredDigest_.assign(decodedCapacity, TimelineDigestRecord{});
    nextEvent_.assign(decodedCapacity, UINT32_MAX);
    transactions_.assign(pool->MaxScheduledTransactions(),
                         TransactionScratch{});
    touchedTransactions_.assign(pool->MaxScheduledTransactions(), 0);
    sinkEvents_.assign(kTimelineMaxEventsPerVoiceQuantum,
                       TimelineSinkEvent{});
    pageGroups_.assign(pool->MaxPlan03GroupHeadersPerPage(),
                       Plan03PageGroupView{});
    counters_ = {};
    touchGeneration_ = 1;
    valid_ = true;
    (void)pool_->ConsumerFrontier();
    return true;
}

void TimelineExecutor::PrepassPlan03Deadlines(
    std::uint64_t absoluteQuantum,
    const TimelineExecutionPlan& plan) noexcept {
    if (decisionTable_ == nullptr || pageGroups_.empty()) {
        return;
    }
    std::uint64_t page = plan.expectedFrontier;
    for (std::uint32_t inspected = 0;
         inspected < plan.maxPages && inspected < 2 &&
         page < absoluteQuantum;
         ++inspected, ++page) {
        const std::size_t count = pool_->CapturePlan03PageGroupsRt(
            page, pageGroups_.data(), pageGroups_.size());
        if (count > pageGroups_.size()) {
            return;
        }
        for (std::size_t group = 0; group < count; ++group) {
            Plan03PageGroupView& view = pageGroups_[group];
            if (view.lease != nullptr) {
                (void)view.lease->Invalidate(
                    AudioSliceInvalidateReason::Deadline);
            }
            const DecisionDeadlineDisposition disposition =
                decisionTable_->InvalidateForDeadline(
                    view.identity.decision);
            for (std::uint16_t member = 0;
                 member < view.memberCount; ++member) {
                (void)armTable_->RequestCancel(view.members[member]);
            }
            if (disposition ==
                DecisionDeadlineDisposition::NewlyInvalidated) {
                sink_->FailPlan03Deadline(view.identity.decision,
                                          absoluteQuantum);
            }
        }
    }
}

void TimelineExecutor::FoldEvent(
    const ConsumedEvent& event, const TimelineEventPayload* payload,
    std::uint64_t effectiveFrame, std::uint8_t disposition,
    const TimelineSemanticTarget* resolvedTarget) noexcept {
    if (deferDigest_) {
        if (deferredDigestCount_ == deferredDigest_.size()) {
            ++counters_.protocolViolation;
            return;
        }
        auto& record = deferredDigest_[deferredDigestCount_++];
        record = {};
        record.event = event;
        record.effectiveFrame = effectiveFrame;
        record.disposition = disposition;
        record.payloadValid = payload != nullptr;
        if (payload != nullptr) record.payload = *payload;
        record.bindingValid = resolvedTarget != nullptr;
        if (resolvedTarget != nullptr) record.resolvedBinding = *resolvedTarget;
        return;
    }
    HashLE(&counters_.digest, event.executeFrame);
    HashLE(&counters_.digest, effectiveFrame);
    HashLE(&counters_.digest, event.logicCommitSequence);
    HashLE(&counters_.digest, event.eventIndex);
    HashLE(&counters_.digest, event.transactionOrdinal);
    HashLE(&counters_.digest, event.subeventPhase);
    HashLE(&counters_.digest,
           payload == nullptr ? 0u : static_cast<std::uint8_t>(payload->kind));
    HashLE(&counters_.digest,
           payload == nullptr ? std::uint64_t{0}
                              : payload->sourceStartFrame);
    HashLE(&counters_.digest,
           payload == nullptr ? std::uint64_t{0}
                              : payload->envelopeStartFrame);
    HashLE(&counters_.digest,
           payload == nullptr ? std::uint64_t{0}
                              : payload->envelopeEndFrame);
    HashLE(&counters_.digest,
           payload == nullptr
               ? std::uint8_t{0}
               : static_cast<std::uint8_t>(payload->envelopeDirection));
    HashLE(&counters_.digest,
           payload == nullptr
               ? std::uint8_t{0}
               : static_cast<std::uint8_t>(
                     payload->authoredFadeOutCandidate));
    if (payload != nullptr) {
        const TimelineSemanticTarget* targets[2] = {&payload->target,
                                                    resolvedTarget};
        for (const TimelineSemanticTarget* target : targets) {
            if (target == nullptr) {
                for (std::uint32_t i = 0; i < 10; ++i) {
                    HashLE(&counters_.digest, std::uint64_t{0});
                }
                continue;
            }
            HashLE(&counters_.digest, target->programSemanticFingerprint);
            HashLE(&counters_.digest, target->clipGuidHigh);
            HashLE(&counters_.digest, target->clipGuidLow);
            HashLE(&counters_.digest, target->subjectGuidHigh);
            HashLE(&counters_.digest, target->subjectGuidLow);
            HashLE(&counters_.digest, target->trackId);
            HashLE(&counters_.digest, target->clipId);
            HashLE(&counters_.digest, target->subjectId);
            HashLE(&counters_.digest, target->loopIteration);
            HashLE(&counters_.digest, target->occurrenceOrdinal);
        }
    } else {
        for (std::uint32_t target = 0; target < 2; ++target) {
            for (std::uint32_t field = 0; field < 10; ++field) {
                HashLE(&counters_.digest, std::uint64_t{0});
            }
        }
    }
    HashLE(&counters_.digest, event.transactionId);
    HashLE(&counters_.digest, disposition);
    if (digestSink_ != nullptr) {
        TimelineDigestRecord record{};
        record.event = event;
        record.effectiveFrame = effectiveFrame;
        record.disposition = disposition;
        record.payloadValid = payload != nullptr;
        if (payload != nullptr) {
            record.payload = *payload;
        }
        record.bindingValid = resolvedTarget != nullptr;
        if (resolvedTarget != nullptr) {
            record.resolvedBinding = *resolvedTarget;
        }
        digestSink_->Fold(record);
    }
}

void TimelineExecutor::CleanupCancelledArms(
    std::uint64_t audioSequence) noexcept {
    for (std::uint32_t slot = 0; slot < armTable_->Capacity(); ++slot) {
        TimelineArmTable::Row* row = armTable_->RowAt(slot);
        if (row == nullptr ||
            row->state.load(std::memory_order_acquire) !=
                ArmState::CancelRequested) {
            continue;
        }
        const std::uint64_t identity =
            row->identity.load(std::memory_order_acquire);
        if (static_cast<std::uint32_t>(identity) != slot) {
            continue;
        }
        const TimelineArmToken token{
            slot, static_cast<std::uint32_t>(identity >> 32), row->renderSlot,
            row->renderGeneration};
        if (sink_->CancelBinding(token.renderSlot, token.renderGeneration) ||
            !sink_->BindingValid(token.renderSlot,
                                 token.renderGeneration)) {
            (void)armTable_->Tombstone(token, audioSequence);
        }
    }
}

void TimelineExecutor::ExecuteQuantum(
    std::uint64_t absoluteQuantum) noexcept {
    TimelineExecutionPlan plan{};
    if (valid_) {
        plan.expectedFrontier =
            pool_->ConsumerFrontier().load(std::memory_order_acquire);
        std::uint64_t page = plan.expectedFrontier;
        while (plan.maxPages < 2 && page <= absoluteQuantum) {
            if (pool_->PageStamp(page) != page) {
                plan.stopReason = TimelinePagePrefixStopReason::Unpublished;
                break;
            }
            ++plan.maxPages;
            if (page == UINT64_MAX) {
                break;
            }
            ++page;
        }
        if (plan.stopReason != TimelinePagePrefixStopReason::Unpublished) {
            plan.stopReason =
                plan.maxPages == 2 && page <= absoluteQuantum
                    ? TimelinePagePrefixStopReason::PageLimit
                    : TimelinePagePrefixStopReason::ReachedCurrentQuantum;
        }
    }
    (void)ExecuteQuantum(absoluteQuantum, plan);
}

TimelineExecutionResult TimelineExecutor::ExecuteQuantum(
    std::uint64_t absoluteQuantum,
    const TimelineExecutionPlan& plan) noexcept {
    TimelineExecutionResult result{};
    result.frontier = plan.expectedFrontier;
    result.planStopReason = plan.stopReason;
    if (!valid_) {
        return result;
    }
    const std::uint64_t actualFrontier =
        pool_->ConsumerFrontier().load(std::memory_order_acquire);
    result.frontier = actualFrontier;
    if (actualFrontier != plan.expectedFrontier || plan.maxPages > 2) {
        return result;
    }
    PrepassPlan03Deadlines(absoluteQuantum, plan);
    const bool independentWork = sink_->CaptureIndependentWork(absoluteQuantum);
    if ((plan.maxPages != 0 || independentWork) &&
        !sink_->AdmitQuantum(absoluteQuantum)) {
        sink_->DiscardIndependentWork();
        NoteRenderUnavailable();
        return result;
    }
    if (!sink_->BeginQuantum(absoluteQuantum)) {
        sink_->DiscardIndependentWork();
        return result;
    }
    if (!sink_->ApplyPlan03Groups(absoluteQuantum)) {
        sink_->EndQuantum(absoluteQuantum);
        return result;
    }
    CleanupCancelledArms(absoluteQuantum);
    const bool plannedUnpublishedMiss =
        plan.stopReason == TimelinePagePrefixStopReason::Unpublished &&
        plan.expectedFrontier <= absoluteQuantum;
    if (plannedUnpublishedMiss) {
        ++counters_.deadlineMissQuanta;
    }
    std::uint64_t next = actualFrontier;
    std::uint32_t pages = 0;
    std::size_t decodedCount = 0;
    while (pages < plan.maxPages && next <= absoluteQuantum) {
        if (pool_->PageStamp(next) != next) {
            if (!plannedUnpublishedMiss) {
                ++counters_.deadlineMissQuanta;
            }
            break;
        }
        if (!sink_->CollectFrontierGroups(next)) {
            ++counters_.protocolViolation;
            break;
        }
        decodedCount += pool_->ConsumePageRt(
            next, decoded_.data() + decodedCount,
            decoded_.size() - decodedCount);
        ++next;
        ++pages;
        pool_->ConsumerFrontier().store(next, std::memory_order_release);
        result.frontier = next;
        result.consumedPageCount = pages;
    }

    if (++touchGeneration_ == 0) {
        touchGeneration_ = 1;
        for (TransactionScratch& transaction : transactions_) {
            transaction.touchedGeneration = 0;
        }
    }
    deferredDigestCount_ = 0;
    deferDigest_ = true;
    std::uint32_t touchedCount = 0;
    for (std::uint32_t i = 0; i < decodedCount; ++i) {
        const ConsumedEvent& event = decoded_[i];
        if (event.txnSlot >= transactions_.size()) {
            ++counters_.protocolViolation;
            FoldEvent(event, nullptr, event.executeFrame, 5);
            continue;
        }
        TransactionScratch& transaction = transactions_[event.txnSlot];
        if (transaction.touchedGeneration != touchGeneration_) {
            transaction = {};
            transaction.touchedGeneration = touchGeneration_;
            transaction.firstEvent = i;
            touchedTransactions_[touchedCount++] = event.txnSlot;
        } else {
            nextEvent_[transaction.lastEvent] = i;
        }
        transaction.lastEvent = i;
        ++transaction.eventCount;
        nextEvent_[i] = UINT32_MAX;
    }

    const std::uint64_t quantumStart = absoluteQuantum * quantumFrames_;
    for (std::uint32_t t = 0; t < touchedCount; ++t) {
        const TransactionScratch& transaction =
            transactions_[touchedTransactions_[t]];
        const ConsumedEvent& first = decoded_[transaction.firstEvent];
        const TimelineProgramView* program = first.program.program;
        if (program != nullptr && program->role == TimelineProgramRole::AuthoredStopFence) {
            const auto* fence = program->authoredStopFence;
            const auto* payload = program->payloads != nullptr && program->payloadCount == 1
                ? &program->payloads[0] : nullptr;
            const bool valid = first.frozenTxnValid && transaction.eventCount == 1 &&
                !first.armToken.Valid() && first.payload == 0 && first.subeventPhase == 50 &&
                fence != nullptr && fence->program == program &&
                fence->programGeneration == first.program.programGeneration &&
                program->generation == fence->programGeneration && fence->Active() &&
                first.ownerInstanceHandle == fence->identity.root &&
                first.ownerInstanceScheduleEpoch == fence->identity.sourceEpoch &&
                first.executeFrame == fence->identity.stopFrame &&
                first.executeFrame < quantumStart + quantumFrames_ &&
                payload != nullptr && payload->kind == TimelineEventKind::Stop &&
                payload->reserved == kTimelineEventAuthoredRootStop &&
                payload->envelopeEndFrame == fence->identity.stopFrame;
            std::uint8_t disposition = kTimelineDispositionStale;
            if (valid) {
                const TimelineExecutionOrder order{first.executeFrame, first.logicCommitSequence,
                    first.executionOrdinal, first.eventIndex, first.transactionOrdinal,
                    first.subeventPhase, first.rowSequence};
                if (sink_->CollectAuthoredStopFence(*fence, absoluteQuantum, order)) {
                    disposition = kTimelineDispositionApplied;
                } else {
                    disposition = kTimelineDispositionProtocolViolation;
                    ++counters_.spanOverflowDropped;
                    ++counters_.protocolViolation;
                }
            } else {
                ++counters_.staleSkippedTransactions;
            }
            for (auto index = transaction.firstEvent; index != UINT32_MAX; index = nextEvent_[index]) {
                FoldEvent(decoded_[index], payload,
                    std::max(quantumStart, decoded_[index].executeFrame), disposition);
            }
            continue;
        }
        bool frontierTransaction = false;
        for (auto index = transaction.firstEvent; index != UINT32_MAX; index = nextEvent_[index])
            frontierTransaction = frontierTransaction || decoded_[index].frontier.Present();
        if (frontierTransaction) {
            for (auto index = transaction.firstEvent; index != UINT32_MAX; index = nextEvent_[index]) {
                const auto& event = decoded_[index];
                const auto* eventProgram = event.program.program;
                const auto* payload = event.frozenTxnValid && eventProgram != nullptr &&
                    eventProgram->generation == event.program.programGeneration &&
                    eventProgram->payloads != nullptr && event.payload < eventProgram->payloadCount ?
                    &eventProgram->payloads[event.payload] : nullptr;
                TimelineSemanticTarget target{};
                ArmState state = ArmState::Free;
                const bool controller = payload != nullptr &&
                    payload->kind == TimelineEventKind::ControllerActivation;
                const bool bindingValid = !controller && armTable_->Snapshot(event.armToken, &state, &target) &&
                    sink_->BindingValid(event.armToken.renderSlot, event.armToken.renderGeneration);
                const bool frameValid = event.executeFrame >= quantumStart &&
                    event.executeFrame - quantumStart < quantumFrames_;
                const auto expectedPhase = payload == nullptr ? 0u :
                    payload->kind == TimelineEventKind::SetStartCursor ? 10u :
                    payload->kind == TimelineEventKind::Start || controller ? 20u :
                    payload->kind == TimelineEventKind::Envelope ||
                    payload->kind == TimelineEventKind::VariableWeight ? 30u : 50u;
                bool valid = event.frontier.Valid() && payload != nullptr && frameValid &&
                    event.subeventPhase == expectedPhase && event.transactionId == first.transactionId &&
                    event.program.program == program &&
                    (controller ? eventProgram->role == TimelineProgramRole::ControllerActivation &&
                        !event.armToken.Valid() && (event.frontier.kind == FrontierOperationKind::Entry ||
                            event.frontier.group.Exact()) :
                        bindingValid && SameTarget(payload->target, target));
                if (payload != nullptr && payload->kind == TimelineEventKind::VariableWeight) {
                    float weight = 0.0f;
                    valid = valid && payload->variableWeight.startFrame == event.executeFrame &&
                        ResolveWeightSegment(payload->variableWeight, event.executeFrame, &weight);
                }
                const auto disposition = valid ? kTimelineDispositionApplied : kTimelineDispositionProtocolViolation;
                if (!sink_->StageFrontierEvent(event, payload, disposition)) ++counters_.spanOverflowDropped;
                if (!valid) ++counters_.protocolViolation;
                if (event.executeFrame < quantumStart) ++counters_.lateConsumedEvents;
                FoldEvent(event, payload, event.executeFrame, disposition, bindingValid ? &target : nullptr);
            }
            continue;
        }
        TimelineArmObservation parentObservation{};
        DecisionCommittedSnapshot parent{};
        const bool parentCaptured = first.frozenTxnValid &&
            armTable_->Observe(first.armToken, &parentObservation) &&
            parentObservation.parentGroup.Valid() && decisionTable_ != nullptr &&
            decisionTable_->CaptureCommitted(parentObservation.parentGroup.decision, &parent);
        if (parentCaptured && parent.authoredStopInvalidated) {
            for (auto index = transaction.firstEvent; index != UINT32_MAX; index = nextEvent_[index]) {
                FoldEvent(decoded_[index], nullptr,
                    std::max(quantumStart, decoded_[index].executeFrame),
                    kTimelineDispositionStopDominated);
            }
            continue;
        }
        ArmState armState = ArmState::Free;
        TimelineSemanticTarget armTarget{};
        const bool armValid =
            armTable_->Snapshot(first.armToken, &armState, &armTarget);
        bool valid = first.frozenTxnValid && program != nullptr &&
                     program->role == TimelineProgramRole::Voice && armValid &&
                     program->generation == first.program.programGeneration &&
                      sink_->BindingValid(first.armToken.renderSlot,
                                          first.armToken.renderGeneration);
        if (valid && parentObservation.parentGroup.Valid()) {
            valid = parentCaptured &&
                    (parent.audioState == AudioGroupState::Admitted ||
                     (parent.audioState == AudioGroupState::AudioClaimed &&
                      armState == ArmState::AudioClaimed) ||
                     (armState == ArmState::AudioClaimed &&
                      (parent.audioState ==
                           AudioGroupState::PostStartInvalidated ||
                       parent.audioState == AudioGroupState::
                                                PostStartDeadlineInvalidated)));
        }
        std::uint8_t disposition = valid ? 0 : 1;
        if (!first.frozenTxnValid) {
            ++counters_.staleSkippedTransactions;
        } else if (!armValid || !valid) {
            ++counters_.armFailedTransactions;
            disposition = 3;
        }
        if (valid && armState != ArmState::Published &&
            armState != ArmState::AudioClaimed &&
            armState != ArmState::Started) {
            valid = false;
            disposition = DispositionForArmState(armState);
            if (disposition == 2) {
                ++counters_.armPendingSkipped;
            } else {
                ++counters_.armFailedTransactions;
            }
        }
        if (!valid) {
            for (auto index = transaction.firstEvent; index != UINT32_MAX; index = nextEvent_[index]) {
                FoldEvent(decoded_[index], nullptr,
                    std::max(quantumStart, decoded_[index].executeFrame), disposition,
                    armValid ? &armTarget : nullptr);
            }
            continue;
        }
        if (transaction.eventCount > sinkEvents_.size()) {
            valid = false;
            disposition = 5;
            ++counters_.spanOverflowDropped;
            ++counters_.protocolViolation;
        }

        std::uint32_t eventIndex = transaction.firstEvent;
        std::uint32_t sinkCount = 0;
        bool hasStart = false;
        bool hasStop = false;
        bool hasVariableWeight = false;
        bool duplicateVariableWeight = false;
        bool duplicateStart = false;
        bool haveCursor = false;
        std::uint64_t cursorValue = 0;
        bool cursorConflict = false;
        bool haveStop = false;
        std::uint64_t stopValue = 0;
        bool stopConflict = false;
        while (eventIndex != UINT32_MAX) {
            const ConsumedEvent& event = decoded_[eventIndex];
            const TimelineEventPayload* payload = nullptr;
            if (program != nullptr && event.program.program == program &&
                event.program.programGeneration == program->generation &&
                event.payload < program->payloadCount) {
                payload = &program->payloads[event.payload];
            }
            if (payload == nullptr || !armValid ||
                !SameTarget(payload->target, armTarget)) {
                valid = false;
                disposition = 5;
                ++counters_.protocolViolation;
            } else {
                const std::uint32_t expectedPhase =
                    payload->kind == TimelineEventKind::SetStartCursor
                        ? 10u
                        : payload->kind == TimelineEventKind::Start
                              ? 20u
                              : payload->kind == TimelineEventKind::Envelope ||
                                        payload->kind ==
                                            TimelineEventKind::VariableWeight
                                    ? 30u
                                    : 50u;
                if (event.subeventPhase != expectedPhase) {
                    valid = false;
                    disposition = 5;
                    ++counters_.protocolViolation;
                }
                if (payload->kind == TimelineEventKind::SetStartCursor) {
                    if (haveCursor && cursorValue != payload->sourceStartFrame) {
                        cursorConflict = true;
                    }
                    haveCursor = true;
                    cursorValue = payload->sourceStartFrame;
                } else if (payload->kind == TimelineEventKind::Start) {
                    duplicateStart = duplicateStart || hasStart;
                    hasStart = true;
                } else if (payload->kind ==
                           TimelineEventKind::VariableWeight) {
                    float validatedWeight = 0.0f;
                    duplicateVariableWeight =
                        duplicateVariableWeight || hasVariableWeight;
                    hasVariableWeight = true;
                    if (payload->variableWeight.startFrame !=
                            event.executeFrame ||
                        !parentObservation.parentGroup.Valid() ||
                        payload->variableWeight.decision.slot !=
                            parentObservation.parentGroup.decision.slot ||
                        payload->variableWeight.decision.generation !=
                            parentObservation.parentGroup.decision.generation ||
                        !ResolveWeightSegment(
                            payload->variableWeight,
                            payload->variableWeight.startFrame,
                            &validatedWeight)) {
                        valid = false;
                        disposition = 5;
                        ++counters_.protocolViolation;
                    }
                } else if (payload->kind == TimelineEventKind::Stop) {
                    if (haveStop && stopValue != payload->envelopeEndFrame) {
                        stopConflict = true;
                    }
                    haveStop = true;
                    stopValue = payload->envelopeEndFrame;
                    hasStop = true;
                }
            }
            const bool late = event.executeFrame < quantumStart;
            if (late) {
                ++counters_.lateConsumedEvents;
            }
            const std::uint64_t effectiveFrame =
                late ? quantumStart : event.executeFrame;
            if (effectiveFrame >= quantumStart + quantumFrames_) {
                valid = false;
                disposition = 5;
                ++counters_.protocolViolation;
            }
            if (valid && payload != nullptr) {
                bool duplicate = false;
                if (payload->kind == TimelineEventKind::SetStartCursor) {
                    for (std::uint32_t prior = 0; prior < sinkCount; ++prior) {
                        duplicate = duplicate ||
                                    (sinkEvents_[prior].payload->kind ==
                                         TimelineEventKind::SetStartCursor &&
                                     sinkEvents_[prior].payload
                                             ->sourceStartFrame ==
                                         payload->sourceStartFrame);
                    }
                } else if (payload->kind == TimelineEventKind::Stop) {
                    for (std::uint32_t prior = 0; prior < sinkCount; ++prior) {
                        duplicate = duplicate ||
                                    sinkEvents_[prior].payload->kind ==
                                        TimelineEventKind::Stop;
                    }
                }
                if (!duplicate) {
                    const std::uint32_t offset = static_cast<std::uint32_t>(
                        effectiveFrame - quantumStart);
                    const bool authored =
                        payload->kind == TimelineEventKind::Stop &&
                        event.authoredFadeOutCandidate;
                    sinkEvents_[sinkCount++] = {
                        offset, payload, authored,
                        {event.executeFrame, event.logicCommitSequence,
                         event.executionOrdinal, event.eventIndex,
                         event.transactionOrdinal, event.subeventPhase,
                         event.rowSequence}};
                }
            }
            eventIndex = nextEvent_[eventIndex];
        }

        if (valid && (duplicateStart || duplicateVariableWeight ||
                      cursorConflict || stopConflict)) {
            valid = false;
            disposition = 5;
            ++counters_.protocolViolation;
        }
        if (valid && hasStart && hasStop) {
            valid = false;
            disposition = 4;
            (void)sink_->CancelBinding(first.armToken.renderSlot,
                                       first.armToken.renderGeneration);
            (void)armTable_->RequestCancel(first.armToken);
            ++counters_.armFailedTransactions;
        }
        if (valid && !sink_->ApplyTransaction(
                         first.armToken.renderSlot,
                         first.armToken.renderGeneration, absoluteQuantum,
                         sinkEvents_.data(), sinkCount, first.armToken)) {
            valid = false;
            disposition = 5;
            ++counters_.spanOverflowDropped;
            ++counters_.protocolViolation;
        }
        eventIndex = transaction.firstEvent;
        while (eventIndex != UINT32_MAX) {
            const ConsumedEvent& event = decoded_[eventIndex];
            const TimelineEventPayload* payload =
                program != nullptr && event.payload < program->payloadCount
                    ? &program->payloads[event.payload]
                    : nullptr;
            const bool late = event.executeFrame < quantumStart;
            const std::uint64_t effectiveFrame =
                late ? quantumStart : event.executeFrame;
            FoldEvent(event, payload, effectiveFrame, disposition,
                      armValid ? &armTarget : nullptr);
            eventIndex = nextEvent_[eventIndex];
        }
    }
    sink_->EndQuantum(absoluteQuantum);
    deferDigest_ = false;
    for (std::size_t index = 0; index < deferredDigestCount_; ++index) {
        auto& record = deferredDigest_[index];
        if (record.event.frontier.Present())
            record.disposition = sink_->FrontierEventDisposition(record.event);
        FoldEvent(record.event, record.payloadValid ? &record.payload : nullptr,
            record.effectiveFrame, record.disposition, record.bindingValid ? &record.resolvedBinding : nullptr);
    }
    return result;
}

} // namespace harken
