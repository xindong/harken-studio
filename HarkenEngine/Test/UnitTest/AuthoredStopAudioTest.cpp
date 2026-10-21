#include "Common/ObjectIdTestSupport.h"
#include "Engine.h"
#include "Common/TestUtil.h"
#include "VoicePoolReleaseTestAccess.h"
#include "CommandQueueSubmissionTestAccess.h"
#include "harken/core/AllocGuard.h"
#include "harken/render/VoiceTimelineEligibility.h"
#include "harken/render/FrontierTimelineFinalizer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <optional>
#include <type_traits>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t kBlock = 64;
constexpr std::uint32_t kRate = 48000;

VnConfig AudioConfig(std::uint32_t workers) {
    auto config = harkentest::MakeNullConfig(2, kBlock, kRate);
    // These tests use stop/audio primitives without nested Action dispatch.
    config.maxTriggerDepth = 1;
    // Keep each engine isolated without reserving 256 unused Action attempts.
    config.maxActionFirings = 8;
    config.renderWorkerCount = workers;
    return config;
}

struct StopAudioHarness {
    explicit StopAudioHarness(std::uint32_t workers)
        : session(AudioConfig(workers)), samples(4096, 0.25f), arena(65536) {
        EXPECT_EQ(VN_OK, session.InitResult());
        engine = session.Engine();
        if (engine == nullptr) return;
        const auto pcm = harkentest::MonoDesc(samples, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        EXPECT_EQ(workers, engine->voices.RenderWorkerCount());
        engine->voices.DisableRenderWorkers();
        EXPECT_TRUE(engine->voices.EnsureRenderWorkersReady());
        EXPECT_TRUE(engine->voices.EnableOfflineRenderWorkers());
        storage.resize(engine->config.maxBuses * 2 * kBlock);
        planes.resize(engine->config.maxBuses * 2);
        for (std::size_t i = 0; i < planes.size(); ++i) {
            planes[i] = storage.data() + i * kBlock;
        }
    }

    VnVoiceHandle Play(bool gated = true, std::uint32_t pcmId = 1, bool apply = true) {
        const auto pcm = engine->registry.Lookup(harkentest::ObjectId(pcmId));
        VnVoiceHandle render = 0;
        EXPECT_EQ(VN_OK, engine->voices.Play(pcm, 0.0f, 1.0, 0, false, 0, 0, 0,
            harken::ResolvePanGains(engine->config.mixChannelLayout, 0.0f),
            0, pcm.frames, 1, harken::ResampleProfile::LegacyCompatibility, &render));
        EXPECT_TRUE(engine->voices.CaptureReleaseObserver(render, &lastObserver));
        if (retainSources) {
            const auto leaf = engine->playableInstances.Acquire(harken::PlayableKind::Sound, 0);
            const auto epoch = engine->playableInstances.Classify(leaf).scheduleEpoch;
            EXPECT_TRUE(engine->playableInstances.AcquireSourceRetirementHold(leaf, epoch));
            const auto token = engine->sourceTerminals.Reserve(engine->playableInstances,
                leaf, epoch, render, lastObserver, 0);
            EXPECT_TRUE(token.Valid());
            EXPECT_TRUE(engine->voices.BindSourceTerminal(render, engine->sourceTerminals.Resolve(token), token));
        }
        if (captureControl) {
            EXPECT_LT(controlCount, controls.size());
            if (controlCount < controls.size()) {
                auto& receipt = controls[controlCount];
                EXPECT_TRUE(receipt.TryArm(1));
                auto& binding = controlBindings[controlCount];
                binding = {&receipt, 1, render, controlCount, 1};
                EXPECT_TRUE(engine->voices.BindMonitorControl(render, binding));
                ++controlCount;
            }
        }
        if (!apply) return render;
        engine->voices.ApplyPlay(render, render, 0, quantum);
        auto* context = engine->voices.Resolve(render);
        if (context != nullptr) context->timelineGated = gated;
        return render;
    }

    void Begin() { engine->voices.BeginTimelineQuantum(quantum); }

    void Append(VnVoiceHandle render, const harken::VoiceTimelineEvent& event) {
        EXPECT_TRUE(engine->voices.AppendTimelineEvent(static_cast<std::uint32_t>(render),
            static_cast<std::uint32_t>(render >> 32), quantum, event));
    }

    void Render() { RenderWith(arena); }

    void RenderWith(harken::BlockArena& blockArena) {
        blockArena.Reset();
        std::fill(storage.begin(), storage.end(), 0.0f);
        const auto result = engine->voices.RenderAll(blockArena, planes.data(), kBlock,
            engine->notifications, quantum, 0, quantum * kBlock);
        EXPECT_EQ(0u, result.overrunTicks);
        ++quantum;
    }

    float Sample(std::uint32_t frame) const { return planes[0][frame]; }

    std::array<harken::MonitorVoiceControlReceipt, 2> controls;
    std::array<harken::MonitorVoiceControlBinding, 2> controlBindings{};
    std::uint32_t controlCount = 0;
    bool captureControl = false;
    harkentest::EngineSession session;
    std::vector<float> samples;
    harken::BlockArena arena;
    harken::Engine* engine = nullptr;
    std::vector<float> storage;
    std::vector<float*> planes;
    std::uint64_t quantum = 0;
    harken::VoiceReleaseObserver lastObserver{};
    bool retainSources = false;
};

harken::VoiceTimelineEvent StopAt(std::uint16_t offset, bool root = true) {
    harken::VoiceTimelineEvent event{};
    event.kind = harken::TimelineEventKind::Stop;
    event.offset = offset;
    event.authoredRootStop = root;
    return event;
}

harken::VoiceTimelineEvent StartAt(std::uint16_t offset) {
    harken::VoiceTimelineEvent event{};
    event.kind = harken::TimelineEventKind::Start;
    event.offset = offset;
    return event;
}

harken::VoiceTimelineEvent WithFrontierProof(harken::VoiceTimelineEvent event,
    VnVoiceHandle render, std::uint64_t quantum, harken::FrontierEventState state) {
    auto& proof = event.frontierProof;
    proof.state = state;
    proof.entry.coordinator = {0, 1};
    proof.entry.attempt = 1;
    proof.entry.owner = {1, 1, 0, 0, 1, 1, 0, 1};
    proof.operation.kind = harken::FrontierOperationKind::Entry;
    proof.operation.owner = proof.entry.owner;
    proof.operation.order.executeFrame = quantum * kBlock + event.offset;
    proof.operation.order.logicCommitSequence = 1;
    proof.operation.transport = 1;
    proof.operation.world = 1;
    proof.operation.group = {quantum, 1, {0, 1}, 0, 0, 1, 1};
    proof.quantum = quantum;
    proof.renderGeneration = static_cast<std::uint32_t>(render >> 32);
    event.order = proof.operation.order;
    return event;
}

struct FrontierAudioHarness {
    explicit FrontierAudioHarness(std::uint32_t workers, std::uint32_t decisionCapacity = 2)
        : audio(workers), decisions(decisionCapacity, 2), sink(audio.engine->voices, arms) {}

    void Prepare(std::uint64_t stopFrame = 32, bool controllerOnly = false, bool missingEntry = false,
                 bool variable = false, harken::VariableControlSide side = harken::VariableControlSide::Discrete,
                 bool future = false, bool queueInitial = false, bool conditional = false,
                 bool groupedControl = false, std::uint64_t transitionFrame = UINT64_MAX,
                 bool twoVoices = false, bool laneEntry = false,
                 bool secondCoordinatorMissing = false,
                 bool secondCoordinatorWithEvent = false,
                 bool plainBeforeFrontier = false,
                 std::uint64_t memberStartFrame = 8u,
                 std::uint64_t secondCoordinatorFrame = 8u) {
        laneBackedEntry = laneEntry;
        const bool delayedLaneMember = laneEntry && variable && !future && memberStartFrame > 8u;
        const std::uint64_t memberFrame = future ? 16 : 8;
        const bool delayedQueue = conditional && groupedControl && future;
        const bool preludeQueue = conditional && groupedControl && !future;
        const auto futureKind = conditional ? harken::FrontierOperationKind::Genuine : harken::FrontierOperationKind::Dependent;
        harken::ScheduledTransactionPool::Config config{};
        config.quantumFrames = kBlock;
        config.scheduleHorizonFrames = 4 * kBlock;
        config.publishLeadQuanta = 1;
        config.maxScheduledTransactions = 8;
        config.maxTransactionSpanFrames = 4 * kBlock;
        config.maxIngressTransactionsPerQuantum = 8;
        config.maxFrameBatchesPerQuantum = 8;
        config.maxEventsPerQuantum = 16;
        config.maxTransactionEvents = 8;
        config.maxCurrentSlicesPerQuantum = 4;
        config.maxPlan03GroupHeadersPerPage = groupedControl ||
            secondCoordinatorMissing || secondCoordinatorWithEvent ? 3 : 2;
        config.maxArmsPerGroup = 2;
        if (laneBackedEntry) {
            config.maxImmediateLaneGroupRows = 4;
            config.maxImmediateLaneOwnerGates = 4;
            config.maxImmediateLaneDecisionGates = 4;
        }
        ASSERT_TRUE(pool.Reset(config, 0, 1));
        if (laneBackedEntry) {
            ASSERT_TRUE(lane.Reset({1, 8, 8, kBlock}));
            ASSERT_TRUE(lane.Reserve(17, 8, &laneHandle));
        }
        harken::DecisionReserveRequest request{};
        request.ticket.sequence = 1;
        request.world = request.transport = request.expectedContinuationGeneration = 1;
        request.sliceCount = 1;
        ASSERT_TRUE(decisions.Reserve(request, &decision));
        harken::Plan03GroupSliceInput groupInput{};
        groupInput.executeFrame = 8;
        groupInput.decision = decision;
        if (laneBackedEntry)
            ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 0, groupInput, arms,
                &lanePrimary));
        else
            ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                pool.ReservePlan03GroupSlice(groupInput, arms));
        ASSERT_EQ(1u, pool.CaptureLivePlan03Groups(&group, 1));
        if (laneBackedEntry) {
            ASSERT_TRUE(pool.BindImmediateLaneGroupOwner(
                lanePrimary, lane, 17, laneHandle.slot, laneHandle.generation));
            ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, 0));
        }
        descriptor.entry = {decision, 1, {1, 1, 0, 0, 1, 1, 0, 1}};
        descriptor.primary = group.identity;
        descriptor.worldGeneration = descriptor.worldSemanticFingerprint = descriptor.programGeneration = 1;
        descriptor.expectedEntryEventCount = controllerOnly ? 1 : 2;
        if (twoVoices && !laneBackedEntry) ++descriptor.expectedEntryEventCount;
        descriptor.expectedMemberCount = variable ? 1 : 0;
        if (variable) ++descriptor.expectedEntryEventCount;
        if (delayedLaneMember) --descriptor.expectedEntryEventCount;
        if (future) {
            descriptor.expectedMemberCount = 0;
            descriptor.expectedEntryEventCount = 1;
        }
        if (missingEntry) ++descriptor.expectedEntryEventCount;
        controllerProgram = decisions.PrepareFrontierControlProgram(decision, 1);
        ASSERT_NE(nullptr, controllerProgram);
        auto control = Event(8, 0, 20, harken::FrontierOperationKind::Entry, 0);
        if (laneBackedEntry) control.frontier.group = group.identity;
        File(1, *controllerProgram, {}, &control, 1, &descriptor.order);
        ASSERT_TRUE(decisions.PrepareFrontierActivation(decision, descriptor, pool));
        harken::FrontierGroupBinding binding{};
        binding.role = harken::FrontierGroupRole::Coordinator;
        binding.entry = descriptor.entry;
        binding.primary = group.identity;
        binding.operation.kind = harken::FrontierOperationKind::Entry;
        binding.operation.owner = descriptor.entry.owner;
        binding.operation.order = descriptor.order;
        binding.operation.group = group.identity;
        binding.operation.transport = binding.operation.world = 1;
        if (laneBackedEntry) {
            auto wrongFrame = binding;
            ++wrongFrame.operation.order.executeFrame;
            ASSERT_EQ(binding.operation.order.executeFrame / kBlock,
                      wrongFrame.operation.order.executeFrame / kBlock);
            EXPECT_FALSE(pool.BindPlan03GroupFrontier(group.identity,
                                                      wrongFrame));
        }
        ASSERT_TRUE(pool.BindPlan03GroupFrontier(group.identity, binding));
        ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(group.identity, &group));

        if (variable) {
            request.ticket.sequence = 2;
            request.sliceCount = groupedControl ? 2 : 1;
            if (transitionFrame != UINT64_MAX) {
                ASSERT_TRUE(transitions.Acquire({transitionFrame, 0}, &transition));
                request.transitionGate = transition;
            }
            ASSERT_TRUE(decisions.Reserve(request, &memberDecision));
            if (queueInitial || conditional) {
                harken::QueueInstanceCreateRequest create{};
                create.world = create.rootInstance = create.initial.version = 1;
                create.containerStableId = harkentest::ObjectId(9);
                create.programSemanticFingerprint = 7;
                create.childCount = conditional ? 2 : 1;
                ASSERT_TRUE(queues.Acquire(create, &queue));
                if (conditional) {
                    const auto pcm = harkentest::MonoDesc(sourceSamples, kRate);
                    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &pcm));
                    sourceRender = audio.Play(false, 2, false);
                    ASSERT_TRUE(occurrences.Acquire({1, harkentest::ObjectId(47), 0, 3}, &currentOccurrence));
                    ASSERT_TRUE(occurrences.Acquire({1, harkentest::ObjectId(47), 1, 3}, &successorOccurrence));
                    ASSERT_TRUE(occurrences.BindCompletionProgress(currentOccurrence,
                        static_cast<std::uint32_t>(sourceRender >> 32), 1));
                    const auto owner = harken::CompletionOwnerRef::Single(memberDecision, 1);
                    ASSERT_TRUE(occurrences.TryPinCompletionOwner(currentOccurrence, owner));
                    ASSERT_TRUE(audio.engine->voices.BindCompletionProgress(sourceRender,
                        occurrences.CompletionProgressOf(currentOccurrence), currentOccurrence, 1));
                    audio.engine->voices.ApplyPlay(sourceRender, sourceRender, 0, 0);
                    harken::KnownCompletionFactsV1 facts{};
                    facts.occurrence = currentOccurrence;
                    facts.renderGeneration = static_cast<std::uint32_t>(sourceRender >> 32);
                    facts.residentExactMedia = facts.pitchFixed = facts.serialRender = true;
                    facts.pcmFrameCount = sourceSamples.size();
                    facts.loopEndFrame = 16;
                    facts.loopCount = 1;
                    facts.sourceRate = facts.mixRate = kRate;
                    facts.playbackFactsRevision = 1;
                    facts.program.kind = harken::PlayableKindV1::Sound;
                    facts.program.stableId = harkentest::ObjectId(47);
                    facts.program.world = 1;
                    facts.program.runtimeSemanticFingerprint = 7;
                    harken::KnownCompletionPredicateV1 candidate{};
                    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
                        harken::MaterializeKnownCompletion(facts, &candidate));
                    harken::ConditionalQueuePrepareRequest prepare{};
                    prepare.expectedCurrentVersion = 1;
                    prepare.rootDecision = memberDecision;
                    prepare.expectedCurrentOccurrence = currentOccurrence;
                    prepare.successorOccurrence = successorOccurrence;
                    ASSERT_TRUE(harken::BindKnownCompletionPredicate(candidate, facts.program, owner, &prepare.completion));
                    prepare.next.version = 2;
                    prepare.next.cursor = prepare.next.currentChildOrdinal = 1;
                    ASSERT_TRUE(queues.PrepareConditional(queue, prepare, &conditionalVersion));
                }
            } else {
                ASSERT_EQ(harken::VariableControlInstanceStatus::Ok, variables.Acquire({1, 1, harkentest::ObjectId(9), 7}, &instance));
                ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
                    variables.PrepareSide(instance, side, memberDecision, bank));
            }
        }

        if (!controllerOnly) {
            render = audio.Play();
            if (variable) {
                ASSERT_TRUE(arms.ReserveGrouped(static_cast<std::uint32_t>(render),
                    static_cast<std::uint32_t>(render >> 32), delayedLaneMember ? memberStartFrame :
                        (delayedQueue ? transitionFrame : (preludeQueue ? 16 : memberFrame)),
                    0, 1, {}, {memberDecision}, preludeQueue ? 1 : 0, &arm));
                if (preludeQueue) {
                    ASSERT_TRUE(arms.ReserveGrouped(static_cast<std::uint32_t>(sourceRender),
                        static_cast<std::uint32_t>(sourceRender >> 32), memberFrame, 0, 1, {}, {memberDecision}, 0, &sourceArm));
                    ASSERT_TRUE(arms.PinFrontierReader(sourceArm, &sourcePin));
                    queueMembers = {sourceArm, arm};
                }
            } else {
                ASSERT_TRUE(arms.Reserve(static_cast<std::uint32_t>(render),
                    static_cast<std::uint32_t>(render >> 32), 8, 0, 1, {}, &arm));
            }
            ASSERT_TRUE(arms.PinFrontierReader(arm, &armPin));
            if (!variable) ASSERT_TRUE(arms.BindFrontierAdmission(arm, *group.frontier, *group.lease));
        }
        if (variable) {
            ASSERT_TRUE(armGroups.Reserve(memberDecision, controllerOnly ? nullptr : (preludeQueue ? queueMembers.data() : &arm),
                controllerOnly ? 0 : (preludeQueue ? 2 : 1), &memberArmGroup));
            groupInput.decision = memberDecision;
            groupInput.executeFrame = memberFrame;
            groupInput.members = controllerOnly ? nullptr : (preludeQueue ? queueMembers.data() : &arm);
            groupInput.memberCount = controllerOnly ? 0 : (preludeQueue ? 2 : 1);
            groupInput.sourceControlCount = preludeQueue ? 1 : 0;
            if (laneBackedEntry)
                ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 0, groupInput,
                    arms, &laneMember));
            else
                ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                    pool.ReservePlan03GroupSlice(groupInput, arms));
            std::array<harken::Plan03PageGroupView, 2> views{};
            ASSERT_EQ(2u, pool.CaptureLivePlan03Groups(views.data(), views.size()));
            for (const auto& view : views)
                if (view.identity.decision.slot == memberDecision.slot) member = view;
            if (laneBackedEntry) {
                ASSERT_TRUE(pool.BindImmediateLaneGroupOwner(
                    laneMember, lane, 17, laneHandle.slot, laneHandle.generation));
                ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, 0));
            }
            auto memberBinding = binding;
            memberBinding.role = future ? harken::FrontierGroupRole::Future : harken::FrontierGroupRole::EntryMember;
            if (future) {
                memberBinding.primary = {};
                memberBinding.operation.kind = futureKind;
                memberBinding.operation.order.executeFrame = memberFrame;
            }
            memberBinding.operation.group = member.identity;
            memberProgram = decisions.PrepareFrontierControlProgram(memberDecision, 1);
            ASSERT_NE(nullptr, memberProgram);
            auto event = Event(memberFrame, 0, 20, future ? futureKind :
                harken::FrontierOperationKind::Entry, future ? UINT32_MAX :
                    (controllerOnly || delayedLaneMember ? 1 : 2));
            if (future) event.frontier.operationEventCount = controllerOnly || delayedQueue ? 1 : 2;
            event.frontier.group = member.identity;
            File(5, *memberProgram, {}, &event, 1, &memberBinding.operation.order);
            memberBinding.operation.program = memberProgram;
            memberBinding.operation.programGeneration = 1;
            if (transition.Valid())
                memberBinding.transition = {transition.slot, transition.generation, transitionFrame};
            if (conditional) memberBinding.queueConditional = {queue.slot, queue.generation, 1, conditionalVersion.version,
                currentOccurrence, successorOccurrence};
            else if (queueInitial) memberBinding.queueInitial = {queue.slot, queue.generation, 1, 7};
            else memberBinding.variableSide = {instance.slot, instance.generation, static_cast<std::uint8_t>(side),
                    2, bank.slot, bank.generation};
            ASSERT_TRUE(pool.BindPlan03GroupFrontier(member.identity, memberBinding));
            ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(member.identity, &member));
            if (conditional) ASSERT_TRUE(queues.BindFrontierConditional(queue, *member.frontier, pool));
            else if (queueInitial) ASSERT_TRUE(queues.BindFrontierInitial(queue, *member.frontier, pool));
            else ASSERT_TRUE(variables.BindFrontierSide(instance, *member.frontier, pool));
            ASSERT_TRUE(decisions.BindFrontierDecision(memberDecision, *member.frontier, pool));
            if (transition.Valid()) ASSERT_TRUE(transitions.BindFrontier(transition, *member.frontier, pool));
            if (preludeQueue) ASSERT_TRUE(arms.BindFrontierAdmission(sourceArm, *member.frontier, *member.lease));
            if (!controllerOnly && !delayedQueue && !preludeQueue)
                ASSERT_TRUE(arms.BindFrontierAdmission(arm, *member.frontier, *member.lease));
        }
        if (!controllerOnly) {
            if (preludeQueue) {
                ASSERT_TRUE(arms.AcknowledgeApplied(sourceArm));
                ASSERT_TRUE(armGroups.Acknowledge(memberArmGroup, sourceArm, 1));
                ASSERT_TRUE(arms.Publish(sourceArm));
            } else if (!delayedQueue) {
                ASSERT_TRUE(arms.AcknowledgeApplied(arm));
                if (variable) ASSERT_TRUE(armGroups.Acknowledge(memberArmGroup, arm, 1));
                ASSERT_TRUE(arms.Publish(arm));
            }
            voicePayloads[0].kind = harken::TimelineEventKind::Start;
            if (delayedLaneMember) voicePayloads[0].reserved = harken::kTimelineEventTransientStartArm;
            voicePayloads[1].kind = harken::TimelineEventKind::Envelope;
            voicePayloads[1].envelopeDirection = harken::TimelineEnvelopeDirection::FadeOut;
            voicePayloads[1].envelopeStartFrame = 16;
            voicePayloads[1].envelopeEndFrame = 32;
            voicePayloads[2].kind = harken::TimelineEventKind::SetStartCursor;
            voicePayloads[2].sourceStartFrame = 1024;
            if (preludeQueue) {
                voicePayloads[1].envelopeStartFrame = 8;
                voicePayloads[1].envelopeEndFrame = 16;
                voicePayloads[2].kind = harken::TimelineEventKind::Stop;
            }
            voiceProgram = {1, voicePayloads.data(), 3};
            std::array events{
                Event(8, 0, 20, harken::FrontierOperationKind::Entry, 1),
                Event(16, 1, 30, harken::FrontierOperationKind::Dependent),
                Event(48, 2, 10, harken::FrontierOperationKind::Dependent)};
            if (variable) events[0].frontier.group = member.identity;
            if (delayedLaneMember) {
                events[0].executeFrame = memberStartFrame;
                events[0].frontier.kind = harken::FrontierOperationKind::Dependent;
                events[0].frontier.entryEventOrdinal = UINT32_MAX;
            }
            if (future) {
                events[0].executeFrame = memberFrame;
                events[0].frontier.kind = futureKind;
                events[0].frontier.entryEventOrdinal = UINT32_MAX;
                events[0].frontier.operationEventCount = 2;
            }
            if (preludeQueue) {
                events[0].payload = events[0].eventIndex = 1;
                events[0].subeventPhase = 30;
                File(2, voiceProgram, sourceArm, events.data(), 1);
            } else if (!delayedQueue) File(2, voiceProgram, arm, events.data(), variable ? 1 : events.size());
        }
        if (twoVoices) {
            secondRender = audio.Play();
            ASSERT_TRUE(arms.Reserve(static_cast<std::uint32_t>(secondRender),
                static_cast<std::uint32_t>(secondRender >> 32), 8, 0, 1, {}, &secondArm));
            ASSERT_TRUE(arms.PinFrontierReader(secondArm, &secondPin));
            if (!laneBackedEntry)
                ASSERT_TRUE(arms.BindFrontierAdmission(secondArm, *group.frontier, *group.lease));
            ASSERT_TRUE(arms.AcknowledgeApplied(secondArm));
            ASSERT_TRUE(arms.Publish(secondArm));
            auto event = Event(8, 0, 20, harken::FrontierOperationKind::Entry, 2);
            if (laneBackedEntry) event.frontier = {};
            File(9, voiceProgram, secondArm, &event, 1);
            if (laneBackedEntry && laneEventCount != 0)
                laneEvents[laneEventCount - 1].event.logicCommitSequence =
                    plainBeforeFrontier ? 0 : 2;
        }
        if (variable) {
            if (groupedControl) {
                groupInput.sliceIndex = 1;
                const auto controlFrame = delayedQueue ? transitionFrame : 16;
                groupInput.executeFrame = controlFrame;
                ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                    pool.ReservePlan03GroupSlice(groupInput, arms));
                std::array<harken::Plan03PageGroupView, 3> views{};
                ASSERT_EQ(3u, pool.CaptureLivePlan03Groups(views.data(), views.size()));
                for (const auto& view : views)
                    if (view.identity.decision.slot == memberDecision.slot && view.identity.sliceIndex == 1)
                        later = view;
                auto laterBinding = *member.frontier;
                laterBinding.role = harken::FrontierGroupRole::Future;
                laterBinding.primary = {};
                laterBinding.operation.kind = harken::FrontierOperationKind::Dependent;
                laterBinding.operation.group = later.identity;
                auto control = Event(controlFrame, 0, 20, harken::FrontierOperationKind::Dependent);
                control.frontier.group = later.identity;
                control.frontier.operationEventCount = controllerOnly ? 1 : (preludeQueue ? 3 : 2);
                File(preludeQueue ? 8 : 7, *memberProgram, {}, &control, 1, &laterBinding.operation.order);
                ASSERT_TRUE(pool.BindPlan03GroupFrontier(later.identity, laterBinding));
                ASSERT_TRUE(pool.ResolvePlan03GroupSliceProducer(later.identity, &later));
                if (delayedQueue || preludeQueue) ASSERT_TRUE(queues.BindFrontierConditional(queue, *later.frontier, pool));
                else ASSERT_TRUE(variables.BindFrontierSide(instance, *later.frontier, pool));
                ASSERT_TRUE(decisions.BindFrontierDecision(memberDecision, *later.frontier, pool));
                if (transition.Valid()) ASSERT_TRUE(transitions.BindFrontier(transition, *later.frontier, pool));
                if (delayedQueue || preludeQueue) {
                    ASSERT_TRUE(arms.BindFrontierAdmission(arm, *later.frontier, *later.lease));
                    ASSERT_TRUE(arms.AcknowledgeApplied(arm));
                    ASSERT_TRUE(armGroups.Acknowledge(memberArmGroup, arm, 1));
                    ASSERT_TRUE(arms.Publish(arm));
                }
                if (preludeQueue) {
                    auto stop = Event(controlFrame, 2, 50, harken::FrontierOperationKind::Dependent);
                    stop.frontier.group = later.identity;
                    stop.frontier.operationEventCount = 3;
                    File(6, voiceProgram, sourceArm, &stop, 1);
                    auto start = Event(controlFrame, 0, 20, harken::FrontierOperationKind::Dependent);
                    start.frontier.group = later.identity;
                    start.frontier.operationEventCount = 3;
                    File(7, voiceProgram, arm, &start, 1);
                } else if (!controllerOnly) {
                    auto fade = Event(controlFrame, delayedQueue ? 0 : 1,
                        delayedQueue ? 20 : 30, harken::FrontierOperationKind::Dependent);
                    fade.frontier.group = later.identity;
                    fade.frontier.operationEventCount = 2;
                    File(6, voiceProgram, arm, &fade, 1);
                }
            }
            const harken::DecisionParticipantRef participant = queueInitial || conditional ?
                harken::DecisionParticipantRef{harken::DecisionParticipantKind::QueueInstance, queue.slot, queue.generation} :
                harken::DecisionParticipantRef{harken::DecisionParticipantKind::Variable, instance.slot, instance.generation};
            harken::DecisionCommitPayload memberCommit{};
            memberCommit.pages = commitPages;
            memberCommit.armGroup = memberArmGroup;
            memberCommit.participants = &participant;
            memberCommit.participantCount = 1;
            ASSERT_TRUE(decisions.Commit(memberDecision, memberCommit));
        }
        const harken::AuthoredStopOperationIdentity stopIdentity{0, 1, 1, 1, 1, 1, stopFrame};
        stopOperation.emplace(stopIdentity);
        stopPayload.kind = harken::TimelineEventKind::Stop;
        stopPayload.reserved = harken::kTimelineEventAuthoredRootStop;
        stopPayload.envelopeEndFrame = stopFrame;
        stopPayload.authoredStopOperation = stopIdentity;
        stopProgram = {1, &stopPayload, 1};
        stopProgram.role = harken::TimelineProgramRole::AuthoredStopFence;
        stopFence.emplace(&*stopOperation, stopIdentity, harken::DecisionToken{}, &stopProgram, 1);
        stopProgram.authoredStopFence = &*stopFence;
        harken::ScheduledEventInput stopEvent{};
        stopEvent.executeFrame = stopFrame;
        stopEvent.logicCommitSequence = 2;
        stopEvent.subeventPhase = 50;
        File(3, stopProgram, {}, &stopEvent, 1);
        if (!controllerOnly) {
            voiceStopProgram = {1, &stopPayload, 1};
            File(4, voiceStopProgram, arm, &stopEvent, 1);
            if (twoVoices) File(10, voiceStopProgram, secondArm, &stopEvent, 1);
        }
        harken::DecisionCommitPayload commit{};
        commit.pages = commitPages;
        commit.armGroup = {0, 1};
        harken::DecisionParticipantRef participant{harken::DecisionParticipantKind::Subdecision,
            memberDecision.slot, memberDecision.generation};
        if (variable && !future) {
            commit.participants = &participant;
            commit.participantCount = 1;
        }
        ASSERT_TRUE(decisions.Commit(decision, commit));
        ASSERT_TRUE(decisions.PublishFrontierActivation(decision));
        if (secondCoordinatorMissing || secondCoordinatorWithEvent) {
            request.ticket.sequence = 3;
            request.sliceCount = 1;
            ASSERT_TRUE(decisions.Reserve(request, &secondDecision));
            harken::Plan03GroupSliceInput secondInput{};
            secondInput.executeFrame = secondCoordinatorFrame;
            secondInput.decision = secondDecision;
            ASSERT_TRUE(pool.ReserveImmediateLaneGroup(17, 0, secondInput, arms,
                &secondGroup));
            ASSERT_TRUE(pool.BindImmediateLaneGroupOwner(
                secondGroup, lane, 17, laneHandle.slot, laneHandle.generation));
            ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, 0));
            secondDescriptor = descriptor;
            secondDescriptor.entry.coordinator = secondDecision;
            secondDescriptor.primary = secondGroup;
            secondDescriptor.order.executeFrame = secondCoordinatorFrame;
            secondDescriptor.order.logicCommitSequence = request.ticket.sequence;
            secondDescriptor.expectedMemberCount = 0;
            secondDescriptor.expectedEntryEventCount = 1;
            if (secondCoordinatorWithEvent) {
                secondProgram = decisions.PrepareFrontierControlProgram(secondDecision, 1);
                ASSERT_NE(nullptr, secondProgram);
                auto control = Event(secondCoordinatorFrame, 0, 20, harken::FrontierOperationKind::Entry, 0);
                control.logicCommitSequence = request.ticket.sequence;
                control.frontier.entry = secondDescriptor.entry;
                control.frontier.group = secondGroup;
                File(11, *secondProgram, {}, &control, 1, &secondDescriptor.order);
            }
            ASSERT_TRUE(decisions.PrepareFrontierActivation(secondDecision,
                secondDescriptor, pool));
            auto secondBinding = binding;
            secondBinding.entry = secondDescriptor.entry;
            secondBinding.primary = secondGroup;
            secondBinding.operation.group = secondGroup;
            secondBinding.operation.order = secondDescriptor.order;
            ASSERT_TRUE(pool.BindPlan03GroupFrontier(secondGroup, secondBinding));
            harken::DecisionCommitPayload secondCommit{};
            secondCommit.pages = commitPages;
            secondCommit.armGroup = {0, 1};
            ASSERT_TRUE(decisions.Commit(secondDecision, secondCommit));
            ASSERT_TRUE(decisions.PublishFrontierActivation(secondDecision));
        }
        pool.Publish(kBlock, nullptr, nullptr, &arms);
        if (laneBackedEntry) {
            ASSERT_TRUE(pool.RebaseImmediateLaneGroups(17, 0));
            if (variable && !future && !controllerOnly) {
                ASSERT_EQ(1u, member.memberCount);
                std::uint32_t matches = 0;
                for (std::size_t index = 0; index < laneEventCount; ++index) {
                    const auto& laneEvent = laneEvents[index];
                    const auto& token = laneEvent.event.armToken;
                    if (!(laneEvent.event.frontier.group == member.identity) ||
                        token.slot != member.members[0].slot ||
                        token.generation != member.members[0].generation)
                        continue;
                    harken::ImmediateLaneMemberManifest declaration{};
                    declaration.group = member.identity;
                    declaration.arm = token;
                    declaration.relativeFrame = laneEvent.relativeFrame;
                    declaration.future = laneEvent.futureGroupedStart;
                    declaration.programGeneration =
                        laneEvent.event.program.programGeneration;
                    declaration.target =
                        laneEvent.event.program.program->payloads[
                            laneEvent.event.payload].target;
                    declaration.ownerInstance =
                        laneEvent.event.ownerInstanceHandle;
                    declaration.ownerEpoch =
                        laneEvent.event.ownerInstanceScheduleEpoch;
                    ASSERT_TRUE(lane.DeclareMember(laneHandle, declaration));
                    ++matches;
                }
                ASSERT_EQ(1u, matches);
            }
            ASSERT_TRUE(lane.Prepare(laneHandle, laneEvents.data(),
                static_cast<std::uint32_t>(laneEventCount)));
            std::uint64_t anchor = UINT64_MAX;
            ASSERT_TRUE(lane.TryPlace(laneHandle, 0, 0, &anchor));
            ASSERT_EQ(0u, anchor);
            ASSERT_TRUE(lane.PinGroups(laneHandle, pool, decisions,
                config.maxPlan03GroupHeadersPerPage));
            if (secondCoordinatorMissing) {
                std::uint64_t movedAnchor = UINT64_MAX;
                EXPECT_FALSE(lane.TryPlace(laneHandle, kBlock, 0, &movedAnchor));
            }
            ASSERT_TRUE(lane.PinArms(laneHandle, arms));
            ASSERT_TRUE(pool.PublishImmediateLaneGroups(17));
            ASSERT_TRUE(lane.Publish(laneHandle));
            if (secondCoordinatorMissing) {
                const auto* bucket = lane.CaptureDue(laneHandle.slot, 0);
                ASSERT_NE(nullptr, bucket);
                EXPECT_EQ(3u, bucket->groupCount);
                ASSERT_TRUE(lane.ReleaseCapture(laneHandle.slot, bucket));
            }
            ASSERT_TRUE(sink.AttachPlan03ScratchAuthority(decisions, pool));
            ASSERT_TRUE(sink.AttachImmediateLane(lane, kBlock));
        }
        if (!laneEntry) ASSERT_TRUE(sink.AttachPlan03ScratchAuthority(decisions, pool));
        if (variable) {
            ASSERT_TRUE(sink.AttachQueueAuthority(queues, armGroups, occurrences));
            ASSERT_TRUE(sink.AttachVariableControlAuthority(variables));
            ASSERT_TRUE(sink.AttachTransitionAuthority(transitions));
        }
        sink.SetArenaAdmission(UINT32_MAX, kBlock);
        ASSERT_TRUE(executor.Reset(kBlock, config.maxEventsPerQuantum, &pool, &arms, &sink, &decisions));
        executor.SetDigestSink(&digest);
    }

    harken::ScheduledEventInput Event(std::uint64_t frame, std::uint32_t payload,
        std::uint32_t phase, harken::FrontierOperationKind kind,
        std::uint32_t ordinal = UINT32_MAX) const {
        harken::ScheduledEventInput event{};
        event.executeFrame = frame;
        event.logicCommitSequence = 1;
        event.eventIndex = event.payload = payload;
        event.subeventPhase = phase;
        event.frontier.kind = kind;
        event.frontier.entry = descriptor.entry;
        event.frontier.owner = descriptor.entry.owner;
        event.frontier.transport = event.frontier.world = 1;
        event.frontier.operationOrdinal = payload;
        event.frontier.operationEventCount = 1;
        event.frontier.entryEventOrdinal = ordinal;
        return event;
    }
    void File(std::uint64_t id, const harken::TimelineProgramView& program,
        harken::TimelineArmToken token, const harken::ScheduledEventInput* events, std::size_t count,
        harken::TimelineExecutionOrder* order = nullptr) {
        if (laneBackedEntry && events[0].executeFrame >= kBlock &&
            (events[0].frontier.kind == harken::FrontierOperationKind::Entry ||
             (program.role == harken::TimelineProgramRole::Voice &&
              program.payloads[events[0].payload].kind == harken::TimelineEventKind::Start &&
              program.payloads[events[0].payload].reserved == harken::kTimelineEventTransientStartArm))) {
            // Native lane producers freeze authored events directly, without page ownership.
            for (std::size_t index = 0; index < count; ++index) {
                ASSERT_LT(laneEventCount, laneEvents.size());
                auto& laneEvent = laneEvents[laneEventCount++];
                laneEvent = {};
                laneEvent.relativeFrame = events[index].executeFrame;
                auto& event = laneEvent.event;
                event.executeFrame = events[index].executeFrame;
                event.logicCommitSequence = events[index].logicCommitSequence;
                event.executionOrdinal = events[index].executionOrdinal;
                event.eventIndex = events[index].eventIndex;
                event.transactionOrdinal = static_cast<std::uint32_t>(index);
                event.subeventPhase = events[index].subeventPhase;
                event.rowSequence = id * 8u + index + 1u;
                event.payload = events[index].payload;
                event.program = {program.generation, &program};
                event.armToken = token;
                event.frontier = events[index].frontier;
                event.transactionId = id;
                event.ownerInstanceHandle = event.ownerInstanceScheduleEpoch = 1;
                event.frozenTxnValid = true;
                laneEvent.futureGroupedStart = event.frontier.kind == harken::FrontierOperationKind::Dependent;
                if (laneEvent.futureGroupedStart) laneEvent.armProgramGeneration = program.generation;
                if (index == 0 && order != nullptr)
                    *order = {event.executeFrame, event.logicCommitSequence, event.executionOrdinal,
                        event.eventIndex, event.transactionOrdinal, event.subeventPhase, event.rowSequence};
            }
            return;
        }
        harken::TransactionRequest request{};
        request.transactionId = id;
        request.transportEpoch = request.ownerInstanceHandle = request.ownerInstanceScheduleEpoch = 1;
        request.program = {1, &program};
        request.armToken = token;
        request.events = events;
        request.eventCount = count;
        harken::PreparedTransactionBatch prepared{};
        ASSERT_EQ(harken::ReserveRefusal::None, pool.PrepareTransactionBatch(&request, 1, 0, &prepared));
        ASSERT_TRUE(pool.FreezePreparedTransactionBatch(prepared, events[0].logicCommitSequence));
        harken::ConsumedEvent actual{};
        ASSERT_TRUE(pool.CapturePreparedTransactionEvent(prepared, 0, 0, &actual));
        if (laneBackedEntry) {
            for (std::size_t index = 0; index < count; ++index) {
                harken::ConsumedEvent captured{};
                ASSERT_TRUE(pool.CapturePreparedTransactionEvent(prepared, 0,
                    static_cast<std::uint32_t>(index), &captured));
                const bool delayedStart = captured.frontier.kind == harken::FrontierOperationKind::Dependent &&
                    captured.program.program->payloads[captured.payload].kind == harken::TimelineEventKind::Start &&
                    captured.program.program->payloads[captured.payload].reserved == harken::kTimelineEventTransientStartArm;
                if (captured.frontier.kind != harken::FrontierOperationKind::Entry && id != 9 && !delayedStart)
                    continue;
                ASSERT_LT(laneEventCount, laneEvents.size());
                auto& target = laneEvents[laneEventCount++];
                target = {};
                target.relativeFrame = captured.executeFrame;
                target.event = captured;
                target.futureGroupedStart = delayedStart;
                if (delayedStart) target.armProgramGeneration = captured.program.programGeneration;
            }
        }
        if (order != nullptr) {
            *order = {actual.executeFrame, actual.logicCommitSequence, actual.executionOrdinal,
                actual.eventIndex, actual.transactionOrdinal, actual.subeventPhase, actual.rowSequence};
        }
        if (laneBackedEntry)
            ASSERT_TRUE(pool.CancelPreparedTransactionBatch(&prepared));
        else
            ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&prepared, events[0].logicCommitSequence));
    }
    harken::FrontierActivationReceipt Receipt(std::uint64_t audioSequence = 1u) {
        harken::FrontierActivationReceipt receipt{};
        EXPECT_EQ(harken::FrontierReceiptRead::Ready,
            decisions.ResolveFrontierActivation(decision)->Read(descriptor.entry, audioSequence, &receipt));
        return receipt;
    }
    struct Digest final : harken::TimelineDigestSink {
        void Fold(const harken::TimelineDigestRecord& record) noexcept override {
            if (count < records.size()) records[count++] = record;
        }
        std::array<harken::TimelineDigestRecord, 16> records{};
        std::size_t count = 0;
    } digest;
    StopAudioHarness audio;
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{2};
    harken::DecisionTable decisions{2, 2};
    harken::ImmediateActionLane lane;
    harken::ImmediateLaneHandle laneHandle{};
    harken::Plan03GroupSliceIdentity lanePrimary{};
    harken::Plan03GroupSliceIdentity laneMember{};
    std::array<harken::ImmediateLaneEvent, 8> laneEvents{};
    std::size_t laneEventCount = 0;
    bool laneBackedEntry = false;
    harken::VariableControlInstanceTable variables{1};
    harken::QueueInstanceTable queues{1};
    harken::QueueInstanceToken queue{};
    harken::OccurrenceStatePool occurrences{2};
    harken::OccurrenceToken currentOccurrence{}, successorOccurrence{};
    harken::ConditionalQueueVersionToken conditionalVersion{};
    VnVoiceHandle sourceRender = 0;
    std::vector<float> sourceSamples = std::vector<float>(16, 0.25f);
    harken::ArmGroupTable armGroups{2, 2};
    harken::VariableControlInstanceToken instance{};
    harken::BankVersionToken bank{2, 3};
    harken::DecisionToken memberDecision{};
    harken::ArmGroupToken memberArmGroup{};
    harken::Plan03PageGroupView member{};
    harken::Plan03PageGroupView later{};
    harken::TransitionInstancePool transitions{1};
    harken::TransitionInstanceToken transition{};
    const harken::TimelineProgramView* memberProgram = nullptr;
    harken::VoiceTimelineSink sink;
    harken::TimelineExecutor executor;
    harken::DecisionToken decision{};
    harken::DecisionToken secondDecision{};
    harken::Plan03GroupSliceIdentity secondGroup{};
    harken::FrontierActivationDescriptor secondDescriptor{};
    const harken::TimelineProgramView* secondProgram = nullptr;
    harken::PreparedGroupToken commitPages{0, 1};
    harken::Plan03PageGroupView group{};
    harken::FrontierActivationDescriptor descriptor{};
    harken::TimelineArmToken arm{};
    harken::TimelineArmReaderPin armPin{};
    harken::TimelineArmToken sourceArm{};
    harken::TimelineArmReaderPin sourcePin{};
    std::array<harken::TimelineArmToken, 2> queueMembers{};
    VnVoiceHandle render = 0;
    VnVoiceHandle secondRender = 0;
    harken::TimelineArmToken secondArm{};
    harken::TimelineArmReaderPin secondPin{};
    harken::TimelineEventPayload stopPayload{};
    std::array<harken::TimelineEventPayload, 3> voicePayloads{};
    const harken::TimelineProgramView* controllerProgram = nullptr;
    harken::TimelineProgramView voiceProgram{}, stopProgram{}, voiceStopProgram{};
    std::optional<harken::AuthoredStopOperation> stopOperation;
    std::optional<harken::AuthoredStopFenceDescriptor> stopFence;
};

TEST(AuthoredStopAudio, AbandonedWorkerKeepsFrozenFrontierWhileNextQuantumFinalizes) {
    FrontierAudioHarness h(1);
    h.audio.retainSources = true;
    h.audio.captureControl = true;
    h.Prepare(32, false, false, false, harken::VariableControlSide::Discrete,
        false, false, false, false, UINT64_MAX, true);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    auto& engine = *h.audio.engine;
    struct Gate {
        std::thread::id coordinator = std::this_thread::get_id();
        std::atomic<bool> entered{false}, release{false}, exited{false};
        const harken::VoiceEventSpan* span = nullptr;
        harken::VoiceEventSpan before{};
        harken::SourceTerminalRecord* source = nullptr;
        harken::SourceTerminalToken sourceToken{};
    } gate;
    struct JoinGuard {
        Gate& gate;
        harken::VoicePool& voices;
        ~JoinGuard() {
            gate.release.store(true, std::memory_order_release);
            harken::detail::VoicePoolReleaseTestAccess::JoinWorkers(voices);
        }
    };
    struct EffectContext { Gate* gate; harken::VoiceRenderContext* voice; } effects[2]{};
    JoinGuard join{gate, engine.voices};
    const std::array renders{h.render, h.secondRender};
    for (std::size_t index = 0; index < renders.size(); ++index) {
        auto* voice = engine.voices.Resolve(renders[index]);
        ASSERT_NE(nullptr, voice);
        effects[index] = {&gate, voice};
        voice->fxSlots[0].state = &effects[index];
        voice->fxSlots[0].process = [](void* opaque, float* const*, std::uint32_t, std::uint32_t, std::uint32_t) {
            auto& effect = *static_cast<EffectContext*>(opaque);
            auto& gate = *effect.gate;
            if (std::this_thread::get_id() == gate.coordinator) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (!gate.entered.load(std::memory_order_acquire) &&
                    std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            } else {
                gate.span = &effect.voice->timelineEvents;
                gate.before = *gate.span;
                gate.source = effect.voice->sourceTerminal;
                gate.sourceToken = effect.voice->sourceTerminalToken;
                gate.entered.store(true, std::memory_order_release);
                while (!gate.release.load(std::memory_order_acquire)) std::this_thread::yield();
                gate.exited.store(true, std::memory_order_release);
            }
        };
    }
    h.executor.ExecuteQuantum(0);
    ASSERT_EQ(harken::FrontierReceiptOutcome::Applied, h.Receipt().outcome);
    harken::detail::VoicePoolReleaseTestAccess::SetWorkerDeadline(engine.voices, harken::CpuClockTicksPerSecond() / 20u);
    const auto rendered = engine.voices.RenderAll(h.audio.arena, h.audio.planes.data(), kBlock,
        engine.notifications, 0, 0, 0);
    ASSERT_TRUE(gate.entered.load(std::memory_order_acquire));
    ASSERT_TRUE(rendered.batchAbandoned);
    ASSERT_GT(rendered.quarantinedVoices, 0u);
    ASSERT_NE(nullptr, gate.span);
    ASSERT_NE(nullptr, gate.source);
    ASSERT_GE(gate.before.eventCount, 2u);
    EXPECT_EQ(0u, gate.before.quantumIndex);
    EXPECT_EQ(harken::FrontierEventState::Applied, gate.before.events[0].frontierProof.state);

    const harken::AuthoredStopOperationIdentity nextIdentity{1, 2, 1, 1, 1, 1, 80};
    harken::AuthoredStopOperation nextStop(nextIdentity);
    auto nextPayload = h.stopPayload;
    nextPayload.envelopeEndFrame = 80;
    nextPayload.authoredStopOperation = nextIdentity;
    harken::TimelineProgramView nextProgram{1, &nextPayload, 1};
    nextProgram.role = harken::TimelineProgramRole::AuthoredStopFence;
    harken::AuthoredStopFenceDescriptor nextFence(&nextStop, nextIdentity, {}, &nextProgram, 1);
    nextProgram.authoredStopFence = &nextFence;
    harken::ScheduledEventInput nextEvent{};
    nextEvent.executeFrame = 80;
    nextEvent.logicCommitSequence = 3;
    nextEvent.subeventPhase = 50;
    h.File(11, nextProgram, {}, &nextEvent, 1);
    auto nextControl = h.Event(72, 1, 30, harken::FrontierOperationKind::Dependent);
    nextControl.logicCommitSequence = 3;
    h.File(12, h.voiceProgram, gate.source->render == h.render ? h.arm : h.secondArm, &nextControl, 1);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    h.pool.Publish(2u * kBlock, nullptr, nullptr, &h.arms);
    const auto beforeDigest = h.digest.count;
    h.executor.ExecuteQuantum(1);
    EXPECT_GT(h.digest.count, beforeDigest);
    EXPECT_EQ(2u, nextStop.AppliedAudioSequence());
    EXPECT_FALSE(gate.exited.load(std::memory_order_acquire));
    EXPECT_EQ(gate.before.quantumIndex, gate.span->quantumIndex);
    EXPECT_EQ(gate.before.renderGeneration, gate.span->renderGeneration);
    ASSERT_EQ(gate.before.eventCount, gate.span->eventCount);
    for (std::uint32_t index = 0; index < gate.before.eventCount; ++index) {
        const auto& before = gate.before.events[index];
        const auto& after = gate.span->events[index];
        EXPECT_EQ(before.kind, after.kind);
        EXPECT_EQ(before.offset, after.offset);
        EXPECT_EQ(before.authoredStopOperation, after.authoredStopOperation);
        EXPECT_EQ(before.frontierProof.state, after.frontierProof.state);
        EXPECT_EQ(before.frontierProof.entry, after.frontierProof.entry);
        const auto operationIdentity = [](const harken::FrontierDueOperationRef& operation) {
            return std::tuple(operation.kind, operation.owner, operation.order.executeFrame,
                operation.order.logicCommitSequence, operation.order.executionOrdinal, operation.order.eventIndex,
                operation.order.transactionOrdinal, operation.order.subeventPhase, operation.order.rowSequence,
                operation.group.absoluteQuantum, operation.group.pageGeneration, operation.group.decision.slot,
                operation.group.decision.generation, operation.group.sliceIndex, operation.transport, operation.world,
                operation.transactionId, operation.transactionSlot, operation.program, operation.programGeneration,
                operation.armSlot, operation.armGeneration, operation.renderSlot, operation.renderGeneration);
        };
        EXPECT_EQ(operationIdentity(before.frontierProof.operation), operationIdentity(after.frontierProof.operation));
        EXPECT_EQ(before.frontierProof.quantum, after.frontierProof.quantum);
        EXPECT_EQ(before.frontierProof.renderGeneration, after.frontierProof.renderGeneration);
        EXPECT_EQ(before.frontierProof.cutoffFrame, after.frontierProof.cutoffFrame);
        EXPECT_EQ(before.frontierProof.cutoffOperation, after.frontierProof.cutoffOperation);
    }
    harken::SourceTerminalPayload terminal{};
    EXPECT_FALSE(engine.sourceTerminals.Capture(gate.sourceToken, UINT64_MAX, &terminal));
    const auto binding = std::find_if(h.audio.controlBindings.begin(), h.audio.controlBindings.end(),
        [&](const auto& candidate) { return candidate.render == gate.source->render; });
    ASSERT_NE(h.audio.controlBindings.end(), binding);
    harken::MonitorVoiceControlFact control{};
    EXPECT_FALSE(binding->receipt->TryRead(*binding, &control));
    EXPECT_TRUE(engine.playableInstances.HasSourceRetirementHold(gate.source->leaf));
    EXPECT_TRUE(engine.playableInstances.Classify(gate.source->leaf).live);
    gate.release.store(true, std::memory_order_release);
    harken::detail::VoicePoolReleaseTestAccess::JoinWorkers(engine.voices);
    EXPECT_TRUE(gate.exited.load(std::memory_order_acquire));
    EXPECT_FALSE(engine.sourceTerminals.Capture(gate.sourceToken, UINT64_MAX, &terminal));
    EXPECT_FALSE(binding->receipt->TryRead(*binding, &control));
    EXPECT_TRUE(engine.playableInstances.HasSourceRetirementHold(gate.source->leaf));
}

TEST(AuthoredStopAudio, FrontierExecutorFinalizesEntryAndEarlierControlBeforeStop) {
    for (const auto workers : {0u, 2u}) {
        FrontierAudioHarness h(workers);
        h.Prepare();
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        h.executor.ExecuteQuantum(0);
        EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, h.Receipt().outcome);
        EXPECT_EQ(1u, h.stopOperation->AppliedAudioSequence());
        ASSERT_EQ(6u, h.digest.count);
        for (std::size_t i = 0; i < h.digest.count; ++i) {
            const auto& record = h.digest.records[i];
            EXPECT_EQ(record.event.executeFrame == 48 ? harken::kTimelineDispositionStopDominated :
                harken::kTimelineDispositionApplied, record.disposition) << record.event.executeFrame;
        }
        h.audio.Render();
        for (std::uint32_t i = 0; i < 8; ++i) EXPECT_EQ(0.0f, h.audio.Sample(i));
        for (std::uint32_t i = 8; i < 16; ++i) EXPECT_NEAR(0.25f, h.audio.Sample(i), 1e-6f);
        EXPECT_NEAR(0.25f * (11.0f / 15.0f), h.audio.Sample(20), 1e-6f);
        EXPECT_NEAR(0.0f, h.audio.Sample(31), 1e-6f);
    }
}

TEST(AuthoredStopAudio, FrontierExecutorStopAtEntryRejectsWholeEntryIncludingEmptyController) {
    for (const bool empty : {false, true}) {
        for (const auto stop : {4u, 8u}) {
            FrontierAudioHarness h(0);
            h.Prepare(stop, empty);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            h.executor.ExecuteQuantum(0);
            const auto receipt = h.Receipt();
            EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied, receipt.outcome);
            EXPECT_EQ(harken::FrontierReceiptReason::AuthoredStop, receipt.reason);
            EXPECT_EQ(1u, h.stopOperation->AppliedAudioSequence());
            h.audio.Render();
            for (std::uint32_t i = 0; i < kBlock; ++i) EXPECT_EQ(0.0f, h.audio.Sample(i));
        }
    }
}

TEST(AuthoredStopAudio, FrontierExecutorStopAtDependentDoesNotRewriteAppliedEntry) {
    FrontierAudioHarness h(0);
    h.Prepare(16);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    h.executor.ExecuteQuantum(0);
    EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, h.Receipt().outcome);
    for (std::size_t i = 0; i < h.digest.count; ++i) {
        const auto& record = h.digest.records[i];
        if (record.event.frontier.kind == harken::FrontierOperationKind::Dependent)
            EXPECT_EQ(harken::kTimelineDispositionStopDominated, record.disposition);
    }
    h.audio.Render();
    for (std::uint32_t i = 8; i < 16; ++i) EXPECT_NEAR(0.25f, h.audio.Sample(i), 1e-6f);
}

TEST(AuthoredStopAudio, FrontierExecutorMissingFinalEntryLeavesNoPartialVoiceOrReceipt) {
    FrontierAudioHarness h(0);
    h.Prepare(32, false, true);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    h.executor.ExecuteQuantum(0);
    const auto receipt = h.Receipt();
    EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied, receipt.outcome);
    EXPECT_EQ(harken::FrontierReceiptReason::MissingMember, receipt.reason);
    harken::TimelineArmObservation arm{};
    ASSERT_TRUE(h.arms.Observe(h.arm, &arm));
    EXPECT_EQ(0u, arm.startedBlockSequence);
    h.audio.Render();
    for (std::uint32_t i = 0; i < kBlock; ++i) EXPECT_EQ(0.0f, h.audio.Sample(i));
}

TEST(AuthoredStopAudio, FrontierVariableMemberCommitsActualBankAndDecisionWithWholeEntry) {
    for (const auto workers : {2u}) {
        for (const auto side : {harken::VariableControlSide::Discrete, harken::VariableControlSide::Continuous}) {
            for (const bool empty : {false, true}) {
                FrontierAudioHarness h(workers);
                h.Prepare(32, empty, false, true, side);
                ASSERT_FALSE(::testing::Test::HasFatalFailure());
                h.executor.ExecuteQuantum(0);
                EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, h.Receipt().outcome);
                harken::VariableControlInstanceSnapshot state{};
                ASSERT_TRUE(h.variables.Capture(h.instance, &state));
                const auto& actual = side == harken::VariableControlSide::Discrete ? state.discrete : state.continuous;
                EXPECT_EQ(harken::VariableControlSideState::Committed, actual.state);
                EXPECT_EQ(h.bank.generation, actual.lastApplied.generation);
                harken::DecisionCommittedSnapshot decision{};
                ASSERT_TRUE(h.decisions.CaptureCommitted(h.memberDecision, &decision));
                EXPECT_EQ(harken::AudioGroupState::Admitted, decision.audioState);
                EXPECT_EQ(1u, decision.consumedSlices);
                h.audio.Render();
                for (std::uint32_t frame = 0; frame < 8; ++frame) EXPECT_EQ(0.0f, h.audio.Sample(frame));
                for (std::uint32_t frame = 8; frame < 32; ++frame)
                    EXPECT_NEAR(empty ? 0.0f : 0.25f, h.audio.Sample(frame), 1e-6f);
            }
        }
    }
}

TEST(AuthoredStopAudio, FrontierVariableMemberFailureLeavesBankDecisionAndAudioUnapplied) {
    for (const bool empty : {false, true}) {
        for (const bool missing : {false, true}) {
            FrontierAudioHarness h(0);
            h.Prepare(missing ? 32 : 8, empty, missing, true);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            h.executor.ExecuteQuantum(0);
            EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied, h.Receipt().outcome);
            harken::VariableControlInstanceSnapshot state{};
            ASSERT_TRUE(h.variables.Capture(h.instance, &state));
            EXPECT_EQ(harken::VariableControlSideState::Pending, state.discrete.state);
            EXPECT_FALSE(state.discrete.lastApplied.Valid());
            harken::DecisionCommittedSnapshot decision{};
            ASSERT_TRUE(h.decisions.CaptureCommitted(h.memberDecision, &decision));
            EXPECT_EQ(harken::AudioGroupState::Pending, decision.audioState);
            EXPECT_EQ(0u, decision.consumedSlices);
            h.audio.Render();
            for (std::uint32_t frame = 0; frame < kBlock; ++frame) EXPECT_EQ(0.0f, h.audio.Sample(frame));
        }
    }
}

TEST(AuthoredStopAudio, LaneVariableEntryFlowsThroughExecutorAndSink) {
    for (const bool missing : {false, true}) {
        FrontierAudioHarness h(0);
        h.Prepare(32, false, missing, true, harken::VariableControlSide::Discrete,
            false, false, false, false, UINT64_MAX, false, true);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        h.executor.ExecuteQuantum(0);
        const auto receipt = h.Receipt();
        EXPECT_EQ(missing ? harken::FrontierReceiptOutcome::DefinitivelyNotApplied :
            harken::FrontierReceiptOutcome::Applied, receipt.outcome);
        harken::ImmediateLaneObservation observation{};
        ASSERT_TRUE(h.lane.Observe(h.laneHandle, &observation));
        EXPECT_TRUE(observation.complete);
        EXPECT_EQ(!missing, observation.applied);
        EXPECT_EQ(missing, observation.missed);
        h.audio.Render();
        for (std::uint32_t frame = 0; frame < kBlock; ++frame)
            EXPECT_NEAR(!missing && frame >= 8 ? 0.25f : 0.0f,
                h.audio.Sample(frame), 1e-6f) << frame;
    }
}

TEST(AuthoredStopAudio, LanePlainStartAndVariableEntryShareBucketDecision) {
    for (const bool plainBefore : {false, true}) {
        for (const bool missing : {false, true}) {
            FrontierAudioHarness h(0);
            h.Prepare(32, false, missing, true, harken::VariableControlSide::Discrete,
                false, false, false, false, UINT64_MAX, true, true, false, false,
                plainBefore);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            h.executor.ExecuteQuantum(0);
            EXPECT_EQ(missing ? harken::FrontierReceiptOutcome::DefinitivelyNotApplied :
                harken::FrontierReceiptOutcome::Applied, h.Receipt().outcome);
            harken::ImmediateLaneObservation observation{};
            ASSERT_TRUE(h.lane.Observe(h.laneHandle, &observation));
            EXPECT_TRUE(observation.complete);
            EXPECT_EQ(!missing, observation.applied);
            EXPECT_EQ(missing, observation.missed);
            h.audio.Render();
            for (std::uint32_t frame = 0; frame < kBlock; ++frame)
                EXPECT_NEAR(!missing && frame >= 8 ? 0.5f : 0.0f,
                    h.audio.Sample(frame), 1e-6f) << plainBefore << missing << frame;
        }
    }
}

TEST(AuthoredStopAudio, LaneSinkRejectsMissingSecondCoordinatorWithWholeBucket) {
    FrontierAudioHarness h(0, 3);
    h.Prepare(32, false, false, true, harken::VariableControlSide::Discrete,
        false, false, false, false, UINT64_MAX, false, true, true);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    h.executor.ExecuteQuantum(0);
    EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied,
        h.Receipt().outcome);
    harken::FrontierActivationReceipt second{};
    const auto* activation = h.decisions.ResolveFrontierActivation(h.secondDecision);
    ASSERT_NE(nullptr, activation);
    ASSERT_EQ(harken::FrontierReceiptRead::Ready,
        activation->Read(h.secondDescriptor.entry, 1, &second));
    EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied,
        second.outcome);
    h.audio.Render();
    for (std::uint32_t frame = 0; frame < kBlock; ++frame)
        EXPECT_EQ(0.0f, h.audio.Sample(frame));
}

TEST(AuthoredStopAudio, LaneCaptureKeepsGroupAndDecisionAliveAcrossCancellation) {
    FrontierAudioHarness h(0);
    h.Prepare(32, false, false, true, harken::VariableControlSide::Discrete,
        false, false, false, false, UINT64_MAX, false, true);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    ASSERT_TRUE(h.sink.CaptureIndependentWork(0));
    ASSERT_TRUE(h.lane.Cancel(h.laneHandle));
    EXPECT_FALSE(h.lane.Retire(h.laneHandle));
    (void)h.decisions.Invalidate(h.decision);
    (void)h.decisions.Invalidate(h.memberDecision);
    ASSERT_TRUE(h.variables.CancelSide(h.instance,
        harken::VariableControlSide::Discrete, h.memberDecision));
    h.pool.SettleInvalidatedImmediateLaneFrontiers();
    ASSERT_TRUE(h.pool.DetachPlan03GroupFrontier(h.laneMember, 0));
    ASSERT_TRUE(h.pool.DetachPlan03GroupFrontier(h.lanePrimary, 0));
    harken::FrontierActivationReceipt cancelled{};
    ASSERT_TRUE(h.decisions.ReconcileFrontierActivation(h.decision,
        h.descriptor.entry, 0, &cancelled));
    ASSERT_TRUE(h.decisions.ReleaseFrontierDecisionBinding(h.memberDecision));
    ASSERT_TRUE(h.decisions.Retire(h.memberDecision, 1));
    EXPECT_FALSE(h.decisions.ReclaimRetired(h.memberDecision));
    harken::DecisionCommittedSnapshot memberSnapshot{};
    ASSERT_TRUE(h.decisions.CaptureCommitted(h.memberDecision, &memberSnapshot));
    EXPECT_EQ(1u, memberSnapshot.futurePinCount);
    EXPECT_FALSE(h.pool.CancelImmediateLaneGroups(17, 0));
    harken::Plan03PageGroupView group{};
    ASSERT_TRUE(h.pool.ResolvePlan03GroupSliceProducer(h.lanePrimary, &group));
    ASSERT_NE(nullptr, group.frontier);
    ASSERT_TRUE(h.pool.ResolvePlan03GroupSliceProducer(h.laneMember, &group));
    ASSERT_NE(nullptr, group.frontier);
    ASSERT_NE(nullptr, h.memberProgram);

    h.executor.ExecuteQuantum(0);
    EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied,
        h.Receipt().outcome);
    h.audio.Render();
    for (std::uint32_t frame = 0; frame < kBlock; ++frame)
        EXPECT_EQ(0.0f, h.audio.Sample(frame));
    EXPECT_TRUE(h.lane.Retire(h.laneHandle, 2));
    EXPECT_TRUE(h.pool.CancelImmediateLaneGroups(17, 2));
    EXPECT_FALSE(h.pool.ResolvePlan03GroupSliceProducer(h.lanePrimary, &group));
    EXPECT_FALSE(h.pool.ResolvePlan03GroupSliceProducer(h.laneMember, &group));
    EXPECT_TRUE(h.decisions.ReclaimRetired(h.memberDecision));
    harken::DecisionReserveRequest replacement{};
    replacement.ticket.sequence = 3;
    replacement.world = replacement.transport =
        replacement.expectedContinuationGeneration = 1;
    replacement.sliceCount = 1;
    harken::DecisionToken reused{};
    ASSERT_TRUE(h.decisions.Reserve(replacement, &reused));
    EXPECT_EQ(h.memberDecision.slot, reused.slot);
    EXPECT_NE(h.memberDecision.generation, reused.generation);
}

TEST(AuthoredStopAudio, LaneBackedVariableEntryAppliesOnlyAfterWholeBucketClaim) {
    for (const int scenario : {0, 1, 2}) {
        const bool missing = scenario == 1;
        const bool fakeClaim = scenario == 2;
        const bool expectedApplied = !missing && !fakeClaim;
        FrontierAudioHarness h(0);
        h.Prepare(32, false, missing, true, harken::VariableControlSide::Discrete,
            false, false, false, false, UINT64_MAX, false, true);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        harken::TimelineArmObservation armState{};
        ASSERT_TRUE(h.arms.Observe(h.arm, &armState));
        EXPECT_EQ(harken::ArmState::Published, armState.state);
        harken::TimelineArmTable::BoundAudioProof directProof;
        EXPECT_TRUE(h.arms.ProveBoundFrontierArm(h.arm, h.descriptor.entry,
            1, true, &directProof, h.member.frontier));
        directProof.Release();
        harken::FrontierTimelineFinalizer finalizer(h.audio.engine->voices, h.arms);
        ASSERT_TRUE(finalizer.Attach(h.decisions, h.pool, true));
        ASSERT_TRUE(finalizer.AttachLaneEventStorage(h.lane));
        finalizer.AttachArmGroups(h.armGroups);
        finalizer.AttachVariableControls(h.variables);
        const auto* bucket = h.lane.CaptureDue(h.laneHandle.slot, 0);
        ASSERT_NE(nullptr, bucket);
        finalizer.BeginLane(0, kBlock, h.lane, bucket, h.laneHandle.slot);
        std::array<harken::Plan03PageGroupView, 4> groups{};
        ASSERT_EQ(2u, h.pool.CaptureImmediateLaneGroupsRt(0,
            groups.data(), groups.size()));
        for (const auto& group : groups)
            if (group.identity.Valid()) ASSERT_TRUE(finalizer.CollectGroup(group));
        const auto* events = h.lane.Events(h.laneHandle.slot, bucket);
        ASSERT_NE(nullptr, events);
        EXPECT_EQ(3u, bucket->eventCount);
        for (std::uint32_t i = 0; i < bucket->eventCount; ++i) {
            const auto& source = events[i].event;
            const auto* program = source.program.program;
            ASSERT_NE(nullptr, program);
            ASSERT_LT(source.payload, program->payloadCount);
            if (program->payloads[source.payload].kind == harken::TimelineEventKind::Start) {
                EXPECT_EQ(h.member.identity, source.frontier.group);
                EXPECT_EQ(h.arm.slot, source.armToken.slot);
                EXPECT_EQ(h.arm.generation, source.armToken.generation);
                harken::TimelineArmTable::BoundAudioProof laneProof;
                EXPECT_TRUE(h.arms.ProveBoundFrontierArm(source.armToken,
                    h.descriptor.entry, 1, true, &laneProof, groups[1].frontier));
                laneProof.Release();
            }
            ASSERT_TRUE(finalizer.CollectEvent(source,
                &program->payloads[source.payload], harken::kTimelineDispositionApplied));
        }
        h.audio.Begin();
        struct Claim {
            harken::ImmediateActionLane* lane;
            std::uint32_t slot;
            std::uint32_t generation;
            const harken::ImmediateLaneBucket* bucket;
            bool fake;
            bool claimed = false;
            harken::ImmediateLaneBucketClaimReceipt receipt{};
        } claim{&h.lane, h.laneHandle.slot, h.laneHandle.generation,
                bucket, fakeClaim};
        const bool applied = finalizer.FinishLaneEntryBucket([](
            void* context, harken::ImmediateLaneBucketClaimReceipt* receipt) noexcept {
            auto& state = *static_cast<Claim*>(context);
            if (state.fake) return true;
            state.claimed = state.lane->Claim(
                {state.slot, state.generation}, state.bucket, receipt);
            if (state.claimed) state.receipt = *receipt;
            return state.claimed;
        }, &claim, &h.lane, bucket, h.laneHandle.slot,
            h.laneHandle.generation);
        EXPECT_EQ(expectedApplied, applied);
        EXPECT_EQ(expectedApplied, claim.claimed);
        if (claim.claimed) {
            EXPECT_TRUE(h.pool.ValidateImmediateLaneGroupClaim(
                h.group.identity, h.group.lease, claim.receipt));
            EXPECT_TRUE(h.pool.ValidateImmediateLaneGroupClaim(
                h.member.identity, h.member.lease, claim.receipt));
            auto wrong = h.member.identity;
            ++wrong.rowClaimGeneration;
            EXPECT_FALSE(h.pool.ValidateImmediateLaneGroupClaim(
                wrong, h.member.lease, claim.receipt));
            EXPECT_FALSE(h.pool.ValidateImmediateLaneGroupClaim(
                h.member.identity, nullptr, claim.receipt));
            EXPECT_TRUE(h.lane.Finish(claim.slot, bucket, applied));
            EXPECT_FALSE(h.pool.ValidateImmediateLaneGroupClaim(
                h.group.identity, h.group.lease, claim.receipt));
        }
        else EXPECT_TRUE(h.lane.ReleaseCapture(claim.slot, bucket));
        harken::VariableControlInstanceSnapshot state{};
        ASSERT_TRUE(h.variables.Capture(h.instance, &state));
        EXPECT_EQ(expectedApplied ? harken::VariableControlSideState::Committed :
            harken::VariableControlSideState::Pending, state.discrete.state);
        EXPECT_EQ(expectedApplied ? harken::FrontierReceiptOutcome::Applied :
            harken::FrontierReceiptOutcome::DefinitivelyNotApplied,
            h.Receipt().outcome);
        if (expectedApplied)
            EXPECT_EQ(harken::FrontierReceiptReason::None, h.Receipt().reason);
        h.audio.Render();
        for (std::uint32_t frame = 8; frame < 16; ++frame)
            EXPECT_NEAR(expectedApplied ? 0.25f : 0.0f,
                        h.audio.Sample(frame), 1e-6f);
    }
}

TEST(AuthoredStopAudio, LaneMixedFutureWaitDoesNotGrantIndependentEntryLatePermission) {
    FrontierAudioHarness h(0, 3);
    h.Prepare(3u * kBlock + 16u, false, false, true, harken::VariableControlSide::Discrete,
        false, false, false, false, UINT64_MAX, false, true, false, true,
        false, 2u * kBlock + 8u, 2u * kBlock + 8u);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    const auto* first = h.lane.CaptureDue(h.laneHandle.slot, 0u);
    ASSERT_NE(nullptr, first);
    ASSERT_TRUE(h.lane.SuspendCaptureForSourceWait(h.laneHandle.slot, first, &h.descriptor.entry, 1u));
    h.executor.ExecuteQuantum(1u);
    const auto receipt = h.Receipt(2u);
    ASSERT_EQ(harken::FrontierReceiptOutcome::Applied, receipt.outcome);
    ASSERT_EQ(2u, receipt.audioSequence);

    harken::FrontierTimelineFinalizer finalizer(h.audio.engine->voices, h.arms);
    ASSERT_TRUE(finalizer.Attach(h.decisions, h.pool, true));
    ASSERT_TRUE(finalizer.AttachLaneEventStorage(h.lane));
    const auto inspect = [&](const harken::ImmediateLaneBucket* bucket, std::uint64_t quantum) {
        finalizer.BeginLane(quantum, kBlock, h.lane, bucket, h.laneHandle.slot);
        const auto* groups = h.lane.Groups(h.laneHandle.slot, bucket);
        for (std::uint32_t index = 0; index < bucket->groupCount; ++index) {
            harken::Plan03PageGroupView group{};
            ASSERT_TRUE(h.pool.ResolvePlan03GroupSliceRt(groups[index].identity, &group));
            ASSERT_TRUE(finalizer.CollectGroup(group));
        }
        const auto* events = h.lane.Events(h.laneHandle.slot, bucket);
        ASSERT_NE(nullptr, events);
        ASSERT_EQ(2u, bucket->eventCount);
        bool foundDependent = false, foundIndependent = false;
        for (std::uint32_t index = 0; index < bucket->eventCount; ++index) {
            const auto& event = events[index].event;
            if (event.frontier.entry == h.descriptor.entry) {
                foundDependent = true;
                EXPECT_EQ(quantum == 2u ? harken::VariableReadCutStatus::Waiting :
                    harken::VariableReadCutStatus::Applied, finalizer.LaneEventTimingStatus(event));
            } else if (event.frontier.entry == h.secondDescriptor.entry) {
                foundIndependent = true;
                EXPECT_EQ(quantum == 2u ? harken::VariableReadCutStatus::Applied :
                    harken::VariableReadCutStatus::Failed, finalizer.LaneEventTimingStatus(event));
            }
        }
        EXPECT_TRUE(foundDependent);
        EXPECT_TRUE(foundIndependent);
        EXPECT_FALSE(h.lane.CapturedEntrySourceWait(h.laneHandle.slot, bucket,
            h.secondDescriptor.entry));
    };
    const auto* mixed = h.lane.CaptureDue(h.laneHandle.slot, 2u);
    ASSERT_NE(nullptr, mixed);
    inspect(mixed, 2u);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    auto forged = h.secondDescriptor.entry;
    ++forged.attempt;
    const std::array rejectedWaits{h.secondDescriptor.entry, forged};
    EXPECT_FALSE(h.lane.SuspendCaptureForSourceWait(h.laneHandle.slot, mixed,
        rejectedWaits.data(), rejectedWaits.size()));
    EXPECT_FALSE(h.lane.CapturedEntrySourceWait(h.laneHandle.slot, mixed, h.secondDescriptor.entry));
    EXPECT_TRUE(h.lane.CapturedEntrySourceWait(h.laneHandle.slot, mixed, h.descriptor.entry));
    ASSERT_TRUE(h.lane.SuspendCaptureForSourceWait(h.laneHandle.slot, mixed));
    const auto* resumed = h.lane.CaptureDue(h.laneHandle.slot, 3u);
    ASSERT_EQ(mixed, resumed);
    inspect(resumed, 3u);
    ASSERT_TRUE(h.lane.ReleaseCapture(h.laneHandle.slot, resumed));
    h.executor.ExecuteQuantum(3u);
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(h.lane.Observe(h.laneHandle, &observation));
    EXPECT_TRUE(observation.protocolRefused);
    ASSERT_TRUE(h.lane.Cancel(h.laneHandle));
}

TEST(AuthoredStopAudio, LaneBucketMissingSecondCoordinatorKeepsFirstUnapplied) {
    FrontierAudioHarness h(0, 3);
    h.Prepare(32, false, false, true, harken::VariableControlSide::Discrete,
        false, false, false, false, UINT64_MAX, false, true, true);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    harken::FrontierTimelineFinalizer finalizer(h.audio.engine->voices, h.arms);
    ASSERT_TRUE(finalizer.Attach(h.decisions, h.pool, true));
    ASSERT_TRUE(finalizer.AttachLaneEventStorage(h.lane));
    finalizer.AttachArmGroups(h.armGroups);
    finalizer.AttachVariableControls(h.variables);
    const auto* bucket = h.lane.CaptureDue(h.laneHandle.slot, 0);
    ASSERT_NE(nullptr, bucket);
    finalizer.BeginLane(0, kBlock, h.lane, bucket, h.laneHandle.slot);
    std::array<harken::Plan03PageGroupView, 4> groups{};
    ASSERT_EQ(3u, h.pool.CaptureImmediateLaneGroupsRt(0,
        groups.data(), groups.size()));
    for (const auto& group : groups)
        if (group.identity.Valid()) ASSERT_TRUE(finalizer.CollectGroup(group));
    const auto* events = h.lane.Events(h.laneHandle.slot, bucket);
    ASSERT_NE(nullptr, events);
    ASSERT_EQ(3u, bucket->eventCount);
    for (std::uint32_t i = 0; i < bucket->eventCount; ++i) {
        const auto& source = events[i].event;
        const auto* program = source.program.program;
        ASSERT_NE(nullptr, program);
        ASSERT_LT(source.payload, program->payloadCount);
        ASSERT_TRUE(finalizer.CollectEvent(source,
            &program->payloads[source.payload], harken::kTimelineDispositionApplied));
    }
    h.audio.Begin();
    bool claimed = false;
    EXPECT_FALSE(finalizer.FinishLaneEntryBucket([](
        void* context, harken::ImmediateLaneBucketClaimReceipt*) noexcept {
        *static_cast<bool*>(context) = true;
        return true;
    }, &claimed, &h.lane, bucket, h.laneHandle.slot,
        h.laneHandle.generation));
    EXPECT_FALSE(claimed);
    EXPECT_TRUE(h.lane.ReleaseCapture(h.laneHandle.slot, bucket));
    const auto first = h.Receipt();
    EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied, first.outcome);
    EXPECT_EQ(harken::FrontierReceiptReason::Cancelled, first.reason);
    harken::FrontierActivationReceipt second{};
    ASSERT_EQ(harken::FrontierReceiptRead::Ready,
        h.decisions.ResolveFrontierActivation(h.secondDecision)->Read(
            h.secondDescriptor.entry, 1, &second));
    EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied, second.outcome);
    EXPECT_EQ(harken::FrontierReceiptReason::MissingMember, second.reason);
    harken::VariableControlInstanceSnapshot state{};
    ASSERT_TRUE(h.variables.Capture(h.instance, &state));
    EXPECT_EQ(harken::VariableControlSideState::Pending, state.discrete.state);
    h.audio.Render();
    for (std::uint32_t frame = 8; frame < 16; ++frame)
        EXPECT_EQ(0.0f, h.audio.Sample(frame));
}

TEST(AuthoredStopAudio, LaneStagedStartRequiresExactOriginalAndHeldNativeProofs) {
    FrontierAudioHarness h(0);
    h.Prepare(32, false, false, true, harken::VariableControlSide::Discrete,
        false, false, false, false, UINT64_MAX, false, true, false, false, false, 16u);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    const auto* bucket = h.lane.CaptureDue(h.laneHandle.slot, 0);
    ASSERT_NE(nullptr, bucket);
    const auto* captured = h.lane.Events(h.laneHandle.slot, bucket);
    ASSERT_NE(nullptr, captured);
    ASSERT_EQ(3u, bucket->eventCount);
    ASSERT_EQ(1u, bucket->memberCount);
    const harken::ImmediateLaneEvent* start = nullptr;
    for (unsigned i = 0; i < bucket->eventCount; ++i)
        if (captured[i].futureGroupedStart) start = &captured[i];
    ASSERT_NE(nullptr, start);
    const auto& original = start->event;
    const auto* payload = &original.program.program->payloads[original.payload];
    h.audio.Begin();
    harken::TimelineArmTable::LaneAudioProof arm;
    ASSERT_TRUE(h.arms.ProveLaneFutureStart(original.armToken, *h.member.frontier, 0u,
        original.executeFrame, 1u, &arm, start->armProgramGeneration, payload->target, true));
    harken::VariableTimelineInitializerProof initializer;
    ASSERT_TRUE(h.audio.engine->voices.ProveTimelineVariableInitializer(original.armToken.renderSlot,
        original.armToken.renderGeneration, arm, &initializer));
    harken::VoiceTimelineReservation reservation;
    ASSERT_TRUE(h.audio.engine->voices.ReserveLaneTimelineEvents(original.armToken.renderSlot,
        original.armToken.renderGeneration, 0u, 1u, &reservation));
    harken::VoiceTimelineEvent event{};
    event.kind = harken::TimelineEventKind::Start;
    event.offset = 16u;
    event.order = {original.executeFrame, original.logicCommitSequence, original.executionOrdinal,
        original.eventIndex, original.transactionOrdinal, original.subeventPhase, original.rowSequence};
    event.frontierProof.state = harken::FrontierEventState::Pending;
    event.frontierProof.quantum = 0u;
    event.frontierProof.renderGeneration = original.armToken.renderGeneration;
    ASSERT_TRUE(h.audio.engine->voices.StageReservedLaneTimelineEvent(reservation, 0u, event));
    harken::VoiceTimelineReservationProof proof;
    ASSERT_TRUE(h.audio.engine->voices.ProveTimelineReservation(reservation, &proof));
    ASSERT_TRUE(h.arms.MatchesHeldLaneFutureStart(arm, *h.member.frontier, 0u, original.armToken,
        original.executeFrame, 1u, start->armProgramGeneration, payload->target, true));
    ASSERT_TRUE(h.audio.engine->voices.MatchesHeldLaneStart(original.armToken.renderSlot,
        original.armToken.renderGeneration, 0u, event, proof, arm, initializer));
    EXPECT_FALSE(h.arms.MatchesHeldLaneFutureStart(arm, *h.member.frontier, 1u, original.armToken,
        original.executeFrame, 1u, start->armProgramGeneration, payload->target, true));
    EXPECT_FALSE(h.arms.MatchesHeldLaneFutureStart(arm, *h.member.frontier, 0u, original.armToken,
        original.executeFrame + 1u, 1u, start->armProgramGeneration, payload->target, true));
    EXPECT_FALSE(h.arms.MatchesHeldLaneFutureStart(arm, *h.member.frontier, 0u, original.armToken,
        original.executeFrame, 2u, start->armProgramGeneration, payload->target, true));

    harken::VoiceTimelineReservation wrongReservation;
    ASSERT_TRUE(h.audio.engine->voices.ReserveLaneTimelineEvents(original.armToken.renderSlot,
        original.armToken.renderGeneration, 0u, 1u, &wrongReservation));
    auto wrongEvent = event;
    ++wrongEvent.offset;
    ASSERT_TRUE(h.audio.engine->voices.StageReservedLaneTimelineEvent(wrongReservation, 0u, wrongEvent));
    harken::VoiceTimelineReservationProof wrongProof;
    ASSERT_TRUE(h.audio.engine->voices.ProveTimelineReservation(wrongReservation, &wrongProof));
    EXPECT_FALSE(h.audio.engine->voices.MatchesHeldLaneStart(original.armToken.renderSlot,
        original.armToken.renderGeneration, 0u, event, wrongProof, arm, initializer));

    enum class Fault { Missing, Copied, Duplicate, DoubleOwner, WrongReservation, Complete };
    for (const auto fault : {Fault::Missing, Fault::Copied, Fault::Duplicate,
            Fault::DoubleOwner, Fault::WrongReservation, Fault::Complete}) {
        SCOPED_TRACE(static_cast<unsigned>(fault));
        harken::FrontierTimelineFinalizer finalizer(h.audio.engine->voices, h.arms);
        ASSERT_TRUE(finalizer.Attach(h.decisions, h.pool, true));
        ASSERT_TRUE(finalizer.AttachLaneEventStorage(h.lane));
        finalizer.AttachArmGroups(h.armGroups);
        finalizer.AttachVariableControls(h.variables);
        finalizer.BeginLane(0u, kBlock, h.lane, bucket, h.laneHandle.slot);
        ASSERT_TRUE(finalizer.CollectGroup(h.group));
        ASSERT_TRUE(finalizer.CollectGroup(h.member));
        for (unsigned i = 0; i < bucket->eventCount; ++i) {
            if (&captured[i] == start) continue;
            const auto& source = captured[i].event;
            ASSERT_TRUE(finalizer.CollectEvent(source, &source.program.program->payloads[source.payload],
                harken::kTimelineDispositionApplied));
        }
        if (fault == Fault::Copied) {
            const auto copy = *start;
            EXPECT_FALSE(finalizer.CollectLaneStagedStart(copy, arm, proof, initializer));
        } else if (fault != Fault::Missing) {
            ASSERT_TRUE(finalizer.CollectLaneStagedStart(*start, arm,
                fault == Fault::WrongReservation ? wrongProof : proof, initializer));
            if (fault == Fault::Duplicate) {
                EXPECT_FALSE(finalizer.CollectLaneStagedStart(*start, arm, proof, initializer));
                continue;
            }
            if (fault == Fault::DoubleOwner)
                ASSERT_TRUE(finalizer.CollectEvent(original, payload, harken::kTimelineDispositionApplied));
        }
        struct Claim {
            FrontierAudioHarness* harness;
            const harken::ImmediateLaneBucket* bucket;
            unsigned calls = 0u;
        } claim{&h, bucket};
        const bool applied = finalizer.FinishLaneEntryBucket(
            [](void* context, harken::ImmediateLaneBucketClaimReceipt* receipt) noexcept {
                auto& state = *static_cast<Claim*>(context);
                ++state.calls;
                return state.harness->lane.Claim(state.harness->laneHandle, state.bucket, receipt);
            }, &claim, &h.lane, bucket, h.laneHandle.slot, h.laneHandle.generation);
        if (fault == Fault::Complete) {
            ASSERT_TRUE(applied);
            ASSERT_EQ(1u, claim.calls);
        } else {
            EXPECT_FALSE(applied);
            EXPECT_EQ(0u, claim.calls);
            EXPECT_EQ(harken::AudioSliceState::Ready, h.group.lease->Capture().state);
            EXPECT_EQ(harken::AudioSliceState::Ready, h.member.lease->Capture().state);
        }
    }
    harken::VoicePool::ReleaseTimelineReservationProof(wrongProof);
    ASSERT_TRUE(h.audio.engine->voices.RejectTimelineReservation(wrongReservation));
    h.audio.engine->voices.ApplyTimelineVariableInitializer(initializer);
    harken::VoicePool::ApplyProvenLaneTimelineReservation(proof);
    harken::TimelineArmTable::ApplyLaneStart(arm);
    ASSERT_TRUE(h.lane.Finish(h.laneHandle.slot, bucket, true));
    h.audio.Render();
    for (unsigned frame = 0u; frame < kBlock; ++frame)
        EXPECT_NEAR(frame < 16u ? 0.0f : 0.25f, h.audio.Sample(frame), 1e-6f);
}

TEST(AuthoredStopAudio, LaneBucketClaimsAllCoordinatorsOrAbortsAllClaimedLeases) {
    for (const bool rejectBucket : {false, true}) {
        FrontierAudioHarness h(0, 3);
        h.Prepare(32, false, false, true, harken::VariableControlSide::Discrete,
            false, false, false, false, UINT64_MAX, false, true, false, true);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        harken::FrontierTimelineFinalizer finalizer(h.audio.engine->voices, h.arms);
        ASSERT_TRUE(finalizer.Attach(h.decisions, h.pool, true));
        ASSERT_TRUE(finalizer.AttachLaneEventStorage(h.lane));
        finalizer.AttachArmGroups(h.armGroups);
        finalizer.AttachVariableControls(h.variables);
        const auto* bucket = h.lane.CaptureDue(h.laneHandle.slot, 0);
        ASSERT_NE(nullptr, bucket);
        finalizer.BeginLane(0, kBlock, h.lane, bucket, h.laneHandle.slot);
        std::array<harken::Plan03PageGroupView, 4> groups{};
        ASSERT_EQ(3u, h.pool.CaptureImmediateLaneGroupsRt(0,
            groups.data(), groups.size()));
        harken::AudioSliceLease* firstLease = nullptr;
        harken::AudioSliceLease* secondLease = nullptr;
        for (const auto& group : groups) {
            if (!group.identity.Valid()) continue;
            ASSERT_TRUE(finalizer.CollectGroup(group));
            if (group.identity == h.group.identity) firstLease = group.lease;
            if (group.identity == h.secondGroup) secondLease = group.lease;
        }
        ASSERT_NE(nullptr, firstLease);
        ASSERT_NE(nullptr, secondLease);
        ASSERT_NE(firstLease, secondLease);
        const auto* events = h.lane.Events(h.laneHandle.slot, bucket);
        ASSERT_NE(nullptr, events);
        ASSERT_EQ(4u, bucket->eventCount);
        for (std::uint32_t i = 0; i < bucket->eventCount; ++i) {
            const auto& source = events[i].event;
            const auto* program = source.program.program;
            ASSERT_NE(nullptr, program);
            ASSERT_LT(source.payload, program->payloadCount);
            ASSERT_TRUE(finalizer.CollectEvent(source,
                &program->payloads[source.payload], harken::kTimelineDispositionApplied));
        }
        h.audio.Begin();
        struct Claim {
            harken::ImmediateActionLane* lane;
            harken::AudioSliceLease* first;
            harken::AudioSliceLease* second;
            const harken::ImmediateLaneBucket* bucket;
            std::uint32_t slot;
            std::uint32_t generation;
            bool reject;
            bool entered = false;
            bool bothClaimed = false;
            bool claimed = false;
        } claim{&h.lane, firstLease, secondLease, bucket,
                h.laneHandle.slot, h.laneHandle.generation, rejectBucket};
        const bool applied = finalizer.FinishLaneEntryBucket([](
            void* context, harken::ImmediateLaneBucketClaimReceipt* receipt) noexcept {
            auto& state = *static_cast<Claim*>(context);
            state.entered = true;
            state.bothClaimed =
                state.first->Capture().state == harken::AudioSliceState::SliceClaimed &&
                state.second->Capture().state == harken::AudioSliceState::SliceClaimed;
            if (state.reject) return false;
            state.claimed = state.lane->Claim(
                {state.slot, state.generation}, state.bucket, receipt);
            return state.claimed;
        }, &claim, &h.lane, bucket, h.laneHandle.slot,
            h.laneHandle.generation);
        EXPECT_TRUE(claim.entered);
        EXPECT_TRUE(claim.bothClaimed);
        EXPECT_EQ(!rejectBucket, applied);
        EXPECT_EQ(!rejectBucket, claim.claimed);
        if (claim.claimed) EXPECT_TRUE(h.lane.Finish(claim.slot, bucket, applied));
        else EXPECT_TRUE(h.lane.ReleaseCapture(claim.slot, bucket));
        const auto expectedLease = rejectBucket ? harken::AudioSliceState::SliceInvalidated :
            harken::AudioSliceState::SliceApplied;
        EXPECT_EQ(expectedLease, firstLease->Capture().state);
        EXPECT_EQ(expectedLease, secondLease->Capture().state);
        const auto expectedOutcome = rejectBucket ?
            harken::FrontierReceiptOutcome::DefinitivelyNotApplied :
            harken::FrontierReceiptOutcome::Applied;
        EXPECT_EQ(expectedOutcome, h.Receipt().outcome);
        harken::FrontierActivationReceipt second{};
        ASSERT_EQ(harken::FrontierReceiptRead::Ready,
            h.decisions.ResolveFrontierActivation(h.secondDecision)->Read(
                h.secondDescriptor.entry, 1, &second));
        EXPECT_EQ(expectedOutcome, second.outcome);
        harken::VariableControlInstanceSnapshot state{};
        ASSERT_TRUE(h.variables.Capture(h.instance, &state));
        EXPECT_EQ(rejectBucket ? harken::VariableControlSideState::Pending :
            harken::VariableControlSideState::Committed, state.discrete.state);
        h.audio.Render();
        for (std::uint32_t frame = 8; frame < 16; ++frame)
            EXPECT_NEAR(rejectBucket ? 0.0f : 0.25f, h.audio.Sample(frame), 1e-6f);
    }
}

TEST(AuthoredStopAudio, FrontierGroupedFutureUsesOwnLeaseAndKeepsEarlierEntryReceipt) {
    for (const auto workers : {2u}) {
        for (const bool empty : {false, true}) {
            for (const auto stopFrame : {8u, 16u, 32u}) {
                FrontierAudioHarness h(workers);
                h.Prepare(stopFrame, empty, false, true, harken::VariableControlSide::Discrete, true);
                ASSERT_FALSE(::testing::Test::HasFatalFailure());
                ASSERT_NE(h.group.lease, h.member.lease);
                h.executor.ExecuteQuantum(0);
                EXPECT_EQ(stopFrame == 8 ? harken::FrontierReceiptOutcome::DefinitivelyNotApplied :
                    harken::FrontierReceiptOutcome::Applied, h.Receipt().outcome);
                harken::VariableControlInstanceSnapshot state{};
                ASSERT_TRUE(h.variables.Capture(h.instance, &state));
                EXPECT_EQ(stopFrame > 16 ? harken::VariableControlSideState::Committed :
                    harken::VariableControlSideState::Pending, state.discrete.state);
                EXPECT_EQ(stopFrame > 16, state.discrete.lastApplied.Valid());
                EXPECT_EQ(stopFrame > 16 ? harken::AudioSliceState::SliceApplied :
                    harken::AudioSliceState::SliceInvalidated, h.member.lease->Capture().state);
                h.audio.Render();
                for (std::uint32_t frame = 0; frame < 32; ++frame)
                    EXPECT_NEAR(!empty && stopFrame > 16 && frame >= 16 ? 0.25f : 0.0f,
                        h.audio.Sample(frame), 1e-6f);
            }
        }
    }
}

TEST(AuthoredStopAudio, FrontierGroupedControlUsesNextSliceAfterEntryWithoutRestartingArm) {
    for (const auto workers : {2u}) {
        for (const auto side : {harken::VariableControlSide::Discrete, harken::VariableControlSide::Continuous}) {
            for (const auto stopFrame : {16u, 32u}) {
                FrontierAudioHarness h(workers);
                h.Prepare(stopFrame, false, false, true, side, false, false, false, true);
                ASSERT_FALSE(::testing::Test::HasFatalFailure());
                ASSERT_NE(h.member.lease, h.later.lease);
                h.executor.ExecuteQuantum(0);
                EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, h.Receipt().outcome);
                harken::DecisionCommittedSnapshot state{};
                ASSERT_TRUE(h.decisions.CaptureCommitted(h.memberDecision, &state));
                EXPECT_EQ(stopFrame > 16 ? 2u : 1u, state.consumedSlices);
                EXPECT_EQ(harken::AudioSliceState::SliceApplied, h.member.lease->Capture().state);
                EXPECT_EQ(stopFrame > 16 ? harken::AudioSliceState::SliceApplied :
                    harken::AudioSliceState::SliceInvalidated, h.later.lease->Capture().state);
                harken::TimelineArmObservation arm{};
                ASSERT_TRUE(h.arms.Observe(h.arm, &arm));
                EXPECT_EQ(1u, arm.startedBlockSequence);
                for (std::size_t index = 0; index < h.digest.count; ++index) {
                    const auto& record = h.digest.records[index];
                    if (record.event.executeFrame != 16 || !record.event.frontier.Present()) continue;
                    EXPECT_EQ(stopFrame > 16 ? harken::kTimelineDispositionApplied :
                        harken::kTimelineDispositionStopDominated, record.disposition);
                }
                h.audio.Render();
                for (std::uint32_t frame = 0; frame < 8; ++frame) EXPECT_EQ(0.0f, h.audio.Sample(frame));
                for (std::uint32_t frame = 8; frame < 16; ++frame) EXPECT_NEAR(0.25f, h.audio.Sample(frame), 1e-6f);
                if (stopFrame > 16) {
                    EXPECT_NEAR(0.25f * (11.0f / 15.0f), h.audio.Sample(20), 1e-6f);
                    EXPECT_NEAR(0.0f, h.audio.Sample(31), 1e-6f);
                }
            }
        }
    }
}

TEST(AuthoredStopAudio, FrontierTransitionLocksAtOriginalAudibleSliceAndCannotBypassClaim) {
    for (const auto workers : {2u}) {
        for (const auto audibleFrame : {8u, 16u}) {
            for (const auto stopFrame : {8u, 16u, 32u}) {
                FrontierAudioHarness h(workers);
                h.Prepare(stopFrame, false, false, true, harken::VariableControlSide::Discrete,
                    false, false, false, true, audibleFrame);
                ASSERT_FALSE(::testing::Test::HasFatalFailure());
                EXPECT_FALSE(h.transitions.CancelBeforePublication(h.transition));
                EXPECT_FALSE(h.transitions.CanLockFromAudioClaim(h.transition));
                h.transitions.LockFromClaimedAudioSlice(h.transition);
                harken::TransitionInstanceSnapshot state{};
                ASSERT_TRUE(h.transitions.Capture(h.transition, &state));
                EXPECT_EQ(harken::TransitionAudibleGateState::Replaceable, state.state);
                h.executor.ExecuteQuantum(0);
                ASSERT_TRUE(h.transitions.Capture(h.transition, &state));
                EXPECT_EQ(stopFrame > audibleFrame ? harken::TransitionAudibleGateState::Locked :
                    harken::TransitionAudibleGateState::Replaceable, state.state);
                EXPECT_EQ(stopFrame > 8 ? harken::FrontierReceiptOutcome::Applied :
                    harken::FrontierReceiptOutcome::DefinitivelyNotApplied, h.Receipt().outcome);
                EXPECT_FALSE(h.transitions.ReleaseFrontierBinding(h.transition));
                h.audio.Render();
                if (stopFrame > 16) {
                    EXPECT_NEAR(0.25f, h.audio.Sample(8), 1e-6f);
                    EXPECT_NEAR(0.25f * (11.0f / 15.0f), h.audio.Sample(20), 1e-6f);
                }
            }
        }
    }
}

TEST(AuthoredStopAudio, FrontierTransitionCancellationUsesOriginalLeaseBeforeAndAfterClaim) {
    for (const bool afterClaim : {false, true}) {
        FrontierAudioHarness h(0);
        h.Prepare(32, true, false, true, harken::VariableControlSide::Discrete,
            true, false, false, false, 16);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        struct Context {
            FrontierAudioHarness* harness;
            harken::TransitionInstancePool::FrontierProof proof;
            bool afterClaim;
            bool applied = false;
        } context{&h, {}, afterClaim};
        ASSERT_TRUE(h.transitions.ProveFrontier(*h.member.frontier, *h.member.lease,
            harken::TransitionAudibleGateState::Replaceable, true, &context.proof));
        harken::AudioSliceOperation operation{
            &context,
            [](void* opaque) noexcept {
                auto& state = *static_cast<Context*>(opaque);
                if (!state.afterClaim) EXPECT_FALSE(state.harness->transitions.InvalidateReady(state.harness->transition));
                return true;
            },
            [](void* opaque) noexcept {
                auto& state = *static_cast<Context*>(opaque);
                EXPECT_FALSE(state.harness->transitions.InvalidateReady(state.harness->transition));
                EXPECT_FALSE(state.harness->transitions.BeginRetire(state.harness->transition, 1));
                EXPECT_FALSE(state.harness->transitions.Reclaim(state.harness->transition, 2));
            },
            [](void* opaque) noexcept {
                auto& state = *static_cast<Context*>(opaque);
                state.harness->transitions.ApplyFrontier(state.proof);
                state.applied = true;
            }};
        (void)harken::ApplyAudioSliceWhole(*h.member.lease, &operation, 1);
        context.proof.Release();
        EXPECT_EQ(afterClaim, context.applied);
        harken::TransitionInstanceSnapshot state{};
        ASSERT_TRUE(h.transitions.Capture(h.transition, &state));
        EXPECT_EQ(afterClaim ? harken::TransitionAudibleGateState::Locked :
            harken::TransitionAudibleGateState::Replaceable, state.state);
    }
}

TEST(AuthoredStopAudio, FrontierQueueDelayedTargetKeepsCompletionAndAudibleLockOnTheirOwnSlices) {
    for (const auto stopFrame : {16u, 24u, 40u}) {
        FrontierAudioHarness h(0);
        h.Prepare(stopFrame, false, false, true, harken::VariableControlSide::Discrete,
            true, false, true, true, 24);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        h.executor.ExecuteQuantum(0);
        EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, h.Receipt().outcome);
        harken::QueueInstanceSnapshot queue{};
        ASSERT_TRUE(h.queues.Capture(h.queue, &queue));
        EXPECT_EQ(stopFrame > 16 ? harken::ConditionalQueueActivation::Activated :
            harken::ConditionalQueueActivation::Waiting, queue.pendingActivation);
        harken::TransitionInstanceSnapshot transition{};
        ASSERT_TRUE(h.transitions.Capture(h.transition, &transition));
        EXPECT_EQ(stopFrame > 24 ? harken::TransitionAudibleGateState::Locked :
            harken::TransitionAudibleGateState::Replaceable, transition.state);
        harken::DecisionCommittedSnapshot decision{};
        ASSERT_TRUE(h.decisions.CaptureCommitted(h.memberDecision, &decision));
        EXPECT_EQ(stopFrame <= 16 ? 0u : (stopFrame <= 24 ? 1u : 2u), decision.consumedSlices);
        h.audio.Render();
        for (std::uint32_t frame = 0; frame < 16; ++frame) EXPECT_NEAR(0.25f, h.audio.Sample(frame), 1e-6f);
        for (std::uint32_t frame = 16; frame < 24; ++frame) EXPECT_EQ(0.0f, h.audio.Sample(frame));
        for (std::uint32_t frame = 24; frame < 40; ++frame)
            EXPECT_NEAR(stopFrame > 24 ? 0.25f : 0.0f, h.audio.Sample(frame), 1e-6f);
    }
}

TEST(AuthoredStopAudio, FrontierQueuePreludeDoesNotAdvanceUntilActualCompletionTargetSlice) {
    for (const auto stopFrame : {8u, 16u, 40u}) {
        FrontierAudioHarness h(0);
        h.Prepare(stopFrame, false, false, true, harken::VariableControlSide::Discrete,
            false, false, true, true, 8);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        harken::CompletionProgressSnapshot before{};
        ASSERT_TRUE(harken::CaptureCompletionProgressExact(*h.occurrences.CompletionProgressOf(h.currentOccurrence),
            h.currentOccurrence, static_cast<std::uint32_t>(h.sourceRender >> 32), &before));
        h.executor.ExecuteQuantum(0);
        harken::QueueInstanceSnapshot queue{};
        ASSERT_TRUE(h.queues.Capture(h.queue, &queue));
        EXPECT_EQ(stopFrame > 16 ? harken::ConditionalQueueActivation::Activated :
            harken::ConditionalQueueActivation::Waiting, queue.pendingActivation);
        harken::TransitionInstanceSnapshot transition{};
        ASSERT_TRUE(h.transitions.Capture(h.transition, &transition));
        EXPECT_EQ(stopFrame > 8 ? harken::TransitionAudibleGateState::Locked :
            harken::TransitionAudibleGateState::Replaceable, transition.state);
        harken::DecisionCommittedSnapshot decision{};
        ASSERT_TRUE(h.decisions.CaptureCommitted(h.memberDecision, &decision));
        EXPECT_EQ(stopFrame <= 8 ? 0u : (stopFrame <= 16 ? 1u : 2u), decision.consumedSlices);
        h.audio.Render();
        ASSERT_TRUE(h.audio.engine->voices.PublishCompletionProgress(0));
        EXPECT_NEAR(0.25f, h.audio.Sample(7), 1e-6f);
        EXPECT_NEAR(stopFrame > 8 ? 0.25f * (3.0f / 7.0f) : 0.25f, h.audio.Sample(12), 1e-6f);
        for (std::uint32_t frame = 16; frame < 40; ++frame)
            EXPECT_NEAR(stopFrame > 16 ? 0.25f : 0.0f, h.audio.Sample(frame), 1e-6f);
        harken::CompletionProgressSnapshot progress{};
        ASSERT_TRUE(harken::CaptureCompletionProgressExact(*h.occurrences.CompletionProgressOf(h.currentOccurrence),
            h.currentOccurrence, static_cast<std::uint32_t>(h.sourceRender >> 32), &progress));
        EXPECT_GT(progress.completedThroughBlockSequence, before.completedThroughBlockSequence);
        EXPECT_GT(progress.progressRevision, before.progressRevision);
        EXPECT_EQ(16u, progress.logicalOutputFramesProduced);
    }
}

TEST(AuthoredStopAudio, FrontierQueueInitialSharesEntryWithoutInventingConditionalOrAdvancingMirror) {
    for (const auto workers : {2u}) {
        for (const bool empty : {false, true}) {
            for (const auto stopFrame : {8u, 32u}) {
                FrontierAudioHarness h(workers);
                h.Prepare(stopFrame, empty, false, true, harken::VariableControlSide::Discrete, false, true);
                ASSERT_FALSE(::testing::Test::HasFatalFailure());
                h.executor.ExecuteQuantum(0);
                const bool applied = stopFrame > 8;
                EXPECT_EQ(applied ? harken::FrontierReceiptOutcome::Applied :
                    harken::FrontierReceiptOutcome::DefinitivelyNotApplied, h.Receipt().outcome);
                harken::QueueInstanceSnapshot state{};
                ASSERT_TRUE(h.queues.Capture(h.queue, &state));
                EXPECT_EQ(applied ? 1u : 0u, state.initialActivationSequence);
                EXPECT_EQ(0u, state.pendingVersion);
                EXPECT_EQ(1u, state.logicMirror.version);
                EXPECT_EQ(0u, state.logicMirror.cursor);
                EXPECT_FALSE(h.queues.Release(h.queue));
                h.audio.Render();
                for (std::uint32_t frame = 0; frame < 32; ++frame)
                    EXPECT_NEAR(!empty && applied && frame >= 8 ? 0.25f : 0.0f, h.audio.Sample(frame), 1e-6f);
                EXPECT_FALSE(h.pool.DetachPlan03GroupFrontier(h.member.identity, 1));
                ASSERT_TRUE(h.pool.DetachPlan03GroupFrontier(h.member.identity, 2));
                ASSERT_TRUE(h.pool.DetachPlan03GroupFrontier(h.group.identity, 2));
                harken::FrontierActivationReceipt receiptScratch{};
                ASSERT_TRUE(h.decisions.ReconcileFrontierActivation(h.decision, h.descriptor.entry, 2, &receiptScratch));
                ASSERT_TRUE(h.decisions.ReleaseFrontierDecisionBinding(h.memberDecision));
                EXPECT_TRUE(h.queues.Release(h.queue));
            }
        }
    }
}

TEST(AuthoredStopAudio, FrontierQueueInitialClaimDefersFailureAndMirrorMutation) {
    FrontierAudioHarness h(0);
    h.Prepare(32, true, false, true, harken::VariableControlSide::Discrete, false, true);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    struct Context {
        FrontierAudioHarness* h;
        harken::QueueInstanceTable::FrontierInitialProof proof;
    } context{&h};
    auto wrong = *h.member.frontier;
    ++wrong.queueInitial.version;
    EXPECT_FALSE(h.queues.ProveFrontierInitial(wrong, *h.member.lease, 1, &context.proof));
    ASSERT_TRUE(h.queues.ProveFrontierInitial(*h.member.frontier, *h.member.lease, 1, &context.proof));
    harken::AudioSliceOperation operation{
        &context,
        [](void*) noexcept { return true; },
        [](void* opaque) noexcept {
            auto& current = *static_cast<Context*>(opaque);
            auto& h = *current.h;
            EXPECT_EQ(harken::QueueFailureDisposition::DeferredByFrontier,
                h.queues.FailContainerBeforeConditional(h.queue, harken::QueueFailureReason::TransportReset, 8));
            harken::EffectiveQueueState next{};
            next.version = 2;
            EXPECT_FALSE(h.queues.AdvanceAfterSignalCompletion(h.queue, 1, next));
            EXPECT_FALSE(h.queues.Release(h.queue));
        },
        [](void* opaque) noexcept {
            auto& current = *static_cast<Context*>(opaque);
            current.h->queues.ApplyFrontierInitial(current.proof);
        }};
    EXPECT_EQ(harken::AudioSliceApplyStatus::AppliedWithDeferredInvalidation,
        harken::ApplyAudioSliceWhole(*h.member.lease, &operation, 1));
    harken::QueueInstanceSnapshot state{};
    ASSERT_TRUE(h.queues.Capture(h.queue, &state));
    EXPECT_EQ(1u, state.initialActivationSequence);
    EXPECT_EQ(1u, state.logicMirror.version);
    EXPECT_EQ(harken::QueueFailurePhase::Healthy, state.failurePhase);
    EXPECT_EQ(harken::QueueFailureDisposition::NewlyFailed,
        h.queues.FailContainerBeforeConditional(h.queue, harken::QueueFailureReason::TransportReset, 8));
}

TEST(AuthoredStopAudio, FrontierQueueInitialFailureBeforeClaimRejectsEntireEntry) {
    FrontierAudioHarness h(0);
    h.Prepare(32, false, false, true, harken::VariableControlSide::Discrete, false, true);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    EXPECT_EQ(harken::QueueFailureDisposition::Stale,
        h.queues.FailContainerBeforeConditional({h.queue.slot, h.queue.generation + 1},
            harken::QueueFailureReason::TransportReset, 8));
    EXPECT_EQ(harken::AudioSliceState::Ready, h.group.lease->Capture().state);
    EXPECT_EQ(harken::QueueFailureDisposition::NewlyFailed,
        h.queues.FailContainerBeforeConditional(h.queue, harken::QueueFailureReason::TransportReset, 8));
    h.executor.ExecuteQuantum(0);
    EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied, h.Receipt().outcome);
    harken::QueueInstanceSnapshot state{};
    ASSERT_TRUE(h.queues.Capture(h.queue, &state));
    EXPECT_EQ(0u, state.initialActivationSequence);
    EXPECT_EQ(1u, state.logicMirror.version);
    h.audio.Render();
    for (std::uint32_t frame = 0; frame < kBlock; ++frame) EXPECT_EQ(0.0f, h.audio.Sample(frame));
}

TEST(AuthoredStopAudio, FrontierQueueConditionalUsesActualCompletionAndOwnGateAfterEntry) {
    {
        constexpr std::uint32_t workers = 0;
        for (const bool empty : {false, true}) {
            for (const auto stopFrame : {16u, 32u}) {
                FrontierAudioHarness h(workers);
                h.Prepare(stopFrame, empty, false, true, harken::VariableControlSide::Discrete, true, false, true);
                ASSERT_FALSE(::testing::Test::HasFatalFailure());
                harken::CompletionProgressSnapshot before{};
                ASSERT_TRUE(harken::CaptureCompletionProgressExact(*h.occurrences.CompletionProgressOf(h.currentOccurrence),
                    h.currentOccurrence, static_cast<std::uint32_t>(h.sourceRender >> 32), &before));
                h.executor.ExecuteQuantum(0);
                EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, h.Receipt().outcome);
                harken::QueueInstanceSnapshot queue{};
                ASSERT_TRUE(h.queues.Capture(h.queue, &queue));
                EXPECT_EQ(stopFrame > 16 ? harken::ConditionalQueueActivation::Activated :
                    harken::ConditionalQueueActivation::Waiting, queue.pendingActivation);
                EXPECT_EQ(1u, queue.logicMirror.version);
                EXPECT_EQ(2u, queue.pendingVersion);
                h.audio.Render();
                ASSERT_TRUE(h.audio.engine->voices.PublishCompletionProgress(0));
                harken::CompletionProgressSnapshot after{};
                ASSERT_TRUE(harken::CaptureCompletionProgressExact(*h.occurrences.CompletionProgressOf(h.currentOccurrence),
                    h.currentOccurrence, static_cast<std::uint32_t>(h.sourceRender >> 32), &after));
                EXPECT_GT(after.completedThroughBlockSequence, before.completedThroughBlockSequence);
                EXPECT_GT(after.progressRevision, before.progressRevision);
                EXPECT_EQ(16u, after.logicalOutputFramesProduced);
                for (std::uint32_t frame = 0; frame < 16; ++frame) EXPECT_NEAR(0.25f, h.audio.Sample(frame), 1e-6f);
                for (std::uint32_t frame = 16; frame < 32; ++frame)
                    EXPECT_NEAR(!empty && stopFrame > 16 ? 0.25f : 0.0f, h.audio.Sample(frame), 1e-6f);
            }
        }
    }
}

TEST(AuthoredStopAudio, FrontierQueueConditionalRejectsMissingCompletionOwnerDespitePositiveEntry) {
    FrontierAudioHarness h(0);
    h.Prepare(32, false, false, true, harken::VariableControlSide::Discrete, true, false, true);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    ASSERT_TRUE(h.occurrences.ClearCompletionOwnerExact(h.currentOccurrence,
        harken::CompletionOwnerRef::Single(h.memberDecision, 1)));
    h.executor.ExecuteQuantum(0);
    EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, h.Receipt().outcome);
    harken::QueueInstanceSnapshot state{};
    ASSERT_TRUE(h.queues.Capture(h.queue, &state));
    EXPECT_EQ(harken::ConditionalQueueActivation::Waiting, state.pendingActivation);
    EXPECT_EQ(1u, state.logicMirror.version);
    EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, h.member.lease->Capture().state);
    h.audio.Render();
    for (std::uint32_t frame = 16; frame < 32; ++frame) EXPECT_EQ(0.0f, h.audio.Sample(frame));
}

TEST(AuthoredStopAudio, FrontierQueueConditionalClaimExcludesLegacyActivationCancelAndRetirement) {
    FrontierAudioHarness h(0);
    h.Prepare(32, false, false, true, harken::VariableControlSide::Discrete, true, false, true);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    struct Context {
        FrontierAudioHarness* h;
        harken::QueueInstanceTable::FrontierConditionalProof proof;
    } context{&h};
    ASSERT_TRUE(h.queues.ProveFrontierConditional(*h.member.frontier, *h.member.lease, h.occurrences,
        nullptr, 0, kBlock, 0, &context.proof));
    EXPECT_FALSE(h.queues.ActivateConditional(h.queue, h.conditionalVersion, h.memberDecision, h.currentOccurrence));
    const harken::AudioSliceOperation operation{
        &context, [](void*) noexcept { return true; },
        [](void* opaque) noexcept {
            auto& h = *static_cast<Context*>(opaque)->h;
            EXPECT_EQ(harken::QueueFailureDisposition::DeferredByFrontier,
                h.queues.FailContainer(h.queue, h.conditionalVersion, harken::ConditionalQueueActivation::InvalidatedTransport,
                    harken::QueueFailureReason::TransportReset, 16));
            EXPECT_FALSE(h.queues.RetireConditionalForAbort(h.queue, h.conditionalVersion, h.memberDecision));
            EXPECT_FALSE(h.queues.Release(h.queue));
        },
        [](void* opaque) noexcept {
            auto& current = *static_cast<Context*>(opaque);
            current.h->queues.ApplyFrontierConditional(current.proof);
            EXPECT_FALSE(current.h->queues.SettleActivated(current.h->queue, current.h->conditionalVersion));
        }};
    EXPECT_EQ(harken::AudioSliceApplyStatus::AppliedWithDeferredInvalidation,
        harken::ApplyAudioSliceWhole(*h.member.lease, &operation, 1));
    context.proof.Release();
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(h.queues.Capture(h.queue, &snapshot));
    EXPECT_EQ(harken::ConditionalQueueActivation::Activated, snapshot.pendingActivation);
    EXPECT_EQ(1u, snapshot.logicMirror.version);
    EXPECT_TRUE(h.queues.SettleActivated(h.queue, h.conditionalVersion));
    ASSERT_TRUE(h.queues.Capture(h.queue, &snapshot));
    EXPECT_EQ(2u, snapshot.logicMirror.version);
}

TEST(AuthoredStopAudio, FrontierQueueConditionalCancellationBeforeClaimCannotAdvance) {
    FrontierAudioHarness h(0);
    h.Prepare(32, false, false, true, harken::VariableControlSide::Discrete, true, false, true);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    harken::QueueInstanceTable::FrontierConditionalProof proof;
    ASSERT_TRUE(h.queues.ProveFrontierConditional(*h.member.frontier, *h.member.lease, h.occurrences,
        nullptr, 0, kBlock, 0, &proof));
    EXPECT_EQ(harken::QueueFailureDisposition::NewlyFailed,
        h.queues.FailContainer(h.queue, h.conditionalVersion, harken::ConditionalQueueActivation::InvalidatedTransport,
            harken::QueueFailureReason::TransportReset, 16));
    EXPECT_FALSE(h.member.lease->TryClaim());
    proof.Release();
    harken::QueueInstanceSnapshot snapshot{};
    ASSERT_TRUE(h.queues.Capture(h.queue, &snapshot));
    EXPECT_EQ(harken::ConditionalQueueActivation::InvalidatedTransport, snapshot.pendingActivation);
    EXPECT_EQ(1u, snapshot.logicMirror.version);
    EXPECT_TRUE(h.queues.RetireConditionalForAbort(h.queue, h.conditionalVersion, h.memberDecision));
}

TEST(AuthoredStopAudio, FrontierFinalizedSpansSortOriginalFullKeysWithoutLosingGuards) {
    StopAudioHarness h(0);
    const auto render = h.Play();
    h.Begin();
    auto earlier = WithFrontierProof(StartAt(8), render, 0, harken::FrontierEventState::Applied);
    auto later = earlier;
    earlier.order.rowSequence = 3;
    later.order.rowSequence = 9;
    later.frontierProof.state = harken::FrontierEventState::Rejected;
    h.Append(render, later);
    h.Append(render, StopAt(32));
    h.Append(render, earlier);
    h.engine->voices.FinalizeTimelineQuantum(0);
    const auto& span = h.engine->voices.Resolve(render)->timelineEvents;
    ASSERT_EQ(3u, span.eventCount);
    EXPECT_EQ(3u, span.events[0].order.rowSequence);
    EXPECT_EQ(harken::FrontierEventState::Applied, span.events[0].frontierProof.state);
    EXPECT_EQ(9u, span.events[1].order.rowSequence);
    EXPECT_EQ(harken::FrontierEventState::Rejected, span.events[1].frontierProof.state);
    EXPECT_EQ(32u, span.events[2].offset);
    h.Render();
    for (std::uint32_t i = 8; i < 32; ++i) EXPECT_NEAR(0.25f, h.Sample(i), 1e-6f);
}

TEST(AuthoredStopAudio, FrontierReservationsAggregateWithOtherTransactionsAndLegacyEvents) {
    StopAudioHarness h(0);
    const auto render = h.Play();
    h.Begin();
    auto& voices = h.engine->voices;
    const auto slot = static_cast<std::uint32_t>(render);
    const auto generation = static_cast<std::uint32_t>(render >> 32);
    harken::VoiceTimelineReservation first{}, second{}, overflow{};
    ASSERT_TRUE(voices.ReserveTimelineEvents(slot, generation, h.quantum, 3, &first));
    h.Append(render, StopAt(56));
    ASSERT_TRUE(voices.ReserveTimelineEvents(slot, generation, h.quantum, 4, &second));
    EXPECT_FALSE(voices.ReserveTimelineEvents(slot, generation, h.quantum, 1, &overflow));
    EXPECT_FALSE(voices.AppendTimelineEvent(slot, generation, h.quantum, StartAt(0)));
    EXPECT_EQ(8u, voices.Resolve(render)->timelineEvents.eventCount);
    EXPECT_EQ(0u, first.first);
    EXPECT_EQ(4u, second.first);
    EXPECT_TRUE(voices.RejectTimelineReservation(first));
    EXPECT_TRUE(voices.RejectTimelineReservation(second));
    h.Render();
    for (std::uint32_t i = 0; i < kBlock; ++i) EXPECT_EQ(0.0f, h.Sample(i));
}

TEST(AuthoredStopAudio, FrontierReservationMustBeCompleteAndExactBeforeNonFailingApply) {
    for (const auto workers : {2u}) {
        StopAudioHarness h(workers);
        const auto render = h.Play();
        h.Begin();
        auto& voices = h.engine->voices;
        const auto slot = static_cast<std::uint32_t>(render);
        const auto generation = static_cast<std::uint32_t>(render >> 32);
        harken::VoiceTimelineReservation reservation{};
        ASSERT_TRUE(voices.ReserveTimelineEvents(slot, generation, h.quantum, 2, &reservation));
        harken::VoiceTimelineReservationProof proof{};
        EXPECT_FALSE(voices.ProveTimelineReservation(reservation, &proof));
        auto cursor = StartAt(8);
        cursor.kind = harken::TimelineEventKind::SetStartCursor;
        cursor.value = 128;
        cursor = WithFrontierProof(cursor, render, h.quantum, harken::FrontierEventState::Pending);
        ASSERT_TRUE(voices.StageReservedTimelineEvent(reservation, 0, cursor));
        EXPECT_FALSE(voices.StageReservedTimelineEvent(reservation, 0, cursor));
        EXPECT_FALSE(voices.ProveTimelineReservation(reservation, &proof));
        auto start = WithFrontierProof(StartAt(8), render, h.quantum, harken::FrontierEventState::Pending);
        auto stale = reservation;
        ++stale.renderGeneration;
        EXPECT_FALSE(voices.StageReservedTimelineEvent(stale, 1, start));
        stale = reservation;
        ++stale.quantum;
        EXPECT_FALSE(voices.StageReservedTimelineEvent(stale, 1, start));
        ASSERT_TRUE(voices.StageReservedTimelineEvent(reservation, 1, start));
        ASSERT_TRUE(voices.ProveTimelineReservation(reservation, &proof));
        EXPECT_FALSE(voices.CancelTimelineBinding(slot, generation));
        EXPECT_FALSE(voices.RejectTimelineReservation(reservation));
        harken::VoiceTimelineReservationProof duplicate{};
        EXPECT_FALSE(voices.ProveTimelineReservation(reservation, &duplicate));
        h.Append(render, StopAt(32));
        voices.ApplyProvenTimelineReservation(proof, 32);
        EXPECT_FALSE(voices.RejectTimelineReservation(reservation));
        h.Render();
        const auto* context = voices.Resolve(render);
        ASSERT_NE(nullptr, context);
        EXPECT_GE(context->srcCursor, 128.0);
        for (std::uint32_t i = 0; i < 8; ++i) EXPECT_EQ(0.0f, h.Sample(i));
        for (std::uint32_t i = 8; i < 32; ++i) EXPECT_NEAR(0.25f, h.Sample(i), 1e-6f);
    }
}

TEST(AuthoredStopAudio, LaneReservationRemainsInvisibleUntilWholeProofIsApplied) {
    StopAudioHarness h(2);
    const auto render = h.Play();
    h.Begin();
    auto& voices = h.engine->voices;
    const auto slot = static_cast<std::uint32_t>(render);
    const auto generation = static_cast<std::uint32_t>(render >> 32);
    harken::VoiceTimelineReservation reservation{};
    ASSERT_TRUE(voices.ReserveLaneTimelineEvents(
        slot, generation, h.quantum, 1, &reservation));
    auto event = StartAt(8);
    event.frontierProof.state = harken::FrontierEventState::Pending;
    event.frontierProof.quantum = h.quantum;
    event.frontierProof.renderGeneration = generation;
    ASSERT_TRUE(voices.StageReservedLaneTimelineEvent(reservation, 0, event));
    harken::VoiceTimelineReservationProof proof{};
    ASSERT_TRUE(voices.ProveTimelineReservation(reservation, &proof));
    const auto* context = voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_FALSE(context->timelineEvents.events[0].frontierProof.Eligible(
        h.quantum, generation));
    EXPECT_FALSE(voices.CancelTimelineBinding(slot, generation));
    voices.ApplyProvenLaneTimelineReservation(proof);
    EXPECT_TRUE(context->timelineEvents.events[0].frontierProof.Eligible(
        h.quantum, generation));
    EXPECT_FALSE(context->timelineEvents.events[0].frontierProof.Eligible(
        h.quantum + 1u, generation));
    h.Render();
    for (std::uint32_t i = 0; i < 8; ++i) EXPECT_EQ(0.0f, h.Sample(i));
    for (std::uint32_t i = 8; i < kBlock; ++i)
        EXPECT_NEAR(0.25f, h.Sample(i), 1e-6f);
}

TEST(AuthoredStopAudio, PageAndLaneHaveIndependentVoiceEventCapacity) {
    for (const bool laneFirst : {false, true}) {
        StopAudioHarness h(0);
        const auto render = h.Play();
        h.Begin();
        auto& voices = h.engine->voices;
        const auto slot = static_cast<std::uint32_t>(render);
        const auto generation = static_cast<std::uint32_t>(render >> 32);
        harken::VoiceTimelineReservation page{}, lane{}, overflow{};
        if (laneFirst) {
            ASSERT_TRUE(voices.ReserveLaneTimelineEvents(slot, generation, 0, 8, &lane));
            ASSERT_TRUE(voices.ReserveTimelineEvents(slot, generation, 0, 8, &page));
        } else {
            ASSERT_TRUE(voices.ReserveTimelineEvents(slot, generation, 0, 8, &page));
            ASSERT_TRUE(voices.ReserveLaneTimelineEvents(slot, generation, 0, 8, &lane));
        }
        EXPECT_FALSE(voices.ReserveTimelineEvents(slot, generation, 0, 1, &overflow));
        EXPECT_FALSE(voices.ReserveLaneTimelineEvents(slot, generation, 0, 1, &overflow));
        EXPECT_FALSE(voices.AppendTimelineEvent(slot, generation, 0, StartAt(0)));
        ASSERT_TRUE(voices.RejectTimelineReservation(page));
        ASSERT_TRUE(voices.RejectTimelineReservation(lane));
        EXPECT_EQ(16u, voices.Resolve(render)->timelineEvents.eventCount);
        voices.FinalizeTimelineQuantum(0);
        h.Render();
        for (std::uint32_t i = 0; i < kBlock; ++i) EXPECT_EQ(0.0f, h.Sample(i));
    }
}

TEST(AuthoredStopAudio, LaneBucketRequiresEveryArmBeforeAnyVoiceStarts) {
    for (const bool invalidateSecond : {false, true}) {
        StopAudioHarness h(0);
        const VnVoiceHandle renders[] = {h.Play(), h.Play()};
        harken::TimelineArmTable arms(2);
        harken::TimelineArmToken tokens[2]{};
        harken::TimelineEventPayload payloads[2]{};
        harken::TimelineProgramView programs[2]{};
        harken::ImmediateLaneEvent events[2]{};
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({1, 2, 2, kBlock, true}));
        for (std::uint32_t i = 0; i < 2; ++i) {
            const auto frame = 8u + 8u * i;
            ASSERT_TRUE(arms.PreparePendingImmediate(static_cast<std::uint32_t>(renders[i]),
                static_cast<std::uint32_t>(renders[i] >> 32), 1, {}, &tokens[i]));
            ASSERT_TRUE(arms.CommitPrepared(tokens[i]));
            ASSERT_TRUE(arms.AcknowledgeApplied(tokens[i], 1));
            ASSERT_TRUE(arms.BindPendingImmediateStart(tokens[i], frame, frame));
            programs[i] = {11, &payloads[i], 1};
            events[i].relativeFrame = frame;
            events[i].event.program = {11, &programs[i]};
            events[i].event.armToken = tokens[i];
            events[i].event.subeventPhase = 20;
        }
        harken::ImmediateLaneHandle handle{};
        ASSERT_TRUE(lane.Reserve(1, 2, &handle));
        ASSERT_TRUE(lane.Prepare(handle, events, 2));
        ASSERT_TRUE(lane.PinArms(handle, arms));
        std::uint64_t anchor = UINT64_MAX;
        ASSERT_TRUE(lane.TryPlace(handle, 0, 0, &anchor));
        ASSERT_TRUE(arms.Publish(tokens[0]));
        ASSERT_TRUE(arms.Publish(tokens[1]));
        ASSERT_TRUE(lane.Publish(handle));
        if (invalidateSecond) ASSERT_TRUE(arms.RequestCancel(tokens[1]));
        harken::VoiceTimelineSink sink(h.engine->voices, arms);
        ASSERT_TRUE(sink.AttachImmediateLane(lane, kBlock));
        sink.SetArenaAdmission(UINT32_MAX, kBlock);
        ASSERT_TRUE(sink.BeginQuantum(0));
        sink.EndQuantum(0);
        harken::ImmediateLaneObservation observation{};
        ASSERT_TRUE(lane.Observe(handle, &observation));
        EXPECT_TRUE(observation.complete);
        EXPECT_EQ(invalidateSecond, observation.missed);
        EXPECT_EQ(!invalidateSecond, observation.applied);
        h.Render();
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            const float expected = invalidateSecond || i < 8 ? 0.0f : i < 16 ? 0.25f : 0.5f;
            EXPECT_NEAR(expected, h.Sample(i), 1e-6f);
        }
        if (!invalidateSecond) EXPECT_FALSE(lane.Retire(handle, 1));
        h.Begin();
        h.Render();
        EXPECT_TRUE(lane.Retire(handle, 2));
    }
}

TEST(AuthoredStopAudio, LaneOnlyQuantumUsesCapturedWorkAndCannotBypassAdmission) {
    for (const bool publishAfterCapture : {false, true}) {
        StopAudioHarness h(0);
        const auto render = h.Play();
        harken::TimelineArmTable arms(1);
        harken::TimelineArmToken arm{};
        ASSERT_TRUE(arms.PreparePendingImmediate(static_cast<std::uint32_t>(render),
            static_cast<std::uint32_t>(render >> 32), 1, {}, &arm));
        ASSERT_TRUE(arms.CommitPrepared(arm));
        ASSERT_TRUE(arms.AcknowledgeApplied(arm, 1));
        ASSERT_TRUE(arms.BindPendingImmediateStart(arm, 8, 8));
        auto* voice = h.engine->voices.Resolve(render);
        ASSERT_NE(nullptr, voice);
        voice->timelineArmSlot = arm.slot;
        voice->timelineArmGeneration = arm.generation;
        const harken::TimelineEventPayload payload{};
        const harken::TimelineProgramView program{1, &payload, 1};
        harken::ImmediateLaneEvent event{};
        event.relativeFrame = 8;
        event.event.program = {1, &program};
        event.event.armToken = arm;
        event.event.subeventPhase = 20;
        harken::ImmediateActionLane lane;
        ASSERT_TRUE(lane.Reset({1, 1, 1, kBlock, true}));
        harken::ImmediateLaneHandle handle{};
        ASSERT_TRUE(lane.Reserve(1, 1, &handle));
        ASSERT_TRUE(lane.Prepare(handle, &event, 1));
        ASSERT_TRUE(lane.PinArms(handle, arms));
        std::uint64_t anchor = 0;
        ASSERT_TRUE(lane.TryPlace(handle, 0, 0, &anchor));
        ASSERT_TRUE(arms.Publish(arm));
        harken::VoiceTimelineSink sink(h.engine->voices, arms);
        ASSERT_TRUE(sink.AttachImmediateLane(lane, kBlock));
        if (publishAfterCapture) EXPECT_FALSE(sink.CaptureIndependentWork(0));
        ASSERT_TRUE(lane.Publish(handle));
        sink.SetArenaAdmission(publishAfterCapture ? UINT32_MAX : 0, kBlock);
        harken::TimelineExecutor executor;
        ASSERT_TRUE(executor.Reset(kBlock, 1, &h.engine->timelinePool, &arms, &sink));
        harken::TimelineExecutionPlan emptyPages{};
        emptyPages.expectedFrontier = h.engine->timelinePool.ConsumerFrontier().load();
        (void)executor.ExecuteQuantum(0, emptyPages);
        harken::ImmediateLaneObservation observation{};
        ASSERT_TRUE(lane.Observe(handle, &observation));
        EXPECT_FALSE(observation.applied);
        EXPECT_FALSE(observation.claimed);
        EXPECT_EQ(publishAfterCapture ? 0u : 1u, executor.Stats().renderUnavailableDeferrals);
        if (!publishAfterCapture) {
            sink.SetArenaAdmission(UINT32_MAX, kBlock);
            (void)executor.ExecuteQuantum(0, emptyPages);
            ASSERT_TRUE(lane.Observe(handle, &observation));
            EXPECT_TRUE(observation.applied);
        } else {
            (void)executor.ExecuteQuantum(1, emptyPages);
            ASSERT_TRUE(lane.Observe(handle, &observation));
            EXPECT_TRUE(observation.missed);
            EXPECT_FALSE(observation.applied);
        }
    }
}

TEST(AuthoredStopAudio, FrontierReservationProofReleaseAndQuantumReuseCannotExecuteOldSlots) {
    StopAudioHarness h(0);
    const auto render = h.Play();
    h.Begin();
    auto& voices = h.engine->voices;
    const auto slot = static_cast<std::uint32_t>(render);
    const auto generation = static_cast<std::uint32_t>(render >> 32);
    harken::VoiceTimelineReservation reservation{};
    ASSERT_TRUE(voices.ReserveTimelineEvents(slot, generation, h.quantum, 1, &reservation));
    const auto event = WithFrontierProof(StartAt(8), render, h.quantum, harken::FrontierEventState::Pending);
    ASSERT_TRUE(voices.StageReservedTimelineEvent(reservation, 0, event));
    {
        harken::VoiceTimelineReservationProof proof{};
        ASSERT_TRUE(voices.ProveTimelineReservation(reservation, &proof));
        harken::VoiceTimelineReservationProof moved(std::move(proof));
    }
    EXPECT_TRUE(voices.RejectTimelineReservation(reservation));
    h.Begin();
    harken::VoiceTimelineReservation replacement{};
    ASSERT_TRUE(voices.ReserveTimelineEvents(slot, generation, h.quantum, 1, &replacement));
    EXPECT_FALSE(voices.StageReservedTimelineEvent(reservation, 0, event));
    EXPECT_FALSE(voices.RejectTimelineReservation(reservation));
    EXPECT_TRUE(voices.StageReservedTimelineEvent(replacement, 0, event));
    h.Render();
    for (std::uint32_t i = 0; i < kBlock; ++i) EXPECT_EQ(0.0f, h.Sample(i));
}

TEST(AuthoredStopAudio, RejectedAndUnfinalizedFrontierEventsHaveNoCascadeEffects) {
    static_assert(std::is_trivially_copyable_v<harken::FrozenFrontierEventProof>);
    for (const auto workers : {2u}) {
        for (const auto state : {harken::FrontierEventState::Pending, harken::FrontierEventState::Rejected}) {
            StopAudioHarness h(workers);
            const auto render = h.Play();
            h.Begin();
            auto cursor = StartAt(8);
            cursor.kind = harken::TimelineEventKind::SetStartCursor;
            cursor.value = 1024;
            h.Append(render, WithFrontierProof(cursor, render, h.quantum, state));
            h.Append(render, WithFrontierProof(StartAt(8), render, h.quantum, state));
            h.Append(render, WithFrontierProof(StopAt(4), render, h.quantum, state));
            h.Append(render, StartAt(16));
            h.Append(render, StopAt(32));
            h.Render();
            const auto* context = h.engine->voices.Resolve(render);
            ASSERT_NE(nullptr, context);
            EXPECT_LT(context->srcCursor, 1024.0);
            EXPECT_EQ(32u, context->authoredRootStopFrame);
            for (std::uint32_t i = 0; i < 16; ++i) EXPECT_EQ(0.0f, h.Sample(i));
            for (std::uint32_t i = 16; i < 32; ++i) EXPECT_NEAR(0.25f, h.Sample(i), 1e-6f);
        }
    }
}

TEST(AuthoredStopAudio, FrozenFrontierProofKeepsExactQuantumGenerationAndStopFact) {
    for (const auto workers : {2u}) {
        StopAudioHarness h(workers);
        const auto render = h.Play();
        h.Begin();
        auto stale = WithFrontierProof(StartAt(4), render, h.quantum, harken::FrontierEventState::Applied);
        ++stale.frontierProof.renderGeneration;
        h.Append(render, stale);
        stale = WithFrontierProof(StartAt(6), render, h.quantum + 1u, harken::FrontierEventState::Applied);
        h.Append(render, stale);
        auto start = WithFrontierProof(StartAt(8), render, h.quantum, harken::FrontierEventState::Applied);
        start.frontierProof.cutoffFrame = 32;
        h.Append(render, start);
        auto late = WithFrontierProof(StartAt(32), render, h.quantum, harken::FrontierEventState::Applied);
        late.kind = harken::TimelineEventKind::SetStartCursor;
        late.value = 1024;
        late.frontierProof.cutoffFrame = 32;
        h.Append(render, late);
        auto stop = StopAt(32);
        stop.authoredStopOperation = {2, 3, 1, 1, 4, 0, 32};
        h.Append(render, stop);
        const auto* context = h.engine->voices.Resolve(render);
        ASSERT_NE(nullptr, context);
        const auto cutoff = harken::SelectAuthoredRootStop(context->timelineEvents, kBlock, UINT64_MAX);
        EXPECT_EQ(stop.authoredStopOperation, cutoff.operation);
        h.Render();
        context = h.engine->voices.Resolve(render);
        ASSERT_NE(nullptr, context);
        EXPECT_LT(context->srcCursor, 1024.0);
        for (std::uint32_t i = 0; i < 8; ++i) EXPECT_EQ(0.0f, h.Sample(i));
        for (std::uint32_t i = 8; i < 32; ++i) EXPECT_NEAR(0.25f, h.Sample(i), 1e-6f);
        EXPECT_EQ(stop.authoredStopOperation, context->authoredRootStopControl.authoredStopOperation);
    }
}

harken::ScheduledTransactionPool::Config FenceConfig(std::uint32_t slots = 8) {
    harken::ScheduledTransactionPool::Config config{};
    config.quantumFrames = kBlock;
    config.scheduleHorizonFrames = 512;
    config.publishLeadQuanta = 1;
    config.maxScheduledTransactions = slots;
    config.maxTransactionSpanFrames = 512;
    config.maxIngressTransactionsPerQuantum = slots;
    config.maxFrameBatchesPerQuantum = 16;
    config.maxEventsPerQuantum = 32;
    config.maxTransactionEvents = 16;
    config.maxCurrentSlicesPerQuantum = 4;
    config.maxPlan03GroupHeadersPerPage = 4;
    config.maxArmsPerGroup = 2;
    return config;
}

struct FenceRecord {
    FenceRecord(const harken::AuthoredStopOperation& op, harken::DecisionToken decision = {},
        harken::DecisionFuturePin pin = {})
        : descriptor{&op, op.Identity(), decision, &program, 1, std::move(pin)} {
        payload.kind = harken::TimelineEventKind::Stop;
        payload.reserved = harken::kTimelineEventAuthoredRootStop;
        payload.envelopeEndFrame = op.Identity().stopFrame;
        program = {1, &payload, 1, harken::TimelineProgramRole::AuthoredStopFence, &descriptor};
        event.executeFrame = op.Identity().stopFrame;
        event.subeventPhase = 50;
        request.transportEpoch = 1;
        request.program = {1, &program};
        request.ownerInstanceHandle = op.Identity().root;
        request.ownerInstanceScheduleEpoch = op.Identity().sourceEpoch;
        request.events = &event;
        request.eventCount = 1;
    }
    harken::TimelineEventPayload payload{};
    harken::TimelineProgramView program{};
    harken::AuthoredStopFenceDescriptor descriptor;
    harken::ScheduledEventInput event{};
    harken::TransactionRequest request{};
};

struct StopFenceHarness {
    explicit StopFenceHarness(std::uint32_t workers = 0, std::uint32_t slots = 8,
        std::uint32_t decisionCapacity = 8)
        : audio(workers), decisions(decisionCapacity, 4), sink(audio.engine->voices, arms) {
        EXPECT_TRUE(pool.Reset(FenceConfig(slots), 0, 1));
        EXPECT_TRUE(sink.AttachPlan03ScratchAuthority(decisions, pool));
        EXPECT_TRUE(sink.AttachQueueAuthority(queues, groups, occurrences));
        sink.SetArenaAdmission(65536, kBlock);
        EXPECT_TRUE(executor.Reset(kBlock, 32, &pool, &arms, &sink, &decisions));
    }
    harken::DecisionToken Decision(std::uint16_t slices = 1) {
        harken::DecisionReserveRequest request{};
        request.ticket.sequence = 1;
        request.world = 1;
        request.transport = 1;
        request.expectedContinuationGeneration = 1;
        request.sliceCount = slices;
        harken::DecisionToken token{};
        EXPECT_TRUE(decisions.Reserve(request, &token));
        harken::DecisionCommitPayload payload{};
        payload.pages = {1, 1};
        payload.armGroup = {1, 1};
        EXPECT_TRUE(decisions.Commit(token, payload));
        return token;
    }
    void Submit(const harken::TransactionRequest& request) {
        harken::PreparedTransactionBatch batch;
        EXPECT_EQ(harken::ReserveRefusal::None,
            pool.PrepareTransactionBatch(&request, 1, 0, &batch));
        EXPECT_TRUE(pool.CommitPreparedTransactionBatch(&batch, 1));
    }
    void Healthy() {
        EXPECT_EQ(0u, executor.Stats().armFailedTransactions);
        EXPECT_EQ(0u, executor.Stats().protocolViolation);
        EXPECT_EQ(0u, executor.Stats().staleSkippedTransactions);
    }
    harken::DecisionFuturePin Pin(harken::DecisionToken token) {
        harken::DecisionFuturePin pin;
        EXPECT_TRUE(decisions.AcquireFuturePin(token, &pin));
        return pin;
    }
    StopAudioHarness audio;
    harken::ScheduledTransactionPool pool;
    harken::TimelineArmTable arms{4};
    harken::DecisionTable decisions;
    harken::ArmGroupTable groups{4, 2};
    harken::QueueInstanceTable queues{4};
    harken::OccurrenceStatePool occurrences{4};
    harken::VoiceTimelineSink sink;
    harken::TimelineExecutor executor;
};

TEST(AuthoredStopAudio, BindinglessRootAndEmptyDecisionApplyOnlyAtDeadline) {
    StopFenceHarness h;
    const auto decision = h.Decision();
    harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 81});
    FenceRecord root(op);
    FenceRecord group(op, decision, h.Pin(decision));
    h.Submit(root.request);
    h.Submit(group.request);
    h.pool.Publish(256);
    h.executor.ExecuteQuantum(0);
    EXPECT_EQ(0u, op.AppliedAudioSequence());
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(h.decisions.CaptureCommitted(decision, &snapshot));
    EXPECT_FALSE(snapshot.authoredStopInvalidated);
    const auto allocations = harken::memguard::HitCount();
#ifdef _DEBUG
    EXPECT_TRUE(harken::memguard::IsArmed());
#endif
    {
        harken::memguard::AudioThreadScope audioScope;
        h.executor.ExecuteQuantum(1);
    }
    EXPECT_EQ(allocations, harken::memguard::HitCount());
    EXPECT_EQ(2u, op.AppliedAudioSequence());
    ASSERT_TRUE(h.decisions.CaptureCommitted(decision, &snapshot));
    EXPECT_TRUE(snapshot.authoredStopInvalidated);
    EXPECT_EQ(harken::AudioGroupState::Invalidated, snapshot.audioState);
    EXPECT_EQ(0u, h.audio.engine->mixer.BlocksRendered());
    h.Healthy();
}

TEST(AuthoredStopAudio, FenceValidationRejectsStaleIdentitiesAndNormalBindinglessPrograms) {
    for (const auto variant : {0, 1, 2, 3, 4, 5, 6}) {
        SCOPED_TRACE(variant);
        StopFenceHarness h;
        harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 17});
        FenceRecord record(op);
        if (variant == 0) op.Cancel();
        if (variant == 1) record.program.generation = 2;
        if (variant == 2) record.request.ownerInstanceHandle = 18;
        if (variant == 3) record.request.ownerInstanceScheduleEpoch = 20;
        if (variant == 4) record.event.executeFrame = 18;
        if (variant == 5) record.payload.envelopeEndFrame = 18;
        if (variant == 6) record.program.role = harken::TimelineProgramRole::Voice;
        h.Submit(record.request);
        h.pool.Publish(256);
        h.executor.ExecuteQuantum(0);
        EXPECT_EQ(0u, op.AppliedAudioSequence());
        if (variant != 6) EXPECT_EQ(1u, h.executor.Stats().staleSkippedTransactions);
        else EXPECT_GT(h.executor.Stats().armFailedTransactions, 0u);
    }
}

TEST(AuthoredStopAudio, FenceScratchDeduplicatesExactIdentitiesAndHonorsCancellation) {
    StopFenceHarness h(0, 2);
    const auto first = h.Decision();
    const auto second = h.Decision();
    harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 17});
    FenceRecord a(op, first, h.Pin(first));
    FenceRecord duplicate(op, first, h.Pin(first));
    FenceRecord b(op, second, h.Pin(second));
    FenceRecord root(op);
    ASSERT_TRUE(h.sink.BeginQuantum(0));
    ASSERT_TRUE(h.sink.ApplyAuthoredStopFence(a.descriptor, 0));
    for (int i = 0; i < 16; ++i)
        ASSERT_TRUE(h.sink.ApplyAuthoredStopFence(duplicate.descriptor, 0));
    ASSERT_TRUE(h.sink.ApplyAuthoredStopFence(b.descriptor, 0));
    EXPECT_FALSE(h.sink.ApplyAuthoredStopFence(root.descriptor, 0));
    op.Cancel();
    h.sink.EndQuantum(0);
    for (auto token : {first, second}) {
        harken::DecisionCommittedSnapshot snapshot{};
        ASSERT_TRUE(h.decisions.CaptureCommitted(token, &snapshot));
        EXPECT_FALSE(snapshot.authoredStopInvalidated);
    }
    EXPECT_EQ(0u, op.AppliedAudioSequence());
}

TEST(AuthoredStopAudio, OldDecisionAndOperationGenerationsCannotCancelReusedSlots) {
    StopFenceHarness h;
    const auto first = h.Decision();
    ASSERT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated,
        h.decisions.Invalidate(first));
    ASSERT_TRUE(h.decisions.Retire(first, 1));
    ASSERT_TRUE(h.decisions.ReclaimRetired(first));
    const auto second = h.Decision();
    ASSERT_EQ(first.slot, second.slot);
    ASSERT_NE(first.generation, second.generation);
    std::optional<harken::AuthoredStopOperation> op;
    op.emplace(harken::AuthoredStopOperationIdentity{0, 1, 17, 19, 23, 0, 17});
    FenceRecord old(*op, first);
    FenceRecord oldRoot(*op);
    op.reset();
    op.emplace(harken::AuthoredStopOperationIdentity{0, 2, 17, 19, 23, 0, 17});
    FenceRecord staleDecision(*op, first);
    FenceRecord current(*op, second, h.Pin(second));
    ASSERT_TRUE(h.sink.BeginQuantum(0));
    ASSERT_TRUE(h.sink.ApplyAuthoredStopFence(old.descriptor, 0));
    ASSERT_TRUE(h.sink.ApplyAuthoredStopFence(oldRoot.descriptor, 0));
    ASSERT_TRUE(h.sink.ApplyAuthoredStopFence(staleDecision.descriptor, 0));
    h.sink.EndQuantum(0);
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(h.decisions.CaptureCommitted(second, &snapshot));
    EXPECT_FALSE(snapshot.authoredStopInvalidated);
    EXPECT_EQ(0u, op->AppliedAudioSequence());
    ASSERT_TRUE(h.sink.BeginQuantum(1));
    ASSERT_TRUE(h.sink.ApplyAuthoredStopFence(current.descriptor, 1));
    h.sink.EndQuantum(1);
    ASSERT_TRUE(h.decisions.CaptureCommitted(second, &snapshot));
    EXPECT_TRUE(snapshot.authoredStopInvalidated);
}

TEST(AuthoredStopAudio, AuthoredReasonPreservesEarlierInvalidationAndClearsOnReuse) {
    StopFenceHarness h;
    const auto deadline = h.Decision();
    const auto authored = h.Decision();
    EXPECT_EQ(harken::DecisionDeadlineDisposition::NewlyInvalidated,
        h.decisions.InvalidateForDeadline(deadline));
    EXPECT_EQ(harken::DecisionInvalidationDisposition::NotEligible,
        h.decisions.InvalidateForAuthoredStop(deadline));
    EXPECT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated,
        h.decisions.InvalidateForAuthoredStop(authored));
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(h.decisions.CaptureCommitted(deadline, &snapshot));
    EXPECT_FALSE(snapshot.authoredStopInvalidated);
    EXPECT_EQ(harken::AudioGroupState::DeadlineInvalidated, snapshot.audioState);
    ASSERT_TRUE(h.decisions.Retire(authored, 1));
    ASSERT_TRUE(h.decisions.ReclaimRetired(authored));
    const auto next = h.Decision();
    ASSERT_EQ(authored.slot, next.slot);
    ASSERT_TRUE(h.decisions.CaptureCommitted(next, &snapshot));
    EXPECT_FALSE(snapshot.authoredStopInvalidated);
}

TEST(AuthoredStopAudio, ApplyingStopCannotBeCancelledBeforeItsPublishedAudioFact) {
    StopFenceHarness h;
    const auto decision = h.Decision();
    harken::AuthoredStopOperation operation({0, 1, 17, 19, 23, 0, 17});
    FenceRecord first(operation, decision, h.Pin(decision));
    FenceRecord second(operation, {}, {});
    ASSERT_TRUE(first.descriptor.TryBeginApply());
    EXPECT_FALSE(first.descriptor.TryBeginApply());
    ASSERT_TRUE(second.descriptor.TryBeginApply());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, operation.Cancel());
    EXPECT_TRUE(operation.Active(operation.Identity()));
    EXPECT_FALSE(first.descriptor.DisarmAndReleaseDecisionPin());
    operation.PublishApplied(operation.Identity(), 1);
    EXPECT_EQ(1u, operation.AppliedAudioSequence());
    first.descriptor.FinishApply();
    first.descriptor.FinishApply();
    EXPECT_TRUE(first.descriptor.DisarmAndReleaseDecisionPin());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, operation.Cancel());
    second.descriptor.FinishApply();
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, operation.Cancel());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::AlreadyCancelled, operation.Cancel());
    EXPECT_EQ(1u, operation.AppliedAudioSequence());
    EXPECT_FALSE(second.descriptor.TryBeginApply());
}

TEST(AuthoredStopAudio, CancelledOrDisarmedStopCannotAcquireApplicationAccess) {
    harken::AuthoredStopOperation operation({0, 1, 17, 19, 23, 0, 17});
    FenceRecord disarmed(operation, {}, {});
    ASSERT_TRUE(disarmed.descriptor.DisarmAndReleaseDecisionPin());
    EXPECT_FALSE(disarmed.descriptor.TryBeginApply());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, operation.Cancel());
    FenceRecord cancelled(operation, {}, {});
    EXPECT_FALSE(cancelled.descriptor.TryBeginApply());
    operation.PublishApplied(operation.Identity(), 1);
    EXPECT_EQ(0u, operation.AppliedAudioSequence());
}

TEST(AuthoredStopAudio, ApplyingFencePinsExactDecisionUntilFinalAudioAccessCompletes) {
    StopFenceHarness h(0, 8, 1);
    const auto decision = h.Decision();
    harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 17});
    FenceRecord record(op, decision, h.Pin(decision));
    ASSERT_TRUE(record.descriptor.TryBeginApply());
    ASSERT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated,
        h.decisions.InvalidateForAuthoredStop(decision));
    ASSERT_TRUE(h.decisions.Retire(decision, 1));
    EXPECT_FALSE(record.descriptor.DisarmAndReleaseDecisionPin());
    EXPECT_FALSE(h.decisions.ReclaimRetired(decision));
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(h.decisions.CaptureCommitted(decision, &snapshot));
    EXPECT_EQ(1u, snapshot.futurePinCount);
    record.descriptor.FinishApply();
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Done, record.descriptor.Access());
    EXPECT_TRUE(record.descriptor.DisarmAndReleaseDecisionPin());
    EXPECT_TRUE(record.descriptor.DisarmAndReleaseDecisionPin());
    ASSERT_TRUE(h.decisions.CaptureCommitted(decision, &snapshot));
    EXPECT_EQ(0u, snapshot.futurePinCount);
    ASSERT_TRUE(h.decisions.ReclaimRetired(decision));
    const auto next = h.Decision();
    EXPECT_EQ(decision.slot, next.slot);
    EXPECT_NE(decision.generation, next.generation);
    EXPECT_FALSE(record.descriptor.TryBeginApply());
    ASSERT_TRUE(h.decisions.CaptureCommitted(next, &snapshot));
    EXPECT_FALSE(snapshot.authoredStopInvalidated);
}

TEST(AuthoredStopAudio, DisarmedFutureFenceAllowsPromptDecisionReuseWithoutCancellingRoot) {
    StopFenceHarness h(0, 8, 1);
    const auto first = h.Decision();
    harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 145});
    FenceRecord old(op, first, h.Pin(first));
    FenceRecord root(op);
    h.Submit(old.request);
    h.Submit(root.request);
    ASSERT_TRUE(old.descriptor.DisarmAndReleaseDecisionPin());
    ASSERT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated, h.decisions.Invalidate(first));
    ASSERT_TRUE(h.decisions.Retire(first, 1));
    ASSERT_TRUE(h.decisions.ReclaimRetired(first));
    const auto second = h.Decision();
    EXPECT_EQ(first.slot, second.slot);
    EXPECT_NE(first.generation, second.generation);
    FenceRecord current(op, second, h.Pin(second));
    h.Submit(current.request);
    h.pool.Publish(256);
    h.executor.ExecuteQuantum(0);
    h.executor.ExecuteQuantum(1);
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(h.decisions.CaptureCommitted(second, &snapshot));
    EXPECT_FALSE(snapshot.authoredStopInvalidated);
    EXPECT_EQ(0u, op.AppliedAudioSequence());
    h.executor.ExecuteQuantum(2);
    EXPECT_EQ(1u, h.executor.Stats().staleSkippedTransactions);
    EXPECT_EQ(0u, h.executor.Stats().armFailedTransactions);
    EXPECT_EQ(0u, h.executor.Stats().protocolViolation);
    EXPECT_EQ(3u, op.AppliedAudioSequence());
    ASSERT_TRUE(h.decisions.CaptureCommitted(second, &snapshot));
    EXPECT_TRUE(snapshot.authoredStopInvalidated);
    EXPECT_TRUE(current.descriptor.DisarmAndReleaseDecisionPin());
}

TEST(AuthoredStopAudio, MultipleFencePinsAndMoveOnlyReleaseCannotUnderflowOtherOwners) {
    static_assert(!std::is_copy_constructible_v<harken::DecisionFuturePin>);
    static_assert(!std::is_copy_assignable_v<harken::DecisionFuturePin>);
    StopFenceHarness h(0, 8, 1);
    const auto token = h.Decision();
    auto first = h.Pin(token);
    auto moved = std::move(first);
    EXPECT_FALSE(first.Valid());
    EXPECT_TRUE(first.Release());
    harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 17});
    FenceRecord a(op, token, std::move(moved));
    FenceRecord b(op, token, h.Pin(token));
    EXPECT_TRUE(a.descriptor.DisarmAndReleaseDecisionPin());
    EXPECT_TRUE(a.descriptor.DisarmAndReleaseDecisionPin());
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(h.decisions.CaptureCommitted(token, &snapshot));
    EXPECT_EQ(1u, snapshot.futurePinCount);
    ASSERT_TRUE(h.sink.BeginQuantum(0));
    ASSERT_TRUE(h.sink.ApplyAuthoredStopFence(b.descriptor, 0));
    h.sink.EndQuantum(0);
    ASSERT_TRUE(h.decisions.Retire(token, 1));
    EXPECT_FALSE(h.decisions.ReclaimRetired(token));
    EXPECT_TRUE(b.descriptor.DisarmAndReleaseDecisionPin());
    EXPECT_TRUE(h.decisions.ReclaimRetired(token));
}

TEST(AuthoredStopAudio, FuturePinOverflowAndPreparedRollbackLeaveIdentityIntact) {
    StopFenceHarness h(0, 8, 1);
    harken::DecisionReserveRequest request{};
    request.world = 1;
    request.transport = 1;
    request.expectedContinuationGeneration = 1;
    request.sliceCount = 1;
    harken::PreparedDecisionReservation prepared;
    ASSERT_TRUE(h.decisions.PrepareReservation(request, &prepared));
    const auto token = prepared.Token();
    ASSERT_TRUE(h.decisions.SetFuturePinCountForTests(token, UINT32_MAX));
    harken::DecisionFuturePin refused;
    EXPECT_FALSE(h.decisions.AcquireFuturePin(token, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_TRUE(h.decisions.IsOpen(token));
    EXPECT_FALSE(h.decisions.CancelPreparedReservation(&prepared));
    ASSERT_TRUE(h.decisions.SetFuturePinCountForTests(token, 0));
    auto pin = h.Pin(token);
    EXPECT_FALSE(h.decisions.CancelPreparedReservation(&prepared));
    EXPECT_TRUE(h.decisions.IsOpen(token));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_TRUE(pin.Release());
    EXPECT_TRUE(pin.Release());
    EXPECT_TRUE(h.decisions.CancelPreparedReservation(&prepared));
    EXPECT_FALSE(h.decisions.AcquireFuturePin(token, &refused));
    EXPECT_EQ(0u, h.decisions.LiveCount());
}

TEST(AuthoredStopAudio, NonemptyDecisionDescriptorWithoutMatchingPinCannotApply) {
    StopFenceHarness h;
    const auto token = h.Decision();
    const auto other = h.Decision();
    harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 17});
    FenceRecord unpinned(op, token);
    FenceRecord mismatched(op, token, h.Pin(other));
    EXPECT_FALSE(unpinned.descriptor.Active());
    EXPECT_FALSE(mismatched.descriptor.Active());
    EXPECT_FALSE(unpinned.descriptor.TryBeginApply());
    h.Submit(unpinned.request);
    h.Submit(mismatched.request);
    h.pool.Publish(256);
    h.executor.ExecuteQuantum(0);
    EXPECT_EQ(2u, h.executor.Stats().staleSkippedTransactions);
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(h.decisions.CaptureCommitted(token, &snapshot));
    EXPECT_FALSE(snapshot.authoredStopInvalidated);
    EXPECT_EQ(0u, snapshot.futurePinCount);
    EXPECT_EQ(0u, op.AppliedAudioSequence());
}

TEST(AuthoredStopAudio, GatedRootStopReleasesWithoutStartingInSerialAndWorker) {
    for (const auto workers : {0u, 2u}) {
        for (const auto laterStart : {0, 17, 31}) {
            SCOPED_TRACE(testing::Message() << "workers=" << workers << " later=" << laterStart);
            StopAudioHarness h(workers);
            h.captureControl = true;
            const auto render = h.Play();
            const auto observer = h.lastObserver;
            h.Begin();
            auto stop = StopAt(17);
            stop.controlCause.request.key = {5, 17};
            h.Append(render, stop);
            if (laterStart) h.Append(render, StartAt(static_cast<std::uint16_t>(laterStart)));
            h.Render();
            EXPECT_TRUE(h.engine->voices.ReleaseObserved(observer));
            EXPECT_EQ(nullptr, h.engine->voices.Resolve(render));
            harken::MonitorVoiceControlFact control{};
            ASSERT_TRUE(h.controls[0].TryRead(h.controlBindings[0], &control));
            EXPECT_TRUE(control.timelineControl);
            EXPECT_EQ(stop.controlCause.request.key, control.cause.request.key);
            EXPECT_EQ(harken::SourceTerminalKind::Stop, control.sourceKind);
            for (std::uint32_t i = 0; i < kBlock; ++i) EXPECT_EQ(0.0f, h.Sample(i));
            h.Begin();
            h.Render();
            EXPECT_TRUE(h.engine->voices.ReleaseObserved(observer));
        }
    }
}

TEST(AuthoredStopAudio, PreinstalledGroupPreservesEarlySamplesAndCancelsFutureSlices) {
    for (const auto workers : {2u}) {
        SCOPED_TRACE(workers);
        StopFenceHarness h(workers);
        const auto decision = h.Decision(2);
        ASSERT_TRUE(h.decisions.TryClaimAudio(decision));
        ASSERT_TRUE(h.decisions.MarkAudioAdmitted(decision));
        std::array<VnVoiceHandle, 2> renders{};
        std::array<harken::VoiceReleaseObserver, 2> observers{};
        std::array<harken::TimelineArmToken, 2> members{};
        std::array<std::array<harken::TimelineEventPayload, 3>, 2> payloads{};
        std::array<std::array<harken::ScheduledEventInput, 3>, 2> events{};
        std::array<harken::TimelineProgramView, 2> programs{};
        for (std::uint32_t i = 0; i < 2; ++i) {
            renders[i] = h.audio.Play();
            observers[i] = h.audio.lastObserver;
            ASSERT_TRUE(h.arms.ReserveGrouped(static_cast<std::uint32_t>(renders[i]),
                static_cast<std::uint32_t>(renders[i] >> 32), 0, 0, 1, {},
                {decision}, static_cast<std::uint16_t>(i), &members[i]));
            ASSERT_TRUE(h.arms.AcknowledgeApplied(members[i]));
            ASSERT_TRUE(h.arms.Publish(members[i]));
            payloads[i][0].kind = harken::TimelineEventKind::Start;
            payloads[i][1].kind = harken::TimelineEventKind::Stop;
            payloads[i][1].reserved = harken::kTimelineEventAuthoredRootStop;
            payloads[i][1].envelopeEndFrame = 17;
            payloads[i][2].kind = harken::TimelineEventKind::Envelope;
            payloads[i][2].envelopeDirection = harken::TimelineEnvelopeDirection::FadeIn;
            payloads[i][2].envelopeStartFrame = 128;
            payloads[i][2].envelopeEndFrame = 160;
            programs[i] = {1, payloads[i].data(), 3};
            events[i][0].executeFrame = i == 0 ? 8 : 31;
            events[i][0].subeventPhase = 20;
            events[i][1].executeFrame = 17;
            events[i][1].eventIndex = 1;
            events[i][1].payload = 1;
            events[i][1].subeventPhase = 50;
            events[i][2].executeFrame = 128;
            events[i][2].eventIndex = 2;
            events[i][2].payload = 2;
            events[i][2].subeventPhase = 30;
            harken::TransactionRequest request{};
            request.transportEpoch = 1;
            request.program = {1, &programs[i]};
            request.armToken = members[i];
            std::swap(events[i][1], events[i][2]);
            request.events = events[i].data();
            request.eventCount = 2;
            h.Submit(request);
            request.events = &events[i][2];
            request.eventCount = 1;
            h.Submit(request);
        }
        for (const auto block : {0u, 2u}) {
            harken::Plan03GroupSliceInput slice{};
            slice.executeFrame = block * kBlock;
            slice.decision = decision;
            slice.sliceIndex = block / 2;
            slice.members = members.data();
            slice.memberCount = 2;
            ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                h.pool.ReservePlan03GroupSlice(slice, h.arms));
        }
        harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 17});
        FenceRecord root(op);
        FenceRecord group(op, decision, h.Pin(decision));
        h.Submit(root.request);
        h.Submit(group.request);
        h.pool.Publish(384, nullptr, nullptr, &h.arms);
        for (std::uint32_t block = 0; block < 6; ++block) {
            h.executor.ExecuteQuantum(block);
            h.Healthy();
            if (block == 2) {
                harken::Plan03PageGroupView view{};
                ASSERT_EQ(1u, h.pool.CapturePlan03PageGroupsRt(2, &view, 1));
                const auto lease = view.lease->Capture();
                EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, lease.state);
                EXPECT_EQ(harken::AudioSliceReasonCancel, lease.pendingReasonBits);
            }
            h.audio.Render();
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                const auto frame = block * kBlock + i;
                const auto gain = frame < 8 ? 0.0f : frame < 17 ? 1.0f : frame < 257
                    ? static_cast<float>(256 - frame) / 240.0f : 0.0f;
                EXPECT_NEAR(0.25f * gain, h.audio.Sample(i), 1e-6f) << frame;
            }
            if (block == 0) EXPECT_TRUE(h.audio.engine->voices.ReleaseObserved(observers[1]));
        }
        EXPECT_TRUE(h.audio.engine->voices.ReleaseObserved(observers[0]));
    }
}

std::vector<float> RenderDominanceStream(std::uint32_t workers, int adversarialOrder) {
    StopAudioHarness h(workers);
    const auto render = h.Play(false);
    std::vector<float> output;
    for (std::uint32_t block = 0; block < 6; ++block) {
        h.Begin();
        if (h.engine->voices.Resolve(render) != nullptr) {
            if (block == 0 && adversarialOrder != 2) h.Append(render, StopAt(17));
            if (adversarialOrder != 0) {
                auto envelope = StartAt(17);
                envelope.kind = harken::TimelineEventKind::Envelope;
                envelope.envelopeDirection = harken::TimelineEnvelopeDirection::FadeIn;
                envelope.frame = 0;
                envelope.endFrame = 256;
                h.Append(render, envelope);
                auto weight = StartAt(17);
                weight.kind = harken::TimelineEventKind::VariableWeight;
                weight.variableWeight = {0, 256, 0.0f, 0.0f, 1, {1, 1}, {1, 1}};
                h.Append(render, weight);
                h.Append(render, StartAt(17));
                auto cursor = StartAt(17);
                cursor.kind = harken::TimelineEventKind::SetStartCursor;
                cursor.value = 1024;
                h.Append(render, cursor);
                h.Append(render, StopAt(17, false));
                h.Append(render, StopAt(31));
            }
            if (block == 0 && adversarialOrder == 2) h.Append(render, StopAt(17));
        }
        h.Render();
        for (std::uint32_t frame = 0; frame < kBlock; ++frame) output.push_back(h.Sample(frame));
    }
    EXPECT_TRUE(h.engine->voices.ReleaseObserved(h.lastObserver));
    const auto reused = h.Play();
    EXPECT_NE(render, reused);
    h.Begin();
    h.Append(reused, StartAt(9));
    h.Render();
    EXPECT_EQ(0.0f, h.Sample(8));
    EXPECT_NEAR(0.25f, h.Sample(9), 1e-6f);
    return output;
}

TEST(AuthoredStopAudio, SameFrameAppendOrderAndLaterBlocksMatchOnlyAuthorizedControls) {
    for (const auto workers : {0u, 2u}) {
        const auto reference = RenderDominanceStream(workers, 0);
        for (const auto order : {1, 2}) {
            const auto actual = RenderDominanceStream(workers, order);
            ASSERT_EQ(reference.size(), actual.size());
            for (std::size_t i = 0; i < actual.size(); ++i)
                EXPECT_FLOAT_EQ(reference[i], actual[i]) << "workers=" << workers << " frame=" << i;
        }
    }
}

TEST(AuthoredStopAudio, RootStopDoesNotExtendEarlierTimelineFallback) {
    for (const auto workers : {2u}) {
        StopAudioHarness h(workers);
        const auto render = h.Play(false);
        for (std::uint32_t block = 0; block < 5; ++block) {
            h.Begin();
            if (block == 0) h.Append(render, StopAt(7, false));
            if (block == 1) h.Append(render, StopAt(17));
            h.Render();
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                const auto frame = block * kBlock + i;
                const auto gain = frame < 7 ? 1.0f : frame < 247
                    ? static_cast<float>(246 - frame) / 240.0f : 0.0f;
                EXPECT_NEAR(0.25f * gain, h.Sample(i), 1e-6f) << frame;
            }
        }
        EXPECT_TRUE(h.engine->voices.ReleaseObserved(h.lastObserver));
    }
}

TEST(AuthoredStopAudio, PrebufferUsesActualRingReadinessWithoutInventingSourceProgress) {
    for (const auto workers : {2u}) {
        for (const auto ready : {false, true}) {
            SCOPED_TRACE(testing::Message() << workers << " ready=" << ready);
            std::array<float, 512> samples{};
            samples.fill(0.25f);
            float* planes[harken::kMaxChannels]{};
            planes[0] = samples.data();
            harken::StreamRing ring;
            ring.Wire(planes, static_cast<std::uint32_t>(samples.size()));
            ring.ResetForUse(1);
            ring.writeIndex.store(ready ? 512 : 16, std::memory_order_release);
            StopAudioHarness h(workers);
            const auto render = h.Play(false);
            auto* context = h.engine->voices.Resolve(render);
            ASSERT_NE(nullptr, context);
            context->streamRing = &ring;
            context->streamGateOpen = false;
            context->prebufferTargetFrames = kBlock;
            for (std::uint32_t block = 0; block < 6; ++block) {
                h.Begin();
                if (block == 0) h.Append(render, StopAt(17));
                h.Render();
                for (std::uint32_t i = 0; i < kBlock; ++i) {
                    const auto frame = block * kBlock + i;
                    const auto gain = !ready ? 0.0f : frame < 17 ? 1.0f : frame < 257
                        ? static_cast<float>(256 - frame) / 240.0f : 0.0f;
                    EXPECT_NEAR(0.25f * gain, h.Sample(i), 1e-6f) << frame;
                }
                if (!ready) {
                    EXPECT_EQ(0u, ring.readIndex.load());
                    EXPECT_TRUE(h.engine->voices.ReleaseObserved(h.lastObserver));
                }
            }
            EXPECT_TRUE(h.engine->voices.ReleaseObserved(h.lastObserver));
        }
    }
}

TEST(AuthoredStopAudio, ArrivedRootStopSurvivesAnUnrenderableQuantum) {
    for (const auto workers : {2u}) {
      for (const auto missed : {1u, 4u, 6u}) {
        SCOPED_TRACE(testing::Message() << workers << " missed=" << missed);
        StopAudioHarness h(workers);
        const auto render = h.Play(false);
        harken::BlockArena emptyArena(1);
        for (std::uint32_t block = 0; block < 7; ++block) {
            h.Begin();
            if (block == 0) h.Append(render, StopAt(17));
            if (block < missed) h.RenderWith(emptyArena);
            else h.Render();
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                const auto frame = block * kBlock + i;
                const auto gain = block < missed || frame >= 257 ? 0.0f :
                    static_cast<float>(256 - frame) / 240.0f;
                EXPECT_NEAR(0.25f * gain, h.Sample(i), 1e-6f) << frame;
            }
            if (block >= std::max(4u, missed))
                EXPECT_TRUE(h.engine->voices.ReleaseObserved(h.lastObserver)) << block;
        }
        EXPECT_TRUE(h.engine->voices.ReleaseObserved(h.lastObserver));
        for (std::uint32_t i = 0; i < kBlock; ++i) EXPECT_EQ(0.0f, h.Sample(i));
      }
    }
}

TEST(AuthoredStopAudio, MarkerTransactionsRespectPoolCapacityAtomically) {
    StopFenceHarness h(0, 2);
    harken::AuthoredStopOperation first({0, 1, 17, 19, 23, 0, 17});
    harken::AuthoredStopOperation second({1, 1, 17, 19, 23, 0, 17});
    harken::AuthoredStopOperation third({2, 1, 17, 19, 23, 0, 17});
    FenceRecord a(first), b(second), c(third);
    h.Submit(a.request);
    h.Submit(b.request);
    harken::PreparedTransactionBatch refused;
    EXPECT_NE(harken::ReserveRefusal::None,
        h.pool.PrepareTransactionBatch(&c.request, 1, 0, &refused));
    EXPECT_FALSE(refused.Valid());
    h.pool.Publish(256);
    h.executor.ExecuteQuantum(0);
    h.Healthy();
    EXPECT_EQ(1u, first.AppliedAudioSequence());
    EXPECT_EQ(1u, second.AppliedAudioSequence());
    EXPECT_EQ(0u, third.AppliedAudioSequence());
    ASSERT_TRUE(h.sink.BeginQuantum(1));
    ASSERT_TRUE(h.sink.ApplyAuthoredStopFence(a.descriptor, 1));
    h.sink.EndQuantum(1);
    EXPECT_EQ(1u, first.AppliedAudioSequence());
}

TEST(AuthoredStopAudio, ActualCoverageControlsSameQuantumAndLateOrMissingEnvelopeFallback) {
    struct Scenario {
        std::uint64_t stop;
        std::uint64_t envelopeEnd;
        std::uint64_t envelopeDispatch;
        bool envelope;
        bool covered;
    };
    const Scenario scenarios[] = {
        {32, 32, 0, true, true},
        {128, 128, 0, true, true},
        {128, 128, 0, false, false},
        {64, 32, 64, true, false},
        {64, 32, 0, true, false},
    };
    for (const auto workers : {2u}) {
        for (const auto& scenario : scenarios) {
            SCOPED_TRACE(testing::Message() << workers << " stop=" << scenario.stop
                << " envelope=" << scenario.envelope << " end=" << scenario.envelopeEnd
                << " dispatch=" << scenario.envelopeDispatch);
            StopFenceHarness h(workers);
            const auto render = h.audio.Play(false);
            harken::TimelineArmToken arm{};
            ASSERT_TRUE(h.arms.Reserve(static_cast<std::uint32_t>(render),
                static_cast<std::uint32_t>(render >> 32), 0, 0, 1, {}, &arm));
            ASSERT_TRUE(h.arms.AcknowledgeApplied(arm));
            ASSERT_TRUE(h.arms.Publish(arm));
            std::array<harken::TimelineEventPayload, 2> payloads{};
            payloads[0].kind = harken::TimelineEventKind::Envelope;
            payloads[0].envelopeDirection = harken::TimelineEnvelopeDirection::FadeOut;
            payloads[0].envelopeEndFrame = scenario.envelopeEnd;
            payloads[1].kind = harken::TimelineEventKind::Stop;
            payloads[1].reserved = harken::kTimelineEventAuthoredRootStop;
            payloads[1].envelopeEndFrame = scenario.stop;
            payloads[1].authoredFadeOutCandidate = true;
            harken::TimelineProgramView program{1, payloads.data(), 2};
            std::array<harken::ScheduledEventInput, 2> events{};
            events[0].executeFrame = scenario.envelopeDispatch;
            events[0].subeventPhase = 30;
            events[1].executeFrame = scenario.stop;
            events[1].subeventPhase = 50;
            events[1].eventIndex = 1;
            events[1].payload = 1;
            events[1].authoredFadeOutCandidate = true;
            harken::TransactionRequest request{};
            request.transportEpoch = 1;
            request.program = {1, &program};
            request.armToken = arm;
            request.events = scenario.envelope ? events.data() : &events[1];
            request.eventCount = scenario.envelope ? 2 : 1;
            h.Submit(request);
            h.pool.Publish(384);
            for (std::uint32_t block = 0; block < 6; ++block) {
                h.executor.ExecuteQuantum(block);
                h.Healthy();
                h.audio.Render();
                for (std::uint32_t i = 0; i < kBlock; ++i) {
                    const auto frame = block * kBlock + i;
                    float gain = 1.0f;
                    if (scenario.envelope && scenario.envelopeDispatch == 0 &&
                        frame < scenario.envelopeEnd) {
                        gain = 1.0f - static_cast<float>(frame) /
                            static_cast<float>(scenario.envelopeEnd - 1);
                    }
                    if (frame >= scenario.stop) {
                        gain = scenario.covered || frame >= scenario.stop + 240 ? 0.0f :
                            static_cast<float>(scenario.stop + 239 - frame) / 240.0f;
                    }
                    EXPECT_NEAR(0.25f * gain, h.audio.Sample(i), 1e-6f) << frame;
                }
            }
            EXPECT_TRUE(h.audio.engine->voices.ReleaseObserved(h.audio.lastObserver));
        }
    }
}

TEST(AuthoredStopAudio, ResetTimelineStopCapturesOnlyItsActualSourceWinner) {
    for (const auto workers : {0u, 2u}) {
        for (const auto naturalFirst : {false, true}) {
            SCOPED_TRACE(testing::Message() << "workers=" << workers << " natural=" << naturalFirst);
            StopAudioHarness h(workers);
            h.captureControl = true;
            std::vector<float> shortPcm(8, 0.25f);
            if (naturalFirst) {
                const auto pcm = harkentest::MonoDesc(shortPcm, kRate);
                ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &pcm));
            }
            const auto render = h.Play(false, naturalFirst ? 2 : 1);
            ASSERT_EQ(nullptr, h.engine->voices.Resolve(render)->sourceTerminal);
            const auto observer = h.lastObserver;
            harken::MonitorControlCause cause{};
            cause.request.key = {11, 17};
            cause.actionFiring = 23;
            cause.executionOrdinal = 2;
            for (std::uint32_t block = 0; block < 6; ++block) {
                h.Begin();
                if (block < 2 && h.engine->voices.Resolve(render) != nullptr) {
                    auto stop = StopAt(block == 0 ? 17 : 0, false);
                    stop.controlCause = cause;
                    if (block != 0) stop.controlCause.request.key.sequence = 99;
                    h.Append(render, stop);
                }
                h.Render();
            }
            EXPECT_TRUE(h.engine->voices.ReleaseObserved(observer));
            harken::MonitorVoiceControlFact observed{};
            ASSERT_TRUE(h.controls[0].TryRead(h.controlBindings[0], &observed));
            EXPECT_EQ(naturalFirst ? harken::SourceTerminalKind::Natural : harken::SourceTerminalKind::Stop,
                observed.sourceKind);
            EXPECT_EQ(!naturalFirst, observed.timelineControl);
            EXPECT_EQ(naturalFirst ? harken::MonitorRequestKey{} : cause.request.key, observed.cause.request.key);
            if (!naturalFirst) {
                EXPECT_EQ(cause.actionFiring, observed.cause.actionFiring);
                EXPECT_EQ(cause.executionOrdinal, observed.cause.executionOrdinal);
            }
        }
    }
}

TEST(AuthoredStopAudio, FirstAppliedControlSurvivesTimelineAndApiStopMixing) {
    for (const auto workers : {0u, 2u}) {
        for (const auto timelineFirst : {false, true}) {
            for (const auto immediate : {false, true}) {
                SCOPED_TRACE(testing::Message() << "workers=" << workers
                    << " timelineFirst=" << timelineFirst << " immediate=" << immediate);
                StopAudioHarness h(workers);
                h.captureControl = true;
                const auto render = h.Play(false);
                const auto observer = h.lastObserver;
                auto stop = StopAt(17, false);
                stop.controlCause.request.key = {11, 17};
                stop.controlCause.actionFiring = 23;
                h.Begin();
                if (timelineFirst) h.Append(render, stop);
                else h.engine->voices.ApplyStop(render, immediate ? 0 : 5, 0,
                    h.engine->notifications, h.quantum);
                h.Render();
                h.Begin();
                if (timelineFirst) h.engine->voices.ApplyStop(render, immediate ? 0 : 5, 0,
                    h.engine->notifications, h.quantum);
                else if (h.engine->voices.Resolve(render) != nullptr) h.Append(render, stop);
                h.Render();
                for (std::uint32_t i = 0; i < 6; ++i) {
                    h.Begin();
                    h.Render();
                }
                EXPECT_TRUE(h.engine->voices.ReleaseObserved(observer));
                harken::MonitorVoiceControlFact observed{};
                ASSERT_TRUE(h.controls[0].TryRead(h.controlBindings[0], &observed));
                EXPECT_EQ(harken::SourceTerminalKind::Stop, observed.sourceKind);
                EXPECT_EQ(timelineFirst, observed.timelineControl);
                EXPECT_EQ(timelineFirst ? stop.controlCause.request.key : harken::MonitorRequestKey{},
                    observed.cause.request.key);
            }
        }
    }
}

TEST(AuthoredStopAudio, RootStopDominatesLaterControlsAcrossQuanta) {
    for (const auto workers : {2u}) {
        StopAudioHarness h(workers);
        h.captureControl = true;
        const auto render = h.Play(false);
        ASSERT_EQ(nullptr, h.engine->voices.Resolve(render)->sourceTerminal);
        const auto observer = h.lastObserver;
        for (std::uint32_t block = 0; block < 6; ++block) {
            h.Begin();
            if (block == 0) {
                auto stop = StopAt(17);
                stop.controlCause.request.key = {7, 31};
                stop.controlCause.actionFiring = 43;
                h.Append(render, stop);
            }
            if (h.engine->voices.Resolve(render) != nullptr) {
                auto weight = StartAt(31);
                weight.kind = harken::TimelineEventKind::VariableWeight;
                weight.variableWeight = {31, 35, 0.0f, 0.0f, 1, {1, 1}, {1, 1}};
                h.Append(render, weight);
                h.Append(render, StopAt(40, false));
                h.Append(render, StartAt(50));
            }
            h.Render();
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                const auto frame = block * kBlock + i;
                const auto gain = frame < 17 ? 1.0f : frame < 257
                    ? static_cast<float>(256 - frame) / 240.0f : 0.0f;
                EXPECT_NEAR(0.25f * gain, h.Sample(i), 1e-6f) << "frame=" << frame;
            }
        }
        EXPECT_TRUE(h.engine->voices.ReleaseObserved(observer));
        harken::MonitorVoiceControlFact control{};
        ASSERT_TRUE(h.controls[0].TryRead(h.controlBindings[0], &control));
        EXPECT_TRUE(control.timelineControl);
        EXPECT_EQ((harken::MonitorRequestKey{7, 31}), control.cause.request.key);
        EXPECT_EQ(43u, control.cause.actionFiring);
        EXPECT_EQ(harken::SourceTerminalKind::Stop, control.sourceKind);
    }
}

TEST(AuthoredStopAudio, CrossQuantumAuthoredEnvelopeDoesNotRestoreGainAtStop) {
    for (const auto workers : {2u}) {
        SCOPED_TRACE(testing::Message() << "workers=" << workers);
        StopAudioHarness h(workers);
        const auto render = h.Play(false);
        const auto observer = h.lastObserver;
        harken::ScheduledTransactionPool pool;
        harken::ScheduledTransactionPool::Config config{};
        config.quantumFrames = kBlock;
        config.scheduleHorizonFrames = 512;
        config.publishLeadQuanta = 1;
        config.maxScheduledTransactions = 4;
        config.maxTransactionSpanFrames = 256;
        config.maxIngressTransactionsPerQuantum = 4;
        config.maxFrameBatchesPerQuantum = 8;
        config.maxEventsPerQuantum = 8;
        config.maxTransactionEvents = 8;
        ASSERT_TRUE(pool.Reset(config, 0, 1));
        harken::TimelineArmTable arms(1);
        harken::TimelineArmToken arm{};
        ASSERT_TRUE(arms.Reserve(static_cast<std::uint32_t>(render),
            static_cast<std::uint32_t>(render >> 32), 0, 0, 1, {}, &arm));
        ASSERT_TRUE(arms.AcknowledgeApplied(arm));
        ASSERT_TRUE(arms.Publish(arm));
        harken::VoiceTimelineSink sink(h.engine->voices, arms);
        sink.SetArenaAdmission(65536, kBlock);
        harken::TimelineExecutor executor;
        ASSERT_TRUE(executor.Reset(kBlock, 8, &pool, &arms, &sink));
        std::array<harken::TimelineEventPayload, 2> payloads{};
        payloads[0].kind = harken::TimelineEventKind::Envelope;
        payloads[0].envelopeDirection = harken::TimelineEnvelopeDirection::FadeOut;
        payloads[0].envelopeStartFrame = 0;
        payloads[0].envelopeEndFrame = 128;
        payloads[1].kind = harken::TimelineEventKind::Stop;
        payloads[1].envelopeEndFrame = 128;
        payloads[1].authoredFadeOutCandidate = true;
        harken::TimelineProgramView program{1, payloads.data(), 2};
        std::array<harken::ScheduledEventInput, 2> events{};
        events[0].subeventPhase = 30;
        events[1].executeFrame = 128;
        events[1].eventIndex = 1;
        events[1].subeventPhase = 50;
        events[1].payload = 1;
        events[1].authoredFadeOutCandidate = true;
        harken::TransactionRequest request{};
        request.transportEpoch = 1;
        request.program = {1, &program};
        request.armToken = arm;
        request.events = events.data();
        request.eventCount = 2;
        harken::PreparedTransactionBatch prepared;
        ASSERT_EQ(harken::ReserveRefusal::None, pool.PrepareTransactionBatch(&request, 1, 0, &prepared));
        ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&prepared, 1));
        pool.Publish(256);
        for (std::uint32_t block = 0; block < 3; ++block) {
            executor.ExecuteQuantum(block);
            ASSERT_EQ(0u, executor.Stats().armFailedTransactions);
            ASSERT_EQ(0u, executor.Stats().protocolViolation);
            ASSERT_EQ(0u, executor.Stats().staleSkippedTransactions);
            h.Render();
            RecordProperty("workers_" + std::to_string(workers) + "_block_" +
                std::to_string(block) + "_first", std::to_string(h.Sample(0)));
            RecordProperty("workers_" + std::to_string(workers) + "_block_" +
                std::to_string(block) + "_last", std::to_string(h.Sample(kBlock - 1)));
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                const auto frame = block * kBlock + i;
                const auto gain = frame < 128 ? 1.0f - static_cast<float>(frame) / 127.0f : 0.0f;
                EXPECT_NEAR(0.25f * gain, h.Sample(i), 1e-6f) << "frame=" << frame;
            }
        }
        EXPECT_TRUE(h.engine->voices.ReleaseObserved(observer));
    }
}

} // namespace


TEST(AuthoredStopAudio, LaneFencePreparedAbortKeepsOriginalStopUnappliedAndReacquirable) {
    harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 81});
    FenceRecord record(op);
    record.payload.authoredStopOperation = op.Identity();
    harken::AuthoredStopFenceLaneProof proof;
    ASSERT_TRUE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    EXPECT_TRUE(proof.Valid());
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Prepared, record.descriptor.Access());
    EXPECT_FALSE(record.descriptor.TryBeginApply());
    record.descriptor.FinishApply();
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Prepared, record.descriptor.Access());
    EXPECT_FALSE(record.descriptor.DisarmAndReleaseDecisionPin());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, op.Cancel());
    EXPECT_EQ(0u, op.AppliedAudioSequence());
    proof.Reset();
    EXPECT_FALSE(proof.Valid());
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, record.descriptor.Access());
    EXPECT_EQ(0u, op.AppliedAudioSequence());
    ASSERT_TRUE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    proof.Reset();
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, op.Cancel());
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    EXPECT_FALSE(proof.Valid());
}

TEST(AuthoredStopAudio, LaneFenceProofDestructorAndMoveReleaseOnlyTheOriginalPreparedReader) {
    harken::AuthoredStopOperation firstOp({0, 1, 17, 19, 23, 0, 81});
    harken::AuthoredStopOperation secondOp({1, 1, 29, 31, 37, 1, 83});
    FenceRecord first(firstOp), second(secondOp);
    first.payload.authoredStopOperation = firstOp.Identity();
    second.payload.authoredStopOperation = secondOp.Identity();
    {
        harken::AuthoredStopFenceLaneProof left, right;
        ASSERT_TRUE(first.descriptor.ProveLaneApply(first.request.program, &left));
        ASSERT_TRUE(second.descriptor.ProveLaneApply(second.request.program, &right));
        right = std::move(left);
        EXPECT_FALSE(left.Valid());
        EXPECT_TRUE(right.Valid());
        EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, second.descriptor.Access());
        EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, secondOp.Cancel());
        EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, firstOp.Cancel());
        harken::AuthoredStopFenceLaneProof moved(std::move(right));
        EXPECT_FALSE(right.Valid());
        EXPECT_TRUE(moved.Valid());
    }
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, first.descriptor.Access());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, firstOp.Cancel());
    EXPECT_EQ(0u, firstOp.AppliedAudioSequence());
    EXPECT_EQ(0u, secondOp.AppliedAudioSequence());
}

TEST(AuthoredStopAudio, LaneFenceProofConsumptionTransfersTheActualReaderToApplying) {
    harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 81});
    FenceRecord record(op);
    record.payload.authoredStopOperation = op.Identity();
    harken::AuthoredStopFenceLaneProof proof;
    ASSERT_TRUE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    record.descriptor.ApplyLaneProof(&proof);
    EXPECT_FALSE(proof.Valid());
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Applying, record.descriptor.Access());
    proof.Reset();
    EXPECT_FALSE(record.descriptor.DisarmAndReleaseDecisionPin());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, op.Cancel());
    EXPECT_EQ(0u, op.AppliedAudioSequence());
    // This Core control verifies the existing final fact protocol, not PCM.
    op.PublishApplied(op.Identity(), 7);
    record.descriptor.FinishApply();
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Done, record.descriptor.Access());
    EXPECT_EQ(7u, op.AppliedAudioSequence());
    EXPECT_TRUE(record.descriptor.DisarmAndReleaseDecisionPin());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, op.Cancel());
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
}

TEST(AuthoredStopAudio, LaneFenceProofRejectsForeignProgramAndPayloadWithoutConsumingRights) {
    harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 81});
    FenceRecord record(op);
    record.payload.authoredStopOperation = op.Identity();
    harken::AuthoredStopFenceLaneProof proof;
    auto foreignProgram = record.program;
    EXPECT_FALSE(record.descriptor.ProveLaneApply({1, &foreignProgram}, &proof));
    EXPECT_FALSE(record.descriptor.ProveLaneApply({2, &record.program}, &proof));
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, nullptr));
    record.program.generation = 2;
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    record.program.generation = 1;
    record.program.role = harken::TimelineProgramRole::Voice;
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    record.program.role = harken::TimelineProgramRole::AuthoredStopFence;
    record.program.authoredStopFence = nullptr;
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    record.program.authoredStopFence = &record.descriptor;
    record.program.payloadCount = 0;
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    record.program.payloadCount = 1;
    const auto original = record.payload;
    for (std::uint32_t field = 0; field < 7; ++field) {
        record.payload = original;
        auto& identity = record.payload.authoredStopOperation;
        switch (field) {
        case 0: ++identity.slot; break;
        case 1: ++identity.generation; break;
        case 2: ++identity.root; break;
        case 3: ++identity.sourceEpoch; break;
        case 4: ++identity.actionFiring; break;
        case 5: ++identity.executionOrdinal; break;
        default: ++identity.stopFrame; break;
        }
        EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
        EXPECT_FALSE(proof.Valid());
        EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, record.descriptor.Access());
    }
    record.payload = original;
    record.payload.kind = harken::TimelineEventKind::Start;
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    record.payload = original;
    record.payload.reserved = 0;
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    record.payload = original;
    record.payload.reserved |= harken::kTimelineEventTransientControlArm;
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    record.payload = original;
    ++record.payload.envelopeEndFrame;
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    record.payload = original;
    ASSERT_TRUE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    EXPECT_TRUE(proof.Valid());
    EXPECT_EQ(0u, op.AppliedAudioSequence());
    proof.Reset();
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, op.Cancel());
}

TEST(AuthoredStopAudio, LaneFenceProofRejectsRealDecisionAndMalformedNoneWithoutDroppingItsPin) {
    StopFenceHarness h(0, 8, 1);
    const auto decision = h.Decision();
    harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 81});
    FenceRecord record(op, decision, h.Pin(decision));
    record.payload.authoredStopOperation = op.Identity();
    harken::AuthoredStopFenceLaneProof proof;
    EXPECT_FALSE(record.descriptor.ProveLaneApply(record.request.program, &proof));
    harken::DecisionCommittedSnapshot snapshot{};
    ASSERT_TRUE(h.decisions.CaptureCommitted(decision, &snapshot));
    EXPECT_EQ(1u, snapshot.futurePinCount);
    EXPECT_FALSE(snapshot.authoredStopInvalidated);
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, record.descriptor.Access());
    FenceRecord malformed(op, {UINT32_MAX, 1});
    malformed.payload.authoredStopOperation = op.Identity();
    EXPECT_FALSE(malformed.descriptor.ProveLaneApply(malformed.request.program, &proof));
    FenceRecord disarmed(op);
    disarmed.payload.authoredStopOperation = op.Identity();
    ASSERT_TRUE(disarmed.descriptor.DisarmAndReleaseDecisionPin());
    EXPECT_FALSE(disarmed.descriptor.ProveLaneApply(disarmed.request.program, &proof));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, op.Cancel());
    ASSERT_TRUE(record.descriptor.DisarmAndReleaseDecisionPin());
    ASSERT_TRUE(h.decisions.CaptureCommitted(decision, &snapshot));
    EXPECT_EQ(0u, snapshot.futurePinCount);
}

TEST(AuthoredStopAudio, LaneFenceProofAndCancellationRaceHaveOneOriginalOwnershipOutcome) {
    static_assert(!std::is_copy_constructible_v<harken::AuthoredStopFenceLaneProof>);
    static_assert(!std::is_copy_assignable_v<harken::AuthoredStopFenceLaneProof>);
    for (std::uint32_t iteration = 0; iteration < 64; ++iteration) {
        harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 0, 81});
        FenceRecord record(op);
        record.payload.authoredStopOperation = op.Identity();
        harken::AuthoredStopFenceLaneProof proof;
        std::atomic<bool> go{false};
        bool acquired = false;
        harken::AuthoredStopCancelDisposition cancelled{};
        std::thread claimant([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            acquired = record.descriptor.ProveLaneApply(record.request.program, &proof);
        });
        std::thread canceller([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            cancelled = op.Cancel();
        });
        go.store(true, std::memory_order_release);
        claimant.join();
        canceller.join();
        EXPECT_EQ(acquired, proof.Valid());
        if (acquired) {
            EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, cancelled);
            EXPECT_EQ(harken::AuthoredStopFenceAccess::Prepared, record.descriptor.Access());
            proof.Reset();
            EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, op.Cancel());
        } else {
            EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, cancelled);
            EXPECT_EQ(harken::AuthoredStopCancelDisposition::AlreadyCancelled, op.Cancel());
        }
        EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, record.descriptor.Access());
        EXPECT_EQ(0u, op.AppliedAudioSequence());
    }
}

namespace {

struct OriginalControlRecord {
    explicit OriginalControlRecord(std::uint64_t stopFrame = 300,
        std::uint64_t fadeStart = 100, bool envelope = true,
        std::uint32_t renderSlot = 9)
        : operation({0, 1, 17, 19, 23, 7, stopFrame}), hasEnvelope(envelope) {
        target.programSemanticFingerprint = 29;
        target.subjectId = harkentest::ObjectId(31);
        const auto count = envelope ? 2u : 1u;
        for (std::uint32_t index = 0; index < count; ++index) {
            const bool fade = envelope && index == 0;
            auto& payload = payloads[index];
            payload.kind = fade ? harken::TimelineEventKind::Envelope : harken::TimelineEventKind::Stop;
            payload.envelopeDirection = harken::TimelineEnvelopeDirection::FadeOut;
            payload.reserved = harken::kTimelineEventTransientControlArm |
                (fade ? 0u : harken::kTimelineEventAuthoredRootStop);
            payload.target = target;
            payload.envelopeIndex = fade ? 1u : 0u;
            payload.envelopeStartFrame = fade ? fadeStart : 0;
            payload.envelopeEndFrame = stopFrame;
            payload.authoredFadeOutCandidate = envelope;
            payload.authoredStopOperation = operation.Identity();
            auto& event = events[index];
            event.executeFrame = fade ? std::min(stopFrame, std::max(fadeStart, onset)) : stopFrame;
            event.executionOrdinal = operation.Identity().executionOrdinal;
            event.eventIndex = 41 + index;
            event.subeventPhase = fade ? 30u : 50u;
            event.payload = index;
            event.authoredFadeOutCandidate = envelope;
        }
        program = {5, payloads.data(), count};
        input = {&operation, {5, &program}, events.data(), count, onset, fadeStart,
            renderSlot, 11, target, 0};
    }

    bool Install(harken::TimelineArmTable& table) {
        if (!table.PrepareAuthoredControl(input, &token)) return false;
        for (std::uint32_t index = 0; index < input.eventCount; ++index) {
            events[index].armToken = token;
            // Routing metadata is supplied later by the actual publication owner.
            events[index].logicCommitSequence = 37;
            events[index].transactionOrdinal = 43;
        }
        return table.PinFrontierReader(token, &reader) && table.CommitPrepared(token) &&
            table.AcknowledgeApplied(token, 1) && table.Publish(token);
    }

    bool Prove(harken::TimelineArmTable& table, std::uint64_t begin,
        std::uint32_t frames, std::uint32_t first, std::uint32_t count,
        harken::TimelineArmTable::AuthoredControlBucketProof* proof) {
        return table.ProveAuthoredControlBucket(token, reader, input.program, begin, frames,
            begin / frames + 1, events.data() + first, count, proof);
    }

    void CancelAndReclaim(harken::TimelineArmTable& table) {
        harken::ArmState state{};
        ASSERT_TRUE(table.State(token, &state));
        if (state != harken::ArmState::Tombstoned) {
            EXPECT_TRUE(table.RequestCancel(token));
            EXPECT_TRUE(table.Tombstone(token, 11));
        }
        EXPECT_TRUE(reader.Release(12));
        EXPECT_TRUE(table.Reclaim(token, 12));
        EXPECT_EQ(1u, table.FreeCount());
    }

    static constexpr std::uint64_t onset = 80;
    harken::AuthoredStopOperation operation;
    const bool hasEnvelope;
    harken::TimelineSemanticTarget target{};
    std::array<harken::TimelineEventPayload, 2> payloads{};
    std::array<harken::ScheduledEventInput, 2> events{};
    harken::TimelineProgramView program{};
    harken::TimelineArmTable::AuthoredControlInput input{};
    harken::TimelineArmToken token{};
    harken::TimelineArmReaderPin reader{};
};

} // namespace

TEST(AuthoredStopAudio, ControlArmEnvelopeThenStopNeverStartsOrCompletesTheRootFence) {
    harken::TimelineArmTable table(1);
    OriginalControlRecord record;
    ASSERT_TRUE(record.Install(table));
    FenceRecord fence(record.operation);
    fence.payload.authoredStopOperation = record.operation.Identity();
    harken::AuthoredStopFenceLaneProof fenceProof;
    ASSERT_TRUE(fence.descriptor.ProveLaneApply(fence.request.program, &fenceProof));
    harken::TimelineArmTable::AuthoredControlBucketProof proof;
    ASSERT_TRUE(record.Prove(table, 0, 128, 0, 1, &proof));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, record.operation.Cancel());
    EXPECT_FALSE(table.RequestCancel(record.token));
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(table.Observe(record.token, &observation));
    EXPECT_EQ(harken::ArmState::Published, observation.state);
    EXPECT_EQ(0u, observation.startedBlockSequence);
    EXPECT_EQ(0u, observation.logicalCompletionSequence);
    EXPECT_EQ(0u, table.Resolve(record.token)->frontierLastReadSequence.load());
    harken::TimelineArmTable::ApplyAuthoredControlBucket(proof);
    ASSERT_TRUE(table.Observe(record.token, &observation));
    EXPECT_EQ(harken::ArmState::Published, observation.state);
    EXPECT_EQ(0u, observation.startedBlockSequence);
    EXPECT_EQ(0u, observation.logicalCompletionSequence);
    EXPECT_EQ(0u, observation.tombstoneAudioSequence);
    EXPECT_EQ(1u, table.Resolve(record.token)->frontierLastReadSequence.load());
    EXPECT_FALSE(record.reader.Release(2));
    proof.Release();
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Prepared, fence.descriptor.Access());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, record.operation.Cancel());
    ASSERT_TRUE(record.Prove(table, 256, 128, 1, 1, &proof));
    harken::TimelineArmTable::ApplyAuthoredControlBucket(proof);
    ASSERT_TRUE(table.Observe(record.token, &observation));
    EXPECT_EQ(harken::ArmState::Tombstoned, observation.state);
    EXPECT_EQ(0u, observation.startedBlockSequence);
    EXPECT_EQ(3u, observation.logicalCompletionSequence);
    EXPECT_EQ(3u, observation.tombstoneAudioSequence);
    EXPECT_EQ(0u, record.operation.AppliedAudioSequence());
    proof.Release();
    EXPECT_FALSE(record.reader.Release(3));
    EXPECT_FALSE(table.Reclaim(record.token, 4));
    EXPECT_TRUE(record.reader.Release(4));
    EXPECT_FALSE(table.Reclaim(record.token, 3));
    EXPECT_TRUE(table.Reclaim(record.token, 4));
    EXPECT_EQ(1u, table.FreeCount());
    fenceProof.Reset();
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, record.operation.Cancel());
    EXPECT_EQ(0u, record.operation.AppliedAudioSequence());
}

TEST(AuthoredStopAudio, ControlArmSameBucketRequiresTheCompleteOriginalPairExactlyOnce) {
    harken::TimelineArmTable table(1);
    OriginalControlRecord record(120, 100);
    ASSERT_TRUE(record.Install(table));
    harken::TimelineArmTable::AuthoredControlBucketProof proof;
    EXPECT_FALSE(record.Prove(table, 0, 128, 0, 1, &proof));
    EXPECT_FALSE(record.Prove(table, 0, 128, 1, 1, &proof));
    auto duplicate = record.events;
    duplicate[1] = duplicate[0];
    EXPECT_FALSE(table.ProveAuthoredControlBucket(record.token, record.reader, record.input.program,
        0, 128, 1, duplicate.data(), 2, &proof));
    auto reversed = record.events;
    std::swap(reversed[0], reversed[1]);
    ASSERT_TRUE(table.ProveAuthoredControlBucket(record.token, record.reader, record.input.program,
        0, 128, 1, reversed.data(), 2, &proof));
    harken::TimelineArmTable::AuthoredControlBucketProof other;
    EXPECT_FALSE(record.Prove(table, 0, 128, 0, 2, &other));
    EXPECT_FALSE(other.Valid());
    harken::TimelineArmTable::ApplyAuthoredControlBucket(proof);
    proof.Release();
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(table.Observe(record.token, &observation));
    EXPECT_EQ(0u, observation.startedBlockSequence);
    EXPECT_EQ(1u, observation.logicalCompletionSequence);
    EXPECT_EQ(0u, record.operation.AppliedAudioSequence());
    record.CancelAndReclaim(table);
}

TEST(AuthoredStopAudio, ControlArmAbortMoveAndDestructionRetainOriginalCancellationRights) {
    static_assert(!std::is_copy_constructible_v<harken::TimelineArmTable::AuthoredControlBucketProof>);
    static_assert(!std::is_copy_assignable_v<harken::TimelineArmTable::AuthoredControlBucketProof>);
    harken::TimelineArmTable firstTable(1), secondTable(1);
    OriginalControlRecord first, second(300, 100, true, 15);
    ASSERT_TRUE(first.Install(firstTable));
    ASSERT_TRUE(second.Install(secondTable));
    {
        harken::TimelineArmTable::AuthoredControlBucketProof left, right;
        ASSERT_TRUE(first.Prove(firstTable, 0, 128, 0, 1, &left));
        ASSERT_TRUE(second.Prove(secondTable, 0, 128, 0, 1, &right));
        right = std::move(left);
        EXPECT_FALSE(left.Valid());
        EXPECT_TRUE(right.Valid());
        EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, second.operation.Cancel());
        EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, first.operation.Cancel());
        harken::TimelineArmTable::AuthoredControlBucketProof moved(std::move(right));
        EXPECT_FALSE(right.Valid());
        EXPECT_TRUE(moved.Valid());
    }
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, first.operation.Cancel());
    EXPECT_EQ(0u, firstTable.Resolve(first.token)->frontierLastReadSequence.load());
    EXPECT_EQ(0u, secondTable.Resolve(second.token)->frontierLastReadSequence.load());
    EXPECT_EQ(0u, first.operation.AppliedAudioSequence());
    first.CancelAndReclaim(firstTable);
    second.CancelAndReclaim(secondTable);
}

TEST(AuthoredStopAudio, ControlArmRejectsForeignFullTokenReferenceAndCrossTableReader) {
    harken::TimelineArmTable table(1), foreignTable(1);
    OriginalControlRecord record, foreign;
    ASSERT_TRUE(record.Install(table));
    ASSERT_TRUE(foreign.Install(foreignTable));
    ASSERT_EQ(record.token.slot, foreign.token.slot);
    ASSERT_EQ(record.token.generation, foreign.token.generation);
    ASSERT_EQ(record.token.renderSlot, foreign.token.renderSlot);
    ASSERT_EQ(record.token.renderGeneration, foreign.token.renderGeneration);
    harken::TimelineArmTable::AuthoredControlBucketProof proof;
    EXPECT_FALSE(table.ProveAuthoredControlBucket(record.token, foreign.reader, record.input.program,
        0, 128, 1, record.events.data(), 1, &proof));
    harken::TimelineArmReaderPin absent;
    EXPECT_FALSE(table.ProveAuthoredControlBucket(record.token, absent, record.input.program,
        0, 128, 1, record.events.data(), 1, &proof));
    for (std::uint32_t field = 0; field < 4; ++field) {
        auto token = record.token;
        switch (field) {
        case 0: ++token.slot; break;
        case 1: ++token.generation; break;
        case 2: ++token.renderSlot; break;
        default: ++token.renderGeneration; break;
        }
        EXPECT_FALSE(table.ProveAuthoredControlBucket(token, record.reader, record.input.program,
            0, 128, 1, record.events.data(), 1, &proof));
    }
    auto copiedView = record.program;
    EXPECT_FALSE(table.ProveAuthoredControlBucket(record.token, record.reader, {5, &copiedView},
        0, 128, 1, record.events.data(), 1, &proof));
    EXPECT_FALSE(table.ProveAuthoredControlBucket(record.token, record.reader, {6, &record.program},
        0, 128, 1, record.events.data(), 1, &proof));
    EXPECT_FALSE(record.Prove(table, 1, 128, 0, 1, &proof));
    EXPECT_FALSE(table.ProveAuthoredControlBucket(record.token, record.reader, record.input.program,
        UINT64_MAX - 127, 128, 1, record.events.data(), 1, &proof));
    ASSERT_TRUE(record.Prove(table, 0, 128, 0, 1, &proof));
    proof.Release();
    record.CancelAndReclaim(table);
    foreign.CancelAndReclaim(foreignTable);
}

TEST(AuthoredStopAudio, ControlArmFreezesOriginalPayloadAndEventSemanticsBeforePublication) {
    harken::TimelineArmTable table(1);
    OriginalControlRecord record;
    ASSERT_TRUE(record.Install(table));
    harken::TimelineArmTable::AuthoredControlBucketProof proof;
    const auto originalPayload = record.payloads[0];
    for (std::uint32_t field = 0; field < 7; ++field) {
        record.payloads[0] = originalPayload;
        auto& identity = record.payloads[0].authoredStopOperation;
        switch (field) {
        case 0: ++identity.slot; break;
        case 1: ++identity.generation; break;
        case 2: ++identity.root; break;
        case 3: ++identity.sourceEpoch; break;
        case 4: ++identity.actionFiring; break;
        case 5: ++identity.executionOrdinal; break;
        default: ++identity.stopFrame; break;
        }
        EXPECT_FALSE(record.Prove(table, 0, 128, 0, 1, &proof));
    }
    for (std::uint32_t field = 0; field < 7; ++field) {
        record.payloads[0] = originalPayload;
        switch (field) {
        case 0: ++record.payloads[0].target.occurrenceOrdinal; break;
        case 1: ++record.payloads[0].envelopeStartFrame; break;
        case 2: ++record.payloads[0].envelopeEndFrame; break;
        case 3: record.payloads[0].kind = harken::TimelineEventKind::Start; break;
        case 4: record.payloads[0].reserved |= harken::kTimelineEventTransientStartArm; break;
        case 5: record.payloads[0].envelopeDirection = harken::TimelineEnvelopeDirection::FadeIn; break;
        default: record.payloads[0].authoredFadeOutCandidate = false; break;
        }
        EXPECT_FALSE(record.Prove(table, 0, 128, 0, 1, &proof));
    }
    record.payloads[0] = originalPayload;
    const auto originalEvent = record.events[0];
    for (std::uint32_t field = 0; field < 6; ++field) {
        record.events[0] = originalEvent;
        switch (field) {
        case 0: ++record.events[0].executeFrame; break;
        case 1: ++record.events[0].executionOrdinal; break;
        case 2: ++record.events[0].eventIndex; break;
        case 3: ++record.events[0].subeventPhase; break;
        case 4: ++record.events[0].payload; break;
        default: record.events[0].authoredFadeOutCandidate = false; break;
        }
        // Passing the changed original pointer cannot redefine factory authority.
        EXPECT_FALSE(record.Prove(table, 0, 128, 0, 1, &proof));
    }
    record.events[0] = originalEvent;
    ++record.program.generation;
    EXPECT_FALSE(record.Prove(table, 0, 128, 0, 1, &proof));
    --record.program.generation;
    ASSERT_TRUE(record.Prove(table, 0, 128, 0, 1, &proof));
    proof.Release();
    record.CancelAndReclaim(table);
}

TEST(AuthoredStopAudio, ControlArmDistinctPurposeRejectsGenericStartAndReusedRowsClearMetadata) {
    harken::TimelineArmTable table(1);
    OriginalControlRecord record;
    ASSERT_TRUE(record.Install(table));
    EXPECT_FALSE(table.Start(record.token, 1));
    EXPECT_FALSE(table.ClaimAudioSlice(record.token));
    table.ApplyClaimedAudioSlice(record.token);
    table.CompleteClaimedAudioSlice(record.token, 1);
    harken::TimelineArmTable::LaneAudioProof start;
    EXPECT_FALSE(table.ProveLaneStart(record.token, 100, 1, &start, 5, &record.target));
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(table.Observe(record.token, &observation));
    EXPECT_EQ(harken::ArmState::Published, observation.state);
    EXPECT_EQ(0u, observation.startedBlockSequence);
    auto* ownedRow = table.Resolve(record.token);
    ASSERT_NE(nullptr, ownedRow);
    record.CancelAndReclaim(table);
    EXPECT_EQ(nullptr, ownedRow->authoredControl.operation);
    harken::TimelineArmToken generic;
    ASSERT_TRUE(table.Reserve(9, 11, 100, 0, 5, record.target, &generic));
    ASSERT_EQ(record.token.slot, generic.slot);
    EXPECT_NE(record.token.generation, generic.generation);
    harken::TimelineArmReaderPin genericReader;
    ASSERT_TRUE(table.PinFrontierReader(generic, &genericReader));
    ASSERT_TRUE(table.AcknowledgeApplied(generic, 1));
    ASSERT_TRUE(table.Publish(generic));
    for (auto& event : record.events) event.armToken = generic;
    harken::TimelineArmTable::AuthoredControlBucketProof proof;
    EXPECT_FALSE(table.ProveAuthoredControlBucket(generic, genericReader, record.input.program,
        0, 128, 1, record.events.data(), 1, &proof));
    EXPECT_TRUE(table.Start(generic, 1));
    EXPECT_TRUE(table.RequestCancelStartedVoice(generic));
    EXPECT_TRUE(table.Tombstone(generic, 1));
    EXPECT_TRUE(genericReader.Release(2));
    EXPECT_TRUE(table.Reclaim(generic, 2));
}

TEST(AuthoredStopAudio, ControlArmPreparedCancelAndMaxGenerationQuarantineClearOriginalBacking) {
    harken::TimelineArmTable table(1);
    OriginalControlRecord first;
    ASSERT_TRUE(table.PrepareAuthoredControl(first.input, &first.token));
    auto* ownedRow = table.Resolve(first.token);
    ASSERT_NE(nullptr, ownedRow);
    ASSERT_TRUE(table.CancelPrepared(first.token));
    EXPECT_EQ(nullptr, ownedRow->authoredControl.operation);
    EXPECT_EQ(nullptr, ownedRow->authoredControl.events);
    // Test setup advances a Free slot before the actual factory issues its token.
    ownedRow->generation = UINT32_MAX;
    OriginalControlRecord last(100, 0, false);
    ASSERT_TRUE(last.Install(table));
    ASSERT_EQ(UINT32_MAX, last.token.generation);
    harken::TimelineArmTable::AuthoredControlBucketProof proof;
    ASSERT_TRUE(last.Prove(table, 0, 128, 0, 1, &proof));
    harken::TimelineArmTable::ApplyAuthoredControlBucket(proof);
    proof.Release();
    ASSERT_TRUE(last.reader.Release(2));
    EXPECT_FALSE(table.Reclaim(last.token, 2));
    EXPECT_EQ(harken::ArmState::Quarantined, ownedRow->state.load());
    EXPECT_EQ(nullptr, ownedRow->authoredControl.operation);
    EXPECT_EQ(nullptr, ownedRow->authoredControl.events);
    EXPECT_EQ(0u, table.FreeCount());
    EXPECT_EQ(0u, last.operation.AppliedAudioSequence());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, last.operation.Cancel());
}

TEST(AuthoredStopAudio, ControlArmOriginalOperationCancelRacePreservesOneOwnershipOutcome) {
    for (std::uint32_t iteration = 0; iteration < 64; ++iteration) {
        harken::TimelineArmTable table(1);
        OriginalControlRecord record;
        ASSERT_TRUE(record.Install(table));
        harken::TimelineArmTable::AuthoredControlBucketProof proof;
        std::atomic<bool> go{false};
        bool acquired = false;
        harken::AuthoredStopCancelDisposition cancelled{};
        std::thread claimant([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            acquired = record.Prove(table, 0, 128, 0, 1, &proof);
        });
        std::thread canceller([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            cancelled = record.operation.Cancel();
        });
        go.store(true, std::memory_order_release);
        claimant.join();
        canceller.join();
        EXPECT_EQ(acquired, proof.Valid());
        EXPECT_EQ(acquired ? harken::AuthoredStopCancelDisposition::DeferredApplying :
            harken::AuthoredStopCancelDisposition::Cancelled, cancelled);
        proof.Release();
        EXPECT_EQ(acquired ? harken::AuthoredStopCancelDisposition::Cancelled :
            harken::AuthoredStopCancelDisposition::AlreadyCancelled, record.operation.Cancel());
        EXPECT_EQ(0u, record.operation.AppliedAudioSequence());
        EXPECT_EQ(0u, table.Resolve(record.token)->frontierLastReadSequence.load());
        record.CancelAndReclaim(table);
    }
}


TEST(AuthoredStopAudio, ControlArmFactoryUsesActualAuthoredStopSchemaAndRejectsMalformedInputs) {
    harken::TimelineArmTable table(1);
    OriginalControlRecord record(120, 100);
    EXPECT_EQ(1u, record.payloads[0].envelopeIndex);
    EXPECT_TRUE(record.payloads[1].authoredFadeOutCandidate);
    EXPECT_TRUE(record.events[1].authoredFadeOutCandidate);
    harken::TimelineArmToken token;
    auto input = record.input;
    auto copiedView = record.program;
    copiedView.generation = 0;
    input.program.program = &copiedView;
    EXPECT_FALSE(table.PrepareAuthoredControl(input, &token));
    input = record.input;
    input.armDeadline = 101;
    EXPECT_FALSE(table.PrepareAuthoredControl(input, &token));
    input = record.input;
    input.eventCount = 3;
    EXPECT_FALSE(table.PrepareAuthoredControl(input, &token));
    input = record.input;
    input.onsetFrame = 121;
    EXPECT_FALSE(table.PrepareAuthoredControl(input, &token));
    record.payloads[0].envelopeIndex = 0;
    EXPECT_FALSE(table.PrepareAuthoredControl(record.input, &token));
    record.payloads[0].envelopeIndex = 1;
    record.payloads[1].authoredFadeOutCandidate = false;
    EXPECT_FALSE(table.PrepareAuthoredControl(record.input, &token));
    record.payloads[1].authoredFadeOutCandidate = true;
    record.events[1].authoredFadeOutCandidate = false;
    EXPECT_FALSE(table.PrepareAuthoredControl(record.input, &token));
    record.events[1].authoredFadeOutCandidate = true;
    EXPECT_FALSE(token.Valid());
    EXPECT_EQ(1u, table.FreeCount());
    ASSERT_TRUE(record.Install(table));
    EXPECT_FALSE(table.PrepareAuthoredControl(record.input, &record.token));
    EXPECT_EQ(0u, record.operation.AppliedAudioSequence());
    harken::TimelineArmTable::AuthoredControlBucketProof proof;
    ASSERT_TRUE(record.Prove(table, 0, 128, 0, 2, &proof));
    proof.Release();
    record.CancelAndReclaim(table);
    RecordProperty("arm_row_bytes", static_cast<int>(sizeof(harken::TimelineArmTable::Row)));
    RecordProperty("control_metadata_bytes", static_cast<int>(sizeof(harken::TimelineArmTable::AuthoredControlMetadata)));
}

TEST(AuthoredStopAudio, ControlArmOriginalCancelPreventsFactoryAndNoFadeStopHasNoCandidateFlag) {
    harken::TimelineArmTable table(1);
    OriginalControlRecord cancelled;
    ASSERT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, cancelled.operation.Cancel());
    harken::TimelineArmToken rejected;
    EXPECT_FALSE(table.PrepareAuthoredControl(cancelled.input, &rejected));
    EXPECT_FALSE(rejected.Valid());
    EXPECT_EQ(1u, table.FreeCount());
    OriginalControlRecord direct(100, 0, false);
    EXPECT_EQ(0u, direct.payloads[0].envelopeIndex);
    EXPECT_FALSE(direct.payloads[0].authoredFadeOutCandidate);
    EXPECT_FALSE(direct.events[0].authoredFadeOutCandidate);
    ASSERT_TRUE(direct.Install(table));
    harken::TimelineArmTable::AuthoredControlBucketProof proof;
    ASSERT_TRUE(direct.Prove(table, 0, 128, 0, 1, &proof));
    harken::TimelineArmTable::ApplyAuthoredControlBucket(proof);
    proof.Release();
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(table.Observe(direct.token, &observation));
    EXPECT_EQ(harken::ArmState::Tombstoned, observation.state);
    EXPECT_EQ(0u, observation.startedBlockSequence);
    EXPECT_EQ(1u, observation.logicalCompletionSequence);
    EXPECT_EQ(0u, direct.operation.AppliedAudioSequence());
    direct.CancelAndReclaim(table);
}

TEST(AuthoredStopAudio, ControlArmFactoryRefusesReplacingAnOwnedOutputEvenWithFreshBackingAndCapacity) {
    harken::TimelineArmTable table(2);
    OriginalControlRecord original, fresh(120, 100, true, 15);
    ASSERT_TRUE(original.Install(table));
    ASSERT_EQ(1u, table.FreeCount());
    const auto retained = original.token;
    ASSERT_FALSE(fresh.events[0].armToken.Valid());
    ASSERT_FALSE(fresh.events[1].armToken.Valid());
    EXPECT_FALSE(table.PrepareAuthoredControl(fresh.input, &original.token));
    EXPECT_EQ(retained.slot, original.token.slot);
    EXPECT_EQ(retained.generation, original.token.generation);
    EXPECT_EQ(retained.renderSlot, original.token.renderSlot);
    EXPECT_EQ(retained.renderGeneration, original.token.renderGeneration);
    EXPECT_EQ(1u, table.FreeCount());
    // The new canonical input really can reserve the remaining original slot.
    harken::TimelineArmToken newPrivate;
    ASSERT_TRUE(table.PrepareAuthoredControl(fresh.input, &newPrivate));
    EXPECT_NE(retained.slot, newPrivate.slot);
    EXPECT_EQ(0u, table.FreeCount());
    ASSERT_TRUE(table.CancelPrepared(newPrivate));
    EXPECT_EQ(1u, table.FreeCount());
    harken::TimelineArmTable::AuthoredControlBucketProof proof;
    ASSERT_TRUE(original.Prove(table, 0, 128, 0, 1, &proof));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, original.operation.Cancel());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, fresh.operation.Cancel());
    proof.Release();
    EXPECT_EQ(0u, original.operation.AppliedAudioSequence());
    ASSERT_TRUE(table.RequestCancel(original.token));
    ASSERT_TRUE(table.Tombstone(original.token, 1));
    ASSERT_TRUE(original.reader.Release(2));
    ASSERT_TRUE(table.Reclaim(original.token, 2));
    EXPECT_EQ(2u, table.FreeCount());
}


namespace {

harken::ConsumedEvent LaneControlInput(const harken::ScheduledEventInput& input,
    harken::TimelineTransactionProgramRef program) {
    harken::ConsumedEvent event{};
    event.executeFrame = input.executeFrame;
    event.logicCommitSequence = input.logicCommitSequence;
    event.executionOrdinal = input.executionOrdinal;
    event.eventIndex = input.eventIndex;
    event.transactionOrdinal = input.transactionOrdinal;
    event.subeventPhase = input.subeventPhase;
    event.payload = input.payload;
    event.authoredFadeOutCandidate = input.authoredFadeOutCandidate;
    event.armToken = input.armToken;
    event.frontier = input.frontier;
    event.program = program;
    return event;
}

struct ControlLaneAudioHarness {
    ControlLaneAudioHarness(std::uint32_t workers, std::uint64_t stopFrame = 32,
        std::uint64_t fadeStart = 8, bool envelope = true, bool gated = false)
        : audio(workers), render(audio.Play(gated)),
          record(stopFrame, fadeStart, envelope, static_cast<std::uint32_t>(render)),
          fence(record.operation), sink(audio.engine->voices, arms) {
        record.input.renderGeneration = static_cast<std::uint32_t>(render >> 32);
        record.input.onsetFrame = 0;
        if (envelope) record.events[0].executeFrame = fadeStart;
        EXPECT_TRUE(record.Install(arms));
        fence.payload.authoredStopOperation = record.operation.Identity();
        EXPECT_TRUE(lane.Reset({2, 4, 8, kBlock, true}));
        EXPECT_TRUE(sink.AttachImmediateLane(lane, kBlock));
        sink.SetArenaAdmission(UINT32_MAX, kBlock);
    }

    void InstallControl() {
        std::array<harken::ImmediateLaneEvent, 3> events{};
        for (std::uint32_t index = 0; index < record.input.eventCount; ++index) {
            events[index].timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
            events[index].fixedFrame = record.events[index].executeFrame;
            events[index].event = LaneControlInput(record.events[index], record.input.program);
        }
        auto& root = events[record.input.eventCount];
        root.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
        root.fixedFrame = record.operation.Identity().stopFrame;
        root.event = LaneControlInput(fence.event, fence.request.program);
        root.event.executionOrdinal = record.operation.Identity().executionOrdinal;
        root.event.eventIndex = 90;
        const auto count = record.input.eventCount + 1u;
        ASSERT_TRUE(lane.Reserve(17, count, &control));
        ASSERT_TRUE(lane.Prepare(control, events.data(), count));
        ASSERT_TRUE(lane.PinArms(control, arms));
        std::uint64_t anchor = UINT64_MAX;
        ASSERT_TRUE(lane.TryPlace(control, 0, 0, &anchor));
        ASSERT_TRUE(lane.Publish(control));
    }

    void InstallStart(std::uint64_t frame) {
        ASSERT_TRUE(arms.PreparePendingImmediate(static_cast<std::uint32_t>(render),
            static_cast<std::uint32_t>(render >> 32), 1, {}, &startArm));
        ASSERT_TRUE(arms.CommitPrepared(startArm));
        ASSERT_TRUE(arms.AcknowledgeApplied(startArm, 1));
        ASSERT_TRUE(arms.BindPendingImmediateStart(startArm, frame, frame));
        startProgram = {11, &startPayload, 1};
        harken::ImmediateLaneEvent event{};
        event.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
        event.fixedFrame = frame;
        event.event.executeFrame = frame;
        event.event.program = {11, &startProgram};
        event.event.armToken = startArm;
        event.event.subeventPhase = 20;
        ASSERT_TRUE(lane.Reserve(17, 1, &start));
        ASSERT_TRUE(lane.Prepare(start, &event, 1));
        ASSERT_TRUE(lane.PinArms(start, arms));
        std::uint64_t anchor = UINT64_MAX;
        ASSERT_TRUE(lane.TryPlace(start, 0, 0, &anchor));
        ASSERT_TRUE(arms.Publish(startArm));
        ASSERT_TRUE(lane.Publish(start));
    }

    void Step() {
        ASSERT_TRUE(sink.BeginQuantum(audio.quantum));
        sink.EndQuantum(audio.quantum);
        audio.Render();
    }

    void Retire(bool startPresent = false, std::uint32_t expectedFree = 3) {
        EXPECT_FALSE(lane.Retire(control, audio.quantum));
        audio.Begin();
        audio.Render();
        EXPECT_TRUE(lane.Retire(control, audio.quantum));
        if (startPresent) EXPECT_TRUE(lane.Retire(start, audio.quantum));
        EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, record.operation.Cancel());
        harken::ArmState state{};
        ASSERT_TRUE(arms.State(record.token, &state));
        if (state != harken::ArmState::Tombstoned) {
            EXPECT_TRUE(arms.RequestCancel(record.token));
            EXPECT_TRUE(arms.Tombstone(record.token, audio.quantum));
            audio.Begin(); audio.Render();
        }
        EXPECT_TRUE(record.reader.Release(audio.quantum));
        EXPECT_TRUE(arms.Reclaim(record.token, audio.quantum));
        if (startPresent) {
            EXPECT_TRUE(arms.RequestCancelStartedVoice(startArm));
            EXPECT_TRUE(arms.Tombstone(startArm, audio.quantum));
            audio.Begin(); audio.Render();
            EXPECT_TRUE(arms.Reclaim(startArm, audio.quantum));
        }
        for (int blocks = 0; blocks < 16; ++blocks) { audio.Begin(); audio.Render(); }
        EXPECT_TRUE(audio.engine->voices.ReleaseObserved(audio.lastObserver));
        EXPECT_EQ(expectedFree, arms.FreeCount());
    }

    StopAudioHarness audio;
    const VnVoiceHandle render;
    OriginalControlRecord record;
    FenceRecord fence;
    harken::TimelineArmTable arms{3};
    harken::TimelineEventPayload startPayload{};
    harken::TimelineProgramView startProgram{};
    harken::TimelineArmToken startArm{};
    harken::ImmediateActionLane lane;
    harken::ImmediateLaneHandle control{}, start{};
    harken::VoiceTimelineSink sink;
};

} // namespace

TEST(AuthoredStopAudio, TypedLaneControlRendersOriginalFadeAndFenceAtExactFixedFrame) {
    for (const auto workers : {0u, 2u}) {
        for (const auto stopFrame : {32u, 100u}) {
            SCOPED_TRACE(workers);
            SCOPED_TRACE(stopFrame);
            ControlLaneAudioHarness h(workers, stopFrame);
            h.InstallControl();
            EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, h.fence.descriptor.Access());
            EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, h.record.operation.Cancel());
            const auto allocations = harken::memguard::HitCount();
            for (std::uint32_t block = 0; block <= stopFrame / kBlock; ++block) {
                { harken::memguard::AudioThreadScope scope; h.Step(); }
                EXPECT_EQ(allocations, harken::memguard::HitCount());
                for (std::uint32_t sample = 0; sample < kBlock; ++sample) {
                    const auto frame = block * kBlock + sample;
                    const auto gain = frame < 8 ? 1.0f : frame < stopFrame
                        ? static_cast<float>(stopFrame - 1u - frame) / static_cast<float>(stopFrame - 9) : 0.0f;
                    EXPECT_NEAR(0.25f * gain, h.audio.Sample(sample), 1e-6f) << frame;
                }
                EXPECT_EQ(block == stopFrame / kBlock ? block + 1u : 0u,
                    h.record.operation.AppliedAudioSequence());
            }
            harken::TimelineArmObservation observation{};
            ASSERT_TRUE(h.arms.Observe(h.record.token, &observation));
            EXPECT_EQ(0u, observation.startedBlockSequence);
            EXPECT_EQ(h.audio.quantum, observation.logicalCompletionSequence);
            EXPECT_EQ(harken::AuthoredStopFenceAccess::Done, h.fence.descriptor.Access());
            h.Retire();
        }
    }
}

TEST(AuthoredStopAudio, TypedLaneStopDominatesAnotherOriginalLaneStartAtSameFrameInBothOrders) {
    for (const auto workers : {0u, 2u}) {
        for (const bool stopFirst : {false, true}) {
            ControlLaneAudioHarness h(workers, 32, 8, false, true);
            if (stopFirst) { h.InstallControl(); h.InstallStart(32); }
            else { h.InstallStart(32); h.InstallControl(); }
            h.Step();
            const auto* context = h.audio.engine->voices.Resolve(h.render);
            if (context != nullptr) EXPECT_EQ(0u, context->srcCursor);
            for (std::uint32_t sample = 0; sample < kBlock; ++sample)
                EXPECT_EQ(0.0f, h.audio.Sample(sample));
            EXPECT_EQ(1u, h.record.operation.AppliedAudioSequence());
            h.Retire(true);
        }
    }
}

TEST(AuthoredStopAudio, TypedLaneStopRefusalReturnsEveryProofWithoutChangingEarlierStartGuard) {
    for (const auto mode : {0, 1, 2}) {
        SCOPED_TRACE(mode);
        ControlLaneAudioHarness h(0, 32, 8, false, true);
        h.InstallStart(32);
        h.InstallControl();
        if (mode == 0) ASSERT_TRUE(h.arms.RequestCancel(h.record.token));
        if (mode == 1) ASSERT_TRUE(h.lane.Cancel(h.control));
        ASSERT_TRUE(h.sink.BeginQuantum(0));
        if (mode == 2) {
            harken::VoiceTimelineReservation occupied{};
            ASSERT_TRUE(h.audio.engine->voices.ReserveLaneTimelineEvents(
                static_cast<std::uint32_t>(h.render), static_cast<std::uint32_t>(h.render >> 32),
                0, 7, &occupied));
            ASSERT_TRUE(h.audio.engine->voices.RejectTimelineReservation(occupied));
        }
        h.sink.EndQuantum(0);
        const auto* context = h.audio.engine->voices.Resolve(h.render);
        ASSERT_NE(nullptr, context);
        bool foundStart = false;
        for (const auto& event : context->timelineEvents.events) {
            if (event.kind == harken::TimelineEventKind::Start &&
                event.frontierProof.state == harken::FrontierEventState::LaneApplied) {
                foundStart = true;
                EXPECT_EQ(UINT64_MAX, event.frontierProof.cutoffFrame);
                EXPECT_FALSE(event.frontierProof.cutoffOperation.Valid());
            }
        }
        EXPECT_TRUE(foundStart);
        EXPECT_EQ(0u, h.record.operation.AppliedAudioSequence());
        EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, h.fence.descriptor.Access());
        harken::ImmediateLaneObservation observation{};
        ASSERT_TRUE(h.lane.Observe(h.control, &observation));
        EXPECT_FALSE(observation.applied);
        h.audio.Render();
        for (std::uint32_t sample = 0; sample < kBlock; ++sample)
            EXPECT_NEAR(sample < 32 ? 0.0f : 0.25f, h.audio.Sample(sample), 1e-6f);
        ASSERT_TRUE(h.lane.Cancel(h.control));
        h.audio.Begin(); h.audio.Render();
        ASSERT_TRUE(h.lane.Retire(h.control, 2));
        ASSERT_TRUE(h.lane.Retire(h.start, 2));
        EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.record.operation.Cancel());
        ASSERT_TRUE(h.arms.RequestCancel(h.record.token));
        ASSERT_TRUE(h.arms.Tombstone(h.record.token, h.audio.quantum));
        h.audio.Begin(); h.audio.Render();
        ASSERT_TRUE(h.record.reader.Release(h.audio.quantum));
        ASSERT_TRUE(h.arms.Reclaim(h.record.token, h.audio.quantum));
        ASSERT_TRUE(h.arms.RequestCancelStartedVoice(h.startArm));
        ASSERT_TRUE(h.arms.Tombstone(h.startArm, h.audio.quantum));
        h.audio.Begin(); h.audio.Render();
        ASSERT_TRUE(h.arms.Reclaim(h.startArm, h.audio.quantum));
        EXPECT_EQ(3u, h.arms.FreeCount());
        h.audio.engine->voices.ApplyStop(h.render, 0, 0,
            h.audio.engine->notifications, h.audio.quantum);
        h.audio.Begin(); h.audio.Render();
        EXPECT_TRUE(h.audio.engine->voices.ReleaseObserved(h.audio.lastObserver));
    }
}

TEST(AuthoredStopAudio, FinalizedCurrentSpanMergesOnlyTrueEligibleCutoffAcrossPageAndLaneReservations) {
    for (const bool laneFirst : {false, true}) {
        for (const bool rejectStop : {false, true}) {
            StopAudioHarness h(0);
            const auto render = h.Play();
            h.Begin();
            auto& voices = h.engine->voices;
            const auto slot = static_cast<std::uint32_t>(render);
            const auto generation = static_cast<std::uint32_t>(render >> 32);
            const harken::AuthoredStopOperationIdentity identity{0, 1, 17, 19, 23, 7, 32};
            harken::VoiceTimelineReservation page{}, lane{};
            const auto reservePage = [&] {
                EXPECT_TRUE(voices.ReserveTimelineEvents(slot, generation, 0, 1, &page));
                auto event = WithFrontierProof(StartAt(32), render, 0, harken::FrontierEventState::Pending);
                EXPECT_TRUE(voices.StageReservedTimelineEvent(page, 0, event));
                harken::VoiceTimelineReservationProof proof;
                EXPECT_TRUE(voices.ProveTimelineReservation(page, &proof));
                voices.ApplyProvenTimelineReservation(proof, UINT64_MAX);
            };
            const auto reserveLane = [&] {
                EXPECT_TRUE(voices.ReserveLaneTimelineEvents(slot, generation, 0, 1, &lane));
                auto event = StopAt(32);
                event.order.executeFrame = 32;
                event.authoredStopOperation = identity;
                event.frontierProof.state = harken::FrontierEventState::Pending;
                event.frontierProof.quantum = 0;
                event.frontierProof.renderGeneration = generation;
                EXPECT_TRUE(voices.StageReservedLaneTimelineEvent(lane, 0, event));
                harken::VoiceTimelineReservationProof proof;
                EXPECT_TRUE(voices.ProveTimelineReservation(lane, &proof));
                if (rejectStop) {
                    harken::VoicePool::ReleaseTimelineReservationProof(proof);
                    EXPECT_TRUE(voices.RejectTimelineReservation(lane));
                } else voices.ApplyProvenLaneTimelineReservation(proof);
            };
            if (laneFirst) { reserveLane(); reservePage(); }
            else { reservePage(); reserveLane(); }
            voices.FinalizeTimelineQuantum(0);
            const auto* context = voices.Resolve(render);
            ASSERT_NE(nullptr, context);
            for (std::uint32_t index = 0; index < context->timelineEvents.eventCount; ++index) {
                const auto& event = context->timelineEvents.events[index];
                if (event.kind != harken::TimelineEventKind::Start) continue;
                EXPECT_EQ(rejectStop ? UINT64_MAX : 32u, event.frontierProof.cutoffFrame);
                EXPECT_EQ(rejectStop ? harken::AuthoredStopOperationIdentity{} : identity,
                    event.frontierProof.cutoffOperation);
            }
            EXPECT_EQ(UINT64_MAX, context->authoredRootStopFrame);
            h.Render();
            for (std::uint32_t sample = 0; sample < kBlock; ++sample)
                EXPECT_NEAR(rejectStop && sample >= 32 ? 0.25f : 0.0f, h.Sample(sample), 1e-6f);
        }
    }
}

TEST(AuthoredStopAudio, TypedLaneStopDominatesStartFromActualFixedPageExecutor) {
    for (const auto workers : {0u, 2u}) {
        ControlLaneAudioHarness h(workers, 32, 8, false, true);
        h.InstallControl();
        ASSERT_TRUE(h.arms.Prepare(static_cast<std::uint32_t>(h.render),
            static_cast<std::uint32_t>(h.render >> 32), 32, 32, 11, {}, &h.startArm));
        ASSERT_TRUE(h.arms.CommitPrepared(h.startArm));
        ASSERT_TRUE(h.arms.AcknowledgeApplied(h.startArm, 1));
        ASSERT_TRUE(h.arms.Publish(h.startArm));
        h.startProgram = {11, &h.startPayload, 1};
        harken::ScheduledEventInput event{};
        event.executeFrame = 32;
        event.armToken = h.startArm;
        event.subeventPhase = 20;
        harken::TransactionRequest request{};
        request.transportEpoch = 1;
        request.program = {11, &h.startProgram};
        request.armToken = h.startArm;
        request.events = &event;
        request.eventCount = 1;
        harken::ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(FenceConfig(4), 0, 1));
        harken::PreparedTransactionBatch batch;
        ASSERT_EQ(harken::ReserveRefusal::None, pool.PrepareTransactionBatch(&request, 1, 0, &batch));
        ASSERT_TRUE(pool.CommitPreparedTransactionBatch(&batch, 1));
        pool.Publish(128);
        harken::TimelineExecutor executor;
        ASSERT_TRUE(executor.Reset(kBlock, 8, &pool, &h.arms, &h.sink));
        executor.ExecuteQuantum(0);
        EXPECT_EQ(0u, executor.Stats().protocolViolation);
        EXPECT_EQ(0u, executor.Stats().armFailedTransactions);
        h.audio.Render();
        for (std::uint32_t sample = 0; sample < kBlock; ++sample)
            EXPECT_EQ(0.0f, h.audio.Sample(sample));
        EXPECT_EQ(1u, h.record.operation.AppliedAudioSequence());
        h.Retire(false, 2);
        ASSERT_TRUE(h.arms.RequestCancelStartedVoice(h.startArm));
        ASSERT_TRUE(h.arms.Tombstone(h.startArm, h.audio.quantum));
        h.audio.Begin(); h.audio.Render();
        ASSERT_TRUE(h.arms.Reclaim(h.startArm, h.audio.quantum));
        EXPECT_EQ(3u, h.arms.FreeCount());
    }
}

namespace {

struct PlainFenceWithdrawalHarness {
    explicit PlainFenceWithdrawalHarness(std::uint64_t secondFrame = 32)
        : first({0, 1, 17, 19, 23, 7, 16}),
          second({1, 1, 17, 19, 23, 8, secondFrame}),
          a(first), b(second), sink(audio.engine->voices, arms) {
        a.payload.authoredStopOperation = first.Identity();
        b.payload.authoredStopOperation = second.Identity();
        EXPECT_TRUE(lane.Reset({2, 4, 8, kBlock, true}));
        EXPECT_TRUE(sink.AttachImmediateLane(lane, kBlock));
        sink.SetArenaAdmission(UINT32_MAX, kBlock);
    }

    void Install(std::initializer_list<FenceRecord*> originals,
        harken::ImmediateLaneHandle* handle, bool publish = true) {
        std::array<harken::ImmediateLaneEvent, 4> events{};
        std::uint32_t count = 0;
        for (const auto* original : originals) {
            auto& event = events[count++];
            event.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
            event.fixedFrame = original->event.executeFrame;
            event.event = LaneControlInput(original->event, original->request.program);
        }
        ASSERT_TRUE(lane.Reserve(17, {count, 0, 0, count}, handle));
        ASSERT_TRUE(lane.Prepare(*handle, events.data(), count));
        ASSERT_TRUE(lane.PinArms(*handle, arms));
        std::uint64_t anchor = UINT64_MAX;
        ASSERT_TRUE(lane.TryPlace(*handle, 0, 0, &anchor));
        if (publish) ASSERT_TRUE(lane.Publish(*handle));
    }

    bool Withdraw(harken::ImmediateLaneHandle handle, FenceRecord& original) {
        return lane.WithdrawPlainNoneFence(handle, 17, original.request.program,
            original.descriptor, audio.quantum);
    }

    void Step() {
        ASSERT_TRUE(sink.BeginQuantum(audio.quantum));
        sink.EndQuantum(audio.quantum);
        audio.Render();
    }

    StopAudioHarness audio{0};
    harken::AuthoredStopOperation first, second;
    FenceRecord a, b;
    harken::TimelineArmTable arms{1};
    harken::ImmediateActionLane lane;
    harken::VoiceTimelineSink sink;
};

} // namespace

TEST(AuthoredStopAudio, PlainNoneFenceWithdrawalPreservesSameBucketSiblingApplication) {
    PlainFenceWithdrawalHarness h;
    harken::ImmediateLaneHandle handle{};
    h.Install({&h.a, &h.b}, &handle);
    ASSERT_TRUE(h.Withdraw(handle, h.a));
    EXPECT_FALSE(h.Withdraw(handle, h.a));
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, h.a.descriptor.Access());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.first.Cancel());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, h.second.Cancel());
    const auto allocations = harken::memguard::HitCount();
    { harken::memguard::AudioThreadScope scope; h.Step(); }
    EXPECT_EQ(allocations, harken::memguard::HitCount());
    EXPECT_EQ(0u, h.first.AppliedAudioSequence());
    EXPECT_EQ(1u, h.second.AppliedAudioSequence());
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(h.lane.Observe(handle, &observation));
    EXPECT_TRUE(observation.applied);
    EXPECT_FALSE(observation.missed);
    EXPECT_FALSE(h.lane.Retire(handle, h.audio.quantum));
    h.Step();
    ASSERT_TRUE(h.lane.Retire(handle, h.audio.quantum));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.second.Cancel());
}

TEST(AuthoredStopAudio, PlainNoneFenceWithdrawalDoesNotDisarmAnotherActualLaneReference) {
    PlainFenceWithdrawalHarness h;
    harken::ImmediateLaneHandle left{}, right{};
    h.Install({&h.a}, &left);
    h.Install({&h.a, &h.b}, &right);
    ASSERT_TRUE(h.Withdraw(left, h.a));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, h.first.Cancel());
    h.Step();
    EXPECT_EQ(1u, h.first.AppliedAudioSequence());
    EXPECT_EQ(1u, h.second.AppliedAudioSequence());
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(h.lane.Observe(left, &observation));
    EXPECT_TRUE(observation.cancelledOnly);
    EXPECT_FALSE(observation.missed);
    ASSERT_TRUE(h.lane.Retire(left, h.audio.quantum));
    EXPECT_FALSE(h.lane.Retire(right, h.audio.quantum));
    h.Step();
    ASSERT_TRUE(h.lane.Retire(right, h.audio.quantum));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.first.Cancel());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.second.Cancel());
}

namespace {

void CheckPlainNoneFenceArenaDiscard(bool lateRequest, bool requestWithdrawal = true) {
    PlainFenceWithdrawalHarness h(kBlock + 16);
    auto original = std::make_unique<FenceRecord>(h.first);
    original->payload.authoredStopOperation = h.first.Identity();
    harken::ImmediateLaneHandle handle{};
    h.Install({original.get(), &h.b}, &handle);
    if (requestWithdrawal && !lateRequest) {
        ASSERT_TRUE(h.Withdraw(handle, *original));
        original.reset();
    }
    const auto render = h.audio.Play(false);
    OriginalControlRecord control(32, 8, false, static_cast<std::uint32_t>(render));
    control.input.renderGeneration = static_cast<std::uint32_t>(render >> 32);
    control.input.onsetFrame = 0;
    ASSERT_TRUE(control.Install(h.arms));
    harken::ImmediateLaneEvent controlEvent{};
    controlEvent.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    controlEvent.fixedFrame = 32;
    controlEvent.event = LaneControlInput(control.events[0], control.input.program);
    harken::ImmediateLaneHandle controlLane{};
    ASSERT_TRUE(h.lane.Reserve(17, {1, 0, 0, 1}, &controlLane));
    ASSERT_TRUE(h.lane.Prepare(controlLane, &controlEvent, 1));
    ASSERT_TRUE(h.lane.PinArms(controlLane, h.arms));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(h.lane.TryPlace(controlLane, 0, 0, &anchor));
    ASSERT_TRUE(h.lane.Publish(controlLane));
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(kBlock, 8, &h.audio.engine->timelinePool, &h.arms, &h.sink));
    harken::TimelineExecutionPlan plan{};
    plan.expectedFrontier = h.audio.engine->timelinePool.ConsumerFrontier().load();
    h.sink.SetArenaAdmission(0, kBlock);
    if (lateRequest) {
        ASSERT_TRUE(h.sink.CaptureIndependentWork(0));
        if (requestWithdrawal) EXPECT_FALSE(h.Withdraw(handle, *original));
    }
    executor.ExecuteQuantum(0, plan);
    if (requestWithdrawal && lateRequest) {
        ASSERT_TRUE(h.Withdraw(handle, *original));
        original.reset();
    }
    EXPECT_EQ(1u, executor.Stats().renderUnavailableDeferrals);
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(h.lane.Observe(handle, &observation));
    EXPECT_FALSE(observation.missed);
    EXPECT_EQ(requestWithdrawal, observation.cancelledOnly);
    harken::BlockArena empty(0);
    h.audio.RenderWith(empty);
    ASSERT_TRUE(h.lane.Cancel(controlLane));
    ASSERT_TRUE(h.lane.Retire(controlLane, h.audio.quantum));
    ASSERT_TRUE(h.arms.RequestCancel(control.token));
    ASSERT_TRUE(h.arms.Tombstone(control.token, h.audio.quantum));
    h.sink.SetArenaAdmission(UINT32_MAX, kBlock);
    h.Step();
    ASSERT_TRUE(control.reader.Release(h.audio.quantum));
    ASSERT_TRUE(h.arms.Reclaim(control.token, h.audio.quantum));
    EXPECT_EQ(0u, control.operation.AppliedAudioSequence());
    EXPECT_EQ(0u, h.first.AppliedAudioSequence());
    EXPECT_EQ(requestWithdrawal ? 2u : 0u, h.second.AppliedAudioSequence());
    for (std::uint32_t sample = 0; sample < kBlock; ++sample)
        EXPECT_NEAR(0.25f, h.audio.Sample(sample), 1e-6f);
    ASSERT_TRUE(h.lane.Observe(handle, &observation));
    EXPECT_EQ(!requestWithdrawal, observation.missed);
    if (requestWithdrawal)
        EXPECT_FALSE(h.lane.Retire(handle, h.audio.quantum));
    h.Step();
    ASSERT_TRUE(h.lane.Retire(handle, h.audio.quantum));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.first.Cancel());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.second.Cancel());
    h.audio.engine->voices.ApplyStop(render, 0, 0, h.audio.engine->notifications, h.audio.quantum);
    for (std::uint32_t block = 0; block < 16; ++block) { h.audio.Begin(); h.audio.Render(); }
    EXPECT_TRUE(h.audio.engine->voices.ReleaseObserved(h.audio.lastObserver));
}

} // namespace

TEST(AuthoredStopAudio, PlainNoneFenceWithdrawalArenaDiscardFinishesSkippedBucketAndKeepsFutureSibling) {
    CheckPlainNoneFenceArenaDiscard(false);
}

TEST(AuthoredStopAudio, PlainNoneFenceWithdrawalLateRequestThenActualArenaDiscardKeepsFutureSibling) {
    CheckPlainNoneFenceArenaDiscard(true);
}

TEST(AuthoredStopAudio, PlainNoneFenceWithdrawalUnrequestedArenaSurvivorRetainsCapacityFailureContract) {
    CheckPlainNoneFenceArenaDiscard(true, false);
}

TEST(AuthoredStopAudio, PlainNoneFenceWithdrawalCaptureWinnerKeepsCurrentProofUntilNewCompletion) {
    PlainFenceWithdrawalHarness h;
    harken::ImmediateLaneHandle handle{};
    h.Install({&h.a, &h.b}, &handle);
    ASSERT_TRUE(h.sink.BeginQuantum(0));
    EXPECT_FALSE(h.Withdraw(handle, h.a));
    EXPECT_TRUE(h.lane.HasProgramReference(1));
    h.sink.EndQuantum(0);
    h.audio.Render();
    EXPECT_EQ(1u, h.first.AppliedAudioSequence());
    EXPECT_EQ(1u, h.second.AppliedAudioSequence());
    EXPECT_FALSE(h.Withdraw(handle, h.a));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, h.first.Cancel());
    h.Step();
    ASSERT_TRUE(h.Withdraw(handle, h.a));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.first.Cancel());
    ASSERT_TRUE(h.lane.Retire(handle, h.audio.quantum));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.second.Cancel());
}

TEST(AuthoredStopAudio, PlainNoneFenceWithdrawalRefusedClaimRetainsTheActualPreparedProofCapture) {
    PlainFenceWithdrawalHarness h;
    harken::ImmediateLaneHandle handle{};
    h.Install({&h.a}, &handle);
    const auto* bucket = h.lane.CaptureDue(handle.slot, 0);
    ASSERT_NE(nullptr, bucket);
    ASSERT_EQ(&h.a.descriptor, h.lane.CapturedNoneFence(handle, bucket, bucket->firstEvent));
    harken::AuthoredStopFenceLaneProof proof;
    ASSERT_TRUE(h.a.descriptor.ProveLaneApply(h.a.request.program, &proof));
    EXPECT_FALSE(h.Withdraw(handle, h.a));
    ASSERT_TRUE(h.lane.Cancel(handle));
    EXPECT_FALSE(h.lane.ClaimRetainingCaptureOnRefusal(handle, bucket));
    EXPECT_EQ(nullptr, h.lane.CapturedNoneFence(handle, bucket, bucket->firstEvent));
    EXPECT_TRUE(h.lane.IsEventAdmitted(handle.slot, bucket, bucket->firstEvent));
    EXPECT_TRUE(proof.Valid());
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Prepared, h.a.descriptor.Access());
    EXPECT_FALSE(h.lane.Retire(handle, h.audio.quantum));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, h.first.Cancel());
    proof.Reset();
    ASSERT_TRUE(h.lane.ReleaseCapture(handle.slot, bucket));
    h.audio.Begin(); h.audio.Render();
    EXPECT_FALSE(h.lane.Retire(handle, h.audio.quantum));
    h.audio.Begin(); h.audio.Render();
    ASSERT_TRUE(h.lane.Retire(handle, h.audio.quantum));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.first.Cancel());
    EXPECT_EQ(0u, h.first.AppliedAudioSequence());
}

TEST(AuthoredStopAudio, PlainNoneFenceWithdrawalRejectsForeignBackingIdentityAndUnpublishedAttempt) {
    PlainFenceWithdrawalHarness h;
    harken::ImmediateLaneHandle handle{};
    h.Install({&h.a}, &handle, false);
    EXPECT_FALSE(h.Withdraw(handle, h.a));
    ASSERT_TRUE(h.lane.Publish(handle));
    auto stale = handle; ++stale.generation;
    EXPECT_FALSE(h.Withdraw(stale, h.a));
    EXPECT_FALSE(h.lane.WithdrawPlainNoneFence(handle, 18, h.a.request.program,
        h.a.descriptor, h.audio.quantum));
    EXPECT_FALSE(h.lane.WithdrawPlainNoneFence(handle, 17, h.a.request.program,
        h.b.descriptor, h.audio.quantum));
    auto copy = h.a.program;
    EXPECT_FALSE(h.lane.WithdrawPlainNoneFence(handle, 17, {1, &copy},
        h.a.descriptor, h.audio.quantum));
    auto payloadCopy = h.a.payload;
    h.a.program.payloads = &payloadCopy;
    EXPECT_FALSE(h.Withdraw(handle, h.a));
    h.a.program.payloads = &h.a.payload;
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, h.first.Cancel());
    ASSERT_TRUE(h.Withdraw(handle, h.a));
    h.Step();
    ASSERT_TRUE(h.lane.Retire(handle, h.audio.quantum));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.first.Cancel());
}

TEST(AuthoredStopAudio, PlainNoneFenceWithdrawalReusedActualLaneClearsAdmissionBytes) {
    PlainFenceWithdrawalHarness h(kBlock + 16);
    harken::ImmediateLaneHandle handle{};
    h.Install({&h.a}, &handle);
    ASSERT_TRUE(h.Withdraw(handle, h.a));
    EXPECT_FALSE(h.lane.HasProgramReference(1));
    h.Step();
    ASSERT_TRUE(h.lane.Retire(handle, h.audio.quantum));
    const auto old = handle;
    handle = {};
    h.Install({&h.b}, &handle);
    ASSERT_EQ(old.slot, handle.slot);
    ASSERT_NE(old.generation, handle.generation);
    EXPECT_FALSE(h.Withdraw(old, h.b));
    EXPECT_TRUE(h.lane.HasProgramReference(1));
    h.Step();
    EXPECT_EQ(2u, h.second.AppliedAudioSequence());
    h.Step();
    ASSERT_TRUE(h.lane.Retire(handle, h.audio.quantum));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.first.Cancel());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.second.Cancel());
}

TEST(AuthoredStopAudio, PlainNoneFenceWithdrawalLeavesIndependentPreparedDescriptorProofOwned) {
    PlainFenceWithdrawalHarness h;
    harken::ImmediateLaneHandle handle{};
    h.Install({&h.a}, &handle);
    harken::AuthoredStopFenceLaneProof proof;
    ASSERT_TRUE(h.a.descriptor.ProveLaneApply(h.a.request.program, &proof));
    ASSERT_TRUE(h.Withdraw(handle, h.a));
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Prepared, h.a.descriptor.Access());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, h.first.Cancel());
    proof.Reset();
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, h.a.descriptor.Access());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.first.Cancel());
    h.Step();
    ASSERT_TRUE(h.lane.Retire(handle, h.audio.quantum));
    EXPECT_EQ(0u, h.first.AppliedAudioSequence());
}

TEST(AuthoredStopAudio, PlainNoneFenceWithdrawalRequestWinsWhileActualCaptureGateIsHeld) {
    PlainFenceWithdrawalHarness h;
    harken::ImmediateLaneHandle handle{};
    h.Install({&h.a, &h.b}, &handle);
    struct Race { std::atomic<bool> entered{false}, proceed{false}; } race;
    h.lane.SetCaptureGateHookForTest([](void* context) {
        auto& state = *static_cast<Race*>(context);
        state.entered.store(true, std::memory_order_release);
        while (!state.proceed.load(std::memory_order_acquire)) std::this_thread::yield();
    }, &race);
    std::thread capture([&] { EXPECT_TRUE(h.sink.BeginQuantum(0)); h.sink.EndQuantum(0); });
    while (!race.entered.load(std::memory_order_acquire)) std::this_thread::yield();
    EXPECT_TRUE(h.Withdraw(handle, h.a));
    race.proceed.store(true, std::memory_order_release);
    capture.join();
    h.lane.SetCaptureGateHookForTest(nullptr, nullptr);
    h.audio.Render();
    EXPECT_EQ(0u, h.first.AppliedAudioSequence());
    EXPECT_EQ(1u, h.second.AppliedAudioSequence());
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(h.lane.Observe(handle, &observation));
    EXPECT_FALSE(observation.missed);
    h.Step();
    ASSERT_TRUE(h.lane.Retire(handle, h.audio.quantum));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.first.Cancel());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.second.Cancel());
}

TEST(AuthoredStopAudio, NoneFenceFutureReferenceOwnsOnlyOriginalReaderAcrossMovesAndAbortPrefixes) {
    static_assert(!std::is_copy_constructible_v<harken::AuthoredStopFenceLaneReference>);
    harken::AuthoredStopOperation first({0, 1, 17, 19, 23, 7, 32});
    harken::AuthoredStopOperation second({1, 1, 17, 19, 23, 7, 32});
    FenceRecord a(first), b(second);
    a.payload.authoredStopOperation = first.Identity();
    b.payload.authoredStopOperation = second.Identity();
    harken::AuthoredStopFenceLaneReference x, y;
    ASSERT_TRUE(a.descriptor.AcquireLaneReference(a.request.program, &x));
    ASSERT_TRUE(b.descriptor.AcquireLaneReference(b.request.program, &y));
    EXPECT_FALSE(a.descriptor.AcquireLaneReference(a.request.program, &x));
    auto moved = std::move(x);
    EXPECT_FALSE(x.Valid());
    moved = std::move(y);
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, first.Cancel());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, second.Cancel());
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, b.descriptor.Access());
    EXPECT_EQ(0u, second.AppliedAudioSequence());
    moved.Reset(); moved.Reset();
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, second.Cancel());
    EXPECT_FALSE(b.descriptor.AcquireLaneReference(b.request.program, &x));
}

TEST(AuthoredStopAudio, NoneFenceLanePinPrefixRollbackAndCapturedBorrowRequireOriginalCapture) {
    harken::AuthoredStopOperation op({0, 1, 17, 19, 23, 7, 32});
    FenceRecord fence(op);
    fence.payload.authoredStopOperation = op.Identity();
    harken::TimelineArmTable arms(1);
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 2, 2, kBlock, true}));
    harken::ImmediateLaneEvent events[2]{};
    events[0].timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    events[0].fixedFrame = 32;
    events[0].event = LaneControlInput(fence.event, fence.request.program);
    const harken::TimelineEventPayload ordinaryPayload{};
    const harken::TimelineProgramView ordinary{3, &ordinaryPayload, 1};
    events[1].relativeFrame = 40;
    events[1].event.program = {3, &ordinary};
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(17, 2, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 2));
    EXPECT_FALSE(lane.PinArms(handle, arms));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, op.Cancel());
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, fence.descriptor.Access());
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.Retire(handle));
    harken::AuthoredStopOperation active({0, 2, 17, 19, 23, 7, 32});
    FenceRecord original(active);
    original.payload.authoredStopOperation = active.Identity();
    events[0].event = LaneControlInput(original.event, original.request.program);
    handle = {};
    ASSERT_TRUE(lane.Reserve(17, 1, &handle));
    ASSERT_TRUE(lane.Prepare(handle, events, 1));
    ASSERT_TRUE(lane.PinArms(handle, arms));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(lane.TryPlace(handle, 0, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 0);
    ASSERT_NE(nullptr, bucket);
    EXPECT_EQ(nullptr, lane.CapturedArmReader(handle, bucket, bucket->firstEvent));
    auto stale = handle; ++stale.generation;
    EXPECT_EQ(nullptr, lane.CapturedNoneFence(stale, bucket, bucket->firstEvent));
    EXPECT_EQ(nullptr, lane.CapturedNoneFence(handle, bucket, bucket->firstEvent + 1));
    EXPECT_EQ(&original.descriptor, lane.CapturedNoneFence(handle, bucket, bucket->firstEvent));
    EXPECT_FALSE(lane.Retire(handle, 9));
    ASSERT_TRUE(lane.Cancel(handle));
    ASSERT_TRUE(lane.ReleaseCapture(handle.slot, bucket));
    EXPECT_EQ(nullptr, lane.CapturedNoneFence(handle, bucket, bucket->firstEvent));
    EXPECT_FALSE(lane.Retire(handle, 1));
    ASSERT_TRUE(lane.Retire(handle, 2));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, active.Cancel());
    EXPECT_EQ(0u, active.AppliedAudioSequence());
}

TEST(AuthoredStopAudio, NoneFenceOriginalReferenceFollowsPinnedReplacementOrderAndFailedPlacement) {
    ControlLaneAudioHarness h(0, 80, 8, false, true);
    ASSERT_TRUE(h.arms.Prepare(static_cast<std::uint32_t>(h.render),
        static_cast<std::uint32_t>(h.render >> 32), 96, 64, 11, {}, &h.startArm));
    ASSERT_TRUE(h.arms.CommitPrepared(h.startArm));
    ASSERT_TRUE(h.arms.AcknowledgeApplied(h.startArm, 1));
    h.startProgram = {11, &h.startPayload, 1};
    harken::ImmediateLaneEvent events[3]{};
    events[0].relativeFrame = 32;
    events[0].event.program = {11, &h.startProgram};
    events[0].event.armToken = h.startArm;
    events[0].event.subeventPhase = 20;
    events[1].timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    events[1].fixedFrame = 80;
    events[1].event = LaneControlInput(h.record.events[0], h.record.input.program);
    events[2].timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    events[2].fixedFrame = 80;
    events[2].event = LaneControlInput(h.fence.event, h.fence.request.program);
    events[2].event.eventIndex = 90;
    ASSERT_TRUE(h.lane.Reserve(17, 3, &h.control));
    ASSERT_TRUE(h.lane.Prepare(h.control, events, 3));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(h.lane.TryPlace(h.control, 0, 0, &anchor));
    ASSERT_EQ(0u, anchor);
    ASSERT_TRUE(h.lane.PinArms(h.control, h.arms));
    ASSERT_TRUE(h.lane.TryPlace(h.control, 64, 0, &anchor));
    ASSERT_EQ(64u, anchor);
    EXPECT_FALSE(h.lane.TryPlace(h.control, 128, 0, &anchor));
    EXPECT_EQ(64u, anchor);
    ASSERT_TRUE(h.arms.Publish(h.startArm));
    ASSERT_TRUE(h.lane.Publish(h.control));
    h.Step();
    EXPECT_EQ(0u, h.record.operation.AppliedAudioSequence());
    h.Step();
    EXPECT_EQ(2u, h.record.operation.AppliedAudioSequence());
    for (std::uint32_t sample = 0; sample < kBlock; ++sample)
        EXPECT_EQ(0.0f, h.audio.Sample(sample));
    h.Retire(false, 2);
    ASSERT_TRUE(h.arms.RequestCancelStartedVoice(h.startArm));
    ASSERT_TRUE(h.arms.Tombstone(h.startArm, h.audio.quantum));
    h.audio.Begin(); h.audio.Render();
    ASSERT_TRUE(h.arms.Reclaim(h.startArm, h.audio.quantum));
    EXPECT_EQ(3u, h.arms.FreeCount());
}

TEST(AuthoredStopAudio, NoneFenceCapturedReferenceRejectsEqualPayloadBackingSubstitutionAndRecoversOriginal) {
    harken::AuthoredStopOperation operation({0, 1, 17, 19, 23, 7, 32});
    FenceRecord fence(operation);
    fence.payload.authoredStopOperation = operation.Identity();
    const auto alternate = fence.payload;
    harken::TimelineArmTable arms(1);
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, kBlock, true}));
    harken::ImmediateLaneEvent event{};
    event.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    event.fixedFrame = 32;
    event.event = LaneControlInput(fence.event, fence.request.program);
    harken::ImmediateLaneHandle handle{};
    ASSERT_TRUE(lane.Reserve(17, 1, &handle));
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    ASSERT_TRUE(lane.PinArms(handle, arms));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(lane.TryPlace(handle, 0, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    const auto* bucket = lane.CaptureDue(handle.slot, 0);
    ASSERT_NE(nullptr, bucket);
    fence.program.payloads = &alternate;
    EXPECT_EQ(nullptr, lane.CapturedNoneFence(handle, bucket, bucket->firstEvent));
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, fence.descriptor.Access());
    EXPECT_EQ(0u, operation.AppliedAudioSequence());
    fence.program.payloads = &fence.payload;
    const auto* descriptor = lane.CapturedNoneFence(handle, bucket, bucket->firstEvent);
    ASSERT_EQ(&fence.descriptor, descriptor);
    harken::AuthoredStopFenceLaneProof proof;
    ASSERT_TRUE(descriptor->ProveLaneApply(fence.request.program, &proof));
    ASSERT_TRUE(lane.Claim(handle, bucket));
    descriptor->ApplyLaneProof(&proof);
    operation.PublishApplied(operation.Identity(), 1);
    descriptor->FinishApply();
    ASSERT_TRUE(lane.Finish(handle.slot, bucket, true));
    EXPECT_FALSE(lane.Retire(handle, 1));
    ASSERT_TRUE(lane.Retire(handle, 2));
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, operation.Cancel());
}

TEST(AuthoredStopAudio, TypedLaneArenaRefusalPreservesOriginalControlAndFenceUntilActualCancellationRetires) {
    for (const auto workers : {0u, 2u}) {
        ControlLaneAudioHarness h(workers);
        h.InstallControl();
        h.sink.SetArenaAdmission(0, kBlock);
        harken::TimelineExecutor executor;
        ASSERT_TRUE(executor.Reset(kBlock, 8, &h.audio.engine->timelinePool, &h.arms, &h.sink));
        harken::TimelineExecutionPlan plan{};
        plan.expectedFrontier = h.audio.engine->timelinePool.ConsumerFrontier().load();
        const auto allocations = harken::memguard::HitCount();
        {
            harken::memguard::AudioThreadScope scope;
            executor.ExecuteQuantum(0, plan);
        }
        EXPECT_EQ(allocations, harken::memguard::HitCount());
        EXPECT_EQ(1u, executor.Stats().renderUnavailableDeferrals);
        harken::ImmediateLaneObservation observation{};
        ASSERT_TRUE(h.lane.Observe(h.control, &observation));
        EXPECT_FALSE(observation.applied);
        EXPECT_FALSE(observation.claimed);
        EXPECT_FALSE(observation.missed);
        EXPECT_FALSE(observation.arenaCapacityMissed);
        const auto* refused = h.lane.CaptureDue(h.control.slot, 0);
        ASSERT_NE(nullptr, refused);
        EXPECT_EQ(harken::ImmediateLaneAdmissionFailure::ArenaCapacity,
            refused->admissionFailure.load());
        ASSERT_TRUE(h.lane.ReleaseCapture(h.control.slot, refused));
        EXPECT_EQ(0u, h.record.operation.AppliedAudioSequence());
        EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, h.fence.descriptor.Access());
        harken::TimelineArmObservation arm{};
        ASSERT_TRUE(h.arms.Observe(h.record.token, &arm));
        EXPECT_EQ(0u, arm.startedBlockSequence);
        EXPECT_EQ(0u, arm.logicalCompletionSequence);
        harken::BlockArena emptyArena(0);
        h.audio.RenderWith(emptyArena);
        for (std::uint32_t sample = 0; sample < kBlock; ++sample)
            EXPECT_EQ(0.0f, h.audio.Sample(sample));
        EXPECT_EQ(0u, h.audio.engine->voices.Resolve(h.render)->srcCursor);
        h.audio.Begin(); h.audio.Render();
        for (std::uint32_t sample = 0; sample < kBlock; ++sample)
            EXPECT_NEAR(0.25f, h.audio.Sample(sample), 1e-6f);
        ASSERT_TRUE(h.lane.Cancel(h.control));
        ASSERT_TRUE(h.lane.Retire(h.control, h.audio.quantum));
        ASSERT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.record.operation.Cancel());
        ASSERT_TRUE(h.arms.RequestCancel(h.record.token));
        ASSERT_TRUE(h.arms.Tombstone(h.record.token, h.audio.quantum));
        h.audio.Begin(); h.audio.Render();
        ASSERT_TRUE(h.record.reader.Release(h.audio.quantum));
        ASSERT_TRUE(h.arms.Reclaim(h.record.token, h.audio.quantum));
        h.audio.engine->voices.ApplyStop(h.render, 0, 0, h.audio.engine->notifications, h.audio.quantum);
        h.audio.Begin(); h.audio.Render();
        EXPECT_TRUE(h.audio.engine->voices.ReleaseObserved(h.audio.lastObserver));
        EXPECT_EQ(3u, h.arms.FreeCount());
    }
}

TEST(AuthoredStopAudio, TypedLaneArenaRetryAtSameQuantumAppliesOnlyAfterRealScratchAdmission) {
    for (const auto workers : {0u, 2u}) {
        ControlLaneAudioHarness h(workers);
        h.InstallControl();
        harken::TimelineExecutor executor;
        ASSERT_TRUE(executor.Reset(kBlock, 8, &h.audio.engine->timelinePool, &h.arms, &h.sink));
        harken::TimelineExecutionPlan plan{};
        plan.expectedFrontier = h.audio.engine->timelinePool.ConsumerFrontier().load();
        h.sink.SetArenaAdmission(kBlock * sizeof(float) - 1, kBlock);
        executor.ExecuteQuantum(0, plan);
        EXPECT_EQ(0u, h.record.operation.AppliedAudioSequence());
        EXPECT_EQ(harken::AuthoredStopFenceAccess::Armed, h.fence.descriptor.Access());
        h.sink.SetArenaAdmission(kBlock * sizeof(float), kBlock);
        executor.ExecuteQuantum(0, plan);
        EXPECT_EQ(1u, h.record.operation.AppliedAudioSequence());
        EXPECT_EQ(harken::AuthoredStopFenceAccess::Done, h.fence.descriptor.Access());
        harken::BlockArena exactArena(kBlock * sizeof(float));
        h.audio.RenderWith(exactArena);
        for (std::uint32_t sample = 0; sample < kBlock; ++sample) {
            const auto expected = sample < 8 ? 0.25f : sample < 32 ?
                0.25f * static_cast<float>(31 - sample) / 23.0f : 0.0f;
            EXPECT_NEAR(expected, h.audio.Sample(sample), 1e-6f);
        }
        h.Retire();
    }
}

TEST(AuthoredStopAudio, ControlScratchAdmissionIncludesEarlierOrdinaryVoicesAndRejectsStaleTargets) {
    StopAudioHarness h(0);
    const auto earlier = h.Play(false);
    const auto controlled = h.Play(false);
    const auto slot = static_cast<std::uint32_t>(controlled);
    const auto generation = static_cast<std::uint32_t>(controlled >> 32);
    ASSERT_LT(static_cast<std::uint32_t>(earlier), slot);
    std::vector<std::uint32_t> targets(h.engine->voices.Capacity());
    EXPECT_TRUE(h.engine->voices.AdmitTimelineQuantum(0, kBlock, nullptr, nullptr, 0,
        targets.data(), static_cast<std::uint32_t>(targets.size())));
    targets[slot] = generation + 1;
    EXPECT_TRUE(h.engine->voices.AdmitTimelineQuantum(0, kBlock, nullptr, nullptr, 0,
        targets.data(), static_cast<std::uint32_t>(targets.size())));
    targets[slot] = generation;
    EXPECT_FALSE(h.engine->voices.AdmitTimelineQuantum(kBlock * sizeof(float), kBlock,
        nullptr, nullptr, 0, targets.data(), static_cast<std::uint32_t>(targets.size())));
    EXPECT_TRUE(h.engine->voices.AdmitTimelineQuantum(2 * kBlock * sizeof(float), kBlock,
        nullptr, nullptr, 0, targets.data(), static_cast<std::uint32_t>(targets.size())));
}

TEST(AuthoredStopAudio, NoneOnlyCapturedFenceRequiresNoVoiceScratchAndRetiresAfterActualReadBlock) {
    StopAudioHarness h(0);
    harken::AuthoredStopOperation operation({0, 1, 17, 19, 23, 7, 32});
    FenceRecord fence(operation);
    fence.payload.authoredStopOperation = operation.Identity();
    harken::TimelineArmTable arms(1);
    harken::ImmediateActionLane lane;
    ASSERT_TRUE(lane.Reset({1, 1, 1, kBlock, true}));
    harken::ImmediateLaneEvent event{};
    event.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    event.fixedFrame = 32;
    event.event = LaneControlInput(fence.event, fence.request.program);
    harken::ImmediateLaneHandle handle{};
    harken::OriginalImmediateLaneAttemptLifetimePin original, foreign;
    ASSERT_TRUE(lane.Reserve(17, {1, 0, 0, 1}, &handle, &original));
    ASSERT_TRUE(original.Borrow(&foreign));
    ASSERT_TRUE(lane.Prepare(handle, &event, 1));
    ASSERT_TRUE(lane.PinArms(handle, arms));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(lane.TryPlace(handle, 0, 0, &anchor));
    ASSERT_TRUE(lane.Publish(handle));
    harken::VoiceTimelineSink sink(h.engine->voices, arms);
    ASSERT_TRUE(sink.AttachImmediateLane(lane, kBlock));
    sink.SetArenaAdmission(0, kBlock);
    harken::TimelineExecutor executor;
    ASSERT_TRUE(executor.Reset(kBlock, 1, &h.engine->timelinePool, &arms, &sink));
    harken::TimelineExecutionPlan plan{};
    plan.expectedFrontier = h.engine->timelinePool.ConsumerFrontier().load();
    executor.ExecuteQuantum(0, plan);
    EXPECT_EQ(0u, executor.Stats().renderUnavailableDeferrals);
    EXPECT_EQ(1u, operation.AppliedAudioSequence());
    EXPECT_EQ(harken::AuthoredStopFenceAccess::Done, fence.descriptor.Access());
    h.Render();
    ASSERT_EQ(1u, h.quantum);
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(original, handle, 17, h.quantum));
    EXPECT_FALSE(lane.Retire(handle, h.quantum, &original));
    foreign.Return();
    EXPECT_TRUE(original.Valid());
    // The actual read fence still refuses equality after the independent loan ends.
    EXPECT_FALSE(lane.Retire(handle, h.quantum, &original));
    h.Begin(); h.Render();
    ASSERT_EQ(2u, h.quantum);
    EXPECT_FALSE(lane.ObserveOriginalUnpublishedStorageOnlyRetention(original, handle, 17, h.quantum));
    ASSERT_TRUE(lane.Retire(handle, h.quantum, &original));
    EXPECT_FALSE(original.Valid());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, operation.Cancel());
}

TEST(AuthoredStopAudio, TypedLaneOriginalControlAndNoneFenceProofsSurviveActualCancelClaimWinners) {
    for (std::uint32_t trial = 0; trial < 8; ++trial) {
        ControlLaneAudioHarness h(0, 32, 8, false);
        h.InstallControl();
        h.audio.Begin();
        const auto* bucket = h.lane.CaptureDue(h.control.slot, 0);
        ASSERT_NE(nullptr, bucket);
        const auto* events = h.lane.Events(h.control.slot, bucket);
        ASSERT_NE(nullptr, events);
        std::uint32_t controlIndex = UINT32_MAX, fenceIndex = UINT32_MAX;
        for (std::uint32_t i = 0; i < bucket->eventCount; ++i) {
            if (events[i].event.armToken.Valid()) controlIndex = i;
            else fenceIndex = i;
        }
        ASSERT_NE(UINT32_MAX, controlIndex);
        ASSERT_NE(UINT32_MAX, fenceIndex);
        const auto& input = events[controlIndex].event;
        const harken::ScheduledEventInput due{input.executeFrame, input.logicCommitSequence,
            input.executionOrdinal, input.eventIndex, input.transactionOrdinal,
            input.subeventPhase, input.payload, input.authoredFadeOutCandidate,
            input.armToken, input.frontier};
        const auto* reader = h.lane.CapturedArmReader(h.control, bucket, bucket->firstEvent + controlIndex);
        ASSERT_NE(nullptr, reader);
        harken::TimelineArmTable::AuthoredControlBucketProof controlProof;
        ASSERT_TRUE(h.arms.ProveAuthoredControlBucket(input.armToken, *reader, input.program,
            0, kBlock, 1, &due, 1, &controlProof));
        const auto* descriptor = h.lane.CapturedNoneFence(h.control, bucket, bucket->firstEvent + fenceIndex);
        ASSERT_EQ(&h.fence.descriptor, descriptor);
        harken::AuthoredStopFenceLaneProof fenceProof;
        ASSERT_TRUE(descriptor->ProveLaneApply(events[fenceIndex].event.program, &fenceProof));
        auto& voices = h.audio.engine->voices;
        harken::VoiceTimelineReservation reservation{};
        ASSERT_TRUE(voices.ReserveLaneTimelineEvents(static_cast<std::uint32_t>(h.render),
            static_cast<std::uint32_t>(h.render >> 32), 0, 1, &reservation));
        auto event = StopAt(32);
        event.order = {input.executeFrame, input.logicCommitSequence, input.executionOrdinal,
            input.eventIndex, input.transactionOrdinal, input.subeventPhase, input.rowSequence};
        event.authoredStopOperation = h.record.operation.Identity();
        event.frontierProof.state = harken::FrontierEventState::Pending;
        event.frontierProof.quantum = 0;
        event.frontierProof.renderGeneration = static_cast<std::uint32_t>(h.render >> 32);
        ASSERT_TRUE(voices.StageReservedLaneTimelineEvent(reservation, 0, event));
        harken::VoiceTimelineReservationProof voiceProof;
        ASSERT_TRUE(voices.ProveTimelineReservation(reservation, &voiceProof));
        bool claimed = false;
        if (trial == 0) {
            ASSERT_TRUE(h.lane.Cancel(h.control));
            claimed = h.lane.Claim(h.control, bucket);
            EXPECT_FALSE(claimed);
        } else if (trial == 1) {
            claimed = h.lane.Claim(h.control, bucket);
            ASSERT_TRUE(claimed);
            EXPECT_FALSE(h.lane.Cancel(h.control));
        } else {
            std::atomic<bool> go{false};
            std::thread cancelling([&] {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                (void)h.lane.Cancel(h.control);
            });
            go.store(true, std::memory_order_release);
            claimed = h.lane.Claim(h.control, bucket);
            cancelling.join();
        }
        if (claimed) {
            harken::VoicePool::ApplyProvenLaneTimelineReservation(voiceProof);
            harken::TimelineArmTable::ApplyAuthoredControlBucket(controlProof);
            controlProof.Release();
            descriptor->ApplyLaneProof(&fenceProof);
            h.record.operation.PublishApplied(h.record.operation.Identity(), 1);
            descriptor->FinishApply();
            ASSERT_TRUE(h.lane.Finish(h.control.slot, bucket, true));
        } else {
            controlProof.Release(); fenceProof.Reset();
            harken::VoicePool::ReleaseTimelineReservationProof(voiceProof);
            ASSERT_TRUE(voices.RejectTimelineReservation(reservation));
        }
        voices.FinalizeTimelineQuantum(0);
        EXPECT_EQ(claimed ? 1u : 0u, h.record.operation.AppliedAudioSequence());
        harken::TimelineArmObservation observation{};
        ASSERT_TRUE(h.arms.Observe(h.record.token, &observation));
        EXPECT_EQ(0u, observation.startedBlockSequence);
        EXPECT_EQ(claimed ? 1u : 0u, observation.logicalCompletionSequence);
        h.audio.Render();
        for (std::uint32_t sample = 0; sample < 32; ++sample)
            EXPECT_NEAR(0.25f, h.audio.Sample(sample), 1e-6f);
        EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, h.record.operation.Cancel());
        EXPECT_FALSE(h.lane.Retire(h.control, h.audio.quantum));
        h.audio.Begin(); h.audio.Render();
        ASSERT_TRUE(h.lane.Retire(h.control, h.audio.quantum));
        ASSERT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.record.operation.Cancel());
        if (!claimed) {
            ASSERT_TRUE(h.arms.RequestCancel(h.record.token));
            ASSERT_TRUE(h.arms.Tombstone(h.record.token, h.audio.quantum));
            voices.ApplyStop(h.render, 0, 0, h.audio.engine->notifications, h.audio.quantum);
            h.audio.Begin(); h.audio.Render();
        }
        ASSERT_TRUE(h.record.reader.Release(h.audio.quantum));
        ASSERT_TRUE(h.arms.Reclaim(h.record.token, h.audio.quantum));
        for (int blocks = 0; blocks < 16; ++blocks) { h.audio.Begin(); h.audio.Render(); }
        EXPECT_TRUE(voices.ReleaseObserved(h.audio.lastObserver));
        EXPECT_EQ(3u, h.arms.FreeCount());
    }
}

namespace {

struct PendingControlRecord {
    explicit PendingControlRecord(std::uint64_t stop = 300, std::uint64_t fade = 100)
        : record(stop, fade, fade < stop) {
        pending = {&record.operation, record.input.program, record.events.data(),
            fade < stop ? 2u : 1u, fade, {}, record.target};
    }

    bool PrepareSource(harken::TimelineArmTable& table) {
        if (!table.PreparePendingImmediate(record.input.renderSlot, record.input.renderGeneration,
            7, record.target, &source)) return false;
        return PrepareControl(table);
    }

    bool PrepareControl(harken::TimelineArmTable& table) {
        pending.sourceStart = source;
        return table.PreparePendingAuthoredControl(pending, &record.token);
    }

    bool CommitControl(harken::TimelineArmTable& table) {
        harken::TimelineArmTable::PreparedCommitProof proof;
        if (!table.ProvePendingAuthoredControlCommit(record.token, pending, &proof)) return false;
        table.ConsumePreparedCommit(std::move(proof));
        return table.AcknowledgeApplied(record.token, 2);
    }

    bool BindSource(harken::TimelineArmTable& table, std::uint64_t onset) {
        if (!table.CommitPrepared(source) || !table.AcknowledgeApplied(source, 1) ||
            !table.BindPendingImmediateStart(source, onset, onset)) return false;
        return table.PinFrontierReader(source, &sourceReader);
    }

    void Finalize(std::uint64_t onset) {
        const auto stop = record.operation.Identity().stopFrame;
        const bool envelope = pending.fadeStartFrame < stop && onset < stop;
        const auto count = envelope ? 2u : 1u;
        record.payloads = {};
        record.events = {};
        for (std::uint32_t i = 0; i < count; ++i) {
            const bool fade = envelope && i == 0;
            auto& payload = record.payloads[i];
            payload.kind = fade ? harken::TimelineEventKind::Envelope : harken::TimelineEventKind::Stop;
            payload.envelopeDirection = harken::TimelineEnvelopeDirection::FadeOut;
            payload.reserved = harken::kTimelineEventTransientControlArm |
                (fade ? 0u : harken::kTimelineEventAuthoredRootStop);
            payload.target = record.target;
            payload.envelopeIndex = fade ? 1u : 0u;
            payload.envelopeStartFrame = fade ? pending.fadeStartFrame : 0;
            payload.envelopeEndFrame = stop;
            payload.authoredFadeOutCandidate = envelope;
            payload.authoredStopOperation = record.operation.Identity();
            auto& event = record.events[i];
            event.executeFrame = fade ? std::min(stop, std::max(pending.fadeStartFrame, onset)) : stop;
            event.executionOrdinal = record.operation.Identity().executionOrdinal;
            event.eventIndex = 41 + i;
            event.subeventPhase = fade ? 30u : 50u;
            event.payload = i;
            event.authoredFadeOutCandidate = envelope;
            event.armToken = record.token;
        }
        record.program.payloadCount = count;
        record.input.eventCount = count;
        record.input.onsetFrame = onset;
    }

    bool Prove(harken::TimelineArmTable& table,
        harken::TimelineArmTable::PendingAuthoredControlBindingProof* out) {
        return table.ProvePendingAuthoredControlBinding(record.token, sourceReader, record.input, out);
    }

    void DisposeControl(harken::TimelineArmTable& table) {
        harken::ArmState state{};
        ASSERT_TRUE(table.State(record.token, &state));
        if (state == harken::ArmState::Prepared) {
            ASSERT_TRUE(table.CancelPrepared(record.token));
            return;
        }
        if (state != harken::ArmState::Tombstoned) {
            ASSERT_TRUE(table.RequestCancel(record.token));
            ASSERT_TRUE(table.Tombstone(record.token, 10));
        }
        if (record.reader.Valid()) ASSERT_TRUE(record.reader.Release(11));
        ASSERT_TRUE(table.Reclaim(record.token, 11));
    }

    void DisposeSource(harken::TimelineArmTable& table) {
        harken::ArmState state{};
        ASSERT_TRUE(table.State(source, &state));
        if (state == harken::ArmState::Prepared) {
            ASSERT_TRUE(table.CancelPrepared(source));
            return;
        }
        if (state != harken::ArmState::Tombstoned) {
            ASSERT_TRUE(table.RequestCancel(source));
            ASSERT_TRUE(table.Tombstone(source, 10));
        }
        if (sourceReader.Valid()) ASSERT_TRUE(sourceReader.Release(11));
        ASSERT_TRUE(table.Reclaim(source, 11));
    }

    OriginalControlRecord record;
    harken::TimelineArmTable::PendingAuthoredControlInput pending{};
    harken::TimelineArmToken source{};
    harken::TimelineArmReaderPin sourceReader;
};

struct MixerPendingControlHarness {
    MixerPendingControlHarness() : audio(0) {}

    bool PrepareSource(std::span<const harken::CommandRecord> metadata = {},
        harken::ParentAudioGroupRef parent = {}, std::uint16_t memberIndex = UINT16_MAX) {
        audio.retainSources = true;
        render = audio.Play(false, 1, false);
        auto& table = audio.engine->timelineArms;
        const bool prepared = parent.Valid()
            ? table.PreparePendingImmediateGrouped(static_cast<std::uint32_t>(render),
                static_cast<std::uint32_t>(render >> 32), 7, control.record.target,
                parent, memberIndex, &control.source)
            : table.PreparePendingImmediate(static_cast<std::uint32_t>(render),
                static_cast<std::uint32_t>(render >> 32), 7, control.record.target, &control.source);
        if (!prepared || !table.CommitPrepared(control.source)) return false;
        return Push(harken::PlayLedBatchKind::TimelineArm, control.source, metadata);
    }

    bool InstallSource() {
        if (!PrepareSource()) return false;
        Pump();
        auto* voice = audio.engine->voices.Resolve(render);
        return voice != nullptr && voice->timelineGated &&
            voice->timelineArmSlot == control.source.slot &&
            voice->timelineArmGeneration == control.source.generation;
    }

    bool InstallControl() {
        auto& table = audio.engine->timelineArms;
        control.record.input.renderSlot = static_cast<std::uint32_t>(render);
        control.record.input.renderGeneration = static_cast<std::uint32_t>(render >> 32);
        if (!control.PrepareControl(table)) return false;
        harken::TimelineArmTable::PreparedCommitProof proof;
        if (!table.ProvePendingAuthoredControlCommit(control.record.token,
            control.pending, &proof)) return false;
        table.ConsumePreparedCommit(std::move(proof));
        return Push(harken::PlayLedBatchKind::TimelineControlArm, control.record.token);
    }

    bool Push(harken::PlayLedBatchKind kind, harken::TimelineArmToken token,
        std::span<const harken::CommandRecord> metadata = {}) {
        harken::CommandRecord command{};
        command.opcode = harken::kOpcodePlay;
        command.version = harken::kCommandVersion;
        command.payload.play.batch = {harken::kPlayLedBatchVersion, kind, 0,
            static_cast<std::uint32_t>(metadata.size() + 1),
            (static_cast<std::uint64_t>(token.generation) << 32) | token.slot};
        command.payload.play.handle = render;
        command.payload.play.sourceIdentity = render;
        if (metadata.empty()) return audio.engine->commands.TryPush(command);
        std::array<harken::CommandRecord, 18> records{};
        if (metadata.size() >= records.size()) return false;
        records[0] = command;
        std::copy(metadata.begin(), metadata.end(), records.begin() + 1);
        return audio.engine->commands.TryPushBatch(records.data(),
            static_cast<std::uint32_t>(metadata.size() + 1));
    }

    void Pump() {
        const auto before = audio.engine->mixer.BlocksRendered();
        audio.engine->nullBackend.Pump(1);
        EXPECT_EQ(before + 1, audio.engine->mixer.BlocksRendered());
    }

    void ExpectSourceUnchanged() {
        auto* voice = audio.engine->voices.Resolve(render);
        ASSERT_NE(nullptr, voice);
        EXPECT_TRUE(voice->timelineGated);
        EXPECT_EQ(control.source.slot, voice->timelineArmSlot);
        EXPECT_EQ(control.source.generation, voice->timelineArmGeneration);
        EXPECT_EQ(0u, voice->srcCursor);
        EXPECT_FALSE(audio.engine->voices.ReleaseObserved(audio.lastObserver));
        EXPECT_EQ(0u, control.record.operation.AppliedAudioSequence());
        const float* output = audio.engine->nullBackend.LastBlock();
        ASSERT_NE(nullptr, output);
        for (std::uint32_t i = 0; i < 2 * kBlock; ++i) EXPECT_FLOAT_EQ(0.0f, output[i]);
    }

    void Dispose() {
        auto& table = audio.engine->timelineArms;
        if (control.record.token.Valid()) {
            harken::ArmState state{};
            EXPECT_TRUE(table.State(control.record.token, &state));
            if (state != harken::ArmState::Tombstoned) {
                EXPECT_TRUE(table.RequestCancel(control.record.token));
                EXPECT_TRUE(table.Tombstone(control.record.token, audio.engine->mixer.BlocksRendered()));
            }
        }
        harken::CommandRecord stop{};
        stop.opcode = harken::kOpcodeStop;
        stop.version = harken::kCommandVersion;
        stop.payload.stop.handle = render;
        EXPECT_TRUE(audio.engine->commands.TryPush(stop));
        Pump();
        EXPECT_TRUE(audio.engine->voices.ReleaseObserved(audio.lastObserver));
        harken::ArmState sourceState{};
        EXPECT_TRUE(table.State(control.source, &sourceState));
        if (sourceState != harken::ArmState::Tombstoned) {
            if (sourceState == harken::ArmState::Started)
                EXPECT_TRUE(table.RequestCancelStartedVoice(control.source));
            else EXPECT_TRUE(table.RequestCancel(control.source));
            EXPECT_TRUE(table.Tombstone(control.source, audio.engine->mixer.BlocksRendered()));
        }
        Pump();
        if (control.record.token.Valid())
            EXPECT_TRUE(table.Reclaim(control.record.token, audio.engine->mixer.BlocksRendered()));
        EXPECT_TRUE(table.Reclaim(control.source, audio.engine->mixer.BlocksRendered()));
    }

    StopAudioHarness audio;
    PendingControlRecord control;
    VnVoiceHandle render = 0;
};

struct HeldArmIdentity {
    HeldArmIdentity(harken::TimelineArmTable& table, harken::TimelineArmToken token)
        : table(table) {
        table.SetResolveIdentityHookForTests([](void* context) {
            auto& hold = *static_cast<HeldArmIdentity*>(context);
            hold.entered.store(true, std::memory_order_release);
            while (!hold.release.load(std::memory_order_acquire)) std::this_thread::yield();
        }, this);
        thread = std::thread([this, token] {
            harken::TimelineArmToken output{};
            resolved = this->table.ResolveIdentity(
                (static_cast<std::uint64_t>(token.generation) << 32) | token.slot, &output);
        });
    }
    ~HeldArmIdentity() { Finish(); }
    bool Wait() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!entered.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }
    void Finish() {
        release.store(true, std::memory_order_release);
        if (thread.joinable()) thread.join();
        table.SetResolveIdentityHookForTests(nullptr, nullptr);
    }
    harken::TimelineArmTable& table;
    std::atomic<bool> entered{false}, release{false};
    std::thread thread;
    bool resolved = false;
};


struct HeldRenderingCancel {
    HeldRenderingCancel(harken::VoicePool& voices, VnVoiceHandle original) : voices(voices) {
        harken::detail::VoicePoolReleaseTestAccess::SetCancelTimelineReadHook(voices,
            [](void* context) {
                auto& hold = *static_cast<HeldRenderingCancel*>(context);
                hold.entered.store(true, std::memory_order_release);
                while (!hold.release.load(std::memory_order_acquire)) std::this_thread::yield();
            }, this);
        thread = std::thread([this, original] {
            cancelled = this->voices.CancelTimelineBinding(static_cast<std::uint32_t>(original),
                static_cast<std::uint32_t>(original >> 32));
        });
    }
    ~HeldRenderingCancel() { Finish(); }
    bool Wait() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!entered.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }
    void Finish() {
        release.store(true, std::memory_order_release);
        if (thread.joinable()) thread.join();
        harken::detail::VoicePoolReleaseTestAccess::SetCancelTimelineReadHook(voices, nullptr, nullptr);
    }
    harken::VoicePool& voices;
    std::atomic<bool> entered{false}, release{false};
    std::thread thread;
    bool cancelled = false;
};

} // namespace


TEST(AuthoredStopAudio, RenderingCancelCurrentNonGatedGenerationRetainsFreshPlayback) {
    StopAudioHarness h(0);
    const auto render = h.Play(false);
    const auto observer = h.lastObserver;
    auto& engine = *h.engine;
    const auto* initial = engine.voices.Resolve(render);
    ASSERT_NE(nullptr, initial);
    const auto sourceFrame = initial->resampler.SourceFrame();
    EXPECT_FALSE(engine.voices.CancelTimelineBinding(static_cast<std::uint32_t>(render),
        static_cast<std::uint32_t>(render >> 32)));
    EXPECT_FALSE(engine.voices.ReleaseObserved(observer));
    EXPECT_NE(nullptr, engine.voices.Resolve(render));
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    EXPECT_EQ(before + 1, engine.mixer.BlocksRendered());
    const auto* current = engine.voices.Resolve(render);
    ASSERT_NE(nullptr, current);
    EXPECT_NEAR(sourceFrame + kBlock, current->resampler.SourceFrame(), 1e-6);
    const auto* pcm = engine.nullBackend.LastBlock();
    ASSERT_NE(nullptr, pcm);
    float energy = 0.0f;
    for (std::uint32_t i = 0; i < 2 * kBlock; ++i) energy += pcm[i] * pcm[i];
    EXPECT_GT(energy, 0.01f);
    engine.voices.ApplyStop(render, 0, 0, engine.notifications, engine.mixer.BlocksRendered());
    engine.nullBackend.Pump(1);
    EXPECT_TRUE(engine.voices.ReleaseObserved(observer));
}

TEST(AuthoredStopAudio, RenderingCancelOriginalGenerationCannotReleaseReissuedVoice) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    auto& engine = *h.audio.engine;
    const auto original = h.render;
    const auto originalObserver = h.audio.lastObserver;
    HeldRenderingCancel hold(engine.voices, original);
    ASSERT_TRUE(hold.Wait());
    ASSERT_TRUE(engine.voices.CancelTimelineBinding(static_cast<std::uint32_t>(original),
        static_cast<std::uint32_t>(original >> 32)));
    EXPECT_TRUE(engine.voices.ReleaseObserved(originalObserver));
    h.audio.retainSources = false;
    const auto replacement = h.audio.Play(false);
    const auto replacementObserver = h.audio.lastObserver;
    ASSERT_EQ(static_cast<std::uint32_t>(original), static_cast<std::uint32_t>(replacement));
    ASSERT_NE(static_cast<std::uint32_t>(original >> 32),
        static_cast<std::uint32_t>(replacement >> 32));
    auto* context = engine.voices.Resolve(replacement);
    ASSERT_NE(nullptr, context);
    EXPECT_FALSE(context->timelineGated);
    EXPECT_EQ(0u, context->srcCursor);
    const auto sourceFrame = context->resampler.SourceFrame();
    hold.Finish();
    EXPECT_FALSE(hold.cancelled);
    EXPECT_FALSE(engine.voices.ReleaseObserved(replacementObserver));
    EXPECT_NE(nullptr, engine.voices.Resolve(replacement));
    h.Pump();
    float energy = 0.0f;
    const auto* pcm = engine.nullBackend.LastBlock();
    ASSERT_NE(nullptr, pcm);
    for (std::uint32_t i = 0; i < 2 * kBlock; ++i) energy += pcm[i] * pcm[i];
    EXPECT_GT(energy, 0.01f);
    if (const auto* current = engine.voices.Resolve(replacement)) {
        EXPECT_NEAR(sourceFrame + kBlock, current->resampler.SourceFrame(), 1e-6);
        EXPECT_GT(current->srcCursor, 0u);
    }
    else ADD_FAILURE() << "Replacement voice disappeared after the original cancel resumed";
    engine.voices.ApplyStop(replacement, 0, 0, engine.notifications, engine.mixer.BlocksRendered());
    h.Pump();
    EXPECT_TRUE(engine.voices.ReleaseObserved(replacementObserver));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(h.control.source));
    ASSERT_TRUE(engine.timelineArms.Tombstone(h.control.source, engine.mixer.BlocksRendered()));
    h.Pump();
    EXPECT_TRUE(engine.timelineArms.Reclaim(h.control.source, engine.mixer.BlocksRendered()));
}

TEST(AuthoredStopAudio, RenderingCancelCurrentGatedGenerationReleasesActualOriginal) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    auto& engine = *h.audio.engine;
    const auto observer = h.audio.lastObserver;
    EXPECT_TRUE(engine.voices.CancelTimelineBinding(static_cast<std::uint32_t>(h.render),
        static_cast<std::uint32_t>(h.render >> 32)));
    EXPECT_TRUE(engine.voices.ReleaseObserved(observer));
    EXPECT_EQ(nullptr, engine.voices.Resolve(h.render));
    h.Pump();
    const auto* pcm = engine.nullBackend.LastBlock();
    ASSERT_NE(nullptr, pcm);
    for (std::uint32_t i = 0; i < 2 * kBlock; ++i) EXPECT_FLOAT_EQ(0.0f, pcm[i]);
    ASSERT_TRUE(engine.timelineArms.RequestCancel(h.control.source));
    ASSERT_TRUE(engine.timelineArms.Tombstone(h.control.source, engine.mixer.BlocksRendered()));
    h.Pump();
    EXPECT_TRUE(engine.timelineArms.Reclaim(h.control.source, engine.mixer.BlocksRendered()));
}

TEST(AuthoredStopAudio, MixerBusyOriginalSourceRetainsAcknowledgementUntilActualRowRelease) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.PrepareSource());
    auto& engine = *h.audio.engine;
    const auto settled = engine.commands.SettledFrontier();
    const auto violations = engine.mixer.CommandProtocolViolationCount();
    const auto before = engine.mixer.BlocksRendered();
    HeldArmIdentity hold(engine.timelineArms, h.control.source);
    ASSERT_TRUE(hold.Wait());
    for (std::uint32_t block = 0; block < 2; ++block) {
        h.Pump();
        EXPECT_EQ(settled, engine.commands.ConsumedCount());
        EXPECT_EQ(settled, engine.commands.SettledFrontier());
        EXPECT_EQ(violations, engine.mixer.CommandProtocolViolationCount());
        EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
        const float* output = engine.nullBackend.LastBlock();
        ASSERT_NE(nullptr, output);
        for (std::uint32_t index = 0; index < 2 * kBlock; ++index)
            EXPECT_FLOAT_EQ(0.0f, output[index]);
    }
    hold.Finish();
    ASSERT_TRUE(hold.resolved);
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.source, &observed));
    EXPECT_EQ(harken::ArmState::PendingApply, observed.state);
    EXPECT_EQ(0u, observed.armedCompletionSequence);
    h.Pump();
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.source, &observed));
    EXPECT_EQ(harken::ArmState::Armed, observed.state);
    EXPECT_GT(observed.armedCompletionSequence, before);
    EXPECT_LE(observed.armedCompletionSequence, engine.mixer.BlocksRendered());
    EXPECT_EQ(0u, observed.startedBlockSequence);
    EXPECT_EQ(engine.commands.ConsumedCount(), engine.commands.SettledFrontier());
    EXPECT_EQ(violations, engine.mixer.CommandProtocolViolationCount());
    EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    h.Dispose();
}

TEST(AuthoredStopAudio, SourceCommandProofAbortRetainsActualClaimAndQueuedOriginal) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.PrepareSource());
    auto& engine = *h.audio.engine;
    auto& table = engine.timelineArms;
    using RowStatus = harken::TimelineArmTable::SourceCommandRowStatus;
    using VoiceStatus = harken::VoicePool::SourceCommandInstallStatus;
    harken::TimelineArmTable::SourceCommandRowProof row;
    auto wrong = h.control.source;
    ++wrong.renderGeneration;
    EXPECT_EQ(RowStatus::NotPendingSource, table.ProvePendingSourceCommand(wrong, &row));
    ASSERT_EQ(RowStatus::Ready, table.ProvePendingSourceCommand(h.control.source, &row));
    harken::TimelineArmTable::SourceCommandRowProof another;
    EXPECT_EQ(RowStatus::DeferredReservation, table.ProvePendingSourceCommand(h.control.source, &another));
    EXPECT_EQ(RowStatus::Invalid, table.ProvePendingSourceCommand(h.control.source, &row));
    harken::VoicePool::SourceCommandInstallProof install;
    ASSERT_EQ(VoiceStatus::Ready, engine.voices.ProveSourceCommandInstallation(row, &install));
    EXPECT_EQ(h.audio.lastObserver.slot, install.ReleaseObserver().slot);
    EXPECT_EQ(h.audio.lastObserver.targetSequence, install.ReleaseObserver().targetSequence);
    EXPECT_EQ(nullptr, engine.voices.Resolve(h.render));
    EXPECT_FALSE(engine.voices.AbortClaimedVoice(h.render));
    EXPECT_FALSE(engine.voices.CancelTimelineBinding(h.control.source.renderSlot,
        h.control.source.renderGeneration));
    EXPECT_FALSE(table.RequestCancel(h.control.source));
    const auto consumed = engine.commands.ConsumedCount();
    h.Pump();
    EXPECT_EQ(consumed, engine.commands.ConsumedCount());
    EXPECT_EQ(consumed, engine.commands.SettledFrontier());
    EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    harken::VoicePool::SourceCommandInstallProof moved(std::move(install));
    EXPECT_FALSE(install.Valid());
    moved.Release();
    ASSERT_NE(nullptr, engine.voices.Resolve(h.render));
    EXPECT_FALSE(engine.voices.Resolve(h.render)->timelineGated);
    row.Release();
    h.Pump();
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(table.Observe(h.control.source, &observation));
    EXPECT_EQ(harken::ArmState::Armed, observation.state);
    EXPECT_EQ(engine.mixer.BlocksRendered(), observation.armedCompletionSequence);
    h.ExpectSourceUnchanged();
    h.Dispose();
}

TEST(AuthoredStopAudio, SourceCommandInstallationHoldsVoiceThroughMetadataAndOriginalAck) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.PrepareSource());
    auto& engine = *h.audio.engine;
    harken::TimelineArmTable::SourceCommandRowProof row;
    ASSERT_EQ(harken::TimelineArmTable::SourceCommandRowStatus::Ready,
        engine.timelineArms.ProvePendingSourceCommand(h.control.source, &row));
    harken::VoicePool::SourceCommandInstallProof install;
    ASSERT_EQ(harken::VoicePool::SourceCommandInstallStatus::Ready,
        engine.voices.ProveSourceCommandInstallation(row, &install));
    harken::CommandRecord command{};
    ASSERT_TRUE(engine.commands.TryPop(command));
    ASSERT_EQ(harken::kOpcodePlay, command.opcode);
    const auto block = engine.mixer.BlocksRendered();
    harken::VoicePool::BeginSourceCommandInstallation(install,
        command.payload.play.sourceIdentity, command.payload.play.sourceStartFrame, block);
    EXPECT_EQ(nullptr, engine.voices.Resolve(h.render));
    EXPECT_FALSE(engine.voices.AbortClaimedVoice(h.render));
    EXPECT_FALSE(engine.voices.CancelTimelineBinding(h.control.source.renderSlot,
        h.control.source.renderGeneration));
    EXPECT_FALSE(engine.timelineArms.RequestCancel(h.control.source));
    engine.voices.ApplySetVolume(h.render, -20.0f, 0);
    {
        harken::VoicePool::SourceCommandMetadataAccess metadata(install);
        engine.voices.ApplySetVolume(h.render, -6.020600f, 0);
        engine.voices.ApplySetPitch(h.render, 1.25, 0);
        EXPECT_EQ(nullptr, engine.voices.Resolve(h.render));
        EXPECT_FALSE(engine.voices.CancelTimelineBinding(h.control.source.renderSlot,
            h.control.source.renderGeneration));
        EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    }
    harken::TimelineArmTable::AcknowledgeSourceCommand(row, block + 1);
    harken::VoicePool::SourceCommandInstallProof secondInstallation;
    EXPECT_EQ(harken::VoicePool::SourceCommandInstallStatus::NotClaimed,
        engine.voices.ProveSourceCommandInstallation(row, &secondInstallation));
    EXPECT_EQ(nullptr, engine.voices.Resolve(h.render));
    EXPECT_FALSE(engine.voices.AbortClaimedVoice(h.render));
    harken::VoicePool::FinishSourceCommandInstallation(install);
    EXPECT_FALSE(install.Valid());
    row.Release();
    engine.commands.PublishSettledFrontier();
    auto* voice = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, voice);
    EXPECT_NEAR(0.5f, voice->gain.target, 0.00001f);
    EXPECT_NEAR(1.25, voice->resampler.CurrentRatio(), 0.000001);
    EXPECT_EQ(h.control.source.slot, voice->timelineArmSlot);
    EXPECT_EQ(h.control.source.generation, voice->timelineArmGeneration);
    EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    h.Dispose();
}

TEST(AuthoredStopAudio, MixerBusySourceRetainsWholeMetadataBatchAndOrderedSibling) {
    MixerPendingControlHarness h;
    auto& engine = *h.audio.engine;
    const auto other = h.audio.Play(false, 1, false);
    const auto otherObserver = h.audio.lastObserver;
    // The next actual source claim uses the next free physical slot.
    std::array<harken::CommandRecord, 2> metadata{};
    metadata[0].opcode = harken::kOpcodeSetVoicePitch;
    metadata[0].payload.setVoicePitch.handle = other;
    metadata[0].payload.setVoicePitch.ratio = 1.25;
    // PrepareSource supplies the real source handle only after Claim; the pan
    // target is the actual second slot observed from this two-voice fixture.
    const auto nextSlot = static_cast<std::uint32_t>(other) + 1u;
    std::uint32_t nextGeneration = 0;
    ASSERT_TRUE(engine.voices.FreeSlotGeneration(nextSlot, &nextGeneration));
    const auto source = (static_cast<std::uint64_t>(nextGeneration) << 32) | nextSlot;
    const auto pan = harken::ResolvePanGains(engine.config.mixChannelLayout, 0.5f);
    metadata[1].opcode = harken::kOpcodeSetVoicePan;
    metadata[1].payload.setVoicePan = {source, 0.5f, pan.gain0, pan.gain1, pan.channel0, pan.channel1, 0.5f, 0.0f, 0};
    ASSERT_TRUE(h.PrepareSource(metadata));
    ASSERT_EQ(source, h.render);
    harken::CommandRecord sibling{};
    sibling.opcode = harken::kOpcodeSetVoiceVolume;
    sibling.payload.setVoiceVolume = {h.render, -12.0f, 0, 0};
    ASSERT_TRUE(engine.commands.TryPush(sibling));
    const auto consumed = engine.commands.ConsumedCount();
    const auto violations = engine.mixer.CommandProtocolViolationCount();
    HeldArmIdentity hold(engine.timelineArms, h.control.source);
    ASSERT_TRUE(hold.Wait());
    h.Pump();
    h.Pump();
    EXPECT_EQ(consumed, engine.commands.ConsumedCount());
    EXPECT_EQ(consumed, engine.commands.SettledFrontier());
    EXPECT_NEAR(1.0, engine.voices.Resolve(other)->resampler.CurrentRatio(), 0.000001);
    hold.Finish();
    h.Pump();
    EXPECT_EQ(consumed + 4, engine.commands.ConsumedCount());
    EXPECT_EQ(engine.commands.ConsumedCount(), engine.commands.SettledFrontier());
    EXPECT_EQ(violations, engine.mixer.CommandProtocolViolationCount());
    auto* original = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, original);
    EXPECT_FLOAT_EQ(0.5f, original->pan);
    EXPECT_NEAR(0.25118864f, original->gain.target, 0.00001f);
    EXPECT_NEAR(1.25, engine.voices.Resolve(other)->resampler.CurrentRatio(), 0.000001);
    EXPECT_FALSE(engine.voices.ReleaseObserved(otherObserver));
    ASSERT_TRUE(engine.voices.AbortClaimedVoice(other));
    EXPECT_TRUE(engine.voices.ReleaseObserved(otherObserver));
    h.Dispose();
}

TEST(AuthoredStopAudio, MixerSourceCompletionMaximumRetainsActualCommandAndCanRecover) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.PrepareSource());
    auto& engine = *h.audio.engine;
    const auto consumed = engine.commands.ConsumedCount();
    const auto published = engine.commands.PublishedHead();
    const auto block = engine.mixer.BlocksRendered();
    const auto violations = engine.mixer.CommandProtocolViolationCount();
    EXPECT_EQ(1u, engine.mixer.DrainCommandsForTest(UINT64_MAX));
    EXPECT_EQ(consumed, engine.commands.ConsumedCount());
    EXPECT_EQ(consumed, engine.commands.SettledFrontier());
    EXPECT_EQ(published, engine.commands.PublishedHead());
    EXPECT_EQ(block, engine.mixer.BlocksRendered());
    EXPECT_EQ(violations, engine.mixer.CommandProtocolViolationCount());
    auto* voice = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, voice);
    EXPECT_FALSE(voice->timelineGated);
    EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.source, &observation));
    EXPECT_EQ(harken::ArmState::PendingApply, observation.state);
    EXPECT_EQ(0u, observation.armedCompletionSequence);
    h.Pump();
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.source, &observation));
    EXPECT_EQ(harken::ArmState::Armed, observation.state);
    EXPECT_EQ(block + 1, observation.armedCompletionSequence);
    EXPECT_EQ(published, engine.commands.SettledFrontier());
    h.Dispose();
}

TEST(AuthoredStopAudio, MixerGroupedSourceCopiesHeldOriginalParentAndMember) {
    MixerPendingControlHarness h;
    auto& engine = *h.audio.engine;
    harken::DecisionReserveRequest request{};
    request.ticket.sequence = 1;
    request.world = 1;
    request.transport = 1;
    request.expectedContinuationGeneration = 1;
    request.sliceCount = 1;
    harken::DecisionToken decision{};
    ASSERT_TRUE(engine.decisions.Reserve(request, &decision));
    ASSERT_TRUE(h.PrepareSource({}, {decision}, 3));
    HeldArmIdentity hold(engine.timelineArms, h.control.source);
    ASSERT_TRUE(hold.Wait());
    h.Pump();
    EXPECT_EQ(0u, engine.commands.ConsumedCount());
    hold.Finish();
    h.Pump();
    auto* voice = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, voice);
    EXPECT_EQ(decision.slot, voice->timelineParentGroup.decision.slot);
    EXPECT_EQ(decision.generation, voice->timelineParentGroup.decision.generation);
    EXPECT_EQ(3u, voice->timelineGroupMemberIndex);
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.source, &observation));
    EXPECT_EQ(harken::ArmState::Armed, observation.state);
    EXPECT_EQ(0u, observation.startedBlockSequence);
    EXPECT_EQ(engine.mixer.BlocksRendered(), observation.armedCompletionSequence);
    h.Dispose();
    ASSERT_TRUE(engine.decisions.Cancel(decision));
    EXPECT_TRUE(engine.decisions.ReclaimCancelled(decision));
}

TEST(AuthoredStopAudio, MixerSourceHeldMetadataInstallsRealPreparedCompressorFlag) {
    MixerPendingControlHarness h;
    auto& engine = *h.audio.engine;
    std::uint32_t generation = 0;
    ASSERT_TRUE(engine.voices.FreeSlotGeneration(0, &generation));
    const VnVoiceHandle expectedSource = static_cast<std::uint64_t>(generation) << 32;
    const auto effect = engine.effectBank.Acquire(VN_EFFECT_COMPRESSOR);
    ASSERT_EQ(VN_EFFECT_COMPRESSOR, effect.type);
    auto* state = static_cast<harken::CompressorState*>(
        engine.effectBank.StateFor(effect.type, effect.poolIndex));
    ASSERT_NE(nullptr, state);
    harken::CompressorInit(state, kRate);
    harken::CompressorParameters parameters{};
    parameters.thresholdDb = -24.0f;
    parameters.ratio = 4.0f;
    parameters.makeupDb = 3.0f;
    harken::CompressorPreparedParameters prepared{};
    ASSERT_TRUE(harken::PrepareCompressorParameters(parameters, kRate, &prepared));
    std::array<harken::CommandRecord, 2> metadata{};
    metadata[0].opcode = harken::kOpcodeSetVoiceEffect;
    metadata[0].payload.setVoiceEffect = {expectedSource, 0, effect.type, effect.poolIndex, effect.generation};
    metadata[1].opcode = harken::kOpcodeSetVoiceEffectParam;
    metadata[1].flags = harken::kCommandFlagPreparedCompressor;
    auto& payload = metadata[1].payload.setCompressorEffectParam;
    payload.host = expectedSource;
    payload.slot = 0;
    payload.effectType = VN_EFFECT_COMPRESSOR;
    payload.thresholdDb = prepared.thresholdDb;
    payload.reciprocalRatio = prepared.reciprocalRatio;
    payload.attackCoeff = prepared.attackCoeff;
    payload.releaseCoeff = prepared.releaseCoeff;
    payload.kneeDb = prepared.kneeDb;
    payload.makeupLinear = prepared.makeupLinear;
    payload.lowerKneeLinear = prepared.lowerKneeLinear;
    payload.upperKneeLinear = prepared.upperKneeLinear;
    payload.hardKneeScale = prepared.hardKneeScale;
    ASSERT_TRUE(h.PrepareSource(metadata));
    ASSERT_EQ(expectedSource, h.render);
    engine.effectBank.PublishInstalled(effect);
    h.Pump();
    auto* voice = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, voice);
    ASSERT_GT(voice->fxSlotCount, 0u);
    EXPECT_EQ(effect.generation, voice->fxSlotEffectRef[0].generation);
    EXPECT_EQ(state, voice->fxSlots[0].state);
    EXPECT_FLOAT_EQ(prepared.thresholdDb, state->parameters.thresholdDb);
    EXPECT_FLOAT_EQ(prepared.reciprocalRatio, state->parameters.reciprocalRatio);
    EXPECT_FLOAT_EQ(prepared.makeupLinear, state->parameters.makeupLinear);
    EXPECT_EQ(0u, engine.mixer.CommandProtocolViolationCount());
    h.Dispose();
    engine.effectBank.DrainRetire();
    EXPECT_EQ(nullptr, engine.effectBank.LiveStateFor(effect));
}

TEST(AuthoredStopAudio, MixerCancelledControlPreservesGenuineGatedOriginalSource) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.InstallControl());
    auto& table = h.audio.engine->timelineArms;
    ASSERT_TRUE(table.RequestCancel(h.control.record.token));
    h.Pump();
    h.ExpectSourceUnchanged();
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(table.Observe(h.control.record.token, &observed));
    EXPECT_EQ(harken::ArmState::Tombstoned, observed.state);
    EXPECT_EQ(0u, observed.armedCompletionSequence);
    EXPECT_EQ(0u, observed.startedBlockSequence);
    EXPECT_EQ(h.audio.engine->mixer.BlocksRendered(), observed.tombstoneAudioSequence);
    h.Dispose();
}

TEST(AuthoredStopAudio, MixerBusyLastControlRetainsAckAcrossRealBlocksWithoutAdvancingSettled) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.InstallControl());
    auto& engine = *h.audio.engine;
    RecordProperty("mixer_pending_control_ack_bytes",
        static_cast<int>(engine.mixer.PendingControlAckBytesForTest()));
    RecordProperty("mixer_bytes", static_cast<int>(sizeof(harken::MixerLoop)));
    RecordProperty("audio_allocation_guard_armed", harken::memguard::IsArmed() ? 1 : 0);
    const auto allocationHits = harken::memguard::HitCount();
    const auto settled = engine.commands.SettledFrontier();
    const auto violations = engine.mixer.CommandProtocolViolationCount();
    const auto firstCompletion = engine.mixer.BlocksRendered() + 1;
    HeldArmIdentity hold(engine.timelineArms, h.control.record.token);
    ASSERT_TRUE(hold.Wait());
    h.Pump();
    h.ExpectSourceUnchanged();
    EXPECT_EQ(engine.commands.PublishedHead(), engine.commands.ConsumedCount());
    EXPECT_EQ(settled, engine.commands.SettledFrontier());
    EXPECT_EQ(1u, engine.mixer.CommandsDeferredLastBlock());
    h.Pump();
    EXPECT_EQ(settled, engine.commands.SettledFrontier());
    EXPECT_EQ(1u, engine.mixer.CommandsDeferredLastBlock());
    EXPECT_EQ(violations, engine.mixer.CommandProtocolViolationCount());
    hold.Finish();
    ASSERT_TRUE(hold.resolved);
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &observed));
    EXPECT_EQ(harken::ArmState::PendingApply, observed.state);
    EXPECT_EQ(0u, observed.armedCompletionSequence);
    h.Pump();
    h.ExpectSourceUnchanged();
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &observed));
    EXPECT_EQ(harken::ArmState::Armed, observed.state);
    EXPECT_EQ(firstCompletion, observed.armedCompletionSequence);
    EXPECT_EQ(0u, observed.startedBlockSequence);
    EXPECT_EQ(engine.commands.ConsumedCount(), engine.commands.SettledFrontier());
    EXPECT_EQ(0u, engine.mixer.CommandsDeferredLastBlock());
    EXPECT_EQ(violations, engine.mixer.CommandProtocolViolationCount());
    EXPECT_EQ(allocationHits, harken::memguard::HitCount());
    h.Dispose();
}

TEST(AuthoredStopAudio, MixerBusyControlCancellationSettlesBeforeActualSiblingStop) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.InstallControl());
    auto& engine = *h.audio.engine;
    const auto settled = engine.commands.SettledFrontier();
    const auto firstCompletion = engine.mixer.BlocksRendered() + 1;
    const auto violations = engine.mixer.CommandProtocolViolationCount();
    harken::CommandRecord sibling{};
    sibling.opcode = harken::kOpcodeStop;
    sibling.version = harken::kCommandVersion;
    sibling.payload.stop.handle = h.render;
    ASSERT_TRUE(engine.commands.TryPush(sibling));
    HeldArmIdentity hold(engine.timelineArms, h.control.record.token);
    ASSERT_TRUE(hold.Wait());
    h.Pump();
    EXPECT_EQ(1u, engine.commands.PublishedHead() - engine.commands.ConsumedCount());
    EXPECT_EQ(2u, engine.mixer.CommandsDeferredLastBlock());
    EXPECT_EQ(settled, engine.commands.SettledFrontier());
    h.ExpectSourceUnchanged();
    h.Pump();
    EXPECT_EQ(1u, engine.commands.PublishedHead() - engine.commands.ConsumedCount());
    h.ExpectSourceUnchanged();
    hold.Finish();
    ASSERT_TRUE(engine.timelineArms.RequestCancel(h.control.record.token));
    h.Pump();
    EXPECT_TRUE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &observed));
    EXPECT_EQ(harken::ArmState::Tombstoned, observed.state);
    EXPECT_EQ(0u, observed.armedCompletionSequence);
    EXPECT_EQ(engine.mixer.BlocksRendered(), observed.tombstoneAudioSequence);
    EXPECT_GT(observed.tombstoneAudioSequence, firstCompletion);
    EXPECT_FALSE(engine.timelineArms.Reclaim(h.control.record.token,
        observed.tombstoneAudioSequence));
    EXPECT_EQ(0u, engine.commands.PublishedHead() - engine.commands.ConsumedCount());
    EXPECT_EQ(engine.commands.ConsumedCount(), engine.commands.SettledFrontier());
    EXPECT_EQ(violations, engine.mixer.CommandProtocolViolationCount());
    h.Dispose();
}

TEST(AuthoredStopAudio, MixerInvalidControlPhysicalIdentityReportsOnceAndKeepsOriginalAck) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.InstallControl());
    auto& engine = *h.audio.engine;
    h.Pump();
    harken::TimelineArmObservation before{}, after{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &before));
    const auto violations = engine.mixer.CommandProtocolViolationCount();
    auto wrong = h.control.record.token;
    ++wrong.renderGeneration;
    harken::CommandRecord command{};
    command.opcode = harken::kOpcodePlay;
    command.version = harken::kCommandVersion;
    command.payload.play.batch = {harken::kPlayLedBatchVersion,
        harken::PlayLedBatchKind::TimelineControlArm, 0, 1,
        (static_cast<std::uint64_t>(wrong.generation) << 32) | wrong.slot};
    command.payload.play.handle =
        (static_cast<std::uint64_t>(wrong.renderGeneration) << 32) | wrong.renderSlot;
    ASSERT_TRUE(engine.commands.TryPush(command));
    h.Pump();
    h.ExpectSourceUnchanged();
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &after));
    EXPECT_EQ(before.state, after.state);
    EXPECT_EQ(before.armedCompletionSequence, after.armedCompletionSequence);
    EXPECT_EQ(violations + 1, engine.mixer.CommandProtocolViolationCount());
    h.Pump();
    EXPECT_EQ(violations + 1, engine.mixer.CommandProtocolViolationCount());
    EXPECT_EQ(engine.commands.ConsumedCount(), engine.commands.SettledFrontier());
    h.Dispose();
}

TEST(AuthoredStopAudio, MixerOriginalSourceCancellationStillReleasesItsOwnGatedVoice) {
    MixerPendingControlHarness h;
    h.audio.retainSources = true;
    h.render = h.audio.Play(false, 1, false);
    auto& engine = *h.audio.engine;
    ASSERT_TRUE(engine.timelineArms.PreparePendingImmediate(
        static_cast<std::uint32_t>(h.render), static_cast<std::uint32_t>(h.render >> 32),
        7, h.control.record.target, &h.control.source));
    ASSERT_TRUE(engine.timelineArms.CommitPrepared(h.control.source));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(h.control.source));
    ASSERT_TRUE(h.Push(harken::PlayLedBatchKind::TimelineArm, h.control.source));
    h.Pump();
    EXPECT_TRUE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.source, &observed));
    EXPECT_EQ(harken::ArmState::Tombstoned, observed.state);
    EXPECT_FALSE(engine.timelineArms.Reclaim(h.control.source, observed.tombstoneAudioSequence));
    h.Pump();
    EXPECT_TRUE(engine.timelineArms.Reclaim(h.control.source, engine.mixer.BlocksRendered()));
}

TEST(AuthoredStopAudio, MixerStaleControlCommandNeverConfirmsGenuinelyReissuedRow) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    auto& engine = *h.audio.engine;
    harken::TimelineArmToken old{}, current{};
    ASSERT_TRUE(engine.timelineArms.Prepare(static_cast<std::uint32_t>(h.render),
        static_cast<std::uint32_t>(h.render >> 32), 300, 300, 5, {}, &old));
    ASSERT_TRUE(h.Push(harken::PlayLedBatchKind::TimelineControlArm, old));
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(old));
    ASSERT_TRUE(engine.timelineArms.Prepare(static_cast<std::uint32_t>(h.render),
        static_cast<std::uint32_t>(h.render >> 32), 300, 300, 5, {}, &current));
    ASSERT_EQ(old.slot, current.slot);
    ASSERT_NE(old.generation, current.generation);
    ASSERT_TRUE(engine.timelineArms.CommitPrepared(current));
    const auto violations = engine.mixer.CommandProtocolViolationCount();
    h.Pump();
    h.ExpectSourceUnchanged();
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(current, &observed));
    EXPECT_EQ(harken::ArmState::PendingApply, observed.state);
    EXPECT_EQ(0u, observed.armedCompletionSequence);
    EXPECT_EQ(violations, engine.mixer.CommandProtocolViolationCount());
    EXPECT_EQ(engine.commands.ConsumedCount(), engine.commands.SettledFrontier());
    ASSERT_TRUE(engine.timelineArms.RequestCancel(current));
    ASSERT_TRUE(engine.timelineArms.Tombstone(current, engine.mixer.BlocksRendered()));
    h.Pump();
    EXPECT_TRUE(engine.timelineArms.Reclaim(current, engine.mixer.BlocksRendered()));
    h.Dispose();
}

TEST(AuthoredStopAudio, MixerLegacyGroupedControlPreservesGenuineAdmissionRetirement) {
    for (const bool cancelBeforeConsume : {false, true}) {
        MixerPendingControlHarness h;
        ASSERT_TRUE(h.InstallSource());
        auto& engine = *h.audio.engine;
        harken::DecisionReserveRequest request{};
        request.ticket.sequence = 1;
        request.world = 1;
        request.transport = 1;
        request.expectedContinuationGeneration = 1;
        request.sliceCount = 1;
        harken::DecisionToken decision{};
        ASSERT_TRUE(engine.decisions.Reserve(request, &decision));
        harken::TimelineArmToken control{};
        ASSERT_TRUE(engine.timelineArms.PreparePendingImmediateGrouped(
            static_cast<std::uint32_t>(h.render), static_cast<std::uint32_t>(h.render >> 32),
            7, h.control.record.target, {decision}, 0, &control));
        harken::TimelineArmReaderPin pin;
        ASSERT_TRUE(engine.timelineArms.PinFrontierReader(control, &pin));
        harken::AudioSliceLease lease;
        harken::FrontierGroupBinding binding{};
        binding.role = harken::FrontierGroupRole::Future;
        binding.entry = {{1, 1}, 1, {1, 1, 0, 0, 1, 1, 0, 1}};
        binding.primary = {2, 1, decision, 0, 0, 1, 1};
        binding.operation.kind = harken::FrontierOperationKind::Dependent;
        binding.operation.owner = binding.entry.owner;
        binding.operation.group = binding.primary;
        binding.operation.order = {300, 1};
        binding.operation.world = 1;
        binding.operation.transport = 1;
        ASSERT_TRUE(engine.timelineArms.BindFrontierAdmission(control, binding, lease));
        ASSERT_TRUE(engine.timelineArms.CommitPrepared(control));
        ASSERT_TRUE(h.Push(harken::PlayLedBatchKind::TimelineControlArm, control));
        if (cancelBeforeConsume) ASSERT_TRUE(engine.timelineArms.RequestCancel(control));
        h.Pump();
        h.ExpectSourceUnchanged();
        harken::TimelineArmObservation observed{};
        ASSERT_TRUE(engine.timelineArms.Observe(control, &observed));
        EXPECT_EQ(decision.slot, observed.parentGroup.decision.slot);
        if (!cancelBeforeConsume) {
            EXPECT_EQ(harken::ArmState::Armed, observed.state);
            EXPECT_EQ(engine.mixer.BlocksRendered(), observed.armedCompletionSequence);
            ASSERT_TRUE(engine.timelineArms.RequestCancel(control));
            ASSERT_TRUE(h.Push(harken::PlayLedBatchKind::TimelineControlArm, control));
            h.Pump();
            h.ExpectSourceUnchanged();
            ASSERT_TRUE(engine.timelineArms.Observe(control, &observed));
        }
        EXPECT_EQ(harken::ArmState::Tombstoned, observed.state);
        EXPECT_EQ(harken::AudioSliceState::SliceInvalidated, lease.Capture().state);
        EXPECT_FALSE(engine.timelineArms.FrontierAdmissionReleasedForRetirement(control, binding, lease));
        EXPECT_FALSE(engine.timelineArms.Reclaim(control, observed.tombstoneAudioSequence));
        h.Pump();
        ASSERT_TRUE(pin.Release(engine.mixer.BlocksRendered()));
        EXPECT_TRUE(engine.timelineArms.FrontierAdmissionReleasedForRetirement(control, binding, lease));
        EXPECT_TRUE(engine.timelineArms.Reclaim(control, engine.mixer.BlocksRendered()));
        ASSERT_TRUE(engine.decisions.Cancel(decision));
        EXPECT_TRUE(engine.decisions.ReclaimCancelled(decision));
        h.Dispose();
    }
}

TEST(AuthoredStopAudio, MixerBusyControlCancelRetryPreservesOriginalVoiceAndRealTombstoneBlock) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.InstallControl());
    auto& engine = *h.audio.engine;
    const auto firstCompletion = engine.mixer.BlocksRendered() + 1;
    HeldArmIdentity hold(engine.timelineArms, h.control.record.token);
    ASSERT_TRUE(hold.Wait());
    h.Pump();
    h.Pump();
    h.ExpectSourceUnchanged();
    hold.Finish();
    ASSERT_TRUE(engine.timelineArms.RequestCancel(h.control.record.token));
    h.Pump();
    h.ExpectSourceUnchanged();
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &observed));
    EXPECT_EQ(harken::ArmState::Tombstoned, observed.state);
    EXPECT_EQ(0u, observed.armedCompletionSequence);
    EXPECT_GT(observed.tombstoneAudioSequence, firstCompletion);
    EXPECT_EQ(engine.mixer.BlocksRendered(), observed.tombstoneAudioSequence);
    EXPECT_FALSE(engine.timelineArms.Reclaim(h.control.record.token,
        engine.mixer.BlocksRendered()));
    EXPECT_EQ(engine.commands.ConsumedCount(), engine.commands.SettledFrontier());
    EXPECT_EQ(0u, engine.mixer.CommandsDeferredLastBlock());
    h.Dispose();
}

TEST(AuthoredStopAudio, MixerUnissuedFutureControlGenerationIsProtocolErrorWithoutRowWrite) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.InstallControl());
    auto& engine = *h.audio.engine;
    h.Pump();
    harken::TimelineArmObservation before{}, after{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &before));
    auto unissued = h.control.record.token;
    ++unissued.generation;
    const auto violations = engine.mixer.CommandProtocolViolationCount();
    ASSERT_TRUE(h.Push(harken::PlayLedBatchKind::TimelineControlArm, unissued));
    h.Pump();
    h.ExpectSourceUnchanged();
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &after));
    EXPECT_EQ(before.state, after.state);
    EXPECT_EQ(before.armedCompletionSequence, after.armedCompletionSequence);
    EXPECT_EQ(violations + 1, engine.mixer.CommandProtocolViolationCount());
    EXPECT_EQ(engine.commands.ConsumedCount(), engine.commands.SettledFrontier());
    h.Pump();
    EXPECT_EQ(violations + 1, engine.mixer.CommandProtocolViolationCount());
    h.Dispose();
}

TEST(AuthoredStopAudio, ControlAckStateRefusalsAndIdempotencePreserveOriginalCompletion) {
    using Status = harken::TimelineArmTable::ControlCommandAckStatus;
    harken::TimelineArmTable table(1);
    harken::TimelineArmToken token{};
    ASSERT_TRUE(table.Prepare(0, 1, 300, 300, 5, {}, &token));
    EXPECT_EQ(Status::Invalid, table.AcknowledgeControlCommand(token, 2, 2));
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(table.Observe(token, &observed));
    EXPECT_EQ(harken::ArmState::Prepared, observed.state);
    EXPECT_EQ(0u, observed.armedCompletionSequence);
    ASSERT_TRUE(table.CommitPrepared(token));
    EXPECT_EQ(Status::Invalid, table.AcknowledgeControlCommand(token, 0, 2));
    EXPECT_EQ(Status::Invalid, table.AcknowledgeControlCommand(token, 3, 2));
    EXPECT_EQ(Status::Applied, table.AcknowledgeControlCommand(token, 2, 2));
    EXPECT_EQ(Status::AlreadySettled, table.AcknowledgeControlCommand(token, 2, 3));
    EXPECT_EQ(Status::Invalid, table.AcknowledgeControlCommand(token, 3, 3));
    ASSERT_TRUE(table.Observe(token, &observed));
    EXPECT_EQ(2u, observed.armedCompletionSequence);
    EXPECT_EQ(0u, observed.startedBlockSequence);
    EXPECT_EQ(0u, observed.logicalCompletionSequence);
    ASSERT_TRUE(table.Publish(token));
    EXPECT_EQ(Status::Invalid, table.AcknowledgeControlCommand(token, 2, 3));
    ASSERT_TRUE(table.Start(token, 3));
    EXPECT_EQ(Status::Invalid, table.AcknowledgeControlCommand(token, 2, 3));
    ASSERT_TRUE(table.Tombstone(token, 3));
    EXPECT_EQ(Status::AlreadySettled, table.AcknowledgeControlCommand(token, 2, 4));
    ASSERT_TRUE(table.Observe(token, &observed));
    EXPECT_EQ(3u, observed.tombstoneAudioSequence);
    EXPECT_FALSE(table.Reclaim(token, 3));
    ASSERT_TRUE(table.Reclaim(token, 4));
    EXPECT_EQ(Status::Stale, table.AcknowledgeControlCommand(token, 2, 4));
}

TEST(AuthoredStopAudio, MixerCancelledControlOriginalStartStillProducesFreshPcmAndReleasesRealSource) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.InstallControl());
    auto& engine = *h.audio.engine;
    ASSERT_TRUE(engine.timelineArms.RequestCancel(h.control.record.token));
    h.Pump();
    h.ExpectSourceUnchanged();
    auto* original = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, original);
    ASSERT_NE(nullptr, original->sourceTerminal);
    const auto sourceToken = original->sourceTerminalToken;
    const auto leaf = original->sourceTerminal->leaf;
    const auto initialRead = original->resampler.SourceFrame();
    // Centered non-spatial mono uses plain duplication into the stereo device.
    const float expected = 0.25f;
    ASSERT_GT(expected, 0.0f);
    const auto firstFrame = (engine.mixer.BlocksRendered() + 2u) * kBlock;
    harken::TimelineEventPayload payload{};
    payload.kind = harken::TimelineEventKind::Start;
    payload.target = h.control.record.target;
    harken::TimelineProgramView program{7, &payload, 1};
    harken::ImmediateLaneEvent event{};
    event.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    event.fixedFrame = firstFrame;
    event.event.executeFrame = firstFrame;
    event.event.program = {7, &program};
    event.event.armToken = h.control.source;
    event.event.subeventPhase = 20;
    event.event.ownerInstanceHandle = leaf;
    event.event.ownerInstanceScheduleEpoch = original->sourceTerminal->acceptedEpoch;
    harken::ImmediateLaneHandle lane{};
    ASSERT_TRUE(engine.immediateActionLane.Reserve(leaf, 1, &lane));
    ASSERT_TRUE(engine.immediateActionLane.Prepare(lane, &event, 1));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(engine.immediateActionLane.TryPlace(lane, firstFrame, 0, &anchor));
    ASSERT_EQ(firstFrame, anchor);
    ASSERT_TRUE(engine.timelineArms.BindPendingImmediateStart(h.control.source, firstFrame, firstFrame));
    ASSERT_TRUE(engine.immediateActionLane.PinArms(lane, engine.timelineArms));
    ASSERT_TRUE(engine.timelineArms.Publish(h.control.source));
    ASSERT_TRUE(engine.immediateActionLane.Publish(lane));
    while (engine.mixer.BlocksRendered() < firstFrame / kBlock) {
        h.Pump();
        h.ExpectSourceUnchanged();
    }
    h.Pump();
    const float* output = engine.nullBackend.LastBlock();
    ASSERT_NE(nullptr, output);
    for (std::uint32_t frame = kBlock - 16; frame < kBlock; ++frame)
        EXPECT_NEAR(expected, output[2 * frame], 1e-6f);
    original = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, original);
    EXPECT_FALSE(original->timelineGated);
    EXPECT_EQ(harken::kResampleChunkFrames, original->srcCursor);
    // The legacy interpolation center includes its initialized history pad.
    EXPECT_EQ(initialRead + kBlock, original->resampler.SourceFrame());
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.source, &observed));
    EXPECT_EQ(harken::ArmState::Started, observed.state);
    EXPECT_EQ(firstFrame / kBlock + 1u, observed.startedBlockSequence);
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &observed));
    EXPECT_EQ(harken::ArmState::Tombstoned, observed.state);
    EXPECT_EQ(0u, observed.startedBlockSequence);
    EXPECT_EQ(0u, h.control.record.operation.AppliedAudioSequence());
    EXPECT_FALSE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
    h.Pump();
    EXPECT_TRUE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
    h.Dispose();
    harken::SourceTerminalPayload terminal{};
    ASSERT_TRUE(engine.sourceTerminals.Capture(sourceToken, engine.mixer.BlocksRendered(), &terminal));
    EXPECT_EQ(harken::SourceTerminalKind::Stop, terminal.kind);
    EXPECT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(leaf));
    EXPECT_TRUE(engine.sourceTerminals.Release(sourceToken));
    EXPECT_EQ(nullptr, engine.sourceTerminals.Resolve(sourceToken));
    EXPECT_TRUE(engine.playableInstances.SetTerminalOutcome(leaf,
        {harken::PlayableTerminalKind::RequestedStop, VN_OK, 0, 0}));
    EXPECT_TRUE(engine.playableInstances.AdvanceLifecycle(leaf, harken::PlayableLifecycle::Retired));
    EXPECT_FALSE(engine.playableInstances.Classify(leaf).live);
}

TEST(AuthoredStopAudio, PendingControlUsesRealCreditTypedCommitAndRejectsEveryUnboundGenericRoute) {
    harken::TimelineArmTable table(2);
    PendingControlRecord h;
    ASSERT_TRUE(h.PrepareSource(table));
    EXPECT_EQ(0u, table.FreeCount());
    EXPECT_EQ(2u, table.LiveCount());
    PendingControlRecord overflow;
    overflow.source = h.source;
    EXPECT_FALSE(overflow.PrepareControl(table));
    EXPECT_FALSE(overflow.record.token.Valid());
    harken::TimelineArmTable::PreparedCommitInput generic{};
    generic.arm = h.record.token;
    generic.programGeneration = h.record.program.generation;
    generic.target = h.record.target;
    harken::TimelineArmTable::PreparedCommitProof commit;
    EXPECT_FALSE(table.ProvePreparedCommit(generic, &commit));
    generic.pendingImmediateStart = true;
    EXPECT_FALSE(table.ProvePreparedCommit(generic, &commit));
    EXPECT_FALSE(table.CommitPrepared(h.record.token));
    harken::TimelineArmReaderPin reader;
    EXPECT_FALSE(table.PinFrontierReader(h.record.token, &reader));
    ASSERT_TRUE(table.ProvePendingAuthoredControlCommit(h.record.token, h.pending, &commit));
    EXPECT_FALSE(table.CancelPrepared(h.record.token));
    EXPECT_FALSE(table.AcknowledgeApplied(h.record.token, 2));
    commit.Release();
    ASSERT_TRUE(h.CommitControl(table));
    EXPECT_FALSE(table.Publish(h.record.token));
    EXPECT_FALSE(table.PinFrontierReader(h.record.token, &reader));
    EXPECT_FALSE(table.BindPendingImmediateStart(h.record.token, 80, 80));
    EXPECT_FALSE(table.Start(h.record.token, 1));
    harken::TimelineArmTable::LaneAudioProof laneProof;
    EXPECT_FALSE(table.ProveLaneStart(h.record.token, 0, 1, &laneProof));
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(table.Observe(h.record.token, &observed));
    EXPECT_EQ(harken::ArmState::Armed, observed.state);
    EXPECT_EQ(2u, observed.armedCompletionSequence);
    EXPECT_EQ(0u, observed.startedBlockSequence);
    EXPECT_EQ(0u, observed.logicalCompletionSequence);
    EXPECT_FALSE(table.Resolve(h.record.token)->startBound);
    ASSERT_NO_FATAL_FAILURE(h.DisposeControl(table));
    ASSERT_NO_FATAL_FAILURE(h.DisposeSource(table));
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(AuthoredStopAudio, PendingControlTypedCommitChecksOriginalOwnersAndReleasesExactReservation) {
    harken::TimelineArmTable table(2);
    PendingControlRecord h;
    ASSERT_TRUE(h.PrepareSource(table));
    for (unsigned field = 0; field < 11; ++field) {
        SCOPED_TRACE(field);
        auto wrong = h.pending;
        auto copiedProgram = h.record.program;
        std::array<harken::ScheduledEventInput, 2> copiedEvents = h.record.events;
        switch (field) {
        case 0: wrong.operation = nullptr; break;
        case 1: wrong.program.program = &copiedProgram; break;
        case 2: ++wrong.program.programGeneration; break;
        case 3: wrong.events = copiedEvents.data(); break;
        case 4: --wrong.eventCapacity; break;
        case 5: ++wrong.fadeStartFrame; break;
        case 6: ++wrong.sourceStart.generation; break;
        case 7: ++wrong.sourceStart.renderSlot; break;
        case 8: ++wrong.sourceStart.renderGeneration; break;
        case 9: ++wrong.target.programSemanticFingerprint; break;
        case 10: wrong.sourceStart.slot = h.record.token.slot; break;
        }
        harken::TimelineArmTable::PreparedCommitProof rejected;
        EXPECT_FALSE(table.ProvePendingAuthoredControlCommit(h.record.token, wrong, &rejected));
        EXPECT_FALSE(rejected.Valid());
        EXPECT_EQ(harken::ArmState::Prepared, table.Resolve(h.record.token)->state.load());
    }
    harken::TimelineArmTable::PreparedCommitProof held;
    ASSERT_TRUE(table.ProvePendingAuthoredControlCommit(h.record.token, h.pending, &held));
    harken::TimelineArmTable::PreparedCommitProof moved(std::move(held));
    EXPECT_FALSE(held.Valid());
    EXPECT_FALSE(table.CancelPrepared(h.record.token));
    moved.Release();
    ASSERT_TRUE(table.CancelPrepared(h.record.token));
    EXPECT_EQ(nullptr, table.RowAt(h.record.token.slot)->authoredControl.operation);
    ASSERT_NO_FATAL_FAILURE(h.DisposeSource(table));
}

TEST(AuthoredStopAudio, PendingControlBindsFinalOnsetAndLiteralStopOnceWithRealHeldRights) {
    using Proof = harken::TimelineArmTable::PendingAuthoredControlBindingProof;
    static_assert(!std::is_copy_constructible_v<Proof>);
    for (const auto onset : {80u, 100u, 120u, 300u, 320u}) {
        SCOPED_TRACE(onset);
        harken::TimelineArmTable table(2);
        PendingControlRecord h;
        ASSERT_TRUE(h.PrepareSource(table));
        ASSERT_TRUE(h.CommitControl(table));
        ASSERT_TRUE(h.BindSource(table, onset));
        h.Finalize(onset);
        Proof proof, refused;
        ASSERT_TRUE(h.Prove(table, &proof));
        bool controlCancel = true, sourceCancel = true, sourceRelease = true;
        harken::AuthoredStopCancelDisposition operationCancel{};
        std::thread competitor([&] {
            controlCancel = table.RequestCancel(h.record.token);
            sourceCancel = table.RequestCancel(h.source);
            sourceRelease = h.sourceReader.Release(11);
            operationCancel = h.record.operation.Cancel();
        });
        competitor.join();
        EXPECT_FALSE(controlCancel);
        EXPECT_FALSE(sourceCancel);
        EXPECT_FALSE(sourceRelease);
        EXPECT_TRUE(h.sourceReader.Valid());
        EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, operationCancel);
        EXPECT_FALSE(h.Prove(table, &refused));
        Proof moved(std::move(proof));
        EXPECT_FALSE(proof.Valid());
        moved.Release();
        EXPECT_FALSE(table.Resolve(h.record.token)->startBound);
        ASSERT_TRUE(h.Prove(table, &proof));
        // Changing the DTO does not redirect the already-held original geometry.
        h.record.input.onsetFrame = 299;
        table.ConsumePendingAuthoredControlBinding(std::move(proof));
        h.record.input.onsetFrame = onset;
        EXPECT_FALSE(proof.Valid());
        EXPECT_FALSE(h.Prove(table, &proof));
        const auto* row = table.Resolve(h.record.token);
        ASSERT_NE(nullptr, row);
        EXPECT_TRUE(row->startBound);
        EXPECT_EQ(harken::TimelineArmTable::AuthoredControlStage::Bound, row->authoredControl.stage);
        EXPECT_EQ(onset, row->authoredControl.onsetFrame);
        EXPECT_EQ(onset >= 300 ? 300u : std::max(100u, onset), row->startFrame);
        EXPECT_EQ(onset >= 300 ? 1u : 2u, row->authoredControl.eventCount);
        EXPECT_EQ(2u, row->authoredControl.eventCapacity);
        EXPECT_EQ(0u, row->startedBlockSequence.load());
        EXPECT_EQ(0u, h.record.operation.AppliedAudioSequence());
        ASSERT_TRUE(table.PinFrontierReader(h.record.token, &h.record.reader));
        ASSERT_TRUE(table.Publish(h.record.token));
        harken::TimelineArmTable::AuthoredControlBucketProof apply;
        if (onset < 300) {
            ASSERT_TRUE(h.record.Prove(table, 0, 128, 0, 1, &apply));
            table.ApplyAuthoredControlBucket(apply);
            apply.Release();
        }
        ASSERT_TRUE(h.record.Prove(table, 256, 128, onset >= 300 ? 0u : 1u, 1, &apply));
        table.ApplyAuthoredControlBucket(apply);
        EXPECT_FALSE(h.record.reader.Release(4));
        apply.Release();
        EXPECT_FALSE(h.record.reader.Release(3));
        EXPECT_FALSE(table.Reclaim(h.record.token, 4));
        ASSERT_TRUE(h.record.reader.Release(4));
        EXPECT_FALSE(table.Reclaim(h.record.token, 3));
        ASSERT_TRUE(table.Reclaim(h.record.token, 4));
        EXPECT_EQ(nullptr, table.RowAt(h.record.token.slot)->authoredControl.operation);
        ASSERT_NO_FATAL_FAILURE(h.DisposeSource(table));
        EXPECT_EQ(2u, table.FreeCount());
        EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.record.operation.Cancel());
    }
}

TEST(AuthoredStopAudio, BoundPublicationKeepsSourceControlIdentityLoansAndOperationReservation) {
    harken::TimelineArmTable table(2);
    PendingControlRecord h;
    ASSERT_TRUE(h.PrepareSource(table));
    ASSERT_TRUE(h.CommitControl(table));
    ASSERT_TRUE(h.BindSource(table, 120));
    h.Finalize(120);
    harken::TimelineArmTable::PendingAuthoredControlBindingProof binding;
    ASSERT_TRUE(h.Prove(table, &binding));
    table.ConsumePendingAuthoredControlBinding(std::move(binding));
    ASSERT_TRUE(table.PinFrontierReader(h.record.token, &h.record.reader));
    std::array<harken::TimelineArmTable::BoundArmPublicationInput, 3> inputs{};
    inputs[0].expected = {h.source, 120, 120, 7, h.record.target, {}, UINT16_MAX, true};
    inputs[0].reader = &h.sourceReader;
    inputs[1].expected = {h.record.token, 120, h.record.input.armDeadline,
        h.record.input.program.programGeneration, h.record.target, {}, UINT16_MAX, false};
    inputs[1].reader = &h.record.reader;
    inputs[1].control = &h.record.input;
    inputs[1].sourceStart = h.source;
    inputs[2] = inputs[1];
    std::array<harken::TimelineArmTable::Row*, 3> rows{};
    for (unsigned refusal = 0; refusal < 7; ++refusal) {
        SCOPED_TRACE(refusal);
        auto rejected = inputs;
        auto final = h.record.input;
        rejected[1].control = &final;
        switch (refusal) {
        case 0: ++rejected[1].sourceStart.generation; break;
        case 1: ++final.armDeadline; break;
        case 2: ++final.program.programGeneration; break;
        case 3: ++final.onsetFrame; break;
        case 4: ++final.renderGeneration; break;
        case 5: rejected[1].reader = &h.sourceReader; break;
        case 6: rejected[1].sourceStart.slot = UINT32_MAX; break;
        }
        harken::TimelineArmTable::BoundArmPublicationProof proof;
        EXPECT_FALSE(table.ProveBoundArmPublicationBatch(rejected.data(), rejected.size(), rows.data(), rows.size(), &proof));
        EXPECT_FALSE(proof.Valid());
        EXPECT_EQ(harken::ArmState::Armed, table.Resolve(h.source)->state.load());
        EXPECT_EQ(harken::ArmState::Armed, table.Resolve(h.record.token)->state.load());
        EXPECT_EQ(2u, table.Resolve(h.source)->frontierReaderPins.load());
        EXPECT_EQ(1u, table.Resolve(h.record.token)->frontierReaderPins.load());
    }
    harken::TimelineArmTable::BoundArmPublicationProof proof;
    ASSERT_TRUE(table.ProveBoundArmPublicationBatch(inputs.data(), inputs.size(), rows.data(), rows.size(), &proof));
    bool sourceCancel = true, controlCancel = true, readerRelease = true;
    harken::AuthoredStopCancelDisposition operationCancel{};
    std::thread competitor([&] {
        sourceCancel = table.RequestCancel(h.source);
        controlCancel = table.RequestCancel(h.record.token);
        readerRelease = h.sourceReader.Release(11);
        operationCancel = h.record.operation.Cancel();
    });
    competitor.join();
    EXPECT_FALSE(sourceCancel);
    EXPECT_FALSE(controlCancel);
    EXPECT_FALSE(readerRelease);
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, operationCancel);
    table.ApplyBoundArmPublicationBatch(proof);
    EXPECT_FALSE(proof.Valid());
    EXPECT_EQ(harken::ArmState::Published, table.Resolve(h.source)->state.load());
    EXPECT_EQ(harken::ArmState::Published, table.Resolve(h.record.token)->state.load());
    EXPECT_EQ(2u, table.Resolve(h.source)->frontierReaderPins.load());
    EXPECT_EQ(1u, table.Resolve(h.record.token)->frontierReaderPins.load());
    EXPECT_EQ(h.record.operation.Identity(), table.Resolve(h.source)->originalPlainControlIdentity);
    EXPECT_EQ(0u, h.record.operation.AppliedAudioSequence());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.record.operation.Cancel());
    ASSERT_NO_FATAL_FAILURE(h.DisposeControl(table));
    ASSERT_NO_FATAL_FAILURE(h.DisposeSource(table));
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(AuthoredStopAudio, BoundPublicationRefusesMissingSourceAndReturnsControlOperationReservation) {
    harken::TimelineArmTable table(2);
    PendingControlRecord h;
    ASSERT_TRUE(h.PrepareSource(table));
    ASSERT_TRUE(h.CommitControl(table));
    ASSERT_TRUE(h.BindSource(table, 640));
    h.Finalize(640);
    harken::TimelineArmTable::PendingAuthoredControlBindingProof binding;
    ASSERT_TRUE(h.Prove(table, &binding));
    table.ConsumePendingAuthoredControlBinding(std::move(binding));
    ASSERT_TRUE(table.PinFrontierReader(h.record.token, &h.record.reader));
    harken::TimelineArmTable::BoundArmPublicationInput input{};
    input.expected = {h.record.token, 300, h.record.input.armDeadline,
        h.record.input.program.programGeneration, h.record.target, {}, UINT16_MAX, false};
    input.reader = &h.record.reader;
    input.control = &h.record.input;
    input.sourceStart = h.source;
    harken::TimelineArmTable::Row* row = nullptr;
    harken::TimelineArmTable::BoundArmPublicationProof proof;
    EXPECT_FALSE(table.ProveBoundArmPublicationBatch(&input, 1, &row, 1, &proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_EQ(nullptr, row);
    EXPECT_EQ(harken::ArmState::Armed, table.Resolve(h.source)->state.load());
    EXPECT_EQ(harken::ArmState::Armed, table.Resolve(h.record.token)->state.load());
    EXPECT_EQ(2u, table.Resolve(h.source)->frontierReaderPins.load());
    EXPECT_EQ(1u, table.Resolve(h.record.token)->frontierReaderPins.load());
    EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.record.operation.Cancel());
    ASSERT_NO_FATAL_FAILURE(h.DisposeControl(table));
    ASSERT_NO_FATAL_FAILURE(h.DisposeSource(table));
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(AuthoredStopAudio, PendingControlSourceLoanReclaimRetainsBusyAndEqualReadFence) {
    harken::TimelineArmTable table(2);
    PendingControlRecord h;
    ASSERT_TRUE(h.PrepareSource(table));
    ASSERT_TRUE(h.CommitControl(table));
    ASSERT_TRUE(h.BindSource(table, 640));
    h.Finalize(640);
    harken::TimelineArmTable::PendingAuthoredControlBindingProof binding;
    ASSERT_TRUE(h.Prove(table, &binding));
    table.ConsumePendingAuthoredControlBinding(std::move(binding));
    ASSERT_EQ(2u, table.RowAt(h.source.slot)->frontierReaderPins.load());
    ASSERT_TRUE(table.PinFrontierReader(h.record.token, &h.record.reader));
    ASSERT_TRUE(table.Publish(h.source));
    ASSERT_TRUE(table.Publish(h.record.token));
    harken::TimelineArmTable::AuthoredControlBucketProof stop;
    ASSERT_TRUE(h.record.Prove(table, 256, 128, 0, 1, &stop));
    table.ApplyAuthoredControlBucket(stop);
    stop.Release();
    harken::TimelineArmTable::LaneAudioProof start;
    ASSERT_TRUE(table.ProveLaneStart(h.source, 640, 11, &start, 7, &h.record.target));
    EXPECT_TRUE(table.LaneStartCancelled(start));
    table.ApplyLaneStart(start);
    EXPECT_EQ(0u, table.RowAt(h.source.slot)->startedBlockSequence.load());
    ASSERT_TRUE(h.record.reader.Release(4));
    EXPECT_FALSE(table.Reclaim(h.record.token, 11));
    EXPECT_TRUE(table.RowAt(h.record.token.slot)->pendingControlSourceReader.Valid());
    EXPECT_EQ(2u, table.RowAt(h.source.slot)->frontierReaderPins.load());
    {
        HeldArmIdentity held(table, h.source);
        ASSERT_TRUE(held.Wait());
        EXPECT_FALSE(table.Reclaim(h.record.token, 12));
        EXPECT_EQ(harken::ArmState::Tombstoned, table.RowAt(h.record.token.slot)->state.load());
        EXPECT_TRUE(table.RowAt(h.record.token.slot)->pendingControlSourceReader.Valid());
        EXPECT_EQ(2u, table.RowAt(h.source.slot)->frontierReaderPins.load());
    }
    ASSERT_TRUE(table.Reclaim(h.record.token, 12));
    EXPECT_EQ(1u, table.RowAt(h.source.slot)->frontierReaderPins.load());
    ASSERT_TRUE(table.Tombstone(h.source, 11));
    ASSERT_TRUE(h.sourceReader.Release(12));
    ASSERT_TRUE(table.Reclaim(h.source, 12));
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(AuthoredStopAudio, PendingControlSecondActualBindingRefusedAfterOriginalControlReclaimUntilSourceReissue) {
    harken::TimelineArmTable table(3);
    PendingControlRecord first, second(500, 100), fresh;
    ASSERT_TRUE(first.PrepareSource(table));
    ASSERT_TRUE(first.CommitControl(table));
    second.source = first.source;
    ASSERT_TRUE(second.PrepareControl(table));
    ASSERT_TRUE(second.CommitControl(table));
    ASSERT_TRUE(first.BindSource(table, 120));
    first.Finalize(120);
    harken::TimelineArmTable::PendingAuthoredControlBindingProof binding;
    ASSERT_TRUE(first.Prove(table, &binding));
    table.ConsumePendingAuthoredControlBinding(std::move(binding));
    const auto originalSource = first.source;
    const auto originalControl = first.record.token;
    ASSERT_NO_FATAL_FAILURE(first.DisposeControl(table));
    second.Finalize(120);
    EXPECT_FALSE(table.ProvePendingAuthoredControlBinding(second.record.token,
        first.sourceReader, second.record.input, &binding));
    EXPECT_FALSE(binding.Valid());
    const auto retainedControl = table.RowAt(originalSource.slot)->originalPlainControl;
    EXPECT_EQ(originalControl.slot, retainedControl.slot);
    EXPECT_EQ(originalControl.generation, retainedControl.generation);
    EXPECT_EQ(originalControl.renderSlot, retainedControl.renderSlot);
    EXPECT_EQ(originalControl.renderGeneration, retainedControl.renderGeneration);
    EXPECT_EQ(first.record.operation.Identity(), table.RowAt(originalSource.slot)->originalPlainControlIdentity);
    EXPECT_EQ(1u, table.RowAt(originalSource.slot)->frontierReaderPins.load());
    EXPECT_FALSE(table.RowAt(second.record.token.slot)->startBound);
    ASSERT_NO_FATAL_FAILURE(second.DisposeControl(table));
    ASSERT_NO_FATAL_FAILURE(first.DisposeSource(table));
    ASSERT_TRUE(fresh.PrepareSource(table));
    ASSERT_EQ(originalSource.slot, fresh.source.slot);
    ASSERT_NE(originalSource.generation, fresh.source.generation);
    ASSERT_TRUE(fresh.CommitControl(table));
    ASSERT_TRUE(fresh.BindSource(table, 120));
    fresh.Finalize(120);
    ASSERT_TRUE(fresh.Prove(table, &binding));
    table.ConsumePendingAuthoredControlBinding(std::move(binding));
    EXPECT_EQ(0u, table.RowAt(fresh.source.slot)->appliedPlainStopSequence.load());
    ASSERT_NO_FATAL_FAILURE(fresh.DisposeControl(table));
    ASSERT_NO_FATAL_FAILURE(fresh.DisposeSource(table));
    EXPECT_EQ(3u, table.FreeCount());
}

TEST(AuthoredStopAudio, PendingControlSourceLoanMaximumCountRefusesWithoutMutation) {
    harken::TimelineArmTable table(2);
    PendingControlRecord h;
    ASSERT_TRUE(h.PrepareSource(table));
    ASSERT_TRUE(h.CommitControl(table));
    ASSERT_TRUE(h.BindSource(table, 120));
    h.Finalize(120);
    auto* row = table.RowAt(h.source.slot);
    ASSERT_EQ(1u, row->frontierReaderPins.load());
    // Saturate the qualified-out count to exercise fail-closed overflow handling.
    row->frontierReaderPins.store(UINT32_MAX);
    harken::TimelineArmTable::PendingAuthoredControlBindingProof binding;
    EXPECT_FALSE(h.Prove(table, &binding));
    EXPECT_FALSE(binding.Valid());
    EXPECT_FALSE(row->originalPlainControl.Valid());
    EXPECT_FALSE(table.RowAt(h.record.token.slot)->pendingControlSourceReader.Valid());
    EXPECT_EQ(UINT32_MAX, row->frontierReaderPins.load());
    row->frontierReaderPins.store(1u);
    ASSERT_TRUE(h.Prove(table, &binding));
    table.ConsumePendingAuthoredControlBinding(std::move(binding));
    ASSERT_NO_FATAL_FAILURE(h.DisposeControl(table));
    ASSERT_NO_FATAL_FAILURE(h.DisposeSource(table));
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(AuthoredStopAudio, PendingControlSourceLoanReturnsBeforeMaximumGenerationQuarantine) {
    for (const bool sourceMaximum : {false, true}) {
        SCOPED_TRACE(sourceMaximum);
        harken::TimelineArmTable table(2);
        PendingControlRecord h;
        ASSERT_TRUE(table.SetFreeGenerationForTests(sourceMaximum ? 0u : 1u, UINT32_MAX));
        ASSERT_TRUE(h.PrepareSource(table));
        ASSERT_TRUE(h.CommitControl(table));
        ASSERT_TRUE(h.BindSource(table, 120));
        h.Finalize(120);
        harken::TimelineArmTable::PendingAuthoredControlBindingProof binding;
        ASSERT_TRUE(h.Prove(table, &binding));
        table.ConsumePendingAuthoredControlBinding(std::move(binding));
        ASSERT_EQ(2u, table.RowAt(h.source.slot)->frontierReaderPins.load());
        ASSERT_TRUE(table.RequestCancel(h.record.token));
        ASSERT_TRUE(table.Tombstone(h.record.token, 10));
        EXPECT_EQ(sourceMaximum, table.Reclaim(h.record.token, 11));
        EXPECT_FALSE(table.RowAt(h.record.token.slot)->pendingControlSourceReader.Valid());
        EXPECT_EQ(1u, table.RowAt(h.source.slot)->frontierReaderPins.load());
        if (!sourceMaximum)
            EXPECT_EQ(harken::ArmState::Quarantined, table.RowAt(h.record.token.slot)->state.load());
        ASSERT_TRUE(table.RequestCancel(h.source));
        ASSERT_TRUE(table.Tombstone(h.source, 10));
        ASSERT_TRUE(h.sourceReader.Release(11));
        EXPECT_EQ(!sourceMaximum, table.Reclaim(h.source, 11));
        if (sourceMaximum)
            EXPECT_EQ(harken::ArmState::Quarantined, table.RowAt(h.source.slot)->state.load());
        EXPECT_EQ(1u, table.FreeCount());
    }
}

TEST(AuthoredStopAudio, PendingControlBindingRefusesForeignReaderOriginalBackingAndWrongCanonicalShape) {
    harken::TimelineArmTable table(2), foreignTable(2);
    PendingControlRecord h, foreign;
    ASSERT_TRUE(h.PrepareSource(table));
    ASSERT_TRUE(foreign.PrepareSource(foreignTable));
    ASSERT_TRUE(h.CommitControl(table));
    ASSERT_TRUE(foreign.CommitControl(foreignTable));
    ASSERT_TRUE(h.BindSource(table, 120));
    ASSERT_TRUE(foreign.BindSource(foreignTable, 120));
    h.Finalize(120);
    foreign.Finalize(120);
    ASSERT_EQ(h.source.generation, foreign.source.generation);
    harken::TimelineArmTable::PendingAuthoredControlBindingProof proof;
    EXPECT_FALSE(table.ProvePendingAuthoredControlBinding(h.record.token, foreign.sourceReader, h.record.input, &proof));
    harken::TimelineArmReaderPin absent;
    EXPECT_FALSE(table.ProvePendingAuthoredControlBinding(h.record.token, absent, h.record.input, &proof));
    for (unsigned field = 0; field < 19; ++field) {
        SCOPED_TRACE(field);
        auto wrong = h.record.input;
        auto copiedProgram = h.record.program;
        auto savedEvents = h.record.events;
        auto savedPayloads = h.record.payloads;
        const auto originalCount = h.record.program.payloadCount;
        switch (field) {
        case 0: wrong.operation = &foreign.record.operation; break;
        case 1: wrong.program.program = &copiedProgram; break;
        case 2: ++wrong.program.programGeneration; break;
        case 3: wrong.events = foreign.record.events.data(); break;
        case 4: wrong.eventCount = 1; break;
        case 5: ++wrong.onsetFrame; break;
        case 6: ++wrong.fadeStartFrame; break;
        case 7: ++wrong.renderSlot; break;
        case 8: ++wrong.renderGeneration; break;
        case 9: ++wrong.target.occurrenceOrdinal; break;
        case 10: wrong.armDeadline = 121; break;
        case 11: ++h.record.events[0].executeFrame; break;
        case 12: ++h.record.events[1].armToken.generation; break;
        case 13: ++h.record.events[1].subeventPhase; break;
        case 14: h.record.events[1].eventIndex = h.record.events[0].eventIndex; break;
        case 15: ++h.record.payloads[1].authoredStopOperation.stopFrame; break;
        case 16: ++h.record.payloads[0].envelopeIndex; break;
        case 17: h.record.program.payloads = foreign.record.payloads.data(); break;
        case 18: h.record.program.payloadCount = 1; break;
        }
        EXPECT_FALSE(table.ProvePendingAuthoredControlBinding(h.record.token, h.sourceReader, wrong, &proof));
        EXPECT_FALSE(proof.Valid());
        EXPECT_FALSE(table.Resolve(h.record.token)->startBound);
        EXPECT_EQ(0u, table.Resolve(h.record.token)->startedBlockSequence.load());
        h.record.events = savedEvents;
        h.record.payloads = savedPayloads;
        h.record.program.payloads = h.record.payloads.data();
        h.record.program.payloadCount = originalCount;
    }
    ASSERT_TRUE(h.Prove(table, &proof));
    proof.Release();
    ASSERT_NO_FATAL_FAILURE(h.DisposeControl(table));
    ASSERT_NO_FATAL_FAILURE(h.DisposeSource(table));
    ASSERT_NO_FATAL_FAILURE(foreign.DisposeControl(foreignTable));
    ASSERT_NO_FATAL_FAILURE(foreign.DisposeSource(foreignTable));
}

TEST(AuthoredStopAudio, OriginalRowOwnerRejectsBothGenuineTypedControlPreparations) {
    harken::TimelineArmTable table(2);
    PendingControlRecord pending;
    ASSERT_TRUE(pending.PrepareSource(table));
    harken::TimelineArmTable::PreparedCommitProof proof;
    ASSERT_TRUE(table.ProvePendingAuthoredControlCommit(pending.record.token,
        pending.pending, &proof));
    harken::TimelineArmTable::OriginalArmRowOwner owner;
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(proof, &owner));
    EXPECT_FALSE(owner.Valid());
    EXPECT_TRUE(proof.Valid());
    harken::TimelineArmTable::PreparedCommitProof moved(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(moved, &owner));
    moved.Release();
    ASSERT_TRUE(table.CancelPrepared(pending.record.token));
    ASSERT_TRUE(table.CancelPrepared(pending.source));

    OriginalControlRecord bound;
    ASSERT_TRUE(table.PrepareAuthoredControl(bound.input, &bound.token));
    harken::TimelineArmTable::PreparedCommitInput expected{};
    expected.arm = bound.token;
    expected.startFrame = std::min(bound.operation.Identity().stopFrame,
        std::max(bound.input.fadeStartFrame, bound.input.onsetFrame));
    expected.armDeadline = bound.input.armDeadline;
    expected.programGeneration = bound.input.program.programGeneration;
    expected.target = bound.input.target;
    ASSERT_TRUE(table.ProvePreparedCommit(expected, &proof));
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(proof, &owner));
    EXPECT_FALSE(owner.Valid());
    EXPECT_TRUE(proof.Valid());
    proof.Release();
    ASSERT_TRUE(table.CancelPrepared(bound.token));
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(AuthoredStopAudio, PendingControlOriginalPreparedSourceCancellationCannotFollowRealReissue) {
    harken::TimelineArmTable table(3);
    PendingControlRecord old, fresh;
    ASSERT_TRUE(old.PrepareSource(table));
    ASSERT_TRUE(old.CommitControl(table));
    const auto cancelled = old.source;
    ASSERT_TRUE(table.CancelPrepared(cancelled));
    ASSERT_TRUE(table.PreparePendingImmediate(cancelled.renderSlot, cancelled.renderGeneration,
        7, old.record.target, &fresh.source));
    ASSERT_EQ(cancelled.slot, fresh.source.slot);
    ASSERT_NE(cancelled.generation, fresh.source.generation);
    ASSERT_TRUE(fresh.PrepareControl(table));
    ASSERT_TRUE(fresh.CommitControl(table));
    ASSERT_TRUE(fresh.BindSource(table, 120));
    old.Finalize(120);
    fresh.Finalize(120);
    harken::TimelineArmTable::PendingAuthoredControlBindingProof proof;
    EXPECT_FALSE(table.ProvePendingAuthoredControlBinding(old.record.token, fresh.sourceReader, old.record.input, &proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_FALSE(table.AcknowledgeApplied(cancelled, 9));
    EXPECT_FALSE(table.CommitPrepared(cancelled));
    EXPECT_FALSE(table.CancelPrepared(cancelled));
    EXPECT_FALSE(table.BindPendingImmediateStart(cancelled, 80, 80));
    EXPECT_EQ(harken::ArmState::Armed, table.Resolve(fresh.source)->state.load());
    EXPECT_FALSE(table.Resolve(old.record.token)->startBound);
    ASSERT_TRUE(fresh.Prove(table, &proof));
    table.ConsumePendingAuthoredControlBinding(std::move(proof));
    ASSERT_NO_FATAL_FAILURE(old.DisposeControl(table));
    ASSERT_NO_FATAL_FAILURE(fresh.DisposeControl(table));
    ASSERT_NO_FATAL_FAILURE(fresh.DisposeSource(table));
    EXPECT_EQ(3u, table.FreeCount());
}

TEST(AuthoredStopAudio, PendingControlBindingAbortMoveAssignmentAndCancelledOwnersKeepOriginalFacts) {
    harken::TimelineArmTable leftTable(2), rightTable(2);
    PendingControlRecord left, right;
    ASSERT_TRUE(left.PrepareSource(leftTable));
    ASSERT_TRUE(right.PrepareSource(rightTable));
    ASSERT_TRUE(left.CommitControl(leftTable));
    ASSERT_TRUE(right.CommitControl(rightTable));
    ASSERT_TRUE(left.BindSource(leftTable, 120));
    ASSERT_TRUE(right.BindSource(rightTable, 120));
    left.Finalize(120);
    right.Finalize(120);
    {
        harken::TimelineArmTable::PendingAuthoredControlBindingProof a, b;
        ASSERT_TRUE(left.Prove(leftTable, &a));
        ASSERT_TRUE(right.Prove(rightTable, &b));
        b = std::move(a);
        EXPECT_FALSE(a.Valid());
        EXPECT_TRUE(b.Valid());
        EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, right.record.operation.Cancel());
        EXPECT_EQ(harken::AuthoredStopCancelDisposition::DeferredApplying, left.record.operation.Cancel());
        EXPECT_FALSE(leftTable.RequestCancel(left.source));
        EXPECT_FALSE(left.sourceReader.Release(11));
    }
    harken::TimelineArmTable::PendingAuthoredControlBindingProof proof;
    EXPECT_FALSE(right.Prove(rightTable, &proof));
    ASSERT_TRUE(leftTable.RequestCancel(left.source));
    EXPECT_FALSE(left.Prove(leftTable, &proof));
    EXPECT_FALSE(leftTable.Resolve(left.record.token)->startBound);
    EXPECT_EQ(0u, left.record.operation.AppliedAudioSequence());
    ASSERT_NO_FATAL_FAILURE(left.DisposeControl(leftTable));
    ASSERT_NO_FATAL_FAILURE(left.DisposeSource(leftTable));
    ASSERT_NO_FATAL_FAILURE(right.DisposeControl(rightTable));
    ASSERT_NO_FATAL_FAILURE(right.DisposeSource(rightTable));
}

TEST(AuthoredStopAudio, PendingControlPreparedMaximumDisposesToQuarantineWithoutWrappingOrCreditRecovery) {
    for (const bool sourceMaximum : {false, true}) {
        harken::TimelineArmTable table(2);
        PendingControlRecord h;
        ASSERT_TRUE(table.SetFreeGenerationForTests(sourceMaximum ? 0u : 1u, UINT32_MAX));
        ASSERT_TRUE(h.PrepareSource(table));
        const auto token = sourceMaximum ? h.source : h.record.token;
        ASSERT_EQ(UINT32_MAX, token.generation);
        ASSERT_TRUE(table.CancelPrepared(token));
        const auto* row = table.RowAt(token.slot);
        EXPECT_EQ(harken::ArmState::Quarantined, row->state.load());
        EXPECT_EQ(UINT32_MAX, row->generation);
        EXPECT_EQ(0u, row->identity.load());
        EXPECT_EQ(nullptr, row->authoredControl.operation);
        EXPECT_EQ(0u, table.FreeCount());
        EXPECT_EQ(1u, table.LiveCount());
        harken::TimelineArmToken refused{};
        EXPECT_FALSE(table.PreparePendingImmediate(9, 11, 7, h.record.target, &refused));
        EXPECT_FALSE(table.CancelPrepared(token));
        if (sourceMaximum) ASSERT_NO_FATAL_FAILURE(h.DisposeControl(table));
        else ASSERT_NO_FATAL_FAILURE(h.DisposeSource(table));
        EXPECT_EQ(1u, table.FreeCount());
        EXPECT_EQ(0u, table.LiveCount());
    }
}

TEST(AuthoredStopAudio, PlainReferenceWithdrawalSourceKeepsActualSiblingPcmAndOriginalRetirementOwners) {
    for (const bool permuted : {false, true}) {
        SCOPED_TRACE(permuted);
        MixerPendingControlHarness h;
        ASSERT_TRUE(h.InstallSource());
        auto& engine = *h.audio.engine;
        auto& table = engine.timelineArms;
        std::array<harken::VoiceReleaseObserver, 2> observers{};
        observers[0] = h.audio.lastObserver;
        std::array<VnVoiceHandle, 2> renders{h.render, h.audio.Play(false, 1, false)};
        observers[1] = h.audio.lastObserver;
        std::array<harken::TimelineArmToken, 2> arms{h.control.source, {}};
        ASSERT_TRUE(table.PreparePendingImmediate(static_cast<std::uint32_t>(renders[1]),
            static_cast<std::uint32_t>(renders[1] >> 32), 8, h.control.record.target, &arms[1]));
        ASSERT_TRUE(table.CommitPrepared(arms[1]));
        harken::CommandRecord header{};
        header.opcode = harken::kOpcodePlay;
        header.version = harken::kCommandVersion;
        header.payload.play.batch = {harken::kPlayLedBatchVersion,
            harken::PlayLedBatchKind::TimelineArm, 0, 1,
            (static_cast<std::uint64_t>(arms[1].generation) << 32) | arms[1].slot};
        header.payload.play.handle = renders[1];
        header.payload.play.sourceIdentity = renders[1];
        ASSERT_TRUE(engine.commands.TryPush(header));
        h.Pump();
        const auto first = (engine.mixer.BlocksRendered() + 2u) * kBlock;
        const auto anchor = first + (permuted ? kBlock : 0u);
        std::array<harken::TimelineEventPayload, 2> payloads{};
        std::array<harken::TimelineProgramView, 2> programs{};
        std::array<harken::ImmediateLaneEvent, 2> events{};
        std::array<harken::SourceTerminalToken, 2> terminals{};
        std::array<harken::PlayableInstanceHandle, 2> leaves{};
        for (std::uint32_t index = 0; index < 2; ++index) {
            auto* voice = engine.voices.Resolve(renders[index]);
            ASSERT_NE(nullptr, voice);
            ASSERT_TRUE(voice->timelineGated);
            ASSERT_NE(nullptr, voice->sourceTerminal);
            terminals[index] = voice->sourceTerminalToken;
            leaves[index] = voice->sourceTerminal->leaf;
            ASSERT_TRUE(observers[index].Valid());
            EXPECT_FALSE(engine.voices.ReleaseObserved(observers[index]));
            payloads[index].kind = harken::TimelineEventKind::Start;
            payloads[index].target = h.control.record.target;
            programs[index] = {7u + index, &payloads[index], 1};
            auto& event = events[index];
            event.timeDomain = index == 0 ? harken::ImmediateLaneTimeDomain::RelativeToAnchor :
                harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
            if (index == 0) event.relativeFrame = permuted ? kBlock : 0u;
            if (index == 1) event.fixedFrame = first + 16;
            event.event.executeFrame = index == 0 ? event.relativeFrame : event.fixedFrame;
            event.event.program = {7u + index, &programs[index]};
            event.event.armToken = arms[index];
            event.event.subeventPhase = 20;
            event.event.ownerInstanceHandle = leaves[index];
            event.event.ownerInstanceScheduleEpoch = voice->sourceTerminal->acceptedEpoch;
            ASSERT_TRUE(table.BindPendingImmediateStart(arms[index],
                index == 0 ? anchor : first + 16, index == 0 ? anchor : first + 16));
        }
        harken::ImmediateLaneHandle lane{};
        ASSERT_TRUE(engine.immediateActionLane.Reserve(41, 2, &lane));
        ASSERT_TRUE(engine.immediateActionLane.Prepare(lane, events.data(), 2));
        ASSERT_TRUE(engine.immediateActionLane.PinArms(lane, table));
        std::uint64_t placed = UINT64_MAX;
        ASSERT_TRUE(engine.immediateActionLane.TryPlace(lane, first, 0, &placed));
        ASSERT_EQ(first, placed);
        EXPECT_FALSE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lane, 41,
            {7, &programs[0]}, arms[0], engine.mixer.BlocksRendered()));
        ASSERT_TRUE(table.Publish(arms[0]));
        ASSERT_TRUE(table.Publish(arms[1]));
        ASSERT_TRUE(engine.immediateActionLane.Publish(lane));
        EXPECT_FALSE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lane, 42,
            {7, &programs[0]}, arms[0], engine.mixer.BlocksRendered()));
        auto staleLane = lane;
        ++staleLane.generation;
        EXPECT_FALSE(engine.immediateActionLane.WithdrawPlainSourceStartReference(staleLane, 41,
            {7, &programs[0]}, arms[0], engine.mixer.BlocksRendered()));
        auto copied = programs[0];
        EXPECT_FALSE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lane, 41,
            {7, &copied}, arms[0], engine.mixer.BlocksRendered()));
        auto wrong = arms[0];
        ++wrong.renderGeneration;
        EXPECT_FALSE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lane, 41,
            {7, &programs[0]}, wrong, engine.mixer.BlocksRendered()));
        ASSERT_TRUE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lane, 41,
            {7, &programs[0]}, arms[0], engine.mixer.BlocksRendered()));
        EXPECT_FALSE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lane, 41,
            {7, &programs[0]}, arms[0], engine.mixer.BlocksRendered()));
        EXPECT_EQ(0u, table.RowAt(arms[0].slot)->frontierReaderPins.load());
        EXPECT_EQ(1u, table.RowAt(arms[1].slot)->frontierReaderPins.load());
        EXPECT_FALSE(engine.immediateActionLane.HasProgramReference(7));
        EXPECT_TRUE(engine.immediateActionLane.HasProgramReference(8));
        EXPECT_EQ(harken::ArmState::Published, table.RowAt(arms[0].slot)->state.load());
        EXPECT_FALSE(engine.voices.ReleaseObserved(observers[0]));
        while (engine.mixer.BlocksRendered() <= anchor / kBlock) {
            const auto quantum = engine.mixer.BlocksRendered();
            h.Pump();
            if (quantum == first / kBlock) {
                const auto* output = engine.nullBackend.LastBlock();
                ASSERT_NE(nullptr, output);
                for (std::uint32_t frame = 0; frame < kBlock; ++frame)
                    EXPECT_NEAR(frame < 16 ? 0.0f : 0.25f, output[2 * frame], 1e-6f);
            }
        }
        harken::TimelineArmObservation source{}, sibling{};
        ASSERT_TRUE(table.Observe(arms[0], &source));
        ASSERT_TRUE(table.Observe(arms[1], &sibling));
        EXPECT_EQ(0u, source.startedBlockSequence);
        EXPECT_EQ(first / kBlock + 1u, sibling.startedBlockSequence);
        ASSERT_NE(nullptr, engine.voices.Resolve(renders[0]));
        EXPECT_TRUE(engine.voices.Resolve(renders[0])->timelineGated);
        EXPECT_EQ(0u, engine.voices.Resolve(renders[0])->srcCursor);
        harken::ImmediateLaneObservation laneState{};
        ASSERT_TRUE(engine.immediateActionLane.Observe(lane, &laneState));
        EXPECT_FALSE(laneState.missed);
        const auto lastRead = table.RowAt(arms[1].slot)->frontierLastReadSequence.load();
        ASSERT_EQ(first / kBlock + 1u, lastRead);
        if (permuted) {
            ASSERT_GT(engine.mixer.BlocksRendered(), lastRead);
        } else {
            ASSERT_EQ(engine.mixer.BlocksRendered(), lastRead);
            EXPECT_FALSE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
            h.Pump();
        }
        ASSERT_TRUE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
        std::array<harken::CommandRecord, 2> stops{};
        for (std::uint32_t index = 0; index < 2; ++index) {
            stops[index].opcode = harken::kOpcodeStop;
            stops[index].version = harken::kCommandVersion;
            stops[index].payload.stop.handle = renders[index];
        }
        ASSERT_TRUE(engine.commands.TryPushBatch(stops.data(), 2));
        h.Pump();
        for (std::uint32_t index = 0; index < 2; ++index) {
            EXPECT_TRUE(engine.voices.ReleaseObserved(observers[index]));
            if (index == 0) {
                ASSERT_EQ(harken::ArmState::Published, table.RowAt(arms[index].slot)->state.load());
                ASSERT_TRUE(table.RequestCancel(arms[index]));
                ASSERT_TRUE(table.Tombstone(arms[index], engine.mixer.BlocksRendered()));
            }
            ASSERT_EQ(harken::ArmState::Tombstoned, table.RowAt(arms[index].slot)->state.load());
            const auto fence = table.RowAt(arms[index].slot)->tombstoneAudioSequence.load();
            EXPECT_FALSE(table.Reclaim(arms[index], fence));
        }
        h.Pump();
        for (std::uint32_t index = 0; index < 2; ++index) {
            ASSERT_TRUE(table.Reclaim(arms[index], engine.mixer.BlocksRendered()));
            harken::SourceTerminalPayload terminal{};
            ASSERT_TRUE(engine.sourceTerminals.Capture(terminals[index], engine.mixer.BlocksRendered(), &terminal));
            EXPECT_EQ(harken::SourceTerminalKind::Stop, terminal.kind);
            ASSERT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(leaves[index]));
            ASSERT_TRUE(engine.sourceTerminals.Release(terminals[index]));
            ASSERT_TRUE(engine.playableInstances.SetTerminalOutcome(leaves[index],
                {harken::PlayableTerminalKind::RequestedStop, VN_OK, 0, 0}));
            ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(leaves[index], harken::PlayableLifecycle::Retired));
            EXPECT_FALSE(engine.playableInstances.Classify(leaves[index]).live);
        }
    }
}

TEST(AuthoredStopAudio, PlainReferenceWithdrawalTypedControlPreservesIndependentReaderAndPhysicalVoice) {
    for (const auto stop : {32u, 100u}) {
        SCOPED_TRACE(stop);
        ControlLaneAudioHarness h(0, stop);
        h.InstallControl();
        auto copiedProgram = h.record.program;
        auto wrongArm = h.record.token;
        ++wrongArm.renderGeneration;
        harken::AuthoredStopOperation foreignOperation(h.record.operation.Identity());
        EXPECT_FALSE(h.lane.WithdrawPlainAuthoredControlReferences(h.control, 18,
            h.record.input.program, h.record.token, h.record.operation, h.audio.quantum));
        EXPECT_FALSE(h.lane.WithdrawPlainAuthoredControlReferences(h.control, 17,
            {h.record.program.generation, &copiedProgram}, h.record.token, h.record.operation, h.audio.quantum));
        EXPECT_FALSE(h.lane.WithdrawPlainAuthoredControlReferences(h.control, 17,
            h.record.input.program, wrongArm, h.record.operation, h.audio.quantum));
        EXPECT_FALSE(h.lane.WithdrawPlainAuthoredControlReferences(h.control, 17,
            h.record.input.program, h.record.token, foreignOperation, h.audio.quantum));
        EXPECT_EQ(3u, h.arms.RowAt(h.record.token.slot)->frontierReaderPins.load());
        harken::TimelineArmTable::AuthoredControlBucketProof independent;
        ASSERT_TRUE(h.record.Prove(h.arms, 0, kBlock, 0, stop == 32 ? 2u : 1u, &independent));
        // The independent proof owns the real row gate; this return must not take it.
        ASSERT_TRUE(h.lane.WithdrawPlainAuthoredControlReferences(h.control, 17,
            h.record.input.program, h.record.token, h.record.operation, h.audio.quantum));
        EXPECT_FALSE(h.lane.WithdrawPlainAuthoredControlReferences(h.control, 17,
            h.record.input.program, h.record.token, h.record.operation, h.audio.quantum));
        EXPECT_TRUE(independent.Valid());
        EXPECT_TRUE(h.record.reader.Valid());
        EXPECT_EQ(1u, h.arms.RowAt(h.record.token.slot)->frontierReaderPins.load());
        EXPECT_EQ(harken::ArmState::Published, h.arms.RowAt(h.record.token.slot)->state.load());
        EXPECT_FALSE(h.lane.HasProgramReference(h.record.program.generation));
        independent.Release();
        const auto allocations = harken::memguard::HitCount();
        do { h.Step(); } while (h.audio.quantum <= stop / kBlock);
        EXPECT_EQ(allocations, harken::memguard::HitCount());
        EXPECT_EQ(stop / kBlock + 1u, h.record.operation.AppliedAudioSequence());
        for (std::uint32_t frame = 0; frame < kBlock; ++frame)
            EXPECT_NEAR(0.25f, h.audio.Sample(frame), 1e-6f);
        ASSERT_NE(nullptr, h.audio.engine->voices.Resolve(h.render));
        EXPECT_FALSE(h.audio.engine->voices.ReleaseObserved(h.audio.lastObserver));
        h.audio.engine->voices.ApplyStop(h.render, 0, 0, h.audio.engine->notifications, h.audio.quantum);
        h.Retire();
    }
}

TEST(AuthoredStopAudio, PlainReferenceWithdrawalCaptureWinnerKeepsWholeControlScopeAndTombstonedPins) {
    ControlLaneAudioHarness h(0);
    h.InstallControl();
    struct Gate { std::atomic<bool> entered{false}, release{false}; } gate;
    h.lane.SetEventAdmissionHookForTest([](void* context, std::uint32_t index) {
        if (index != 0) return;
        auto& gate = *static_cast<Gate*>(context);
        gate.entered.store(true, std::memory_order_release);
        while (!gate.release.load(std::memory_order_acquire)) std::this_thread::yield();
    }, &gate);
    std::thread audio([&] { h.Step(); });
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!gate.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < limit)
        std::this_thread::yield();
    const bool entered = gate.entered.load(std::memory_order_acquire);
    if (entered) EXPECT_FALSE(h.lane.WithdrawPlainAuthoredControlReferences(h.control, 17,
        h.record.input.program, h.record.token, h.record.operation, 0));
    gate.release.store(true, std::memory_order_release);
    audio.join();
    h.lane.SetEventAdmissionHookForTest(nullptr, nullptr);
    ASSERT_TRUE(entered);
    EXPECT_EQ(1u, h.record.operation.AppliedAudioSequence());
    EXPECT_EQ(harken::ArmState::Tombstoned, h.arms.RowAt(h.record.token.slot)->state.load());
    EXPECT_EQ(3u, h.arms.RowAt(h.record.token.slot)->frontierReaderPins.load());
    EXPECT_FALSE(h.lane.WithdrawPlainAuthoredControlReferences(h.control, 17,
        h.record.input.program, h.record.token, h.record.operation, h.audio.quantum));
    h.audio.Begin(); h.audio.Render();
    ASSERT_TRUE(h.lane.WithdrawPlainAuthoredControlReferences(h.control, 17,
        h.record.input.program, h.record.token, h.record.operation, h.audio.quantum));
    EXPECT_EQ(1u, h.arms.RowAt(h.record.token.slot)->frontierReaderPins.load());
    ASSERT_TRUE(h.lane.Retire(h.control, h.audio.quantum));
    ASSERT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.record.operation.Cancel());
    ASSERT_TRUE(h.record.reader.Release(h.audio.quantum));
    ASSERT_TRUE(h.arms.Reclaim(h.record.token, h.audio.quantum));
    for (unsigned block = 0; block < 16; ++block) { h.audio.Begin(); h.audio.Render(); }
    EXPECT_TRUE(h.audio.engine->voices.ReleaseObserved(h.audio.lastObserver));
    EXPECT_EQ(3u, h.arms.FreeCount());
}

TEST(AuthoredStopAudio, PlainReferenceWithdrawalLiteralStopBeforeBoundStartNeverStartsDisposedSource) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.InstallControl());
    h.Pump();
    auto& engine = *h.audio.engine;
    auto& table = engine.timelineArms;
    auto* voice = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, voice);
    ASSERT_NE(nullptr, voice->sourceTerminal);
    const auto terminalToken = voice->sourceTerminalToken;
    const auto leaf = voice->sourceTerminal->leaf;
    const auto epoch = voice->sourceTerminal->acceptedEpoch;
    ASSERT_TRUE(table.BindPendingImmediateStart(h.control.source, 320, 320));
    ASSERT_TRUE(table.PinFrontierReader(h.control.source, &h.control.sourceReader));
    h.control.Finalize(320);
    harken::TimelineArmTable::PendingAuthoredControlBindingProof binding;
    ASSERT_TRUE(h.control.Prove(table, &binding));
    table.ConsumePendingAuthoredControlBinding(std::move(binding));
    ASSERT_TRUE(table.PinFrontierReader(h.control.record.token, &h.control.record.reader));
    ASSERT_EQ(1u, h.control.record.program.payloadCount);
    ASSERT_EQ(300u, h.control.record.events[0].executeFrame);
    FenceRecord fence(h.control.record.operation);
    fence.payload.authoredStopOperation = h.control.record.operation.Identity();
    harken::TimelineEventPayload startPayload{};
    startPayload.kind = harken::TimelineEventKind::Start;
    startPayload.target = h.control.record.target;
    harken::TimelineProgramView startProgram{7, &startPayload, 1};
    std::array<harken::ImmediateLaneEvent, 3> events{};
    events[0].timeDomain = harken::ImmediateLaneTimeDomain::RelativeToAnchor;
    events[0].relativeFrame = 64;
    events[0].event.executeFrame = 64;
    events[0].event.program = {7, &startProgram};
    events[0].event.armToken = h.control.source;
    events[0].event.subeventPhase = 20;
    events[0].event.ownerInstanceHandle = leaf;
    events[0].event.ownerInstanceScheduleEpoch = epoch;
    events[1].timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    events[1].fixedFrame = 300;
    events[1].event = LaneControlInput(h.control.record.events[0], h.control.record.input.program);
    events[2].timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    events[2].fixedFrame = 300;
    events[2].event = LaneControlInput(fence.event, fence.request.program);
    events[2].event.eventIndex = 90;
    harken::ImmediateLaneHandle lane{};
    ASSERT_TRUE(engine.immediateActionLane.Reserve(41, 3, &lane));
    ASSERT_TRUE(engine.immediateActionLane.Prepare(lane, events.data(), 3));
    ASSERT_TRUE(engine.immediateActionLane.PinArms(lane, table));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(engine.immediateActionLane.TryPlace(lane, 256, 0, &anchor));
    ASSERT_EQ(256u, anchor);
    ASSERT_TRUE(table.Publish(h.control.source));
    ASSERT_TRUE(table.Publish(h.control.record.token));
    ASSERT_TRUE(engine.immediateActionLane.Publish(lane));
    while (engine.mixer.BlocksRendered() <= 300 / kBlock) {
        h.Pump();
        const auto* output = engine.nullBackend.LastBlock();
        ASSERT_NE(nullptr, output);
        for (std::uint32_t sample = 0; sample < 2 * kBlock; ++sample)
            EXPECT_FLOAT_EQ(0.0f, output[sample]);
    }
    EXPECT_EQ(5u, h.control.record.operation.AppliedAudioSequence());
    EXPECT_EQ(0u, table.RowAt(h.control.source.slot)->startedBlockSequence.load());
    EXPECT_EQ(5u, table.RowAt(h.control.record.token.slot)->logicalCompletionSequence.load());
    ASSERT_TRUE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lane, 41,
        {7, &startProgram}, h.control.source, engine.mixer.BlocksRendered()));
    h.Pump();
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(engine.immediateActionLane.Observe(lane, &observation));
    EXPECT_FALSE(observation.missed);
    EXPECT_EQ(0u, table.RowAt(h.control.source.slot)->startedBlockSequence.load());
    ASSERT_TRUE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
    ASSERT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.control.record.operation.Cancel());
    ASSERT_TRUE(h.control.record.reader.Release(engine.mixer.BlocksRendered()));
    ASSERT_TRUE(h.control.sourceReader.Release(engine.mixer.BlocksRendered()));
    ASSERT_NO_FATAL_FAILURE(h.Dispose());
    harken::SourceTerminalPayload terminal{};
    ASSERT_TRUE(engine.sourceTerminals.Capture(terminalToken, engine.mixer.BlocksRendered(), &terminal));
    EXPECT_EQ(harken::SourceTerminalKind::Stop, terminal.kind);
    ASSERT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(leaf));
    ASSERT_TRUE(engine.sourceTerminals.Release(terminalToken));
    ASSERT_TRUE(engine.playableInstances.SetTerminalOutcome(leaf,
        {harken::PlayableTerminalKind::RequestedStop, VN_OK, 0, 0}));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(leaf, harken::PlayableLifecycle::Retired));
}

TEST(AuthoredStopAudio, PendingControlActualStopCancellationBeforeFLeavesOriginalStartAudible) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.InstallControl());
    h.Pump();
    auto& engine = *h.audio.engine;
    auto& table = engine.timelineArms;
    auto* voice = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, voice);
    ASSERT_NE(nullptr, voice->sourceTerminal);
    const auto terminalToken = voice->sourceTerminalToken;
    const auto leaf = voice->sourceTerminal->leaf;
    const auto epoch = voice->sourceTerminal->acceptedEpoch;
    ASSERT_TRUE(table.BindPendingImmediateStart(h.control.source, 320, 320));
    ASSERT_TRUE(table.PinFrontierReader(h.control.source, &h.control.sourceReader));
    h.control.Finalize(320);
    harken::TimelineArmTable::PendingAuthoredControlBindingProof binding;
    ASSERT_TRUE(h.control.Prove(table, &binding));
    table.ConsumePendingAuthoredControlBinding(std::move(binding));
    ASSERT_TRUE(table.PinFrontierReader(h.control.record.token, &h.control.record.reader));
    FenceRecord fence(h.control.record.operation);
    fence.payload.authoredStopOperation = h.control.record.operation.Identity();
    harken::TimelineEventPayload startPayload{};
    startPayload.kind = harken::TimelineEventKind::Start;
    startPayload.target = h.control.record.target;
    harken::TimelineProgramView startProgram{7, &startPayload, 1};
    std::array<harken::ImmediateLaneEvent, 3> events{};
    events[0].timeDomain = harken::ImmediateLaneTimeDomain::RelativeToAnchor;
    events[0].relativeFrame = 64;
    events[0].event.executeFrame = 64;
    events[0].event.program = {7, &startProgram};
    events[0].event.armToken = h.control.source;
    events[0].event.subeventPhase = 20;
    events[0].event.ownerInstanceHandle = leaf;
    events[0].event.ownerInstanceScheduleEpoch = epoch;
    events[1].timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    events[1].fixedFrame = 300;
    events[1].event = LaneControlInput(h.control.record.events[0], h.control.record.input.program);
    events[2].timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    events[2].fixedFrame = 300;
    events[2].event = LaneControlInput(fence.event, fence.request.program);
    events[2].event.eventIndex = 90;
    harken::ImmediateLaneHandle lane{};
    ASSERT_TRUE(engine.immediateActionLane.Reserve(41, 3, &lane));
    ASSERT_TRUE(engine.immediateActionLane.Prepare(lane, events.data(), 3));
    ASSERT_TRUE(engine.immediateActionLane.PinArms(lane, table));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(engine.immediateActionLane.TryPlace(lane, 256, 0, &anchor));
    ASSERT_EQ(256u, anchor);
    ASSERT_TRUE(table.Publish(h.control.source));
    ASSERT_TRUE(table.Publish(h.control.record.token));
    ASSERT_TRUE(engine.immediateActionLane.Publish(lane));
    // Withdraw the original Stop and None owners before F; retain the original Start.
    ASSERT_TRUE(engine.immediateActionLane.WithdrawPlainAuthoredControlReferences(lane, 41,
        h.control.record.input.program, h.control.record.token, h.control.record.operation,
        engine.mixer.BlocksRendered()));
    ASSERT_TRUE(engine.immediateActionLane.WithdrawPlainNoneFence(lane, 41,
        fence.request.program, fence.descriptor, engine.mixer.BlocksRendered()));
    ASSERT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.control.record.operation.Cancel());
    ASSERT_TRUE(table.RequestCancel(h.control.record.token));
    ASSERT_TRUE(table.Tombstone(h.control.record.token, engine.mixer.BlocksRendered()));
    ASSERT_TRUE(h.control.record.reader.Release(engine.mixer.BlocksRendered()));
    h.Pump();
    ASSERT_TRUE(table.Reclaim(h.control.record.token, engine.mixer.BlocksRendered()));
    h.control.record.token = {};
    bool heard = false;
    while (engine.mixer.BlocksRendered() < 9u) {
        const auto begin = engine.mixer.BlocksRendered() * kBlock;
        h.Pump();
        const auto* output = engine.nullBackend.LastBlock();
        ASSERT_NE(nullptr, output);
        if (begin < 384u) continue;
        for (std::uint32_t sample = kBlock - 16u; sample < kBlock; ++sample)
            for (std::uint32_t channel = 0; channel < engine.nullBackend.Channels(); ++channel)
                EXPECT_NEAR(0.25f, output[sample * engine.nullBackend.Channels() + channel], 1e-6f);
        heard = true;
    }
    EXPECT_TRUE(heard);
    EXPECT_EQ(6u, table.RowAt(h.control.source.slot)->startedBlockSequence.load());
    EXPECT_EQ(0u, table.RowAt(h.control.source.slot)->appliedPlainStopSequence.load());
    EXPECT_EQ(0u, h.control.record.operation.AppliedAudioSequence());
    EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    harken::ImmediateLaneObservation observation{};
    ASSERT_TRUE(engine.immediateActionLane.Observe(lane, &observation));
    EXPECT_TRUE(observation.complete);
    EXPECT_FALSE(observation.missed);
    EXPECT_FALSE(observation.protocolRefused);
    EXPECT_EQ(0u, engine.mixer.CommandProtocolViolationCount());
    ASSERT_TRUE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
    ASSERT_TRUE(h.control.sourceReader.Release(engine.mixer.BlocksRendered()));
    ASSERT_NO_FATAL_FAILURE(h.Dispose());
    harken::SourceTerminalPayload terminal{};
    ASSERT_TRUE(engine.sourceTerminals.Capture(terminalToken, engine.mixer.BlocksRendered(), &terminal));
    EXPECT_EQ(harken::SourceTerminalKind::Stop, terminal.kind);
    ASSERT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(leaf));
    ASSERT_TRUE(engine.sourceTerminals.Release(terminalToken));
    ASSERT_TRUE(engine.playableInstances.SetTerminalOutcome(leaf,
        {harken::PlayableTerminalKind::RequestedStop, VN_OK, 0, 0}));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(leaf, harken::PlayableLifecycle::Retired));
}

TEST(AuthoredStopAudio, PlainReferenceWithdrawalSourceCaptureWinnerRetainsRealStartUntilGreaterCompletion) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    auto& engine = *h.audio.engine;
    auto& table = engine.timelineArms;
    const auto frame = (engine.mixer.BlocksRendered() + 2u) * kBlock;
    const auto* voice = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, voice);
    ASSERT_NE(nullptr, voice->sourceTerminal);
    const auto terminalToken = voice->sourceTerminalToken;
    const auto leaf = voice->sourceTerminal->leaf;
    harken::TimelineEventPayload payload{};
    payload.kind = harken::TimelineEventKind::Start;
    payload.target = h.control.record.target;
    harken::TimelineProgramView program{7, &payload, 1};
    harken::ImmediateLaneEvent event{};
    event.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    event.fixedFrame = frame;
    event.event.executeFrame = frame;
    event.event.program = {7, &program};
    event.event.armToken = h.control.source;
    event.event.subeventPhase = 20;
    event.event.ownerInstanceHandle = leaf;
    event.event.ownerInstanceScheduleEpoch = voice->sourceTerminal->acceptedEpoch;
    ASSERT_TRUE(table.BindPendingImmediateStart(h.control.source, frame, frame));
    harken::ImmediateLaneHandle lane{};
    ASSERT_TRUE(engine.immediateActionLane.Reserve(41, 1, &lane));
    ASSERT_TRUE(engine.immediateActionLane.Prepare(lane, &event, 1));
    ASSERT_TRUE(engine.immediateActionLane.PinArms(lane, table));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(engine.immediateActionLane.TryPlace(lane, frame, 0, &anchor));
    ASSERT_TRUE(table.Publish(h.control.source));
    ASSERT_TRUE(engine.immediateActionLane.Publish(lane));
    while (engine.mixer.BlocksRendered() < frame / kBlock) h.Pump();
    struct Gate { std::atomic<bool> entered{false}, release{false}; } gate;
    engine.immediateActionLane.SetEventAdmissionHookForTest([](void* context, std::uint32_t index) {
        if (index != 0) return;
        auto& gate = *static_cast<Gate*>(context);
        gate.entered.store(true, std::memory_order_release);
        while (!gate.release.load(std::memory_order_acquire)) std::this_thread::yield();
    }, &gate);
    const auto before = engine.mixer.BlocksRendered();
    const auto allocations = harken::memguard::HitCount();
    std::thread audio([&] { h.Pump(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!gate.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool entered = gate.entered.load(std::memory_order_acquire);
    if (entered) EXPECT_FALSE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lane, 41,
        {7, &program}, h.control.source, before));
    gate.release.store(true, std::memory_order_release);
    audio.join();
    engine.immediateActionLane.SetEventAdmissionHookForTest(nullptr, nullptr);
    ASSERT_TRUE(entered);
    EXPECT_EQ(allocations, harken::memguard::HitCount());
    EXPECT_EQ(before + 1u, table.RowAt(h.control.source.slot)->startedBlockSequence.load());
    EXPECT_EQ(1u, table.RowAt(h.control.source.slot)->frontierReaderPins.load());
    EXPECT_TRUE(engine.immediateActionLane.HasProgramReference(7));
    const auto* output = engine.nullBackend.LastBlock();
    ASSERT_NE(nullptr, output);
    for (std::uint32_t sample = 0; sample < 2 * kBlock; ++sample)
        EXPECT_NEAR(0.25f, output[sample], 1e-6f);
    EXPECT_FALSE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lane, 41,
        {7, &program}, h.control.source, engine.mixer.BlocksRendered()));
    EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    h.Pump();
    ASSERT_TRUE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lane, 41,
        {7, &program}, h.control.source, engine.mixer.BlocksRendered()));
    EXPECT_EQ(0u, table.RowAt(h.control.source.slot)->frontierReaderPins.load());
    EXPECT_FALSE(engine.immediateActionLane.HasProgramReference(7));
    ASSERT_TRUE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
    ASSERT_NO_FATAL_FAILURE(h.Dispose());
    harken::SourceTerminalPayload terminal{};
    ASSERT_TRUE(engine.sourceTerminals.Capture(terminalToken, engine.mixer.BlocksRendered(), &terminal));
    EXPECT_EQ(harken::SourceTerminalKind::Stop, terminal.kind);
    ASSERT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(leaf));
    ASSERT_TRUE(engine.sourceTerminals.Release(terminalToken));
    ASSERT_TRUE(engine.playableInstances.SetTerminalOutcome(leaf,
        {harken::PlayableTerminalKind::RequestedStop, VN_OK, 0, 0}));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(leaf, harken::PlayableLifecycle::Retired));
}

TEST(AuthoredStopAudio, PlainReferenceWithdrawalFreedSourceBackingKeepsOtherLaneAndReissuedSlotFresh) {
    MixerPendingControlHarness h;
    ASSERT_TRUE(h.InstallSource());
    auto& engine = *h.audio.engine;
    auto& table = engine.timelineArms;
    const auto source = h.control.source;
    std::array<VnVoiceHandle, 2> renders{h.render, 0};
    std::array<harken::TimelineArmToken, 2> arms{source, {}};
    std::array<harken::VoiceReleaseObserver, 2> observers{h.audio.lastObserver, {}};
    std::array<harken::SourceTerminalToken, 2> terminals{};
    std::array<harken::PlayableInstanceHandle, 2> leaves{};
    const auto* originalVoice = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, originalVoice);
    ASSERT_NE(nullptr, originalVoice->sourceTerminal);
    terminals[0] = originalVoice->sourceTerminalToken;
    leaves[0] = originalVoice->sourceTerminal->leaf;
    const auto frame = (engine.mixer.BlocksRendered() + 2u) * kBlock;
    ASSERT_TRUE(table.BindPendingImmediateStart(source, frame, frame));
    auto firstPayload = std::make_unique<harken::TimelineEventPayload>();
    firstPayload->kind = harken::TimelineEventKind::Start;
    firstPayload->target = h.control.record.target;
    auto firstProgram = std::make_unique<harken::TimelineProgramView>(
        harken::TimelineProgramView{7, firstPayload.get(), 1});
    const harken::TimelineTransactionProgramRef oldProgram{7, firstProgram.get()};
    auto survivorPayload = *firstPayload;
    harken::TimelineProgramView survivorProgram{9, &survivorPayload, 1};
    harken::ImmediateLaneEvent event{};
    event.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    event.fixedFrame = frame;
    event.event.executeFrame = frame;
    event.event.armToken = source;
    event.event.subeventPhase = 20;
    event.event.ownerInstanceHandle = leaves[0];
    event.event.ownerInstanceScheduleEpoch = originalVoice->sourceTerminal->acceptedEpoch;
    std::array<harken::ImmediateLaneHandle, 2> lanes{};
    for (std::uint32_t index = 0; index < 2; ++index) {
        event.event.program = index == 0 ? oldProgram : harken::TimelineTransactionProgramRef{9, &survivorProgram};
        ASSERT_TRUE(engine.immediateActionLane.Reserve(41u + index, 1, &lanes[index]));
        ASSERT_TRUE(engine.immediateActionLane.Prepare(lanes[index], &event, 1));
        ASSERT_TRUE(engine.immediateActionLane.PinArms(lanes[index], table));
        std::uint64_t anchor = UINT64_MAX;
        ASSERT_TRUE(engine.immediateActionLane.TryPlace(lanes[index], frame, 0, &anchor));
    }
    ASSERT_TRUE(table.Publish(source));
    ASSERT_TRUE(engine.immediateActionLane.Publish(lanes[0]));
    ASSERT_TRUE(engine.immediateActionLane.Publish(lanes[1]));
    ASSERT_EQ(2u, table.RowAt(source.slot)->frontierReaderPins.load());
    ASSERT_TRUE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lanes[0], 41,
        oldProgram, source, engine.mixer.BlocksRendered()));
    EXPECT_EQ(1u, table.RowAt(source.slot)->frontierReaderPins.load());
    EXPECT_FALSE(engine.immediateActionLane.HasProgramReference(7));
    EXPECT_TRUE(engine.immediateActionLane.HasProgramReference(9));
    firstProgram.reset();
    firstPayload.reset();
    EXPECT_FALSE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lanes[0], 41,
        oldProgram, source, engine.mixer.BlocksRendered()));
    const auto allocations = harken::memguard::HitCount();
    while (engine.mixer.BlocksRendered() <= frame / kBlock) h.Pump();
    EXPECT_EQ(allocations, harken::memguard::HitCount());
    EXPECT_EQ(frame / kBlock + 1u, table.RowAt(source.slot)->startedBlockSequence.load());
    const auto* output = engine.nullBackend.LastBlock();
    ASSERT_NE(nullptr, output);
    for (std::uint32_t sample = 0; sample < 2 * kBlock; ++sample)
        EXPECT_NEAR(0.25f, output[sample], 1e-6f);
    ASSERT_TRUE(engine.immediateActionLane.Retire(lanes[0], engine.mixer.BlocksRendered()));
    EXPECT_FALSE(engine.immediateActionLane.Retire(lanes[1], engine.mixer.BlocksRendered()));
    h.Pump();
    ASSERT_TRUE(engine.immediateActionLane.Retire(lanes[1], engine.mixer.BlocksRendered()));
    renders[1] = h.audio.Play(false, 1, false);
    observers[1] = h.audio.lastObserver;
    ASSERT_TRUE(table.PreparePendingImmediate(static_cast<std::uint32_t>(renders[1]),
        static_cast<std::uint32_t>(renders[1] >> 32), 10, h.control.record.target, &arms[1]));
    ASSERT_TRUE(table.CommitPrepared(arms[1]));
    harken::CommandRecord header{};
    header.opcode = harken::kOpcodePlay;
    header.version = harken::kCommandVersion;
    header.payload.play.batch = {harken::kPlayLedBatchVersion, harken::PlayLedBatchKind::TimelineArm,
        0, 1, (static_cast<std::uint64_t>(arms[1].generation) << 32) | arms[1].slot};
    header.payload.play.handle = renders[1];
    header.payload.play.sourceIdentity = renders[1];
    ASSERT_TRUE(engine.commands.TryPush(header));
    h.Pump();
    const auto* newVoice = engine.voices.Resolve(renders[1]);
    ASSERT_NE(nullptr, newVoice);
    ASSERT_NE(nullptr, newVoice->sourceTerminal);
    terminals[1] = newVoice->sourceTerminalToken;
    leaves[1] = newVoice->sourceTerminal->leaf;
    const auto nextFrame = (engine.mixer.BlocksRendered() + 1u) * kBlock;
    ASSERT_TRUE(table.BindPendingImmediateStart(arms[1], nextFrame, nextFrame));
    harken::TimelineEventPayload newPayload{};
    newPayload.kind = harken::TimelineEventKind::Start;
    newPayload.target = h.control.record.target;
    harken::TimelineProgramView newProgram{11, &newPayload, 1};
    event.fixedFrame = nextFrame;
    event.event.executeFrame = nextFrame;
    event.event.program = {11, &newProgram};
    event.event.armToken = arms[1];
    event.event.ownerInstanceHandle = leaves[1];
    event.event.ownerInstanceScheduleEpoch = newVoice->sourceTerminal->acceptedEpoch;
    harken::ImmediateLaneHandle reissued{};
    ASSERT_TRUE(engine.immediateActionLane.Reserve(43, 1, &reissued));
    ASSERT_EQ(lanes[0].slot, reissued.slot);
    ASSERT_NE(lanes[0].generation, reissued.generation);
    ASSERT_TRUE(engine.immediateActionLane.Prepare(reissued, &event, 1));
    ASSERT_TRUE(engine.immediateActionLane.PinArms(reissued, table));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(engine.immediateActionLane.TryPlace(reissued, nextFrame, 0, &anchor));
    ASSERT_TRUE(table.Publish(arms[1]));
    ASSERT_TRUE(engine.immediateActionLane.Publish(reissued));
    EXPECT_FALSE(engine.immediateActionLane.WithdrawPlainSourceStartReference(lanes[0], 41,
        oldProgram, source, engine.mixer.BlocksRendered()));
    EXPECT_TRUE(engine.immediateActionLane.HasProgramReference(11));
    while (engine.mixer.BlocksRendered() <= nextFrame / kBlock) h.Pump();
    EXPECT_EQ(nextFrame / kBlock + 1u, table.RowAt(arms[1].slot)->startedBlockSequence.load());
    output = engine.nullBackend.LastBlock();
    ASSERT_NE(nullptr, output);
    for (std::uint32_t sample = 0; sample < 2 * kBlock; ++sample)
        EXPECT_NEAR(0.5f, output[sample], 1e-6f);
    EXPECT_FALSE(engine.immediateActionLane.Retire(reissued, engine.mixer.BlocksRendered()));
    h.Pump();
    ASSERT_TRUE(engine.immediateActionLane.Retire(reissued, engine.mixer.BlocksRendered()));
    std::array<harken::CommandRecord, 2> stops{};
    for (std::uint32_t index = 0; index < 2; ++index) {
        stops[index].opcode = harken::kOpcodeStop;
        stops[index].version = harken::kCommandVersion;
        stops[index].payload.stop.handle = renders[index];
    }
    ASSERT_TRUE(engine.commands.TryPushBatch(stops.data(), 2));
    h.Pump();
    for (std::uint32_t index = 0; index < 2; ++index) {
        EXPECT_TRUE(engine.voices.ReleaseObserved(observers[index]));
        ASSERT_EQ(harken::ArmState::Tombstoned, table.RowAt(arms[index].slot)->state.load());
        EXPECT_FALSE(table.Reclaim(arms[index], table.RowAt(arms[index].slot)->tombstoneAudioSequence.load()));
    }
    h.Pump();
    for (std::uint32_t index = 0; index < 2; ++index) {
        ASSERT_TRUE(table.Reclaim(arms[index], engine.mixer.BlocksRendered()));
        harken::SourceTerminalPayload terminal{};
        ASSERT_TRUE(engine.sourceTerminals.Capture(terminals[index], engine.mixer.BlocksRendered(), &terminal));
        EXPECT_EQ(harken::SourceTerminalKind::Stop, terminal.kind);
        ASSERT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(leaves[index]));
        ASSERT_TRUE(engine.sourceTerminals.Release(terminals[index]));
        ASSERT_TRUE(engine.playableInstances.SetTerminalOutcome(leaves[index],
            {harken::PlayableTerminalKind::RequestedStop, VN_OK, 0, 0}));
        ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(leaves[index], harken::PlayableLifecycle::Retired));
    }
    EXPECT_EQ(table.Capacity(), table.FreeCount());
}

namespace {

struct OriginalControlHeaderHarness : MixerPendingControlHarness {
    bool Prepare() {
        auto& engine = *audio.engine;
        control.record.input.renderSlot = static_cast<std::uint32_t>(render);
        control.record.input.renderGeneration = static_cast<std::uint32_t>(render >> 32);
        if (!control.PrepareControl(engine.timelineArms) ||
            !engine.timelineArms.ProvePendingAuthoredControlCommit(
                control.record.token, control.pending, &coreCommit) ||
            !engine.timelineArms.IssueOriginalControlHeaderRowOwner(coreCommit, &original)) return false;
        header = {};
        header.opcode = harken::kOpcodePlay;
        header.version = harken::kCommandVersion;
        header.payload.play.batch = {harken::kPlayLedBatchVersion,
            harken::PlayLedBatchKind::TimelineControlArm, 0, 1,
            (static_cast<std::uint64_t>(control.record.token.generation) << 32) |
                control.record.token.slot};
        header.payload.play.handle = render;
        header.payload.play.sourceIdentity = render;
        return true;
    }

    bool Freeze(const harken::CommandRecord& record) {
        auto& queue = audio.engine->commands;
        return queue.ReserveBatch(1, &reservation) &&
            queue.ProvePreparedBatch(&reservation, &record, 1, &queueCommit);
    }

    bool Attach() {
        auto& resource = audio.engine->authoredStopResources[0];
        return audio.engine->commands.IssueOriginalControlHeaderRange(
            queueCommit, 0, std::move(original), &resource, &resource.originalControlHeader);
    }

    bool CommitAndPublish() {
        auto& engine = *audio.engine;
        if (!coreCommit.Valid() || !queueCommit.Valid()) return false;
        engine.timelineArms.ConsumePreparedCommit(std::move(coreCommit));
        (void)engine.commands.ConsumePreparedBatch(std::move(queueCommit));
        return true;
    }

    harken::CommandQueue::OriginalBatchRange& Range() {
        return audio.engine->authoredStopResources[0].originalControlHeader;
    }

    harken::TimelineArmTable::PreparedCommitProof coreCommit;
    harken::TimelineArmTable::OriginalControlHeaderRowOwner original;
    harken::CommandQueue::BatchReservation reservation{};
    harken::CommandQueue::PreparedBatchProof queueCommit;
    harken::CommandRecord header{};
};

} // namespace

TEST(AuthoredStopAudio, OriginalControlHeaderFactoryKeepsTypedLifetimeSeparateFromSourcePurpose) {
    using Owner = harken::TimelineArmTable::OriginalControlHeaderRowOwner;
    static_assert(!std::is_copy_constructible_v<Owner>);
    harken::TimelineArmTable table(2), foreign(2);
    PendingControlRecord record;
    ASSERT_TRUE(record.PrepareSource(table));
    harken::TimelineArmTable::PreparedCommitInput source{};
    source.arm = record.source;
    source.programGeneration = 7;
    source.target = record.record.target;
    source.pendingImmediateStart = true;
    harken::TimelineArmTable::PreparedCommitProof sourceProof;
    ASSERT_TRUE(table.ProvePreparedCommit(source, &sourceProof));
    Owner rejected;
    EXPECT_FALSE(table.IssueOriginalControlHeaderRowOwner(sourceProof, &rejected));
    sourceProof.Release();
    harken::TimelineArmTable::PreparedCommitProof controlProof;
    ASSERT_TRUE(table.ProvePendingAuthoredControlCommit(record.record.token, record.pending, &controlProof));
    EXPECT_FALSE(foreign.IssueOriginalControlHeaderRowOwner(controlProof, &rejected));
    harken::TimelineArmTable::OriginalArmRowOwner sourceOwner;
    EXPECT_FALSE(table.IssueOriginalArmRowOwner(controlProof, &sourceOwner));
    Owner owner;
    ASSERT_TRUE(table.IssueOriginalControlHeaderRowOwner(controlProof, &owner));
    EXPECT_EQ(&table, owner.Owner());
    EXPECT_EQ(record.record.token.renderGeneration, owner.Token().renderGeneration);
    EXPECT_FALSE(table.IssueOriginalControlHeaderRowOwner(controlProof, &rejected));
    Owner moved(std::move(owner));
    EXPECT_FALSE(owner.Valid());
    EXPECT_TRUE(moved.Valid());
    controlProof.Release();
    EXPECT_FALSE(table.CancelPrepared(record.record.token));
    moved.Release();
    ASSERT_TRUE(table.CancelPrepared(record.record.token));
    ASSERT_TRUE(table.CancelPrepared(record.source));
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(AuthoredStopAudio, OriginalControlHeaderFrozenFactoryRejectsForeignHeaderAndAbortsRealUnpublishedRange) {
    for (unsigned field = 0; field < 9; ++field) {
        SCOPED_TRACE(field);
        OriginalControlHeaderHarness h;
        ASSERT_TRUE(h.InstallSource());
        ASSERT_TRUE(h.Prepare());
        auto wrong = h.header;
        switch (field) {
        case 0: wrong.flags = 1; break;
        case 1: wrong.payload.play.batch.batchKind = harken::PlayLedBatchKind::TimelineArm; break;
        case 2: ++wrong.payload.play.batch.batchCommandCount; break;
        case 3: ++wrong.payload.play.batch.armToken; break;
        case 4: ++wrong.payload.play.handle; break;
        case 5: ++wrong.payload.play.sourceIdentity; break;
        case 6: ++wrong.payload.play.sourceStartFrame; break;
        case 7: ++wrong.payload.play.batch.reserved; break;
        case 8: ++wrong.payload.play.batch.batchVersion; break;
        }
        auto& queue = h.audio.engine->commands;
        const auto head = queue.PublishedHead();
        ASSERT_TRUE(h.Freeze(wrong));
        EXPECT_FALSE(h.Attach());
        EXPECT_TRUE(h.original.Valid());
        EXPECT_EQ(harken::CommandQueue::OriginalRangeState::Empty, h.Range().State());
        h.queueCommit.Release();
        ASSERT_TRUE(h.Freeze(h.header));
        harken::CommandQueue foreign(2);
        EXPECT_FALSE(foreign.IssueOriginalControlHeaderRange(h.queueCommit, 0,
            std::move(h.original), &h, &h.Range()));
        ASSERT_TRUE(h.Attach());
        EXPECT_FALSE(h.original.Valid());
        EXPECT_FALSE(h.Range().ResetAfterDisposition());
        h.queueCommit.Release();
        EXPECT_EQ(head, queue.PublishedHead());
        EXPECT_EQ(harken::CommandQueue::OriginalRangeState::UnpublishedCancelled, h.Range().State());
        h.coreCommit.Release();
        EXPECT_FALSE(h.audio.engine->timelineArms.CancelPrepared(h.control.record.token));
        ASSERT_TRUE(h.Range().ResetAfterDisposition());
        ASSERT_TRUE(h.audio.engine->timelineArms.CancelPrepared(h.control.record.token));
        h.control.record.token = {};
        h.Dispose();
    }
}

TEST(AuthoredStopAudio, OriginalControlHeaderConsumedCommitUnpublishedAbortRetainsTrueReclaimFence) {
    OriginalControlHeaderHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.Prepare());
    ASSERT_TRUE(h.Freeze(h.header));
    ASSERT_TRUE(h.Attach());
    auto& engine = *h.audio.engine;
    const auto completed = engine.mixer.BlocksRendered();
    engine.timelineArms.ConsumePreparedCommit(std::move(h.coreCommit));
    h.queueCommit.Release();
    ASSERT_EQ(harken::CommandQueue::OriginalRangeState::UnpublishedCancelled, h.Range().State());
    ASSERT_TRUE(engine.timelineArms.RequestCancel(h.control.record.token));
    ASSERT_TRUE(engine.timelineArms.Tombstone(h.control.record.token, completed));
    h.Pump();
    EXPECT_FALSE(engine.timelineArms.Reclaim(h.control.record.token, engine.mixer.BlocksRendered()));
    ASSERT_TRUE(h.Range().ResetAfterDisposition());
    EXPECT_FALSE(engine.timelineArms.Reclaim(h.control.record.token, completed));
    ASSERT_TRUE(engine.timelineArms.Reclaim(h.control.record.token, engine.mixer.BlocksRendered()));
    h.control.record.token = {};
    h.ExpectSourceUnchanged();
    h.Dispose();
}

TEST(AuthoredStopAudio, OriginalControlHeaderReceiverRefusesGenuineOtherTableWithIdenticalFullTokenBeforeMovingOwner) {
    OriginalControlHeaderHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.Prepare());
    ASSERT_TRUE(h.Freeze(h.header));
    harken::TimelineArmTable foreign(2);
    PendingControlRecord other;
    other.record.input.renderSlot = static_cast<std::uint32_t>(h.render);
    other.record.input.renderGeneration = static_cast<std::uint32_t>(h.render >> 32);
    ASSERT_TRUE(other.PrepareSource(foreign));
    ASSERT_EQ(h.control.record.token.slot, other.record.token.slot);
    ASSERT_EQ(h.control.record.token.generation, other.record.token.generation);
    ASSERT_EQ(h.control.record.token.renderSlot, other.record.token.renderSlot);
    ASSERT_EQ(h.control.record.token.renderGeneration, other.record.token.renderGeneration);
    harken::TimelineArmTable::PreparedCommitProof held;
    ASSERT_TRUE(foreign.ProvePendingAuthoredControlCommit(other.record.token, other.pending, &held));
    harken::TimelineArmTable::OriginalControlHeaderRowOwner owner;
    ASSERT_TRUE(foreign.IssueOriginalControlHeaderRowOwner(held, &owner));
    auto& queue = h.audio.engine->commands;
    const auto head = queue.PublishedHead();
    EXPECT_FALSE(queue.IssueOriginalControlHeaderRange(h.queueCommit, 0,
        std::move(owner), &h, &h.Range()));
    EXPECT_TRUE(owner.Valid());
    EXPECT_EQ(harken::CommandQueue::OriginalRangeState::Empty, h.Range().State());
    EXPECT_EQ(head, queue.PublishedHead());
    EXPECT_TRUE(h.queueCommit.Valid());
    harken::CommandQueue unbound(2);
    harken::CommandQueue::BatchReservation reservation;
    harken::CommandQueue::PreparedBatchProof proof;
    ASSERT_TRUE(unbound.ReserveBatch(1, &reservation));
    ASSERT_TRUE(unbound.ProvePreparedBatch(&reservation, &h.header, 1, &proof));
    harken::CommandQueue::OriginalBatchRange rejected;
    EXPECT_FALSE(unbound.IssueOriginalControlHeaderRange(proof, 0, std::move(owner), &h, &rejected));
    EXPECT_TRUE(owner.Valid());
    proof.Release();
    held.Release();
    owner.Release();
    ASSERT_TRUE(foreign.CancelPrepared(other.record.token));
    ASSERT_TRUE(foreign.CancelPrepared(other.source));
    ASSERT_TRUE(h.Attach());
    ASSERT_TRUE(h.CommitAndPublish());
    h.Pump();
    EXPECT_EQ(harken::CommandQueue::OriginalControlDisposition::Settled, h.Range().ControlDisposition());
    ASSERT_TRUE(h.Range().ResetAfterDisposition());
    h.Dispose();
}

TEST(AuthoredStopAudio, OriginalControlHeaderReissuedRangeAndRowConfirmOnlyNewGenerationAndKeepTrueSourcePcm) {
    OriginalControlHeaderHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.Prepare());
    ASSERT_TRUE(h.Freeze(h.header));
    ASSERT_TRUE(h.Attach());
    ASSERT_TRUE(h.CommitAndPublish());
    auto& engine = *h.audio.engine;
    const auto old = h.control.record.token;
    ASSERT_TRUE(engine.timelineArms.RequestCancel(old));
    h.Pump();
    ASSERT_EQ(harken::CommandQueue::OriginalControlDisposition::Cancelled, h.Range().ControlDisposition());
    h.Pump();
    ASSERT_TRUE(h.Range().ResetAfterDisposition());
    ASSERT_TRUE(engine.timelineArms.Reclaim(old, engine.mixer.BlocksRendered()));
    h.control.record.token = {};
    PendingControlRecord fresh(4096, 4000);
    fresh.record.input.renderSlot = static_cast<std::uint32_t>(h.render);
    fresh.record.input.renderGeneration = static_cast<std::uint32_t>(h.render >> 32);
    fresh.source = h.control.source;
    ASSERT_TRUE(fresh.PrepareControl(engine.timelineArms));
    ASSERT_EQ(old.slot, fresh.record.token.slot);
    ASSERT_EQ(old.generation + 1u, fresh.record.token.generation);
    harken::TimelineArmTable::PreparedCommitProof core;
    ASSERT_TRUE(engine.timelineArms.ProvePendingAuthoredControlCommit(fresh.record.token, fresh.pending, &core));
    harken::TimelineArmTable::OriginalControlHeaderRowOwner owner;
    ASSERT_TRUE(engine.timelineArms.IssueOriginalControlHeaderRowOwner(core, &owner));
    ASSERT_TRUE(h.Freeze(h.header));
    EXPECT_FALSE(engine.commands.IssueOriginalControlHeaderRange(h.queueCommit, 0,
        std::move(owner), &h, &h.Range()));
    EXPECT_TRUE(owner.Valid());
    EXPECT_EQ(harken::CommandQueue::OriginalRangeState::Empty, h.Range().State());
    h.queueCommit.Release();
    auto header = h.header;
    header.payload.play.batch.armToken =
        (static_cast<std::uint64_t>(fresh.record.token.generation) << 32) | fresh.record.token.slot;
    ASSERT_TRUE(h.Freeze(header));
    ASSERT_TRUE(engine.commands.IssueOriginalControlHeaderRange(h.queueCommit, 0,
        std::move(owner), &h, &h.Range()));
    engine.timelineArms.ConsumePreparedCommit(std::move(core));
    (void)engine.commands.ConsumePreparedBatch(std::move(h.queueCommit));
    const auto completion = engine.mixer.BlocksRendered() + 1u;
    const auto hits = harken::memguard::HitCount();
    h.Pump();
    EXPECT_EQ(hits, harken::memguard::HitCount());
    ASSERT_EQ(harken::CommandQueue::OriginalControlDisposition::Settled, h.Range().ControlDisposition());
    EXPECT_EQ(fresh.record.token.generation, h.Range().ControlOwner()->Token().generation);
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(fresh.record.token, &observed));
    EXPECT_EQ(completion, observed.armedCompletionSequence);
    EXPECT_EQ(0u, observed.startedBlockSequence);
    h.ExpectSourceUnchanged();
    auto* original = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, original);
    ASSERT_NE(nullptr, original->sourceTerminal);
    const auto terminalToken = original->sourceTerminalToken;
    const auto leaf = original->sourceTerminal->leaf;
    const auto first = (engine.mixer.BlocksRendered() + 2u) * kBlock;
    harken::TimelineEventPayload payload{};
    payload.kind = harken::TimelineEventKind::Start;
    payload.target = h.control.record.target;
    harken::TimelineProgramView program{7, &payload, 1};
    harken::ImmediateLaneEvent event{};
    event.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
    event.fixedFrame = event.event.executeFrame = first;
    event.event.program = {7, &program};
    event.event.armToken = h.control.source;
    event.event.subeventPhase = 20;
    event.event.ownerInstanceHandle = leaf;
    event.event.ownerInstanceScheduleEpoch = original->sourceTerminal->acceptedEpoch;
    harken::ImmediateLaneHandle lane{};
    ASSERT_TRUE(engine.immediateActionLane.Reserve(leaf, 1, &lane));
    ASSERT_TRUE(engine.immediateActionLane.Prepare(lane, &event, 1));
    std::uint64_t anchor = UINT64_MAX;
    ASSERT_TRUE(engine.immediateActionLane.TryPlace(lane, first, 0, &anchor));
    ASSERT_TRUE(engine.timelineArms.BindPendingImmediateStart(h.control.source, first, first));
    ASSERT_TRUE(engine.immediateActionLane.PinArms(lane, engine.timelineArms));
    ASSERT_TRUE(engine.timelineArms.Publish(h.control.source));
    ASSERT_TRUE(engine.immediateActionLane.Publish(lane));
    while (engine.mixer.BlocksRendered() <= first / kBlock) h.Pump();
    const auto* pcm = engine.nullBackend.LastBlock();
    ASSERT_NE(nullptr, pcm);
    for (std::uint32_t frame = kBlock - 16; frame < kBlock; ++frame)
        EXPECT_NEAR(0.25f, pcm[2u * frame], 1e-6f);
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.source, &observed));
    EXPECT_EQ(first / kBlock + 1u, observed.startedBlockSequence);
    EXPECT_EQ(0u, fresh.record.operation.AppliedAudioSequence());
    EXPECT_FALSE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
    h.Pump();
    ASSERT_TRUE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(fresh.record.token));
    ASSERT_TRUE(engine.timelineArms.Tombstone(fresh.record.token, engine.mixer.BlocksRendered()));
    h.Pump();
    EXPECT_FALSE(engine.timelineArms.Reclaim(fresh.record.token, engine.mixer.BlocksRendered()));
    ASSERT_TRUE(h.Range().ResetAfterDisposition());
    ASSERT_TRUE(engine.timelineArms.Reclaim(fresh.record.token, engine.mixer.BlocksRendered()));
    h.Dispose();
    harken::SourceTerminalPayload terminal{};
    ASSERT_TRUE(engine.sourceTerminals.Capture(terminalToken, engine.mixer.BlocksRendered(), &terminal));
    EXPECT_EQ(harken::SourceTerminalKind::Stop, terminal.kind);
    ASSERT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(leaf));
    ASSERT_TRUE(engine.sourceTerminals.Release(terminalToken));
    ASSERT_TRUE(engine.playableInstances.SetTerminalOutcome(leaf,
        {harken::PlayableTerminalKind::RequestedStop, VN_OK, 0, 0}));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(leaf, harken::PlayableLifecycle::Retired));
    EXPECT_EQ(engine.timelineArms.Capacity(), engine.timelineArms.FreeCount());
    EXPECT_EQ(0u, engine.mixer.CommandProtocolViolationCount());
}

TEST(AuthoredStopAudio, OriginalControlHeaderPublishedCancelAndMaximumInitialBlockKeepActualOriginalUntilCompletion) {
    OriginalControlHeaderHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.Prepare());
    ASSERT_TRUE(h.Freeze(h.header));
    ASSERT_TRUE(h.Attach());
    ASSERT_TRUE(h.CommitAndPublish());
    auto& engine = *h.audio.engine;
    ASSERT_TRUE(engine.timelineArms.RequestCancel(h.control.record.token));
    const auto tail = engine.commands.ConsumedCount();
    EXPECT_EQ(1u, engine.mixer.DrainCommandsForTest(UINT64_MAX));
    EXPECT_EQ(tail, engine.commands.ConsumedCount());
    EXPECT_EQ(harken::CommandQueue::OriginalRangeState::Published, h.Range().State());
    EXPECT_FALSE(h.Range().ResetAfterDisposition());
    const auto hits = harken::memguard::HitCount();
    h.Pump();
    EXPECT_EQ(hits, harken::memguard::HitCount());
    EXPECT_EQ(harken::CommandQueue::OriginalRangeState::Completed, h.Range().State());
    EXPECT_EQ(harken::CommandQueue::OriginalControlDisposition::Cancelled, h.Range().ControlDisposition());
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &observed));
    EXPECT_EQ(harken::ArmState::Tombstoned, observed.state);
    EXPECT_EQ(0u, observed.armedCompletionSequence);
    EXPECT_EQ(0u, observed.startedBlockSequence);
    const auto fence = engine.mixer.BlocksRendered();
    h.Pump();
    EXPECT_FALSE(engine.timelineArms.Reclaim(h.control.record.token, engine.mixer.BlocksRendered()));
    ASSERT_TRUE(h.Range().ResetAfterDisposition());
    EXPECT_FALSE(engine.timelineArms.Reclaim(h.control.record.token, fence));
    ASSERT_TRUE(engine.timelineArms.Reclaim(h.control.record.token, engine.mixer.BlocksRendered()));
    h.control.record.token = {};
    h.ExpectSourceUnchanged();
    EXPECT_EQ(0u, engine.mixer.CommandProtocolViolationCount());
    h.Dispose();
}

TEST(AuthoredStopAudio, OriginalControlHeaderBusyAckRetainsLoanAndRejectsMaximumRetryWithoutAdvancingSettled) {
    for (const bool cancel : {false, true}) {
        SCOPED_TRACE(cancel);
        OriginalControlHeaderHarness h;
        ASSERT_TRUE(h.InstallSource());
        ASSERT_TRUE(h.Prepare());
        ASSERT_TRUE(h.Freeze(h.header));
        ASSERT_TRUE(h.Attach());
        ASSERT_TRUE(h.CommitAndPublish());
        auto& engine = *h.audio.engine;
        const auto settled = engine.commands.SettledFrontier();
        const auto originalCompletion = engine.mixer.BlocksRendered() + 1u;
        HeldArmIdentity hold(engine.timelineArms, h.control.record.token);
        ASSERT_TRUE(hold.Wait());
        h.Pump();
        EXPECT_EQ(1u, engine.mixer.CommandsDeferredLastBlock());
        EXPECT_EQ(settled, engine.commands.SettledFrontier());
        EXPECT_EQ(harken::CommandQueue::OriginalRangeState::Published, h.Range().State());
        EXPECT_FALSE(h.Range().ResetAfterDisposition());
        const auto tail = engine.commands.ConsumedCount();
        EXPECT_EQ(1u, engine.mixer.DrainCommandsForTest(UINT64_MAX));
        EXPECT_EQ(tail, engine.commands.ConsumedCount());
        EXPECT_EQ(settled, engine.commands.SettledFrontier());
        hold.Finish();
        if (cancel) ASSERT_TRUE(engine.timelineArms.RequestCancel(h.control.record.token));
        h.Pump();
        EXPECT_EQ(harken::CommandQueue::OriginalRangeState::Completed, h.Range().State());
        EXPECT_EQ(cancel ? harken::CommandQueue::OriginalControlDisposition::Cancelled :
            harken::CommandQueue::OriginalControlDisposition::Settled, h.Range().ControlDisposition());
        harken::TimelineArmObservation observed{};
        ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &observed));
        EXPECT_EQ(cancel ? 0u : originalCompletion, observed.armedCompletionSequence);
        EXPECT_EQ(0u, observed.startedBlockSequence);
        EXPECT_EQ(0u, engine.mixer.CommandProtocolViolationCount());
        ASSERT_TRUE(h.Range().ResetAfterDisposition());
        h.ExpectSourceUnchanged();
        h.Dispose();
    }
}

TEST(AuthoredStopAudio, OriginalControlHeaderActualCorruptRecordCompletesInvalidWithoutConfirmingSourceOrControl) {
    OriginalControlHeaderHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.Prepare());
    ASSERT_TRUE(h.Freeze(h.header));
    ASSERT_TRUE(h.Attach());
    ASSERT_TRUE(h.CommitAndPublish());
    auto& engine = *h.audio.engine;
    harken::CommandRecord corrupt{};
    ASSERT_TRUE(engine.commands.Peek(corrupt));
    ++corrupt.payload.play.handle;
    harken::detail::CommandQueueSubmissionTestAccess::OverwriteFront(engine.commands, corrupt);
    engine.mixer.SuppressCommandProtocolAssertForTest(true);
    h.Pump();
    EXPECT_EQ(1u, engine.mixer.CommandProtocolViolationCount());
    EXPECT_EQ(harken::CommandQueue::OriginalRangeState::Completed, h.Range().State());
    EXPECT_EQ(harken::CommandQueue::OriginalControlDisposition::Invalid, h.Range().ControlDisposition());
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &observed));
    EXPECT_EQ(harken::ArmState::PendingApply, observed.state);
    EXPECT_EQ(0u, observed.armedCompletionSequence);
    h.ExpectSourceUnchanged();
    ASSERT_TRUE(h.Range().ResetAfterDisposition());
    h.Dispose();
}

TEST(AuthoredStopAudio, OriginalControlHeaderEngineShutdownReturnsQueuedAndPendingRealLifetimeOwnersAfterJoin) {
    for (const bool pendingAck : {false, true}) {
        SCOPED_TRACE(pendingAck);
        OriginalControlHeaderHarness h;
        ASSERT_TRUE(h.InstallSource());
        ASSERT_TRUE(h.Prepare());
        ASSERT_TRUE(h.Freeze(h.header));
        ASSERT_TRUE(h.Attach());
        ASSERT_TRUE(h.CommitAndPublish());
        if (pendingAck) {
            HeldArmIdentity hold(h.audio.engine->timelineArms, h.control.record.token);
            ASSERT_TRUE(hold.Wait());
            h.Pump();
            EXPECT_EQ(harken::CommandQueue::OriginalRangeState::Published, h.Range().State());
            hold.Finish();
        }
        ASSERT_EQ(VN_OK, Vn_Term());
    }
}

TEST(AuthoredStopAudio, PendingControlRejectsUnoriginalSourceAndIncompleteWorstCaseStorageBeforeReservation) {
    for (unsigned mode = 0; mode < 7; ++mode) {
        SCOPED_TRACE(mode);
        harken::TimelineArmTable table(3);
        PendingControlRecord h;
        ASSERT_TRUE(table.PreparePendingImmediate(9, 11, 7, h.record.target, &h.source));
        h.pending.sourceStart = h.source;
        switch (mode) {
        case 0: h.pending.eventCapacity = 1; break;
        case 1: ++h.pending.sourceStart.renderGeneration; break;
        case 2: ++h.pending.target.programSemanticFingerprint; break;
        case 3:
            ASSERT_TRUE(table.CommitPrepared(h.source));
            ASSERT_TRUE(table.AcknowledgeApplied(h.source, 1));
            ASSERT_TRUE(table.BindPendingImmediateStart(h.source, 80, 80));
            break;
        case 4: ASSERT_TRUE(table.CancelPrepared(h.source)); break;
        case 5: EXPECT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, h.record.operation.Cancel()); break;
        case 6: h.record.program.payloads = nullptr; break;
        }
        const auto free = table.FreeCount();
        EXPECT_FALSE(table.PreparePendingAuthoredControl(h.pending, &h.record.token));
        EXPECT_FALSE(h.record.token.Valid());
        EXPECT_EQ(free, table.FreeCount());
        if (mode != 4) ASSERT_NO_FATAL_FAILURE(h.DisposeSource(table));
        EXPECT_EQ(3u, table.FreeCount());
    }
}

namespace harken::detail {
// Actual state observations and genuine already-issued actor borrows only.
// The two setters remove prerequisites; they cannot issue any capability.
struct OriginalControlOriginTestAccess {
    using Queue = CommandQueue;
    using Range = Queue::OriginalBatchRange;
    using Pin = Queue::OriginalControlOriginPin;
    using Actor = Queue::OriginalControlMutationActor;
    struct Observation {
        std::uint8_t mode = 0;
        std::uint64_t activity = 0;
        std::uint64_t epoch = 0;
        std::uint32_t pins = 0;
        bool native = false;
        std::uint64_t cohort = 0;
        std::uint64_t head = 0;
        const void* client = nullptr;
    };
    static Observation Observe(const Range& range) {
        const auto word = range.controlOriginWord_.load(std::memory_order_acquire);
        const auto mode = Queue::ControlMode(word);
        const std::uint8_t semanticMode = mode == Queue::ControlOriginMode::Normal ? 1u :
            mode == Queue::ControlOriginMode::ExhaustedNormal ? 2u :
            mode == Queue::ControlOriginMode::Closed ? 3u :
            mode == Queue::ControlOriginMode::Redirect ? 4u :
            mode == Queue::ControlOriginMode::ExhaustedRedirect ? 5u : 0u;
        return {semanticMode,
            Queue::ControlActivity(word), Queue::ControlEpoch(word),
            range.controlOriginPins_.load(std::memory_order_acquire),
            range.controlNativeActorHeld_.load(std::memory_order_acquire),
            range.origin_, range.head_, range.client_};
    }
    static bool Matches(Queue& queue, const Pin& pin) {
        return queue.MatchesControlOrigin(pin);
    }
    static bool BorrowPending(MixerLoop& mixer, const Range& range, Actor* out) {
        return mixer.m_pendingControlAck.originalRange == &range &&
            mixer.m_pendingControlOriginActor.Borrow(out);
    }
    static bool HasPendingActor(const MixerLoop& mixer) {
        return mixer.m_pendingControlOriginActor.Valid();
    }
    static void RefuseNewOptionalScope(TimelineArmTable::OriginalControlHeaderRowOwner& owner) {
        assert(owner.Valid());
        // Preserve the original genuine typed-row holder. Only new metadata is absent.
        owner.scope_ = {};
    }
    static void ExhaustActualEpoch(Range& range) {
        const auto word = range.controlOriginWord_.load(std::memory_order_acquire);
        assert(range.controlNativeActorHeld_.load(std::memory_order_acquire) &&
            Queue::ControlMode(word) == Queue::ControlOriginMode::Normal &&
            Queue::ControlActivity(word) != 0);
        range.controlOriginWord_.store(Queue::ControlWord(Queue::ControlMode(word),
            Queue::ControlActivity(word), Queue::kControlEpochMax), std::memory_order_release);
    }
};
} // namespace harken::detail

namespace {
using ControlOriginAccess = harken::detail::OriginalControlOriginTestAccess;
using ControlOriginPin = harken::CommandQueue::OriginalControlOriginPin;
using ControlOriginActor = harken::CommandQueue::OriginalControlMutationActor;
constexpr std::uint8_t kControlNormal = 1;
constexpr std::uint8_t kControlExhausted = 2;
constexpr std::uint8_t kControlClosed = 3;

struct OriginalControlOriginHarness : OriginalControlHeaderHarness {
    bool AttachOrigin() {
        auto& resource = audio.engine->authoredStopResources[0];
        return audio.engine->commands.IssueOriginalControlHeaderRange(queueCommit, 0,
            std::move(original), &resource, &resource.originalControlHeader, &origin);
    }
    ControlOriginPin origin;
};

void PreparePublishedControlOrigin(OriginalControlOriginHarness& h) {
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.Prepare());
    ASSERT_TRUE(h.Freeze(h.header));
    ASSERT_TRUE(h.AttachOrigin());
    ASSERT_TRUE(h.CommitAndPublish());
}

void DisposeControlPhysicalSourceAndOriginalTerminal(OriginalControlHeaderHarness& h) {
    auto& engine = *h.audio.engine;
    const auto* source = engine.voices.Resolve(h.render);
    ASSERT_NE(nullptr, source);
    ASSERT_NE(nullptr, source->sourceTerminal);
    const auto terminalToken = source->sourceTerminalToken;
    const auto leaf = source->sourceTerminal->leaf;
    const auto before = engine.mixer.BlocksRendered();
    ASSERT_NO_FATAL_FAILURE(h.Dispose());
    ASSERT_GT(engine.mixer.BlocksRendered(), before);
    ASSERT_TRUE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    harken::SourceTerminalPayload terminal{};
    ASSERT_TRUE(engine.sourceTerminals.Capture(terminalToken,
        engine.mixer.BlocksRendered(), &terminal));
    EXPECT_EQ(harken::SourceTerminalKind::Stop, terminal.kind);
    ASSERT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(leaf));
    ASSERT_TRUE(engine.sourceTerminals.Release(terminalToken));
    ASSERT_TRUE(engine.playableInstances.SetTerminalOutcome(leaf,
        {harken::PlayableTerminalKind::RequestedStop, VN_OK, 0, 0}));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(leaf, harken::PlayableLifecycle::Retired));
}

void ReturnControlOriginAndDisposePhysicalSource(OriginalControlOriginHarness& h) {
    auto& engine = *h.audio.engine;
    EXPECT_FALSE(ControlOriginAccess::HasPendingActor(engine.mixer));
    h.origin.Release();
    const auto state = ControlOriginAccess::Observe(h.Range());
    EXPECT_EQ(0u, state.activity);
    EXPECT_EQ(0u, state.pins);
    EXPECT_FALSE(state.native);
    EXPECT_EQ(kControlClosed, state.mode);
    ASSERT_TRUE(h.Range().ResetAfterDisposition());
    EXPECT_FALSE(engine.timelineArms.RowAt(h.control.record.token.slot)->originalLifetimePinned.load());
    // Only the actual subsequent native Stop releases the physical Source.
    EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    ASSERT_NO_FATAL_FAILURE(DisposeControlPhysicalSourceAndOriginalTerminal(h));
    EXPECT_TRUE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    EXPECT_EQ(engine.timelineArms.Capacity(), engine.timelineArms.FreeCount());
}
} // namespace

TEST(AuthoredStopAudio, ControlOriginTruePreparedAndFrozenFactoriesRetainFullScopeAndAbortWithoutHeadExposure) {
    static_assert(!std::is_copy_constructible_v<ControlOriginPin>);
    static_assert(std::is_nothrow_move_constructible_v<ControlOriginPin>);
    static_assert(!std::is_copy_constructible_v<ControlOriginActor>);
    OriginalControlOriginHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.Prepare());
    auto& engine = *h.audio.engine;
    const auto& scope = h.original.OriginalScope();
    EXPECT_EQ(engine.timelineArms.Incarnation(), scope.incarnation);
    EXPECT_EQ(h.control.source.slot, scope.sourceStart.slot);
    EXPECT_EQ(h.control.source.generation, scope.sourceStart.generation);
    EXPECT_EQ(h.control.source.renderSlot, scope.sourceStart.renderSlot);
    EXPECT_EQ(h.control.source.renderGeneration, scope.sourceStart.renderGeneration);
    EXPECT_EQ(&h.control.record.operation, scope.operation);
    EXPECT_EQ(h.control.record.operation.Identity(), scope.operationIdentity);
    EXPECT_EQ(h.control.record.program.generation, scope.program.programGeneration);
    EXPECT_EQ(&h.control.record.program, scope.program.program);
    EXPECT_EQ(h.control.record.target, scope.target);
    const auto head = engine.commands.PublishedHead();
    const auto consumed = engine.commands.ConsumedCount();
    ASSERT_TRUE(h.Freeze(h.header));
    ASSERT_TRUE(h.AttachOrigin());
    auto observation = ControlOriginAccess::Observe(h.Range());
    EXPECT_EQ(kControlNormal, observation.mode);
    EXPECT_EQ(1u, observation.epoch);
    EXPECT_EQ(0u, observation.activity);
    EXPECT_EQ(1u, observation.pins);
    EXPECT_NE(0u, observation.cohort);
    EXPECT_EQ(head, observation.head);
    EXPECT_EQ(&engine.authoredStopResources[0], observation.client);
    ControlOriginPin first, second;
    ASSERT_TRUE(h.origin.Borrow(&first));
    ASSERT_TRUE(h.origin.Borrow(&second));
    EXPECT_FALSE(first.Borrow(&second));
    EXPECT_FALSE(first.Borrow(&first));
    EXPECT_FALSE(first.Borrow(nullptr));
    harken::CommandQueue foreign(8, &engine.timelineArms);
    EXPECT_FALSE(ControlOriginAccess::Matches(foreign, first));
    EXPECT_TRUE(ControlOriginAccess::Matches(engine.commands, first));
    ControlOriginPin moved(std::move(first));
    EXPECT_FALSE(first.Valid());
    moved = std::move(second);
    EXPECT_FALSE(second.Valid());
    EXPECT_EQ(2u, ControlOriginAccess::Observe(h.Range()).pins);
    ControlOriginActor noNative;
    EXPECT_FALSE(ControlOriginAccess::BorrowPending(engine.mixer, h.Range(), &noNative));
    EXPECT_FALSE(noNative.Valid());
    EXPECT_FALSE(noNative.BeforeFirstWrite());
    h.queueCommit.Release();
    h.coreCommit.Release();
    EXPECT_EQ(head, engine.commands.PublishedHead());
    EXPECT_EQ(consumed, engine.commands.ConsumedCount());
    EXPECT_EQ(harken::CommandQueue::OriginalRangeState::UnpublishedCancelled, h.Range().State());
    EXPECT_EQ(kControlClosed, ControlOriginAccess::Observe(h.Range()).mode);
    EXPECT_FALSE(h.Range().ResetAfterDisposition());
    EXPECT_FALSE(engine.timelineArms.CancelPrepared(h.control.record.token));
    moved.Release();
    h.origin.Release();
    ASSERT_TRUE(h.Range().ResetAfterDisposition());
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(h.control.record.token));
    h.control.record.token = {};
    ASSERT_NO_FATAL_FAILURE(DisposeControlPhysicalSourceAndOriginalTerminal(h));
    EXPECT_EQ(engine.timelineArms.Capacity(), engine.timelineArms.FreeCount());
}

TEST(AuthoredStopAudio, ControlOriginDefaultLegacyGenuineTypedHeaderDoesNotRequireUnusedNewScopeOrMintActor) {
    OriginalControlHeaderHarness h;
    ASSERT_TRUE(h.InstallSource());
    ASSERT_TRUE(h.Prepare());
    auto& engine = *h.audio.engine;
    // A negative prerequisite on new optional metadata, never on the genuine
    // original typed row/token/operation. It does not mint a legacy capability.
    ControlOriginAccess::RefuseNewOptionalScope(h.original);
    ASSERT_TRUE(h.original.Valid());
    ASSERT_TRUE(h.Freeze(h.header));
    ControlOriginPin rejected;
    auto& resource = engine.authoredStopResources[0];
    EXPECT_FALSE(engine.commands.IssueOriginalControlHeaderRange(h.queueCommit, 0,
        std::move(h.original), &resource, &resource.originalControlHeader, &rejected));
    EXPECT_FALSE(rejected.Valid());
    EXPECT_TRUE(h.original.Valid());
    EXPECT_EQ(harken::CommandQueue::OriginalRangeState::Empty, h.Range().State());
    ASSERT_TRUE(h.Attach());
    ASSERT_TRUE(h.CommitAndPublish());
    const auto before = engine.mixer.BlocksRendered();
    h.Pump();
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    ASSERT_EQ(harken::CommandQueue::OriginalRangeState::Completed, h.Range().State());
    EXPECT_EQ(harken::CommandQueue::OriginalControlDisposition::Settled, h.Range().ControlDisposition());
    harken::TimelineArmObservation control{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &control));
    EXPECT_EQ(before + 1u, control.armedCompletionSequence);
    EXPECT_EQ(0u, control.startedBlockSequence);
    const auto state = ControlOriginAccess::Observe(h.Range());
    EXPECT_EQ(0u, state.mode);
    EXPECT_EQ(0u, state.epoch);
    EXPECT_EQ(0u, state.pins);
    EXPECT_FALSE(ControlOriginAccess::HasPendingActor(engine.mixer));
    EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    ASSERT_TRUE(h.Range().ResetAfterDisposition());
    ASSERT_NO_FATAL_FAILURE(DisposeControlPhysicalSourceAndOriginalTerminal(h));
    EXPECT_EQ(engine.timelineArms.Capacity(), engine.timelineArms.FreeCount());
}

TEST(AuthoredStopAudio, ControlOriginActualBusyAckKeepsNativeActorAcrossTwoCallbacksAndForeignDoesNotBlockAck) {
    OriginalControlOriginHarness h;
    ASSERT_NO_FATAL_FAILURE(PreparePublishedControlOrigin(h));
    auto& engine = *h.audio.engine;
    const auto before = engine.mixer.BlocksRendered();
    const auto settled = engine.commands.SettledFrontier();
    HeldArmIdentity hold(engine.timelineArms, h.control.record.token);
    ASSERT_TRUE(hold.Wait());
    h.Pump();
    auto first = ControlOriginAccess::Observe(h.Range());
    EXPECT_EQ(1u, first.activity);
    EXPECT_GE(first.epoch, 2u);
    EXPECT_TRUE(first.native);
    EXPECT_EQ(settled, engine.commands.SettledFrontier());
    ControlOriginActor foreign;
    ASSERT_TRUE(ControlOriginAccess::BorrowPending(engine.mixer, h.Range(), &foreign));
    EXPECT_FALSE(ControlOriginAccess::BorrowPending(engine.mixer, h.Range(), &foreign));
    ControlOriginActor secondLoan;
    ASSERT_TRUE(foreign.Borrow(&secondLoan));
    ControlOriginActor moved(std::move(foreign));
    EXPECT_FALSE(foreign.Valid());
    moved = std::move(secondLoan);
    EXPECT_FALSE(secondLoan.Valid());
    foreign = std::move(moved);
    EXPECT_FALSE(moved.Valid());
    EXPECT_EQ(2u, ControlOriginAccess::Observe(h.Range()).activity);
    h.Pump();
    const auto second = ControlOriginAccess::Observe(h.Range());
    EXPECT_EQ(first.epoch, second.epoch);
    EXPECT_EQ(2u, second.activity);
    EXPECT_TRUE(second.native);
    EXPECT_EQ(settled, engine.commands.SettledFrontier());
    EXPECT_FALSE(h.Range().ResetAfterDisposition());
    hold.Finish();
    h.Pump();
    ASSERT_EQ(before + 3u, engine.mixer.BlocksRendered());
    harken::TimelineArmObservation control{};
    ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &control));
    EXPECT_EQ(before + 1u, control.armedCompletionSequence);
    EXPECT_EQ(0u, control.startedBlockSequence);
    ASSERT_EQ(harken::CommandQueue::OriginalRangeState::Completed, h.Range().State());
    EXPECT_EQ(1u, ControlOriginAccess::Observe(h.Range()).activity);
    EXPECT_FALSE(ControlOriginAccess::HasPendingActor(engine.mixer));
    EXPECT_TRUE(foreign.Valid());
    EXPECT_FALSE(foreign.BeforeFirstWrite());
    EXPECT_FALSE(foreign.Borrow(nullptr));
    EXPECT_FALSE(h.Range().ResetAfterDisposition());
    h.ExpectSourceUnchanged();
    foreign.Release();
    ASSERT_NO_FATAL_FAILURE(ReturnControlOriginAndDisposePhysicalSource(h));
}

TEST(AuthoredStopAudio, ControlOriginActualCommandCutoffAndMaximumDoNotBorrowOrPublishCompletion) {
    OriginalControlOriginHarness h;
    ASSERT_NO_FATAL_FAILURE(PreparePublishedControlOrigin(h));
    auto& engine = *h.audio.engine;
    const auto consumed = engine.commands.ConsumedCount();
    const auto before = engine.mixer.BlocksRendered();
    const auto limit = engine.mixer.MaxCommandsPerQuantum();
    engine.mixer.SetMaxCommandsPerQuantumForTest(0);
    h.Pump();
    EXPECT_EQ(consumed, engine.commands.ConsumedCount());
    EXPECT_EQ(1u, ControlOriginAccess::Observe(h.Range()).epoch);
    EXPECT_EQ(0u, ControlOriginAccess::Observe(h.Range()).activity);
    EXPECT_FALSE(ControlOriginAccess::HasPendingActor(engine.mixer));
    EXPECT_EQ(0u, engine.timelineArms.RowAt(h.control.record.token.slot)->armedCompletionSequence.load());
    engine.mixer.SetMaxCommandsPerQuantumForTest(limit);
    EXPECT_EQ(1u, engine.mixer.DrainCommandsForTest(UINT64_MAX));
    EXPECT_EQ(before + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(consumed, engine.commands.ConsumedCount());
    EXPECT_EQ(1u, ControlOriginAccess::Observe(h.Range()).epoch);
    EXPECT_EQ(0u, ControlOriginAccess::Observe(h.Range()).activity);
    h.Pump();
    EXPECT_EQ(before + 2u, engine.mixer.BlocksRendered());
    EXPECT_EQ(before + 2u, engine.timelineArms.RowAt(h.control.record.token.slot)->armedCompletionSequence.load());
    EXPECT_EQ(kControlClosed, ControlOriginAccess::Observe(h.Range()).mode);
    EXPECT_GE(ControlOriginAccess::Observe(h.Range()).epoch, 2u);
    ASSERT_NO_FATAL_FAILURE(ReturnControlOriginAndDisposePhysicalSource(h));
}

TEST(AuthoredStopAudio, ControlOriginRealMaximumEpochExhaustsWithoutWrapAndStillCompletesActualOldAck) {
    OriginalControlOriginHarness h;
    ASSERT_NO_FATAL_FAILURE(PreparePublishedControlOrigin(h));
    auto& engine = *h.audio.engine;
    HeldArmIdentity hold(engine.timelineArms, h.control.record.token);
    ASSERT_TRUE(hold.Wait());
    h.Pump();
    ControlOriginActor child;
    ASSERT_TRUE(ControlOriginAccess::BorrowPending(engine.mixer, h.Range(), &child));
    ControlOriginAccess::ExhaustActualEpoch(h.Range());
    ASSERT_TRUE(child.BeforeFirstWrite());
    auto state = ControlOriginAccess::Observe(h.Range());
    EXPECT_EQ(kControlExhausted, state.mode);
    EXPECT_EQ(UINT64_MAX >> 11, state.epoch);
    hold.Finish();
    h.Pump();
    ASSERT_EQ(harken::CommandQueue::OriginalRangeState::Completed, h.Range().State());
    EXPECT_EQ(UINT64_MAX >> 11, ControlOriginAccess::Observe(h.Range()).epoch);
    EXPECT_TRUE(child.Valid());
    EXPECT_FALSE(child.BeforeFirstWrite());
    EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    child.Release();
    ASSERT_NO_FATAL_FAILURE(ReturnControlOriginAndDisposePhysicalSource(h));
}

TEST(AuthoredStopAudio, ControlOriginRealChildCountExhaustionKeepsActualAckProgressAndBlocksOnlyRangeReuse) {
    OriginalControlOriginHarness h;
    ASSERT_NO_FATAL_FAILURE(PreparePublishedControlOrigin(h));
    auto& engine = *h.audio.engine;
    HeldArmIdentity hold(engine.timelineArms, h.control.record.token);
    ASSERT_TRUE(hold.Wait());
    h.Pump();
    std::array<ControlOriginActor, 253> loans;
    ASSERT_TRUE(ControlOriginAccess::BorrowPending(engine.mixer, h.Range(), &loans[0]));
    for (std::size_t i = 1; i < loans.size(); ++i) ASSERT_TRUE(loans[0].Borrow(&loans[i]));
    ControlOriginActor refused;
    EXPECT_FALSE(loans[0].Borrow(&refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(kControlExhausted, ControlOriginAccess::Observe(h.Range()).mode);
    EXPECT_EQ(254u, ControlOriginAccess::Observe(h.Range()).activity);
    hold.Finish();
    h.Pump();
    ASSERT_EQ(harken::CommandQueue::OriginalRangeState::Completed, h.Range().State());
    EXPECT_EQ(253u, ControlOriginAccess::Observe(h.Range()).activity);
    EXPECT_TRUE(loans[0].Valid());
    EXPECT_FALSE(loans[0].Borrow(&refused));
    EXPECT_FALSE(loans[0].BeforeFirstWrite());
    h.origin.Release();
    EXPECT_FALSE(h.Range().ResetAfterDisposition());
    EXPECT_FALSE(engine.voices.ReleaseObserved(h.audio.lastObserver));
    for (auto& loan : loans) loan.Release();
    ASSERT_NO_FATAL_FAILURE(ReturnControlOriginAndDisposePhysicalSource(h));
}

TEST(AuthoredStopAudio, ControlOriginActualCancelAndCorruptWireReturnActorWithoutStartingOrReleasingSource) {
    for (const bool cancel : {false, true}) {
        SCOPED_TRACE(cancel);
        OriginalControlOriginHarness h;
        ASSERT_NO_FATAL_FAILURE(PreparePublishedControlOrigin(h));
        auto& engine = *h.audio.engine;
        const auto before = engine.mixer.BlocksRendered();
        if (cancel) ASSERT_TRUE(engine.timelineArms.RequestCancel(h.control.record.token));
        else {
            harken::CommandRecord corrupt{};
            ASSERT_TRUE(engine.commands.Peek(corrupt));
            ++corrupt.payload.play.handle;
            harken::detail::CommandQueueSubmissionTestAccess::OverwriteFront(engine.commands, corrupt);
            engine.mixer.SuppressCommandProtocolAssertForTest(true);
        }
        h.Pump();
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
        ASSERT_EQ(harken::CommandQueue::OriginalRangeState::Completed, h.Range().State());
        EXPECT_EQ(cancel ? harken::CommandQueue::OriginalControlDisposition::Cancelled :
            harken::CommandQueue::OriginalControlDisposition::Invalid, h.Range().ControlDisposition());
        const auto state = ControlOriginAccess::Observe(h.Range());
        EXPECT_GE(state.epoch, 2u);
        EXPECT_EQ(0u, state.activity);
        EXPECT_FALSE(state.native);
        EXPECT_EQ(kControlClosed, state.mode);
        harken::TimelineArmObservation control{};
        ASSERT_TRUE(engine.timelineArms.Observe(h.control.record.token, &control));
        EXPECT_EQ(0u, control.armedCompletionSequence);
        EXPECT_EQ(0u, control.startedBlockSequence);
        if (cancel) {
            EXPECT_EQ(before + 1u, control.tombstoneAudioSequence);
            EXPECT_FALSE(engine.timelineArms.Reclaim(h.control.record.token, control.tombstoneAudioSequence));
        }
        h.ExpectSourceUnchanged();
        ASSERT_NO_FATAL_FAILURE(ReturnControlOriginAndDisposePhysicalSource(h));
    }
}

TEST(AuthoredStopAudio, ControlOriginActualQuiescentTermReturnsQueuedOrRetainedPendingActorsAfterJoin) {
    for (const bool busy : {false, true}) {
        SCOPED_TRACE(busy);
        OriginalControlOriginHarness h;
        ASSERT_NO_FATAL_FAILURE(PreparePublishedControlOrigin(h));
        h.origin.Release();
        if (busy) {
            HeldArmIdentity hold(h.audio.engine->timelineArms, h.control.record.token);
            ASSERT_TRUE(hold.Wait());
            h.Pump();
            EXPECT_TRUE(ControlOriginAccess::HasPendingActor(h.audio.engine->mixer));
            EXPECT_EQ(1u, ControlOriginAccess::Observe(h.Range()).activity);
            hold.Finish();
        } else {
            EXPECT_FALSE(ControlOriginAccess::HasPendingActor(h.audio.engine->mixer));
            EXPECT_EQ(0u, ControlOriginAccess::Observe(h.Range()).pins);
        }
        // No borrowed capability survives Engine destruction; the native actual
        // range/PendingAck supplies its own authentic standing until joined End.
        ASSERT_EQ(VN_OK, Vn_Term());
    }
}
