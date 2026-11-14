#include "SignalPublication.h"
#include "Engine.h"
#include "HarkenApiInternal.h"
#include "harken/render/BusActivityPlan.h"

namespace harken {
WorldInstallStatus PrepareSignalPublication(Engine& engine,
    std::span<const SignalRelationV1> relations, std::span<const SignalReceiverV1> receivers,
    std::span<const SignalPropertyInput> properties, std::uint64_t contentIdentity,
    const TopologyPlanView* topology, PreparedSignalPublication& out,
    std::span<const SignalRetainedDefinitionInput> retained) noexcept {
    try {
        PreparedSignalPublication next;
        next.predecessor = engine.acceptedSignalRevision;
        next.changed = !relations.empty() || !receivers.empty() || engine.acceptedSignalPlan != nullptr;
        if (!next.changed) { out = std::move(next); return WorldInstallStatus::Ok; }
        if (next.predecessor == UINT64_MAX) return WorldInstallStatus::Capacity;
        if (!relations.empty() || !receivers.empty()) {
            std::vector<SignalBusInput> buses(engine.BusCapacity());
            if (topology != nullptr) {
                if (!topology->valid || topology->slotCount > buses.size()) return WorldInstallStatus::InvalidWorld;
                for (std::uint32_t i = 0; i < topology->slotCount; ++i)
                    if (topology->alive[i] != 0) buses[i] = {topology->busIds[i], topology->generations[i]};
            } else {
                for (std::uint32_t i = 0; i < engine.buses.BusCount(); ++i)
                    if (engine.buses.AliveAt(i)) buses[i] = {engine.buses.IdAt(i), engine.buses.GenerationAt(i)};
            }
            const SignalBlockFormat format{engine.config.mixChannelCount, engine.config.blockFrames,
                engine.config.mixRate.SampleRate(), engine.config.mixChannelLayout};
            const auto validBus = [&](const SignalEndpointRefV1& endpoint) {
                return endpoint.kind != SignalEndpointKindV1::Bus ||
                    std::any_of(buses.begin(), buses.end(), [&](const auto& bus) {
                        return bus.id != VnObjectId{} && bus.id == endpoint.stableId;
                    });
            };
            for (const auto& relation : relations)
                if (!validBus(relation.source) || !validBus(relation.target)) return WorldInstallStatus::InvalidWorld;
            for (const auto& receiver : receivers)
                if (!validBus(receiver.target)) return WorldInstallStatus::InvalidWorld;
            auto owner = PrepareSignalWorldPlan(format, relations, receivers, properties, buses, retained);
            if (!owner) return WorldInstallStatus::Capacity;
            // Match the ordinary live gain-setter's ten millisecond transition.
            const auto rampFrames = (static_cast<std::uint64_t>(format.sampleRate) * 10 + 999) / 1000;
            if (!owner->ConfigurePublication(next.predecessor + 1, contentIdentity,
                    engine.acceptedSignalPlan.get(), rampFrames))
                return WorldInstallStatus::Capacity;
            next.owner = std::move(owner);
        }
        out = std::move(next);
        return WorldInstallStatus::Ok;
    } catch (...) { return WorldInstallStatus::Capacity; }
}

WorldInstallStatus PrepareWorldSignals(Engine& engine, const ContainerWorldInstallViewV1& world,
    std::uint64_t generation, const TopologyPlanView* topology, PreparedSignalPublication& out) noexcept {
    try {
        std::vector<SignalPropertyInput> properties;
        if (world.signalRelationCount != 0 || world.signalReceiverCount != 0) {
            properties.reserve(world.playablePropertyCount);
            const ProgramSourceIdentityV1 source{generation, 0, 0, 0, world.sourceBodyVintage};
            for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i)
                properties.push_back({world.playableProperties + i, source});
        }
        return PrepareSignalPublication(engine, {world.signalRelations, world.signalRelationCount},
            {world.signalReceivers, world.signalReceiverCount}, properties,
            ComputeCurrentWorldFingerprintV5(world), topology, out);
    } catch (...) { return WorldInstallStatus::Capacity; }
}

bool CanCommitSignals(const Engine& engine, const PreparedSignalPublication& prepared) noexcept {
    return prepared.predecessor == engine.acceptedSignalRevision;
}

