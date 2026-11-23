#include "ProjectGenerationTransaction.h"

#include "Engine.h"
#include "BusBindingSourceStamp.h"
#include "MonitorCaptureHub.h"
#include "TraceRecorder.h"
#include "HarkenApiInternal.h"

#include "harken/core/CommandQueue.h"
#include "harken/BusDefinitionFingerprintValue.h"
#include "harken/core/MonitorLifecycleSink.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/media/CurrentWorldBodyVintage.h"
#include "harken/render/BusTree.h"
#include "harken/render/DynamicRoute.h"
#include "harken/render/TopologyPlan.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <map>
#include <new>
#include <vector>
#include <cstring>
#include <set>

namespace harken {

ProjectGenerationPrepareStatusV1 ValidatePreviewBusEffects(
    std::span<const VnBusId> buses, std::span<const std::uint32_t> faders,
    std::span<const PreviewBusEffect> effects, std::span<const PreviewEffectParameter> parameters,
    std::uint32_t slots, std::uint32_t sampleRate) noexcept {
    using Status = ProjectGenerationPrepareStatusV1;
    if (faders.size() != buses.size() || effects.size() > buses.size() * std::size_t{8} ||
        parameters.size() > effects.size() * std::size_t{2}) return Status::InvalidView;
    for (const auto position : faders)
        if (position != 8 || position > slots) return Status::CapabilityUnavailable;
    std::size_t cursor = 0, priorBus = 0;
    std::uint32_t priorSlot = 0;
    bool first = true;
    for (std::size_t i = 0; i < effects.size(); ++i) {
        const auto& row = effects[i];
        const auto found = std::find(buses.begin(), buses.end(), row.busId);
        const auto bus = static_cast<std::size_t>(found - buses.begin());
        if (found == buses.end() || IsNull(row.effectId) || row.slot >= slots ||
            (row.flags & ~1u) != 0 || row.parameterFirst != cursor ||
            row.parameterCount > parameters.size() - cursor ||
            (!first && (bus < priorBus || (bus == priorBus && row.slot <= priorSlot))))
            return Status::InvalidView;
        for (std::size_t j = 0; j < i; ++j)
            if (effects[j].effectId == row.effectId) return Status::InvalidView;
        if (row.effectType != VN_EFFECT_LIMITER ||
            row.descriptorVintage != FindEffectRow(VN_EFFECT_LIMITER)->descriptorVintage)
            return Status::CapabilityUnavailable;
        if (row.parameterCount != 2) return Status::InvalidView;
        for (std::uint32_t parameter = 0; parameter < 2; ++parameter) {
            float prepared = 0;
            if (parameters[cursor + parameter].index != parameter ||
                ValidateEffectParam(VN_EFFECT_LIMITER, parameter,
                    parameters[cursor + parameter].value, sampleRate) != VN_OK ||
                !PrepareLimiterParam(parameter, parameters[cursor + parameter].value, sampleRate, &prepared))
                return Status::InvalidView;
        }
        cursor += row.parameterCount;
        priorBus = bus;
        priorSlot = row.slot;
        first = false;
    }
    return cursor == parameters.size() ? Status::Ok : Status::InvalidView;
}

namespace {

bool ProjectCheckpoint(detail::LogicAllocSite site) noexcept {
    try {
        detail::LogicAllocCheckpoint(site);
        return true;
    } catch (const std::bad_alloc&) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ProjectAdmissionSnapshot);
        return false;
    }
}

BusAdmissionClaimKeyV1 MakeStudioKey(
    const RuntimeProjectGenerationTokenV1& token,
    std::uint64_t compileTargetProfileFingerprint) noexcept {
    BusAdmissionClaimKeyV1 key{};
    key.tag = kBusAdmissionTagStudioCurrentWorld;
    key.fields[0] = token.projectSessionId;
    key.fields[1] = token.authoringRevision;
    key.fields[2] = token.runtimeEpoch;
    key.fields[3] = token.submissionSerial;
    key.fields[4] = compileTargetProfileFingerprint;
    return key;
}

ProjectGenerationPrepareStatusV1 MapWorldPrepareStatus(
    WorldInstallStatus status) noexcept {
    switch (status) {
        case WorldInstallStatus::Ok:
            return ProjectGenerationPrepareStatusV1::Ok;
        case WorldInstallStatus::Capacity:
            return ProjectGenerationPrepareStatusV1::Capacity;
        case WorldInstallStatus::CapabilityUnavailable:
            return ProjectGenerationPrepareStatusV1::CapabilityUnavailable;
        case WorldInstallStatus::GenerationExhausted:
            return ProjectGenerationPrepareStatusV1::DeviceError;
        case WorldInstallStatus::Backpressure:
            return ProjectGenerationPrepareStatusV1::Backpressure;
        case WorldInstallStatus::InvalidFormat:
            return ProjectGenerationPrepareStatusV1::InvalidView;
        case WorldInstallStatus::InvalidWorld:
        default:
            return ProjectGenerationPrepareStatusV1::InvalidWorld;
    }
}

bool CurrentWorldProtectedStateBlocked(const Engine& engine) noexcept {
    return engine.variables.DrainingTransport() ||
           engine.variables.DrainingWorld() ||
           engine.variables.HasUnreclaimedLiveWorldRows();
}

bool CurrentWorldCommitBlocked(const Engine& engine, std::uint32_t vintage,
    const VariableServiceV1::PreparedCurrentWorldImageV1& image) noexcept {
    const bool liveBusUpdate = (vintage == 3 || vintage == 4 || vintage == 5) &&
        image.Valid() && image.PreservesContent();
    return CurrentWorldProtectedStateBlocked(engine) ||
        (HasCurrentWorldPublicationConsumers(engine) && !liveBusUpdate);
}

} // namespace

bool SameOwnerRow(const BusTopologyOwnerRecord& a, const BusTopologyOwnerRecord& b) noexcept {
    return a.live == b.live && a.ownerId == b.ownerId && a.currentRevision == b.currentRevision &&
        a.predecessorRevision == b.predecessorRevision && a.digestLow == b.digestLow && a.digestHigh == b.digestHigh;
}

bool SameHolderRow(const PreviewMasterHolderV1& a, const PreviewMasterHolderV1& b) noexcept {
    return a.occupied == b.occupied && a.ownerId == b.ownerId && a.revision == b.revision && a.fingerprint == b.fingerprint;
}

namespace {

void ReleasePreparedSlot(Engine& engine, std::uint32_t planIndex,
                         std::uint32_t ownerSlot) noexcept {
    if (planIndex != UINT32_MAX) {
        engine.preparedTopologyPlans.Recycle(planIndex);
    }
    if (ownerSlot < engine.preparedOwnerClaims.size()) {
        auto& claim = engine.preparedOwnerClaims[ownerSlot];
        if (claim.preparedSerial != UINT64_MAX) ++claim.preparedSerial;
        claim.inUse = 0;
        claim.zero = 0;
        claim.ownerId = 0;
        claim.baseRevision = 0;
        claim.revision = 0;
        claim.digestLow = 0;
        claim.digestHigh = 0;
        engine.preparedPreviewOwnerSlots[ownerSlot] = UINT32_MAX;
    }
}

} // namespace

