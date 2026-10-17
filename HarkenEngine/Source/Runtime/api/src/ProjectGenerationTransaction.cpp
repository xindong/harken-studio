#include "ProjectGenerationTransaction.h"

#include "Engine.h"
#include "MonitorCaptureHub.h"
#include "TraceRecorder.h"
#include "HarkenApiInternal.h"

#include "harken/core/CommandQueue.h"
#include "harken/BusDefinitionFingerprintValue.h"
#include "harken/core/MonitorLifecycleSink.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/render/BusTree.h"
#include "harken/render/DynamicRoute.h"
#include "harken/render/TopologyPlan.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <map>
#include <new>
#include <vector>

namespace harken {

TopologyOwnerRevisionDispositionV1 ClassifyTopologyOwnerRevisionV1(
    const BusTopologyOwnerRecord* current, std::uint64_t baseRevision,
    std::uint64_t revision, std::uint64_t digestLow, std::uint64_t digestHigh) noexcept {
    if (revision == 0) return TopologyOwnerRevisionDispositionV1::Conflict;
    const auto currentRevision = current != nullptr && current->live ? current->currentRevision : 0;
    if (revision == currentRevision) {
        return current != nullptr && current->live && baseRevision == current->predecessorRevision &&
            digestLow == current->digestLow && digestHigh == current->digestHigh
            ? TopologyOwnerRevisionDispositionV1::Retry : TopologyOwnerRevisionDispositionV1::Conflict;
    }
    return revision > currentRevision && baseRevision == currentRevision
        ? TopologyOwnerRevisionDispositionV1::Advance : TopologyOwnerRevisionDispositionV1::Conflict;
}

std::uint64_t NeutralMasterFingerprintV1(const Engine& engine) noexcept {
    auto hash = busfingerprint::kOffsetBasis;
    const auto byte = [&](std::uint8_t value) { hash = busfingerprint::Step(hash, value); };
    const std::uint32_t fields[]{1, engine.config.mixChannelLayout,
        engine.config.deviceChannelLayout, engine.config.fxSlotsPerBus, 0, 0, 0, 0, 0};
    for (const auto field : fields) busfingerprint::AppendU32(byte, field);
    float matrix[64]{};
    DefaultDownmixMatrix(engine.config.mixChannelLayout, engine.config.deviceChannelLayout, matrix);
    for (const auto value : matrix) busfingerprint::AppendU32(byte, busfingerprint::CanonicalFloatBits(value));
    return busfingerprint::Finalize(hash);
}

bool ValidateTopologyPlanVolumesV1(const TopologyPlanView& plan) noexcept {
    if (!plan.valid) return false;
    if (plan.busVolumeApply == nullptr) return true;
    const auto count = plan.slotCount != 0 ? plan.slotCount : plan.orderCount;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto mask = plan.busVolumeApply[i];
        if ((mask & kApplyBusVolume) != 0 && (plan.preparedBusVolumeDb == nullptr ||
            !TryVolumeGainFromDb(plan.preparedBusVolumeDb[i]))) return false;
        if ((mask & kApplyBusOutputVolume) != 0 && (plan.preparedBusOutputVolumeDb == nullptr ||
            !TryVolumeGainFromDb(plan.preparedBusOutputVolumeDb[i]))) return false;
    }
    return true;
}

bool MasterHasAuthoredClaimV1(const Engine& engine) noexcept {
    for (const auto& holder : engine.previewMasterHolders) if (holder.occupied) return true;
    return false;
}

bool PreviewOwnsSoleMasterV1(const Engine& engine, std::uint64_t ownerId) noexcept {
    for (const auto& holder : engine.previewMasterHolders) {
        if (holder.occupied && holder.ownerId != ownerId) return false;
    }
    return true;
}

bool CanResetNeutralBusPropertiesV1(const Engine& engine, std::uint32_t slot) noexcept {
    if (slot >= engine.config.maxBuses || engine.busSoloActiveCount != 0) return false;
    if (slot >= engine.buses.BusCount() || !engine.buses.AliveAt(slot)) return true;
    const auto generation = engine.buses.GenerationAt(slot);
    if (engine.busOwnerCookies[slot] != 0) return false;
    for (std::uint32_t target = 0; target < kBusTargetCount; ++target) {
        if (engine.busTargets.ForeignClaim(slot, generation, target)) return false;
    }
    for (std::uint32_t effect = 0; effect < engine.config.fxSlotsPerBus; ++effect) {
        if (engine.busEffectOwnerCookies[slot][effect] != 0 ||
            engine.effectParamTargets.ClaimedInSlot(EffectParamTargetStore::Scope::Bus,
                slot, generation, effect)) return false;
    }
    if (slot != 0) {
        for (std::uint32_t child = 1; child < engine.buses.BusCount(); ++child) {
            if (engine.buses.AliveAt(child) && engine.buses.ParentOf(child) == slot &&
                engine.busTopologyOwnerIds[child] != engine.busTopologyOwnerIds[slot]) return false;
        }
    }
    bool referenced = false;
    const auto targets = [&](const auto& attachment) {
        return attachment.busIndex == slot && (attachment.scopeKind == kTargetScopeBus ||
            attachment.scopeKind == VN_TARGET_SCOPE_BUS_EFFECT_PARAM);
    };
    engine.bindings.ForEachAttachment([&](const auto& attachment, const auto&) { referenced |= targets(attachment); });
    engine.bindings.ForEachProbeAttachment([&](const auto& attachment, const auto&) {
        referenced |= targets(attachment) && attachment.targetGeneration == generation;
    });
    for (std::uint32_t i = 0; i < engine.meterDetectors.Capacity(); ++i) {
        const auto* meter = engine.meterDetectors.RowAt(i);
        if ((meter->state == MeterSystem::State::Installed || meter->state == MeterSystem::State::Withdrawing) &&
            meter->sourceBusIndex == slot && meter->identity.sourceBusGeneration == generation) referenced = true;
    }
    engine.dynamicRoutes.ForEachDefinition([&](const auto& route) {
        referenced |= route.targetBusIndex == slot ||
            (route.sourceKind == VN_DYNAMIC_SOURCE_BUS && route.sourceIndex == slot && route.sourceGeneration == generation);
    });
    return !referenced;
}

void ResetNeutralBusPropertiesV1(Engine& engine, std::uint32_t slot) noexcept {
    const auto generation = engine.buses.GenerationAt(slot);
    engine.busTargets.InitializeBusGeneration(slot, generation);
    engine.effectParamTargets.ClearObject(EffectParamTargetStore::Scope::Bus, slot, generation);
    engine.busEffectSlotTypes[slot].fill(kEffectTypeNone);
    engine.busEffectInstanceRefs[slot].fill(EffectInstanceRef{});
    engine.busEffectOwnerCookies[slot].fill(0);
    engine.busEffectSlotBypass[slot].fill(0);
    engine.busMuted[slot] = 0;
    engine.busSoloed[slot] = 0;
    engine.busFaderAfterSlots[slot] = engine.config.fxSlotsPerBus;
    const auto layout = slot == 0 ? engine.config.deviceChannelLayout : engine.config.mixChannelLayout;
    engine.busOutputLayout[slot] = layout;
    DefaultDownmixMatrix(engine.config.mixChannelLayout, layout,
        engine.busDownmixCoeffs.data() + static_cast<std::size_t>(slot) * 64);
}