WorldInstallStatus PreparePackSignals(Engine& engine, const PackComposition::PublishedView* published,
    const TopologyPlanView* topology, PreparedSignalPublication& out) noexcept {
    try {
        if (published == nullptr)
            return PrepareSignalPublication(engine, {}, {}, {}, 1, topology, out);
        std::vector<SignalPropertyInput> properties;
        std::vector<SignalEndpointRefV1> endpoints;
        for (const auto& relation : published->signalRelations) {
            endpoints.push_back(relation.source); endpoints.push_back(relation.target);
        }
        for (const auto& receiver : published->signalReceivers) endpoints.push_back(receiver.target);
        std::sort(endpoints.begin(), endpoints.end(), [](const auto& a, const auto& b) {
            return std::pair(a.kind, a.stableId) < std::pair(b.kind, b.stableId);
        });
        endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
        // Content identity deliberately excludes owner revision and signal
        // parameters. Exact playback ownership remains in each scalar proof.
        std::uint64_t content = 14695981039346656037ull;
        const auto hash = [&](std::uint64_t value) {
            for (unsigned i = 0; i < 8; ++i) { content ^= value & 255; content *= 1099511628211ull; value >>= 8; }
        };
        for (const auto& owner : published->owners) {
            if (owner.readiness != PackOwnerReadiness::Active) continue;
            hash(owner.packStableId); hash(ComputeCurrentWorldFingerprintV5(owner.view));
            if (!endpoints.empty()) {
                const ProgramSourceIdentityV1 source{published->compositeGeneration, owner.packStableId,
                    owner.partitionRevision, 0, owner.view.sourceBodyVintage};
                for (std::uint32_t i = 0; i < owner.view.playablePropertyCount; ++i)
                    properties.push_back({owner.view.playableProperties + i, source});
            }
        }
        std::vector<SignalRetainedDefinitionInput> retained;
        struct Context {
            const std::vector<SignalEndpointRefV1>& endpoints;
            std::vector<SignalRetainedDefinitionInput>& retained;
        } context{endpoints, retained};
        if (!endpoints.empty()) engine.packComposition.ForEachRetainedDefinition(&context,
            [](void* opaque, const PackPlaybackView& view, const PackClosureOwnerSnapshot& owner,
               const ProgramSourceIdentityV1& source) {
                auto& context = *static_cast<Context*>(opaque);
                const auto add = [&](playableproperties::ContentPropertyKindV1 kind, VnObjectId id, std::uint64_t fingerprint) {
                    if (kind != playableproperties::ContentPropertyKindV1::Sound &&
                        kind != playableproperties::ContentPropertyKindV1::Container) return;
                    const auto endpointKind = kind == playableproperties::ContentPropertyKindV1::Sound
                        ? SignalEndpointKindV1::Sound : SignalEndpointKindV1::Container;
                    const auto key = std::pair(endpointKind, id);
                    const auto found = std::lower_bound(context.endpoints.begin(), context.endpoints.end(), key,
                        [](const auto& endpoint, const auto& key) { return std::pair(endpoint.kind, endpoint.stableId) < key; });
                    if (found == context.endpoints.end() || std::pair(found->kind, found->stableId) != key ||
                        found->expectedPropertyFingerprint != fingerprint) return;
                    if (endpointKind == SignalEndpointKindV1::Container &&
                        !view.ResolveContainer(id, found->expectedProgramFingerprint)) return;
                    context.retained.push_back({endpointKind, id, fingerprint, source});
                };
                for (std::uint32_t i = 0; i < owner.definitionView.playablePropertyCount; ++i) {
                    const auto& property = owner.definitionView.playableProperties[i];
                    add(property.contentPropertyKind, property.stableId, property.programSemanticFingerprint);
                    for (std::uint32_t j = 0; j < property.structuralAncestorCount; ++j) {
                        const auto& ancestor = property.structuralAncestors[j];
                        add(ancestor.contentPropertyKind, ancestor.stableId, ancestor.programSemanticFingerprint);
                    }
                }
            });
        return PrepareSignalPublication(engine, published->signalRelations, published->signalReceivers,
            properties, content == 0 ? 1 : content, topology, out, retained);
    } catch (...) { return WorldInstallStatus::Capacity; }
}

