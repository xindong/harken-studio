#include "TopologySlotPlan.h"

#include "Engine.h"
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
            const auto before = engine.busEffectInstanceRefs[removed.slot][slot];
            const auto owner = engine.busEffectOwnerCookies[removed.slot][slot];
            if ((owner != 0 && owner != allowedOwner) ||
                (before.type != kEffectTypeNone && (allowedOwner == 0 || owner != allowedOwner)) ||
                engine.effectParamTargets.ClaimedInSlot(EffectParamTargetStore::Scope::Bus,
                    removed.slot, removed.generation, slot) ||
                engine.dynamicRoutes.HasTargetRoutes(removed.slot, slot)) return VN_IN_USE;
            if (before.type == kEffectTypeNone) continue;
            if (!allowStructure) return VN_BACKPRESSURE;
            state->structureChanged = true;
            state->rows.emplace_back();
            auto& row = state->rows.back();
            row.bus = removed.slot;
            row.generation = removed.generation;
            row.slot = slot;
            row.before = before;
            row.owner = owner;
            row.bypassBefore = engine.busEffectSlotBypass[removed.slot][slot];
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
            const auto before = retained ? engine.busEffectInstanceRefs[bus][slot] : EffectInstanceRef{};
            const auto owner = retained ? engine.busEffectOwnerCookies[bus][slot] : 0;
            if ((owner != 0 && owner != allowedOwner) ||
                (before.type != kEffectTypeNone && (allowedOwner == 0 || owner != allowedOwner)) ||
                engine.effectParamTargets.ClaimedInSlot(EffectParamTargetStore::Scope::Bus,
                    bus, assignment.generation, slot) || engine.dynamicRoutes.HasTargetRoutes(bus, slot))
                return VN_IN_USE;
            if (!effect && before.type == kEffectTypeNone) continue;
            state->rows.emplace_back();
            auto& row = state->rows.back();
            row.bus = bus;
            row.generation = assignment.generation;
            row.slot = slot;
            row.before = before;
            row.owner = owner;
            row.bypassBefore = retained ? engine.busEffectSlotBypass[bus][slot] : 0;
            row.bypassAfter = effect ? effect->flags & 1u : 0;
            if (before.type != kEffectTypeNone)
                for (std::uint32_t p = 0; p < EffectParamCount(before.type); ++p) {
                    row.originalParameters.push_back(engine.effectParamTargets.Reduce(
                        EffectParamTargetStore::Scope::Bus, bus, slot, p));
                    row.originalAuthoredParameters.push_back(engine.effectParamTargets.AuthoredBase(
                        EffectParamTargetStore::Scope::Bus, bus, slot, p));
                }
            if (effect)
                for (std::uint32_t p = 0; p < effect->parameterCount; ++p)
                    row.parameters.push_back(parameters[effect->parameterFirst + p].value);
            const bool same = effect && before.type == effect->effectType &&
                engine.effectBank.ObjectIdentity(before).effectId == effect->effectId;
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
                row.after = engine.effectBank.AcquirePrepared(effect->effectType,
                    {effect->effectId, engine.CurrentProjectId()});
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
        const auto& live = engine.busEffectInstanceRefs[row.bus][row.slot];
        if (live.type != row.before.type || live.poolIndex != row.before.poolIndex ||
            live.generation != row.before.generation ||
            engine.busEffectOwnerCookies[row.bus][row.slot] != row.owner ||
            engine.busEffectSlotBypass[row.bus][row.slot] != row.bypassBefore)
            return false;
        for (std::uint32_t p = 0; p < row.originalParameters.size(); ++p)
            if (engine.effectParamTargets.Reduce(EffectParamTargetStore::Scope::Bus,
                    row.bus, row.slot, p) != row.originalParameters[p] ||
                engine.effectParamTargets.AuthoredBase(EffectParamTargetStore::Scope::Bus,
                    row.bus, row.slot, p) != row.originalAuthoredParameters[p]) return false;
    }
    return true;
}

