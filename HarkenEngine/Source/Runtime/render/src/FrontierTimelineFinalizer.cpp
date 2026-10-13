#include "harken/render/FrontierTimelineFinalizer.h"
#include "harken/core/ImmediateActionLane.h"
#include "harken/core/OriginalImmediateLaneConditionalProjection.h"
#include "harken/render/VoicePool.h"
#include "harken/core/VariableControlRuntime.h"
#include "harken/core/QueueRuntime.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/ArmGroupTable.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <limits>

namespace harken {
namespace {

TimelineExecutionOrder OrderOf(const ConsumedEvent& event) noexcept {
    return {event.executeFrame, event.logicCommitSequence, event.executionOrdinal,
        event.eventIndex, event.transactionOrdinal, event.subeventPhase, event.rowSequence};
}

bool SameScope(const FrontierOwnerIdentity& owner, const AuthoredStopOperationIdentity& stop) noexcept {
    return (owner.invocation == stop.root && owner.invocationEpoch == stop.sourceEpoch) ||
        (owner.child == stop.root && owner.childEpoch == stop.sourceEpoch) ||
        (owner.parent == stop.root && owner.parentEpoch == stop.sourceEpoch);
}

bool SameArm(TimelineArmToken a, TimelineArmToken b) noexcept {
    return a.slot == b.slot && a.generation == b.generation &&
        a.renderSlot == b.renderSlot && a.renderGeneration == b.renderGeneration;
}

} // namespace

struct FrontierTimelineFinalizer::Impl {
    struct Event {
        ConsumedEvent source{};
        VoiceTimelineEvent voice{};
        FrontierDueOperationRef operation{};
        VoiceTimelineReservation reservation{};
        VoiceTimelineReservationProof appendProof;
        TimelineArmTable::BoundAudioProof armProof;
        const AuthoredStopFenceDescriptor* stop = nullptr;
        AuthoredStopOperationIdentity stopIdentity{};
        TimelineExecutionOrder order{};
        std::uint32_t representative = UINT32_MAX;
        std::uint32_t group = UINT32_MAX;
        std::uint8_t disposition = kTimelineDispositionApplied;
        bool controller = false;
        bool completesControl = false;
        bool reserved = false;
        bool stopApplying = false;
        bool stopApplied = false;
    };
    struct Group {
        Plan03PageGroupView view{};
        FrontierDecisionApplyProof decisionProof;
        VariableControlInstanceTable::FrontierSideProof sideProof;
        QueueInstanceTable::FrontierInitialProof queueProof;
        QueueInstanceTable::FrontierInitialCompletionProof queueCompletionProof;
        QueueInstanceTable::FrontierConditionalProof conditionalProof;
        TransitionInstancePool::FrontierProof transitionProof;
        QueueInstanceTable::FrontierConditionalSlice queueSlice = QueueInstanceTable::FrontierConditionalSlice::Activation;
        bool delayedActivation = false;
        bool variablePrelude = false;
        bool variableCompletion = false;
        bool initialQueuePrelude = false;
        bool initialQueueCompletion = false;
        bool conditionalQueueCompletion = false;
        FrontierActivation* activation = nullptr;
        Impl* owner = nullptr;
        std::uint32_t index = 0;
        FrontierReceiptReason failure = FrontierReceiptReason::None;
        AuthoredStopOperationIdentity cutoff{};
        bool audioWriter = false;
        bool applied = false;
        std::uint32_t firstCompletionProof = 0;
        std::uint32_t completionProofCount = 0;
    };
    struct OrderRef {
        TimelineExecutionOrder order{};
        std::uint32_t index = 0;
        bool group = false;
    };

    VoicePool& voices;
    TimelineArmTable& arms;
    DecisionTable* decisions = nullptr;
    const ScheduledTransactionPool* pool = nullptr;
    VariableControlInstanceTable* variableControls = nullptr;
    QueueInstanceTable* queues = nullptr;
    const OccurrenceStatePool* occurrences = nullptr;
    const CompositeCompletionOwnerPool* composites = nullptr;
    const ArmGroupTable* armGroups = nullptr;
    TransitionInstancePool* transitions = nullptr;
    const ImmediateActionLane* immediateLane = nullptr;
    const ImmediateLaneBucket* immediateLaneBucket = nullptr;
    std::uint32_t immediateLaneSlot = UINT32_MAX;
    std::uint32_t immediateLaneGeneration = 0;
#if !defined(HARKEN_RT_QUALIFICATION)
    void (*variableSliceClaimHook)(void*) = nullptr;
    void* variableSliceClaimContext = nullptr;
    void (*queueFutureValidatedHook)(void*) = nullptr;
    void (*queueFutureClaimedHook)(void*) = nullptr;
    void (*queueFutureAppliedHook)(void*, std::uint16_t) = nullptr;
    void* queueFutureHookContext = nullptr;
    Plan03GroupSliceIdentity queueLaneProbeGroup{};

    Group* SelectedLaneQueueProbe(bool continuation) noexcept {
        if (!continuation || !laneMode || !queueLaneProbeGroup.Exact() ||
            immediateLane == nullptr || frames == 0) return nullptr;
        Group* selected = nullptr;
        for (std::uint32_t index = 0; index < groupCount; ++index) {
            auto& group = groups[index];
            if (!(group.view.identity == queueLaneProbeGroup)) continue;
            const auto* binding = group.view.frontier;
            if (selected != nullptr || binding == nullptr ||
                group.view.identity.backing != Plan03GroupBacking::ImmediateLane ||
                (binding->role != FrontierGroupRole::EntryMember &&
                 !(binding->role == FrontierGroupRole::Future && group.initialQueueCompletion)) ||
                binding->operation.order.executeFrame / frames != quantum ||
                (!binding->queueInitial.Valid() && !binding->queueConditional.Valid()) ||
                group.failure != FrontierReceiptReason::None) return nullptr;
            selected = &group;
        }
        return selected;
    }

    bool ProbeLaneReceipt(const ImmediateLaneBucketClaimReceipt& receipt,
        const ImmediateLaneBucket* bucket) const noexcept {
        if (pool == nullptr || immediateLane == nullptr || bucket == nullptr ||
            bucket->quantum != quantum || receipt.Slot() != immediateLaneSlot ||
            receipt.Generation() != immediateLaneGeneration ||
            !immediateLane->ValidateClaimReceipt(receipt, bucket)) return false;
        for (std::uint32_t index = 0; index < groupCount; ++index)
            if (!pool->ValidateImmediateLaneGroupClaim(groups[index].view.identity,
                    groups[index].view.lease, receipt)) return false;
        return true;
    }

    void ProbeLaneApplied(bool continuation) noexcept {
        auto* selected = SelectedLaneQueueProbe(continuation);
        if (selected == nullptr || !selected->applied || selected->view.lease == nullptr ||
            selected->view.lease->Capture().state != AudioSliceState::SliceApplied) return;
        for (std::uint32_t index = 0; index < groupCount; ++index)
            if (!groups[index].applied || groups[index].view.lease == nullptr ||
                groups[index].view.lease->Capture().state != AudioSliceState::SliceApplied) return;
        if (queueFutureAppliedHook != nullptr)
            queueFutureAppliedHook(queueFutureHookContext, selected->view.identity.sliceIndex);
    }

    static bool QueueFutureProbe(const Group& group) noexcept {
        return !group.owner->laneMode && group.view.identity.backing == Plan03GroupBacking::Page &&
            group.view.frontier != nullptr && group.view.frontier->role == FrontierGroupRole::Future &&
            group.view.frontier->queueConditional.Valid() && group.failure == FrontierReceiptReason::None;
    }

    static void AfterSliceClaim(void* opaque) noexcept {
        const auto& group = *static_cast<Group*>(opaque);
        bool variable = group.view.frontier->variableSide.Valid();
        if (group.view.frontier->role == FrontierGroupRole::Coordinator) {
            for (std::uint32_t index = 0; index < group.owner->groupCount; ++index) {
                const auto& binding = *group.owner->groups[index].view.frontier;
                variable = variable || (binding.role == FrontierGroupRole::EntryMember &&
                    binding.entry == group.view.frontier->entry && binding.variableSide.Valid());
            }
        }
        if (variable && group.owner->variableSliceClaimHook != nullptr)
            group.owner->variableSliceClaimHook(group.owner->variableSliceClaimContext);
        if (QueueFutureProbe(group) && group.owner->queueFutureClaimedHook != nullptr)
            group.owner->queueFutureClaimedHook(group.owner->queueFutureHookContext);
    }
#endif
    std::unique_ptr<DecisionParticipantRef[]> participants;
    std::uint32_t participantCapacity = 0;
    std::unique_ptr<TimelineArmTable::BoundAudioProof[]> completionProofs;
    std::uint32_t completionCapacity = 0;
    std::uint32_t completionCount = 0;
    std::uint32_t deferredStartCount = 0;
    std::unique_ptr<Event[]> events;
    std::unique_ptr<Group[]> groups;
    std::unique_ptr<OrderRef[]> order;
    std::unique_ptr<ImmediateLaneFrontierClaim[]> laneClaims;
    std::uint32_t eventCapacity = 0;
    std::uint32_t groupCapacity = 0;
    std::uint32_t eventCount = 0;
    std::uint32_t groupCount = 0;
    std::uint64_t quantum = 0;
    std::uint32_t frames = 0;
    bool laneMode = false;

    Impl(VoicePool& v, TimelineArmTable& a) : voices(v), arms(a) {}

    std::uint32_t FindGroup(const Plan03GroupSliceIdentity& identity) const noexcept {
        for (std::uint32_t i = 0; i < groupCount; ++i)
            if (groups[i].view.identity == identity) return i;
        return UINT32_MAX;
    }

    std::uint32_t FindEntry(const FrontierEntryReceiptRef& entry) const noexcept {
        for (std::uint32_t i = 0; i < groupCount; ++i) {
            const auto& binding = *groups[i].view.frontier;
            if (binding.role == FrontierGroupRole::Coordinator && binding.entry == entry) return i;
        }
        return UINT32_MAX;
    }

    bool SameOperation(const Event& a, const Event& b) const noexcept {
        return a.stop == nullptr && b.stop == nullptr && !a.source.frontier.group.Valid() &&
            !b.source.frontier.group.Valid() && a.source.frontier.kind == b.source.frontier.kind &&
            a.source.frontier.entry == b.source.frontier.entry &&
            a.source.frontier.owner == b.source.frontier.owner &&
            a.source.frontier.operationOrdinal == b.source.frontier.operationOrdinal &&
            a.source.txnSlot == b.source.txnSlot && a.source.transactionId == b.source.transactionId &&
            a.source.program.program == b.source.program.program &&
            a.source.program.programGeneration == b.source.program.programGeneration &&
            a.source.frontier.transport == b.source.frontier.transport &&
            a.source.frontier.world == b.source.frontier.world &&
            a.source.executeFrame == b.source.executeFrame && SameArm(a.source.armToken, b.source.armToken);
    }

    std::uint64_t CutoffFor(const Event& event, AuthoredStopOperationIdentity* identity) const noexcept {
        std::uint64_t cutoff = UINT64_MAX;
        *identity = {};
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            const auto& stop = events[i];
            if (stop.stop == nullptr || (!stop.stopApplying && !stop.stopApplied) ||
                !SameScope(event.source.frontier.owner, stop.stopIdentity)) continue;
            if (stop.stopIdentity.stopFrame < cutoff) {
                cutoff = stop.stopIdentity.stopFrame;
                *identity = stop.stopIdentity;
            }
        }
        if (!event.controller) {
            const auto token = event.source.armToken;
            const auto handle = (static_cast<std::uint64_t>(token.renderGeneration) << 32) | token.renderSlot;
            const auto* voice = voices.Resolve(handle);
            if (voice != nullptr && voice->authoredRootStopFrame < cutoff) {
                cutoff = voice->authoredRootStopFrame;
                *identity = voice->authoredRootStopControl.authoredStopOperation;
            }
            if (voice != nullptr) {
                for (std::uint32_t i = 0; i < voice->timelineEvents.eventCount; ++i) {
                    const auto& control = voice->timelineEvents.events[i];
                    if (control.kind != TimelineEventKind::Stop || !control.authoredRootStop ||
                        !control.frontierProof.Eligible(quantum, token.renderGeneration)) continue;
                    if (control.order.executeFrame < cutoff) {
                        cutoff = control.order.executeFrame;
                        *identity = control.authoredStopOperation;
                    }
                }
            }
        }
        return cutoff;
    }

    void ReleaseProofs(Event& event) noexcept {
        event.armProof.Release();
        std::destroy_at(&event.appendProof);
        std::construct_at(&event.appendProof);
    }

    void Reject(Event& event, std::uint8_t disposition) noexcept {
        ReleaseProofs(event);
        if (event.reserved) (void)voices.RejectTimelineReservation(event.reservation);
        event.disposition = disposition;
    }