void RefreshRetainedPackSignals(Engine& engine) noexcept {
    if (!engine.packComposition.RetainedDefinitionsChanged()) return;
    if (!engine.acceptedSignalPlan || engine.packComposition.Authority() != PackAuthorityMode::Pack) {
        engine.packComposition.AcknowledgeRetainedDefinitions();
        return;
    }
    PreparedSignalPublication prepared;
    if (PreparePackSignals(engine, engine.packComposition.Published(), nullptr, prepared) != WorldInstallStatus::Ok) return;
    if (prepared.owner && prepared.owner->SameMembership(*engine.acceptedSignalPlan)) {
        engine.packComposition.AcknowledgeRetainedDefinitions();
        return;
    }
    PrivateSignalTopology topology(engine);
    if (!topology.Prepare(prepared)) return;
    CommitSignals(engine, prepared);
    topology.Commit();
    engine.packComposition.AcknowledgeRetainedDefinitions();
}
void AttachPreparedSignals(TopologyPlanPool& pool, std::uint32_t index,
    const PreparedSignalPublication& prepared) {
    pool.SetSignals(index, prepared.owner);
}
void CommitSignals(Engine& engine, PreparedSignalPublication& prepared) noexcept {
    assert(CanCommitSignals(engine, prepared));
    if (!prepared.changed) return;
    engine.acceptedSignalPlan = std::move(prepared.owner);
    ++engine.acceptedSignalRevision;
}

// Structural publications and ordinary wakes use the same owner census.
// A definition alone never creates a physical work-row peak.
static bool SelectRetainedBusActivity(Engine& engine, const TopologyPlanView& topology,
    std::span<const CommandRecord> commands, std::span<const BusTree::WorkBinding> providedWork,
    BusActivitySelection& selected) noexcept {
    const auto* current = engine.buses.AcceptedActivity();
    try {
        BusActivityRoots roots;
        if (!CaptureBusActivityOwners(engine, topology, commands, roots)) return false;
        const auto retainBus = [&](BusSlotRef bus) {
            if (bus.index < topology.slotCount && topology.alive[bus.index] &&
                topology.generations[bus.index] == bus.generation) roots.buses.push_back(bus);
        };
        // Master always has a plane, but is only a dependency root while it
        // can still produce output. A quiet proof must cover every command
        // accepted before this preparation, not an earlier completed block.
        const auto master = engine.buses.RefAt(0);
        const auto rawHead = engine.commands.PublishedHead();
        const auto initialHead = engine.commands.InitialSourcePublishedHead();
        BusTree::WorkObservation fact;
        const bool pristine = current == nullptr && rawHead == 0 && initialHead == 0;
        const bool quietMaster = engine.buses.CaptureWorkObservation(master,
            engine.buses.AcceptedWorkRef(master), fact) && fact.rendered && fact.localRecoveryReady &&
            !fact.inputActive && !fact.detectorActive && !fact.physicalVoice &&
            fact.sendTailFrames == 0 && fact.outputTailFrames == 0 &&
            fact.graphRevision == engine.acceptedBusTopologyRevision &&
            fact.commandFrontier == rawHead && fact.initialSourceFrontier == initialHead;
        if (!pristine && !quietMaster) retainBus(master);
        if (current != nullptr)
            for (const auto& binding : current->working)
                if (binding.bus.index != 0) retainBus(binding.bus);
        for (const auto& binding : providedWork) retainBus(binding.bus);
        for (std::uint32_t i = 0; i < topology.effectUpdateCount; ++i)
            retainBus(BusSlotRef::FromPacked(topology.effectUpdates[i].host));
        // An unconditional wake must be safe even if S is rejected. Keep all
        // A rows in that case, not merely the subset S proposed to retain.
        if (current != nullptr && current == engine.buses.PendingSleep() && current->sleepPrevious != nullptr)
            for (const auto& binding : current->sleepPrevious->working)
                if (binding.bus.index != 0) retainBus(binding.bus);
        const auto retainSignals = [&](const SignalRenderPlan* prior) {
            if (prior == nullptr || topology.signals == nullptr) return;
            std::vector<std::uint8_t> formal(prior->NodeCount());
            for (const auto& relation : prior->Relations())
                formal[relation.source] = formal[relation.target] = 1;
            for (std::uint32_t i = 0; i < topology.signals->NodeCount(); ++i) {
                const auto node = topology.signals->NodeDefinition(i);
                for (std::uint32_t j = 0; j < prior->NodeCount(); ++j) {
                    const auto other = prior->NodeDefinition(j);
                    if (!formal[j] && !other.receiver) continue;
                    if (node.kind == other.kind && node.stableId == other.stableId &&
                        node.busId == other.busId && node.busGeneration == other.busGeneration) {
                        roots.signals.push_back(i);
                        break;
                    }
                }
            }
        };
        retainSignals(current != nullptr ? current->signals.get() : nullptr);
        retainSignals(engine.buses.AcceptedActivityAlternate());
        std::vector<BusActivityDetectorDependency> detectors;
        const auto detector = [&](const auto& route) {
            if (route.targetBusIndex >= topology.slotCount || !topology.alive[route.targetBusIndex]) return;
            const BusSlotRef target{route.targetBusIndex, topology.generations[route.targetBusIndex]};
            if (route.sourceKind == VN_DYNAMIC_SOURCE_BUS && route.sourceIndex < topology.slotCount &&
                topology.alive[route.sourceIndex] && topology.generations[route.sourceIndex] == route.sourceGeneration)
                detectors.push_back({target, {route.sourceIndex, route.sourceGeneration}});
            else if (route.sourceKind == VN_DYNAMIC_SOURCE_VOICE &&
                engine.voices.IsAlive((static_cast<std::uint64_t>(route.sourceGeneration) << 32) | route.sourceIndex))
                roots.buses.push_back(target);
        };
        if (engine.dynamicRoutes.ActiveRouteCount() != 0) engine.dynamicRoutes.ForEachDefinition(detector);
        for (const auto& command : commands) {
            std::uint32_t index = kInvalidDynamicRoutePlanIndex;
            switch (command.opcode) {
            case kOpcodeSetBusTopologyPlan: index = command.payload.setBusTopologyPlan.dynamicRoutePlanIndex; break;
            case kOpcodeSetBusEffect: index = command.payload.setBusEffect.dynamicRoutePlanIndex; break;
            case kOpcodeClearBusEffect: index = command.payload.clearBusEffect.dynamicRoutePlanIndex; break;
            default: break;
            }
            if (index == kInvalidDynamicRoutePlanIndex) continue;
            const auto plan = engine.dynamicRoutes.View(index);
            if (!plan.valid) return false;
            for (std::uint32_t row = 0; row < plan.rowCount; ++row) detector(plan.rows[row]);
        }
        if (!PrepareBusActivitySelection(topology, topology.signals, roots.buses, roots.signals,
                detectors, roots.members, selected)) return false;
        return true;
    } catch (...) { return false; }
}

