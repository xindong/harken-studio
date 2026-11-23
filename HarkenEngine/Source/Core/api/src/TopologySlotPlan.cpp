#include "TopologySlotPlan.h"

#include "Engine.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "BusBindingSourceStamp.h"
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
#include <set>
#include <span>
#include <vector>

namespace harken {

struct PreparedBusEffectsV1::State {
    struct Row {
        std::uint32_t bus = 0;
        std::uint32_t generation = 0;
        std::uint32_t slot = 0;
        EffectInstanceRef before{};
        std::uint32_t beforeType = kEffectTypeNone, afterType = kEffectTypeNone;
        EffectObjectIdentity beforeIdentity{};
        EffectObjectIdentity afterIdentity{};
        EffectInstanceRef after{};
        std::uint64_t owner = 0;
        std::uint32_t bypassBefore = 0;
        std::uint32_t bypassAfter = 0;
        bool replace = false;
        bool retire = false;
        std::vector<float> originalParameters;
        std::vector<float> originalAuthoredParameters;
        std::vector<std::uint8_t> boundParameters;
        std::vector<float> parameters;
        EffectParamTargetStore::PreparedSeedToken seed;
    };
    Engine* engine = nullptr;
    std::weak_ptr<ProjectGenerationReservationDomainV1> domain;
    std::uint64_t nextOwner = 0;
    bool committed = false;
    bool structureChanged = false;
    bool deferStructureWhilePlaying = false;
    std::vector<Row> rows;
    std::vector<CommandRecord> commands;
    bool Live() const noexcept {
        const auto current = domain.lock();
        return current && current->live && current->liveEngine == engine;
    }
    ~State() {
        if (committed || !Live()) return;
        for (auto& row : rows) {
            engine->effectParamTargets.CancelPreparedInstalledEffectSeed(&row.seed);
            if (row.replace && row.after.type != kEffectTypeNone)
                engine->effectBank.CancelPrepared(row.after);
        }
    }
};

PreparedBusEffectsV1::PreparedBusEffectsV1() = default;
PreparedBusEffectsV1::~PreparedBusEffectsV1() = default;

VnResult PreparedBusEffectsV1::Prepare(Engine& engine, const SlotPlanV1& plan,
    std::span<const PackBusDefinitionEffect> effects,
    std::span<const PackBusDefinitionEffectParameter> parameters,
    std::uint64_t allowedOwner, std::uint64_t nextOwner, bool deferStructureWhilePlaying) {
    state_.reset();
    auto state = std::make_unique<State>();
    state->engine = &engine;
    state->domain = engine.projectGenerationReservationDomain;
    state->nextOwner = nextOwner;
    state->deferStructureWhilePlaying = deferStructureWhilePlaying;
    const bool allowStructure = !deferStructureWhilePlaying ||
        !HasEffectChainPublicationConsumers(engine);
    std::map<std::pair<VnBusId, std::uint32_t>, const PackBusDefinitionEffect*> incoming;
    std::set<VnObjectId> identities;
    std::uint64_t nextParameter = 0;
    for (const auto& effect : effects) {
        const auto* registry = FindEffectRow(effect.effectType);
        if (!registry || effect.descriptorVintage != registry->descriptorVintage ||
            effect.parameterCount != registry->paramCount ||
            effect.parameterFirst != nextParameter ||
            nextParameter + effect.parameterCount > parameters.size() ||
            (effect.flags & ~1u) != 0 || IsNull(effect.effectId) ||
            effect.slot >= engine.config.fxSlotsPerBus ||
            !identities.insert(effect.effectId).second ||
            !incoming.emplace(std::make_pair(effect.busId, effect.slot), &effect).second)
            return VN_INVALID_ARG;
        const auto assigned = FindAssignedSlot(plan, effect.busId);
        if (assigned >= plan.busCount || plan.neutralSlots[assigned] == 0) return VN_INVALID_ARG;
        for (std::uint32_t p = 0; p < effect.parameterCount; ++p) {
            const auto& parameter = parameters[effect.parameterFirst + p];
            if (parameter.index != p || ValidateEffectParam(effect.effectType, p,
                    parameter.value, engine.config.mixRate.SampleRate()) != VN_OK)
                return VN_INVALID_ARG;
        }
        nextParameter += effect.parameterCount;
    }
    if (nextParameter != parameters.size()) return VN_INVALID_ARG;
    for (const auto& removed : plan.removed) {
        for (std::uint32_t slot = 0; slot < engine.config.fxSlotsPerBus; ++slot) {
            const auto before = engine.BusEffectRefs(removed.slot)[slot];
            const auto beforeType = engine.busEffects.Get(removed.slot, slot).type;
            const auto owner = engine.busEffects.Get(removed.slot, slot).owner;
            if ((owner != 0 && owner != allowedOwner) ||
                (beforeType != kEffectTypeNone && (allowedOwner == 0 || owner != allowedOwner)) ||
                engine.effectParamTargets.ClaimedInSlot(EffectParamTargetStore::Scope::Bus,
                    removed.slot, removed.generation, slot) ||
                engine.dynamicRoutes.HasTargetRoutes(removed.slot, slot)) return VN_IN_USE;
            if (beforeType == kEffectTypeNone) continue;
            if (!allowStructure) return VN_BACKPRESSURE;
            state->structureChanged = true;
            state->rows.emplace_back();
            auto& row = state->rows.back();
            row.bus = removed.slot;
            row.generation = removed.generation;
            row.slot = slot;
            row.before = before;
            row.beforeType = beforeType;
            row.beforeIdentity = engine.busEffects.Get(row.bus, slot).identity;
            row.owner = owner;
            row.bypassBefore = engine.BusEffectBypass(removed.slot, slot);
            row.retire = true;
        }
    }
    engine.effectBank.DrainRetire();
    for (const auto& assignment : plan.assignments) {
        const auto bus = assignment.slot;
        if (plan.neutralSlots[bus] == 0) continue;
        const bool retained = assignment.retain != 0;
        if (retained && engine.busFaderAfterSlots[bus] != engine.config.fxSlotsPerBus) {
            CommandRecord command{};
            command.opcode = kOpcodeSetBusFaderPoint;
            command.payload.setBusFaderPoint.busIndex = bus;
            command.payload.setBusFaderPoint.faderAfterSlot = engine.config.fxSlotsPerBus;
            command.payload.setBusFaderPoint.busGeneration = assignment.generation;
            state->commands.push_back(command);
        }
        for (std::uint32_t slot = 0; slot < engine.config.fxSlotsPerBus; ++slot) {
            const auto found = incoming.find({assignment.id, slot});
            const auto* effect = found == incoming.end() ? nullptr : found->second;
            const auto before = retained ? engine.BusEffectRefs(bus)[slot] : EffectInstanceRef{};
            const auto beforeType = retained ? engine.busEffects.Get(bus, slot).type : kEffectTypeNone;
            const auto owner = retained ? engine.busEffects.Get(bus, slot).owner : 0;
            if ((owner != 0 && owner != allowedOwner) ||
                (beforeType != kEffectTypeNone && (allowedOwner == 0 || owner != allowedOwner)) ||
                engine.effectParamTargets.ClaimedInSlot(EffectParamTargetStore::Scope::Bus,
                    bus, assignment.generation, slot) || engine.dynamicRoutes.HasTargetRoutes(bus, slot))
                return VN_IN_USE;
            if (!effect && beforeType == kEffectTypeNone) continue;
            state->rows.emplace_back();
            auto& row = state->rows.back();
            row.bus = bus;
            row.generation = assignment.generation;
            row.slot = slot;
            row.before = before;
            row.beforeType = beforeType;
            row.beforeIdentity = engine.busEffects.Get(row.bus, slot).identity;
            row.owner = owner;
            row.bypassBefore = retained ? engine.BusEffectBypass(bus, slot) : 0;
            row.bypassAfter = effect ? effect->flags & 1u : 0;
            row.afterType = effect ? effect->effectType : kEffectTypeNone;
            if (beforeType != kEffectTypeNone)
                for (std::uint32_t p = 0; p < EffectParamCount(beforeType); ++p) {
                    row.originalParameters.push_back(engine.effectParamTargets.Reduce(
                        EffectParamTargetStore::Scope::Bus, bus, slot, p));
                    row.originalAuthoredParameters.push_back(engine.effectParamTargets.AuthoredBase(
                        EffectParamTargetStore::Scope::Bus, bus, slot, p));
                }
            if (effect)
                for (std::uint32_t p = 0; p < effect->parameterCount; ++p)
                    row.parameters.push_back(parameters[effect->parameterFirst + p].value);
            const bool same = effect && beforeType == effect->effectType &&
                engine.busEffects.Get(bus, slot).identity.effectId == effect->effectId;
            row.replace = !same;
            state->structureChanged |= !same;
            if (!same && !allowStructure) return VN_BACKPRESSURE;
            if (row.bypassBefore != row.bypassAfter) {
                CommandRecord command{};
                command.opcode = kOpcodeSetBusEffectBypass;
                command.payload.setBusEffectBypass.busIndex = bus;
                command.payload.setBusEffectBypass.slot = slot;
                command.payload.setBusEffectBypass.bypass = row.bypassAfter;
                command.payload.setBusEffectBypass.busGeneration = row.generation;
                state->commands.push_back(command);
            }
            if (same) {
                row.after = before;
                row.afterIdentity = row.beforeIdentity;
                auto effective = row.parameters;
                for (std::uint32_t p = 0; p < row.parameters.size(); ++p) {
                    const bool bound = engine.bindings.HasEffectParamBinding(
                        EffectParamTargetStore::Scope::Bus, bus, slot, p, effect->effectType);
                    row.boundParameters.push_back(bound);
                    if (bound) effective[p] = engine.effectParamTargets.LastCommitted(
                        EffectParamTargetStore::Scope::Bus, bus, slot, p);
                }
                for (std::uint32_t p = 0; p < row.parameters.size(); ++p) {
                    if (row.boundParameters[p] || row.parameters[p] == row.originalAuthoredParameters[p]) continue;
                    CommandRecord command{};
                    if (!BuildBusEffectParameterCommandV1(engine, bus, slot, p,
                            effective, effect->effectType, &command)) return VN_INVALID_ARG;
                    state->commands.push_back(command);
                }
            } else if (effect) {
                const auto* registry = FindEffectRow(effect->effectType);
                alignas(VnEffectDesc) std::array<std::uint8_t, 512> descriptor{};
                if (registry->descriptorSize > descriptor.size() ||
                    registry->descriptorSize < sizeof(VnEffectDesc) + row.parameters.size() * sizeof(float))
                    return VN_INVALID_ARG;
                const VnEffectDesc header{registry->descriptorSize, 0, effect->effectId};
                std::memcpy(descriptor.data(), &header, sizeof(header));
                std::memcpy(descriptor.data() + sizeof(header), row.parameters.data(),
                    row.parameters.size() * sizeof(float));
                if (ValidateEffectDesc(effect->effectType, descriptor.data(),
                        engine.config.mixRate.SampleRate()) != VN_OK) return VN_INVALID_ARG;
                row.afterIdentity = {effect->effectId, engine.CurrentProjectId()};
                row.after = engine.effectBank.AcquirePrepared(effect->effectType, row.afterIdentity);
                if (row.after.type == kEffectTypeNone) return VN_NO_EFFECT;
                void* storage = engine.effectBank.StateFor(row.after.type, row.after.poolIndex);
                const auto built = BuildEffect(row.after.type, storage);
                if (built.reset) built.reset(storage);
                ApplyEffectDesc(row.after.type, descriptor.data(), storage);
                if (!engine.effectParamTargets.PrepareInstalledEffectSeed(
                        EffectParamTargetStore::Scope::Bus, bus, row.generation, slot,
                        effect->effectType, row.parameters.data(), effect->parameterCount, &row.seed))
                    return VN_NO_EFFECT;
                CommandRecord command{};
                command.opcode = kOpcodeSetBusEffect;
                command.payload.setBusEffect = {bus, slot, row.after.type,
                    row.after.poolIndex, row.after.generation, kInvalidTopologyPlanIndex,
                    kInvalidDynamicRoutePlanIndex, row.generation};
                state->commands.push_back(command);
            } else {
                CommandRecord command{};
                command.opcode = kOpcodeClearBusEffect;
                command.payload.clearBusEffect = {bus, slot, kInvalidTopologyPlanIndex,
                    kInvalidDynamicRoutePlanIndex, row.generation};
                state->commands.push_back(command);
            }
        }
    }
    state_ = std::move(state);
    return VN_OK;
}

bool PreparedBusEffectsV1::CanCommit() const noexcept {
    if (!state_ || !state_->Live() || state_->committed) return false;
    const auto& engine = *state_->engine;
    if (state_->deferStructureWhilePlaying && state_->structureChanged &&
        HasEffectChainPublicationConsumers(engine)) return false;
    for (const auto& row : state_->rows) {
        const auto& live = engine.BusEffectRefs(row.bus)[row.slot];
        if (live.type != row.before.type || live.poolIndex != row.before.poolIndex ||
            live.generation != row.before.generation ||
            engine.busEffects.Get(row.bus, row.slot).type != row.beforeType ||
            engine.busEffects.Get(row.bus, row.slot).identity.effectId != row.beforeIdentity.effectId ||
            engine.busEffects.Get(row.bus, row.slot).identity.projectId != row.beforeIdentity.projectId ||
            engine.busEffects.Get(row.bus, row.slot).owner != row.owner ||
            engine.BusEffectBypass(row.bus, row.slot) != row.bypassBefore)
            return false;
        for (std::uint32_t p = 0; p < row.originalParameters.size(); ++p)
            if (engine.effectParamTargets.Reduce(EffectParamTargetStore::Scope::Bus,
                    row.bus, row.slot, p) != row.originalParameters[p] ||
                engine.effectParamTargets.AuthoredBase(EffectParamTargetStore::Scope::Bus,
                    row.bus, row.slot, p) != row.originalAuthoredParameters[p]) return false;
    }
    return true;
}

bool PreparedBusEffectsV1::AppendSettingsWrites(std::vector<BusEffectSettings::Write>& writes) const noexcept {
    try {
        if (state_) for (const auto& row : state_->rows)
            if (!row.retire && row.afterType != kEffectTypeNone)
                writes.push_back({row.bus, row.slot, {row.afterType, row.afterIdentity, state_->nextOwner}});
        return true;
    } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::ProjectAdmissionSnapshot); return false; }
}