    void PrepareSpans() noexcept {
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            auto& event = events[i];
            if (event.stop != nullptr) {
                event.stopApplying = event.stop->TryBeginApply();
                continue;
            }
            const auto& binding = event.source.frontier;
            if (binding.kind == FrontierOperationKind::Entry) {
                event.group = FindEntry(binding.entry);
                if (binding.group.Valid()) {
                    const auto member = FindGroup(binding.group);
                    if (member == UINT32_MAX || event.group == UINT32_MAX ||
                        !(groups[member].view.frontier->entry == binding.entry) ||
                        groups[member].view.lease != groups[event.group].view.lease ||
                        !(groups[member].view.frontier->operation.owner == binding.owner))
                        event.disposition = kTimelineDispositionProtocolViolation;
                }
            } else if (binding.group.Valid()) {
                event.group = FindGroup(binding.group);
                if (laneMode && binding.kind == FrontierOperationKind::Dependent &&
                    event.group != UINT32_MAX) {
                    const auto& member = groups[event.group];
                    if (member.view.frontier->role == FrontierGroupRole::EntryMember &&
                        (member.view.frontier->queueInitial.Valid() ||
                         (member.view.frontier->queueConditional.Valid() && member.view.frontier->transition.Valid() &&
                          (member.view.sourceControlCount == 0u || member.view.identity.sliceIndex == 0u))) &&
                        member.view.frontier->entry == binding.entry) {
                        const auto entry = FindEntry(binding.entry);
                        if (entry == UINT32_MAX ||
                            groups[entry].view.lease != member.view.lease)
                            event.disposition = kTimelineDispositionProtocolViolation;
                        else event.group = entry;
                    }
                }
            } else {
                event.representative = i;
                for (std::uint32_t prior = 0; prior < i; ++prior) {
                    if (SameOperation(event, events[prior])) {
                        event.representative = events[prior].representative;
                        break;
                    }
                }
            }
            if (event.group != UINT32_MAX) event.operation = groups[event.group].view.frontier->operation;
            else {
                event.operation.kind = binding.kind;
                event.operation.owner = binding.owner;
                event.operation.order = event.representative == UINT32_MAX ? event.order :
                    events[event.representative].order;
                event.operation.transport = binding.transport;
                event.operation.world = binding.world;
                event.operation.transactionId = event.source.transactionId;
                event.operation.transactionSlot = event.source.txnSlot;
                event.operation.program = event.source.program.program;
                event.operation.programGeneration = event.source.program.programGeneration;
                event.operation.armSlot = event.source.armToken.slot;
                event.operation.armGeneration = event.source.armToken.generation;
                event.operation.renderSlot = event.source.armToken.renderSlot;
                event.operation.renderGeneration = event.source.armToken.renderGeneration;
            }
            if (event.disposition != kTimelineDispositionApplied || !event.operation.Valid()) {
                event.disposition = kTimelineDispositionProtocolViolation;
                continue;
            }
            if (event.controller) continue;
            auto& guard = event.voice.frontierProof;
            guard.state = FrontierEventState::Pending;
            if (!laneMode) {
                guard.entry = binding.entry;
                guard.operation = event.operation;
            }
            guard.quantum = quantum;
            guard.renderGeneration = event.source.armToken.renderGeneration;
            event.reserved = laneMode
                ? voices.ReserveLaneTimelineEvents(event.source.armToken.renderSlot,
                    event.source.armToken.renderGeneration, quantum, 1, &event.reservation)
                : voices.ReserveTimelineEvents(event.source.armToken.renderSlot,
                    event.source.armToken.renderGeneration, quantum, 1, &event.reservation);
            const bool staged = event.reserved && (laneMode
                ? voices.StageReservedLaneTimelineEvent(event.reservation, 0, event.voice)
                : voices.StageReservedTimelineEvent(event.reservation, 0, event.voice));
            if (!staged)
                event.disposition = kTimelineDispositionProtocolViolation;
        }
    }

    static bool ValidateEntry(void* opaque) noexcept {
        const auto& group = *static_cast<Group*>(opaque);
        const bool valid = group.failure == FrontierReceiptReason::None;
#if !defined(HARKEN_RT_QUALIFICATION)
        if (valid && QueueFutureProbe(group) && group.owner->queueFutureValidatedHook != nullptr)
            group.owner->queueFutureValidatedHook(group.owner->queueFutureHookContext);
#endif
        return valid;
    }

    static void ApplyEntry(void* opaque) noexcept {
        auto& group = *static_cast<Group*>(opaque);
        auto& self = *group.owner;
        for (std::uint32_t i = 0; i < self.groupCount; ++i) {
            auto& member = self.groups[i];
            if (member.view.frontier->role != FrontierGroupRole::EntryMember ||
                !(member.view.frontier->entry == group.view.frontier->entry)) continue;
            self.ApplyMode(member);
            for (std::uint32_t proof = member.firstCompletionProof;
                 proof < member.firstCompletionProof +
                     member.completionProofCount; ++proof)
                self.arms.ApplyBoundFrontierArm(
                    self.completionProofs[proof], true);
            self.decisions->ApplyClaimedFrontierDecision(member.decisionProof);
            member.applied = true;
        }
        for (std::uint32_t i = 0; i < self.eventCount; ++i) {
            auto& event = self.events[i];
            if (event.stop != nullptr || event.group != group.index) continue;
            if (event.armProof.Valid()) self.arms.ApplyBoundFrontierArm(event.armProof, event.completesControl);
            if (event.reserved) {
                AuthoredStopOperationIdentity stop{};
                const auto cutoff = self.CutoffFor(event, &stop);
                if (self.laneMode)
                    self.voices.ApplyProvenLaneTimelineReservation(event.appendProof);
                else
                    self.voices.ApplyProvenTimelineReservation(event.appendProof, cutoff, stop);
            }
        }
        self.decisions->ApplyClaimedFrontierDecision(group.decisionProof);
        group.applied = true;
    }

    void ApplyMode(Group& member) noexcept {
        if (member.transitionProof.Valid()) transitions->ApplyFrontier(member.transitionProof);
        if (member.view.frontier->variableSide.Valid()) {
            if (!member.variablePrelude) variableControls->ApplyFrontierSide(member.sideProof);
        }
        else if (member.view.frontier->queueInitial.Valid()) {
            if (!member.initialQueueCompletion) queues->ApplyFrontierInitial(member.queueProof);
            else if (member.view.frontier->canonicalInitialCompletion)
                queues->ApplyFrontierInitialCompletion(member.queueCompletionProof);
        }
        else queues->ApplyFrontierConditional(member.conditionalProof);
    }

    std::uint32_t ConditionalTargetEventCount(const Plan03PageGroupView& view,
        const ImmediateActionLane& lane, std::uint32_t slot, std::uint32_t generation) const noexcept {
        if (view.members == nullptr || view.sourceControlCount >= view.memberCount) return 0;
        std::uint32_t count = 0;
        for (std::uint16_t member = view.sourceControlCount; member < view.memberCount; ++member) {
            const ImmediateLaneEvent* mapped = nullptr;
            std::uint16_t span = 0;
            if (!lane.FutureMember(slot, generation, view.identity, member, view.members[member], &mapped, &span) ||
                mapped == nullptr || (span != 1u && span != 2u)) return 0;
            count += span;
        }
        return count;
    }

    bool HasConditionalTargetSpans(const Plan03PageGroupView& view,
        const ImmediateActionLane& lane, std::uint32_t slot, std::uint32_t generation) const noexcept {
        if (view.frontier == nullptr || view.members == nullptr || view.memberCount == 0u ||
            view.sourceControlCount != 0u || view.identity.backing != Plan03GroupBacking::ImmediateLane)
            return false;
        const auto& binding = *view.frontier;
        DecisionCommittedSnapshot decision{};
        if (binding.role != FrontierGroupRole::EntryMember || !binding.queueConditional.Valid() ||
            binding.queueInitial.Valid() || binding.variableSide.Valid() || !binding.transition.Valid() ||
            binding.operation.kind != FrontierOperationKind::Dependent ||
            binding.operation.order.executeFrame != binding.transition.audibleFrame ||
            !decisions->CaptureCommitted(view.identity.decision, &decision) || decision.sliceCount != 1u ||
            decision.transitionGate.slot != binding.transition.slot ||
            decision.transitionGate.generation != binding.transition.generation ||
            !decisions->HasExclusiveParticipant(view.identity.decision,
                {DecisionParticipantKind::QueueInstance, binding.queueConditional.slot,
                    binding.queueConditional.generation})) return false;
        for (std::uint16_t index = 0; index < view.memberCount; ++index) {
            const ImmediateLaneEvent* mapped = nullptr;
            std::uint16_t span = 0;
            if (!lane.FutureMember(slot, generation, view.identity, index, view.members[index], &mapped, &span) ||
                mapped == nullptr || (span != 1u && span != 2u) || mapped->event.frontier.kind != FrontierOperationKind::Dependent ||
                !(mapped->event.frontier.entry == binding.entry) ||
                !(mapped->event.frontier.owner == binding.operation.owner) ||
                mapped->event.executeFrame != binding.operation.order.executeFrame) return false;
        }
        return true;
    }

    bool ProveConditionalTargetEvents(const Group& member, const Group& primary) const noexcept {
        if (immediateLane == nullptr ||
            !HasConditionalTargetSpans(member.view, *immediateLane, immediateLaneSlot, immediateLaneGeneration))
            return false;
        const auto targetEvents = ConditionalTargetEventCount(member.view, *immediateLane,
            immediateLaneSlot, immediateLaneGeneration);
        if (targetEvents == 0u) return false;
        std::uint32_t matchedEvents = 0;
        for (std::uint16_t index = 0; index < member.view.memberCount; ++index) {
            const ImmediateLaneEvent* mapped = nullptr;
            std::uint16_t span = 0;
            if (!immediateLane->FutureMember(immediateLaneSlot, immediateLaneGeneration,
                    member.view.identity, index, member.view.members[index], &mapped, &span) || (span != 1u && span != 2u))
                return false;
            const Event* start = nullptr;
            const Event* envelope = nullptr;
            for (std::uint32_t eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                const auto& event = events[eventIndex];
                const auto& source = event.source;
                if (!(source.frontier.group == member.view.identity) ||
                    !SameArm(source.armToken, member.view.members[index])) continue;
                const auto* program = source.program.program;
                const auto* payload = program != nullptr && program->payloads != nullptr &&
                    source.payload < program->payloadCount ? &program->payloads[source.payload] : nullptr;
                if (event.stop != nullptr || event.controller || event.group != primary.index || payload == nullptr ||
                    event.disposition != kTimelineDispositionApplied || !event.reserved ||
                    source.program.program != mapped->event.program.program ||
                    source.program.programGeneration != mapped->event.program.programGeneration ||
                    source.ownerInstanceHandle != mapped->event.ownerInstanceHandle ||
                    source.ownerInstanceScheduleEpoch != mapped->event.ownerInstanceScheduleEpoch ||
                    !(payload->target == mapped->event.program.program->payloads[mapped->event.payload].target) ||
                    source.frontier.operationEventCount != 1u + targetEvents ||
                    source.executeFrame != member.view.frontier->operation.order.executeFrame) return false;
                if (event.voice.kind == TimelineEventKind::Start) {
                    if (start != nullptr || !(event.order == OrderOf(mapped->event)) ||
                        payload->reserved != 0u) return false;
                    start = &event;
                } else if (event.voice.kind == TimelineEventKind::Envelope) {
                    if (envelope != nullptr || payload->envelopeDirection != TimelineEnvelopeDirection::FadeIn)
                        return false;
                    envelope = &event;
                } else return false;
                ++matchedEvents;
            }
            if (start == nullptr || (span == 2u && (envelope == nullptr || !(start->order < envelope->order))) ||
                (span == 1u && envelope != nullptr)) return false;
        }
        return matchedEvents == targetEvents;
    }

    bool HasConditionalNaturalSpans(const Plan03PageGroupView& view,
        const ImmediateActionLane& lane, std::uint32_t slot, std::uint32_t generation,
        Plan03PageGroupView* sourceOut = nullptr, Plan03PageGroupView* targetOut = nullptr) const noexcept {
        if (pool == nullptr || decisions == nullptr || view.frontier == nullptr || view.lease == nullptr ||
            view.members == nullptr || view.memberCount < 2u || view.sourceControlCount != 1u ||
            view.identity.backing != Plan03GroupBacking::ImmediateLane ||
            !view.frontier->queueConditional.Valid() || !view.frontier->transition.Valid() ||
            view.frontier->queueInitial.Valid() || view.frontier->variableSide.Valid()) return false;
        Plan03PageGroupView source{}, target{};
        std::uint32_t published = 0, sourceCount = 0, targetCount = 0;
        if (!lane.CapturePublishedGroupCount({slot, generation}, &published)) return false;
        for (std::uint32_t index = 0; index < published; ++index) {
            ImmediateLaneGroupRef descriptor{};
            if (!lane.CapturePublishedGroupDescriptor({slot, generation}, index, &descriptor)) return false;
            if (descriptor.identity.decision.slot != view.identity.decision.slot ||
                descriptor.identity.decision.generation != view.identity.decision.generation) continue;
            if (descriptor.identity.sliceIndex == 0u) {
                ++sourceCount;
                if (!pool->ResolvePlan03GroupSliceRt(descriptor.identity, &source)) return false;
            } else if (descriptor.identity.sliceIndex == 1u) {
                ++targetCount;
                if (!pool->ResolvePlan03GroupSliceRt(descriptor.identity, &target)) return false;
            } else return false;
        }
        if (sourceCount != 1u || targetCount != 1u || source.frontier == nullptr || target.frontier == nullptr ||
            source.lease == nullptr || target.lease == nullptr || source.members == nullptr || target.members == nullptr ||
            source.memberCount != view.memberCount || target.memberCount != view.memberCount ||
            source.sourceControlCount != 1u || target.sourceControlCount != 1u) return false;
        const auto& prelude = *source.frontier;
        const auto& completion = *target.frontier;
        const auto sameQueue = [](const auto& a, const auto& b) noexcept {
            return a.slot == b.slot && a.generation == b.generation && a.currentVersion == b.currentVersion &&
                a.pendingVersion == b.pendingVersion && a.current.slot == b.current.slot &&
                a.current.generation == b.current.generation && a.successor.slot == b.successor.slot &&
                a.successor.generation == b.successor.generation;
        };
        DecisionCommittedSnapshot decision{};
        const auto* activation = decisions->ResolveFrontierActivation(prelude.entry.coordinator);
        if (prelude.role != FrontierGroupRole::EntryMember || completion.role != FrontierGroupRole::Future ||
            !prelude.queueConditional.Valid() || !completion.queueConditional.Valid() ||
            prelude.queueInitial.Valid() || completion.queueInitial.Valid() ||
            prelude.variableSide.Valid() || completion.variableSide.Valid() ||
            prelude.operation.kind != FrontierOperationKind::Dependent ||
            completion.operation.kind != FrontierOperationKind::Dependent ||
            !(prelude.entry == completion.entry) || !(prelude.primary == completion.primary) ||
            !(prelude.operation.owner == completion.operation.owner) ||
            !(prelude.operation.group == source.identity) || !(completion.operation.group == target.identity) ||
            prelude.operation.world != completion.operation.world || prelude.operation.transport != completion.operation.transport ||
            !sameQueue(prelude.queueConditional, completion.queueConditional) ||
            prelude.transition.slot != completion.transition.slot ||
            prelude.transition.generation != completion.transition.generation ||
            prelude.transition.audibleFrame != completion.transition.audibleFrame ||
            prelude.operation.order.executeFrame != prelude.transition.audibleFrame ||
            completion.operation.order.executeFrame <= prelude.operation.order.executeFrame ||
            activation == nullptr || !(activation->Descriptor().entry == prelude.entry) ||
            !(activation->Descriptor().primary == prelude.primary) ||
            activation->Descriptor().order.executeFrame != prelude.operation.order.executeFrame ||
            activation->Descriptor().expectedMemberCount != 1u || activation->Descriptor().expectedEntryEventCount != 2u ||
            !decisions->CaptureCommitted(view.identity.decision, &decision) || decision.sliceCount != 2u ||
            decision.transitionGate.slot != prelude.transition.slot ||
            decision.transitionGate.generation != prelude.transition.generation ||
            !decisions->HasExclusiveParticipant(view.identity.decision,
                {DecisionParticipantKind::QueueInstance, prelude.queueConditional.slot, prelude.queueConditional.generation}))
            return false;
        for (std::uint16_t index = 0; index < view.memberCount; ++index) {
            if (!SameArm(source.members[index], target.members[index])) return false;
            if (index == 0u) continue;
            ImmediateLaneMemberManifest frozen{};
            const ImmediateLaneEvent* mapped = nullptr;
            std::uint16_t span = 0;
            if (!lane.CaptureFrozenFutureTarget({slot, generation}, source.identity, target.identity, &frozen, index) ||
                frozen.memberIndex != index || !SameArm(frozen.arm, target.members[index]) ||
                !lane.FutureMember(slot, generation, target.identity, index, target.members[index], &mapped, &span) ||
                mapped == nullptr || (span != 1u && span != 2u) || mapped->event.executeFrame != completion.operation.order.executeFrame ||
                !(mapped->event.frontier.entry == completion.entry) ||
                !(mapped->event.frontier.owner == completion.operation.owner)) return false;
        }
        if (sourceOut != nullptr) *sourceOut = source;
        if (targetOut != nullptr) *targetOut = target;
        return true;
    }

    bool ProveConditionalCompletionEvents(Group& group) noexcept {
        if (!group.conditionalQueueCompletion || immediateLane == nullptr ||
            !HasConditionalNaturalSpans(group.view, *immediateLane, immediateLaneSlot, immediateLaneGeneration))
            return false;
        const auto& binding = *group.view.frontier;
        const auto expectedEvents = ConditionalTargetEventCount(group.view, *immediateLane,
            immediateLaneSlot, immediateLaneGeneration);
        if (expectedEvents == 0u) return false;
        std::uint32_t matched = 0;
        for (std::uint16_t member = group.view.sourceControlCount; member < group.view.memberCount; ++member) {
            const ImmediateLaneEvent* mapped = nullptr;
            std::uint16_t span = 0;
            if (!immediateLane->FutureMember(immediateLaneSlot, immediateLaneGeneration, group.view.identity,
                    member, group.view.members[member], &mapped, &span) || mapped == nullptr || (span != 1u && span != 2u))
                return false;
            Event* start = nullptr;
            Event* envelope = nullptr;
            for (std::uint32_t index = 0; index < eventCount; ++index) {
                auto& event = events[index];
                if (event.group != group.index || !SameArm(event.source.armToken, group.view.members[member])) continue;
                const auto& source = event.source;
                const auto* program = source.program.program;
                const auto* payload = program != nullptr && program->payloads != nullptr && source.payload < program->payloadCount
                    ? &program->payloads[source.payload] : nullptr;
                AuthoredStopOperationIdentity stop{};
                if (event.stop != nullptr || event.controller || event.disposition != kTimelineDispositionApplied ||
                    payload == nullptr || program->role != TimelineProgramRole::Voice ||
                    program != mapped->event.program.program || program->generation != source.program.programGeneration ||
                    source.program.programGeneration != mapped->event.program.programGeneration ||
                    source.ownerInstanceHandle != mapped->event.ownerInstanceHandle ||
                    source.ownerInstanceScheduleEpoch != mapped->event.ownerInstanceScheduleEpoch ||
                    !(payload->target == mapped->event.program.program->payloads[mapped->event.payload].target) ||
                    !(source.frontier.group == group.view.identity) || !(source.frontier.entry == binding.entry) ||
                    !(source.frontier.owner == binding.operation.owner) || source.frontier.kind != binding.operation.kind ||
                    source.frontier.world != binding.operation.world || source.frontier.transport != binding.operation.transport ||
                    source.frontier.operationEventCount != expectedEvents || source.frontier.entryEventOrdinal >= expectedEvents ||
                    source.executeFrame != binding.operation.order.executeFrame || source.executeFrame >= CutoffFor(event, &stop) ||
                    !event.reserved || !voices.ProveTimelineReservation(event.reservation, &event.appendProof)) return false;
                for (std::uint32_t prior = 0; prior < index; ++prior)
                    if (events[prior].group == group.index &&
                        events[prior].source.frontier.entryEventOrdinal == source.frontier.entryEventOrdinal) return false;
                if (event.voice.kind == TimelineEventKind::Start) {
                    if (start != nullptr || payload->reserved != 0u || source.subeventPhase != 20u ||
                        !(event.order == OrderOf(mapped->event))) return false;
                    start = &event;
                } else if (event.voice.kind == TimelineEventKind::Envelope) {
                    if (envelope != nullptr || payload->envelopeDirection != TimelineEnvelopeDirection::FadeIn) return false;
                    envelope = &event;
                } else return false;
                ++matched;
            }
            if (start == nullptr || (span == 2u && (envelope == nullptr || !(start->order < envelope->order))) ||
                (span == 1u && envelope != nullptr) ||
                !arms.ProveGroupedFrontierOperation(start->source.armToken, binding, *group.view.lease, *pool,
                    quantum + 1u, true, &start->armProof)) return false;
        }
        return matched == expectedEvents;
    }

    bool ProveModeMember(Group& member, const Group* primary) noexcept {
        const auto& binding = *member.view.frontier;
        member.firstCompletionProof = completionCount;
        const bool variable = binding.variableSide.Valid();
        const bool queue = binding.queueInitial.Valid();
        const bool conditional = binding.queueConditional.Valid();
        const bool naturalConditional = laneMode && immediateLane != nullptr && conditional &&
            HasConditionalNaturalSpans(member.view, *immediateLane, immediateLaneSlot, immediateLaneGeneration);
        member.conditionalQueueCompletion = naturalConditional && binding.role == FrontierGroupRole::Future &&
            member.view.identity.sliceIndex == 1u;
        if (static_cast<unsigned>(variable) + static_cast<unsigned>(queue) + static_cast<unsigned>(conditional) != 1 ||
            (variable && variableControls == nullptr) || ((queue || conditional) && queues == nullptr) ||
            (conditional && occurrences == nullptr)) return false;
        const DecisionParticipantRef participant = variable ?
            DecisionParticipantRef{DecisionParticipantKind::Variable, binding.variableSide.slot, binding.variableSide.generation} :
            DecisionParticipantRef{DecisionParticipantKind::QueueInstance,
                conditional ? binding.queueConditional.slot : binding.queueInitial.slot,
                conditional ? binding.queueConditional.generation : binding.queueInitial.generation};
        DecisionCommittedSnapshot decision{};
        ArmGroupSnapshot armGroup{};
        if (armGroups == nullptr ||
            (primary != nullptr && (!(binding.primary == primary->view.identity) || member.view.lease != primary->view.lease ||
                binding.operation.order.executeFrame != primary->view.frontier->operation.order.executeFrame)) ||
            !decisions->CaptureCommitted(member.view.identity.decision, &decision) ||
            !decisions->HasExclusiveParticipant(member.view.identity.decision, participant) ||
            !armGroups->Capture(decision.armGroup, &armGroup) || armGroup.state != ArmGroupState::Ready ||
            armGroup.parent.slot != member.view.identity.decision.slot ||
            armGroup.parent.generation != member.view.identity.decision.generation ||
            armGroup.memberCount != member.view.memberCount || armGroup.ackedCount != member.view.memberCount)
            return false;
        if (decision.transitionGate.Valid() != binding.transition.Valid()) return false;
        if (variable && decision.sliceCount == 2 && member.view.sourceControlCount != 0) {
            if (!binding.transition.Valid() || binding.variableSide.side != 0 ||
                member.view.sourceControlCount == 0 ||
                member.view.sourceControlCount >= member.view.memberCount) return false;
            member.variablePrelude = member.view.identity.sliceIndex == 0;
            member.variableCompletion = member.view.identity.sliceIndex == 1;
            const auto frame = binding.operation.order.executeFrame;
            if ((member.variablePrelude && frame != binding.transition.audibleFrame) ||
                (member.variableCompletion && frame <= binding.transition.audibleFrame)) return false;
        }
        if (binding.transition.Valid()) {
            if (transitions == nullptr || decision.transitionGate.slot != binding.transition.slot ||
                decision.transitionGate.generation != binding.transition.generation) return false;
            const auto frame = binding.operation.order.executeFrame;
            const auto audible = binding.transition.audibleFrame;
            if (conditional && decision.sliceCount == 2) {
                if (member.view.sourceControlCount != 0 && member.view.sourceControlCount < member.view.memberCount) {
                    if (member.view.identity.sliceIndex == 0) {
                        if (frame != audible) return false;
                        member.queueSlice = QueueInstanceTable::FrontierConditionalSlice::Prelude;
                    } else if (frame <= audible) return false;
                } else if (member.view.sourceControlCount == 0 && member.view.memberCount == 1) {
                    if (member.view.identity.sliceIndex == 0) {
                        if (frame >= audible) return false;
                        member.delayedActivation = true;
                    } else {
                        if (frame != audible) return false;
                        member.queueSlice = QueueInstanceTable::FrontierConditionalSlice::TargetLock;
                    }
                } else return false;
            }
            if (laneMode && queue && decision.sliceCount == 2) {
                if (binding.variableSide.Valid() || binding.queueConditional.Valid() ||
                    member.view.identity.backing != Plan03GroupBacking::ImmediateLane ||
                    member.view.memberCount != 2u || member.view.sourceControlCount != 1u)
                    return false;
                member.initialQueuePrelude = binding.role == FrontierGroupRole::EntryMember &&
                    member.view.identity.sliceIndex == 0u && frame == audible;
                member.initialQueueCompletion = binding.role == FrontierGroupRole::Future &&
                    member.view.identity.sliceIndex == 1u && frame > audible;
                if (!member.initialQueuePrelude && !member.initialQueueCompletion) return false;
            }
            const bool locks = frame == audible;
            const auto expected = frame <= audible ? TransitionAudibleGateState::Replaceable :
                TransitionAudibleGateState::Locked;
            if ((frame < audible && (decision.sliceCount != 2 || member.view.identity.sliceIndex != 0)) ||
                !transitions->ProveFrontier(binding, *member.view.lease, expected, locks,
                    &member.transitionProof)) return false;
        }
        if (member.view.sourceControlCount != 0 &&
            !(conditional && binding.transition.Valid()) &&
            !member.initialQueuePrelude && !member.initialQueueCompletion &&
            !(variable && binding.variableSide.side == 0 &&
                (binding.transition.Valid() || member.view.sourceControlCount == member.view.memberCount ||
                    (binding.role == FrontierGroupRole::Future && decision.sliceCount == 1 &&
                        member.view.identity.sliceIndex == 0)))) return false;
        std::uint32_t controls = 0;
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            const auto& event = events[i];
            if (event.stop != nullptr || !(event.source.frontier.group == member.view.identity)) continue;
            if (!event.controller) {
                bool listed = false;
                for (std::uint16_t arm = 0; arm < member.view.memberCount; ++arm)
                    listed = listed || (!member.delayedActivation &&
                        (member.queueSlice != QueueInstanceTable::FrontierConditionalSlice::Prelude ||
                            arm < member.view.sourceControlCount) &&
                        (!member.variablePrelude || arm < member.view.sourceControlCount) &&
                        (!member.initialQueuePrelude || arm < member.view.sourceControlCount) &&
                        (!member.initialQueueCompletion || arm >= member.view.sourceControlCount) &&
                        (!member.conditionalQueueCompletion || arm >= member.view.sourceControlCount) &&
                        SameArm(event.source.armToken, member.view.members[arm]));
                if (!listed) return false;
                continue;
            }
            ++controls;
            if (!(event.order == binding.operation.order) ||
                event.source.program.program != binding.operation.program ||
                event.source.program.programGeneration != binding.operation.programGeneration) return false;
        }
        std::uint32_t groupEvents = 0;
        for (std::uint32_t i = 0; i < eventCount; ++i)
            groupEvents += events[i].stop == nullptr &&
                events[i].source.frontier.group == member.view.identity;
        const bool groupOnly = laneMode && groupEvents == 0 &&
            binding.role == FrontierGroupRole::EntryMember &&
            (queue || conditional);
        const bool conditionalTargetOnly = laneMode && primary != nullptr && controls == 0u &&
            groupEvents == ConditionalTargetEventCount(member.view, *immediateLane, immediateLaneSlot, immediateLaneGeneration) &&
            ProveConditionalTargetEvents(member, *primary);
        const bool initialQueueSourceOnly = member.initialQueuePrelude &&
            primary != nullptr && groupEvents == 1u && controls == 0u;
        const bool initialQueueTargetOnly = member.initialQueueCompletion &&
            primary == nullptr && groupEvents == 2u && controls == 0u;
        const bool conditionalSourceOnly = naturalConditional && primary != nullptr && controls == 0u &&
            member.queueSlice == QueueInstanceTable::FrontierConditionalSlice::Prelude && groupEvents == 1u;
        const bool conditionalCompletionOnly = member.conditionalQueueCompletion && primary == nullptr && controls == 0u &&
            groupEvents == ConditionalTargetEventCount(member.view, *immediateLane, immediateLaneSlot, immediateLaneGeneration);
        if (controls != 1 && !(groupOnly && controls == 0) && !initialQueueSourceOnly && !initialQueueTargetOnly &&
            !conditionalTargetOnly && !conditionalSourceOnly && !conditionalCompletionOnly) return false;
        for (std::uint16_t index = 0; index < member.view.memberCount; ++index) {
            const auto token = member.view.members[index];
            ArmGroupMemberSnapshot actual{};
            if (!armGroups->CaptureMember(decision.armGroup, index, &actual) ||
                !SameArm(actual.token, token) || !actual.acked ||
                (actual.tombstoned && !((member.initialQueueCompletion || member.conditionalQueueCompletion) && index == 0u))) return false;
            bool found = false;
            for (std::uint32_t event = 0; event < eventCount; ++event) {
                const auto& candidate = events[event];
                found = found || (candidate.stop == nullptr && candidate.source.frontier.group == member.view.identity &&
                    candidate.source.frontier.kind == binding.operation.kind &&
                    SameArm(candidate.source.armToken, token));
            }
            const bool required = !member.delayedActivation &&
                (!member.variablePrelude || index < member.view.sourceControlCount) &&
                (!member.variableCompletion || index >= member.view.sourceControlCount) &&
                (!member.initialQueuePrelude || index < member.view.sourceControlCount) &&
                (!member.initialQueueCompletion || index >= member.view.sourceControlCount) &&
                (!member.conditionalQueueCompletion || index >= member.view.sourceControlCount) &&
                (member.queueSlice != QueueInstanceTable::FrontierConditionalSlice::Prelude ||
                    index < member.view.sourceControlCount);
            if (required && !found) {
                const ImmediateLaneEvent* future = nullptr;
                if (laneMode && immediateLane != nullptr &&
                    immediateLane->FutureMember(immediateLaneSlot,
                        immediateLaneGeneration,
                        member.view.identity, index, token, &future) &&
                    future != nullptr &&
                    future->event.frontier.entry == binding.entry &&
                    future->event.frontier.owner == binding.operation.owner &&
                    future->event.frontier.kind == FrontierOperationKind::Dependent &&
                    (future->event.executeFrame > binding.operation.order.executeFrame ||
                     (future->event.executeFrame == binding.operation.order.executeFrame &&
                      binding.operation.kind == FrontierOperationKind::Dependent)))
                    continue;
                if (groupOnly && index < member.view.sourceControlCount) {
                    if (completionCount == completionCapacity ||
                        !arms.ProveGroupedFrontierOperation(token, binding,
                            *member.view.lease, *pool, quantum + 1u,
                            false, &completionProofs[completionCount]))
                        return false;
                    ++completionCount;
                    continue;
                }
                if (!variable || binding.role != FrontierGroupRole::Future ||
                    index < member.view.sourceControlCount || completionCount == completionCapacity ||
                    !arms.ProveDeferredFrontierStart(token, binding, *member.view.lease, *pool,
                        quantum + 1u, &completionProofs[completionCount])) return false;
                ++completionCount;
                ++deferredStartCount;
            } else if (!required && found && !(member.variableCompletion && index < member.view.sourceControlCount)) {
                return false;
            }
        }
        member.completionProofCount =
            completionCount - member.firstCompletionProof;
        if (!decisions->ValidateFrontierMemberDecisionApply(binding, *member.view.lease, quantum + 1u,
            &member.decisionProof)) return false;
        if (variable) return variableControls->ProveFrontierSide(binding, *member.view.lease, &member.sideProof);
        if (member.initialQueueCompletion) {
            if (immediateLane == nullptr || decision.consumedSlices != 1u ||
                decision.lastAudioSequence == 0u || decision.lastAudioSequence > quantum) return false;
            std::uint32_t publishedCount = 0;
            if (!immediateLane->CapturePublishedGroupCount(
                    {immediateLaneSlot, immediateLaneGeneration}, &publishedCount)) return false;
            Plan03PageGroupView source{};
            std::uint32_t matches = 0;
            for (std::uint32_t index = 0; index < publishedCount; ++index) {
                ImmediateLaneGroupRef descriptor{};
                if (!immediateLane->CapturePublishedGroupDescriptor(
                        {immediateLaneSlot, immediateLaneGeneration}, index, &descriptor)) return false;
                if (descriptor.identity.decision.slot != member.view.identity.decision.slot ||
                    descriptor.identity.decision.generation != member.view.identity.decision.generation ||
                    descriptor.identity.sliceIndex != 0u) continue;
                ++matches;
                ImmediateLaneMemberManifest manifest{};
                if (!immediateLane->CaptureFrozenFutureTarget(
                        {immediateLaneSlot, immediateLaneGeneration}, descriptor.identity,
                        member.view.identity, &manifest) ||
                    !pool->ResolvePlan03GroupSliceRt(descriptor.identity, &source) ||
                    source.memberCount != member.view.memberCount ||
                    source.sourceControlCount != member.view.sourceControlCount ||
                    !SameArm(source.members[0], member.view.members[0]) ||
                    !SameArm(source.members[1], member.view.members[1]) ||
                    !SameArm(manifest.arm, member.view.members[1]) ||
                    !pool->NotePlan03GroupReadRt(descriptor.identity, quantum + 1u)) return false;
            }
            if (occurrences == nullptr || quantum > UINT64_MAX / frames ||
                quantum * frames > UINT64_MAX - frames || matches != 1u ||
                !queues->ProveFrontierInitialCompletion(binding,
                    source, *member.view.lease, *pool, decision.lastAudioSequence,
                    *occurrences, composites, quantum * frames, quantum * frames + frames,
                    quantum, &member.queueCompletionProof)) return false;
            ArmGroupMemberSnapshot historical{};
            if (!armGroups->CaptureMember(decision.armGroup, 0u, &historical) ||
                !SameArm(historical.token, member.view.members[0])) return false;
            if (!historical.tombstoned) {
                if (completionCount == completionCapacity ||
                    !arms.ProveGroupedFrontierOperation(historical.token, binding,
                        *member.view.lease, *pool, quantum + 1u, false,
                        &completionProofs[completionCount])) return false;
                ++completionCount;
                member.completionProofCount = completionCount - member.firstCompletionProof;
            }
            return true;
        }
        if (member.conditionalQueueCompletion) {
            Plan03PageGroupView source{};
            TimelineArmObservation historical{};
            if (decision.consumedSlices != 1u || decision.lastAudioSequence == 0u || decision.lastAudioSequence > quantum ||
                !HasConditionalNaturalSpans(member.view, *immediateLane, immediateLaneSlot, immediateLaneGeneration, &source) ||
                source.lease->Capture().state != AudioSliceState::SliceApplied ||
                !pool->NotePlan03GroupReadRt(source.identity, quantum + 1u) ||
                !arms.Observe(member.view.members[0], &historical))
                return false;
            if (historical.state == ArmState::Tombstoned) {
                if (historical.logicalCompletionSequence == 0u || historical.logicalCompletionSequence > quantum)
                    return false;
            } else {
                if (completionCount == completionCapacity ||
                    !arms.ProveGroupedFrontierOperation(member.view.members[0], binding,
                        *member.view.lease, *pool, quantum + 1u, false,
                        &completionProofs[completionCount])) return false;
                ++completionCount;
                member.completionProofCount = completionCount - member.firstCompletionProof;
            }
        }
        if (queue) return queues->ProveFrontierInitial(binding, *member.view.lease, quantum + 1u, &member.queueProof);
        if (quantum > UINT64_MAX / frames || quantum * frames > UINT64_MAX - frames ||
            !queues->ProveFrontierConditional(binding, *member.view.lease, *occurrences, composites,
                quantum * frames, quantum * frames + frames, quantum, &member.conditionalProof, member.queueSlice)) return false;
        if (member.queueSlice != QueueInstanceTable::FrontierConditionalSlice::Activation) return true;
        const auto& proof = member.conditionalProof.Completion();
        if (proof.completionMode != QueueConditionalCompletionMode::RetiredSource) return true;
        if (member.view.memberCount == 0 ||
            (member.view.sourceControlCount != 0 && !member.conditionalQueueCompletion)) return false;
        for (std::uint32_t index = 0; index < member.view.memberCount; ++index)
            if (SameArm(proof.retiredSourceArm, member.view.members[index])) return false;
        return proof.retiredSourceTombstoneAudioSequence <= quantum &&
            occurrences->StageOf(proof.currentOccurrence) == OccurrenceStage::AwaitingEventFence &&
            occurrences->StageOf(proof.successorOccurrence) == OccurrenceStage::Preparing &&
            voices.Resolve(proof.retiredSourceRenderHandle) == nullptr;
    }

    bool ProveInitialQueueCompletionEvents(Group& group) noexcept {
        if (!group.initialQueueCompletion)
            return false;
        const auto& binding = *group.view.frontier;
        Event* start = nullptr;
        Event* envelope = nullptr;
        const ImmediateLaneEvent* mapped = nullptr;
        if (!immediateLane->FutureMember(immediateLaneSlot, immediateLaneGeneration,
                group.view.identity, 1u, group.view.members[1], &mapped) || mapped == nullptr)
            return false;
        std::uint32_t completionEventCount = 0;
        for (std::uint32_t index = 0; index < eventCount; ++index) {
            auto& event = events[index];
            if (event.group != group.index) continue;
            ++completionEventCount;
            const auto& source = event.source;
            const auto* program = source.program.program;
            const auto* payload = program != nullptr && program->payloads != nullptr &&
                source.payload < program->payloadCount ? &program->payloads[source.payload] : nullptr;
            AuthoredStopOperationIdentity stop{};
            if (event.stop != nullptr || event.controller || event.group != group.index ||
                event.disposition != kTimelineDispositionApplied || payload == nullptr ||
                program->role != TimelineProgramRole::Voice ||
                program->generation != source.program.programGeneration ||
                source.program.programGeneration != mapped->event.program.programGeneration ||
                !(payload->target == mapped->event.program.program->payloads[mapped->event.payload].target) ||
                !(source.frontier.group == group.view.identity) ||
                !(source.frontier.entry == binding.entry) ||
                !(source.frontier.owner == binding.operation.owner) ||
                source.frontier.kind != binding.operation.kind ||
                source.frontier.world != binding.operation.world ||
                source.frontier.transport != binding.operation.transport ||
                source.frontier.operationEventCount != 2u ||
                source.executeFrame != binding.operation.order.executeFrame ||
                !SameArm(source.armToken, group.view.members[1]) ||
                source.executeFrame >= CutoffFor(event, &stop) ||
                !event.reserved || !voices.ProveTimelineReservation(event.reservation, &event.appendProof))
                return false;
            if (event.voice.kind == TimelineEventKind::Start) {
                if (start != nullptr || payload->reserved != kTimelineEventTransientStartArm ||
                    source.subeventPhase != 20u || !(event.order == OrderOf(mapped->event))) return false;
                start = &event;
            } else if (event.voice.kind == TimelineEventKind::Envelope) {
                if (envelope != nullptr) return false;
                envelope = &event;
            } else return false;
        }
        return completionEventCount == 2u && start != nullptr && envelope != nullptr && start->order < envelope->order &&
            arms.ProveLaneGroupedFutureStart(start->source.armToken, binding,
                *group.view.lease, *pool, quantum + 1u, 1u,
                mapped->armProgramGeneration,
                mapped->event.program.program->payloads[mapped->event.payload].target,
                &start->armProof);
    }

    bool ProveFrozenQueueChild(const Group& parent, const Group& side,
        const ImmediateLaneGroupRef& controlGroup, const Plan03PageGroupView& controlView,
        std::uint32_t publishedCount) noexcept {
        const auto& control = *controlView.frontier;
        const auto& prerequisite = *side.view.frontier;
        const auto& owner = control.operation.owner;
        const auto& parentOwner = prerequisite.operation.owner;
        const auto* activation = decisions->ResolveFrontierActivation(control.entry.coordinator);
        if (parent.failure != FrontierReceiptReason::None ||
            parent.activation->Descriptor().expectedMemberCount > participantCapacity ||
            activation == nullptr || armGroups == nullptr || queues == nullptr ||
            controlView.lease == nullptr || side.view.lease == nullptr ||
            control.role != FrontierGroupRole::Coordinator ||
            control.operation.kind != FrontierOperationKind::Entry ||
            !(control.entry.owner == owner) || !owner.Valid() || !parentOwner.Valid() ||
            !(control.prerequisite == parent.view.frontier->entry) ||
            !(control.prerequisiteMember == side.view.identity) ||
            prerequisite.role != FrontierGroupRole::EntryMember ||
            !prerequisite.variableSide.Valid() || prerequisite.queueInitial.Valid() ||
            prerequisite.queueConditional.Valid() ||
            !(prerequisite.entry == control.prerequisite) ||
            !(prerequisite.primary == parent.view.identity) ||
            !(prerequisite.operation.group == side.view.identity) ||
            owner.parent != parentOwner.child || owner.parentEpoch != parentOwner.childEpoch ||
            owner.invocation != parentOwner.invocation ||
            owner.invocationEpoch != parentOwner.invocationEpoch ||
            control.operation.world != prerequisite.operation.world ||
            control.operation.transport != prerequisite.operation.transport ||
            !(control.primary == controlGroup.identity) ||
            !(control.operation.group == controlGroup.identity) ||
            controlGroup.dueQuantum < quantum ||
            controlGroup.dueQuantum != control.operation.order.executeFrame / frames ||
            !(prerequisite.operation.order < control.operation.order) ||
            side.view.lease->Capture().state != AudioSliceState::Ready ||
            controlView.lease->Capture().state != AudioSliceState::Ready) return false;
        const auto& descriptor = activation->Descriptor();
        if (!descriptor.Valid() || !(descriptor.entry == control.entry) ||
            !(descriptor.primary == controlGroup.identity) ||
            descriptor.order != control.operation.order ||
            descriptor.worldGeneration != control.operation.world ||
            descriptor.primary.transportEpoch != control.operation.transport ||
            descriptor.expectedMemberCount != 1u || descriptor.expectedEntryEventCount != 2u)
            return false;
        Plan03PageGroupView source{}, target{};
        std::uint32_t controls = 0, sources = 0, targets = 0, sideParticipants = 0;
        for (std::uint32_t index = 0; index < parent.activation->Descriptor().expectedMemberCount; ++index)
            sideParticipants += participants[index].kind == DecisionParticipantKind::Subdecision &&
                participants[index].slot == side.view.identity.decision.slot &&
                participants[index].generation == side.view.identity.decision.generation;
        if (sideParticipants != 1u) return false;
        const ImmediateLaneHandle lane{immediateLaneSlot, immediateLaneGeneration};
        for (std::uint32_t index = 0; index < publishedCount; ++index) {
            ImmediateLaneGroupRef declaration{};
            Plan03PageGroupView view{};
            if (!immediateLane->CapturePublishedGroupDescriptor(lane, index, &declaration) ||
                !pool->ResolvePlan03GroupSliceRt(declaration.identity, &view) ||
                view.frontier == nullptr) return false;
            const auto& binding = *view.frontier;
            if (!(binding.entry == control.entry)) continue;
            if (!(binding.primary == controlGroup.identity) ||
                !(binding.operation.owner == owner) ||
                !(binding.operation.group == declaration.identity) ||
                binding.operation.world != descriptor.worldGeneration ||
                binding.operation.transport != descriptor.primary.transportEpoch ||
                declaration.dueQuantum != binding.operation.order.executeFrame / frames ||
                view.lease == nullptr || view.lease->Capture().state != AudioSliceState::Ready)
                return false;
            if (binding.role == FrontierGroupRole::Coordinator) {
                if (!(declaration.identity == controlGroup.identity)) return false;
                ++controls;
                continue;
            }
            if (!binding.queueInitial.Valid() || binding.variableSide.Valid() ||
                binding.queueConditional.Valid() || !binding.transition.Valid() ||
                binding.operation.kind != FrontierOperationKind::Dependent ||
                declaration.identity.backing != Plan03GroupBacking::ImmediateLane ||
                view.memberCount != 2u || view.sourceControlCount != 1u || view.members == nullptr ||
                binding.operation.order < descriptor.order) return false;
            if (binding.role == FrontierGroupRole::EntryMember) {
                if (declaration.identity.sliceIndex != 0u ||
                    declaration.dueQuantum != controlGroup.dueQuantum ||
                    binding.operation.order.executeFrame != descriptor.order.executeFrame ||
                    binding.operation.order.executeFrame != binding.transition.audibleFrame ||
                    view.lease != controlView.lease) return false;
                source = view;
                ++sources;
            } else if (binding.role == FrontierGroupRole::Future) {
                if (declaration.identity.sliceIndex != 1u ||
                    declaration.dueQuantum <= controlGroup.dueQuantum ||
                    binding.operation.order.executeFrame <= binding.transition.audibleFrame)
                    return false;
                target = view;
                ++targets;
            } else return false;
        }
        if (controls != 1u || sources != 1u || targets != 1u ||
            source.identity.decision.slot != target.identity.decision.slot ||
            source.identity.decision.generation != target.identity.decision.generation ||
            !(source.frontier->queueInitial == target.frontier->queueInitial) ||
            source.frontier->transition.slot != target.frontier->transition.slot ||
            source.frontier->transition.generation != target.frontier->transition.generation ||
            source.frontier->transition.audibleFrame != target.frontier->transition.audibleFrame ||
            !SameArm(source.members[0], target.members[0]) ||
            !SameArm(source.members[1], target.members[1])) return false;
        DecisionCommittedSnapshot decision{};
        ArmGroupSnapshot armsSnapshot{};
        QueueInstanceSnapshot queue{};
        const auto& initial = source.frontier->queueInitial;
        const DecisionParticipantRef queueParticipant{
            DecisionParticipantKind::QueueInstance, initial.slot, initial.generation};
        DecisionParticipantRef childParticipant{};
        std::uint32_t childParticipantCount = 0;
        if (!decisions->CopyCommittedParticipants(control.entry.coordinator,
                &childParticipant, 1u, &childParticipantCount) || childParticipantCount != 1u ||
            childParticipant.kind != DecisionParticipantKind::Subdecision ||
            childParticipant.slot != source.identity.decision.slot ||
            childParticipant.generation != source.identity.decision.generation ||
            !decisions->CaptureCommitted(source.identity.decision, &decision) ||
            !decisions->HasExclusiveParticipant(source.identity.decision, queueParticipant) ||
            decision.sliceCount != 2u ||
            !armGroups->Capture(decision.armGroup, &armsSnapshot) ||
            armsSnapshot.state != ArmGroupState::Ready || armsSnapshot.memberCount != 2u ||
            armsSnapshot.ackedCount != 2u ||
            armsSnapshot.parent.slot != source.identity.decision.slot ||
            armsSnapshot.parent.generation != source.identity.decision.generation ||
            !queues->Capture({initial.slot, initial.generation}, &queue) ||
            queue.logicMirror.version != initial.version ||
            queue.programSemanticFingerprint != initial.programSemanticFingerprint ||
            queue.rootInstance != owner.child || queue.world != descriptor.worldGeneration)
            return false;
        for (std::uint16_t index = 0; index < 2u; ++index) {
            ArmGroupMemberSnapshot actual{};
            if (!armGroups->CaptureMember(decision.armGroup, index, &actual) ||
                !actual.acked || actual.tombstoned || !SameArm(actual.token, source.members[index]))
                return false;
        }
        ImmediateLaneMemberManifest manifest{};
        if (!immediateLane->CaptureFrozenFutureTarget(lane, source.identity, target.identity, &manifest)) {
            return false;
        }
        const auto* targetArm = arms.Resolve(manifest.arm);
        const bool valid = SameArm(manifest.arm, source.members[1]) && targetArm != nullptr &&
            targetArm->programGeneration == manifest.programGeneration && targetArm->target == manifest.target &&
            targetArm->frontierAdmission == target.frontier && targetArm->frontierAdmissionLease == target.lease &&
            targetArm->parentGroup.decision.slot == target.identity.decision.slot &&
            targetArm->parentGroup.decision.generation == target.identity.decision.generation &&
            targetArm->memberIndex == 1u;
        return valid;
    }

    void ProveEntry(std::uint32_t index) noexcept {
        auto& group = groups[index];
        const auto& binding = *group.view.frontier;
        group.activation = decisions->ResolveFrontierActivation(binding.entry.coordinator);
        if (group.activation == nullptr || !(group.activation->Descriptor().primary == group.view.identity)) {
            group.failure = FrontierReceiptReason::Identity;
        } else {
            group.audioWriter = group.activation->TryBeginAudio(binding.entry);
            if (!group.audioWriter) group.failure = FrontierReceiptReason::Cancelled;
        }
        std::uint32_t count = 0;
        std::uint32_t controllerCount = 0;
        if (group.audioWriter) {
            const auto& descriptor = group.activation->Descriptor();
            if (binding.operation.order != descriptor.order ||
                binding.operation.world != descriptor.worldGeneration ||
                binding.operation.transport != descriptor.primary.transportEpoch ||
                group.view.memberCount != 0 || group.view.sourceControlCount != 0 ||
                descriptor.order.executeFrame / frames != quantum)
                group.failure = FrontierReceiptReason::Identity;
            std::uint32_t participantCount = 0;
            if (!decisions->CopyCommittedParticipants(binding.entry.coordinator, participants.get(),
                    participantCapacity, &participantCount) || participantCount != descriptor.expectedMemberCount)
                group.failure = FrontierReceiptReason::MissingMember;
            std::uint32_t memberCount = 0;
            if (laneMode) {
                const ImmediateLaneHandle laneHandle{immediateLaneSlot, immediateLaneGeneration};
                std::uint32_t queueSourceCount = 0;
                for (std::uint32_t candidate = 0; candidate < groupCount; ++candidate) {
                    const auto& possible = groups[candidate];
                    queueSourceCount += possible.view.frontier->role == FrontierGroupRole::EntryMember &&
                        possible.view.frontier->queueInitial.Valid() &&
                        possible.view.frontier->entry == binding.entry;
                }
                const auto sourceFor = [&](const FrontierGroupBinding& declared) noexcept -> const Group* {
                    const Group* source = nullptr;
                    for (std::uint32_t candidate = 0; candidate < groupCount; ++candidate) {
                        const auto& possible = groups[candidate];
                        const auto& initial = *possible.view.frontier;
                        if (initial.role != FrontierGroupRole::EntryMember ||
                            !initial.queueInitial.Valid() || !(initial.entry == binding.entry)) continue;
                        const bool matches = declared.role == FrontierGroupRole::Coordinator
                            ? declared.prerequisite == binding.entry &&
                              declared.prerequisiteMember == possible.view.identity
                            : declared.role == FrontierGroupRole::Future &&
                              declared.entry == binding.entry &&
                              declared.operation.owner == initial.operation.owner &&
                              declared.queueInitial == initial.queueInitial;
                        if (!matches) continue;
                        if (source != nullptr) return nullptr;
                        source = &possible;
                    }
                    return source;
                };
                std::uint32_t publishedCount = 0;
                if (immediateLane == nullptr ||
                    !immediateLane->CapturePublishedGroupCount(laneHandle, &publishedCount))
                    group.failure = FrontierReceiptReason::Identity;
                for (std::uint32_t published = 0; published < publishedCount;
                     ++published) {
                    ImmediateLaneGroupRef declaredGroup{};
                    Plan03PageGroupView declaredView{};
                    if (!immediateLane->CapturePublishedGroupDescriptor(
                            laneHandle, published, &declaredGroup) ||
                        !pool->ResolvePlan03GroupSliceRt(
                            declaredGroup.identity, &declaredView) ||
                        declaredView.frontier == nullptr) {
                        group.failure = FrontierReceiptReason::Identity;
                        continue;
                    }
                    const auto& declared = *declaredView.frontier;
                    const auto* queueSource = sourceFor(declared);
                    if (declared.role == FrontierGroupRole::Coordinator &&
                        declared.prerequisite == binding.entry) {
                        const Group* variableSource = nullptr;
                        std::uint32_t variableSources = 0;
                        for (std::uint32_t candidate = 0; candidate < groupCount; ++candidate) {
                            const auto& possible = groups[candidate];
                            const auto& source = *possible.view.frontier;
                            if (!(possible.view.identity == declared.prerequisiteMember) ||
                                source.role != FrontierGroupRole::EntryMember ||
                                !(source.entry == binding.entry) || !source.variableSide.Valid()) continue;
                            variableSource = &possible;
                            ++variableSources;
                        }
                        if (variableSources != 0u) {
                            if (variableSources != 1u || queueSource != nullptr ||
                                !ProveFrozenQueueChild(group, *variableSource,
                                    declaredGroup, declaredView, publishedCount))
                                group.failure = FrontierReceiptReason::Identity;
                            continue;
                        }
                        if (queueSource == nullptr) {
                            group.failure = FrontierReceiptReason::Identity;
                            continue;
                        }
                        std::uint32_t dependentMembers = 0;
                        ImmediateLaneGroupRef dependentDeclaration{};
                        for (std::uint32_t candidate = 0;
                             candidate < publishedCount; ++candidate) {
                            ImmediateLaneGroupRef dependentGroup{};
                            Plan03PageGroupView dependentView{};
                            if (!immediateLane->CapturePublishedGroupDescriptor(
                                    laneHandle, candidate, &dependentGroup) ||
                                !pool->ResolvePlan03GroupSliceRt(
                                    dependentGroup.identity, &dependentView) ||
                                dependentView.frontier == nullptr) {
                                group.failure = FrontierReceiptReason::Identity;
                                continue;
                            }
                            const auto& dependent = *dependentView.frontier;
                            if (dependentGroup.identity == declaredGroup.identity)
                                continue;
                            if (!(dependent.entry == declared.entry)) continue;
                            if (dependent.role == FrontierGroupRole::Future &&
                                HasConditionalNaturalSpans(dependentView, *immediateLane, immediateLaneSlot, immediateLaneGeneration))
                                continue;
                            ++dependentMembers;
                            dependentDeclaration = dependentGroup;
                            const auto& source = *queueSource->view.frontier;
                            if (dependent.role != FrontierGroupRole::EntryMember ||
                                !(dependent.primary == declaredGroup.identity) ||
                                !(dependent.operation.group == dependentGroup.identity) ||
                                !(dependent.operation.owner == source.operation.owner) ||
                                dependent.operation.kind != FrontierOperationKind::Dependent ||
                                dependentGroup.dueQuantum != declaredGroup.dueQuantum ||
                                dependent.operation.order.executeFrame / frames !=
                                    dependentGroup.dueQuantum ||
                                dependent.operation.order < declared.operation.order ||
                                !dependent.queueConditional.Valid() ||
                                dependent.queueConditional.slot != source.queueInitial.slot ||
                                dependent.queueConditional.generation !=
                                    source.queueInitial.generation ||
                                dependent.queueConditional.currentVersion !=
                                    source.queueInitial.version)
                                group.failure = FrontierReceiptReason::Identity;
                        }
                        const bool withdrawnSameBucket = dependentMembers == 1u &&
                            declaredGroup.dueQuantum == quantum &&
                            OriginalImmediateLaneConditionalProjection::CapturedWithdrawalExcludesPair(
                                *immediateLane, laneHandle, immediateLaneBucket,
                                *pool, *decisions, declaredGroup, dependentDeclaration);
                        const auto& source = *queueSource->view.frontier;
                        const auto* declaredActivation = decisions->ResolveFrontierActivation(
                            declared.entry.coordinator);
                        if (dependentMembers != 1 ||
                            !(declared.prerequisiteMember == queueSource->view.identity) ||
                            declaredActivation == nullptr ||
                            !declaredActivation->Descriptor().Valid() ||
                            !(declaredActivation->Descriptor().entry == declared.entry) ||
                            !(declaredActivation->Descriptor().primary == declaredGroup.identity) ||
                            !(declaredActivation->Descriptor().order == declared.operation.order) ||
                            declaredActivation->Descriptor().worldGeneration != declared.operation.world ||
                            declaredActivation->Descriptor().expectedMemberCount != 1u ||
                            (declaredActivation->Descriptor().expectedEntryEventCount != 1u &&
                             !([&]() noexcept {
                                 Plan03PageGroupView target{};
                                 return pool->ResolvePlan03GroupSliceRt(dependentDeclaration.identity, &target) &&
                                     HasConditionalTargetSpans(target, *immediateLane, immediateLaneSlot, immediateLaneGeneration) &&
                                     declaredActivation->Descriptor().expectedEntryEventCount == 1u +
                                         ConditionalTargetEventCount(target, *immediateLane, immediateLaneSlot, immediateLaneGeneration);
                             })() &&
                             !([&]() noexcept {
                                 Plan03PageGroupView prelude{};
                                 return pool->ResolvePlan03GroupSliceRt(dependentDeclaration.identity, &prelude) &&
                                     HasConditionalNaturalSpans(prelude, *immediateLane, immediateLaneSlot, immediateLaneGeneration) &&
                                     declaredActivation->Descriptor().expectedEntryEventCount == 2u;
                             })()) ||
                            declared.operation.world != source.operation.world ||
                            declared.operation.transport != source.operation.transport ||
                            !(declared.entry.owner == source.operation.owner) ||
                            !(declared.operation.owner == source.operation.owner) ||
                            !(declared.primary == declaredGroup.identity) ||
                            !(declared.operation.group == declaredGroup.identity) ||
                            (declaredGroup.dueQuantum <= quantum && !withdrawnSameBucket) ||
                            declaredGroup.dueQuantum !=
                                declared.operation.order.executeFrame / frames ||
                            !(descriptor.order < declared.operation.order))
                            group.failure = FrontierReceiptReason::Identity;
                        continue;
                    }
                    if (!(declared.entry == binding.entry)) continue;
                    if (declared.role == FrontierGroupRole::Future && declared.queueConditional.Valid()) {
                        if (!HasConditionalNaturalSpans(declaredView, *immediateLane, immediateLaneSlot, immediateLaneGeneration) ||
                            !(declared.primary == descriptor.primary) || declaredGroup.dueQuantum <= quantum ||
                            !(descriptor.order < declared.operation.order))
                            group.failure = FrontierReceiptReason::Identity;
                        continue;
                    }
                    if (declared.role == FrontierGroupRole::Future && queueSourceCount != 0) {
                        if (queueSource == nullptr) {
                            group.failure = FrontierReceiptReason::Identity;
                            continue;
                        }
                        ImmediateLaneMemberManifest manifest{};
                        if (
                            !(declared.primary == descriptor.primary) ||
                            !(declared.operation.group == declaredGroup.identity) ||
                            !(declared.operation.owner ==
                                queueSource->view.frontier->operation.owner) ||
                            declared.queueInitial.slot !=
                                queueSource->view.frontier->queueInitial.slot ||
                            declared.queueInitial.generation !=
                                queueSource->view.frontier->queueInitial.generation ||
                            declaredGroup.dueQuantum <= quantum ||
                            !(descriptor.order < declared.operation.order) ||
                            !immediateLane->CaptureFrozenFutureTarget(laneHandle,
                                queueSource->view.identity, declaredGroup.identity,
                                &manifest))
                            group.failure = FrontierReceiptReason::Identity;
                        continue;
                    }
                    if (declared.role == FrontierGroupRole::Coordinator) {
                        const auto* activation =
                            decisions->ResolveFrontierActivation(
                                declared.entry.coordinator);
                        if (activation == nullptr ||
                            !(activation->Descriptor().entry == declared.entry) ||
                            !(activation->Descriptor().primary ==
                                declaredGroup.identity) ||
                            declaredGroup.dueQuantum !=
                                declared.operation.order.executeFrame / frames)
                            group.failure = FrontierReceiptReason::Identity;
                        continue;
                    }
                    if (declared.role != FrontierGroupRole::EntryMember) {
                        group.failure = FrontierReceiptReason::Identity;
                        continue;
                    }
                    std::uint32_t owners = 0;
                    for (std::uint32_t candidate = 0;
                         candidate < publishedCount; ++candidate) {
                        ImmediateLaneGroupRef coordinatorGroup{};
                        Plan03PageGroupView coordinatorView{};
                        if (!immediateLane->CapturePublishedGroupDescriptor(
                                laneHandle, candidate, &coordinatorGroup) ||
                            !pool->ResolvePlan03GroupSliceRt(
                                coordinatorGroup.identity,
                                &coordinatorView) ||
                            coordinatorView.frontier == nullptr) {
                            group.failure = FrontierReceiptReason::Identity;
                            continue;
                        }
                        const auto& coordinator = *coordinatorView.frontier;
                        owners += coordinator.role ==
                                FrontierGroupRole::Coordinator &&
                            coordinator.entry == declared.entry &&
                            coordinatorGroup.identity == declared.primary &&
                            coordinatorGroup.dueQuantum <=
                                declaredGroup.dueQuantum;
                    }
                    if (owners != 1)
                        group.failure = FrontierReceiptReason::Identity;
                }
                std::uint32_t primaryCount = 0;
                for (std::uint32_t published = 0; published < publishedCount; ++published) {
                    ImmediateLaneGroupRef declaredGroup{};
                    if (!immediateLane->CapturePublishedGroupDescriptor(
                            laneHandle, published, &declaredGroup)) {
                        group.failure = FrontierReceiptReason::Identity;
                        continue;
                    }
                    Plan03PageGroupView member{};
                    if (!pool->ResolvePlan03GroupSliceRt(declaredGroup.identity, &member) ||
                        member.frontier == nullptr) {
                        group.failure = FrontierReceiptReason::MissingMember;
                        continue;
                    }
                    const auto& memberBinding = *member.frontier;
                    if (member.identity == descriptor.primary) {
                        ++primaryCount;
                        if (memberBinding.role != FrontierGroupRole::Coordinator ||
                            !(memberBinding.entry == binding.entry) ||
                            declaredGroup.dueQuantum != quantum ||
                            !(memberBinding.operation.group == descriptor.primary))
                            group.failure = FrontierReceiptReason::Identity;
                        continue;
                    }
                    if (!(memberBinding.entry == binding.entry)) continue;
                    if (memberBinding.role == FrontierGroupRole::Future &&
                        (queueSourceCount != 0 ||
                         HasConditionalNaturalSpans(member, *immediateLane, immediateLaneSlot, immediateLaneGeneration))) continue;
                    if (memberBinding.role != FrontierGroupRole::EntryMember) {
                        group.failure = FrontierReceiptReason::Identity;
                        continue;
                    }
                    ++memberCount;
                    std::uint32_t matches = 0;
                    for (std::uint32_t participant = 0; participant < participantCount; ++participant) {
                        const auto& declared = participants[participant];
                        if (declared.kind == DecisionParticipantKind::Subdecision &&
                            declared.slot == member.identity.decision.slot &&
                            declared.generation == member.identity.decision.generation) ++matches;
                    }
                    const auto& owner = memberBinding.operation.owner;
                    const auto& entryOwner = binding.operation.owner;
                    const bool sameOwner = owner == entryOwner;
                    const bool childQueueOwner = memberBinding.queueInitial.Valid() &&
                        owner.Valid() && entryOwner.Valid() &&
                        owner.parent == entryOwner.child &&
                        owner.parentEpoch == entryOwner.childEpoch &&
                        owner.invocation == entryOwner.invocation &&
                        owner.invocationEpoch == entryOwner.invocationEpoch;
                    const bool companionQueueOwner = memberBinding.queueInitial.Valid() &&
                        pool->IsInitialSoundQueueCompanionC0(descriptor.primary, member.identity, entryOwner, owner);
                    const bool frozenVariableMember =
                        memberBinding.operation.kind == FrontierOperationKind::Entry &&
                        memberBinding.variableSide.Valid() &&
                        !memberBinding.queueInitial.Valid() &&
                        !memberBinding.queueConditional.Valid() &&
                        owner.Valid() && entryOwner.Valid() &&
                        owner.invocation == entryOwner.invocation &&
                        owner.invocationEpoch == entryOwner.invocationEpoch &&
                        memberBinding.operation.order.executeFrame ==
                            descriptor.order.executeFrame;
                    const auto& descendant = memberBinding.queueDescendant;
                    if (memberBinding.queueInitial.Valid() &&
                        ((sameOwner || childQueueOwner || companionQueueOwner)
                            ? !descendant.Empty() : !descendant.Valid())) {
                        group.failure = FrontierReceiptReason::Identity;
                        continue;
                    }
                    std::uint32_t selectedSideCount = 0;
                    if (memberBinding.queueInitial.Valid() &&
                        descendant.Valid() &&
                        memberBinding.operation.group == member.identity) {
                        for (std::uint32_t candidate = 0;
                             candidate < publishedCount; ++candidate) {
                            ImmediateLaneGroupRef sideGroup{};
                            Plan03PageGroupView sideView{};
                            if (!immediateLane->CapturePublishedGroupDescriptor(
                                    laneHandle, candidate, &sideGroup) ||
                                !(sideGroup.identity ==
                                    descendant.selectedSideMember.Expand()) ||
                                sideGroup.dueQuantum !=
                                    declaredGroup.dueQuantum ||
                                !pool->ResolvePlan03GroupSliceRt(
                                    sideGroup.identity, &sideView) ||
                                sideView.frontier == nullptr ||
                                sideView.lease == nullptr) continue;
                            const auto& side = *sideView.frontier;
                            selectedSideCount +=
                                side.role == FrontierGroupRole::EntryMember &&
                                side.entry == binding.entry &&
                                side.primary == descriptor.primary &&
                                side.operation.group == sideGroup.identity &&
                                side.variableSide == descendant.selectedSide &&
                                side.operation.owner.Valid() &&
                                side.operation.owner.invocation ==
                                    entryOwner.invocation &&
                                side.operation.owner.invocationEpoch ==
                                    entryOwner.invocationEpoch &&
                                side.operation.order.executeFrame ==
                                    descriptor.order.executeFrame &&
                                side.operation.transport ==
                                    descriptor.primary.transportEpoch &&
                                side.operation.world ==
                                    descriptor.worldGeneration &&
                                sideView.lease->Capture().state ==
                                    AudioSliceState::Ready;
                        }
                    }
                    const bool deepQueueOwner =
                        !sameOwner && !childQueueOwner && !companionQueueOwner &&
                        memberBinding.queueInitial.Valid() &&
                        descendant.Valid() && selectedSideCount == 1;
                    std::uint32_t frozenChildCount = 0;
                    if (childQueueOwner || deepQueueOwner || companionQueueOwner) {
                        for (std::uint32_t candidate = 0;
                             candidate < publishedCount; ++candidate) {
                            ImmediateLaneGroupRef nextGroup{};
                            Plan03PageGroupView nextView{};
                            if (!immediateLane->CapturePublishedGroupDescriptor(
                                    laneHandle, candidate, &nextGroup) ||
                                !pool->ResolvePlan03GroupSliceRt(
                                    nextGroup.identity, &nextView) ||
                                nextView.frontier == nullptr) continue;
                            const auto& next = *nextView.frontier;
                            if (!(next.role ==
                                    FrontierGroupRole::Coordinator &&
                                next.prerequisite == binding.entry &&
                                next.prerequisiteMember == member.identity &&
                                next.entry.owner == owner &&
                                nextGroup.dueQuantum >= declaredGroup.dueQuantum))
                                continue;
                            if (deepQueueOwner || companionQueueOwner) {
                                if (!next.queueDescendant.Empty() ||
                                    !(next.primary == nextGroup.identity))
                                    continue;
                                std::uint32_t conditionalCount = 0;
                                for (std::uint32_t conditionalIndex = 0;
                                     conditionalIndex < publishedCount;
                                     ++conditionalIndex) {
                                    ImmediateLaneGroupRef conditionalGroup{};
                                    Plan03PageGroupView conditionalView{};
                                    if (!immediateLane->CapturePublishedGroupDescriptor(
                                            laneHandle, conditionalIndex,
                                            &conditionalGroup) ||
                                        !pool->ResolvePlan03GroupSliceRt(
                                            conditionalGroup.identity,
                                            &conditionalView) ||
                                        conditionalView.frontier == nullptr)
                                        continue;
                                    const auto& conditional =
                                        *conditionalView.frontier;
                                    conditionalCount +=
                                        (!companionQueueOwner || pool->IsInitialSoundQueueCompanionC1(
                                            nextGroup.identity, conditionalGroup.identity, member.identity, entryOwner, owner)) &&
                                        conditional.role ==
                                            FrontierGroupRole::EntryMember &&
                                        conditional.entry == next.entry &&
                                        conditional.primary == nextGroup.identity &&
                                        conditional.operation.owner == owner &&
                                        conditional.queueDescendant.Empty() &&
                                        conditional.queueConditional.Valid() &&
                                        conditional.queueConditional.slot ==
                                            memberBinding.queueInitial.slot &&
                                        conditional.queueConditional.generation ==
                                            memberBinding.queueInitial.generation &&
                                        conditionalGroup.dueQuantum ==
                                            nextGroup.dueQuantum;
                                }
                                if (conditionalCount != 1) continue;
                            }
                            ++frozenChildCount;
                        }
                    }
                    if (matches != 1 || !(member.identity == declaredGroup.identity) ||
                        !(memberBinding.operation.group == member.identity) ||
                        !(memberBinding.primary == descriptor.primary) ||
                        (!sameOwner &&
                            !(childQueueOwner && frozenChildCount == 1) &&
                            !(deepQueueOwner && frozenChildCount == 1) &&
                            !(companionQueueOwner && frozenChildCount == 1) &&
                            !frozenVariableMember) ||
                        memberBinding.operation.transport != descriptor.primary.transportEpoch ||
                        memberBinding.operation.world != descriptor.worldGeneration ||
                        memberBinding.operation.order.executeFrame / frames != declaredGroup.dueQuantum ||
                        memberBinding.operation.order < descriptor.order ||
                        member.lease == nullptr ||
                        member.lease->Capture().state != AudioSliceState::Ready) {
                        group.failure = FrontierReceiptReason::Predicate;
                        continue;
                    }
                    const auto current = FindGroup(member.identity);
                    if (declaredGroup.dueQuantum == quantum) {
                        if (current == UINT32_MAX ||
                            !ProveModeMember(groups[current], &group))
                            group.failure = FrontierReceiptReason::Predicate;
                    } else if (declaredGroup.dueQuantum < quantum || current != UINT32_MAX) {
                        group.failure = FrontierReceiptReason::Identity;
                    }
                }
                if (primaryCount != 1)
                    group.failure = FrontierReceiptReason::Identity;
                for (std::uint32_t candidate = 0; candidate < groupCount; ++candidate) {
                    const auto& source = groups[candidate];
                    const auto& initial = *source.view.frontier;
                    if (initial.role != FrontierGroupRole::EntryMember ||
                        !initial.queueInitial.Valid() || !(initial.entry == binding.entry)) continue;
                    std::uint32_t successors = 0;
                    for (std::uint32_t published = 0; published < publishedCount; ++published) {
                        ImmediateLaneGroupRef declaredGroup{};
                        Plan03PageGroupView declaredView{};
                        if (!immediateLane->CapturePublishedGroupDescriptor(laneHandle, published, &declaredGroup) ||
                            !pool->ResolvePlan03GroupSliceRt(declaredGroup.identity, &declaredView) ||
                            declaredView.frontier == nullptr) {
                            group.failure = FrontierReceiptReason::Identity;
                            continue;
                        }
                        successors += sourceFor(*declaredView.frontier) == &source;
                    }
                    if (successors != 1) group.failure = FrontierReceiptReason::Identity;
                }
            } else {
                for (std::uint32_t memberIndex = 0; memberIndex < groupCount; ++memberIndex) {
                    auto& member = groups[memberIndex];
                    if (member.view.frontier->role != FrontierGroupRole::EntryMember ||
                        !(member.view.frontier->entry == binding.entry)) continue;
                    ++memberCount;
                    std::uint32_t matches = 0;
                    for (std::uint32_t participant = 0; participant < participantCount; ++participant) {
                        const auto& declared = participants[participant];
                        if (declared.kind == DecisionParticipantKind::Subdecision &&
                            declared.slot == member.view.identity.decision.slot &&
                            declared.generation == member.view.identity.decision.generation) ++matches;
                    }
                    if (matches != 1 || !ProveModeMember(member, &group)) {
                        group.failure = FrontierReceiptReason::Predicate;
                    }
                }
            }
        if (memberCount != descriptor.expectedMemberCount) group.failure = FrontierReceiptReason::MissingMember;
            for (std::uint32_t i = 0; i < eventCount; ++i) {
                auto& event = events[i];
                if (event.stop != nullptr || event.group != index) continue;
                ++count;
                if (event.source.frontier.transport != descriptor.primary.transportEpoch ||
                    event.source.frontier.world != descriptor.worldGeneration ||
                    event.order.executeFrame != descriptor.order.executeFrame)
                    group.failure = FrontierReceiptReason::Identity;
                const auto ordinal = event.source.frontier.entryEventOrdinal;
                if (event.disposition != kTimelineDispositionApplied || ordinal >= descriptor.expectedEntryEventCount)
                    group.failure = FrontierReceiptReason::Identity;
                for (std::uint32_t prior = 0; prior < i; ++prior) {
                    if (events[prior].stop == nullptr && events[prior].group == index &&
                        events[prior].source.frontier.entryEventOrdinal == ordinal)
                        group.failure = FrontierReceiptReason::DuplicateMember;
                }
                AuthoredStopOperationIdentity stop{};
                if (event.order.executeFrame >= CutoffFor(event, &stop)) {
                    group.failure = FrontierReceiptReason::AuthoredStop;
                    group.cutoff = stop;
                }
                if (event.controller) {
                    if (!event.source.frontier.group.Valid() || event.source.frontier.group == group.view.identity) {
                        ++controllerCount;
                        if (!(event.order == descriptor.order) ||
                            event.source.program.programGeneration != descriptor.programGeneration)
                            group.failure = FrontierReceiptReason::Identity;
                    } else if (FindGroup(event.source.frontier.group) == UINT32_MAX)
                        group.failure = FrontierReceiptReason::Identity;
                    continue;
                }
                if (!event.reserved || !voices.ProveTimelineReservation(event.reservation, &event.appendProof))
                    group.failure = FrontierReceiptReason::AppendCapacity;
                bool firstArm = true;
                bool starts = event.voice.kind == TimelineEventKind::Start;
                bool completesControl = false;
                std::uint32_t startCount = 0;
                for (std::uint32_t other = 0; other < eventCount; ++other) {
                    if (events[other].stop != nullptr || events[other].group != index ||
                        events[other].controller || !SameArm(event.source.armToken, events[other].source.armToken)) continue;
                    firstArm = firstArm && other >= i;
                    starts = starts || events[other].voice.kind == TimelineEventKind::Start;
                    completesControl = completesControl || events[other].completesControl;
                    startCount += events[other].voice.kind == TimelineEventKind::Start ? 1u : 0u;
                }
                event.completesControl = completesControl;
                if (startCount > 1) group.failure = FrontierReceiptReason::DuplicateMember;
                const auto memberIndex = event.source.frontier.group.Valid() ?
                    FindGroup(event.source.frontier.group) : index;
                const auto* actualBinding = memberIndex == UINT32_MAX ? nullptr : groups[memberIndex].view.frontier;
                if (firstArm && (actualBinding == nullptr || !arms.ProveBoundFrontierArm(event.source.armToken,
                    binding.entry, quantum + 1u, starts, &event.armProof, actualBinding)))
                    group.failure = FrontierReceiptReason::Binding;
            }
            if (count != descriptor.expectedEntryEventCount) group.failure = FrontierReceiptReason::MissingMember;
            if (controllerCount != 1) group.failure = FrontierReceiptReason::MissingMember;
            if (!decisions->ValidateFrontierDecisionApply(binding.entry.coordinator, quantum + 1u,
                    &group.decisionProof)) {
                group.failure = FrontierReceiptReason::Predicate;
            }
        }
    }

    void FinishEntry(std::uint32_t index) noexcept {
        ProveEntry(index);
        auto& group = groups[index];
        const auto& binding = *group.view.frontier;
        if (group.audioWriter) {
            if (group.failure == FrontierReceiptReason::None) {
                AudioSliceOperation operation{&group, &ValidateEntry, nullptr, &ApplyEntry};
#if !defined(HARKEN_RT_QUALIFICATION)
                operation.afterClaim = &AfterSliceClaim;
#endif
                (void)ApplyAudioSliceWhole(*group.view.lease, &operation, 1);
                if (!group.applied) group.failure = FrontierReceiptReason::Cancelled;
            }
        }
        if (!group.applied) (void)group.view.lease->Invalidate(AudioSliceInvalidateReason::Cancel);
        if (group.audioWriter) {
            const bool finished = group.activation->FinishAudio(binding.entry,
                group.applied ? FrontierReceiptOutcome::Applied : FrontierReceiptOutcome::DefinitivelyNotApplied,
                group.applied ? FrontierReceiptReason::None : group.failure, quantum + 1u, UINT32_MAX, group.cutoff);
            assert(finished);
            (void)finished;
        }
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            auto& event = events[i];
            if (event.stop != nullptr || event.group != index) continue;
            if (!group.applied) Reject(event, group.failure == FrontierReceiptReason::AuthoredStop ?
                kTimelineDispositionStopDominated : kTimelineDispositionArmFailed);
            else ReleaseProofs(event);
        }
        for (std::uint32_t i = 0; i < groupCount; ++i) {
            if (groups[i].view.frontier->role == FrontierGroupRole::EntryMember &&
                groups[i].view.frontier->entry == binding.entry) {
                groups[i].sideProof.Release();
                groups[i].conditionalProof.Release();
                groups[i].transitionProof.Release();
            }
        }
    }

    static void ApplyFuture(void* opaque) noexcept {
        auto& group = *static_cast<Group*>(opaque);
        auto& self = *group.owner;
        self.ApplyMode(group);
        for (std::uint32_t i = 0; i < self.eventCount; ++i) {
            auto& event = self.events[i];
            if (event.stop != nullptr || event.group != group.index) continue;
            if (event.armProof.Valid()) {
                bool completes = !group.variablePrelude && event.completesControl;
                if (group.queueSlice == QueueInstanceTable::FrontierConditionalSlice::Activation &&
                    group.view.identity.sliceIndex == 1)
                    for (std::uint16_t member = 0; member < group.view.sourceControlCount; ++member)
                        completes = completes || SameArm(event.source.armToken, group.view.members[member]);
                self.arms.ApplyBoundFrontierArm(event.armProof, completes);
            }
            if (event.reserved) {
                AuthoredStopOperationIdentity stop{};
                const auto cutoff = self.CutoffFor(event, &stop);
                self.voices.ApplyProvenTimelineReservation(event.appendProof, cutoff, stop);
            }
        }
        for (std::uint32_t i = self.deferredStartCount; i < self.completionCount; ++i)
            self.arms.ApplyBoundFrontierArm(self.completionProofs[i], true);
        self.decisions->ApplyClaimedFrontierDecision(group.decisionProof);
        group.applied = true;
    }

    void FinishFuture(std::uint32_t index) noexcept {
        assert(completionCount == 0);
        assert(deferredStartCount == 0);
        auto& group = groups[index];
        const auto& binding = *group.view.frontier;
        auto* activation = decisions->ResolveFrontierActivation(binding.entry.coordinator);
        FrontierActivationReceipt receipt{};
        if (activation == nullptr || activation->Read(binding.entry, quantum + 1u, &receipt) != FrontierReceiptRead::Ready ||
            receipt.outcome != FrontierReceiptOutcome::Applied || !(receipt.order < binding.operation.order))
            group.failure = FrontierReceiptReason::Dependency;
        if (group.failure == FrontierReceiptReason::None && !ProveModeMember(group, nullptr))
            group.failure = FrontierReceiptReason::Predicate;
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            auto& event = events[i];
            if (event.stop != nullptr || event.group != index) continue;
            ++count;
            if (event.disposition != kTimelineDispositionApplied ||
                !(event.source.frontier.entry == binding.entry) ||
                !(event.source.frontier.owner == binding.operation.owner) ||
                event.source.frontier.kind != binding.operation.kind ||
                event.source.frontier.world != binding.operation.world ||
                event.source.frontier.transport != binding.operation.transport ||
                event.order.executeFrame != binding.operation.order.executeFrame)
                group.failure = FrontierReceiptReason::Identity;
            AuthoredStopOperationIdentity stop{};
            if (event.order.executeFrame >= CutoffFor(event, &stop)) group.failure = FrontierReceiptReason::AuthoredStop;
            for (std::uint32_t prior = 0; prior < i; ++prior)
                if (events[prior].stop == nullptr && events[prior].group == index &&
                    events[prior].order == event.order) group.failure = FrontierReceiptReason::DuplicateMember;
            if (event.controller) continue;
            if (!event.reserved || !voices.ProveTimelineReservation(event.reservation, &event.appendProof))
                group.failure = FrontierReceiptReason::AppendCapacity;
            bool firstArm = true;
            std::uint32_t starts = 0;
            for (std::uint32_t other = 0; other < eventCount; ++other) {
                const auto& candidate = events[other];
                if (candidate.stop != nullptr || candidate.group != index || candidate.controller ||
                    !SameArm(candidate.source.armToken, event.source.armToken)) continue;
                firstArm = firstArm && other >= i;
                starts += candidate.voice.kind == TimelineEventKind::Start ? 1u : 0u;
            }
            if (starts > 1) group.failure = FrontierReceiptReason::DuplicateMember;
            if (firstArm && !arms.ProveGroupedFrontierOperation(event.source.armToken, binding,
                    *group.view.lease, *pool, quantum + 1u, starts == 1, &event.armProof))
                group.failure = FrontierReceiptReason::Binding;
        }
        for (std::uint32_t i = 0; i < eventCount; ++i)
            if (events[i].stop == nullptr && events[i].group == index &&
                events[i].source.frontier.operationEventCount != count)
                group.failure = FrontierReceiptReason::MissingMember;
        if (group.variableCompletion && group.failure == FrontierReceiptReason::None) {
            // Prelude-only source controls still complete with the second exact group slice.
            for (std::uint32_t member = 0; member < group.view.sourceControlCount; ++member) {
                const auto token = group.view.members[member];
                bool hasEvent = false;
                for (std::uint32_t event = 0; event < eventCount; ++event)
                    hasEvent = hasEvent || (events[event].stop == nullptr && events[event].group == index &&
                        !events[event].controller && SameArm(events[event].source.armToken, token));
                if (hasEvent) continue;
                if (completionCount == completionCapacity || !arms.ProveGroupedFrontierOperation(token, binding,
                        *group.view.lease, *pool, quantum + 1u, false, &completionProofs[completionCount])) {
                    group.failure = FrontierReceiptReason::Binding;
                    break;
                }
                ++completionCount;
            }
        }
        if (group.failure == FrontierReceiptReason::None) {
            AudioSliceOperation operation{&group, &ValidateEntry, nullptr, &ApplyFuture};
#if !defined(HARKEN_RT_QUALIFICATION)
            operation.afterClaim = &AfterSliceClaim;
#endif
            const auto applied = ApplyAudioSliceWhole(*group.view.lease, &operation, 1);
            (void)applied;
#if !defined(HARKEN_RT_QUALIFICATION)
            if (QueueFutureProbe(group) && group.applied &&
                (applied == AudioSliceApplyStatus::Applied ||
                 applied == AudioSliceApplyStatus::AppliedWithDeferredInvalidation) &&
                queueFutureAppliedHook != nullptr)
                queueFutureAppliedHook(queueFutureHookContext, group.view.identity.sliceIndex);
#endif
        }
        if (!group.applied) (void)group.view.lease->Invalidate(AudioSliceInvalidateReason::Cancel);
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            auto& event = events[i];
            if (event.stop != nullptr || event.group != index) continue;
            if (!group.applied) Reject(event, group.failure == FrontierReceiptReason::AuthoredStop ?
                kTimelineDispositionStopDominated : kTimelineDispositionArmFailed);
            else ReleaseProofs(event);
        }
        group.sideProof.Release();
        group.conditionalProof.Release();
        group.transitionProof.Release();
        for (std::uint32_t i = 0; i < completionCount; ++i) completionProofs[i].Release();
        completionCount = 0;
        deferredStartCount = 0;
    }

    void FinishUngrouped(std::uint32_t representative) noexcept {
        auto& first = events[representative];
        auto* activation = decisions->ResolveFrontierActivation(first.source.frontier.entry.coordinator);
        FrontierActivationReceipt receipt{};
        bool valid = activation != nullptr && activation->Read(first.source.frontier.entry, quantum + 1u,
            &receipt) == FrontierReceiptRead::Ready && receipt.outcome == FrontierReceiptOutcome::Applied;
        std::uint32_t count = 0;
        bool starts = false;
        bool duplicateStart = false;
        bool dominated = false;
        bool completesControl = false;
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            auto& event = events[i];
            if (event.stop != nullptr || event.group != UINT32_MAX || event.representative != representative) continue;
            ++count;
            valid = valid && event.source.frontier.operationEventCount == first.source.frontier.operationEventCount;
            for (std::uint32_t prior = 0; prior < i; ++prior) {
                const auto& other = events[prior];
                if (other.stop == nullptr && other.group == UINT32_MAX &&
                    other.representative == representative &&
                    (other.source.rowSequence == event.source.rowSequence ||
                     (other.voice.kind == event.voice.kind &&
                      event.voice.kind != TimelineEventKind::Envelope))) valid = false;
            }
            valid = valid && event.disposition == kTimelineDispositionApplied && event.reserved;
            duplicateStart = duplicateStart || (starts && event.voice.kind == TimelineEventKind::Start);
            starts = starts || event.voice.kind == TimelineEventKind::Start;
            completesControl = completesControl || event.completesControl;
            AuthoredStopOperationIdentity stop{};
            dominated = dominated || event.order.executeFrame >= CutoffFor(event, &stop);
            if (valid && !voices.ProveTimelineReservation(event.reservation, &event.appendProof)) valid = false;
        }
        valid = valid && !dominated && !duplicateStart && count == first.source.frontier.operationEventCount &&
            receipt.order < first.operation.order;
        TimelineArmTable::AudioOperationClaim claim;
        if (valid) valid = arms.BeginFrontierOperation(first.source.armToken, first.operation,
            starts ? TimelineArmOperationKind::Start : completesControl ? TimelineArmOperationKind::TransientControl
                : TimelineArmOperationKind::Control,
            quantum + 1u, &claim, &first.source.frontier, pool) == TimelineArmOperationDisposition::Claimed;
        if (valid) arms.ApplyFrontierOperation(claim);
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            auto& event = events[i];
            if (event.stop != nullptr || event.group != UINT32_MAX || event.representative != representative) continue;
            if (!valid) Reject(event, dominated ? kTimelineDispositionStopDominated : kTimelineDispositionArmFailed);
            else {
                AuthoredStopOperationIdentity stop{};
                const auto cutoff = CutoffFor(event, &stop);
                voices.ApplyProvenTimelineReservation(event.appendProof, cutoff, stop);
            }
        }
        if (valid) arms.FinishFrontierOperation(claim, completesControl);
    }
};