bool PrepareTopologyActivity(Engine& engine, const TopologyPlanView& topology,
    CommandQueue::BatchReservation& reservation, std::span<const CommandRecord> commands,
    CommandQueue::PreparedBatchProof& proof, const BusActivitySelection* selected,
    bool adoptsTopology, std::span<const BusTree::WorkBinding> providedWork,
    bool conditionalSleep, std::span<const BusTree::WorkObservation> sleeping) noexcept {
    if (!topology.valid || commands.empty() || commands.size() > UINT32_MAX) return false;
    const bool alreadyFrozen = proof.Valid();
    if (alreadyFrozen && !engine.commands.PreparedBatchMatches(proof, &reservation,
            static_cast<std::uint32_t>(commands.size()))) return false;
    struct PrivateRows {
        BusTree& tree;
        std::vector<BusTree::WorkRef> rows;
        ~PrivateRows() { for (auto row : rows) tree.ReleaseWorkRow(row); }
    } privateRows{engine.buses};
    BusActivityPlan* activity = nullptr;
    try {
        BusActivitySelection ownedSelection;
        if (selected == nullptr) {
            if (!SelectRetainedBusActivity(engine, topology, commands, providedWork, ownedSelection)) return false;
            selected = &ownedSelection;
        }
        const auto& selection = *selected;
        std::vector<BusTree::WorkBinding> working;
        std::size_t preparedCount = 0;
        std::vector<SignalRenderNodeInput> busBindings;
        working.reserve(selection.buses.size());
        if (topology.signals != nullptr) busBindings.reserve(selection.buses.size());
        privateRows.rows.reserve(selection.buses.size());
        for (const auto bus : selection.buses) {
            const BusSlotRef host{bus, topology.generations[bus]};
            auto work = engine.buses.AcceptedWorkRef(host);
            if (!work.Valid()) {
                const auto supplied = std::find_if(providedWork.begin(), providedWork.end(),
                    [&](const auto& binding) { return binding.bus == host; });
                if (supplied != providedWork.end()) work = supplied->work;
                else {
                    work = engine.buses.PrepareWorkRow();
                    if (!work.Valid()) return false;
                    privateRows.rows.push_back(work);
                }
                if (engine.buses.PreparedWorkRow(work) == nullptr) return false;
                auto& row = *engine.buses.PreparedWorkRow(work);
                if (supplied != providedWork.end()) {
                    if (row.busIndex != bus || row.generation != host.generation || row.id != topology.busIds[bus])
                        return false;
                } else if (engine.buses.RefAt(bus) == host && engine.buses.IdAt(bus) == topology.busIds[bus]) {
                    if (engine.PrepareBusWorkState(bus, row) != VN_OK ||
                        !engine.buses.RestoreDormantWork(row)) return false;
                } else {
                    // New incarnations have no inherited settings or physical
                    // effect ownership. The same batch installs prepared edits.
                    row.id = topology.busIds[bus];
                    row.busIndex = bus;
                    row.generation = host.generation;
                    row.faderAfterSlot = engine.config.fxSlotsPerBus;
                }
                row.parentIndex = topology.parents[bus];
                row.outputLayout = topology.outputLayouts[bus];
                topology.CopyDownmix(bus, row.downmixCoeffs);
                ++preparedCount;
            }
            working.push_back({host, work});
            if (topology.signals != nullptr) {
                SignalRenderNodeInput node;
                node.busIndex = bus;
                node.busId = node.stableId = topology.busIds[bus];
                node.busGeneration = host.generation;
                busBindings.push_back(node);
            }
        }
        const auto* previous = engine.buses.HasAcceptedActivity()
            ? engine.buses.AcceptedActivitySignals() : engine.acceptedSignalPlan.get();
        std::unique_ptr<SignalRenderPlan> signals;
        if (topology.signals != nullptr) {
            signals = topology.signals->PrepareProjection(selection.signalNodes, topology.slotCount, busBindings);
            const bool continuous = signals != nullptr && previous != nullptr &&
                topology.signals->SameContinuityDomain(*previous);
            if (!signals || !signals->ConfigurePublication(engine.buses.NextActivitySignalPublication(previous),
                    topology.signals->ContentIdentity(), previous,
                    (static_cast<std::uint64_t>(engine.config.mixRate.SampleRate()) * 10 + 999) / 1000,
                    continuous ? engine.buses.AcceptedActivityAlternate() : nullptr) ||
                (continuous && (!engine.buses.PrepareDormantSignalContinuity(*signals, *previous) ||
                    !engine.buses.PreparePendingSleepContinuity(*signals)))) return false;
        }
        const bool replace = previous != nullptr &&
            (signals == nullptr || !signals->SameContinuityDomain(*previous));
        activity = engine.buses.PrepareActivity(topology, working, std::move(signals),
            replace ? BusTree::SignalTransition::ReplaceContent : BusTree::SignalTransition::Preserve,
            replace ? previous : nullptr, adoptsTopology);
        if (activity == nullptr) return false;
        if (!engine.mixer.PrepareBusBacking(static_cast<std::uint32_t>(working.size()))) {
            engine.buses.CancelActivity(activity);
            engine.buses.DrainActivities();
            return false;
        }
        activity->meterLanes.reserve(working.size());
        for (const auto& binding : working)
            activity->meterLanes.push_back(engine.meterRing.PrepareLane(binding.bus.index));
        activity->preparedEffects.reserve(preparedCount);
        for (const auto& binding : working) {
            const auto ref = binding.work;
            const auto* preparedRow = engine.buses.PreparedWorkRow(ref);
            if (preparedRow == nullptr) continue;
            const auto& row = *preparedRow;
            const BusSlotRef host{row.busIndex, row.generation};
            {
                BusActivityPlan::PreparedEffects entry;
                entry.bus = host;
                entry.work = ref;
                std::copy_n(row.fxSlotEffectRef, kMaxFxSlotsPerBus, entry.effects.begin());
                // Transactions may commit their final logical mirrors before
                // ConsumePreparedBatch. Publish the final batch ownership,
                // not the old cold chain used to reconstruct this work row.
                const auto applyTopology = [&](std::uint32_t index) {
                    if (index == kInvalidTopologyPlanIndex) return;
                    const auto view = engine.topologyPlans.View(index);
                    for (std::uint32_t i = 0; view.valid && i < view.effectUpdateCount; ++i) {
                        const auto& update = view.effectUpdates[i];
                        if (update.host == host.Packed() && !update.releaseOwner)
                            if (const auto* preparedUpdate = PreparedBusEffectUpdate(update))
                                entry.effects = preparedUpdate->targetRefs;
                    }
                };
                for (const auto& command : commands) {
                    if (command.opcode == kOpcodeSetBusTopologyPlan)
                        applyTopology(command.payload.setBusTopologyPlan.planIndex);
                    else if (command.opcode == kOpcodeSetBusEffect) {
                        const auto& effect = command.payload.setBusEffect;
                        applyTopology(effect.topologyPlanIndex);
                        if (effect.busIndex == host.index && effect.slot < kMaxFxSlotsPerBus &&
                            (effect.busGeneration == 0 || effect.busGeneration == host.generation))
                            entry.effects[effect.slot] = {effect.effectType, effect.poolIndex, effect.poolGeneration};
                    } else if (command.opcode == kOpcodeClearBusEffect) {
                        const auto& effect = command.payload.clearBusEffect;
                        applyTopology(effect.topologyPlanIndex);
                        if (effect.busIndex == host.index && effect.slot < kMaxFxSlotsPerBus &&
                            (effect.busGeneration == 0 || effect.busGeneration == host.generation))
                            entry.effects[effect.slot] = {};
                    }
                }
                activity->preparedEffects.push_back(entry);
            }
        }
        activity->logicContext = &engine;
        activity->logicQueued = [](void* context, const BusActivityPlan& plan) noexcept {
            auto& owner = *static_cast<Engine*>(context);
            for (const auto& entry : plan.preparedEffects)
                owner.buses.SetLogicEffects(entry.work, entry.bus, entry.effects);
        };
        if ((conditionalSleep && !engine.buses.PrepareConditionalSleep(*activity, sleeping, engine.voices)) ||
            (!alreadyFrozen && !engine.commands.ProvePreparedBatch(&reservation, commands.data(),
                static_cast<std::uint32_t>(commands.size()), &proof)) ||
            !engine.commands.BindPreparedBusActivity(proof, *engine.buses.ActivityAttachment(activity))) {
            proof.Release();
            engine.buses.CancelActivity(activity);
            engine.buses.DrainActivities();
            return false;
        }
        return true;
    } catch (...) {
        if (activity != nullptr) engine.buses.CancelActivity(activity);
        engine.buses.DrainActivities();
        return false;
    }
}