bool CanResetVolumeOnlyNonVolumePropertiesV2(const Engine& engine,
                                              std::uint32_t slot,
                                              std::uint64_t allowedOwnerCookie) noexcept {
    static_assert(kBusTargetCount == 2);
    if (slot >= engine.config.maxBuses) return false;
    if (slot >= engine.buses.BusCount() || !engine.buses.AliveAt(slot)) return true;
    const auto generation = engine.buses.GenerationAt(slot);
    if ((engine.busOwnerCookies[slot] != 0 &&
         engine.busOwnerCookies[slot] != allowedOwnerCookie) ||
        engine.busSoloActiveCount < static_cast<std::uint32_t>(engine.busSoloed[slot] != 0))
        return false;
    for (std::uint32_t effect = 0; effect < engine.config.fxSlotsPerBus; ++effect) {
        if (engine.busEffectOwnerCookies[slot][effect] != 0 ||
            engine.effectParamTargets.ClaimedInSlot(EffectParamTargetStore::Scope::Bus,
                slot, generation, effect)) return false;
    }
    if (slot != 0) {
        for (std::uint32_t child = 1; child < engine.buses.BusCount(); ++child) {
            if (engine.buses.AliveAt(child) && engine.buses.ParentOf(child) == slot &&
                engine.busTopologyOwnerIds[child] != engine.busTopologyOwnerIds[slot]) return false;
        }
    }
    bool referenced = false;
    engine.bindings.ForEachAttachment([&](const auto& attachment, const auto&) {
        referenced |= attachment.busIndex == slot &&
            attachment.scopeKind == VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    });
    engine.bindings.ForEachProbeAttachment([&](const auto& attachment, const auto&) {
        referenced |= attachment.busIndex == slot &&
            attachment.scopeKind == VN_TARGET_SCOPE_BUS_EFFECT_PARAM &&
            attachment.targetGeneration == generation;
    });
    for (std::uint32_t i = 0; i < engine.meterDetectors.Capacity(); ++i) {
        const auto* meter = engine.meterDetectors.RowAt(i);
        if ((meter->state == MeterSystem::State::Installed ||
             meter->state == MeterSystem::State::Withdrawing) &&
            meter->sourceBusIndex == slot && meter->identity.sourceBusGeneration == generation)
            referenced = true;
    }
    engine.dynamicRoutes.ForEachDefinition([&](const auto& route) {
        referenced |= route.targetBusIndex == slot ||
            (route.sourceKind == VN_DYNAMIC_SOURCE_BUS && route.sourceIndex == slot &&
             route.sourceGeneration == generation);
    });
    return !referenced;
}

void ResetVolumeOnlyNonVolumePropertiesV2(Engine& engine,
                                           std::uint32_t slot) noexcept {
    const auto generation = engine.buses.GenerationAt(slot);
    engine.effectParamTargets.ClearObject(EffectParamTargetStore::Scope::Bus, slot, generation);
    engine.busEffectSlotTypes[slot].fill(kEffectTypeNone);
    engine.busEffectInstanceRefs[slot].fill(EffectInstanceRef{});
    engine.busEffectOwnerCookies[slot].fill(0);
    engine.busEffectSlotBypass[slot].fill(0);
    engine.busMuted[slot] = 0;
    if (engine.busSoloed[slot] != 0) --engine.busSoloActiveCount;
    engine.busSoloed[slot] = 0;
    engine.busFaderAfterSlots[slot] = engine.config.fxSlotsPerBus;
    const auto layout = slot == 0 ? engine.config.deviceChannelLayout : engine.config.mixChannelLayout;
    engine.busOutputLayout[slot] = layout;
    DefaultDownmixMatrix(engine.config.mixChannelLayout, layout,
        engine.busDownmixCoeffs.data() + static_cast<std::size_t>(slot) * 64);
}

bool CanRetireTopologyBusV1(const Engine& engine, BusSlotRef ref, std::uint64_t revision) noexcept {
    return ref.Valid() && ref.index != 0 && ref.index < engine.busRetirements.size() &&
        engine.buses.AliveAt(ref.index) && engine.buses.GenerationAt(ref.index) == ref.generation &&
        revision != 0 && revision > engine.buses.InstalledGraphRevision() &&
        engine.buses.RetirementRevisionAt(ref.index) == 0 && engine.busRetirements[ref.index].revision == 0;
}

void CommitRetireTopologyBusV1(Engine& engine, BusSlotRef ref, std::uint64_t revision) noexcept {
    assert(CanRetireTopologyBusV1(engine, ref, revision));
    const auto ownerId = engine.busTopologyOwnerIds[ref.index];
    const auto cookie = engine.busOwnerCookies[ref.index];
    const bool held = engine.buses.HoldRetiredSlot(ref, revision);
    assert(held);
    (void)held;
    engine.busRetirements[ref.index] = {ref.generation, revision, ownerId, cookie};
}

void ClearRetiredBusPropertiesV1(Engine& engine, BusSlotRef ref) noexcept {
    assert(ref.index < engine.busRetirements.size() &&
        engine.busRetirements[ref.index].generation == ref.generation &&
        engine.buses.RetirementRevisionAt(ref.index) != 0);
    engine.busTargets.RetireBusGeneration(ref.index, ref.generation);
    engine.effectParamTargets.ClearObject(EffectParamTargetStore::Scope::Bus, ref.index, ref.generation);
    engine.busEffectSlotTypes[ref.index].fill(kEffectTypeNone);
    engine.busEffectInstanceRefs[ref.index].fill(EffectInstanceRef{});
    engine.busEffectOwnerCookies[ref.index].fill(0);
    engine.busEffectSlotBypass[ref.index].fill(0);
    engine.busFaderAfterSlots[ref.index] = engine.config.fxSlotsPerBus;
    engine.busMuted[ref.index] = 0;
    engine.busSoloed[ref.index] = 0;
}