FrontierTimelineFinalizer::FrontierTimelineFinalizer(VoicePool& voices, TimelineArmTable& arms)
    : impl_(std::make_unique<Impl>(voices, arms)) {}
FrontierTimelineFinalizer::~FrontierTimelineFinalizer() = default;

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
std::uint32_t FrontierTimelineFinalizer::EventCapacityForTest() const noexcept {
    return impl_->eventCapacity;
}

std::uint32_t FrontierTimelineFinalizer::GroupCapacityForTest() const noexcept {
    return impl_->groupCapacity;
}
#endif

#if !defined(HARKEN_RT_QUALIFICATION)
void FrontierTimelineFinalizer::SetVariableControlSliceClaimHookForTest(void (*hook)(void*), void* context) noexcept {
    impl_->variableSliceClaimHook = hook;
    impl_->variableSliceClaimContext = context;
}

void FrontierTimelineFinalizer::SetQueueFutureSliceHooksForTest(
    void (*validated)(void*), void (*claimed)(void*),
    void (*applied)(void*, std::uint16_t), void* context) noexcept {
    impl_->queueFutureValidatedHook = validated;
    impl_->queueFutureClaimedHook = claimed;
    impl_->queueFutureAppliedHook = applied;
    impl_->queueFutureHookContext = context;
}