bool AcknowledgeBusSleep(Engine& engine) noexcept {
    return engine.buses.AcknowledgeSleep();
}

static bool SelectBusActivityWake(Engine& engine, std::span<const CommandRecord> commands,
    BusActivitySelection& selected, bool& changed) noexcept {
    changed = false;
    const auto topology = engine.buses.AcceptedTopology();
    const auto* current = engine.buses.AcceptedActivity();
    // Bootstrap and the complete projection already own every possible route.
    if (!topology.valid || current == nullptr) return true;
    bool completeSignals = topology.signals == nullptr;
    if (topology.signals != nullptr && current->signals != nullptr) {
        completeSignals = true;
        for (std::uint32_t i = 0; i < topology.signals->NodeCount(); ++i)
            if (!current->signals->ContainsNodeDefinition(*topology.signals, i)) {
                completeSignals = false;
                break;
            }
    }
    if (engine.buses.PendingSleep() == nullptr && current->working.size() == topology.orderCount &&
        completeSignals) return true;
    try {
        if (!SelectRetainedBusActivity(engine, topology, commands, {}, selected)) return false;
        if (engine.buses.PendingSleep() == nullptr && selected.buses == current->order &&
            (topology.signals == nullptr || (current->signals != nullptr &&
             std::all_of(selected.signalNodes.begin(), selected.signalNodes.end(), [&](auto node) {
                return current->signals->ContainsNodeDefinition(*topology.signals, node);
             })))) return true;
        changed = true;
        return true;
    } catch (...) { return false; }
}