bool HasTopologyBusConsumersV1(Engine& engine, BusSlotRef ref) noexcept {
    if (!ref.Valid()) return false;
    const auto matches = [&](std::uint32_t index, std::uint32_t generation) {
        return index == ref.index && generation == ref.generation;
    };
    const auto sendsMatch = [&](const auto& sends) {
        for (const auto& send : sends) {
            if (send.active && matches(send.targetBusIndex, send.targetBusGeneration)) return true;
        }
        return false;
    };
    for (std::size_t i = 0; i < engine.voicePlayFacts.size(); ++i) {
        const auto& facts = engine.voicePlayFacts[i];
        if (!facts.valid || facts.renderHandle == 0 || engine.voices.Resolve(facts.renderHandle) == nullptr) continue;
        if (matches(facts.busIndex, facts.busGeneration) ||
            (i < engine.voiceSendConfig.size() && sendsMatch(engine.voiceSendConfig[i]))) return true;
    }
    for (const auto& identity : engine.timelineVoiceInstallIdentities) {
        if (identity.renderHandle != 0 && engine.voices.Resolve(identity.renderHandle) != nullptr &&
            (matches(identity.busIndex, identity.busGeneration) || sendsMatch(identity.voiceSends))) return true;
    }
    for (const auto& pending : engine.pendingStarts) {
        if (pending.active && (matches(pending.busIndex, pending.busGeneration) || sendsMatch(pending.voiceSends))) return true;
    }
    for (const auto& parked : engine.parkRecords) {
        if (parked.active && (matches(parked.facts.busIndex, parked.facts.busGeneration) || sendsMatch(parked.voiceSends))) return true;
    }
    for (std::uint32_t i = 0; i < engine.meterDetectors.Capacity(); ++i) {
        const auto* meter = engine.meterDetectors.RowAt(i);
        if ((meter->state == MeterSystem::State::Installed || meter->state == MeterSystem::State::Withdrawing) &&
            matches(meter->sourceBusIndex, meter->identity.sourceBusGeneration)) return true;
    }
    return false;
}

void CommitMigratedLogicVoiceMirrorsV1(Engine& engine, const std::uint32_t* migrationTargets,
    const std::uint32_t* generations, const std::uint8_t* alive, std::uint32_t slotCount) noexcept {
    if (migrationTargets == nullptr || generations == nullptr || alive == nullptr) return;
    const auto stale = [&](std::uint32_t index, std::uint32_t generation) noexcept {
        return index >= slotCount || alive[index] == 0 ||
            (generation != 0 && generations[index] != generation);
    };
    const auto migrateDry = [&](std::uint32_t& index, std::uint32_t& generation) noexcept {
        if (!stale(index, generation)) return;
        const auto target = index < slotCount ? migrationTargets[index] : 0u;
        index = target < slotCount ? target : 0u;
        generation = generations[index];
    };
    const auto clearStaleSend = [&](VoiceSendConfig& send, bool accountLive) noexcept {
        if (!send.active || !stale(send.targetBusIndex, send.targetBusGeneration)) return;
        send = {};
        if (accountLive && engine.voiceSendActive != 0) --engine.voiceSendActive;
    };
    for (auto& perVoice : engine.voiceSendConfig) {
        for (auto& send : perVoice) clearStaleSend(send, true);
    }
    for (auto& pending : engine.pendingStarts) {
        migrateDry(pending.busIndex, pending.busGeneration);
        for (auto& send : pending.voiceSends) clearStaleSend(send, false);
    }
    for (auto& facts : engine.voicePlayFacts) migrateDry(facts.busIndex, facts.busGeneration);
    for (auto& parked : engine.parkRecords) {
        migrateDry(parked.facts.busIndex, parked.facts.busGeneration);
        for (auto& send : parked.voiceSends) clearStaleSend(send, false);
    }
    for (auto& identity : engine.timelineVoiceInstallIdentities) {
        if (identity.renderHandle == 0) continue;
        migrateDry(identity.busIndex, identity.busGeneration);
        for (auto& send : identity.voiceSends) clearStaleSend(send, false);
    }
}

std::uint32_t DrainTopologyRetirementsV1(Engine& engine) noexcept {
    std::uint32_t released = 0;
    const auto installed = engine.buses.InstalledGraphRevision();
    for (std::uint32_t slot = 1; slot < engine.buses.BusCount(); ++slot) {
        auto& row = engine.busRetirements[slot];
        if (row.revision == 0 || installed < row.revision || engine.buses.AliveAt(slot) ||
            row.generation != engine.buses.GenerationAt(slot) ||
            row.revision != engine.buses.RetirementRevisionAt(slot) ||
            row.ownerId != engine.busTopologyOwnerIds[slot] || row.cookie != engine.busOwnerCookies[slot]) continue;
        const BusSlotRef ref{slot, row.generation};
        if (HasTopologyBusConsumersV1(engine, ref)) continue;
        if (engine.buses.ReleaseRetiredSlot(ref, row.revision)) {
            engine.busOwnerCookies[slot] = 0;
            engine.busTopologyOwnerIds[slot] = 0;
            row = {};
            ++released;
        }
    }
    return released;
}