void FrontierTimelineFinalizer::SetQueueLaneProbeGroupForTest(Plan03GroupSliceIdentity group) noexcept {
    impl_->queueLaneProbeGroup = group;
}
#endif

bool FrontierTimelineFinalizer::Attach(DecisionTable& decisions,
    const ScheduledTransactionPool& pool, bool laneOnly) {
    const auto eventCapacity = static_cast<std::uint64_t>(pool.MaxEventsPerQuantum()) *
        (laneOnly ? 1u : 2u);
    const auto groupCapacity = static_cast<std::uint64_t>(pool.MaxPlan03GroupHeadersPerPage()) *
        (laneOnly ? 1u : 2u);
    if (!decisions.Valid() || !pool.Valid() || eventCapacity == 0 || groupCapacity == 0 ||
        eventCapacity + groupCapacity > UINT32_MAX || eventCapacity > SIZE_MAX / sizeof(Impl::Event) ||
        groupCapacity > SIZE_MAX / sizeof(Impl::Group) ||
        eventCapacity + groupCapacity > SIZE_MAX / sizeof(Impl::OrderRef)) return false;
    auto& self = *impl_;
    self.events = std::make_unique<Impl::Event[]>(eventCapacity);
    self.groups = std::make_unique<Impl::Group[]>(groupCapacity);
    self.order = std::make_unique<Impl::OrderRef[]>(eventCapacity + groupCapacity);
    self.laneClaims = std::make_unique<ImmediateLaneFrontierClaim[]>(groupCapacity);
    self.participantCapacity = decisions.MaxParticipantsPerDecision();
    self.participants = std::make_unique<DecisionParticipantRef[]>(self.participantCapacity);
    self.completionCapacity = pool.MaxArmsPerGroup();
    self.completionProofs = std::make_unique<TimelineArmTable::BoundAudioProof[]>(self.completionCapacity);
    self.eventCapacity = static_cast<std::uint32_t>(eventCapacity);
    self.groupCapacity = static_cast<std::uint32_t>(groupCapacity);
    self.decisions = &decisions;
    self.pool = &pool;
    return true;
}

