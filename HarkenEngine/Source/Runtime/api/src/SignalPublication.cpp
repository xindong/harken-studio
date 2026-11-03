#include "SignalPublication.h"
#include "Engine.h"
#include "HarkenApiInternal.h"

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
            std::vector<SignalBusInput> buses(engine.config.maxBuses);
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
                    engine.acceptedSignalPlan.get(), rampFrames) || !engine.mixer.PrepareSignalBacking(*owner))
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
    engine.mixer.CommitSignalBacking(engine.acceptedSignalPlan.get());
}

PrivateSignalTopology::~PrivateSignalTopology() {
    engine_.commands.CancelReservedBatch(&reservation_);
    if (index_ != UINT32_MAX) engine_.topologyPlans.Recycle(index_);
}
bool PrivateSignalTopology::Prepare(const PreparedSignalPublication& signals) noexcept {
    if (!signals.changed) return true;
    if (index_ != UINT32_MAX || !CanCommitSignals(engine_, signals) ||
        engine_.acceptedBusTopologyRevision == UINT64_MAX) return false;
    engine_.topologyPlans.DrainRetire();
    index_ = engine_.topologyPlans.Acquire();
    if (index_ == UINT32_MAX) return false;
    ClearPairedInstallAckIdentity(engine_, index_);
    FillLiveSignalTopology(engine_, index_);
    AttachPreparedSignals(engine_.topologyPlans, index_, signals);
    return AdmitSingleLiveCommand(engine_) && engine_.commands.ReserveBatch(1, &reservation_);
}
void PrivateSignalTopology::Commit() noexcept {
    if (index_ == UINT32_MAX) return;
    CommandRecord command{};
    command.opcode = kOpcodeSetBusTopologyPlan;
    command.payload.setBusTopologyPlan.planIndex = index_;
    command.payload.setBusTopologyPlan.dynamicRoutePlanIndex = kInvalidDynamicRoutePlanIndex;
    command.payload.setBusTopologyPlan.connectFrom = kInvalidBusIndex;
    command.payload.setBusTopologyPlan.connectTo = kInvalidBusIndex;
    command.payload.setBusTopologyPlan.acceptedGraphRevision = ++engine_.acceptedBusTopologyRevision;
    command.payload.setBusTopologyPlan.batchCommandCount = 1;
    engine_.commands.CommitPreparedBatch(&reservation_, &command);
    ++engine_.topologyPlanCount;
    index_ = UINT32_MAX;
}
}