namespace {

bool ProjectCheckpoint(detail::LogicAllocSite site) noexcept {
    try {
        detail::LogicAllocCheckpoint(site);
        return true;
    } catch (const std::bad_alloc&) {
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
    const bool liveBusUpdate = (vintage == 3 || vintage == 4) &&
        image.Valid() && image.Idempotent();
    return CurrentWorldProtectedStateBlocked(engine) ||
        (HasCurrentWorldPublicationConsumers(engine) && !liveBusUpdate);
}

} // namespace

std::uint32_t SelectPreviewOwnerDestination(const Engine& engine, std::uint64_t ownerId) noexcept {
    const auto reserved = [&](std::uint32_t destination) {
        for (std::size_t i = 0; i < engine.preparedOwnerClaims.size(); ++i) {
            if (engine.preparedOwnerClaims[i].inUse != 0 && engine.preparedPreviewOwnerSlots[i] == destination) return true;
        }
        return false;
    };
    for (std::uint32_t i = 0; i < engine.busTopologyOwners.size(); ++i) {
        if (engine.busTopologyOwners[i].live && engine.busTopologyOwners[i].ownerId == ownerId)
            return reserved(i) ? UINT32_MAX : i;
    }
    for (std::uint32_t i = 0; i < engine.busTopologyOwners.size(); ++i) {
        if (!engine.busTopologyOwners[i].live && !reserved(i)) return i;
    }
    return UINT32_MAX;
}

bool SameOwnerRow(const BusTopologyOwnerRecord& a, const BusTopologyOwnerRecord& b) noexcept {
    return a.live == b.live && a.ownerId == b.ownerId && a.currentRevision == b.currentRevision &&
        a.predecessorRevision == b.predecessorRevision && a.digestLow == b.digestLow && a.digestHigh == b.digestHigh;
}

bool SameHolderRow(const PreviewMasterHolderV1& a, const PreviewMasterHolderV1& b) noexcept {
    return a.occupied == b.occupied && a.ownerId == b.ownerId && a.revision == b.revision && a.fingerprint == b.fingerprint;
}

bool PreviewMasterCompatible(const Engine& engine, std::uint64_t fingerprint) noexcept {
    for (const auto& holder : engine.previewMasterHolders) {
        if (holder.occupied && holder.fingerprint != fingerprint) return false;
    }
    return true;
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

std::uint32_t FindAssignedSlot(const SlotPlanV1& plan, VnBusId id) noexcept {
    if (id == VN_BUS_MASTER) return 0;
    for (const auto& row : plan.assignments) {
        if (row.id == id && row.slot != 0xFFFFFFFFu) {
            return row.slot;
        }
    }
    return 0xFFFFFFFFu;
}

bool BuildSlotPlanFromProspective(Engine& engine,
                                  const ProspectiveTopologyView* prospective,
                                  std::uint64_t ownerId,
                                  SlotPlanV1& out) {
    out = SlotPlanV1{};
    const std::uint32_t capacity = engine.config.maxBuses;
    const std::uint32_t sendSlots = engine.config.busSendSlotsPerBus;
    if (capacity == 0 || sendSlots == 0) {
        return false;
    }
    out.ownerId = ownerId;
    out.sendSlotsPerBus = sendSlots;
    out.reserved.assign(capacity, 0);
    out.alive.assign(capacity, 0);
    out.busIds.assign(capacity, VnBusId{});
    out.generations.assign(capacity, 0);
    out.parents.assign(capacity, 0);
    out.sendTargets.assign(static_cast<std::size_t>(capacity) * sendSlots,
                           kEmptySendTarget);
    out.sendLevels.assign(static_cast<std::size_t>(capacity) * sendSlots, 0.0f);
    out.sendLevelApply.assign(static_cast<std::size_t>(capacity) * sendSlots, 0);

    const std::uint32_t masterGeneration = engine.buses.GenerationAt(0) == 0
                                               ? 1u
                                               : engine.buses.GenerationAt(0);
    SlotAssignmentV1 master{};
    master.id = VN_BUS_MASTER;
    master.slot = 0;
    master.generation = masterGeneration;
    master.retain = 1;
    out.assignments.push_back(master);
    out.reserved[0] = 1;
    out.alive[0] = 1;
    out.busIds[0] = VN_BUS_MASTER;
    out.generations[0] = masterGeneration;
    out.parents[0] = 0;

    std::vector<VnBusId> authored;
    if (prospective != nullptr) {
        for (std::uint32_t i = 0; i < prospective->busCount; ++i) {
            if (prospective->alive == nullptr || prospective->alive[i] == 0) {
                continue;
            }
            const VnBusId id = prospective->busIds != nullptr
                                   ? prospective->busIds[i]
                                   : VnBusId{};
            if (id == VnObjectId{}) {
                return false;
            }
            authored.push_back(id);
        }
    }
    std::sort(authored.begin(), authored.end());
    for (std::size_t i = 1; i < authored.size(); ++i) {
        if (authored[i] == authored[i - 1]) {
            return false;
        }
    }

    const auto allocateCreateSlot = [&]() -> std::uint32_t {
        for (std::uint32_t s = 1; s < capacity; ++s) {
            if (out.reserved[s] != 0) {
                continue;
            }
            if (engine.buses.QuarantinedAt(s)) {
                continue;
            }
            if (engine.buses.NextGenerationForSlot(s) == 0) {
                continue;
            }
            if (s < engine.buses.BusCount() && engine.buses.AliveAt(s)) {
                continue;
            }
            return s;
        }
        return 0xFFFFFFFFu;
    };

    for (VnBusId id : authored) {
        if (id == VN_BUS_MASTER) {
            continue;
        }
        SlotAssignmentV1 row{};
        row.id = id;
        const std::uint32_t live = engine.buses.FindIndex(id);
        if (live != kInvalidBusIndex && live != 0 && engine.buses.AliveAt(live)) {
            if (out.reserved[live] != 0) {
                return false;
            }
            row.slot = live;
            row.generation = engine.buses.GenerationAt(live);
            row.retain = 1;
        } else {
            const std::uint32_t slot = allocateCreateSlot();
            if (slot == 0xFFFFFFFFu) {
                return false;
            }
            row.slot = slot;
            row.generation = engine.buses.NextGenerationForSlot(slot);
            row.create = 1;
        }
        if (row.generation == 0) {
            return false;
        }
        out.reserved[row.slot] = 1;
        out.alive[row.slot] = 1;
        out.busIds[row.slot] = id;
        out.generations[row.slot] = row.generation;
        out.assignments.push_back(row);
    }

    std::uint32_t highWater = 0;
    for (std::uint32_t s = 0; s < capacity; ++s) {
        if (out.reserved[s] != 0 && s + 1u > highWater) {
            highWater = s + 1u;
        }
    }
    if (highWater == 0) {
        highWater = 1;
    }
    out.busCount = highWater;

    const auto parentIdOf = [&](VnBusId id, bool* ok) -> VnBusId {
        *ok = false;
        if (id == VN_BUS_MASTER) {
            *ok = true;
            return {};
        }
        if (prospective == nullptr || prospective->busIds == nullptr ||
            prospective->parents == nullptr) {
            return {};
        }
        for (std::uint32_t i = 0; i < prospective->busCount; ++i) {
            if (prospective->alive != nullptr && prospective->alive[i] == 0) {
                continue;
            }
            if (prospective->busIds[i] != id) {
                continue;
            }
            const std::uint32_t parentIndex = prospective->parents[i];
            if (parentIndex == i || parentIndex == kPlanParentUnset ||
                parentIndex >= prospective->busCount) {
                return {};
            }
            *ok = true;
            return prospective->busIds[parentIndex];
        }
        return {};
    };

    for (const auto& row : out.assignments) {
        if (row.slot == 0) {
            continue;
        }
        bool parentOk = false;
        const VnBusId parentId = parentIdOf(row.id, &parentOk);
        if (!parentOk) {
            return false;
        }
        const std::uint32_t parentSlot =
            (parentId == VnObjectId{} || parentId == VN_BUS_MASTER)
                ? 0u
                : FindAssignedSlot(out, parentId);
        if (parentSlot == 0xFFFFFFFFu || parentSlot == row.slot ||
            parentSlot >= out.busCount || out.alive[parentSlot] == 0) {
            return false;
        }
        out.parents[row.slot] = parentSlot;
    }

    if (prospective != nullptr && prospective->sendTargets != nullptr) {
        std::vector<std::uint32_t> used(out.busCount, 0);
        for (std::uint32_t from = 0; from < prospective->busCount; ++from) {
            if (prospective->alive != nullptr && prospective->alive[from] == 0) {
                continue;
            }
            const VnBusId fromId = prospective->busIds != nullptr
                                       ? prospective->busIds[from]
                                       : VnBusId{};
            const std::uint32_t fromSlot = FindAssignedSlot(out, fromId);
            if (fromSlot == 0xFFFFFFFFu || fromSlot >= out.busCount) {
                return false;
            }
            for (std::uint32_t s = 0; s < sendSlots; ++s) {
                const std::uint32_t to = prospective->sendTargets[
                    static_cast<std::size_t>(from) * sendSlots + s];
                if (to == kEmptySendTarget || to == kInvalidBusIndex) {
                    continue;
                }
                if (to >= prospective->busCount ||
                    prospective->busIds == nullptr) {
                    return false;
                }
                const VnBusId toId = prospective->busIds[to];
                const std::uint32_t toSlot = FindAssignedSlot(out, toId);
                if (toSlot == 0xFFFFFFFFu || toSlot == fromSlot ||
                    toSlot >= out.busCount || out.alive[toSlot] == 0) {
                    return false;
                }
                if (used[fromSlot] >= sendSlots) {
                    return false;
                }
                const std::size_t srcFlat =
                    static_cast<std::size_t>(from) * sendSlots + s;
                const std::size_t flat =
                    static_cast<std::size_t>(fromSlot) * sendSlots +
                    used[fromSlot];
                out.sendTargets[flat] = toSlot;
                if (prospective->sendLevels != nullptr) {
                    out.sendLevels[flat] = prospective->sendLevels[srcFlat];
                }
                if (prospective->sendLevelApply != nullptr) {
                    out.sendLevelApply[flat] =
                        prospective->sendLevelApply[srcFlat];
                }
                ++used[fromSlot];
            }
        }
    }
    out.valid = true;
    return true;
}

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
    };
    struct Node { VnBusId parent{}; bool owned = false; std::vector<Edge> edges; };
    const auto capacity = engine.config.maxBuses;
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
    for (std::uint32_t from = 0; from < oldCount; ++from) {
        if (!engine.buses.AliveAt(from)) continue;
        for (std::uint32_t slot = 0; slot < sendSlots; ++slot) {
            const auto flat = static_cast<std::size_t>(from) * sendSlots + slot;
            const auto target = engine.busSendAdj[flat];
            if (target == kEmptySendTarget || engine.busSendTopologyOwnerIds[flat] == ownerId) continue;
            if (target >= oldCount || !engine.buses.AliveAt(target)) return false;
            const auto fromId = engine.buses.IdAt(from);
            const auto toId = engine.buses.IdAt(target);
            if (!graph.contains(fromId) || !graph.contains(toId)) return false;
            float linear = 0.0f;
            if (!TryVolumeGainFromDb(engine.busSendLevelDb[flat], &linear)) return false;
            graph.at(fromId).edges.push_back({toId, slot, engine.busSendLevelDb[flat], linear, 0, 0,
                engine.busSendTopologyOwnerIds[flat], engine.busSendOwnerCookies[flat]});
        }
    }
    if (incoming->sendTargets != nullptr) {
        for (std::uint32_t from = 0; from < incoming->busCount; ++from) {
            if (!incoming->alive[from]) continue;
            auto& edges = graph.at(incoming->busIds[from]).edges;
            const auto oldFrom = engine.buses.FindIndex(incoming->busIds[from]);
            for (std::uint32_t i = 0; i < sendSlots; ++i) {
                const auto src = static_cast<std::size_t>(from) * sendSlots + i;
                const auto target = incoming->sendTargets[src];
                if (target == kEmptySendTarget) continue;
                if (target >= incoming->busCount || !incoming->alive[target]) return false;
                const auto targetId = incoming->busIds[target];
                if (!graph.contains(targetId) || std::any_of(edges.begin(), edges.end(),
                    [&](const Edge& edge) { return edge.target == targetId; })) return false;
                std::uint32_t chosen = UINT32_MAX;
                float db = 0.0f;
                bool retained = false;
                if (oldFrom != kInvalidBusIndex) {
                    for (std::uint32_t s = 0; s < sendSlots; ++s) {
                        const auto flat = static_cast<std::size_t>(oldFrom) * sendSlots + s;
                        const auto oldTarget = engine.busSendAdj[flat];
                        if (engine.busSendTopologyOwnerIds[flat] == ownerId && oldTarget < oldCount &&
                            engine.buses.IdAt(oldTarget) == targetId &&
                            std::none_of(edges.begin(), edges.end(), [&](const Edge& e) { return e.slot == s; })) {
                            chosen = s;
                            db = engine.busSendLevelDb[flat];
                            retained = true;
                            break;
                        }
                    }
                }
                for (std::uint32_t s = 0; chosen == UINT32_MAX && s < sendSlots; ++s) {
                    if (std::none_of(edges.begin(), edges.end(), [&](const Edge& e) { return e.slot == s; })) chosen = s;
                }
                if (chosen == UINT32_MAX) return false;
                const bool apply = incoming->sendLevels != nullptr &&
                    (incoming->sendLevelApply == nullptr || incoming->sendLevelApply[src] != 0);
                float linear = 1.0f;
                if (apply) {
                    linear = incoming->sendLevels[src];
                    if (!std::isfinite(linear) || linear < 0.0f) return false;
                    db = incoming->sendLevelsDb != nullptr
                        ? incoming->sendLevelsDb[src]
                        : (linear == 0.0f ? -160.0f : 20.0f * std::log10(linear));
                    if (!std::isfinite(db) || (incoming->sendLevelsDb != nullptr &&
                        (db < -96.0f || db > 24.0f))) return false;
                } else if (!TryVolumeGainFromDb(db, &linear)) return false;
                const bool changed = !retained || (apply && db != engine.busSendLevelDb[
                    static_cast<std::size_t>(oldFrom) * sendSlots + chosen]);
                edges.push_back({targetId, chosen, db, linear,
                    static_cast<std::uint8_t>(changed),
                    static_cast<std::uint32_t>(retained && changed ? 20 : 0), ownerId, 0});
            }
        }
    }
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
    out.sendOwnerIds = engine.busSendTopologyOwnerIds;
    out.sendCookies = engine.busSendOwnerCookies;
    out.sendLevelDb = engine.busSendLevelDb;
    out.sendLevelRampMs.assign(static_cast<std::size_t>(capacity) * sendSlots, 0);
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
    for (std::size_t flat = 0; flat < out.sendOwnerIds.size(); ++flat) {
        if (out.sendOwnerIds[flat] == ownerId) {
            out.sendOwnerIds[flat] = 0;
            out.sendCookies[flat] = 0;
            out.sendLevelDb[flat] = 0.0f;
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
            const auto flat = static_cast<std::size_t>(slot) * sendSlots + edge.slot;
            out.sendTargets[flat] = FindAssignedSlot(out, edge.target);
            out.sendLevels[flat] = edge.linear;
            out.sendLevelApply[flat] = edge.apply;
            out.sendLevelRampMs[flat] = edge.rampMs;
            out.sendOwnerIds[flat] = edge.owner;
            out.sendCookies[flat] = edge.cookie;
            out.sendLevelDb[flat] = edge.db;
        }
    }
    return true;
}