void FrontierTimelineFinalizer::AttachVariableControls(VariableControlInstanceTable& instances) noexcept {
    impl_->variableControls = &instances;
}

void FrontierTimelineFinalizer::AttachQueues(QueueInstanceTable& queues) noexcept {
    impl_->queues = &queues;
}

void FrontierTimelineFinalizer::AttachQueueCompletion(const OccurrenceStatePool& occurrences,
    const CompositeCompletionOwnerPool* composites) noexcept {
    impl_->occurrences = &occurrences;
    impl_->composites = composites;
}

void FrontierTimelineFinalizer::AttachArmGroups(const ArmGroupTable& groups) noexcept {
    impl_->armGroups = &groups;
}

void FrontierTimelineFinalizer::AttachTransitions(TransitionInstancePool& transitions) noexcept {
    impl_->transitions = &transitions;
}

void FrontierTimelineFinalizer::Begin(std::uint64_t quantum, std::uint32_t frames) noexcept {
    auto& self = *impl_;
    for (std::uint32_t i = 0; i < self.eventCount; ++i) {
        std::destroy_at(&self.events[i]);
        std::construct_at(&self.events[i]);
    }
    for (std::uint32_t i = 0; i < self.groupCount; ++i) {
        std::destroy_at(&self.groups[i]);
        std::construct_at(&self.groups[i]);
    }
    self.eventCount = self.groupCount = 0;
    self.quantum = quantum;
    self.frames = frames;
    self.laneMode = false;
    self.immediateLaneBucket = nullptr;
}