bool BuildPreviewSlotPlan(Engine& engine, const ProspectiveTopologyView* incoming,
                          std::uint64_t ownerId, SlotPlanV1& out) {
    struct Edge {
        VnBusId target{};
        std::uint32_t slot = 0;
        float db = 0.0f;
        float linear = 1.0f;
        std::uint8_t apply = 0;
        std::uint32_t rampMs = 0;
        std::uint64_t owner = 0;
        VnPackHandle cookie = 0;
        std::uint64_t generation = 0;
    };
    struct Node { VnBusId parent{}; bool owned = false; std::vector<Edge> edges; };
    std::uint64_t additions = 0;
    if (incoming != nullptr && incoming->busIds != nullptr)
        for (std::uint32_t i = 0; i < incoming->busCount; ++i)
            if ((incoming->alive == nullptr || incoming->alive[i]) &&
                engine.buses.FindIndex(incoming->busIds[i]) == kInvalidBusIndex) ++additions;
    if (!engine.PrepareAdditionalBusDefinitions(additions)) return false;
    const auto capacity = engine.BusCapacity();
    const auto sendSlots = engine.config.busSendSlotsPerBus;
    const auto oldCount = engine.buses.BusCount();
    const VnBusId masterId = VN_BUS_MASTER;
    const std::uint32_t masterParent = 0;
    const std::uint8_t masterAlive = 1;
    ProspectiveTopologyView masterOnly{};
    masterOnly.busCount = 1;
    masterOnly.busIds = &masterId;
    masterOnly.parents = &masterParent;
    masterOnly.alive = &masterAlive;
    if (incoming == nullptr || incoming->busCount == 0) incoming = &masterOnly;
    if (incoming->busCount > capacity || incoming->busIds == nullptr ||
        incoming->alive == nullptr || incoming->parents == nullptr) return false;
    std::map<VnBusId, Node> graph;
    for (std::uint32_t i = 0; i < oldCount; ++i) {
        if (!engine.buses.AliveAt(i)) continue;
        if (i != 0 && engine.busTopologyOwnerIds[i] == ownerId) {
            if (engine.busOwnerCookies[i] != 0) return false;
            continue;
        }
        graph.emplace(engine.buses.IdAt(i), Node{
            i == 0 ? VnBusId{} : engine.buses.IdAt(engine.buses.ParentOf(i)), false, {}});
    }
    bool sawMaster = false;
    for (std::uint32_t i = 0; i < incoming->busCount; ++i) {
        if (!incoming->alive[i]) continue;
        const auto id = incoming->busIds[i];
        if (id == VN_BUS_MASTER) {
            if (sawMaster) return false;
            sawMaster = true;
            continue;
        }
        const auto parent = incoming->parents[i];
        if (IsNull(id) || parent >= incoming->busCount || !incoming->alive[parent] ||
            !graph.emplace(id, Node{incoming->busIds[parent], true, {}}).second) return false;
    }
    if (!sawMaster || graph.size() > capacity || !graph.contains(VN_BUS_MASTER)) return false;
    for (const auto& [id, node] : graph) {
        if (id != VN_BUS_MASTER && !graph.contains(node.parent)) return false;
    }
    for (const auto& edge : engine.busSends.Records()) {
        if (edge.topologyOwnerId == ownerId) continue;
        if (edge.source >= oldCount || edge.target >= oldCount ||
            !engine.buses.AliveAt(edge.source) || !engine.buses.AliveAt(edge.target)) return false;
        const auto fromId = engine.buses.IdAt(edge.source);
        const auto toId = engine.buses.IdAt(edge.target);
        if (!graph.contains(fromId) || !graph.contains(toId)) return false;
        float linear = 0;
        if (!TryVolumeGainFromDb(edge.levelDb, &linear)) return false;
        graph.at(fromId).edges.push_back({toId, edge.slot, edge.levelDb, linear, 0, 0,
            edge.topologyOwnerId, edge.ownerCookie, edge.generation});
    }
    bool edgesValid = true;
    for (int retainedPass = 1; retainedPass >= 0; --retainedPass) {
    for (std::uint32_t from = 0; from < incoming->busCount; ++from) {
        if (!incoming->alive[from]) continue;
        auto& edges = graph.at(incoming->busIds[from]).edges;
        const auto oldFrom = engine.buses.FindIndex(incoming->busIds[from]);
        incoming->ForEachSend(from, sendSlots, [&](const BusSendRecord& input, auto) {
            if (!edgesValid) return;
            if (input.target >= incoming->busCount || !incoming->alive[input.target]) {
                edgesValid = false;
                return;
            }
            const auto targetId = incoming->busIds[input.target];
            if (!graph.contains(targetId)) {
                edgesValid = false;
                return;
            }
            const auto oldTarget = engine.buses.FindIndex(targetId);
            const auto* prior = engine.busSends.Find(oldFrom, oldTarget);
            const bool retained = prior != nullptr && prior->topologyOwnerId == ownerId &&
                prior->ownerCookie == 0;
            if (retained != (retainedPass != 0)) return;
            if (std::any_of(edges.begin(), edges.end(), [&](const Edge& edge) { return edge.target == targetId; })) {
                edgesValid = false; return;
            }
            std::uint32_t chosen = retained ? prior->slot : 0;
            if (!retained) {
                while (std::any_of(edges.begin(), edges.end(), [&](const auto& edge) { return edge.slot == chosen; }))
                    ++chosen;
            }
            float db = retained ? prior->levelDb : 0;
            float linear = 1;
            if (input.applyLevel) {
                linear = input.preparedGain;
                if (!std::isfinite(linear) || linear < 0) { edgesValid = false; return; }
                db = incoming->sends != nullptr || incoming->sendLevelsDb != nullptr ? input.levelDb
                    : (linear == 0 ? -160.0f : 20.0f * std::log10(linear));
                if (!std::isfinite(db) || (incoming->sendLevelsDb != nullptr && (db < -96 || db > 24))) {
                    edgesValid = false;
                    return;
                }
            } else if (!TryVolumeGainFromDb(db, &linear)) { edgesValid = false; return; }
            const bool changed = !retained || (input.applyLevel && db != prior->levelDb);
            edges.push_back({targetId, chosen, db, linear, static_cast<std::uint8_t>(changed),
                static_cast<std::uint32_t>(retained && changed ? 20 : 0), ownerId, 0,
                retained ? prior->generation : engine.NewBusSendGeneration()});
        });
    }
    }
    if (!edgesValid) return false;
    std::vector<VnBusId> ids;
    std::map<VnBusId, std::uint32_t> index;
    for (const auto& [id, node] : graph) {
        index.emplace(id, static_cast<std::uint32_t>(ids.size()));
        ids.push_back(id);
    }
    std::vector<std::uint32_t> parents(ids.size());
    std::vector<std::uint8_t> alive(ids.size(), 1);
    for (const auto& [id, node] : graph) parents[index.at(id)] = id == VN_BUS_MASTER ? index.at(id) : index.at(node.parent);
    ProspectiveTopologyView merged{};
    merged.busCount = static_cast<std::uint32_t>(ids.size());
    merged.busIds = ids.data();
    merged.parents = parents.data();
    merged.alive = alive.data();
    if (!BuildSlotPlanFromProspective(engine, &merged, ownerId, out)) return false;
    out.preservesForeign = true;
    out.sourceGraphRevision = engine.acceptedBusTopologyRevision;
    out.sourceBusCount = oldCount;
    out.busCount = std::max(out.busCount, oldCount);
    out.busOwnerIds = engine.busTopologyOwnerIds;
    out.busCookies = engine.busOwnerCookies;
    out.neutralSlots.assign(capacity, 0);
    out.neutralSlots[0] = static_cast<std::uint8_t>(PreviewOwnsSoleMasterV1(engine, ownerId));
    out.migrationTargets.resize(capacity);
    for (std::uint32_t slot = 0; slot < capacity; ++slot) {
        out.migrationTargets[slot] = slot;
        if (slot < oldCount && out.alive[slot] == 0) out.generations[slot] = engine.buses.GenerationAt(slot);
        if (slot != 0 && slot < oldCount && engine.buses.AliveAt(slot) && !out.alive[slot]) {
            out.removed.push_back({engine.buses.IdAt(slot), slot, engine.buses.GenerationAt(slot)});
            auto parent = engine.buses.ParentOf(slot);
            std::uint32_t depth = 0;
            while (parent != 0 && parent < oldCount && !out.alive[parent] && depth++ < oldCount)
                parent = engine.buses.ParentOf(parent);
            out.migrationTargets[slot] = parent < capacity && out.alive[parent] ? parent : 0;
        }
    }
    for (const auto& [id, node] : graph) {
        const auto slot = FindAssignedSlot(out, id);
        if (node.owned) {
            out.busOwnerIds[slot] = ownerId;
            out.busCookies[slot] = 0;
            out.neutralSlots[slot] = 1;
        }
        for (const auto& edge : node.edges) {
            BusSendRecord record;
            record.source = slot;
            record.target = FindAssignedSlot(out, edge.target);
            record.slot = edge.slot;
            record.levelDb = edge.db;
            record.ownerCookie = edge.cookie;
            record.topologyOwnerId = edge.owner;
            record.generation = edge.generation;
            record.preparedGain = edge.linear;
            record.rampMs = edge.rampMs;
            record.applyLevel = edge.apply != 0;
            out.sends.Add(record);
        }
    }
    return true;
}

struct AuthoredTraceProjectionV1 {
    std::vector<VnBusId> ids;
    std::vector<std::uint32_t> generations;
    std::vector<std::uint32_t> parents;
    std::vector<std::uint8_t> alive;
    std::vector<TopologySendRange> ranges;
    std::vector<std::uint32_t> slots;
    std::vector<std::uint32_t> targets;
    std::vector<float> levels;
    std::vector<std::uint8_t> apply;
    TopologyPlanView view{};
};

bool BuildAuthoredTraceProjection(const SlotPlanV1& plan,
                                  AuthoredTraceProjectionV1& out) {
    if (!plan.valid || !plan.preservesForeign || plan.busCount == 0 ||
        plan.busIds[0] != VN_BUS_MASTER || plan.alive[0] == 0) return false;
    detail::LogicAllocCheckpoint(detail::LogicAllocSite::ProjectTraceProjection);
    std::vector<std::uint32_t> slots{0};
    for (const auto& assignment : plan.assignments) {
        if (assignment.slot != 0 && plan.alive[assignment.slot] != 0 &&
            plan.busOwnerIds[assignment.slot] == plan.ownerId) slots.push_back(assignment.slot);
    }
    std::sort(slots.begin() + 1, slots.end(), [&](auto a, auto b) { return plan.busIds[a] < plan.busIds[b]; });
    const auto count = static_cast<std::uint32_t>(slots.size());
    std::vector<std::uint32_t> dense(plan.busCount, UINT32_MAX);
    out.ids.resize(count);
    out.generations.resize(count);
    out.parents.assign(count, 0);
    out.alive.assign(count, 1);
    out.ranges.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        dense[slots[i]] = i;
        out.ids[i] = plan.busIds[slots[i]];
        out.generations[i] = plan.generations[slots[i]];
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto slot = slots[i];
        const auto parent = plan.parents[slot];
        if (i != 0) {
            if (parent >= dense.size() || dense[parent] == UINT32_MAX) return false;
            out.parents[i] = dense[parent];
        }
        auto& range = out.ranges[i];
        range.first = static_cast<std::uint32_t>(out.targets.size());
        for (const auto& edge : plan.sends.From(slot)) {
            if (edge.topologyOwnerId != plan.ownerId) continue;
            if (edge.target >= dense.size() || dense[edge.target] == UINT32_MAX ||
                !std::isfinite(edge.preparedGain) || edge.preparedGain < 0) return false;
            out.slots.push_back(range.count++);
            out.targets.push_back(dense[edge.target]);
            out.levels.push_back(edge.preparedGain);
            out.apply.push_back(edge.applyLevel ? 1 : 0);
        }
    }
    out.view.slotCount = count;
    out.view.slotsPerBus = plan.sendSlotsPerBus;
    out.view.busIds = out.ids.data();
    out.view.generations = out.generations.data();
    out.view.parents = out.parents.data();
    out.view.alive = out.alive.data();
    out.view.sendRanges = out.ranges.data();
    out.view.sendSlots = out.slots.data();
    out.view.sendCount = static_cast<std::uint32_t>(out.targets.size());
    out.view.sendTargets = out.targets.data();
    out.view.preparedSendLevels = out.levels.data();
    out.view.sendLevelApply = out.apply.data();
    out.view.valid = true;
    return true;
}

bool HasPredecessorDirectWorldPublication(const Engine& engine) noexcept {
    const auto generation = engine.variables.CurrentWorldGeneration();
    for (std::uint32_t slot = 0; slot < engine.actionDeferredRecords.Capacity(); ++slot) {
        ActionDeferredRecordToken token{};
        ActionDeferredRecord record{};
        ActionDeferredRecordState state{};
        if (engine.actionDeferredRecords.CapturePublicationAt(slot, &token, &record, &state) &&
            record.owner.kind == ActionFiringOwnerKind::DirectWorld &&
            record.owner.worldGeneration == generation) return true;
    }
    return false;
}