bool PrepareBusActivityWake(Engine& engine, CommandQueue::BatchReservation& reservation,
    std::span<const CommandRecord> commands, CommandQueue::PreparedBatchProof& proof) noexcept {
    BusActivitySelection selected;
    bool changed = false;
    if (!SelectBusActivityWake(engine, commands, selected, changed)) return false;
    return !changed || PrepareTopologyActivity(engine, engine.buses.AcceptedTopology(),
        reservation, commands, proof, &selected, false);
}

bool PreparedBusCommandBatch::PrepareWake() noexcept {
    if (proof_.Valid()) return false;
    BusActivitySelection selected;
    bool changed = false;
    if (!SelectBusActivityWake(engine_, {}, selected, changed)) return false;
    if (!changed) return true;
    wakeCommand_.opcode = kOpcodeSetBusActivity;
    commands_ = {&wakeCommand_, 1};
    return engine_.commands.ReserveBatch(1, &reservation_) &&
        PrepareTopologyActivity(engine_, engine_.buses.AcceptedTopology(), reservation_,
            commands_, proof_, &selected, false);
}

void ServiceBusActivity(Engine& engine) noexcept {
    if (!AcknowledgeBusSleep(engine)) return;
#if !defined(HARKEN_RT_QUALIFICATION)
    if (!engine.buses.AutomaticSleepEnabledForTest()) return;
#endif
    const auto topology = engine.buses.AcceptedTopology();
    const auto* current = engine.buses.AcceptedActivity();
    if (!topology.valid || current == nullptr) return;
    CommandQueue::BatchReservation reservation;
    CommandQueue::PreparedBatchProof proof;
    try {
        BusActivityRoots roots;
        if (!CaptureBusActivityOwners(engine, topology, {}, roots)) return;
        std::vector<BusTree::WorkObservation> facts(current->working.size());
        std::uint64_t block = 0;
        for (std::size_t i = 0; i < facts.size(); ++i) {
            const auto& binding = current->working[i];
            if (engine.buses.CaptureWorkObservation(binding.bus, binding.work, facts[i]))
                block = std::max(block, facts[i].blockSequence);
        }
        SignalRenderPlan::ActivityObservation signals;
        const bool signalFacts = current->signals && current->signals->CaptureActivityObservation(signals);
        if (signalFacts) block = std::max(block, signals.blockSequence);
        if (block == 0) return;
        const auto hold = engine.config.mixRate.SampleRate() / 2;
        for (std::size_t i = 0; i < facts.size(); ++i) {
            const auto& fact = facts[i];
            if (fact.blockSequence != block || !fact.rendered || !fact.localRecoveryReady ||
                fact.inputActive || fact.detectorActive || fact.physicalVoice || fact.quietFrames < hold)
                roots.buses.push_back(current->working[i].bus);
        }
        std::vector<std::uint32_t> formalDefinitions;
        if (current->signals && topology.signals) {
            std::vector<std::uint8_t> formal(current->signals->NodeCount());
            for (const auto& relation : current->signals->Relations())
                formal[relation.source] = formal[relation.target] = 1;
            for (std::uint32_t i = 0; i < current->signals->NodeCount(); ++i) {
                const auto node = current->signals->NodeDefinition(i);
                if (!formal[i] && !node.receiver) continue;
                const bool ready = signalFacts && signals.blockSequence == block && i < signals.nodes.size() &&
                    signals.nodes[i].ready && signals.nodes[i].quietFrames >= hold;
                for (std::uint32_t j = 0; j < topology.signals->NodeCount(); ++j) {
                    const auto other = topology.signals->NodeDefinition(j);
                    if (node.kind == other.kind && node.stableId == other.stableId &&
                        node.busId == other.busId && node.busGeneration == other.busGeneration) {
                        formalDefinitions.push_back(j);
                        if (!ready) roots.signals.push_back(j);
                        break;
                    }
                }
            }
        }
        std::vector<BusActivityDetectorDependency> detectors;
        engine.dynamicRoutes.ForEachDefinition([&](const DynamicRouteDefinition& route) {
            if (route.targetBusIndex >= topology.slotCount || !topology.alive[route.targetBusIndex]) return;
            const BusSlotRef target{route.targetBusIndex, topology.generations[route.targetBusIndex]};
            if (route.sourceKind == VN_DYNAMIC_SOURCE_BUS && route.sourceIndex < topology.slotCount &&
                topology.alive[route.sourceIndex] && topology.generations[route.sourceIndex] == route.sourceGeneration)
                detectors.push_back({target, {route.sourceIndex, route.sourceGeneration}});
            else if (route.sourceKind == VN_DYNAMIC_SOURCE_VOICE &&
                engine.voices.IsAlive((static_cast<std::uint64_t>(route.sourceGeneration) << 32) | route.sourceIndex))
                roots.buses.push_back(target);
        });
        BusActivitySelection selected;
        if (!PrepareBusActivitySelection(topology, topology.signals, roots.buses, roots.signals,
                detectors, roots.members, selected)) return;
        // Missing owners need an unconditional wake, never a mixed sleep/wake.
        const bool addsBus = std::any_of(selected.buses.begin(), selected.buses.end(), [&](auto bus) {
            return !engine.buses.AcceptedWorkRef({bus, topology.generations[bus]}).Valid();
        });
        const bool addsSignal = topology.signals && std::any_of(selected.signalNodes.begin(),
            selected.signalNodes.end(), [&](auto node) {
                return !current->signals || !current->signals->ContainsNodeDefinition(*topology.signals, node);
            });
        if (addsBus || addsSignal) {
            PreparedBusCommandBatch wake(engine);
            if (wake.PrepareWake()) wake.Publish();
            return;
        }
        const bool removesSignal = std::any_of(formalDefinitions.begin(), formalDefinitions.end(), [&](auto node) {
            return std::find(selected.signalNodes.begin(), selected.signalNodes.end(), node) == selected.signalNodes.end();
        });
        if (selected.buses == current->order && !removesSignal) return;
        std::vector<BusTree::WorkObservation> sleeping;
        for (std::size_t i = 0; i < facts.size(); ++i)
            if (std::find(selected.buses.begin(), selected.buses.end(), current->working[i].bus.index) == selected.buses.end())
                sleeping.push_back(facts[i]);
        CommandRecord command{};
        command.opcode = kOpcodeSetBusActivity;
        if (engine.commands.ReserveBatch(1, &reservation) &&
            PrepareTopologyActivity(engine, topology, reservation, {&command, 1}, proof,
                &selected, false, {}, true, sleeping)) {
            const auto published = engine.commands.ConsumePreparedBatch(std::move(proof));
            assert(published.Count() == 1);
            (void)published;
        }
    } catch (...) {
        // Optional reclamation never changes an accepted operation's result.
    }
    proof.Release();
    engine.commands.CancelReservedBatch(&reservation);
    engine.buses.DrainActivities();
}

