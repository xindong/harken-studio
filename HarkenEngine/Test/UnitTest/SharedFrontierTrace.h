#pragma once

#include "Common/ObjectIdTestSupport.h"
#include "Engine.h"
#include "harken/core/CurrentWorld.h"
#include "harken/core/detail/CurrentWorldValidation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iterator>
#include <vector>

inline harken::EnginePlayableServiceRow* FrontierServiceForOwner(
    harken::Engine& engine, harken::PlayableInstanceHandle owner) {
    const auto view = engine.playableInstances.Classify(owner);
    if (!view.live) return nullptr;
    for (auto& service : engine.playableServices) {
        if (service.partition.Valid() && service.ownerInstance == owner &&
            service.ownerEpoch == view.scheduleEpoch && service.InvocationValid()) return &service;
    }
    return nullptr;
}

inline harken::EnginePlayableServiceRow* FrontierActiveResourceService(
    harken::Engine& engine, harken::EnginePlayableServiceRow& root) {
    auto* service = &root;
    for (std::size_t depth = 0; depth < engine.playableServices.size(); ++depth) {
        if (!service->partition.Valid() || service->partitionRetiring) return nullptr;
        if (!service->frontierChildService.Valid())
            return service->activeLeafInstance != 0 &&
                engine.playableInstances.Classify(service->activeLeafInstance).live ? service : nullptr;
        service = engine.ResolvePlayableService(service->frontierChildService);
        if (service == nullptr) return nullptr;
    }
    return nullptr;
}

inline void ExpectEmptyInitialFrontierApplied(harken::Engine& engine, const harken::EnginePlayableServiceRow& row) {
    ASSERT_EQ(harken::PlayableControlLifecycle::Preparing, row.controlLifecycle);
    ASSERT_EQ(0u, row.scheduleLeafCount);
    const auto entry = row.frontierEntry;
    const auto* activation = engine.decisions.ResolveFrontierActivation(entry.coordinator);
    ASSERT_NE(nullptr, activation);
    const auto quantum = activation->Descriptor().primary.absoluteQuantum;
    while (engine.mixer.BlocksRendered() <= quantum) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine.nullBackend.Pump(1);
    }
    harken::FrontierActivationReceipt receipt{};
    ASSERT_EQ(harken::FrontierReceiptRead::Ready, activation->Read(entry, engine.mixer.BlocksRendered(), &receipt));
    ASSERT_EQ(harken::FrontierReceiptOutcome::Applied, receipt.outcome);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::PlayableControlLifecycle::Active, row.controlLifecycle);
}