namespace {

struct PreparedAdmissionWriteV1 {
    std::vector<Engine::CurrentWorldBusAdmissionRowV1> expected;
    std::uint32_t destination = UINT32_MAX;
    WorldGeneration sourceGeneration = 0;
    Engine::CurrentWorldBusAdmissionRowV1 replacement{};
};

bool PrepareAdmissionWrite(Engine& engine, WorldGeneration predicted,
                          const BusAdmissionClaimKeyV1& key,
                          PreparedAdmissionWriteV1& out) {
    if (predicted == 0 || key.tag != kBusAdmissionTagStudioCurrentWorld) return false;
    detail::LogicAllocCheckpoint(detail::LogicAllocSite::ProjectAdmissionSnapshot);
    out.expected = engine.currentWorldBusAdmission;
    out.sourceGeneration = engine.variables.CurrentWorldGeneration();
    for (std::uint32_t i = 0; i < out.expected.size(); ++i) {
        const auto& row = out.expected[i];
        if (!row.occupied || row.key.tag == kBusAdmissionTagStudioCurrentWorld) {
            out.destination = i;
            out.replacement.generation = predicted;
            out.replacement.key = key;
            out.replacement.state = BusAdmissionClaimState::Pending;
            out.replacement.occupied = true;
            return true;
        }
    }
    return false;
}

bool CanCommitAdmissionWrite(const Engine& engine,
                             const PreparedAdmissionWriteV1& prepared) noexcept {
    if (prepared.destination >= prepared.expected.size() ||
        engine.variables.CurrentWorldGeneration() != prepared.sourceGeneration ||
        prepared.expected.size() != engine.currentWorldBusAdmission.size()) return false;
    for (std::size_t i = 0; i < prepared.expected.size(); ++i) {
        const auto& a = prepared.expected[i];
        const auto& b = engine.currentWorldBusAdmission[i];
        if (a.occupied != b.occupied || a.generation != b.generation || a.state != b.state ||
            a.key.tag != b.key.tag || a.key.zero != b.key.zero) return false;
        for (std::size_t k = 0; k < 5; ++k) if (a.key.fields[k] != b.key.fields[k]) return false;
    }
    return true;
}

void CommitAdmissionWrite(Engine& engine, const PreparedAdmissionWriteV1& prepared) noexcept {
    for (std::size_t i = 0; i < prepared.expected.size(); ++i) {
        if (prepared.expected[i].occupied &&
            prepared.expected[i].key.tag == kBusAdmissionTagStudioCurrentWorld) {
            engine.currentWorldBusAdmission[i] = {};
        }
    }
    engine.currentWorldBusAdmission[prepared.destination] = prepared.replacement;
}

bool CanApplyPreviewProperties(const Engine& engine, const SlotPlanV1& plan,
                               std::uint32_t vintage = 1) noexcept {
    const bool volumeOnly = vintage >= 3;
    const auto effectOwner = vintage == 5 ? plan.ownerId : 0;
    for (std::uint32_t slot = 0; slot < plan.busCount; ++slot) {
        const bool removed = slot < engine.buses.BusCount() && engine.buses.AliveAt(slot) && !plan.alive[slot];
        if (removed && !CanResetNeutralBusPropertiesV1(engine, slot, plan.ownerId)) return false;
        if (plan.neutralSlots[slot] != 0 &&
            !(volumeOnly ? CanResetVolumeOnlyNonVolumePropertiesV2(engine, slot, 0, effectOwner)
                         : CanResetNeutralBusPropertiesV1(engine, slot))) return false;
    }
    for (const auto& pending : engine.logicPacks.pending) {
        for (const auto id : pending.prospectiveBusIds) {
            const auto oldSlot = engine.buses.FindIndex(id);
            const auto nextSlot = FindAssignedSlot(plan, id);
            if ((oldSlot < plan.busCount && !plan.alive[oldSlot]) ||
                (nextSlot < plan.busCount && plan.neutralSlots[nextSlot] != 0)) return false;
        }
    }
    return true;
}

} // namespace

bool PreparedProjectGenerationV1::Valid() const noexcept {
    const auto domain = domain_.lock();
    if (!occupied_ || domain == nullptr || !domain->live ||
        domain->epoch != domainEpoch_ || domain->liveEngine != engine_ ||
        engine_ == nullptr) {
        return false;
    }
    if (preparedOwnerSlot_ >= engine_->preparedOwnerClaims.size() ||
        engine_->preparedPreviewOwnerSlots[preparedOwnerSlot_] != liveOwnerSlot_ ||
        !engine_->preparedTopologyPlans.IsAcquired(preparedPlanIndex_)) {
        return false;
    }
    const auto& claim = engine_->preparedOwnerClaims[preparedOwnerSlot_];
    return claim.inUse != 0 && claim.zero == 0 && claim.preparedSerial == preparedSerial_ &&
        claim.ownerId == slotPlan_.ownerId && claim.baseRevision == nextOwner_.predecessorRevision &&
        claim.revision == nextOwner_.currentRevision && claim.digestLow == nextOwner_.digestLow &&
        claim.digestHigh == nextOwner_.digestHigh;
}

void PreparedProjectGenerationV1::Reset() noexcept {
    const auto domain = domain_.lock();
    if (domain && domain->live && domain->liveEngine == engine_)
        for (const auto& row : preparedWork_) engine_->buses.ReleaseWorkRow(row.work);
    if (Valid()) {
        for (auto& seed : effectSeeds_) engine_->effectParamTargets.CancelPreparedInstalledEffectSeed(&seed);
        for (auto& row : effectChains_) engine_->buses.CancelPreviewEffectChain(row);
        ReleasePreparedSlot(*engine_, preparedPlanIndex_, preparedOwnerSlot_);
    }
    ClearFields();
}

bool ProjectPreviewVolumeBindings(Engine& engine, const SlotPlanV1& slotPlan,
    const VariableServiceV1::PreparedWorldProjectionV1& world,
    const std::vector<float>& authoredMixDb,
    const std::vector<float>& authoredOutputDb,
    std::vector<float>& effectiveMixDb,
    std::vector<float>& effectiveOutputDb,
    std::vector<std::uint8_t>& projectedApply,
    std::uint32_t planIndex) {
    ParamSystem params = engine.params;
    params.ClearInstalledValues();
    bool globalsReady = true;
    world.ForEachGlobal([&](VnVariableId id, float value) {
        globalsReady &= params.SetInstalledValue(id, value) == VN_OK;
    });
    if (!globalsReady) return false;
    BindingTable bindings = engine.bindings;
    BusTargetStore targets = engine.busTargets;
    for (std::uint32_t slot = 0; slot < slotPlan.busCount; ++slot) {
        if (slotPlan.neutralSlots[slot] == 0) continue;
        const bool retained = slot < engine.buses.BusCount() &&
            engine.buses.AliveAt(slot) && engine.buses.IdAt(slot) == slotPlan.busIds[slot] &&
            engine.buses.GenerationAt(slot) == slotPlan.generations[slot];
        if (!retained)
            targets.InitializeBusGeneration(slot, slotPlan.generations[slot]);
        targets.SetSlot(slot, kBusTargetBusVolume,
            BusTargetStore::kSlotAuthoredIndex, authoredMixDb[slot]);
        targets.SetSlot(slot, kBusTargetBusOutputVolume,
            BusTargetStore::kSlotAuthoredIndex, authoredOutputDb[slot]);
    }
    bindings.EvaluateBusAttachments(params, targets);
    projectedApply.assign(slotPlan.busCount, 0);
    for (std::uint32_t slot = 0; slot < slotPlan.busCount; ++slot) {
        if (slotPlan.alive[slot] == 0 || slotPlan.neutralSlots[slot] == 0) continue;
        const float mix = targets.Reduce(slot, kBusTargetBusVolume);
        const float output = targets.Reduce(slot, kBusTargetBusOutputVolume);
        if (!TryVolumeGainFromDb(mix) || !TryVolumeGainFromDb(output)) return false;
        const std::uint8_t apply = kApplyBusVolume | kApplyBusOutputVolume;
        projectedApply[slot] = apply;
        effectiveMixDb[slot] = mix;
        effectiveOutputDb[slot] = output;
        if (apply == 0) continue;
        engine.preparedTopologyPlans.BusVolumeApplyData(planIndex)[slot] |= apply;
        engine.preparedTopologyPlans.PreparedBusVolumeDbData(planIndex)[slot] = mix;
        engine.preparedTopologyPlans.PreparedBusOutputVolumeDbData(planIndex)[slot] = output;
        engine.preparedTopologyPlans.PreparedBusVolumeRampMsData(planIndex)[slot] = 0;
    }
    return ValidateTopologyPlanVolumesV1(engine.preparedTopologyPlans.View(planIndex));
}