bool FrontierTimelineFinalizer::CollectGroup(const Plan03PageGroupView& group) noexcept {
    auto& self = *impl_;
    if (self.pool == nullptr || group.frontier == nullptr || !group.frontier->Valid() ||
        !(group.frontier->operation.group == group.identity) || group.lease == nullptr) return false;
    if (self.FindGroup(group.identity) != UINT32_MAX) return true;
    if (self.groupCount == self.groupCapacity) return false;
    if (!self.pool->NotePlan03GroupReadRt(group.identity, self.quantum + 1u)) return false;
    auto& target = self.groups[self.groupCount];
    target.view = group;
    target.owner = &self;
    target.index = self.groupCount++;
    return true;
}

bool FrontierTimelineFinalizer::CollectEvent(const ConsumedEvent& source,
    const TimelineEventPayload* payload, std::uint8_t disposition) noexcept {
    auto& self = *impl_;
    if (self.eventCount == self.eventCapacity) return false;
    auto& event = self.events[self.eventCount++];
    event.source = source;
    event.order = OrderOf(source);
    event.disposition = disposition;
    if (payload == nullptr || !source.frontier.Valid() || self.frames == 0 ||
        self.quantum > UINT64_MAX / self.frames || source.executeFrame < self.quantum * self.frames ||
        source.executeFrame - self.quantum * self.frames >= self.frames ||
        source.executeFrame - self.quantum * self.frames > UINT16_MAX) {
        event.disposition = kTimelineDispositionProtocolViolation;
        return true;
    }
    event.controller = payload->kind == TimelineEventKind::ControllerActivation;
    event.completesControl = payload->kind == TimelineEventKind::VariableWeight ||
        (payload->kind == TimelineEventKind::Start && (payload->reserved & kTimelineEventTransientStartArm) != 0) ||
        (payload->kind == TimelineEventKind::Stop && (payload->reserved & kTimelineEventTransientControlArm) != 0);
    event.voice.kind = payload->kind;
    event.voice.offset = static_cast<std::uint16_t>(source.executeFrame - self.quantum * self.frames);
    event.voice.envelopeDirection = payload->envelopeDirection;
    event.voice.authoredFadeOutCandidate = source.authoredFadeOutCandidate;
    event.voice.authoredRootStop = payload->kind == TimelineEventKind::Stop &&
        (payload->reserved & kTimelineEventAuthoredRootStop) != 0;
    event.voice.frame = payload->envelopeStartFrame;
    event.voice.endFrame = payload->envelopeEndFrame;
    event.voice.value = payload->sourceStartFrame;
    event.voice.variableWeight = payload->variableWeight;
    event.voice.variableWeightOwner = payload->variableWeightOwner;
    event.voice.envelopeCurve = payload->envelopeCurve;
    event.voice.order = event.order;
    event.voice.authoredStopOperation = payload->authoredStopOperation;
    return true;
}

bool FrontierTimelineFinalizer::CollectStop(const AuthoredStopFenceDescriptor& descriptor,
    const TimelineExecutionOrder& order) noexcept {
    auto& self = *impl_;
    if (!descriptor.Active() || !descriptor.UsesDecisionTable(self.decisions) ||
        self.frames == 0 || descriptor.identity.stopFrame / self.frames > self.quantum ||
        order.executeFrame != descriptor.identity.stopFrame) return false;
    for (std::uint32_t i = 0; i < self.eventCount; ++i) {
        const auto* prior = self.events[i].stop;
        if (prior != nullptr && prior->operation == descriptor.operation && prior->identity == descriptor.identity &&
            prior->decision.slot == descriptor.decision.slot &&
            prior->decision.generation == descriptor.decision.generation) return true;
    }
    if (self.eventCount == self.eventCapacity) return false;
    auto& event = self.events[self.eventCount++];
    event.stop = &descriptor;
    event.stopIdentity = descriptor.identity;
    event.order = order;
    return true;
}

void FrontierTimelineFinalizer::Finish() noexcept {
    auto& self = *impl_;
    if (self.decisions == nullptr || self.frames == 0 || self.quantum == UINT64_MAX) return;
    self.PrepareSpans();
    std::uint32_t ordered = 0;
    for (std::uint32_t i = 0; i < self.groupCount; ++i) {
        const auto& binding = *self.groups[i].view.frontier;
        if (binding.role != FrontierGroupRole::EntryMember)
            self.order[ordered++] = {binding.operation.order, i, true};
    }
    for (std::uint32_t i = 0; i < self.eventCount; ++i) {
        if (self.events[i].stop != nullptr || self.events[i].representative == i)
            self.order[ordered++] = {self.events[i].order, i, false};
    }
    std::sort(self.order.get(), self.order.get() + ordered, [](const auto& a, const auto& b) {
        return a.order < b.order;
    });
    for (std::uint32_t i = 0; i < ordered; ++i) {
        const auto item = self.order[i];
        if (item.group) {
            auto& group = self.groups[item.index];
            if (group.view.frontier->role == FrontierGroupRole::Coordinator) self.FinishEntry(item.index);
            else self.FinishFuture(item.index);
        } else {
            auto& event = self.events[item.index];
            if (event.stop == nullptr) self.FinishUngrouped(item.index);
            else if (event.stopApplying) {
                if (event.stop->decision.Valid() && event.stop->UsesDecisionTable(self.decisions))
                    (void)self.decisions->InvalidateForAuthoredStop(event.stop->decision);
                if (!event.stop->decision.Valid())
                    event.stop->operation->PublishApplied(event.stop->identity, self.quantum + 1u);
                event.stop->FinishApply();
                event.stopApplying = false;
                event.stopApplied = true;
            }
        }
    }
    for (std::uint32_t i = 0; i < self.eventCount; ++i) {
        auto& event = self.events[i];
        if (event.stop == nullptr && event.group == UINT32_MAX && event.representative == UINT32_MAX)
            self.Reject(event, kTimelineDispositionArmFailed);
    }
}

bool FrontierTimelineFinalizer::FinishLaneEntryBucket(
    bool (*claimBucket)(void*, ImmediateLaneBucketClaimReceipt*) noexcept,
    void* claimContext, const ImmediateActionLane* lane,
    const ImmediateLaneBucket* bucket, std::uint32_t laneSlot,
    std::uint32_t laneGeneration) noexcept {
    return FinishLaneBucketImpl(claimBucket, claimContext, lane, bucket,
        laneSlot, laneGeneration, false);
}