struct AuthoredTraceProjectionV1 {
    std::vector<VnBusId> ids;
    std::vector<std::uint32_t> generations;
    std::vector<std::uint32_t> parents;
    std::vector<std::uint8_t> alive;
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
    const auto edgeCount = static_cast<std::size_t>(count) * plan.sendSlotsPerBus;
    out.targets.assign(edgeCount, kEmptySendTarget);
    out.levels.assign(edgeCount, 0.0f);
    out.apply.assign(edgeCount, 0);
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
        std::uint32_t written = 0;
        for (std::uint32_t s = 0; s < plan.sendSlotsPerBus; ++s) {
            const auto source = static_cast<std::size_t>(slot) * plan.sendSlotsPerBus + s;
            const auto target = plan.sendTargets[source];
            if (target == kEmptySendTarget || plan.sendOwnerIds[source] != plan.ownerId) continue;
            if (target >= dense.size() || dense[target] == UINT32_MAX ||
                !std::isfinite(plan.sendLevels[source]) || plan.sendLevels[source] < 0.0f) return false;
            const auto destination = static_cast<std::size_t>(i) * plan.sendSlotsPerBus + written++;
            out.targets[destination] = dense[target];
            out.levels[destination] = plan.sendLevels[source];
            out.apply[destination] = plan.sendLevelApply[source];
        }
    }
    out.view.slotCount = count;
    out.view.slotsPerBus = plan.sendSlotsPerBus;
    out.view.busIds = out.ids.data();
    out.view.generations = out.generations.data();
    out.view.parents = out.parents.data();
    out.view.alive = out.alive.data();
    out.view.sendTargets = out.targets.data();
    out.view.preparedSendLevels = out.levels.data();
    out.view.sendLevelApply = out.apply.data();
    out.view.valid = true;
    return true;
}