ProjectGenerationPrepareResultV1 PrepareProjectGenerationV1(
    Engine& engine, const ProjectGenerationInstallViewV1& view,
    PreparedProjectGenerationV1& out) noexcept {
    ProjectGenerationPrepareResultV1 result{};
    if (engine.canonicalPackSetBusy) {
        result.status = ProjectGenerationPrepareStatusV1::Backpressure;
        return result;
    }
    if (out.Valid()) {
        result.status = ProjectGenerationPrepareStatusV1::InvalidView;
        return result;
    }
    out.Reset();
    if (engine.projectGenerationReservationDomain == nullptr ||
        !engine.projectGenerationReservationDomain->live ||
        engine.projectGenerationReservationDomain->liveEngine != &engine) {
        result.status = ProjectGenerationPrepareStatusV1::DeviceError;
        return result;
    }
    const bool volumeOnly = view.publicationVintage == 3 ||
        view.publicationVintage == 4 || view.publicationVintage == 5;
    if (view.forceWorldReplacement && view.publicationVintage != 5) {
        result.status = ProjectGenerationPrepareStatusV1::InvalidView;
        return result;
    }
    const bool closeEffects = view.publicationVintage == 1 && view.prospective &&
        view.prospective->busCount == 1 && view.prospective->busIds &&
        view.prospective->busIds[0] == VN_BUS_MASTER;
    if ((view.publicationVintage != 1 && !volumeOnly) || view.zero != 0 ||
        !TokenIsPaired(view.token) || view.ownerId == 0) {
        result.status = ProjectGenerationPrepareStatusV1::InvalidView;
        return result;
    }
    if (volumeOnly && (view.prospective == nullptr || view.authoredBusIds == nullptr ||
        view.authoredBusVolumeDb == nullptr ||
        view.authoredBusOutputVolumeDb == nullptr ||
        view.authoredBusCount != view.prospective->busCount)) {
        result.status = ProjectGenerationPrepareStatusV1::InvalidView;
        return result;
    }
    if (CurrentWorldProtectedStateBlocked(engine) ||
        (HasPredecessorDirectWorldPublication(engine) &&
         !engine.variables.SignalOnlyWorld(view.compiledWorld))) {
        result.status = ProjectGenerationPrepareStatusV1::Backpressure;
        return result;
    }
    const auto destination = SelectPreviewOwnerDestination(engine, view.ownerId);
    if (destination >= engine.busTopologyOwners.size() || destination >= engine.previewMasterHolders.size()) {
        result.status = ProjectGenerationPrepareStatusV1::Capacity;
        return result;
    }
    const auto expectedOwner = engine.busTopologyOwners[destination];
    const auto expectedHolder = engine.previewMasterHolders[destination];
    const auto effectiveBase = expectedOwner.live && view.ownerRevision == expectedOwner.currentRevision &&
        view.ownerBaseRevision == expectedOwner.currentRevision
        ? expectedOwner.predecessorRevision : view.ownerBaseRevision;
    const auto transition = ClassifyTopologyOwnerRevisionV1(expectedOwner.live ? &expectedOwner : nullptr,
        effectiveBase, view.ownerRevision, view.publishedBusFingerprint, view.compileTargetProfileFingerprint);
    const auto masterFingerprint = NeutralMasterFingerprintV1(engine);
    const bool retryHolderMatches = expectedHolder.occupied && expectedHolder.ownerId == view.ownerId &&
        expectedHolder.revision == view.ownerRevision && expectedHolder.fingerprint == masterFingerprint;
    if (transition == TopologyOwnerRevisionDispositionV1::Conflict ||
        (transition == TopologyOwnerRevisionDispositionV1::Retry && !retryHolderMatches) ||
        !PreviewMasterCompatible(engine, masterFingerprint, view.ownerId)) {
        result.status = ProjectGenerationPrepareStatusV1::InvalidView;
        return result;
    }
    const auto commandHeadBefore = engine.commands.PublishedHead();
    const auto initialSourceHeadBefore = engine.commands.InitialSourcePublishedHead();
    const std::uint32_t liveInUse = engine.topologyPlans.InUse();
    const std::uint32_t planIndex = engine.preparedTopologyPlans.AcquireSparse();
    if (planIndex == kInvalidTopologyPlanIndex) {
        result.status = ProjectGenerationPrepareStatusV1::Backpressure;
        return result;
    }
    if (engine.topologyPlans.InUse() != liveInUse) {
        engine.preparedTopologyPlans.Recycle(planIndex);
        result.status = ProjectGenerationPrepareStatusV1::DeviceError;
        return result;
    }
    std::uint32_t ownerSlot = UINT32_MAX;
    for (std::uint32_t index = 0; index < engine.preparedOwnerClaims.size();
         ++index) {
        if (engine.preparedOwnerClaims[index].inUse == 0 &&
            engine.preparedOwnerClaims[index].preparedSerial != UINT64_MAX) {
            ownerSlot = index;
            break;
        }
    }
    if (ownerSlot == UINT32_MAX) {
        const auto next = engine.preparedOwnerClaims.size();
        if (!engine.PrepareBusOwnerCapacity(next + 1)) {
            engine.preparedTopologyPlans.Recycle(planIndex);
            result.status = ProjectGenerationPrepareStatusV1::Capacity;
            return result;
        }
        ownerSlot = static_cast<std::uint32_t>(next);
    }
    auto& owner = engine.preparedOwnerClaims[ownerSlot];
    owner.inUse = 1;
    owner.zero = 0;
    owner.ownerId = view.ownerId;
    owner.baseRevision = effectiveBase;
    owner.revision = view.ownerRevision;
    owner.digestLow = view.publishedBusFingerprint;
    owner.digestHigh = view.compileTargetProfileFingerprint;
    ++owner.preparedSerial;
    engine.preparedPreviewOwnerSlots[ownerSlot] = destination;

    SlotPlanV1 slotPlan;
    ProspectiveTopologyView remapped{};
    const ProspectiveTopologyView* fillView = view.prospective;
    VariableServiceV1::PreparedCurrentWorldImageV1 image;
    PreparedSignalPublication signals;
    WorldInstallStatus imageStatus = WorldInstallStatus::InvalidWorld;
    std::vector<float> authoredMixDb, authoredOutputDb, effectiveMixDb, effectiveOutputDb;
    std::vector<float> sourceMixDb, sourceOutputDb;
    std::vector<std::uint8_t> projectedVolumeApply;
    std::uint64_t bindingSourceStamp = 0;
    std::unique_ptr<PreparedBusEffectsV1> coldEffects;
    std::vector<std::uint8_t> coldSlots;
    std::vector<PreviewBusEffect> liveRows, coldRows;
    std::vector<PreviewEffectParameter> liveParameters, coldParameters;
    try {
        if (!BuildPreviewSlotPlan(engine, view.prospective, view.ownerId,
                                  slotPlan)) {
            ReleasePreparedSlot(engine, planIndex, ownerSlot);
            result.status = ProjectGenerationPrepareStatusV1::Capacity;
            return result;
        }
        engine.preparedTopologyPlans.PreparePlanBusCapacity(planIndex, engine.BusCapacity());
        coldSlots.assign(engine.BusCapacity(), 0);
        const auto isCold = [&](const SlotAssignmentV1& assignment) {
            if (engine.buses.LogicRefMatches({assignment.slot, assignment.generation}))
                for (std::uint32_t slot = 0; slot < engine.config.fxSlotsPerBus; ++slot) {
                    const auto type = engine.busEffects.Get(assignment.slot, slot).type;
                    if (type != kEffectTypeNone && type != VN_EFFECT_LIMITER) return true;
                }
            return std::any_of(view.effects.begin(), view.effects.end(), [&](const auto& effect) {
                return effect.busId == assignment.id && effect.effectType != VN_EFFECT_LIMITER;
            });
        };
        for (const auto& assignment : slotPlan.assignments)
            coldSlots[assignment.slot] = isCold(assignment);
        for (const auto& removed : slotPlan.removed)
            coldSlots[removed.slot] = isCold(removed);
        std::set<VnObjectId> effectIds;
        std::size_t parameterCursor = 0;
        for (const auto& source : view.effects) {
            const auto slot = FindAssignedSlot(slotPlan, source.busId);
            if (slot >= slotPlan.busCount || source.parameterFirst != parameterCursor ||
                source.parameterCount > view.effectParameters.size() - parameterCursor ||
                !effectIds.insert(source.effectId).second) {
                ReleasePreparedSlot(engine, planIndex, ownerSlot);
                result.status = ProjectGenerationPrepareStatusV1::InvalidView;
                return result;
            }
            auto& rows = coldSlots[slot] ? coldRows : liveRows;
            auto& parameters = coldSlots[slot] ? coldParameters : liveParameters;
            auto row = source;
            row.parameterFirst = static_cast<std::uint32_t>(parameters.size());
            rows.push_back(row);
            parameters.insert(parameters.end(), view.effectParameters.begin() + parameterCursor,
                view.effectParameters.begin() + parameterCursor + source.parameterCount);
            parameterCursor += source.parameterCount;
        }
        if (parameterCursor != view.effectParameters.size()) {
            ReleasePreparedSlot(engine, planIndex, ownerSlot);
            result.status = ProjectGenerationPrepareStatusV1::InvalidView;
            return result;
        }
        if (view.publicationVintage == 5) {
            const auto validation = ValidatePreviewBusEffects(
                {view.authoredBusIds, view.authoredBusCount}, view.busFaderPositions,
                liveRows, liveParameters, engine.config.fxSlotsPerBus,
                engine.config.mixRate.SampleRate());
            bool attachmentConflict = false;
            for (const auto& row : slotPlan.assignments)
                attachmentConflict |= HasPreviewEffectAttachment(engine, {row.slot, row.generation});
            const bool masterHasEffects = std::any_of(view.effects.begin(), view.effects.end(),
                [](const auto& row) { return row.busId == VN_BUS_MASTER; });
            if (validation != ProjectGenerationPrepareStatusV1::Ok || attachmentConflict ||
                (masterHasEffects && !PreviewOwnsSoleMasterV1(engine, view.ownerId))) {
                ReleasePreparedSlot(engine, planIndex, ownerSlot);
                result.status = validation != ProjectGenerationPrepareStatusV1::Ok ? validation :
                    ProjectGenerationPrepareStatusV1::CapabilityUnavailable;
                return result;
            }
        }
        if (!CanApplyPreviewProperties(engine, slotPlan, view.publicationVintage)) {
            ReleasePreparedSlot(engine, planIndex, ownerSlot);
            result.status = ProjectGenerationPrepareStatusV1::Backpressure;
            return result;
        }
        if ((view.publicationVintage == 5 || closeEffects) &&
            std::any_of(coldSlots.begin(), coldSlots.end(), [](auto cold) { return cold != 0; })) {
            auto coldPlan = slotPlan;
            std::erase_if(coldPlan.assignments, [&](const auto& row) { return coldSlots[row.slot] == 0; });
            std::erase_if(coldPlan.removed, [&](const auto& row) { return coldSlots[row.slot] == 0; });
            for (std::uint32_t slot = 0; slot < coldPlan.busCount; ++slot)
                if (!coldSlots[slot]) coldPlan.neutralSlots[slot] = 0;
            coldEffects = std::make_unique<PreparedBusEffectsV1>();
            const auto status = coldEffects->Prepare(engine, coldPlan, coldRows,
                coldParameters, view.ownerId, view.ownerId, true);
            if (status != VN_OK) {
                ReleasePreparedSlot(engine, planIndex, ownerSlot);
                result.status = status == VN_BACKPRESSURE || status == VN_IN_USE
                    ? ProjectGenerationPrepareStatusV1::Backpressure
                    : status == VN_NO_EFFECT ? ProjectGenerationPrepareStatusV1::Capacity
                    : ProjectGenerationPrepareStatusV1::InvalidView;
                return result;
            }
        }
        const auto commandCount = 1u + (coldEffects ? coldEffects->Commands().size() : 0u);
        if (commandCount > UINT32_MAX ||
            !LiveCommandBatchFitsCapacities(engine, static_cast<std::uint32_t>(commandCount))) {
            ReleasePreparedSlot(engine, planIndex, ownerSlot);
            result.status = ProjectGenerationPrepareStatusV1::Capacity;
            return result;
        }
        ApplySlotPlanToProspective(slotPlan, remapped);
        fillView = &remapped;
        std::vector<std::uint32_t> feederScratch(remapped.busCount, 0);
        std::vector<std::uint32_t> emittedFeederScratch(remapped.busCount, 0);
        std::vector<std::uint8_t> emittedScratch(remapped.busCount, 0);
        remapped.feederCount = feederScratch.data();
        remapped.emittedFeeders = emittedFeederScratch.data();
        remapped.emitted = emittedScratch.data();
        if (!ProspectiveTopologyIsAcyclic(engine, fillView)) {
            ReleasePreparedSlot(engine, planIndex, ownerSlot);
            result.status = ProjectGenerationPrepareStatusV1::Capacity;
            return result;
        }
        FillLiveOrPreparedPlan(engine, engine.preparedTopologyPlans, planIndex,
                               fillView);
        if (volumeOnly) {
            authoredMixDb.assign(slotPlan.busCount, 0.0f);
            authoredOutputDb.assign(slotPlan.busCount, 0.0f);
            effectiveMixDb.assign(slotPlan.busCount, 0.0f);
            effectiveOutputDb.assign(slotPlan.busCount, 0.0f);
            for (std::uint32_t i = 0; i < view.authoredBusCount; ++i) {
                if (view.authoredBusIds[i] != view.prospective->busIds[i]) {
                    ReleasePreparedSlot(engine, planIndex, ownerSlot);
                    result.status = ProjectGenerationPrepareStatusV1::InvalidView;
                    return result;
                }
                const auto slot = FindAssignedSlot(slotPlan, view.authoredBusIds[i]);
                if (slot == UINT32_MAX || slot >= slotPlan.busCount) {
                    ReleasePreparedSlot(engine, planIndex, ownerSlot);
                    result.status = ProjectGenerationPrepareStatusV1::InvalidView;
                    return result;
                }
                if (slotPlan.neutralSlots[slot] == 0) {
                    // A new owner may share the retained Master during close/reopen.
                    // Its canonical zero row does not own that slot's live volume.
                    if (view.authoredBusIds[i] == VN_BUS_MASTER && slot == 0 &&
                        view.authoredBusVolumeDb[i] == 0.0f &&
                        view.authoredBusOutputVolumeDb[i] == 0.0f)
                        continue;
                    ReleasePreparedSlot(engine, planIndex, ownerSlot);
                    result.status = ProjectGenerationPrepareStatusV1::InvalidView;
                    return result;
                }
                const float mix = view.authoredBusVolumeDb[i];
                const float output = view.authoredBusOutputVolumeDb[i];
                const bool retained = slot < engine.buses.BusCount() &&
                    engine.buses.AliveAt(slot) &&
                    engine.buses.IdAt(slot) == view.authoredBusIds[i] &&
                    engine.buses.GenerationAt(slot) == slotPlan.generations[slot];
                const float effectiveMix = retained
                    ? engine.busTargets.ReduceWithSlot(slot, kBusTargetBusVolume,
                        BusTargetStore::kSlotAuthoredIndex, mix) : mix;
                const float effectiveOutput = retained
                    ? engine.busTargets.ReduceWithSlot(slot, kBusTargetBusOutputVolume,
                        BusTargetStore::kSlotAuthoredIndex, output) : output;
                if (!TryVolumeGainFromDb(mix) || !TryVolumeGainFromDb(output) ||
                    !TryVolumeGainFromDb(effectiveMix) ||
                    !TryVolumeGainFromDb(effectiveOutput)) {
                    ReleasePreparedSlot(engine, planIndex, ownerSlot);
                    result.status = ProjectGenerationPrepareStatusV1::InvalidView;
                    return result;
                }
                authoredMixDb[slot] = mix;
                authoredOutputDb[slot] = output;
                effectiveMixDb[slot] = effectiveMix;
                effectiveOutputDb[slot] = effectiveOutput;
            }
        }
        for (std::uint32_t slot = 0; slot < slotPlan.busCount; ++slot) {
            engine.preparedTopologyPlans.MigrationTargetsData(planIndex)[slot] = slotPlan.migrationTargets[slot];
            if (slotPlan.neutralSlots[slot] != 0) {
                engine.preparedTopologyPlans.BusVolumeApplyData(planIndex)[slot] =
                    kApplyBusNeutralProperties | kApplyBusVolume | kApplyBusOutputVolume |
                    (view.publicationVintage == 5 ? kApplyBusPreserveEffects : 0);
                engine.preparedTopologyPlans.PreparedBusVolumeDbData(planIndex)[slot] =
                    volumeOnly ? effectiveMixDb[slot] : 0.0f;
                engine.preparedTopologyPlans.PreparedBusOutputVolumeDbData(planIndex)[slot] =
                    volumeOnly ? effectiveOutputDb[slot] : 0.0f;
                engine.preparedTopologyPlans.PreparedBusVolumeRampMsData(planIndex)[slot] = 0;
                continue;
            }
            engine.preparedTopologyPlans.OutputLayoutData(planIndex)[slot] = engine.busOutputLayout[slot];
            engine.preparedTopologyPlans.SetCustomDownmix(planIndex, slot, engine.CustomDownmix(slot));
        }
        if (engine.topologyPlans.InUse() != liveInUse ||
            engine.commands.PublishedHead() != commandHeadBefore ||
            engine.commands.InitialSourcePublishedHead() != initialSourceHeadBefore) {
            ReleasePreparedSlot(engine, planIndex, ownerSlot);
            result.status = ProjectGenerationPrepareStatusV1::DeviceError;
            return result;
        }
        imageStatus = engine.variables.PrepareCurrentWorldImageV1(
            view.compiledWorld, image,
            view.forceWorldReplacement && transition != TopologyOwnerRevisionDispositionV1::Retry);
        if (imageStatus == WorldInstallStatus::Ok) {
            const auto current = engine.variables.CurrentWorldGeneration();
            if (current == UINT64_MAX && !image.PreservesContent()) {
                imageStatus = WorldInstallStatus::Capacity;
            } else {
                const auto topology = engine.preparedTopologyPlans.View(planIndex);
                imageStatus = PrepareWorldSignals(engine, view.compiledWorld,
                    current + (image.PreservesContent() ? 0 : 1), &topology, signals);
                if (imageStatus == WorldInstallStatus::Ok)
                    AttachPreparedSignals(engine.preparedTopologyPlans, planIndex, signals);
            }
        }
        if (imageStatus == WorldInstallStatus::Ok &&
            (!engine.params.PrepareVariableStorage(view.compiledWorld.variableCount) ||
             !engine.PrepareQueueSelectionStorage(view.compiledWorld)))
            imageStatus = WorldInstallStatus::Capacity;
        if (imageStatus == WorldInstallStatus::Ok && volumeOnly) {
            bindingSourceStamp = ComputeBusBindingSourceStamp(engine.bindings, engine.params);
            sourceMixDb.resize(slotPlan.busCount);
            sourceOutputDb.resize(slotPlan.busCount);
            for (std::uint32_t slot = 0; slot < slotPlan.busCount; ++slot) {
                sourceMixDb[slot] = engine.busTargets.Reduce(slot, kBusTargetBusVolume);
                sourceOutputDb[slot] = engine.busTargets.Reduce(slot, kBusTargetBusOutputVolume);
            }
            VariableServiceV1::PreparedWorldProjectionV1 world;
            if (!engine.variables.PrepareWorldProjectionV1(image, engine.emitters, &world) ||
                !ProjectPreviewVolumeBindings(engine, slotPlan, world,
                    authoredMixDb, authoredOutputDb, effectiveMixDb, effectiveOutputDb,
                    projectedVolumeApply, planIndex)) {
                ReleasePreparedSlot(engine, planIndex, ownerSlot);
                result.status = ProjectGenerationPrepareStatusV1::InvalidView;
                return result;
            }
        }
    } catch (const std::bad_alloc&) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ProjectAdmissionSnapshot);
        ReleasePreparedSlot(engine, planIndex, ownerSlot);
        result.status = ProjectGenerationPrepareStatusV1::Capacity;
        return result;
    } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ProjectAdmissionSnapshot);
        ReleasePreparedSlot(engine, planIndex, ownerSlot);
        result.status = ProjectGenerationPrepareStatusV1::DeviceError;
        return result;
    }
    if (imageStatus != WorldInstallStatus::Ok) {
        ReleasePreparedSlot(engine, planIndex, ownerSlot);
        result.status = MapWorldPrepareStatus(imageStatus);
        return result;
    }
    if (CurrentWorldCommitBlocked(engine, view.publicationVintage, image) ||
        (HasPredecessorDirectWorldPublication(engine) &&
         !(image.PreservesContent() &&
           view.compiledWorld.sourceBodyVintage == kCurrentWorldBodyVintageV20))) {
        ReleasePreparedSlot(engine, planIndex, ownerSlot);
        result.status = ProjectGenerationPrepareStatusV1::Backpressure;
        return result;
    }
    if (engine.topologyPlans.InUse() != liveInUse ||
        engine.commands.PublishedHead() != commandHeadBefore ||
        engine.commands.InitialSourcePublishedHead() != initialSourceHeadBefore) {
        ReleasePreparedSlot(engine, planIndex, ownerSlot);
        result.status = ProjectGenerationPrepareStatusV1::DeviceError;
        return result;
    }

    std::vector<PreparedBusEffectChain> effects;
    struct PreparedSeeds {
        Engine& engine;
        std::vector<EffectParamTargetStore::PreparedSeedToken> tokens;
        ~PreparedSeeds() {
            for (auto& token : tokens) engine.effectParamTargets.CancelPreparedInstalledEffectSeed(&token);
        }
    } seeds{engine};
    struct PreparedWork {
        Engine& engine;
        std::vector<BusTree::WorkBinding> rows;
        std::vector<BusTree::DormantSource> sources;
        ~PreparedWork() { for (const auto& row : rows) engine.buses.ReleaseWorkRow(row.work); }
    } preparedWork{engine};
    try {
        effects.reserve(slotPlan.assignments.size() + slotPlan.removed.size());
        preparedWork.rows.reserve(slotPlan.assignments.size());
        preparedWork.sources.reserve(slotPlan.assignments.size());
        const auto prepareChain = [&](BusSlotRef host,
            const std::array<PreviewEffectChainEntry, kMaxFxSlotsPerBus>& targets) -> VnResult {
            effects.emplace_back();
            std::array<EffectInstanceRef, kMaxFxSlotsPerBus> refs{};
            if (engine.buses.LogicRefMatches(host)) {
                refs = engine.BusEffectRefs(host.index);
                if (!engine.buses.AcceptedWorkRef(host).Valid() && host.index < slotPlan.alive.size() &&
                    slotPlan.alive[host.index] && slotPlan.generations[host.index] == host.generation) {
                    const auto work = engine.buses.PrepareWorkRow();
                    if (!work.Valid()) return VN_DEVICE_ERROR;
                    preparedWork.rows.push_back({host, work});
                    auto& row = *engine.buses.PreparedWorkRow(work);
                    const auto status = engine.PrepareBusWorkState(host.index, row);
                    if (status != VN_OK) return status;
                    if (!engine.buses.RestoreDormantWork(row)) return VN_DEVICE_ERROR;
                    preparedWork.sources.push_back(engine.buses.DormantSourceFor(host));
                    std::copy_n(row.fxSlotEffectRef, kMaxFxSlotsPerBus, refs.begin());
                }
            }
            const auto result = engine.buses.PreparePreviewEffectChain(host, view.ownerId, targets, refs,
                engine.CurrentProjectId(), effects.back());
            if (result != VN_OK || !effects.back().changes) return result;
            const auto& update = *effects.back().state->update;
            for (std::uint32_t slot = 0; slot < engine.config.fxSlotsPerBus; ++slot) {
                if ((update.changedMask & (1u << slot)) == 0 || targets[slot].effectType == kEffectTypeNone)
                    continue;
                seeds.tokens.emplace_back();
                const float values[]{targets[slot].ceilingDb, targets[slot].releaseMs};
                if (!engine.effectParamTargets.PrepareInstalledEffectSeed(
                        EffectParamTargetStore::Scope::Bus, host.index, host.generation, slot,
                        targets[slot].effectType, values, 2, &seeds.tokens.back())) return VN_NO_EFFECT;
            }
            return VN_OK;
        };
        VnResult effectStatus = VN_OK;
        if (view.publicationVintage == 5 || closeEffects) {
            for (const auto& assignment : slotPlan.assignments) {
                if (slotPlan.neutralSlots[assignment.slot] == 0) continue;
                if (coldSlots[assignment.slot]) {
                    if (!engine.buses.PreviewEffectOwned({assignment.slot, assignment.generation})) continue;
                    if (HasEffectChainPublicationConsumers(engine)) { effectStatus = VN_BACKPRESSURE; break; }
                    effects.emplace_back();
                    effectStatus = engine.buses.PreparePreviewEffectOwnerRelease(
                        {assignment.slot, assignment.generation}, view.ownerId, effects.back());
                    if (effectStatus != VN_OK) break;
                    continue;
                }
                std::array<PreviewEffectChainEntry, kMaxFxSlotsPerBus> targets{};
                for (const auto& row : liveRows) if (row.busId == assignment.id) {
                    targets[row.slot] = {row.effectId, row.effectType, (row.flags & 1u) != 0,
                        liveParameters[row.parameterFirst].value,
                        liveParameters[row.parameterFirst + 1].value};
                }
                effectStatus = prepareChain({assignment.slot, assignment.generation}, targets);
                if (effectStatus != VN_OK) break;
            }
        }
        for (const auto& removed : slotPlan.removed) {
            if (effectStatus != VN_OK) break;
            if (coldSlots[removed.slot]) continue;
            if (engine.buses.PreviewEffectOwned({removed.slot, removed.generation}))
                effectStatus = prepareChain({removed.slot, removed.generation}, {});
        }
        if (effectStatus != VN_OK) {
            for (auto& row : effects) engine.buses.CancelPreviewEffectChain(row);
            ReleasePreparedSlot(engine, planIndex, ownerSlot);
            result.status = effectStatus == VN_NO_EFFECT ? ProjectGenerationPrepareStatusV1::Capacity :
                effectStatus == VN_DEVICE_ERROR ? ProjectGenerationPrepareStatusV1::DeviceError :
                ProjectGenerationPrepareStatusV1::Backpressure;
            return result;
        }
        std::vector<BusEffectChainAdoption> adoptions;
        for (const auto& row : effects) if (row.changes)
            adoptions.push_back({row.state, row.host.Packed(), row.state->preparedOwner,
                row.state->update->targetRevision, row.releaseOwner});
        engine.preparedTopologyPlans.SetEffectUpdates(planIndex, std::move(adoptions));
    } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ProjectAdmissionSnapshot);
        for (auto& row : effects) engine.buses.CancelPreviewEffectChain(row);
        ReleasePreparedSlot(engine, planIndex, ownerSlot);
        result.status = ProjectGenerationPrepareStatusV1::DeviceError;
        return result;
    }
    out.effectChains_ = std::move(effects);
    out.effectSeeds_ = std::move(seeds.tokens);
    out.preparedWork_ = std::move(preparedWork.rows);
    out.preparedCold_ = std::move(preparedWork.sources);
    out.domain_ = engine.projectGenerationReservationDomain;
    out.domainEpoch_ = engine.projectGenerationReservationDomain->epoch;
    out.engine_ = &engine;
    out.occupied_ = true;
    out.idempotent_ = image.Idempotent();
    out.preservesContent_ = image.PreservesContent();
    out.signals_ = std::move(signals);
    out.preparedPlanIndex_ = planIndex;
    out.preparedOwnerSlot_ = ownerSlot;
    out.preparedSerial_ = owner.preparedSerial;
    out.liveOwnerSlot_ = destination;
    out.expectedOwner_ = expectedOwner;
    out.expectedHolder_ = expectedHolder;
    out.nextOwner_ = expectedOwner;
    if (transition == TopologyOwnerRevisionDispositionV1::Advance) {
        out.nextOwner_ = {true, view.ownerId, view.ownerRevision,
            expectedOwner.live ? expectedOwner.currentRevision : view.ownerBaseRevision,
            view.publishedBusFingerprint, view.compileTargetProfileFingerprint};
    }
    out.nextHolder_ = transition == TopologyOwnerRevisionDispositionV1::Retry ? expectedHolder
        : PreviewMasterHolderV1{true, view.ownerId, view.ownerRevision, masterFingerprint};
    out.token_ = view.token;
    out.key_ = MakeStudioKey(view.token, view.compileTargetProfileFingerprint);
    out.publishedBusFingerprint_ = view.publishedBusFingerprint;
    out.compileTargetProfileFingerprint_ = view.compileTargetProfileFingerprint;
    out.publicationVintage_ = view.publicationVintage;
    out.compiledWorld_ = view.compiledWorld;
    out.worldImage_ = std::move(image);
    out.livePlanIndex_ = UINT32_MAX;
    out.slotPlan_ = std::move(slotPlan);
    out.authoredMixDb_ = std::move(authoredMixDb);
    out.authoredOutputDb_ = std::move(authoredOutputDb);
    out.effectiveMixDb_ = std::move(effectiveMixDb);
    out.effectiveOutputDb_ = std::move(effectiveOutputDb);
    out.sourceMixDb_ = std::move(sourceMixDb);
    out.sourceOutputDb_ = std::move(sourceOutputDb);
    out.projectedVolumeApply_ = std::move(projectedVolumeApply);
    out.bindingSourceStamp_ = bindingSourceStamp;
    out.effects_ = std::move(coldEffects);
    result.status = ProjectGenerationPrepareStatusV1::Ok;
    result.preparedSerial = out.preparedSerial_;
    return result;
}