void PreparedBusEffectsV1::Commit() noexcept {
    auto& engine = *state_->engine;
    for (auto& row : state_->rows) {
        const auto handle = PackedMonitorEffectHandle(row.bus, row.slot);
        if ((row.retire || row.replace) && row.before.type != kEffectTypeNone)
            ObserveMonitorEffectRelease(handle, row.before.generation,
                row.beforeIdentity.effectId, VN_ACTION_TERMINAL_NONE);
        if (row.retire) continue;
        if (row.replace) {
            if (row.after.type != kEffectTypeNone) {
                const bool ready = engine.effectBank.CommitPrepared(row.after);
                assert(ready);
                (void)ready;
                engine.effectParamTargets.CommitPreparedInstalledEffectSeed(&row.seed);
                engine.effectBank.PublishInstalled(row.after);
            } else {
                engine.effectParamTargets.ClearSlot(EffectParamTargetStore::Scope::Bus,
                    row.bus, row.generation, row.slot);
            }
        } else {
            for (std::uint32_t p = 0; p < row.parameters.size(); ++p) {
                if (row.parameters[p] == row.originalAuthoredParameters[p]) continue;
                // A binding removed in this transaction also needs a fold:
                // preparation deliberately emitted no direct command for it.
                if (row.boundParameters[p] || engine.bindings.HasEffectParamBinding(
                        EffectParamTargetStore::Scope::Bus, row.bus, row.slot, p, row.afterType))
                    engine.effectParamTargets.NoteDirectBound(EffectParamTargetStore::Scope::Bus,
                        row.bus, row.slot, p, row.parameters[p]);
                else engine.effectParamTargets.NoteDirect(EffectParamTargetStore::Scope::Bus,
                    row.bus, row.slot, p, row.parameters[p], row.parameters[p]);
            }
        }
        engine.busEffects.Set(row.bus, row.slot, {row.afterType, row.afterIdentity,
            row.afterType == kEffectTypeNone ? 0 : state_->nextOwner});
        engine.SetBusEffectRef(row.bus, row.slot, row.after);
        engine.SetBusEffectBypass(row.bus, row.slot, static_cast<std::uint8_t>(row.bypassAfter));
#if HARKEN_ENGINE_DEV
        if (row.after.type != kEffectTypeNone) {
            MonitorEffectFacts facts{};
            facts.bypass01 = row.bypassAfter;
            void* storage = engine.effectBank.StateFor(row.after.type, row.after.poolIndex);
            const auto effect = BuildEffect(row.after.type, storage);
            if (effect.tailFrames) {
                facts.tailFrames = effect.tailFrames(storage);
                facts.tailKnown = true;
            }
            ObserveMonitorEffect(handle, row.after.generation,
                row.afterIdentity.effectId, VN_ACTION_TERMINAL_NONE, facts);
        }
#endif
    }
    state_->committed = true;
}

