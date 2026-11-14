#ifndef HARKEN_RENDER_SIGNAL_WORLD_PLAN_H
#define HARKEN_RENDER_SIGNAL_WORLD_PLAN_H

#include "harken/render/SignalRenderPlan.h"
#include <tuple>

namespace harken {

// Borrowed, already validated formal definitions from one direct world or a
// prepared Pack composition. The compiler copies proofs and retains no views.
struct SignalPropertyInput {
    const PlayablePropertyProgramViewV1* property = nullptr;
    ProgramSourceIdentityV1 source{};
};
struct SignalBusInput {
    VnBusId id{};
    std::uint32_t generation = 0;
    std::uint32_t stableIndex = UINT32_MAX;
};
struct SignalRetainedDefinitionInput {
    SignalEndpointKindV1 kind = SignalEndpointKindV1::Sound;
    VnObjectId stableId{};
    std::uint64_t propertyFingerprint = 0;
    ProgramSourceIdentityV1 source{};
};

inline std::unique_ptr<SignalRenderPlan> PrepareSignalWorldPlan(
    const SignalBlockFormat& format, std::span<const SignalRelationV1> relations,
    std::span<const SignalReceiverV1> receivers,
    std::span<const SignalPropertyInput> properties,
    std::span<const SignalBusInput> buses,
    std::span<const SignalRetainedDefinitionInput> retained = {}) noexcept {
    try {
        if (buses.size() > UINT32_MAX) return {};
        std::vector<SignalEndpointRefV1> endpoints;
        endpoints.reserve(relations.size() * 2 + receivers.size());
        for (const auto& relation : relations) {
            endpoints.push_back(relation.source);
            endpoints.push_back(relation.target);
        }
        for (const auto& receiver : receivers) endpoints.push_back(receiver.target);
        const auto key = [](const SignalEndpointRefV1& ref) {
            return std::pair(static_cast<std::uint32_t>(ref.kind), ref.stableId);
        };
        std::sort(endpoints.begin(), endpoints.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
        for (std::size_t i = 1; i < endpoints.size(); ++i)
            if (key(endpoints[i - 1]) == key(endpoints[i]) && endpoints[i - 1] != endpoints[i]) return {};
        endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
        std::vector<SignalRenderNodeInput> nodes(buses.size());
        for (std::uint32_t i = 0; i < buses.size(); ++i) {
            nodes[i].busIndex = buses[i].stableIndex == UINT32_MAX ? i : buses[i].stableIndex;
            nodes[i].busId = buses[i].id;
            nodes[i].busGeneration = buses[i].generation;
            nodes[i].stableId = buses[i].id;
        }
        std::vector<std::uint32_t> indices;
        indices.reserve(endpoints.size());
        for (const auto& endpoint : endpoints) {
            if (endpoint.kind == SignalEndpointKindV1::Bus) {
                auto it = std::find_if(buses.begin(), buses.end(), [&](const auto& bus) { return bus.id == endpoint.stableId; });
                if (it == buses.end() || it->id == VnObjectId{}) return {};
                indices.push_back(static_cast<std::uint32_t>(it - buses.begin()));
            } else {
                if (nodes.size() >= UINT32_MAX || (endpoint.kind != SignalEndpointKindV1::Sound &&
                    endpoint.kind != SignalEndpointKindV1::Container)) return {};
                indices.push_back(static_cast<std::uint32_t>(nodes.size()));
                nodes.push_back({endpoint.kind});
                nodes.back().stableId = endpoint.stableId;
            }
        }
        const auto resolve = [&](const SignalEndpointRefV1& ref) {
            const auto it = std::lower_bound(endpoints.begin(), endpoints.end(), ref,
                [&](const auto& a, const auto& b) { return key(a) < key(b); });
            return it != endpoints.end() && key(*it) == key(ref)
                ? indices[static_cast<std::size_t>(it - endpoints.begin())] : UINT32_MAX;
        };
        for (const auto& receiver : receivers) {
            const auto index = resolve(receiver.target);
            if (index == UINT32_MAX || nodes[index].receiver || receiver.algorithm != 1) return {};
            nodes[index].receiver = true;
            nodes[index].attackMs = receiver.attackMs;
            nodes[index].releaseMs = receiver.releaseMs;
        }
        std::vector<SignalMemberProof> proofs;
        std::vector<SignalFeedbackEdge> paths;
        const auto member = [&](playableproperties::ContentPropertyKindV1 kind,
                                VnObjectId id, std::uint64_t fingerprint,
                                const ProgramSourceIdentityV1& source) {
            const auto endpointKind = kind == playableproperties::ContentPropertyKindV1::Sound
                ? SignalEndpointKindV1::Sound : SignalEndpointKindV1::Container;
            if (kind != playableproperties::ContentPropertyKindV1::Sound &&
                kind != playableproperties::ContentPropertyKindV1::Container) return UINT32_MAX;
            const SignalEndpointRefV1 ref{endpointKind, 0, id, fingerprint, 0};
            const auto index = resolve(ref);
            if (index == UINT32_MAX) return index;
            const auto endpoint = std::lower_bound(endpoints.begin(), endpoints.end(), ref,
                [&](const auto& a, const auto& b) { return key(a) < key(b); });
            if (endpoint->expectedPropertyFingerprint != fingerprint) return UINT32_MAX;
            proofs.push_back({id, fingerprint, source, index});
            return index;
        };
        for (const auto& input : properties) {
            if (input.property == nullptr) return {};
            const auto& property = *input.property;
            const auto leaf = member(property.contentPropertyKind, property.stableId,
                property.programSemanticFingerprint, input.source);
            std::vector<std::uint32_t> ancestry;
            for (std::uint32_t i = 0; i < property.structuralAncestorCount; ++i) {
                const auto& ancestor = property.structuralAncestors[i];
                const auto node = member(ancestor.contentPropertyKind, ancestor.stableId,
                    ancestor.programSemanticFingerprint, input.source);
                if (node != UINT32_MAX) ancestry.push_back(node);
            }
            // Structural snapshots are root-to-leaf. The local endpoint is
            // deeper than every retained ancestor; each shared stage runs once.
            if (leaf != UINT32_MAX) ancestry.push_back(leaf);
            for (std::size_t i = 1; i < ancestry.size(); ++i)
                if (ancestry[i] != ancestry[i - 1]) paths.push_back({ancestry[i], ancestry[i - 1], 1, false});
        }
        for (const auto& definition : retained) {
            const auto kind = definition.kind == SignalEndpointKindV1::Sound
                ? playableproperties::ContentPropertyKindV1::Sound : playableproperties::ContentPropertyKindV1::Container;
            member(kind, definition.stableId, definition.propertyFingerprint, definition.source);
        }
        for (std::size_t i = 0; i < endpoints.size(); ++i)
            if (endpoints[i].kind != SignalEndpointKindV1::Bus &&
                std::none_of(proofs.begin(), proofs.end(), [&](const auto& proof) { return proof.node == indices[i]; })) return {};
        // Equivalent carrier proofs may repeat; neither a source nor a member
        // becomes duplicated by that repetition.
        const auto proofKey = [](const SignalMemberProof& proof) {
            return std::tuple(proof.node, proof.propertyFingerprint, proof.source.ownerStableId,
                proof.source.ownerRevision, proof.source.ownerStableId == 0 ? proof.source.worldGeneration : 0,
                proof.source.sourceBodyVintage);
        };
        std::sort(proofs.begin(), proofs.end(), [&](const auto& a, const auto& b) { return proofKey(a) < proofKey(b); });
        proofs.erase(std::unique(proofs.begin(), proofs.end(), [&](const auto& a, const auto& b) {
            return proofKey(a) == proofKey(b);
        }), proofs.end());
        std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) {
            return std::pair(a.source, a.target) < std::pair(b.source, b.target);
        });
        paths.erase(std::unique(paths.begin(), paths.end(), [](const auto& a, const auto& b) {
            return a.source == b.source && a.target == b.target;
        }), paths.end());
        std::vector<SignalConnectionInput> connections;
        connections.reserve(relations.size());
        for (const auto& relation : relations) {
            float gain = 0;
            const bool validGain = TryVolumeGainFromDb(relation.sendGainDb, &gain);
            const auto source = resolve(relation.source), target = resolve(relation.target);
            if (!validGain || source == UINT32_MAX || target == UINT32_MAX ||
                relation.consumer != SignalConsumerKindV1::Ducking || !nodes[target].receiver) return {};
            connections.push_back({source, target, gain, relation.stableId});
            paths.push_back({source, target, gain, true});
        }
        auto result = SignalRenderPlan::Prepare(format, nodes, connections, paths, proofs);
        if (result) result->EnableActualRouting();
        return result;
    } catch (...) { return {}; }
}

} // namespace harken
#endif