ProjectGenerationCommitResultV1 CommitPreparedProjectGenerationV1(
    Engine& engine, PreparedProjectGenerationV1& prepared) noexcept {
    harken::monitor::IdentityMutationScope identityMutation;
    ProjectGenerationCommitResultV1 result{};
    if (engine.canonicalPackSetBusy) {
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    }
    if (!prepared.Valid() || prepared.engine_ != &engine ||
        prepared.preparedPlanIndex_ == UINT32_MAX) {
        result.status = ProjectGenerationCommitStatusV1::InvalidPrepared;
        return result;
    }
    if (CurrentWorldCommitBlocked(engine, prepared.publicationVintage_, prepared.worldImage_) ||
        (HasPredecessorDirectWorldPublication(engine) &&
         !(prepared.preservesContent_ &&
           prepared.compiledWorld_.sourceBodyVintage == kCurrentWorldBodyVintageV20))) {
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    }
    if (engine.variables.CurrentWorldGeneration() == UINT64_MAX &&
        !prepared.worldImage_.PreservesContent()) {
        result.status = ProjectGenerationCommitStatusV1::DeviceError;
        return result;
    }
    if (!prepared.worldImage_.Valid() ||
        !CanCommitSignals(engine, prepared.signals_) ||
        !engine.variables.CanCommitPreparedCurrentWorldImageV1(prepared.worldImage_)) {
        result.status = ProjectGenerationCommitStatusV1::InvalidPrepared;
        return result;
    }
    const auto ownerReady = [&]() noexcept {
        const auto slot = prepared.liveOwnerSlot_;
        return slot < engine.busTopologyOwners.size() && slot < engine.previewMasterHolders.size() &&
            SameOwnerRow(engine.busTopologyOwners[slot], prepared.expectedOwner_) &&
            SameHolderRow(engine.previewMasterHolders[slot], prepared.expectedHolder_) &&
            PreviewMasterCompatible(engine, prepared.nextHolder_.fingerprint, prepared.slotPlan_.ownerId) &&
            (prepared.slotPlan_.neutralSlots[0] != 0) == PreviewOwnsSoleMasterV1(engine, prepared.slotPlan_.ownerId);
    };
    const auto effectSourceReady = [&]() noexcept {
        for (std::size_t i = 0; i < prepared.preparedWork_.size(); ++i) {
            const auto& row = prepared.preparedWork_[i];
            if (!engine.buses.LogicRefMatches(row.bus) || engine.buses.AcceptedWorkRef(row.bus).Valid() ||
                engine.buses.DormantSourceFor(row.bus) != prepared.preparedCold_[i]) return false;
        }
        for (const auto& row : prepared.effectChains_) {
            if (!engine.buses.CanCommitPreviewEffectChain(row) ||
                HasPreviewEffectAttachment(engine, row.host) ||
                (row.releaseOwner && HasEffectChainPublicationConsumers(engine))) return false;
        }
        return true;
    };
    if (!effectSourceReady()) {
        result.status = ProjectGenerationCommitStatusV1::InvalidPrepared;
        return result;
    }
    const auto volumeSourceReady = [&]() noexcept {
        if (prepared.publicationVintage_ != 3 && prepared.publicationVintage_ != 4 && prepared.publicationVintage_ != 5) return true;
        if (prepared.bindingSourceStamp_ != ComputeBusBindingSourceStamp(engine.bindings, engine.params) ||
            prepared.sourceMixDb_.size() != prepared.slotPlan_.busCount ||
            prepared.sourceOutputDb_.size() != prepared.slotPlan_.busCount ||
            prepared.projectedVolumeApply_.size() != prepared.slotPlan_.busCount)
            return false;
        for (std::uint32_t slot = 0; slot < prepared.slotPlan_.busCount; ++slot) {
            if (busfingerprint::CanonicalFloatBits(engine.busTargets.Reduce(
                    slot, kBusTargetBusVolume)) !=
                    busfingerprint::CanonicalFloatBits(prepared.sourceMixDb_[slot]) ||
                busfingerprint::CanonicalFloatBits(engine.busTargets.Reduce(
                    slot, kBusTargetBusOutputVolume)) !=
                    busfingerprint::CanonicalFloatBits(prepared.sourceOutputDb_[slot]))
                return false;
        }
        return true;
    };
    if (!prepared.slotPlan_.valid ||
        !SlotPlanMatchesLive(engine, prepared.slotPlan_) || !ownerReady() ||
        !volumeSourceReady() ||
        (prepared.effects_ && !prepared.effects_->CanCommit()) ||
        !CanApplyPreviewProperties(engine, prepared.slotPlan_,
            prepared.publicationVintage_)) {
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    }
    const WorldGeneration priorGeneration = engine.variables.CurrentWorldGeneration();
    const WorldGeneration predictedGeneration = prepared.preservesContent_ ? priorGeneration : priorGeneration + 1;
    VariableServiceV1::PreparedWorldProjectionV1 projection;
    PreparedAdmissionWriteV1 admission;
    PreparedProjectGenerationTraceV1 trace;
    EnginePreparedRoutingMigrationV1 unpublishedRouting;
    try {
        if (!engine.variables.PrepareWorldProjectionV1(prepared.worldImage_, engine.emitters, &projection) ||
            !PrepareAdmissionWrite(engine, predictedGeneration, prepared.key_, admission) ||
            !PrepareUnpublishedTimelineRoutingV1(engine,
                engine.preparedTopologyPlans.View(prepared.preparedPlanIndex_), unpublishedRouting)) {
            result.status = ProjectGenerationCommitStatusV1::Backpressure;
            return result;
        }
    } catch (const std::bad_alloc&) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ProjectAdmissionSnapshot);
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ProjectAdmissionSnapshot);
        result.status = ProjectGenerationCommitStatusV1::DeviceError;
        return result;
    }
    engine.topologyPlans.DrainRetire();
    const std::uint32_t liveIndex = engine.topologyPlans.AcquireSparse(engine.busSendActive + 1);
    if (liveIndex == kInvalidTopologyPlanIndex) {
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    }
    if (!ProjectCheckpoint(detail::LogicAllocSite::ProjectTopologyCopy) ||
        !engine.topologyPlans.CopyPlanFrom(engine.preparedTopologyPlans,
                                           prepared.preparedPlanIndex_,
                                           liveIndex)) {
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::DeviceError;
        return result;
    }
    WorldInstallResult traced{};
    traced.status = WorldInstallStatus::Ok;
    traced.priorGeneration = priorGeneration;
    traced.resultGeneration = predictedGeneration;
    traced.idempotent = prepared.idempotent_;
    try {
        AuthoredTraceProjectionV1 authoredTrace;
        if (engine.trace != nullptr && engine.trace->Active() &&
            !BuildAuthoredTraceProjection(prepared.slotPlan_, authoredTrace)) {
            engine.topologyPlans.Recycle(liveIndex);
            result.status = ProjectGenerationCommitStatusV1::InvalidPrepared;
            return result;
        }
        if (liveIndex >= engine.projectGenerationInstallAckSideTable.size() ||
            !TraceRecorder::PrepareProjectGenerationTraceV1(engine, authoredTrace.view,
                engine.variables.PreparedWorldInstallViewV1(prepared.worldImage_), traced, prepared.token_,
                prepared.publishedBusFingerprint_, prepared.compileTargetProfileFingerprint_,
                prepared.publicationVintage_, &trace)) {
            engine.topologyPlans.Recycle(liveIndex);
            result.status = ProjectGenerationCommitStatusV1::DeviceError;
            return result;
        }
    } catch (const std::bad_alloc&) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ProjectAdmissionSnapshot);
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ProjectAdmissionSnapshot);
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::DeviceError;
        return result;
    }
    std::vector<CommandRecord> commands;
    try {
        commands.resize(1);
        if (prepared.effects_) {
            const auto effects = prepared.effects_->Commands();
            commands.insert(commands.end(), effects.begin(), effects.end());
        }
    } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ProjectAdmissionSnapshot);
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    }
    CommandQueue::BatchReservation reservation{};
    if (commands.size() > UINT32_MAX ||
        !LiveCommandBatchFitsCapacities(engine, static_cast<std::uint32_t>(commands.size()))) {
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::InvalidPrepared;
        return result;
    }
    if (!ProjectCheckpoint(detail::LogicAllocSite::ProjectCommandReserve) ||
        !AdmitLiveCommandBatch(engine, static_cast<std::uint32_t>(commands.size())) ||
        !engine.commands.ReserveBatch(static_cast<std::uint32_t>(commands.size()), &reservation)) {
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    }
    if (engine.acceptedBusTopologyRevision == UINT64_MAX) {
        engine.commands.CancelReservedBatch(&reservation);
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::DeviceError;
        return result;
    }
    const auto acceptedRevision = engine.acceptedBusTopologyRevision + 1;
    bool retirementReady = true;
    for (const auto& removed : prepared.slotPlan_.removed) {
        retirementReady &= CanRetireTopologyBusV1(engine, {removed.slot, removed.generation}, acceptedRevision);
    }
    bool volumeOverlayReady = true;
    if (prepared.publicationVintage_ == 3 || prepared.publicationVintage_ == 4 || prepared.publicationVintage_ == 5) {
        for (const auto& assignment : prepared.slotPlan_.assignments) {
            const auto slot = assignment.slot;
            if (prepared.slotPlan_.neutralSlots[slot] == 0) continue;
            volumeOverlayReady &= TryVolumeGainFromDb(prepared.effectiveMixDb_[slot]) &&
                TryVolumeGainFromDb(prepared.effectiveOutputDb_[slot]);
        }
    }
    if (!ProjectCheckpoint(detail::LogicAllocSite::ProjectFinalPreflight) || !retirementReady ||
        !CanCommitSignals(engine, prepared.signals_) ||
        !volumeOverlayReady || !volumeSourceReady() || !effectSourceReady() ||
        (prepared.effects_ && !prepared.effects_->CanCommit()) ||
        !prepared.Valid() || !ownerReady() ||
        CurrentWorldCommitBlocked(engine, prepared.publicationVintage_, prepared.worldImage_) ||
        (HasPredecessorDirectWorldPublication(engine) &&
         !(prepared.preservesContent_ &&
           prepared.compiledWorld_.sourceBodyVintage == kCurrentWorldBodyVintageV20)) ||
        !SlotPlanMatchesLive(engine, prepared.slotPlan_) ||
        !CanApplyPreviewProperties(engine, prepared.slotPlan_,
            prepared.publicationVintage_) ||
        !ValidateTopologyPlanVolumesV1(engine.topologyPlans.View(liveIndex)) ||
        !engine.variables.CanCommitPreparedCurrentWorldImageV1(prepared.worldImage_) ||
        !engine.variables.CanCommitWorldProjectionV1(projection, engine.emitters) ||
        !CanCommitAdmissionWrite(engine, admission) ||
        !CanCommitUnpublishedTimelineRoutingV1(engine, engine.topologyPlans.View(liveIndex), unpublishedRouting) ||
        !TraceRecorder::CanCommitProjectGenerationTraceV1(trace)) {
        engine.commands.CancelReservedBatch(&reservation);
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    }
    auto& command = commands.front();
    command.opcode = kOpcodeSetBusTopologyPlan;
    command.payload.setBusTopologyPlan.planIndex = liveIndex;
    command.payload.setBusTopologyPlan.dynamicRoutePlanIndex =
        kInvalidDynamicRoutePlanIndex;
    command.payload.setBusTopologyPlan.connectFrom = kInvalidBusIndex;
    command.payload.setBusTopologyPlan.connectTo = kInvalidBusIndex;
    command.payload.setBusTopologyPlan.connectLevelDb = 0.0f;
    command.payload.setBusTopologyPlan.planFlags = 0;
    command.payload.setBusTopologyPlan.connectFromGeneration = 0;
    command.payload.setBusTopologyPlan.connectToGeneration = 0;
    command.payload.setBusTopologyPlan.acceptedGraphRevision =
        acceptedRevision;
    command.payload.setBusTopologyPlan.batchCommandCount = static_cast<std::uint32_t>(commands.size());
    CommandQueue::PreparedBatchProof proof;
    bool targetStorageReady = engine.PrepareBusRetirements(prepared.slotPlan_.removed.size());
    try {
        std::vector<BusEffectSettings::Write> effectWrites;
        if (prepared.effects_) targetStorageReady &= prepared.effects_->AppendSettingsWrites(effectWrites);
        for (const auto& chain : prepared.effectChains_) {
            if (!chain.changes || chain.releaseOwner) continue;
            const auto& update = *chain.state->update;
            for (std::uint32_t slot = 0; slot < engine.config.fxSlotsPerBus; ++slot)
                if (update.targets[slot].effectType != kEffectTypeNone)
                    effectWrites.push_back({chain.host.index, slot, {update.targets[slot].effectType,
                        engine.effectBank.ObjectIdentity(update.targetRefs[slot]), chain.state->preparedOwner}});
        }
        targetStorageReady &= engine.busEffects.Prepare(effectWrites);
    } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ProjectAdmissionSnapshot); targetStorageReady = false; }
    for (const auto& assignment : prepared.slotPlan_.assignments) {
        targetStorageReady &= engine.busTargets.PrepareSettings(assignment.slot, kBusTargetBusVolume);
        targetStorageReady &= engine.busTargets.PrepareSettings(assignment.slot, kBusTargetBusOutputVolume);
    }
    engine.bindings.ForEachBusAttachment([&](const auto& attachment, const auto& definition) {
        targetStorageReady &= engine.busTargets.PrepareActivity(attachment.busIndex, definition.targetId);
    });
    if (!targetStorageReady ||
        !PrepareTopologyActivity(engine, engine.topologyPlans.View(liveIndex), reservation, commands, proof,
            nullptr, true, prepared.preparedWork_)) {
        engine.commands.CancelReservedBatch(&reservation);
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    }
    for (const auto& removed : prepared.slotPlan_.removed)
        CommitRetireTopologyBusV1(engine, {removed.slot, removed.generation}, acceptedRevision);
    CommitUnpublishedTimelineRoutingV1(engine, unpublishedRouting);
    CommitMigratedLogicVoiceMirrorsV1(engine, prepared.slotPlan_.migrationTargets.data(),
        prepared.slotPlan_.generations.data(), prepared.slotPlan_.alive.data(),
        prepared.slotPlan_.busCount);
    for (const auto& removed : prepared.slotPlan_.removed)
        ClearRetiredBusPropertiesV1(engine, {removed.slot, removed.generation});
    InstallSlotPlanLogic(engine, prepared.slotPlan_);
    for (const auto& assignment : prepared.slotPlan_.assignments) {
        if (prepared.slotPlan_.neutralSlots[assignment.slot] == 0) continue;
        const auto slot = assignment.slot;
        if (prepared.publicationVintage_ != 3 && prepared.publicationVintage_ != 4 && prepared.publicationVintage_ != 5) {
            ResetNeutralBusPropertiesV1(engine, slot);
            continue;
        }
        const bool retained = slot < prepared.slotPlan_.sourceBusCount &&
            assignment.retain != 0;
        if (retained) ResetVolumeOnlyNonVolumePropertiesV2(engine, slot, prepared.publicationVintage_ == 5);
        else ResetNeutralBusPropertiesV1(engine, slot);
        engine.busTargets.SetSlot(slot, kBusTargetBusVolume,
            BusTargetStore::kSlotAuthoredIndex, prepared.authoredMixDb_[slot]);
        engine.busTargets.SetSlot(slot, kBusTargetBusOutputVolume,
            BusTargetStore::kSlotAuthoredIndex, prepared.authoredOutputDb_[slot]);
    }

    for (auto& seed : prepared.effectSeeds_)
        engine.effectParamTargets.CommitPreparedInstalledEffectSeed(&seed);
    for (const auto& row : prepared.effectChains_) {
        engine.buses.CommitPreviewEffectChain(row);
        if (!row.changes || row.releaseOwner) continue;
        const auto slot = row.host.index;
        const auto& update = *row.state->update;
        for (std::uint32_t i = 0; i < engine.config.fxSlotsPerBus; ++i) {
            engine.busEffects.Set(slot, i, {update.targets[i].effectType,
                engine.effectBank.ObjectIdentity(update.targetRefs[i]),
                update.targetRefs[i].type == kEffectTypeNone ? 0 : row.state->owner});
            engine.SetBusEffectRef(slot, i, update.targetRefs[i]);
            engine.SetBusEffectBypass(slot, i, update.targets[i].bypass);
            if (update.targetRefs[i].type == kEffectTypeNone)
                engine.effectParamTargets.ClearSlot(EffectParamTargetStore::Scope::Bus, slot, row.host.generation, i);
            else if ((update.changedMask & (1u << i)) == 0) {
                const float values[]{update.targets[i].ceilingDb, update.targets[i].releaseMs};
                for (std::uint32_t p = 0; p < 2; ++p)
                    if ((update.parameterMask[i] & (1u << p)) != 0)
                        engine.effectParamTargets.NoteDirect(EffectParamTargetStore::Scope::Bus,
                            slot, i, p, values[p], values[p]);
            }
            if (update.targetRefs[i].type != kEffectTypeNone) engine.effectBank.PublishInstalled(update.targetRefs[i]);
        }
    }
    for (const auto& removed : prepared.slotPlan_.removed)
        engine.buses.ForgetPreviewEffectOwner({removed.slot, removed.generation});

    if (liveIndex < engine.projectGenerationInstallAckSideTable.size()) {
        auto& ack = engine.projectGenerationInstallAckSideTable[liveIndex];
        ack.valid = 1;
        ack.zero = 0;
        ack.slotGeneration = liveIndex + 1u;
        ack.planSequence = prepared.preparedSerial_;
        ack.planFingerprint = prepared.token_.authoringRevision;
        ack.claimSetFingerprint = prepared.token_.submissionSerial;
    }
    engine.busTopologyOwners[prepared.liveOwnerSlot_] = prepared.nextOwner_;
    engine.previewMasterHolders[prepared.liveOwnerSlot_] = prepared.nextHolder_;

    const WorldGeneration installed =
        engine.variables.CommitPreparedCurrentWorldImageV1(prepared.worldImage_);
    CommitSignals(engine, prepared.signals_);
    if (!prepared.preservesContent_) ReclaimRetiredSelectionMemory(engine);
    engine.variables.CommitWorldProjectionV1(&projection, engine.params, engine.emitters,
                                              engine.bindings, engine.busTargets);
    if (prepared.publicationVintage_ == 3 || prepared.publicationVintage_ == 4 || prepared.publicationVintage_ == 5) {
        engine.bindings.EvaluateBusAttachments(engine.params, engine.busTargets);
        for (std::uint32_t slot = 0; slot < prepared.slotPlan_.busCount; ++slot) {
            if (prepared.slotPlan_.neutralSlots[slot] == 0) continue;
            const auto apply = prepared.projectedVolumeApply_[slot];
            if ((apply & kApplyBusVolume) != 0) {
                assert(busfingerprint::CanonicalFloatBits(engine.busTargets.Reduce(
                    slot, kBusTargetBusVolume)) ==
                    busfingerprint::CanonicalFloatBits(prepared.effectiveMixDb_[slot]));
                engine.busTargets.MarkCommitted(slot, kBusTargetBusVolume,
                    prepared.effectiveMixDb_[slot], 0);
            }
            if ((apply & kApplyBusOutputVolume) != 0) {
                assert(busfingerprint::CanonicalFloatBits(engine.busTargets.Reduce(
                    slot, kBusTargetBusOutputVolume)) ==
                    busfingerprint::CanonicalFloatBits(prepared.effectiveOutputDb_[slot]));
                engine.busTargets.MarkCommitted(slot, kBusTargetBusOutputVolume,
                    prepared.effectiveOutputDb_[slot], 0);
            }
        }
    }
    if (!prepared.preservesContent_) {
        (void)CancelActionDeferredForWorldReplacement(engine, priorGeneration);
        engine.queueCheckpoints.ClearWorld(priorGeneration);
        ObserveMonitorCurrentWorld(installed);
    }
    CommitAdmissionWrite(engine, admission);
    engine.lastPairedProjectGenerationToken = prepared.token_;
    engine.lastPairedPublishedBusFingerprint = prepared.publishedBusFingerprint_;
    engine.lastPairedCompileTargetProfileFingerprint =
        prepared.compileTargetProfileFingerprint_;
    engine.lastPairedPublicationVintage = prepared.publicationVintage_;
    TraceRecorder::CommitProjectGenerationTraceV1(&trace);
    ++engine.acceptedBusTopologyRevision;

    if (prepared.effects_) prepared.effects_->Commit();
    const auto publishedBatch = engine.commands.ConsumePreparedBatch(std::move(proof));
    assert(publishedBatch.Count() == commands.size());
    (void)publishedBatch;
    for (const auto& published : commands) engine.NoteBusSettingCommand(published);
    for (const auto& row : prepared.preparedWork_) engine.buses.ReleaseWorkRow(row.work);
    prepared.preparedWork_.clear();
    prepared.preparedCold_.clear();

    ReleasePreparedSlot(engine, prepared.preparedPlanIndex_,
                        prepared.preparedOwnerSlot_);
    prepared.preparedPlanIndex_ = UINT32_MAX;
    prepared.livePlanIndex_ = liveIndex;
    prepared.occupied_ = false;
    prepared.engine_ = nullptr;
    result.status = ProjectGenerationCommitStatusV1::Ok;
    result.installedWorldGeneration = installed;
    return result;
}