std::span<const CommandRecord> PreparedBusEffectsV1::Commands() const noexcept {
    return state_ ? std::span<const CommandRecord>(state_->commands) : std::span<const CommandRecord>{};
}

bool HasPreviewEffectAttachment(const Engine& engine, BusSlotRef host) noexcept {
    bool conflict = false;
    const auto check = [&](const auto& attachment, const auto&) {
        conflict |= attachment.scopeKind == VN_TARGET_SCOPE_BUS_EFFECT_PARAM &&
            attachment.busIndex == host.index &&
            (attachment.targetGeneration == 0 || attachment.targetGeneration == host.generation);
    };
    engine.bindings.ForEachAttachment(check);
    engine.bindings.ForEachProbeAttachment(check);
    return conflict;
}

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

bool CanResetNeutralBusPropertiesV1(const Engine& engine, std::uint32_t slot,
                                    std::uint64_t allowedEffectOwner) noexcept {
    if (slot >= engine.BusCapacity() || engine.busSoloActiveCount != 0) return false;
    if (slot >= engine.buses.BusCount() || !engine.buses.AliveAt(slot)) return true;
    const auto generation = engine.buses.GenerationAt(slot);
    if (engine.busOwnerCookies[slot] != 0) return false;
    for (std::uint32_t target = 0; target < kBusTargetCount; ++target) {
        if (engine.busTargets.ForeignClaim(slot, generation, target)) return false;
    }
    for (std::uint32_t effect = 0; effect < engine.config.fxSlotsPerBus; ++effect) {
        if ((engine.busEffects.Get(slot, effect).owner != 0 &&
             engine.busEffects.Get(slot, effect).owner != allowedEffectOwner) ||
            (allowedEffectOwner != 0 && engine.busEffects.Get(slot, effect).type != kEffectTypeNone &&
             engine.busEffects.Get(slot, effect).owner != allowedEffectOwner) ||
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
    // Same-generation neutral publication resets effects/fader and authored
    // gains, but the audio path preserves filters and first-volume history.
    // Removed/reused generations are cleared by ClearRetiredBusPropertiesV1.
    engine.effectParamTargets.ClearObject(EffectParamTargetStore::Scope::Bus, slot, generation);
    engine.busEffects.ClearBus(slot);
    engine.SetBusEffectRefs(slot, {});

    engine.busEffectSlotBypass[slot] = 0;
    engine.busMuted[slot] = 0;
    engine.busSoloed[slot] = 0;
    engine.busFaderAfterSlots[slot] = engine.config.fxSlotsPerBus;
    const auto layout = slot == 0 ? engine.config.deviceChannelLayout : engine.config.mixChannelLayout;
    engine.busOutputLayout[slot] = layout;
    engine.busCustomDownmix.erase(slot);
}

bool CanResetVolumeOnlyNonVolumePropertiesV2(const Engine& engine,
                                              std::uint32_t slot,
                                              std::uint64_t allowedOwnerCookie,
                                              std::uint64_t allowedEffectOwner) noexcept {
    static_assert(kBusTargetCount == 2);
    if (slot >= engine.BusCapacity()) return false;
    if (slot >= engine.buses.BusCount() || !engine.buses.AliveAt(slot)) return true;
    const auto generation = engine.buses.GenerationAt(slot);
    if ((engine.busOwnerCookies[slot] != 0 &&
         engine.busOwnerCookies[slot] != allowedOwnerCookie) ||
        engine.busSoloActiveCount < static_cast<std::uint32_t>(engine.busSoloed[slot] != 0))
        return false;
    for (std::uint32_t effect = 0; effect < engine.config.fxSlotsPerBus; ++effect) {
        if ((engine.busEffects.Get(slot, effect).owner != 0 &&
             engine.busEffects.Get(slot, effect).owner != allowedEffectOwner) ||
            (allowedEffectOwner != 0 && engine.busEffects.Get(slot, effect).type != kEffectTypeNone &&
             engine.busEffects.Get(slot, effect).owner != allowedEffectOwner) ||
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
            attachment.scopeKind == VN_TARGET_SCOPE_BUS_EFFECT_PARAM &&
            (allowedOwnerCookie == 0 || attachment.ownerCookie != allowedOwnerCookie);
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
                                           std::uint32_t slot, bool preserveEffects) noexcept {
    const auto generation = engine.buses.GenerationAt(slot);
    if (!preserveEffects) {
        engine.effectParamTargets.ClearObject(EffectParamTargetStore::Scope::Bus, slot, generation);
        engine.busEffects.ClearBus(slot);
        engine.SetBusEffectRefs(slot, {});

        engine.busEffectSlotBypass[slot] = 0;
    }
    engine.busMuted[slot] = 0;
    if (engine.busSoloed[slot] != 0) --engine.busSoloActiveCount;
    engine.busSoloed[slot] = 0;
    engine.busFaderAfterSlots[slot] = engine.config.fxSlotsPerBus;
    const auto layout = slot == 0 ? engine.config.deviceChannelLayout : engine.config.mixChannelLayout;
    engine.busOutputLayout[slot] = layout;
    engine.busCustomDownmix.erase(slot);
}

bool CanRetireTopologyBusV1(const Engine& engine, BusSlotRef ref, std::uint64_t revision) noexcept {
    return ref.Valid() && ref.index != 0 && ref.index < engine.buses.BusCount() &&
        engine.buses.AliveAt(ref.index) && engine.buses.GenerationAt(ref.index) == ref.generation &&
        revision != 0 && revision > engine.buses.InstalledGraphRevision() &&
        engine.buses.RetirementRevisionAt(ref.index) == 0 && engine.BusRetirement(ref.index) == nullptr;
}

void CommitRetireTopologyBusV1(Engine& engine, BusSlotRef ref, std::uint64_t revision) noexcept {
    assert(CanRetireTopologyBusV1(engine, ref, revision));
    const auto ownerId = engine.busTopologyOwnerIds[ref.index];
    const auto cookie = engine.busOwnerCookies[ref.index];
    const bool held = engine.buses.HoldRetiredSlot(ref, revision);
    assert(held);
    (void)held;
    assert(engine.busRetirements.size() < engine.busRetirements.capacity());
    if (engine.busRetirements.size() == engine.busRetirements.capacity()) std::terminate();
    engine.busRetirements.push_back({ref.index, ref.generation, revision, ownerId, cookie});
}

void ClearRetiredBusPropertiesV1(Engine& engine, BusSlotRef ref) noexcept {
    assert(engine.BusRetirement(ref.index) != nullptr &&
        engine.BusRetirement(ref.index)->generation == ref.generation &&
        engine.buses.RetirementRevisionAt(ref.index) != 0);
    engine.busTargets.RetireBusGeneration(ref.index, ref.generation);
    engine.busFilterSettings.Clear(ref.index);
    engine.busVolumePublished[ref.index] = 0;
    engine.effectParamTargets.ClearObject(EffectParamTargetStore::Scope::Bus, ref.index, ref.generation);
    engine.busEffects.ClearBus(ref.index);
    engine.SetBusEffectRefs(ref.index, {});

    engine.busEffectSlotBypass[ref.index] = 0;
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
        if (!facts.valid || facts.renderHandle == 0 || !engine.voices.IsAlive(facts.renderHandle)) continue;
        if (matches(facts.busIndex, facts.busGeneration) ||
            (i < engine.voiceSendConfig.size() && sendsMatch(engine.voiceSendConfig[i]))) return true;
    }
    for (const auto& identity : engine.timelineVoiceInstallIdentities) {
        if (identity.renderHandle != 0 && engine.voices.IsAlive(identity.renderHandle) &&
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

std::vector<VoiceSendMirrorMigrationV1> PrepareVoiceSendMirrorMigrationV1(Engine& engine,
    const std::uint32_t* generations, const std::uint8_t* alive, std::uint32_t count) {
    std::vector<VoiceSendMirrorMigrationV1> result;
    const auto capture = [&](VoiceSendConfigs& current, bool live) {
        auto next = current.Without([&](const auto& send) {
            return send.targetBusIndex >= count || alive[send.targetBusIndex] == 0 ||
                generations[send.targetBusIndex] != send.targetBusGeneration;
        });
        if (!(next == current)) result.push_back({&current, current, std::move(next), live});
    };
    for (auto& current : engine.voiceSendConfig) capture(current, true);
    for (auto& pending : engine.pendingStarts) capture(pending.voiceSends, false);
    for (auto& parked : engine.parkRecords) capture(parked.voiceSends, false);
    for (auto& identity : engine.timelineVoiceInstallIdentities) capture(identity.voiceSends, false);
    return result;
}

bool CanCommitVoiceSendMirrorMigrationV1(std::span<const VoiceSendMirrorMigrationV1> rows) noexcept {
    return std::all_of(rows.begin(), rows.end(), [](const auto& row) {
        return row.destination != nullptr && *row.destination == row.before;
    });
}

void CommitVoiceSendMirrorMigrationV1(Engine& engine,
    std::span<const VoiceSendMirrorMigrationV1> rows) noexcept {
    assert(CanCommitVoiceSendMirrorMigrationV1(rows));
    for (const auto& row : rows) {
        if (row.accountLive) engine.voiceSendActive -= row.before.size() - row.after.size();
        *row.destination = row.after;
    }
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
    for (auto& pending : engine.pendingStarts) {
        migrateDry(pending.busIndex, pending.busGeneration);
    }
    for (auto& facts : engine.voicePlayFacts) migrateDry(facts.busIndex, facts.busGeneration);
    for (auto& parked : engine.parkRecords) {
        migrateDry(parked.facts.busIndex, parked.facts.busGeneration);
    }
    for (auto& identity : engine.timelineVoiceInstallIdentities) {
        if (identity.renderHandle == 0) continue;
        migrateDry(identity.busIndex, identity.busGeneration);
    }
}

std::uint32_t DrainTopologyRetirementsV1(Engine& engine) noexcept {
    std::uint32_t released = 0;
    const auto installed = engine.buses.InstalledGraphRevision();
    std::size_t retained = 0;
    for (const auto& row : engine.busRetirements) {
        const auto slot = row.index;
        const BusSlotRef ref{slot, row.generation};
        if (installed >= row.revision && !engine.buses.AliveAt(slot) &&
            row.generation == engine.buses.GenerationAt(slot) &&
            row.revision == engine.buses.RetirementRevisionAt(slot) &&
            row.ownerId == engine.busTopologyOwnerIds[slot] && row.cookie == engine.busOwnerCookies[slot] &&
            !HasTopologyBusConsumersV1(engine, ref) && engine.buses.ReleaseRetiredSlot(ref, row.revision)) {
            engine.busOwnerCookies[slot] = 0;
            engine.busTopologyOwnerIds[slot] = 0;
            ++released;
        } else engine.busRetirements[retained++] = row;
    }
    engine.busRetirements.resize(retained);
    if (retained == 0) decltype(engine.busRetirements){}.swap(engine.busRetirements);
    return released;
}

std::uint32_t SelectPreviewOwnerDestination(Engine& engine, std::uint64_t ownerId) noexcept {
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
    const auto slot = engine.busTopologyOwners.size();
    return engine.PrepareBusOwnerCapacity(slot + 1) ? static_cast<std::uint32_t>(slot) : UINT32_MAX;
}

bool PreviewMasterCompatible(const Engine& engine, std::uint64_t fingerprint,
    std::uint64_t ownerId) noexcept {
    if (engine.buses.PreviewMasterEffectsClaimedByOther(ownerId)) return false;
    const auto* published = engine.packComposition.Published();
    for (std::uint32_t slot = 0; slot < engine.config.fxSlotsPerBus; ++slot) {
        const auto cookie = engine.busEffects.Get(0, slot).owner;
        if (cookie != 0 && published != nullptr && cookie == published->initHandle &&
            ownerId != kCanonicalPackSetMasterOwnerId) return false;
    }
    for (const auto& holder : engine.previewMasterHolders) {
        if (holder.occupied && holder.fingerprint != fingerprint) return false;
    }
    return true;
}

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
    std::uint64_t additions = 0;
    if (prospective != nullptr && prospective->busIds != nullptr)
        for (std::uint32_t i = 0; i < prospective->busCount; ++i)
            if ((prospective->alive == nullptr || prospective->alive[i]) &&
                engine.buses.FindIndex(prospective->busIds[i]) == kInvalidBusIndex) ++additions;
    if (!engine.PrepareAdditionalBusDefinitions(additions)) return false;
    const std::uint32_t capacity = engine.BusCapacity();
    const std::uint32_t sendSlots = engine.config.busSendSlotsPerBus;
    if (capacity == 0) {
        return false;
    }
    out.ownerId = ownerId;
    out.sendSlotsPerBus = sendSlots;
    out.reserved.assign(capacity, 0);
    out.alive.assign(capacity, 0);
    out.busIds.assign(capacity, VnBusId{});
    out.generations.assign(capacity, 0);
    out.parents.assign(capacity, 0);

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

    bool edgesValid = true;
    if (prospective != nullptr) {
        for (std::uint32_t from = 0; from < prospective->busCount; ++from) {
            if (prospective->alive != nullptr && prospective->alive[from] == 0) continue;
            const auto fromSlot = FindAssignedSlot(out, prospective->busIds[from]);
            if (fromSlot == UINT32_MAX || fromSlot >= out.busCount) return false;
            prospective->ForEachSend(from, sendSlots, [&](BusSendRecord edge, auto) {
                if (!edgesValid) return;
                if (edge.target >= prospective->busCount || prospective->busIds == nullptr) {
                    edgesValid = false;
                    return;
                }
                const auto targetSlot = FindAssignedSlot(out, prospective->busIds[edge.target]);
                if (targetSlot == UINT32_MAX || targetSlot == fromSlot || targetSlot >= out.busCount ||
                    !out.alive[targetSlot] || out.sends.Find(fromSlot, targetSlot) != nullptr) {
                    edgesValid = false;
                    return;
                }
                edge.source = fromSlot;
                edge.target = targetSlot;
                edge.topologyOwnerId = ownerId;
                edge.ownerCookie = 0;
                if (prospective->sends == nullptr && prospective->sendLevelsDb == nullptr && edge.applyLevel)
                    edge.levelDb = edge.preparedGain == 0 ? -160.0f : 20.0f * std::log10(edge.preparedGain);
                const auto* prior = engine.busSends.Find(fromSlot, targetSlot);
                const bool retained = prior != nullptr && prior->topologyOwnerId == ownerId &&
                    prior->ownerCookie == 0 &&
                    out.generations[fromSlot] == engine.buses.GenerationAt(fromSlot) &&
                    out.generations[targetSlot] == engine.buses.GenerationAt(targetSlot);
                edge.generation = retained ? prior->generation : engine.NewBusSendGeneration();
                if (retained) edge.slot = prior->slot;
                out.sends.Add(edge);
            });
        }
    }
    if (!edgesValid) return false;
    out.valid = true;
    return true;
}

void ApplySlotPlanToProspective(SlotPlanV1& plan,
                                ProspectiveTopologyView& view) noexcept {
    view.busCount = plan.busCount;
    view.busIds = plan.busIds.data();
    view.generations = plan.generations.data();
    view.parents = plan.parents.data();
    view.alive = plan.alive.data();
    view.sends = &plan.sends;
}

bool SlotPlanMatchesLive(const Engine& engine, const SlotPlanV1& plan) noexcept {
    if (plan.preservesForeign &&
        (plan.sourceGraphRevision != engine.acceptedBusTopologyRevision ||
         plan.sourceBusCount != engine.buses.BusCount())) return false;
    if (!plan.valid || plan.busCount == 0 ||
        plan.busCount > engine.BusCapacity()) {
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

void InstallSlotPlanLogic(Engine& engine, SlotPlanV1& plan) noexcept {
    const std::uint32_t oldCount = engine.buses.BusCount();
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
    plan.sends.ClearActions();
    engine.busSends.Swap(plan.sends);
    engine.busSendActive = engine.busSends.Count();
    UpdateSendHighWater(engine);
}

}  // namespace harken