PreparedBusCommandBatch::~PreparedBusCommandBatch() {
    proof_.Release();
    engine_.commands.CancelReservedBatch(&reservation_);
    engine_.buses.DrainActivities();
}

bool PreparedBusCommandBatch::Prepare(std::span<const CommandRecord> commands) noexcept {
    if (proof_.Valid() || commands.size() > UINT32_MAX) return false;
    commands_ = commands;
    if (commands.empty()) return true;
    const auto filters = std::count_if(commands.begin(), commands.end(),
        [](const auto& command) { return command.opcode == kOpcodeSetBusFilter; });
    if (filters != 0 && !engine_.busFilterSettings.Prepare(static_cast<std::size_t>(filters))) return false;
    return engine_.commands.ReserveBatch(static_cast<std::uint32_t>(commands.size()), &reservation_) &&
        engine_.commands.ProvePreparedBatch(&reservation_, commands.data(),
            static_cast<std::uint32_t>(commands.size()), &proof_) &&
        PrepareBusActivityWake(engine_, reservation_, commands, proof_);
}

void PreparedBusCommandBatch::Publish() noexcept {
    if (commands_.empty()) return;
    const auto published = engine_.commands.ConsumePreparedBatch(std::move(proof_));
    assert(published.Count() == commands_.size());
    (void)published;
    for (const auto& command : commands_) engine_.NoteBusSettingCommand(command);
    commands_ = {};
}