std::uint32_t DrainProjectGenerationInstallAcksV1(Engine& engine) noexcept {
    ProjectGenerationInstallAckV1 records[8]{};
    std::uint32_t drained = 0;
    for (;;) {
        const std::uint32_t count =
            engine.projectGenerationInstallAckRing.Drain(records, 8);
        if (count == 0) {
            break;
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto& ack = records[index];
            if (ack.planIndex >= engine.projectGenerationInstallAckSideTable.size()) {
                continue;
            }
            const auto& identity =
                engine.projectGenerationInstallAckSideTable[ack.planIndex];
            if (identity.valid != 1u || identity.slotGeneration != ack.slotGeneration ||
                identity.planSequence != ack.planSequence ||
                identity.planFingerprint != ack.planFingerprint ||
                identity.claimSetFingerprint != ack.claimSetFingerprint) {
                continue;
            }
            bool opened = false;
            for (auto& row : engine.currentWorldBusAdmission) {
                if (!row.occupied ||
                    row.state != BusAdmissionClaimState::Pending) {
                    continue;
                }
                if (row.key.fields[1] == ack.planFingerprint &&
                    row.key.fields[3] == ack.claimSetFingerprint) {
                    row.state = BusAdmissionClaimState::Open;
                    opened = true;
                }
            }
            if (opened && engine.trace != nullptr && engine.trace->Active()) {
                engine.trace->RecordProjectGenerationInstallAck(ack);
            }
        }
        drained += count;
    }
    return drained;
}