void PreparedBusEffectsV1::Commit() noexcept {
    auto& engine = *state_->engine;
    for (auto& row : state_->rows) {
        const auto handle = PackedMonitorEffectHandle(row.bus, row.slot);
        if ((row.retire || row.replace) && row.before.type != kEffectTypeNone)
            ObserveMonitorEffectRelease(handle, row.before.generation,
                engine.effectBank.ObjectIdentity(row.before).effectId, VN_ACTION_TERMINAL_NONE);
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
                        EffectParamTargetStore::Scope::Bus, row.bus, row.slot, p, row.after.type))
                    engine.effectParamTargets.NoteDirectBound(EffectParamTargetStore::Scope::Bus,
                        row.bus, row.slot, p, row.parameters[p]);
                else engine.effectParamTargets.NoteDirect(EffectParamTargetStore::Scope::Bus,
                    row.bus, row.slot, p, row.parameters[p], row.parameters[p]);
            }
        }
        engine.busEffectSlotTypes[row.bus][row.slot] = row.after.type;
        engine.busEffectInstanceRefs[row.bus][row.slot] = row.after;
        engine.busEffectOwnerCookies[row.bus][row.slot] =
            row.after.type == kEffectTypeNone ? 0 : state_->nextOwner;
        engine.busEffectSlotBypass[row.bus][row.slot] = static_cast<std::uint8_t>(row.bypassAfter);
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
                engine.effectBank.ObjectIdentity(row.after).effectId, VN_ACTION_TERMINAL_NONE, facts);
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
    if (slot >= engine.config.maxBuses || engine.busSoloActiveCount != 0) return false;
    if (slot >= engine.buses.BusCount() || !engine.buses.AliveAt(slot)) return true;
    const auto generation = engine.buses.GenerationAt(slot);
    if (engine.busOwnerCookies[slot] != 0) return false;
    for (std::uint32_t target = 0; target < kBusTargetCount; ++target) {
        if (engine.busTargets.ForeignClaim(slot, generation, target)) return false;
    }
    for (std::uint32_t effect = 0; effect < engine.config.fxSlotsPerBus; ++effect) {
        if ((engine.busEffectOwnerCookies[slot][effect] != 0 &&
             engine.busEffectOwnerCookies[slot][effect] != allowedEffectOwner) ||
            (allowedEffectOwner != 0 && engine.busEffectSlotTypes[slot][effect] != kEffectTypeNone &&
             engine.busEffectOwnerCookies[slot][effect] != allowedEffectOwner) ||
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
                                              std::uint64_t allowedOwnerCookie,
                                              std::uint64_t allowedEffectOwner) noexcept {
    static_assert(kBusTargetCount == 2);
    if (slot >= engine.config.maxBuses) return false;
    if (slot >= engine.buses.BusCount() || !engine.buses.AliveAt(slot)) return true;
    const auto generation = engine.buses.GenerationAt(slot);
    if ((engine.busOwnerCookies[slot] != 0 &&
         engine.busOwnerCookies[slot] != allowedOwnerCookie) ||
        engine.busSoloActiveCount < static_cast<std::uint32_t>(engine.busSoloed[slot] != 0))
        return false;
    for (std::uint32_t effect = 0; effect < engine.config.fxSlotsPerBus; ++effect) {
        if ((engine.busEffectOwnerCookies[slot][effect] != 0 &&
             engine.busEffectOwnerCookies[slot][effect] != allowedEffectOwner) ||
            (allowedEffectOwner != 0 && engine.busEffectSlotTypes[slot][effect] != kEffectTypeNone &&
             engine.busEffectOwnerCookies[slot][effect] != allowedEffectOwner) ||
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
        engine.busEffectSlotTypes[slot].fill(kEffectTypeNone);
        engine.busEffectInstanceRefs[slot].fill(EffectInstanceRef{});
        engine.busEffectOwnerCookies[slot].fill(0);
        engine.busEffectSlotBypass[slot].fill(0);
    }
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

bool PreviewMasterCompatible(const Engine& engine, std::uint64_t fingerprint,
    std::uint64_t ownerId) noexcept {
    if (engine.buses.PreviewMasterEffectsClaimedByOther(ownerId)) return false;
    const auto* published = engine.packComposition.Published();
    for (std::uint32_t slot = 0; slot < engine.config.fxSlotsPerBus; ++slot) {
        const auto cookie = engine.busEffectOwnerCookies[0][slot];
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

}  // namespace harken