bool FrontierTimelineFinalizer::FinishLaneContinuationBucket(
    bool (*claimBucket)(void*, ImmediateLaneBucketClaimReceipt*) noexcept,
    void* claimContext, const ImmediateActionLane* lane,
    const ImmediateLaneBucket* bucket, std::uint32_t laneSlot,
    std::uint32_t laneGeneration) noexcept {
    auto& self = *impl_;
    if (self.pool == nullptr) return false;
    for (std::uint32_t index = 0; index < self.groupCount; ++index) {
        const auto& observed = self.groups[index].view;
        Plan03PageGroupView original{};
        if (!self.pool->ResolvePlan03GroupSliceRt(observed.identity, &original) ||
            observed.frontier != original.frontier || observed.lease != original.lease ||
            observed.members != original.members || observed.memberCount != original.memberCount ||
            observed.sourceControlCount != original.sourceControlCount) return false;
    }
    if (self.groupCount == 1u && bucket != nullptr && lane != nullptr && self.pool != nullptr &&
        self.decisions != nullptr && self.queues != nullptr && self.frames != 0u &&
        self.groups[0].view.frontier->role == FrontierGroupRole::Future &&
        self.HasConditionalNaturalSpans(self.groups[0].view, *lane, laneSlot, laneGeneration)) {
        const auto targets = self.groups[0].view.memberCount - self.groups[0].view.sourceControlCount;
        if (bucket->groupCount != 1u || bucket->memberCount != targets ||
            bucket->eventCount != self.ConditionalTargetEventCount(
                 self.groups[0].view, *lane, laneSlot, laneGeneration) || self.eventCount != bucket->eventCount) return false;
        return FinishLaneBucketImpl(claimBucket, claimContext, lane, bucket, laneSlot, laneGeneration, true);
    }
    if (self.groupCount == 1u && self.eventCount == 2u && bucket != nullptr &&
        bucket->groupCount == 1u && bucket->eventCount == 2u && bucket->memberCount == 1u &&
        lane != nullptr && self.pool != nullptr && self.decisions != nullptr &&
        self.queues != nullptr && self.frames != 0u) {
        const auto& group = self.groups[0].view;
        const auto& binding = *group.frontier;
        if (binding.role == FrontierGroupRole::Future && binding.queueInitial.Valid() &&
            !binding.variableSide.Valid() && !binding.queueConditional.Valid() &&
            binding.transition.Valid() && group.memberCount == 2u && group.sourceControlCount == 1u &&
            group.identity.backing == Plan03GroupBacking::ImmediateLane && group.identity.sliceIndex == 1u &&
            binding.operation.order.executeFrame > binding.transition.audibleFrame &&
            binding.operation.order.executeFrame / self.frames == self.quantum &&
            self.pool->ProveImmediateLaneGroupBeforeClaim(group.identity, group.lease,
                *lane, {laneSlot, laneGeneration}, bucket))
            return FinishLaneBucketImpl(claimBucket, claimContext, lane, bucket,
                laneSlot, laneGeneration, true);
        return false;
    }
    if (self.groupCount == 2 && bucket != nullptr &&
        bucket->groupCount == 2 && bucket->eventCount == 2 &&
        bucket->memberCount == 0 && self.pool != nullptr &&
        self.decisions != nullptr && self.queues != nullptr && self.frames != 0) {
        const Impl::Group* coordinator = nullptr;
        const Impl::Group* member = nullptr;
        for (std::uint32_t index = 0; index < self.groupCount; ++index) {
            const auto& candidate = self.groups[index];
            if (candidate.view.frontier->role == FrontierGroupRole::Coordinator)
                coordinator = &candidate;
            else if (candidate.view.frontier->role == FrontierGroupRole::EntryMember)
                member = &candidate;
        }
        if (coordinator != nullptr && member != nullptr &&
            member->view.frontier->queueInitial.Valid()) {
            const auto& control = *coordinator->view.frontier;
            const auto& source = *member->view.frontier;
            const auto* activation = self.decisions->ResolveFrontierActivation(control.entry.coordinator);
            if (!control.prerequisite.Valid() ||
                !control.prerequisiteMember.Exact() ||
                !(control.entry == source.entry) ||
                !(source.primary == coordinator->view.identity) ||
                !(control.operation.owner == source.operation.owner) ||
                control.operation.order.executeFrame / self.frames != self.quantum ||
                source.operation.order.executeFrame / self.frames != self.quantum ||
                control.operation.order.executeFrame !=
                    source.operation.order.executeFrame ||
                activation == nullptr || !activation->Descriptor().Valid() ||
                !(activation->Descriptor().entry == control.entry) ||
                !(activation->Descriptor().primary == coordinator->view.identity) ||
                activation->Descriptor().order != control.operation.order ||
                activation->Descriptor().worldGeneration != control.operation.world ||
                activation->Descriptor().primary.transportEpoch != control.operation.transport ||
                activation->Descriptor().expectedMemberCount != 1u ||
                activation->Descriptor().expectedEntryEventCount != 2u ||
                !(control.entry.owner == source.operation.owner) ||
                source.operation.world != control.operation.world ||
                source.operation.transport != control.operation.transport)
                return false;
            Plan03PageGroupView initial{};
            if (!self.pool->ResolvePlan03GroupSliceRt(
                    control.prerequisiteMember, &initial) ||
                initial.frontier == nullptr || initial.lease == nullptr ||
                initial.lease->Capture().state != AudioSliceState::SliceApplied ||
                initial.frontier->role != FrontierGroupRole::EntryMember ||
                !(initial.frontier->entry == control.prerequisite) ||
                source.operation.owner.parent !=
                    initial.frontier->operation.owner.child ||
                source.operation.owner.parentEpoch !=
                    initial.frontier->operation.owner.childEpoch ||
                source.operation.owner.invocation !=
                    initial.frontier->operation.owner.invocation ||
                source.operation.owner.invocationEpoch !=
                    initial.frontier->operation.owner.invocationEpoch)
                return false;
            const auto* priorActivation = self.decisions->ResolveFrontierActivation(
                control.prerequisite.coordinator);
            FrontierActivationReceipt prior{};
            if (priorActivation == nullptr ||
                !(priorActivation->Descriptor().entry == control.prerequisite) ||
                priorActivation->Read(control.prerequisite, self.quantum + 1u,
                    &prior) != FrontierReceiptRead::Ready ||
                prior.outcome != FrontierReceiptOutcome::Applied ||
                prior.audioSequence == 0 ||
                prior.audioSequence >= self.quantum + 1u ||
                !(prior.order < control.operation.order)) return false;
            QueueInstanceSnapshot queue{};
            if (!self.queues->Capture({source.queueInitial.slot,
                    source.queueInitial.generation}, &queue) ||
                queue.logicMirror.version != source.queueInitial.version ||
                queue.programSemanticFingerprint !=
                    source.queueInitial.programSemanticFingerprint ||
                queue.world != control.operation.world ||
                queue.rootInstance != source.operation.owner.child)
                return false;
            std::uint32_t controllerCount = 0;
            std::uint32_t envelopeCount = 0;
            for (std::uint32_t index = 0; index < self.eventCount; ++index) {
                const auto& event = self.events[index];
                controllerCount += event.controller &&
                    event.source.frontier.kind == FrontierOperationKind::Entry &&
                    event.source.frontier.entry == control.entry;
                envelopeCount += event.voice.kind == TimelineEventKind::Envelope &&
                    event.source.frontier.kind == FrontierOperationKind::Dependent &&
                    event.source.frontier.group == member->view.identity &&
                    event.source.frontier.entry == control.entry;
            }
            if (self.eventCount != 2 || controllerCount != 1 || envelopeCount != 1)
                return false;
            for (std::uint32_t index = 0; index < self.groupCount; ++index)
                if (!self.pool->ProveImmediateLaneGroupBeforeClaim(
                        self.groups[index].view.identity,
                        self.groups[index].view.lease, *lane,
                        {laneSlot, laneGeneration}, bucket)) return false;
            return FinishLaneBucketImpl(claimBucket, claimContext, lane, bucket,
                laneSlot, laneGeneration, true);
        }
    }
    if (self.groupCount == 2u && lane != nullptr && bucket != nullptr &&
        self.pool != nullptr && self.decisions != nullptr && self.queues != nullptr && self.frames != 0u) {
        const Impl::Group* coordinator = nullptr;
        const Impl::Group* member = nullptr;
        for (std::uint32_t index = 0; index < self.groupCount; ++index) {
            const auto& candidate = self.groups[index];
            if (candidate.view.frontier->role == FrontierGroupRole::Coordinator) coordinator = &candidate;
            else if (candidate.view.frontier->role == FrontierGroupRole::EntryMember) member = &candidate;
        }
        if (coordinator != nullptr && member != nullptr &&
            (self.HasConditionalTargetSpans(member->view, *lane, laneSlot, laneGeneration) ||
             self.HasConditionalNaturalSpans(member->view, *lane, laneSlot, laneGeneration))) {
            const auto& control = *coordinator->view.frontier;
            const auto& target = *member->view.frontier;
            const auto* activation = self.decisions->ResolveFrontierActivation(control.entry.coordinator);
            const auto* prerequisite = self.decisions->ResolveFrontierActivation(control.prerequisite.coordinator);
            FrontierActivationReceipt prior{};
            Plan03PageGroupView initial{};
            QueueInstanceSnapshot queue{};
            const bool natural = member->view.sourceControlCount == 1u &&
                self.HasConditionalNaturalSpans(member->view, *lane, laneSlot, laneGeneration);
            if (bucket->groupCount != 2u || bucket->memberCount != (natural ? 0u : member->view.memberCount) ||
                bucket->eventCount != (natural ? 2u : 1u + self.ConditionalTargetEventCount(member->view, *lane, laneSlot, laneGeneration)) || self.eventCount != bucket->eventCount ||
                !control.prerequisite.Valid() || control.prerequisite == control.entry ||
                !control.prerequisiteMember.Exact() || !(control.entry == target.entry) ||
                !(control.entry.owner == target.operation.owner) || !(control.operation.owner == target.operation.owner) ||
                !(target.primary == coordinator->view.identity) ||
                control.operation.order.executeFrame != target.operation.order.executeFrame ||
                control.operation.order.executeFrame / self.frames != self.quantum ||
                activation == nullptr || activation->Descriptor().expectedMemberCount != 1u ||
                activation->Descriptor().expectedEntryEventCount != bucket->eventCount ||
                activation->Descriptor().order != control.operation.order ||
                prerequisite == nullptr ||
                prerequisite->Read(control.prerequisite, self.quantum + 1u, &prior) != FrontierReceiptRead::Ready ||
                prior.outcome != FrontierReceiptOutcome::Applied || prior.audioSequence == 0u ||
                prior.audioSequence >= self.quantum + 1u || !(prior.order < control.operation.order) ||
                !self.pool->ResolvePlan03GroupSliceRt(control.prerequisiteMember, &initial) ||
                initial.frontier == nullptr || initial.lease == nullptr ||
                initial.lease->Capture().state != AudioSliceState::SliceApplied ||
                initial.frontier->role != FrontierGroupRole::EntryMember ||
                !(initial.frontier->entry == control.prerequisite) ||
                !(initial.frontier->operation.owner == target.operation.owner) ||
                initial.frontier->queueInitial.slot != target.queueConditional.slot ||
                initial.frontier->queueInitial.generation != target.queueConditional.generation ||
                initial.frontier->queueInitial.version != target.queueConditional.currentVersion ||
                !self.queues->Capture({target.queueConditional.slot, target.queueConditional.generation}, &queue) ||
                queue.initialActivationSequence != prior.audioSequence ||
                queue.logicMirror.version != target.queueConditional.currentVersion ||
                queue.pendingVersion != target.queueConditional.pendingVersion) return false;
            return FinishLaneBucketImpl(claimBucket, claimContext, lane, bucket,
                laneSlot, laneGeneration, true);
        }
    }
    if (self.groupCount < 2 || self.groupCount % 2 != 0 ||
        self.pool == nullptr || self.decisions == nullptr ||
        self.queues == nullptr || self.frames == 0 || lane == nullptr || bucket == nullptr ||
        bucket->groupCount != self.groupCount)
        return false;

    // This describes the original bucket, not permission to apply it.
    const bool zeroStartQueuePair = self.groupCount == 2u &&
        self.eventCount == 1u && bucket->eventCount == 1u &&
        bucket->memberCount == 0u;
    if (!zeroStartQueuePair) {
        std::uint32_t coordinators = 0;
        for (std::uint32_t index = 0; index < self.groupCount; ++index) {
            const auto& group = self.groups[index].view;
            if (group.frontier == nullptr) return false;
            if (group.frontier->role == FrontierGroupRole::Coordinator) {
                if (group.memberCount != 0u || group.sourceControlCount != 0u)
                    return false;
                ++coordinators;
            } else if (group.frontier->role == FrontierGroupRole::EntryMember) {
                std::uint32_t owners = 0;
                for (std::uint32_t candidate = 0; candidate < self.groupCount; ++candidate) {
                    const auto& primary = self.groups[candidate].view;
                    owners += primary.frontier != nullptr &&
                        primary.frontier->role == FrontierGroupRole::Coordinator &&
                        primary.frontier->entry == group.frontier->entry &&
                        primary.identity == group.frontier->primary;
                }
                if (owners != 1u || (group.memberCount != 0u && group.members == nullptr))
                    return false;
            } else return false;
        }
        const auto* captured = lane->Events(laneSlot, bucket);
        if (captured == nullptr) return false;
        std::uint64_t dueMembers = 0;
        for (std::uint32_t index = 0; index < bucket->eventCount; ++index) {
            const auto& original = captured[index];
            if (!original.futureGroupedStart) continue;
            const auto& event = original.event;
            const auto* program = event.program.program;
            const auto* payload = program != nullptr && program->payloads != nullptr &&
                event.payload < program->payloadCount ? &program->payloads[event.payload] : nullptr;
            if (payload == nullptr || event.program.programGeneration == 0u ||
                program->generation != event.program.programGeneration ||
                payload->kind != TimelineEventKind::Start ||
                event.frontier.kind != FrontierOperationKind::Dependent ||
                event.executeFrame / self.frames != self.quantum) return false;
            std::uint32_t matches = 0;
            for (std::uint32_t groupIndex = 0; groupIndex < self.groupCount; ++groupIndex) {
                const auto& group = self.groups[groupIndex].view;
                const auto& binding = *group.frontier;
                if (binding.role != FrontierGroupRole::EntryMember ||
                    !(group.identity == event.frontier.group)) continue;
                if (!(binding.entry == event.frontier.entry) ||
                    !(binding.operation.owner == event.frontier.owner) ||
                    binding.operation.world != event.frontier.world ||
                    binding.operation.transport != event.frontier.transport) return false;
                for (std::uint16_t member = 0; member < group.memberCount; ++member) {
                    if (!SameArm(group.members[member], event.armToken)) continue;
                    const ImmediateLaneEvent* mapped = nullptr;
                    if (!lane->FutureMember(laneSlot, laneGeneration, group.identity,
                            member, event.armToken, &mapped) || mapped != &original)
                        return false;
                    ++matches;
                }
            }
            if (matches != 1u) return false;
            ++dueMembers;
        }
        // Later members retain their genuine FutureMember proofs below.
        if (coordinators != self.groupCount / 2u || self.eventCount != coordinators ||
            dueMembers == 0u || bucket->memberCount != dueMembers ||
            bucket->eventCount != dueMembers + coordinators)
            return false;
    }

    if (zeroStartQueuePair) {
        const Impl::Group* coordinator = nullptr;
        const Impl::Group* conditionalMember = nullptr;
        for (std::uint32_t index = 0; index < self.groupCount; ++index) {
            const auto& group = self.groups[index];
            if (group.view.frontier == nullptr || group.view.lease == nullptr ||
                group.view.memberCount != 0u ||
                group.view.sourceControlCount != 0u)
                return false;
            if (group.view.frontier->role == FrontierGroupRole::Coordinator) {
                if (coordinator != nullptr) return false;
                coordinator = &group;
            } else if (group.view.frontier->role == FrontierGroupRole::EntryMember) {
                if (conditionalMember != nullptr) return false;
                conditionalMember = &group;
            } else return false;
        }
        if (coordinator == nullptr || conditionalMember == nullptr ||
            self.armGroups == nullptr)
            return false;
        const auto& control = *coordinator->view.frontier;
        const auto& conditional = *conditionalMember->view.frontier;
        const auto& identity = conditional.queueConditional;
        const bool sameOccurrence = identity.current.slot == identity.successor.slot &&
            identity.current.generation == identity.successor.generation;
        if (!identity.Valid() || self.occurrences == nullptr ||
            (!sameOccurrence && self.occurrences->StageOf(identity.successor) != OccurrenceStage::Preparing) ||
            conditional.variableSide.Valid() || conditional.queueInitial.Valid() ||
            conditional.transition.Valid() ||
            !(conditional.entry == control.entry) ||
            !(conditional.primary == coordinator->view.identity) ||
            conditional.operation.kind != FrontierOperationKind::Dependent ||
            !(conditional.operation.owner == control.operation.owner) ||
            conditional.operation.order.executeFrame !=
                control.operation.order.executeFrame ||
            conditional.operation.world != control.operation.world ||
            conditional.operation.transport != control.operation.transport)
            return false;

        // Require an actual committed native empty group and its Queue participant.
        // The coordinator's participant and its one Controller are proved again
        // by ProveEntry; this check does not replace those proofs.
        DecisionCommittedSnapshot decision{};
        ArmGroupSnapshot armGroup{};
        if (!self.decisions->CaptureCommitted(
                conditionalMember->view.identity.decision, &decision) ||
            !self.decisions->HasExclusiveParticipant(
                conditionalMember->view.identity.decision,
                {DecisionParticipantKind::QueueInstance, identity.slot,
                    identity.generation}) ||
            !self.armGroups->Capture(decision.armGroup, &armGroup) ||
            armGroup.state != ArmGroupState::Ready ||
            armGroup.parent.slot != conditionalMember->view.identity.decision.slot ||
            armGroup.parent.generation !=
                conditionalMember->view.identity.decision.generation ||
            armGroup.memberCount != 0u || armGroup.ackedCount != 0u)
            return false;

        // With exactly one original event, this proves exhaustive partition:
        // one genuine coordinator Controller and zero target Starts.
        const auto& event = self.events[0];
        if (event.stop != nullptr || !event.controller ||
            event.voice.kind != TimelineEventKind::ControllerActivation ||
            event.disposition != kTimelineDispositionApplied ||
            event.source.frontier.kind != FrontierOperationKind::Entry ||
            !(event.source.frontier.entry == control.entry) ||
            !(event.source.frontier.group == coordinator->view.identity) ||
            !(event.source.frontier.owner == control.operation.owner) ||
            event.source.frontier.world != control.operation.world ||
            event.source.frontier.transport != control.operation.transport ||
            event.source.frontier.operationEventCount != 1u ||
            event.source.frontier.entryEventOrdinal != 0u ||
            !(event.order == control.operation.order))
            return false;
    }

    for (std::uint32_t index = 0; index < self.groupCount; ++index) {
        const auto& group = self.groups[index];
        if (group.view.frontier == nullptr) return false;
        if (group.view.frontier->role == FrontierGroupRole::EntryMember)
            continue;
        if (group.view.frontier->role != FrontierGroupRole::Coordinator)
            return false;
        const auto& control = *group.view.frontier;
        const Impl::Group* member = nullptr;
        for (std::uint32_t candidate = 0; candidate < self.groupCount;
             ++candidate) {
            const auto& possible = self.groups[candidate];
            if (possible.view.frontier->role != FrontierGroupRole::EntryMember ||
                !(possible.view.frontier->entry == control.entry)) continue;
            if (member != nullptr) return false;
            member = &possible;
        }
        if (member == nullptr) return false;
        const auto& conditional = *member->view.frontier;
        const auto* currentActivation = self.decisions->ResolveFrontierActivation(control.entry.coordinator);
        if (!control.prerequisite.Valid() ||
            control.prerequisite == control.entry ||
            !(conditional.primary == group.view.identity) ||
            !conditional.queueConditional.Valid() ||
            !(control.operation.owner == conditional.operation.owner) ||
            currentActivation == nullptr || !currentActivation->Descriptor().Valid() ||
            !(currentActivation->Descriptor().entry == control.entry) ||
            !(currentActivation->Descriptor().primary == group.view.identity) ||
            !(currentActivation->Descriptor().order == control.operation.order) ||
            currentActivation->Descriptor().worldGeneration != control.operation.world ||
            currentActivation->Descriptor().expectedMemberCount != 1u ||
            currentActivation->Descriptor().expectedEntryEventCount != 1u ||
            control.operation.transport != conditional.operation.transport ||
            control.operation.world != conditional.operation.world ||
            control.operation.order.executeFrame >=
                self.quantum * static_cast<std::uint64_t>(self.frames) + self.frames)
            return false;
        const auto& prerequisiteOwner = control.prerequisite.owner;
        const auto& owner = control.entry.owner;
        const bool sameOwner = prerequisiteOwner == owner;
        const bool childOwner = prerequisiteOwner.Valid() && owner.Valid() &&
            owner.parent == prerequisiteOwner.child &&
            owner.parentEpoch == prerequisiteOwner.childEpoch &&
            owner.invocation == prerequisiteOwner.invocation &&
            owner.invocationEpoch == prerequisiteOwner.invocationEpoch;
        if (!control.queueDescendant.Empty() ||
            !conditional.queueDescendant.Empty()) return false;
        const bool companionOwner = self.pool->IsInitialSoundQueueCompanionC1(
            group.view.identity, member->view.identity, control.prerequisiteMember, prerequisiteOwner, owner);
        const bool deepOwner = !sameOwner && !childOwner && !companionOwner;
        Plan03PageGroupView initial{};
        if ((childOwner || deepOwner || companionOwner || zeroStartQueuePair) &&
            (!control.prerequisiteMember.Exact() ||
             !self.pool->ResolvePlan03GroupSliceRt(
                 control.prerequisiteMember, &initial) ||
             initial.frontier == nullptr || initial.lease == nullptr ||
             initial.lease->Capture().state != AudioSliceState::SliceApplied ||
             initial.frontier->role != FrontierGroupRole::EntryMember ||
             !(initial.frontier->entry == control.prerequisite) ||
             !(initial.frontier->operation.group == control.prerequisiteMember) ||
             !(initial.frontier->operation.owner == owner) ||
             initial.frontier->queueInitial.slot !=
                 conditional.queueConditional.slot ||
             initial.frontier->queueInitial.generation !=
                 conditional.queueConditional.generation)) return false;
        if (zeroStartQueuePair &&
            (!initial.frontier->queueInitial.Valid() ||
             initial.frontier->variableSide.Valid() ||
             initial.frontier->queueConditional.Valid() ||
             initial.frontier->queueInitial.version !=
                 conditional.queueConditional.currentVersion ||
             initial.frontier->operation.world != control.operation.world ||
             initial.frontier->operation.transport != control.operation.transport))
            return false;
        if ((childOwner || companionOwner) && !initial.frontier->queueDescendant.Empty())
            return false;
        if (deepOwner) {
            const auto& descendant = initial.frontier->queueDescendant;
            Plan03PageGroupView side{};
            if (!descendant.Valid() ||
                !self.pool->ResolvePlan03GroupSliceRt(
                    descendant.selectedSideMember.Expand(), &side) ||
                side.frontier == nullptr || side.lease == nullptr ||
                side.lease->Capture().state !=
                    AudioSliceState::SliceApplied ||
                side.frontier->role != FrontierGroupRole::EntryMember ||
                !(side.frontier->entry == control.prerequisite) ||
                !(side.frontier->variableSide == descendant.selectedSide) ||
                side.frontier->operation.owner.invocation !=
                    owner.invocation ||
                side.frontier->operation.owner.invocationEpoch !=
                    owner.invocationEpoch)
                return false;
        }
        const auto* prerequisite = self.decisions->ResolveFrontierActivation(
            control.prerequisite.coordinator);
        FrontierActivationReceipt prior{};
        if (prerequisite == nullptr ||
            !(prerequisite->Descriptor().entry == control.prerequisite) ||
            prerequisite->Read(control.prerequisite, self.quantum + 1u,
                &prior) != FrontierReceiptRead::Ready ||
            prior.outcome != FrontierReceiptOutcome::Applied ||
            prior.audioSequence == 0 || prior.audioSequence >= self.quantum + 1u ||
            !(prior.order < control.operation.order)) return false;
        QueueInstanceSnapshot queue{};
        const auto& identity = conditional.queueConditional;
        if (!self.queues->Capture({identity.slot, identity.generation}, &queue) ||
            queue.initialActivationSequence != prior.audioSequence ||
            queue.logicMirror.version != identity.currentVersion ||
            queue.pendingVersion != identity.pendingVersion ||
            queue.pendingActivation != ConditionalQueueActivation::Waiting ||
            queue.failurePhase != QueueFailurePhase::Healthy ||
            queue.world != control.operation.world ||
            queue.rootInstance != owner.child)
            return false;
        if (zeroStartQueuePair && queue.programSemanticFingerprint !=
                initial.frontier->queueInitial.programSemanticFingerprint)
            return false;
    }
    for (std::uint32_t index = 0; index < self.groupCount; ++index)
        if (!self.pool->ProveImmediateLaneGroupBeforeClaim(
                self.groups[index].view.identity,
                self.groups[index].view.lease, *lane,
                {laneSlot, laneGeneration}, bucket)) return false;
    return FinishLaneBucketImpl(claimBucket, claimContext, lane, bucket,
        laneSlot, laneGeneration, true);
}