PrivateSignalTopology::~PrivateSignalTopology() {
    proof_.Release();
    engine_.buses.DrainActivities();
    engine_.commands.CancelReservedBatch(&reservation_);
    if (index_ != UINT32_MAX) engine_.topologyPlans.Recycle(index_);
}
bool PrivateSignalTopology::Prepare(const PreparedSignalPublication& signals) noexcept {
    try {
    if (!signals.changed) return true;
    if (index_ != UINT32_MAX || !CanCommitSignals(engine_, signals) ||
        engine_.acceptedBusTopologyRevision == UINT64_MAX) return false;
    engine_.topologyPlans.DrainRetire();
    index_ = engine_.topologyPlans.AcquireSparse(engine_.busSendActive);
    if (index_ == UINT32_MAX) return false;
    ClearPairedInstallAckIdentity(engine_, index_);
    FillLiveSignalTopology(engine_, index_);
    AttachPreparedSignals(engine_.topologyPlans, index_, signals);
    if (!AdmitSingleLiveCommand(engine_) || !engine_.commands.ReserveBatch(1, &reservation_)) return false;
    CommandRecord command{};
    command.opcode = kOpcodeSetBusTopologyPlan;
    command.payload.setBusTopologyPlan.planIndex = index_;
    command.payload.setBusTopologyPlan.dynamicRoutePlanIndex = kInvalidDynamicRoutePlanIndex;
    command.payload.setBusTopologyPlan.connectFrom = kInvalidBusIndex;
    command.payload.setBusTopologyPlan.connectTo = kInvalidBusIndex;
    acceptedRevision_ = engine_.acceptedBusTopologyRevision + 1;
    command.payload.setBusTopologyPlan.acceptedGraphRevision = acceptedRevision_;
    command.payload.setBusTopologyPlan.batchCommandCount = 1;
    return PrepareTopologyActivity(engine_, engine_.topologyPlans.View(index_), reservation_, {&command, 1}, proof_);
    } catch (...) { return false; }
}
void PrivateSignalTopology::Commit() noexcept {
    if (index_ == UINT32_MAX) return;
    assert(proof_.Valid() && acceptedRevision_ == engine_.acceptedBusTopologyRevision + 1);
    engine_.acceptedBusTopologyRevision = acceptedRevision_;
    const auto published = engine_.commands.ConsumePreparedBatch(std::move(proof_));
    assert(published.Count() == 1);
    (void)published;
    ++engine_.topologyPlanCount;
    index_ = UINT32_MAX;
}
}