void ApplySlotPlanToProspective(SlotPlanV1& plan,
                                ProspectiveTopologyView& view) noexcept {
    view.busCount = plan.busCount;
    view.busIds = plan.busIds.data();
    view.generations = plan.generations.data();
    view.parents = plan.parents.data();
    view.alive = plan.alive.data();
    view.sendTargets = plan.sendTargets.data();
    view.sendLevels = plan.sendLevels.data();
    view.sendLevelApply = plan.sendLevelApply.data();
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
                               bool volumeOnly = false) noexcept {
    for (std::uint32_t slot = 0; slot < plan.busCount; ++slot) {
        const bool removed = slot < engine.buses.BusCount() && engine.buses.AliveAt(slot) && !plan.alive[slot];
        if (removed && !CanResetNeutralBusPropertiesV1(engine, slot)) return false;
        if (plan.neutralSlots[slot] != 0 &&
            !(volumeOnly ? CanResetVolumeOnlyNonVolumePropertiesV2(engine, slot)
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

bool SlotPlanMatchesLive(const Engine& engine, const SlotPlanV1& plan) noexcept {
    if (plan.preservesForeign &&
        (plan.sourceGraphRevision != engine.acceptedBusTopologyRevision ||
         plan.sourceBusCount != engine.buses.BusCount())) return false;
    if (!plan.valid || plan.busCount == 0 ||
        plan.busCount > engine.config.maxBuses) {
        return false;
    }
    if (plan.busIds.empty() || plan.alive.empty() || plan.parents.empty()) {
        return false;
    }
    if (plan.busIds[0] != VN_BUS_MASTER || plan.alive[0] == 0 ||
        plan.parents[0] != 0) {
        return false;
    }
    if (engine.buses.FindIndex(VN_BUS_MASTER) != 0 ||
        !engine.buses.AliveAt(0)) {
        return false;
    }
    for (const auto& row : plan.assignments) {
        if (row.slot >= plan.busCount || row.id == VnObjectId{} || row.generation == 0) {
            return false;
        }
        if (row.retain != 0) {
            if (!engine.buses.AliveAt(row.slot) ||
                engine.buses.IdAt(row.slot) != row.id ||
                engine.buses.GenerationAt(row.slot) != row.generation) {
                return false;
            }
        }
        if (row.create != 0) {
            if (engine.buses.QuarantinedAt(row.slot) ||
                engine.buses.NextGenerationForSlot(row.slot) != row.generation) {
                return false;
            }
            if (row.slot < engine.buses.BusCount() &&
                engine.buses.AliveAt(row.slot) &&
                engine.buses.IdAt(row.slot) == row.id) {
                return false;
            }
        }
        if (row.slot != 0) {
            const std::uint32_t parent = plan.parents[row.slot];
            if (parent >= plan.busCount || plan.alive[parent] == 0 ||
                parent == row.slot) {
                return false;
            }
        }
    }
    return true;
}

void InstallSlotPlanLogic(Engine& engine, const SlotPlanV1& plan) noexcept {
    const std::uint32_t oldCount = engine.buses.BusCount();
    const std::uint32_t capacity = engine.config.maxBuses;
    const std::uint32_t sendSlots = engine.config.busSendSlotsPerBus;
    const std::size_t sendCapacity =
        static_cast<std::size_t>(capacity) * sendSlots;
    for (std::uint32_t i = 1; i < oldCount; ++i) {
        if (!engine.buses.AliveAt(i)) {
            continue;
        }
        const bool keep =
            i < plan.busCount && plan.alive[i] != 0 &&
            plan.busIds[i] == engine.buses.IdAt(i) &&
            plan.generations[i] == engine.buses.GenerationAt(i);
        if (!keep) {
            ResetBusLogicSlot(engine, i, engine.buses.GenerationAt(i), 0);
            engine.buses.MarkLogicDead(i);
            if (i < engine.busTopologyOwnerIds.size()) {
                engine.busTopologyOwnerIds[i] = 0;
            }
            if (i < engine.busOwnerCookies.size()) {
                engine.busOwnerCookies[i] = 0;
            }
        }
    }
    for (const auto& row : plan.assignments) {
        if (row.create == 0 || row.slot == 0 || row.slot >= plan.busCount ||
            plan.alive[row.slot] == 0) {
            continue;
        }
        ResetBusLogicSlot(engine, row.slot, engine.buses.GenerationAt(row.slot),
                          row.generation);
        engine.buses.CommitLogicSlot(row.slot, row.id, plan.parents[row.slot],
                                     row.generation);
        if (row.slot < engine.busTopologyOwnerIds.size()) {
            engine.busTopologyOwnerIds[row.slot] = plan.preservesForeign
                ? plan.busOwnerIds[row.slot] : plan.ownerId;
            if (plan.preservesForeign) engine.busOwnerCookies[row.slot] = plan.busCookies[row.slot];
        }
    }
    for (const auto& row : plan.assignments) {
        if (row.retain == 0 || row.slot == 0 || row.slot >= plan.busCount ||
            plan.alive[row.slot] == 0) {
            continue;
        }
        engine.buses.SetLogicParent(row.slot, plan.parents[row.slot]);
        if (row.slot < engine.busTopologyOwnerIds.size()) {
            engine.busTopologyOwnerIds[row.slot] = plan.preservesForeign
                ? plan.busOwnerIds[row.slot] : plan.ownerId;
            if (plan.preservesForeign) engine.busOwnerCookies[row.slot] = plan.busCookies[row.slot];
        }
    }
    engine.busSendActive = 0;
    for (std::size_t flat = 0; flat < sendCapacity; ++flat) {
        const std::uint32_t target =
            flat < plan.sendTargets.size() ? plan.sendTargets[flat]
                                           : kEmptySendTarget;
        engine.busSendAdj[flat] = target;
        engine.busSendTopologyOwnerIds[flat] = plan.preservesForeign
            ? plan.sendOwnerIds[flat]
            : (target != kEmptySendTarget && target != kInvalidBusIndex ? plan.ownerId : 0);
        engine.busSendOwnerCookies[flat] = plan.preservesForeign ? plan.sendCookies[flat] : 0;
        if (!plan.preservesForeign || plan.sendLevelApply[flat] != 0 ||
            target == kEmptySendTarget) {
            engine.busSendLevelDb[flat] = plan.preservesForeign ? plan.sendLevelDb[flat] : 0.0f;
        }
        if (target != kEmptySendTarget && target != kInvalidBusIndex) {
            ++engine.busSendActive;
        }
    }
    UpdateSendHighWater(engine);
}

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
    if (Valid()) {
        ReleasePreparedSlot(*engine_, preparedPlanIndex_, preparedOwnerSlot_);
    }
    ClearFields();
}

std::uint64_t PreviewBusBindingSourceStamp(const Engine& engine) noexcept {
    std::uint64_t hash = busfingerprint::kOffsetBasis;
    const auto u64 = [&](std::uint64_t value) {
        busfingerprint::AppendU64([&](std::uint8_t byte) {
            hash = busfingerprint::Step(hash, byte);
        }, value);
    };
    engine.bindings.ForEachBusAttachment([&](const auto& attachment, const auto& definition) {
        float input = 0.0f;
        const bool hasValue = !attachment.meterFrozen &&
            engine.params.ScopedVariableValue(nullptr, 0, definition.variableId, &input);
        u64(attachment.bindingId.high);
        u64(attachment.bindingId.low);
        u64(attachment.objectId.high);
        u64(attachment.objectId.low);
        u64(attachment.busIndex);
        u64(attachment.ownerCookie);
        u64(attachment.lastHadValue);
        u64(busfingerprint::CanonicalFloatBits(attachment.lastContribution));
        u64(definition.variableId.high);
        u64(definition.variableId.low);
        u64(definition.targetId);
        u64(definition.revision);
        u64(definition.pendingInstall);
        u64(definition.transitionMs);
        u64(hasValue);
        if (hasValue) {
            u64(busfingerprint::CanonicalFloatBits(input));
            u64(busfingerprint::CanonicalFloatBits(definition.Evaluate(input)));
        }
    });
    return busfingerprint::Finalize(hash);
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
        view.publicationVintage == 4;
    if ((view.publicationVintage != 1 && !volumeOnly) || view.zero != 0 ||
        !TokenIsPaired(view.token) || view.ownerId == 0) {
        result.status = ProjectGenerationPrepareStatusV1::InvalidView;
        return result;
    }
    if (volumeOnly && (view.prospective == nullptr || view.authoredBusIds == nullptr ||
        view.authoredBusVolumeDb == nullptr || view.authoredBusMakeUpGainDb == nullptr ||
        view.authoredBusOutputVolumeDb == nullptr ||
        view.authoredBusCount != view.prospective->busCount)) {
        result.status = ProjectGenerationPrepareStatusV1::InvalidView;
        return result;
    }
    if (CurrentWorldProtectedStateBlocked(engine) || HasPredecessorDirectWorldPublication(engine)) {
        result.status = ProjectGenerationPrepareStatusV1::Backpressure;
        return result;
    }
    if (engine.variables.CurrentWorldGeneration() == UINT64_MAX) {
        result.status = ProjectGenerationPrepareStatusV1::DeviceError;
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
        !PreviewMasterCompatible(engine, masterFingerprint)) {
        result.status = ProjectGenerationPrepareStatusV1::InvalidView;
        return result;
    }
    const std::uint32_t commandFreeBefore = engine.commands.FreeSlots();
    const std::uint32_t liveInUse = engine.topologyPlans.InUse();
    const std::uint32_t planIndex = engine.preparedTopologyPlans.Acquire();
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
        engine.preparedTopologyPlans.Recycle(planIndex);
        result.status = ProjectGenerationPrepareStatusV1::Capacity;
        return result;
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
    WorldInstallStatus imageStatus = WorldInstallStatus::InvalidWorld;
    std::vector<float> authoredMixDb, authoredOutputDb, effectiveMixDb, effectiveOutputDb;
    std::vector<float> sourceMixDb, sourceOutputDb;
    std::vector<std::uint8_t> projectedVolumeApply;
    std::uint64_t bindingSourceStamp = 0;
    try {
        if (!BuildPreviewSlotPlan(engine, view.prospective, view.ownerId,
                                  slotPlan)) {
            ReleasePreparedSlot(engine, planIndex, ownerSlot);
            result.status = ProjectGenerationPrepareStatusV1::Capacity;
            return result;
        }
        if (!CanApplyPreviewProperties(engine, slotPlan, volumeOnly)) {
            ReleasePreparedSlot(engine, planIndex, ownerSlot);
            result.status = ProjectGenerationPrepareStatusV1::Backpressure;
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
        for (std::uint32_t slot = 0; slot < slotPlan.busCount; ++slot) {
            auto* ramps = engine.preparedTopologyPlans.SendLevelRampMsRow(planIndex, slot);
            for (std::uint32_t send = 0; send < slotPlan.sendSlotsPerBus; ++send)
                ramps[send] = slotPlan.sendLevelRampMs[
                    static_cast<std::size_t>(slot) * slotPlan.sendSlotsPerBus + send];
        }
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
                        view.authoredBusMakeUpGainDb[i] == 0.0f &&
                        view.authoredBusOutputVolumeDb[i] == 0.0f)
                        continue;
                    ReleasePreparedSlot(engine, planIndex, ownerSlot);
                    result.status = ProjectGenerationPrepareStatusV1::InvalidView;
                    return result;
                }
                const float mix = view.authoredBusVolumeDb[i] +
                    view.authoredBusMakeUpGainDb[i];
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
                    kApplyBusNeutralProperties | kApplyBusVolume | kApplyBusOutputVolume;
                engine.preparedTopologyPlans.PreparedBusVolumeDbData(planIndex)[slot] =
                    volumeOnly ? effectiveMixDb[slot] : 0.0f;
                engine.preparedTopologyPlans.PreparedBusOutputVolumeDbData(planIndex)[slot] =
                    volumeOnly ? effectiveOutputDb[slot] : 0.0f;
                engine.preparedTopologyPlans.PreparedBusVolumeRampMsData(planIndex)[slot] = 0;
                continue;
            }
            engine.preparedTopologyPlans.OutputLayoutData(planIndex)[slot] = engine.busOutputLayout[slot];
            std::copy_n(engine.busDownmixCoeffs.data() + static_cast<std::size_t>(slot) * 64, 64,
                engine.preparedTopologyPlans.DownmixCoeffsData(planIndex) + static_cast<std::size_t>(slot) * 64);
        }
        if (engine.topologyPlans.InUse() != liveInUse ||
            engine.commands.FreeSlots() != commandFreeBefore) {
            ReleasePreparedSlot(engine, planIndex, ownerSlot);
            result.status = ProjectGenerationPrepareStatusV1::DeviceError;
            return result;
        }
        imageStatus = engine.variables.PrepareCurrentWorldImageV1(
            view.compiledWorld, image);
        if (imageStatus == WorldInstallStatus::Ok && volumeOnly) {
            bindingSourceStamp = PreviewBusBindingSourceStamp(engine);
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
    } catch (const std::bad_alloc&) {
        ReleasePreparedSlot(engine, planIndex, ownerSlot);
        result.status = ProjectGenerationPrepareStatusV1::Capacity;
        return result;
    } catch (...) {
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
        HasPredecessorDirectWorldPublication(engine)) {
        ReleasePreparedSlot(engine, planIndex, ownerSlot);
        result.status = ProjectGenerationPrepareStatusV1::Backpressure;
        return result;
    }
    if (engine.topologyPlans.InUse() != liveInUse ||
        engine.commands.FreeSlots() != commandFreeBefore) {
        ReleasePreparedSlot(engine, planIndex, ownerSlot);
        result.status = ProjectGenerationPrepareStatusV1::DeviceError;
        return result;
    }

    out.domain_ = engine.projectGenerationReservationDomain;
    out.domainEpoch_ = engine.projectGenerationReservationDomain->epoch;
    out.engine_ = &engine;
    out.occupied_ = true;
    out.idempotent_ = image.Idempotent();
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
        HasPredecessorDirectWorldPublication(engine)) {
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    }
    if (engine.variables.CurrentWorldGeneration() == UINT64_MAX &&
        !prepared.worldImage_.Idempotent()) {
        result.status = ProjectGenerationCommitStatusV1::DeviceError;
        return result;
    }
    if (!prepared.worldImage_.Valid() ||
        !engine.variables.CanCommitPreparedCurrentWorldImageV1(prepared.worldImage_)) {
        result.status = ProjectGenerationCommitStatusV1::InvalidPrepared;
        return result;
    }
    const auto ownerReady = [&]() noexcept {
        const auto slot = prepared.liveOwnerSlot_;
        return slot < engine.busTopologyOwners.size() && slot < engine.previewMasterHolders.size() &&
            SameOwnerRow(engine.busTopologyOwners[slot], prepared.expectedOwner_) &&
            SameHolderRow(engine.previewMasterHolders[slot], prepared.expectedHolder_) &&
            PreviewMasterCompatible(engine, prepared.nextHolder_.fingerprint) &&
            (prepared.slotPlan_.neutralSlots[0] != 0) == PreviewOwnsSoleMasterV1(engine, prepared.slotPlan_.ownerId);
    };
    const auto volumeSourceReady = [&]() noexcept {
        if (prepared.publicationVintage_ != 3 && prepared.publicationVintage_ != 4) return true;
        if (prepared.bindingSourceStamp_ != PreviewBusBindingSourceStamp(engine) ||
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
        !CanApplyPreviewProperties(engine, prepared.slotPlan_,
            (prepared.publicationVintage_ == 3 || prepared.publicationVintage_ == 4))) {
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    }
    const WorldGeneration priorGeneration = engine.variables.CurrentWorldGeneration();
    const WorldGeneration predictedGeneration = prepared.idempotent_ ? priorGeneration : priorGeneration + 1;
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
    } catch (const std::bad_alloc&) {
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    } catch (...) {
        result.status = ProjectGenerationCommitStatusV1::DeviceError;
        return result;
    }
    engine.topologyPlans.DrainRetire();
    const std::uint32_t liveIndex = engine.topologyPlans.Acquire();
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
    } catch (const std::bad_alloc&) {
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    } catch (...) {
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::DeviceError;
        return result;
    }
    CommandQueue::BatchReservation reservation{};
    if (!ProjectCheckpoint(detail::LogicAllocSite::ProjectCommandReserve) ||
        !AdmitSingleLiveCommand(engine) ||
        !engine.commands.ReserveBatch(1, &reservation)) {
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
    if (prepared.publicationVintage_ == 3 || prepared.publicationVintage_ == 4) {
        for (const auto& assignment : prepared.slotPlan_.assignments) {
            const auto slot = assignment.slot;
            if (prepared.slotPlan_.neutralSlots[slot] == 0) continue;
            volumeOverlayReady &= TryVolumeGainFromDb(prepared.effectiveMixDb_[slot]) &&
                TryVolumeGainFromDb(prepared.effectiveOutputDb_[slot]);
        }
    }
    if (!ProjectCheckpoint(detail::LogicAllocSite::ProjectFinalPreflight) || !retirementReady ||
        !volumeOverlayReady || !volumeSourceReady() ||
        !prepared.Valid() || !ownerReady() ||
        CurrentWorldCommitBlocked(engine, prepared.publicationVintage_, prepared.worldImage_) ||
        HasPredecessorDirectWorldPublication(engine) ||
        !SlotPlanMatchesLive(engine, prepared.slotPlan_) ||
        !CanApplyPreviewProperties(engine, prepared.slotPlan_,
            (prepared.publicationVintage_ == 3 || prepared.publicationVintage_ == 4)) ||
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
        if (prepared.publicationVintage_ != 3 && prepared.publicationVintage_ != 4) {
            ResetNeutralBusPropertiesV1(engine, slot);
            continue;
        }
        const bool retained = slot < prepared.slotPlan_.sourceBusCount &&
            assignment.retain != 0;
        if (retained) ResetVolumeOnlyNonVolumePropertiesV2(engine, slot);
        else ResetNeutralBusPropertiesV1(engine, slot);
        engine.busTargets.SetSlot(slot, kBusTargetBusVolume,
            BusTargetStore::kSlotAuthoredIndex, prepared.authoredMixDb_[slot]);
        engine.busTargets.SetSlot(slot, kBusTargetBusOutputVolume,
            BusTargetStore::kSlotAuthoredIndex, prepared.authoredOutputDb_[slot]);
    }

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
    if (!prepared.idempotent_) ReclaimRetiredSelectionMemory(engine);
    engine.variables.CommitWorldProjectionV1(&projection, engine.params, engine.emitters,
                                              engine.bindings, engine.busTargets);
    if (prepared.publicationVintage_ == 3 || prepared.publicationVintage_ == 4) {
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
    if (!prepared.idempotent_) {
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

    CommandRecord command{};
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
        engine.acceptedBusTopologyRevision;
    command.payload.setBusTopologyPlan.batchCommandCount = 1;
    engine.commands.CommitPreparedBatch(&reservation, &command);

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
    return engine.config.maxBuses;
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