bool FrontierTimelineFinalizer::FinishLaneBucketImpl(
    bool (*claimBucket)(void*, ImmediateLaneBucketClaimReceipt*) noexcept,
    void* claimContext, const ImmediateActionLane* lane,
    const ImmediateLaneBucket* bucket, std::uint32_t laneSlot,
    std::uint32_t laneGeneration, bool continuation) noexcept {
    auto& self = *impl_;
    if (self.pool == nullptr || self.decisions == nullptr || self.frames == 0 ||
        self.quantum == UINT64_MAX || claimBucket == nullptr || lane == nullptr ||
        bucket == nullptr || bucket->quantum != self.quantum ||
        bucket->attemptGeneration != laneGeneration ||
        self.groupCount == 0) return false;
    self.laneMode = true;
    self.immediateLane = lane;
    self.immediateLaneSlot = laneSlot;
    self.immediateLaneGeneration = laneGeneration;
    self.immediateLaneBucket = bucket;
    for (std::uint32_t index = 0; index < self.groupCount; ++index)
        if (!lane->BeforeConditionalProjectionNativeWrite(
                {laneSlot, laneGeneration}, self.groups[index].view.identity)) return false;
    self.PrepareSpans();

    bool valid = true;
    std::uint32_t coordinatorCount = 0;
    for (std::uint32_t i = 0; i < self.groupCount; ++i) {
        auto& group = self.groups[i];
        self.laneClaims[i] = {};
        const auto& binding = *group.view.frontier;
        valid = binding.operation.group.backing == Plan03GroupBacking::ImmediateLane && valid;
        if (binding.role == FrontierGroupRole::Coordinator) {
            ++coordinatorCount;
            self.ProveEntry(i);
            valid = group.failure == FrontierReceiptReason::None && valid;
        } else if (binding.role != FrontierGroupRole::EntryMember &&
            !(binding.role == FrontierGroupRole::Future &&
              binding.queueInitial.Valid() && !binding.queueConditional.Valid() &&
              !binding.variableSide.Valid() && binding.transition.Valid() &&
              group.view.identity.sliceIndex == 1u && group.view.memberCount == 2u &&
              group.view.sourceControlCount == 1u) &&
              !self.HasConditionalNaturalSpans(group.view, *lane, laneSlot, laneGeneration)) {
            valid = false;
        }
    }
    for (std::uint32_t i = 0; valid && i < self.groupCount; ++i)
        valid = self.pool->ProveImmediateLaneGroupBeforeClaim(
            self.groups[i].view.identity, self.groups[i].view.lease,
            *lane, {laneSlot, laneGeneration}, bucket);
    if (coordinatorCount == 0) {
        for (std::uint32_t i = 0; i < self.groupCount; ++i) {
            auto& group = self.groups[i];
            const auto& binding = *group.view.frontier;
            FrontierActivationReceipt entryReceipt{};
            auto* activation = self.decisions->ResolveFrontierActivation(
                binding.entry.coordinator);
            valid = (binding.role == FrontierGroupRole::EntryMember ||
                (binding.role == FrontierGroupRole::Future &&
                 (binding.queueInitial.Valid() ||
                  self.HasConditionalNaturalSpans(group.view, *lane, laneSlot, laneGeneration)))) &&
                binding.operation.group == group.view.identity &&
                binding.operation.order.executeFrame /
                    self.frames == self.quantum &&
                activation != nullptr &&
                activation->Read(binding.entry, self.quantum + 1u,
                    &entryReceipt) == FrontierReceiptRead::Ready &&
                entryReceipt.outcome == FrontierReceiptOutcome::Applied &&
                entryReceipt.order < binding.operation.order && valid;
            if (valid && !self.ProveModeMember(group, nullptr)) valid = false;
            if (valid && group.initialQueueCompletion &&
                !self.ProveInitialQueueCompletionEvents(group)) valid = false;
            if (valid && group.conditionalQueueCompletion &&
                !self.ProveConditionalCompletionEvents(group)) valid = false;
        }
        for (std::uint32_t i = 0; i < self.eventCount; ++i) {
            const auto& event = self.events[i];
            const bool queueCompletion = event.group < self.groupCount &&
                (self.groups[event.group].initialQueueCompletion || self.groups[event.group].conditionalQueueCompletion);
            valid = event.stop == nullptr && (event.controller || queueCompletion) &&
                event.group < self.groupCount &&
                event.disposition == kTimelineDispositionApplied && valid;
        }
#if !defined(HARKEN_RT_QUALIFICATION)
        if (valid && self.SelectedLaneQueueProbe(continuation) != nullptr &&
            self.queueFutureValidatedHook != nullptr)
            self.queueFutureValidatedHook(self.queueFutureHookContext);
#endif
        if (valid) {
            for (std::uint32_t i = 0; i < self.groupCount; ++i)
                if (!self.pool->ClaimImmediateLaneFrontier(
                        self.groups[i].view.identity, &self.laneClaims[i])) {
                    valid = false;
                    break;
                }
        }
        if (valid) {
            for (std::uint32_t i = 0; i < self.groupCount; ++i)
                if (!self.groups[i].view.lease->TryClaim()) {
                    valid = false;
                    break;
                }
        }
        if (valid) {
            ImmediateLaneBucketClaimReceipt receipt{};
            valid = claimBucket(claimContext, &receipt) &&
                receipt.Slot() == laneSlot &&
                receipt.Generation() == laneGeneration &&
                lane->ValidateClaimReceipt(receipt, bucket);
            for (std::uint32_t i = 0; valid && i < self.groupCount; ++i)
                valid = self.pool->ValidateImmediateLaneGroupClaim(
                    self.groups[i].view.identity,
                    self.groups[i].view.lease, receipt);
#if !defined(HARKEN_RT_QUALIFICATION)
            if (valid && self.SelectedLaneQueueProbe(continuation) != nullptr &&
                self.ProbeLaneReceipt(receipt, bucket) && self.queueFutureClaimedHook != nullptr)
                self.queueFutureClaimedHook(self.queueFutureHookContext);
#endif
        }
        if (valid) {
            for (std::uint32_t i = 0; i < self.groupCount; ++i) {
                auto& group = self.groups[i];
                self.ApplyMode(group);
                if (group.initialQueueCompletion || group.conditionalQueueCompletion) {
                    for (std::uint32_t phase = 0; phase < 2u; ++phase)
                        for (std::uint32_t index = 0; index < self.eventCount; ++index) {
                            auto& event = self.events[index];
                            if (event.group != i || (phase == 0u) !=
                                (event.voice.kind == TimelineEventKind::Start)) continue;
                            if (event.armProof.Valid())
                                self.arms.ApplyBoundFrontierArm(event.armProof, event.completesControl);
                            self.voices.ApplyProvenLaneTimelineReservation(event.appendProof);
                        }
                }
                for (std::uint32_t proof = group.firstCompletionProof;
                     proof < group.firstCompletionProof +
                         group.completionProofCount; ++proof)
                    self.arms.ApplyBoundFrontierArm(
                        self.completionProofs[proof], true);
                self.decisions->ApplyClaimedFrontierDecision(
                    group.decisionProof);
                group.applied = true;
                const bool marked = group.view.lease->MarkApplied();
                assert(marked);
                (void)marked;
            }
        } else {
            for (std::uint32_t i = 0; i < self.groupCount; ++i) {
                const auto state = self.groups[i].view.lease->Capture().state;
                if (state == AudioSliceState::SliceClaimed ||
                    state == AudioSliceState::SliceClaimedWithInvalidation)
                    (void)self.groups[i].view.lease->AbortClaimedBeforeApply(
                        AudioSliceInvalidateReason::Cancel);
            }
        }
#if !defined(HARKEN_RT_QUALIFICATION)
        if (valid) self.ProbeLaneApplied(continuation);
#endif
        for (std::uint32_t i = 0; i < self.groupCount; ++i) {
            auto& group = self.groups[i];
            group.sideProof.Release();
            group.conditionalProof.Release();
            group.transitionProof.Release();
            group.queueCompletionProof.Release();
            if (self.laneClaims[i].Valid())
                (void)self.pool->ReleaseImmediateLaneFrontier(
                    &self.laneClaims[i]);
        }
        for (std::uint32_t i = 0; i < self.completionCount; ++i)
            self.completionProofs[i].Release();
        self.completionCount = 0;
        self.deferredStartCount = 0;
        for (std::uint32_t i = 0; i < self.eventCount; ++i) {
            if (valid) self.ReleaseProofs(self.events[i]);
            else self.Reject(self.events[i], kTimelineDispositionArmFailed);
        }
        self.laneMode = false;
        self.immediateLaneBucket = nullptr;
        return valid;
    }
    valid = coordinatorCount != 0 && valid;
    for (std::uint32_t i = 0; i < self.groupCount; ++i) {
        const auto& binding = *self.groups[i].view.frontier;
        if (binding.role != FrontierGroupRole::EntryMember) continue;
        bool owned = false;
        for (std::uint32_t j = 0; j < self.groupCount; ++j)
            owned = owned || (self.groups[j].view.frontier->role ==
                    FrontierGroupRole::Coordinator &&
                self.groups[j].view.frontier->entry == binding.entry);
        valid = owned && valid;
    }
    for (std::uint32_t i = 0; i < self.eventCount; ++i) {
        const auto& event = self.events[i];
        const bool typedQueueSource = event.group < self.groupCount &&
            self.groups[event.group].view.frontier->role ==
                FrontierGroupRole::Coordinator &&
            event.source.frontier.kind == FrontierOperationKind::Dependent &&
            event.source.frontier.group.Exact() &&
            self.FindGroup(event.source.frontier.group) != UINT32_MAX &&
            self.groups[self.FindGroup(event.source.frontier.group)]
                .view.frontier->queueInitial.Valid();
        const bool typedQueueTarget = event.group < self.groupCount &&
            self.groups[event.group].view.frontier->role == FrontierGroupRole::Coordinator &&
            event.source.frontier.kind == FrontierOperationKind::Dependent &&
            self.FindGroup(event.source.frontier.group) != UINT32_MAX &&
            (self.HasConditionalTargetSpans(self.groups[self.FindGroup(event.source.frontier.group)].view,
                 *lane, laneSlot, laneGeneration) ||
             self.HasConditionalNaturalSpans(self.groups[self.FindGroup(event.source.frontier.group)].view,
                 *lane, laneSlot, laneGeneration));
        valid = event.stop == nullptr && event.group != UINT32_MAX &&
            (event.source.frontier.kind == FrontierOperationKind::Entry ||
                typedQueueSource || typedQueueTarget) && valid;
    }
#if !defined(HARKEN_RT_QUALIFICATION)
    if (valid && self.SelectedLaneQueueProbe(continuation) != nullptr &&
        self.queueFutureValidatedHook != nullptr)
        self.queueFutureValidatedHook(self.queueFutureHookContext);
#endif
    if (valid) {
        for (std::uint32_t i = 0; i < self.groupCount; ++i)
            if (!self.pool->ClaimImmediateLaneFrontier(
                    self.groups[i].view.identity, &self.laneClaims[i])) {
                valid = false;
                break;
            }
    }
    if (valid) {
        for (std::uint32_t i = 0; i < self.groupCount; ++i) {
            auto& group = self.groups[i];
            if (group.view.frontier->role != FrontierGroupRole::Coordinator)
                continue;
            if (!group.view.lease->TryClaim()) {
                valid = false;
                break;
            }
        }
    }
    if (valid) {
        ImmediateLaneBucketClaimReceipt receipt{};
        valid = claimBucket(claimContext, &receipt);
        if (valid && continuation) {
            assert(receipt.Slot() == laneSlot &&
                receipt.Generation() == laneGeneration &&
                lane->ValidateClaimReceipt(receipt, bucket));
            for (std::uint32_t index = 0; index < self.groupCount;
                 ++index)
                assert(self.pool->ValidateImmediateLaneGroupClaim(
                    self.groups[index].view.identity,
                    self.groups[index].view.lease, receipt));
        } else if (valid) {
            valid = receipt.Slot() == laneSlot &&
                receipt.Generation() == laneGeneration &&
                lane->ValidateClaimReceipt(receipt, bucket);
            for (std::uint32_t index = 0; valid && index < self.groupCount;
                  ++index)
                valid = self.pool->ValidateImmediateLaneGroupClaim(
                    self.groups[index].view.identity,
                    self.groups[index].view.lease, receipt);
        }
#if !defined(HARKEN_RT_QUALIFICATION)
        if (valid && self.SelectedLaneQueueProbe(continuation) != nullptr &&
            self.ProbeLaneReceipt(receipt, bucket) && self.queueFutureClaimedHook != nullptr)
            self.queueFutureClaimedHook(self.queueFutureHookContext);
#endif
    }
    if (valid) {
        for (std::uint32_t i = 0; i < self.groupCount; ++i) {
            auto& group = self.groups[i];
            if (group.view.frontier->role == FrontierGroupRole::Coordinator)
                Impl::ApplyEntry(&group);
        }
        for (std::uint32_t i = 0; i < self.groupCount; ++i) {
            auto& group = self.groups[i];
            if (group.view.frontier->role == FrontierGroupRole::Coordinator) {
                const bool marked = group.view.lease->MarkApplied();
                assert(marked);
                (void)marked;
            }
        }
    } else {
        for (std::uint32_t i = 0; i < self.groupCount; ++i) {
            auto& group = self.groups[i];
            if (group.view.frontier->role != FrontierGroupRole::Coordinator)
                continue;
            const auto state = group.view.lease->Capture().state;
            if (state == AudioSliceState::SliceClaimed ||
                state == AudioSliceState::SliceClaimedWithInvalidation)
                (void)group.view.lease->AbortClaimedBeforeApply(
                    AudioSliceInvalidateReason::Cancel);
            else
                (void)group.view.lease->Invalidate(
                    AudioSliceInvalidateReason::Cancel);
        }
    }

#if !defined(HARKEN_RT_QUALIFICATION)
    if (valid) self.ProbeLaneApplied(continuation);
#endif
    for (std::uint32_t i = 0; i < self.groupCount; ++i) {
        auto& group = self.groups[i];
        if (group.view.frontier->role == FrontierGroupRole::Coordinator &&
            group.audioWriter) {
            const auto& binding = *group.view.frontier;
            const auto reason = valid ? FrontierReceiptReason::None :
                group.failure == FrontierReceiptReason::None
                    ? FrontierReceiptReason::Cancelled : group.failure;
            const bool finished = group.activation->FinishAudio(binding.entry,
                valid ? FrontierReceiptOutcome::Applied :
                    FrontierReceiptOutcome::DefinitivelyNotApplied,
                reason, self.quantum + 1u,
                UINT32_MAX,
                group.cutoff);
            assert(finished);
            (void)finished;
        }
        if (group.view.frontier->role == FrontierGroupRole::EntryMember) {
            group.sideProof.Release();
            group.conditionalProof.Release();
            group.transitionProof.Release();
        }
        if (self.laneClaims[i].Valid())
            (void)self.pool->ReleaseImmediateLaneFrontier(&self.laneClaims[i]);
    }
    for (std::uint32_t i = 0; i < self.eventCount; ++i) {
        auto& event = self.events[i];
        if (valid) self.ReleaseProofs(event);
        else self.Reject(event, kTimelineDispositionArmFailed);
    }
    self.laneMode = false;
    self.immediateLaneBucket = nullptr;
    return valid;
}

std::uint8_t FrontierTimelineFinalizer::EventDisposition(const ConsumedEvent& source) const noexcept {
    const auto& self = *impl_;
    for (std::uint32_t i = 0; i < self.eventCount; ++i) {
        const auto& event = self.events[i];
        if (event.stop == nullptr && event.source.txnSlot == source.txnSlot &&
            event.source.transactionId == source.transactionId && event.source.rowSequence == source.rowSequence)
            return event.disposition;
    }
    return kTimelineDispositionProtocolViolation;
}

std::uint64_t FrontierTimelineFinalizer::StorageBytes() const noexcept {
    const auto& self = *impl_;
    return sizeof(Impl) + static_cast<std::uint64_t>(self.eventCapacity) * sizeof(Impl::Event) +
        static_cast<std::uint64_t>(self.participantCapacity) * sizeof(DecisionParticipantRef) +
        static_cast<std::uint64_t>(self.completionCapacity) * sizeof(TimelineArmTable::BoundAudioProof) +
        static_cast<std::uint64_t>(self.groupCapacity) * sizeof(Impl::Group) +
        static_cast<std::uint64_t>(self.groupCapacity) * sizeof(ImmediateLaneFrontierClaim) +
        (static_cast<std::uint64_t>(self.eventCapacity) + self.groupCapacity) * sizeof(Impl::OrderRef);
}

} // namespace harken