inline void ExpectTransferredDecisionsReclaimed(harken::Engine& engine, std::uint32_t baseline) {
    std::uint32_t transferred = 0;
    for (const auto& record : engine.decisionRetirements) {
        if (!record.decision.Valid()) continue;
        ASSERT_TRUE(engine.decisions.Contains(record.decision));
        ASSERT_EQ(harken::EngineDecisionOwnershipPhase::Retiring, record.phase);
        ++transferred;
    }
    ASSERT_EQ(baseline + transferred, engine.decisions.LiveCount());
    ASSERT_EQ(VN_OK, Vn_Update());
    if (engine.decisions.LiveCount() != baseline) {
        auto& groups = engine.timelineGroupCaptureScratch;
        const auto count = engine.timelinePool.CaptureLivePlan03Groups(groups.data(), groups.size());
        ASSERT_GT(count, 0u);
        ASSERT_LE(count, groups.size());
        auto boundary = engine.mixer.BlocksRendered();
        for (std::size_t i = 0; i < count; ++i) {
            const auto decision = groups[i].identity.decision;
            ASSERT_LT(decision.slot, engine.decisionRetirements.size());
            const auto& record = engine.decisionRetirements[decision.slot];
            ASSERT_EQ(decision.slot, record.decision.slot);
            ASSERT_EQ(decision.generation, record.decision.generation);
            ASSERT_EQ(harken::EngineDecisionOwnershipPhase::Retiring, record.phase);
            const auto state = groups[i].lease->Capture().state;
            ASSERT_TRUE(state == harken::AudioSliceState::SliceInvalidated || state == harken::AudioSliceState::SliceApplied);
            boundary = std::max(boundary, groups[i].identity.absoluteQuantum + 2u);
        }
        while (engine.mixer.BlocksRendered() < boundary) {
            engine.nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    EXPECT_EQ(baseline, engine.decisions.LiveCount());
}

inline const harken::VoiceRenderContext::OwnedVariableWeight* FrontierOwnedVariableWeight(
    const harken::VoiceRenderContext& voice, const harken::EnginePlayableServiceRow& row) {
    const harken::VariableWeightControlIdentity owner{row.ownerInstance, row.ownerEpoch, 1};
    const auto found = std::find_if(voice.variableWeightControls.begin(),
        voice.variableWeightControls.begin() + voice.variableWeightControlCount,
        [&](const auto& control) { return control.owner == owner; });
    return found == voice.variableWeightControls.begin() + voice.variableWeightControlCount ? nullptr : &*found;
}

inline harken::PlayablePropertyPayloadViewV1 FrontierOccurrencePayload() {
    harken::PlayablePropertyPayloadViewV1 payload{};
    payload.values[3].bits = 1;
    payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    return payload;
}

inline harken::PlayablePropertyPayloadViewV1 FrontierCanonicalPayload() {
    auto payload = FrontierOccurrencePayload();
    payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    payload.values[6].objectId = VN_BUS_MASTER;
    return payload;
}

struct QueueSequenceBranchesWorldImage {
    std::vector<harken::PlayablePropertyProgramViewV1> properties{7};
    std::vector<harken::PlayableOccurrencePropertyViewV1> occurrences{6};
    std::vector<harken::ContainerChildProgramViewV2> children{6};
    std::vector<harken::ContainerProgramViewV3> containers{3};
    harken::ActionStepProgramViewV1 step{};
    harken::ActionProgramViewV1 action{};
    harken::ContainerWorldInstallViewV1 world{};

    QueueSequenceBranchesWorldImage(const std::array<std::uint64_t, 4>& revisions,
                                    std::uint32_t cycles,
                                    harken::ContainerModeV1 outerMode = harken::ContainerModeV1::SequenceQueue,
                                    harken::ContainerModeV1 innerMode = harken::ContainerModeV1::Sequence) {
        const auto loopIndex = static_cast<std::uint32_t>(
            harken::playableproperties::PlayablePropertyIdV1::LoopCount) - 1u;
        for (std::uint32_t index = 0; index < properties.size(); ++index) {
            auto& property = properties[index];
            property.contentPropertyKind = index < 4
                ? harken::playableproperties::ContentPropertyKindV1::Sound
                : harken::playableproperties::ContentPropertyKindV1::Container;
            property.stableId = harkentest::ObjectId(2300u + index);
            property.authoredStableOrder = index;
            property.contentRevision = index < 4 ? revisions[index] : 5000u + index;
            property.routeStableId = VN_BUS_MASTER;
            property.compatibilityFingerprint = index + 1u;
            property.payload = FrontierCanonicalPayload();
            if (index == 4 || index == 5 ||
                (index == 6 && (outerMode == harken::ContainerModeV1::Sequence || outerMode == harken::ContainerModeV1::Random))) {
                property.payload.presentMask |= UINT64_C(1) << loopIndex;
                property.payload.values[loopIndex].bits = index == 6 ? 2u * cycles : 2u;
            }
            property.programSemanticFingerprint =
                harken::ComputePlayablePropertyFingerprintV1(property);
        }
        for (std::uint32_t index = 0; index < children.size(); ++index) {
            auto& child = children[index];
            child.stableChildOrdinal = 11u + index;
            child.authoredStableOrder = index % 2u;
            child.playable = Ref(index);
            const auto parentMode = index < 4 ? innerMode : outerMode;
            child.frequencyWeight = parentMode == harken::ContainerModeV1::Random ? 1u : 0u;
            child.orderWeight = parentMode == harken::ContainerModeV1::RandomQueue ? 1u : 0u;
            child.queueStopPolicy = parentMode == harken::ContainerModeV1::SequenceQueue ||
                    parentMode == harken::ContainerModeV1::RandomQueue
                ? harken::QueueStopPolicyV1::AdvanceQueue : harken::QueueStopPolicyV1::AbortOwningQueue;
            auto& occurrence = occurrences[index];
            const auto parentIndex = index < 4 ? 4u + index / 2u : 6u;
            occurrence.parentKind = harken::playableproperties::ContentPropertyKindV1::Container;
            occurrence.parentStableId = properties[parentIndex].stableId;
            occurrence.parentProgramSemanticFingerprint = properties[parentIndex].programSemanticFingerprint;
            occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal;
            occurrence.childPlayable = child.playable;
            occurrence.payload = FrontierOccurrencePayload();
            occurrence.occurrenceSemanticFingerprint =
                harken::ComputePlayableOccurrenceFingerprintV1(occurrence);
        }
        for (std::uint32_t index = 0; index < containers.size(); ++index) {
            auto& container = containers[index];
            container.containerStableId = properties[4u + index].stableId;
            container.programSemanticFingerprint = properties[4u + index].programSemanticFingerprint;
            container.authoredStableOrder = 4u + index;
            container.mode = index < 2 ? innerMode : outerMode;
            if (container.mode == harken::ContainerModeV1::Random) container.avoidLastN = 1;
            container.memoryScope = container.mode == harken::ContainerModeV1::RandomQueue
                ? harken::MemoryScopeV1::Emitter : harken::MemoryScopeV1::Global;
            container.children = children.data() + 2u * index;
            container.childCount = 2;
            if (index == 2 && (outerMode == harken::ContainerModeV1::SequenceQueue ||
                              outerMode == harken::ContainerModeV1::RandomQueue)) {
                container.queueCycle = cycles == 1 ? harken::QueueCycleV1::Once : harken::QueueCycleV1::Loop;
                container.queueCycleCount = cycles == 1 ? 0u : cycles;
            }
        }
        step.opcode = harken::ActionOpcodeV1::Play;
        step.target.kind = harken::ActionTargetKindV1::Container;
        step.target.stableId = properties[6].stableId;
        step.target.expectedProgramSemanticFingerprint = properties[6].programSemanticFingerprint;
        for (auto* time : {&step.delay, &step.fade, &step.cooldown}) {
            time->domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
            time->denominator = 1;
        }
        action.actionStableId = harkentest::ObjectId(2200);
        action.steps = &step;
        action.stepCount = 1;
        action.expandedStepOccurrenceCount = 1;
        action.expandedFiringCount = 1;
        action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(action);
        world.actions = &action;
        world.actionCount = 1;
        world.playableProperties = properties.data();
        world.playablePropertyCount = properties.size();
        world.playableOccurrences = occurrences.data();
        world.playableOccurrenceCount = occurrences.size();
        world.containers = containers.data();
        world.containerCount = containers.size();
        world.identity.capabilityMask = harken::detail::ComputeCurrentWorldLocalCapabilityMaskV1(world);
        world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(world);
    }

    harken::PlayableRefV1 Ref(std::uint32_t index) const {
        const auto& property = properties[index];
        harken::PlayableRefV1 ref{};
        ref.kind = index < 4 ? harken::PlayableKindV1::Sound : harken::PlayableKindV1::Container;
        ref.stableId = property.stableId;
        ref.expectedProgramSemanticFingerprint = property.programSemanticFingerprint;
        ref.contentRevision = property.contentRevision;
        ref.routeStableId = property.routeStableId;
        ref.compatibilityFingerprint = property.compatibilityFingerprint;
        return ref;
    }

    void WrapRoot(harken::ContainerModeV1 mode, std::uint32_t count) {
        for (std::uint32_t depth = 0; depth < count; ++depth) {
            const auto priorRoot = Ref(static_cast<std::uint32_t>(properties.size() - 1u));
            auto property = properties.back();
            property.stableId = harkentest::ObjectId(2300u + properties.size());
            property.authoredStableOrder++;
            property.contentRevision++;
            property.payload = FrontierCanonicalPayload();
            property.programSemanticFingerprint = harken::ComputePlayablePropertyFingerprintV1(property);
            properties.push_back(property);
            harken::ContainerChildProgramViewV2 child{};
            child.stableChildOrdinal = 30u + depth;
            child.playable = priorRoot;
            if (mode == harken::ContainerModeV1::Random) child.frequencyWeight = 1;
            if (mode == harken::ContainerModeV1::RandomQueue) child.orderWeight = 1;
            child.queueStopPolicy = mode == harken::ContainerModeV1::SequenceQueue || mode == harken::ContainerModeV1::RandomQueue
                ? harken::QueueStopPolicyV1::AdvanceQueue : harken::QueueStopPolicyV1::AbortOwningQueue;
            children.push_back(child);
            harken::PlayableOccurrencePropertyViewV1 occurrence{};
            occurrence.parentKind = harken::playableproperties::ContentPropertyKindV1::Container;
            occurrence.parentStableId = property.stableId;
            occurrence.parentProgramSemanticFingerprint = property.programSemanticFingerprint;
            occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal;
            occurrence.childPlayable = child.playable;
            occurrence.payload = FrontierOccurrencePayload();
            occurrence.occurrenceSemanticFingerprint = harken::ComputePlayableOccurrenceFingerprintV1(occurrence);
            occurrences.push_back(occurrence);
            harken::ContainerProgramViewV3 container{};
            container.containerStableId = property.stableId;
            container.programSemanticFingerprint = property.programSemanticFingerprint;
            container.authoredStableOrder = property.authoredStableOrder;
            container.mode = mode;
            container.memoryScope = mode == harken::ContainerModeV1::RandomQueue
                ? harken::MemoryScopeV1::Emitter : harken::MemoryScopeV1::Global;
            container.childCount = 1;
            containers.push_back(container);
        }
        for (std::size_t index = 0; index < containers.size(); ++index)
            containers[index].children = children.data() + (index < 3u ? 2u * index : index + 3u);
        step.target.stableId = properties.back().stableId;
        step.target.expectedProgramSemanticFingerprint = properties.back().programSemanticFingerprint;
        action.programSemanticFingerprint = harken::ComputeActionProgramFingerprintV1(action);
        world.playableProperties = properties.data();
        world.playablePropertyCount = static_cast<std::uint32_t>(properties.size());
        world.playableOccurrences = occurrences.data();
        world.playableOccurrenceCount = static_cast<std::uint32_t>(occurrences.size());
        world.containers = containers.data();
        world.containerCount = static_cast<std::uint32_t>(containers.size());
        world.identity.capabilityMask = harken::detail::ComputeCurrentWorldLocalCapabilityMaskV1(world);
        world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(world);
    }
};

struct FrontierHeardArm {
    harken::TimelineArmToken token{};
    VnObjectId subject{};
    std::uint64_t start = 0;
    std::uint64_t complete = 0;
    harken::PlayableInstanceHandle leaf = 0;
    harken::PlayableInstanceHandle parent = 0;
    std::uint64_t expectedStart = 0;
    std::uint64_t leafEpoch = 0;
    std::uint64_t parentEpoch = 0;
    harken::PlayableInstanceHandle parentParent = 0;
    VnObjectId parentStableId{};
    harken::PlayableInstanceHandle root = 0;
    std::uint32_t containerDepth = 0;
    harken::SourceTerminalToken source{};
    harken::VoiceReleaseObserver release{};
    harken::SourceTerminalPayload terminal{};
    std::uint64_t sourceCompletionSequence = 0;
    std::uint64_t releasedAtBlock = 0;
    bool sourceDrivenArm = false;

    void ExpectNaturalSourceReleased() const {
        ASSERT_TRUE(release.Valid());
        if (source.Valid()) {
            EXPECT_EQ(harken::SourceTerminalKind::Natural, terminal.kind);
            EXPECT_GE(sourceCompletionSequence, start);
            EXPECT_GE(releasedAtBlock, sourceCompletionSequence);
        } else {
            ASSERT_TRUE(sourceDrivenArm);
            EXPECT_GE(complete, start);
            EXPECT_GE(releasedAtBlock, complete);
        }
        EXPECT_NE(0u, releasedAtBlock);
    }
};

struct FrontierTrace {
    std::vector<FrontierHeardArm> heard;
    struct AppliedControl {
        harken::TimelineArmToken token{};
        harken::TimelineEventKind kind{};
        VnObjectId subject{};
        std::uint64_t applied = 0;
        std::uint64_t complete = 0;
    };
    std::vector<AppliedControl> controls;

    void Capture(harken::Engine& engine) {
        for (std::uint32_t slot = 0; slot < engine.timelineArms.Capacity(); ++slot) {
            const auto* arm = engine.timelineArms.RowAt(slot);
            if (arm == nullptr) continue;
            if (arm->startedBlockSequence.load() == 0 && arm->frontierLastReadSequence.load() == 0) continue;
            const auto matches = [&](const harken::TimelineArmToken& token) {
                return token.slot == slot && token.generation == arm->generation &&
                    token.renderSlot == arm->renderSlot && token.renderGeneration == arm->renderGeneration;
            };
            auto control = std::find_if(controls.begin(), controls.end(),
                [&](const auto& item) { return matches(item.token); });
            if (control == controls.end()) {
                const auto classify = [&](const harken::TimelineArmToken& token,
                                          const harken::TimelineEventPayload& payload) {
                    if (!matches(token) || payload.kind == harken::TimelineEventKind::Start) return;
                    controls.push_back({token, payload.kind, payload.target.subjectId,
                        arm->frontierLastReadSequence.load() != 0 ? arm->frontierLastReadSequence.load() :
                            arm->startedBlockSequence.load(), arm->logicalCompletionSequence.load()});
                };
                // Retarget tables include entering Start arms as well as dedicated controls.
                for (const auto& service : engine.playableServices) {
                    if (!service.HasFeatureArrays()) continue;
                    for (std::size_t index = 0; index < service.DiscreteRetargetStorage().events.size(); ++index)
                        classify(service.DiscreteRetargetStorage().events[index].armToken,
                            service.DiscreteRetargetStorage().payloads[index]);
                }
                for (const auto& program : engine.variableControlRetargetPrograms)
                    classify(program.event.armToken, program.payload);
                control = std::find_if(controls.begin(), controls.end(),
                    [&](const auto& item) { return matches(item.token); });
            }
            if (control != controls.end()) {
                control->complete = arm->logicalCompletionSequence.load();
                continue;
            }
            if (arm->startedBlockSequence.load() == 0) continue;
            auto found = std::find_if(heard.begin(), heard.end(), [&](const auto& item) {
                return item.token.slot == slot && item.token.generation == arm->generation;
            });
            // A queue successor may be audible before its logic-side ownership is adopted.
            // Retry only unresolved identities while the exact arm generation still exists.
            if (found == heard.end() || (found->leaf == 0 && !found->release.Valid())) {
                harken::PlayableInstanceHandle leafHandle = 0;
                for (std::size_t logical = 0; leafHandle == 0 && logical < engine.playableStorage.memberCount; ++logical) {
                    std::size_t physical = 0;
                    if (engine.ResolvePlayableMemberSlot(logical, &physical) &&
                        matches(engine.ReadPlayableMember(physical).arm))
                        leafHandle = engine.ReadPlayableMemberReference(logical).leaf;
                }
                for (const auto& service : engine.playableServices) {
                    if (leafHandle == 0 && service.partition.Valid() && matches(service.arm) && service.activeLeafInstance != 0) {
                        leafHandle = service.activeLeafInstance;
                        break;
                    }
                }
                const auto leaf = engine.playableInstances.Classify(leafHandle);
                const auto parent = engine.playableInstances.Classify(leaf.parent);
                harken::ContainerPlaybackRow playback{};
                const bool bound = leaf.live && leaf.kind == harken::PlayableKind::Sound && parent.live;
                (void)engine.containerPlayback.Capture(leaf.parent, &playback);
                auto parentStableId = playback.containerStableId;
                if (bound && (parent.kind == harken::PlayableKind::Track || parent.kind == harken::PlayableKind::Piece)) {
                    harken::MonitorContentIdentity identity{};
                    if (engine.playableInstances.CaptureContentIdentity(leaf.parent, &identity))
                        parentStableId = identity.stableId;
                }
                harken::PlayableInstanceHandle root = 0;
                std::uint32_t containerDepth = 0;
                auto ancestor = leaf.parent;
                while (ancestor != 0 && containerDepth < engine.playableInstances.Capacity()) {
                    const auto node = engine.playableInstances.Classify(ancestor);
                    if (!node.live || node.scheduleEpoch == 0 || node.kind == harken::PlayableKind::Sound) break;
                    root = ancestor;
                    ancestor = node.parent;
                    if (node.kind == harken::PlayableKind::Container) ++containerDepth;
                }
                if (found == heard.end()) {
                    heard.push_back({{slot, arm->generation, arm->renderSlot, arm->renderGeneration},
                        arm->target.subjectId, arm->startedBlockSequence.load(), 0});
                    found = std::prev(heard.end());
                }
                found->leaf = bound ? leafHandle : 0;
                found->parent = bound ? leaf.parent : 0;
                found->expectedStart = arm->startFrame / engine.config.blockFrames + 1u;
                found->leafEpoch = bound ? leaf.scheduleEpoch : 0;
                found->parentEpoch = bound ? parent.scheduleEpoch : 0;
                found->parentParent = bound ? parent.parent : 0;
                found->parentStableId = bound ? parentStableId : VnObjectId{};
                found->root = ancestor == 0 ? root : 0;
                found->containerDepth = containerDepth;
                const auto render = (static_cast<std::uint64_t>(found->token.renderGeneration) << 32) |
                    found->token.renderSlot;
                const auto sourceSlot = engine.playableInstances.SourceRetirementSlot(found->leaf);
                // Source records retain their identity until the producer consumes release.
                const auto* source = sourceSlot < engine.sourceTerminals.Capacity()
                    ? engine.sourceTerminals.At(sourceSlot) : nullptr;
                if (source != nullptr && source->occupied && source->leaf == found->leaf &&
                    source->acceptedEpoch == found->leafEpoch && source->render == render) {
                    found->source = {sourceSlot, source->generation};
                    found->release = source->release;
                } else if (found->token.renderSlot < engine.timelineVoiceInstallIdentities.size()) {
                    const auto& install = engine.timelineVoiceInstallIdentities[found->token.renderSlot];
                    bool owningAncestor = false;
                    auto owner = found->leaf;
                    for (std::uint32_t depth = 0; owner != 0 && depth < engine.playableInstances.Capacity(); ++depth) {
                        const auto node = engine.playableInstances.Classify(owner);
                        if (!node.live) break;
                        owningAncestor = owningAncestor || owner == install.rootInstance;
                        owner = node.parent;
                    }
                    if (install.renderHandle == render && matches(install.startArm) &&
                        owningAncestor && install.target.subjectId == found->subject &&
                        install.onsetFrame == arm->startFrame) {
                        found->release = install.releaseObserver;
                        // Ordinary starts retain the source-driven lifecycle arm through completion.
                        found->sourceDrivenArm = true;
                    }
                    if (!found->sourceDrivenArm && found->leaf == 0 &&
                        install.renderHandle == render && matches(install.startArm) &&
                        install.onsetFrame == arm->startFrame) {
                        const auto sameTarget = [](const auto& left, const auto& right) {
                            return left.programSemanticFingerprint == right.programSemanticFingerprint &&
                                left.clipGuidHigh == right.clipGuidHigh && left.clipGuidLow == right.clipGuidLow &&
                                left.subjectGuidHigh == right.subjectGuidHigh && left.subjectGuidLow == right.subjectGuidLow &&
                                left.trackId == right.trackId && left.clipId == right.clipId &&
                                left.subjectId == right.subjectId && left.loopIteration == right.loopIteration &&
                                left.occurrenceOrdinal == right.occurrenceOrdinal;
                        };
                        // A top-level signal-only Queue successor can use its root as the voice owner.
                        for (const auto& service : engine.playableServices) {
                            if (!service.partition.Valid() || !service.queueMode || service.parentService.Valid() ||
                                service.variableControlInstance.Valid() || !matches(service.arm) ||
                                service.renderHandle != render || service.rootInstance != install.rootInstance) continue;
                            const auto rootView = engine.playableInstances.Classify(service.rootInstance);
                            if (!rootView.live || service.schedule.ownerInstance != service.rootInstance ||
                                service.schedule.ownerInstanceScheduleEpoch != rootView.scheduleEpoch ||
                                arm->programGeneration != service.rootInstance ||
                                service.payload.kind != harken::TimelineEventKind::Start ||
                                (service.payload.reserved & (harken::kTimelineEventTransientStartArm |
                                    harken::kTimelineEventTransientControlArm)) != 0 ||
                                !sameTarget(service.payload.target, arm->target) ||
                                !sameTarget(install.target, arm->target)) continue;
                            found->root = service.rootInstance;
                            found->release = install.releaseObserver;
                            found->sourceDrivenArm = true;
                            break;
                        }
                    }
                }
            }
            if (found->sourceDrivenArm) found->complete = arm->logicalCompletionSequence.load();
        }
        for (auto& sound : heard) {
            const auto* source = engine.sourceTerminals.Resolve(sound.source);
            if (source != nullptr && source->leaf == sound.leaf && source->acceptedEpoch == sound.leafEpoch &&
                source->render == ((static_cast<std::uint64_t>(sound.token.renderGeneration) << 32) |
                                   sound.token.renderSlot) &&
                engine.sourceTerminals.Capture(sound.source, engine.mixer.BlocksRendered(), &sound.terminal)) {
                sound.sourceCompletionSequence = source->completedSequence.load(std::memory_order_acquire);
                sound.complete = sound.sourceCompletionSequence;
            }
            if (sound.release.Valid() && sound.releasedAtBlock == 0 && engine.voices.ReleaseObserved(sound.release))
                sound.releasedAtBlock = engine.mixer.BlocksRendered();
        }
    }
};

struct SelectionInputTrace {
    struct Selection {
        VnObjectId program{};
        std::vector<std::uint64_t> input;
        std::vector<std::uint32_t> expected;
        std::vector<VnObjectId> subjects;
        std::vector<harken::SelectionPathNodeV1> path;
        std::uint32_t eventIndex = 0;
        std::uint32_t occurrenceOrdinal = 0;
        std::uint32_t loopIteration = 0;
        std::uint64_t selectionOrdinal = 0;
        bool queue = false;
        std::uint32_t cursor = 0;
    };
    std::vector<Selection> selections;
    FrontierTrace actual;

    void Attach(harken::Engine& engine) {
        engine.selectionInputContextForTest = this;
        engine.selectionInputHookForTest = CaptureSelection;
        engine.queueInputHookForTest = CaptureQueue;
    }

    static void AddProgram(Selection& result, const harken::ContainerProgramViewV3& program) {
        result.program = program.containerStableId;
        result.input.insert(result.input.end(), {program.containerStableId.high, program.containerStableId.low, program.programSemanticFingerprint,
            static_cast<std::uint64_t>(program.mode), static_cast<std::uint64_t>(program.memoryScope),
            program.childCount, program.avoidLastN, program.initialSequenceCursor,
            static_cast<std::uint64_t>(program.sequenceEndPolicy)});
        for (std::uint32_t index = 0; index < program.childCount; ++index) {
            const auto& child = program.children[index];
            result.input.insert(result.input.end(), {child.stableChildOrdinal, child.authoredStableOrder,
                child.frequencyWeight, child.orderWeight, child.playable.stableId.high, child.playable.stableId.low,
                child.playable.expectedProgramSemanticFingerprint});
        }
    }

    static void AddExpectedSubjects(Selection& result, const harken::ContainerProgramViewV3& program) {
        for (const auto ordinal : result.expected) {
            const auto* end = program.children + program.childCount;
            const auto* child = std::find_if(program.children, end,
                [ordinal](const auto& item) { return item.stableChildOrdinal == ordinal; });
            ASSERT_NE(end, child);
            result.subjects.push_back(child->playable.stableId);
        }
    }

    static void CaptureSelection(harken::Engine& engine, const harken::SelectionPlayRequestV1& request, void* context) {
        auto& trace = *static_cast<SelectionInputTrace*>(context);
        ASSERT_NE(nullptr, request.program);
        const auto& program = *request.program;
        const auto before = engine.selectionMemory.Snapshot(request.key);
        ASSERT_EQ(0u, before.pendingCount);
        std::vector<std::uint32_t> recent(before.recentCount);
        EXPECT_EQ(recent.size(), engine.selectionMemory.CopyRecent(request.key, recent.data(), recent.size()));
        Selection result{};
        AddProgram(result, program);
        result.eventIndex = request.eventIndex;
        result.occurrenceOrdinal = request.occurrenceOrdinal;
        result.loopIteration = request.loopIteration;
        result.selectionOrdinal = before.selectionOrdinal;
        if (request.pathCount != 0) result.path.assign(request.path, request.path + request.pathCount);
        result.input.insert(result.input.end(), {request.worldSemanticFingerprint, request.rootStableId.high, request.rootStableId.low,
            request.callerSeed, request.acceptedPostOrdinal, request.ticket,
            request.executeFrame, request.occurrenceOrdinal, request.loopIteration,
            request.key.emitterSlot, request.key.emitterGeneration, before.found,
            before.resetEpoch, before.selectionOrdinal, before.sequenceCursor, before.sequenceStopped,
            before.rotationCursor, before.recentCount, before.permutationCount, request.pathCount});
        for (std::uint32_t index = 0; index < request.pathCount; ++index) {
            const auto& node = request.path[index];
            result.input.insert(result.input.end(), {static_cast<std::uint64_t>(node.kind), node.objectId.high, node.objectId.low,
                node.trackId.high, node.trackId.low, node.clipId.high, node.clipId.low, node.clipGuidHigh, node.clipGuidLow, node.childOrdinal});
        }
        result.input.insert(result.input.end(), recent.begin(), recent.end());
        std::vector<harken::SelectionChildV1> children;
        for (std::uint32_t index = 0; index < program.childCount; ++index) {
            const auto& child = program.children[index];
            children.push_back({child.stableChildOrdinal, child.authoredStableOrder, child.frequencyWeight, child.orderWeight});
        }
        std::uint32_t ordinal = 0;
        if (program.mode == harken::ContainerModeV1::Random) {
            ASSERT_EQ(harken::RandomDisciplineV1::Draw, program.randomDiscipline);
            harken::SelectionSeedInputV1 input{};
            input.worldSemanticFingerprint = request.worldSemanticFingerprint;
            input.programStableId = request.key.containerStableId;
            input.programSemanticFingerprint = request.key.programSemanticFingerprint;
            input.memoryScope = request.key.memoryScope;
            input.emitterSlot = request.key.emitterSlot;
            input.emitterGeneration = request.key.emitterGeneration;
            input.resetEpoch = before.resetEpoch;
            input.rootStableId = request.rootStableId;
            input.callerSeed = request.callerSeed;
            input.acceptedPostOrdinal = request.acceptedPostOrdinal;
            input.pathCount = request.pathCount;
            input.path = request.path;
            input.occurrenceOrdinal = request.occurrenceOrdinal;
            input.loopIteration = request.loopIteration;
            input.selectionOrdinal = before.selectionOrdinal + 1u;
            std::uint64_t seed = 0;
            ASSERT_TRUE(harken::ComputeSelectionSeedV1(input, &seed));
            harken::SplitMix64 stream(seed);
            ASSERT_EQ(harken::SelectionStatusV1::Ok, harken::SelectRandomDrawV1(children.data(), children.size(),
                recent.data(), recent.size(), &stream, &ordinal));
        } else {
            ASSERT_EQ(harken::ContainerModeV1::Sequence, program.mode);
            harken::SequenceSelectionV1 selected{};
            ASSERT_EQ(harken::SelectionStatusV1::Ok, harken::SelectSequenceV1(children.data(), children.size(),
                before.found ? before.sequenceCursor : program.initialSequenceCursor, before.sequenceStopped,
                program.sequenceEndPolicy, &selected));
            ordinal = selected.selectedChildOrdinal;
        }
        result.expected.push_back(ordinal);
        AddExpectedSubjects(result, program);
        trace.selections.push_back(std::move(result));
    }

    static void CaptureQueue(const harken::ContainerProgramViewV3& program, const harken::RandomQueueSeedContext& seed,
                             const harken::EffectiveQueueState& before, void* context) {
        auto& trace = *static_cast<SelectionInputTrace*>(context);
        Selection result{};
        AddProgram(result, program);
        result.queue = true;
        result.cursor = before.cursor;
        result.input.insert(result.input.end(), {seed.worldSemanticFingerprint, seed.rootStableId.high, seed.rootStableId.low, seed.callerSeed,
            seed.acceptedPostOrdinal, seed.emitterSlot, seed.emitterGeneration, seed.resetEpoch,
            before.version, before.cursor, before.currentChildOrdinal, before.cycleOrdinal,
            before.selectionOrdinal, before.randomInitialRecentCount});
        result.input.insert(result.input.end(), before.randomInitialRecent.begin(),
            before.randomInitialRecent.begin() + before.randomInitialRecentCount);
        result.expected.resize(program.childCount);
        if (program.mode == harken::ContainerModeV1::RandomQueue) {
            std::vector<harken::SelectionChildV1> children(program.childCount);
            std::vector<std::uint32_t> remaining(program.childCount);
            ASSERT_EQ(harken::SelectionStatusV1::Ok, harken::BuildRandomQueuePermutationV1(program, seed, before,
                children.data(), remaining.data(), result.expected.data(), program.childCount));
        } else {
            ASSERT_EQ(harken::ContainerModeV1::SequenceQueue, program.mode);
            for (std::uint32_t index = 0; index < program.childCount; ++index)
                result.expected[index] = program.children[index].stableChildOrdinal;
        }
        AddExpectedSubjects(result, program);
        if (before.currentChildOrdinal != 0) {
            ASSERT_LT(before.cursor, result.expected.size());
            EXPECT_EQ(result.expected[before.cursor], before.currentChildOrdinal);
        }
        trace.selections.push_back(std::move(result));
    }

    void ExpectNestedFixture(harken::ContainerModeV1 outer, harken::ContainerModeV1 inner,
                             const FrontierTrace& playback) const {
        const auto isQueue = [](harken::ContainerModeV1 mode) {
            return mode == harken::ContainerModeV1::RandomQueue || mode == harken::ContainerModeV1::SequenceQueue;
        };
        std::array<std::uint32_t, 2> visits{};
        std::vector<VnObjectId> parentOrder;
        std::array<std::vector<const Selection*>, 3> byProgram;
        for (const auto& selection : selections) {
            ASSERT_GE(selection.program, harkentest::ObjectId(2304u));
            ASSERT_LE(selection.program, harkentest::ObjectId(2306u));
            byProgram[selection.program.low - 2304u].push_back(&selection);
        }
        if (outer == harken::ContainerModeV1::VariableControl) {
            EXPECT_TRUE(byProgram[2].empty());
            visits = {1, 1};
        } else {
            ASSERT_FALSE(byProgram[2].empty());
            std::vector<VnObjectId> parents;
            if (isQueue(outer)) parents = byProgram[2].front()->subjects;
            else {
                EXPECT_EQ(2u, byProgram[2].size());
                for (std::size_t index = 0; index < byProgram[2].size(); ++index) {
                    const auto& input = *byProgram[2][index];
                    EXPECT_TRUE(input.path.empty());
                    EXPECT_EQ(index, input.selectionOrdinal);
                    EXPECT_EQ(index, input.loopIteration);
                    parents.insert(parents.end(), input.subjects.begin(), input.subjects.end());
                }
            }
            if (isQueue(outer) && inner == harken::ContainerModeV1::VariableControl) parents.resize(1);
            parentOrder = parents;
            for (const auto parent : parents) {
                ASSERT_GE(parent, harkentest::ObjectId(2304u));
                ASSERT_LE(parent, harkentest::ObjectId(2305u));
                ++visits[parent.low - 2304u];
            }
        }
        std::vector<std::pair<VnObjectId, VnObjectId>> expected;
        for (std::size_t branch = 0; branch < visits.size(); ++branch) {
            const auto parent = harkentest::ObjectId(2304u + branch);
            const auto& inputs = byProgram[branch];
            if (visits[branch] == 0) {
                EXPECT_TRUE(inputs.empty());
                continue;
            }
            if (inner == harken::ContainerModeV1::VariableControl) {
                EXPECT_TRUE(inputs.empty());
                for (std::uint32_t visit = 0; visit < visits[branch]; ++visit) {
                    expected.emplace_back(parent, harkentest::ObjectId(2300u + 2u * branch));
                    expected.emplace_back(parent, harkentest::ObjectId(2301u + 2u * branch));
                }
            } else if (isQueue(inner)) {
                ASSERT_FALSE(inputs.empty());
                ASSERT_EQ(2u, inputs.front()->subjects.size());
                for (const auto* input : inputs) {
                    EXPECT_TRUE(input->queue);
                    EXPECT_EQ(inputs.front()->expected, input->expected);
                }
                for (std::uint32_t visit = 0; visit < visits[branch]; ++visit)
                    for (const auto subject : inputs.front()->subjects) expected.emplace_back(parent, subject);
            } else {
                EXPECT_EQ(2u * visits[branch], inputs.size());
                for (std::size_t index = 0; index < inputs.size(); ++index) {
                    const auto& input = *inputs[index];
                    EXPECT_FALSE(input.queue);
                    EXPECT_EQ(index, input.selectionOrdinal);
                    for (const auto& path : input.path) {
                        EXPECT_EQ(harken::PlayableKindV1::Container, path.kind);
                        EXPECT_EQ(harkentest::ObjectId(2306u), path.objectId);
                        EXPECT_EQ(15u + branch, path.childOrdinal);
                    }
                    for (const auto subject : input.subjects) expected.emplace_back(parent, subject);
                }
            }
        }
        std::vector<std::pair<VnObjectId, VnObjectId>> observed;
        for (const auto& sound : playback.heard) observed.emplace_back(sound.parentStableId, sound.subject);
        for (std::size_t branch = 0; branch < visits.size(); ++branch) {
            std::vector<VnObjectId> expectedOrder, observedOrder;
            const auto parent = harkentest::ObjectId(2304u + branch);
            for (const auto& [owner, subject] : expected)
                if (owner == parent) expectedOrder.push_back(subject);
            for (const auto& [owner, subject] : observed)
                if (owner == parent) observedOrder.push_back(subject);
            EXPECT_EQ(expectedOrder, observedOrder) << "owning parent=" << parent;
        }
        if (!parentOrder.empty()) {
            std::vector<VnObjectId> observedParents;
            for (const auto& [owner, subject] : observed)
                if (observedParents.empty() || observedParents.back() != owner) observedParents.push_back(owner);
            EXPECT_EQ(parentOrder, observedParents);
        }
        std::sort(expected.begin(), expected.end());
        std::sort(observed.begin(), observed.end());
        EXPECT_EQ(expected, observed);
    }

    std::vector<const Selection*> SemanticInputs() const {
        std::vector<const Selection*> result;
        for (const auto& selection : selections) {
            // Queue lookahead can evaluate the same immutable input repeatedly.
            if (selection.queue && std::any_of(result.begin(), result.end(), [&](const auto* prior) {
                    return prior->queue && prior->input == selection.input && prior->expected == selection.expected;
                })) continue;
            result.push_back(&selection);
        }
        return result;
    }

    void ExpectSameInputsAndPlayback(const SelectionInputTrace& common) const {
        const auto commonInputs = common.SemanticInputs();
        const auto inputs = SemanticInputs();
        ::testing::Message mismatch;
        if (commonInputs.size() != inputs.size()) {
            for (const auto* selection : commonInputs) {
                mismatch << "common ";
                for (const auto value : selection->input) mismatch << value << ',';
                mismatch << " expected=" << ::testing::PrintToString(selection->expected) << '\n';
            }
            for (const auto* selection : inputs) {
                mismatch << "actual ";
                for (const auto value : selection->input) mismatch << value << ',';
                mismatch << " expected=" << ::testing::PrintToString(selection->expected) << '\n';
            }
        }
        SCOPED_TRACE(mismatch);
        ASSERT_EQ(commonInputs.size(), inputs.size());
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            SCOPED_TRACE(::testing::Message() << "selection=" << index << " program=" << inputs[index]->program);
            ASSERT_EQ(commonInputs[index]->input.size(), inputs[index]->input.size());
            for (std::size_t field = 0; field < inputs[index]->input.size(); ++field)
                EXPECT_EQ(commonInputs[index]->input[field], inputs[index]->input[field]) << "input field=" << field;
            EXPECT_EQ(commonInputs[index]->expected, inputs[index]->expected);
        }
        ASSERT_EQ(common.actual.heard.size(), actual.heard.size());
        if (actual.heard.empty()) return;
        const auto commonOrigin = common.actual.heard.front().start;
        const auto origin = actual.heard.front().start;
        for (std::size_t index = 0; index < actual.heard.size(); ++index) {
            SCOPED_TRACE(::testing::Message() << "heard=" << index);
            EXPECT_EQ(common.actual.heard[index].subject, actual.heard[index].subject);
            EXPECT_EQ(common.actual.heard[index].parentStableId, actual.heard[index].parentStableId);
            const auto& expected = common.actual.heard[index];
            const auto& heard = actual.heard[index];
            EXPECT_EQ(expected.expectedStart, expected.start);
            EXPECT_EQ(heard.expectedStart, heard.start);
            ASSERT_GE(expected.start, commonOrigin);
            ASSERT_GE(heard.start, origin);
            // Each entry owns its real Source onset. Compare exact authored
            // spacing and duration relative to that independently checked onset.
            EXPECT_EQ(expected.start - commonOrigin, heard.start - origin);
            EXPECT_EQ(expected.complete == 0, heard.complete == 0);
            if (expected.complete != 0 && heard.complete != 0) {
                ASSERT_GE(expected.complete, expected.start);
                ASSERT_GE(heard.complete, heard.start);
                EXPECT_EQ(expected.complete - expected.start, heard.complete - heard.start);
            }
        }
    }
};