void CancelPreparedProjectGenerationV1(
    Engine& engine, PreparedProjectGenerationV1&& prepared) noexcept {
    if (prepared.Valid() && prepared.engine_ != &engine) return;
    prepared.Reset();
}

void CancelPreparedProjectGenerationV1(
    PreparedProjectGenerationV1&& prepared) noexcept {
    prepared.Reset();
}

bool QueryLiveTopologyOwnerRevisionV1(Engine& engine, std::uint64_t ownerId,
                                      std::uint64_t* outRevision) noexcept {
    if (ownerId == 0) {
        return false;
    }
    for (const auto& owner : engine.busTopologyOwners) {
        if (owner.live && owner.ownerId == ownerId) {
            if (outRevision != nullptr) {
                *outRevision = owner.currentRevision;
            }
            return true;
        }
    }
    return false;
}

bool CurrentWorldBusAdmissionIsOpenV1(Engine& engine,
                                      WorldGeneration generation) noexcept {
    if (generation == 0) {
        return false;
    }
    for (const auto& row : engine.currentWorldBusAdmission) {
        if (row.occupied && row.generation == generation &&
            row.state == BusAdmissionClaimState::Open &&
            row.key.tag == kBusAdmissionTagStudioCurrentWorld &&
            row.key.zero == 0) {
            return true;
        }
    }
    return false;
}

std::uint32_t LiveMaxBusesV1(const Engine& engine) noexcept {
    return engine.BusCapacity();
}

std::uint32_t LiveBusSendSlotsPerBusV1(const Engine& engine) noexcept {
    return engine.config.busSendSlotsPerBus;
}

Engine* ActiveEngineForProjectGenerationV1() noexcept {
    return detail::ActiveEngine();
}

void PumpNullMixerForProjectGenerationV1(Engine& engine,
                                         std::uint32_t blockCount) noexcept {
    if (blockCount == 0) {
        return;
    }
    if (!engine.suspended) {
        if (engine.config.nullDevice) {
            engine.nullBackend.Pump(blockCount);
        }
        return;
    }
    // Suspended NullBackend::Pump is a no-op, and a suspended real device has
    // no callback. Opcode 15 still has to apply and push the install-ack ring
    // so Preview Open can happen before Vn_Resume. Drain commands only: a
    // full RenderBlock would mix against unready workers.
    engine.mixer.DrainQueuedCommands(blockCount);
}

}  // namespace harken
