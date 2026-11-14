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

std::size_t PreparedBusEffectsV1::SettingsWriteCount() const noexcept {
    return state_ ? std::count_if(state_->rows.begin(), state_->rows.end(),
        [](const auto& row) { return !row.retire && row.afterType != kEffectTypeNone; }) : 0;
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
    }
    state_->committed = true;
}

std::span<const CommandRecord> PreparedBusEffectsV1::Commands() const noexcept {
    return state_ ? std::span<const CommandRecord>(state_->commands) : std::span<const CommandRecord>{};
}

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
    if (slot >= engine.config.maxBuses) return false;
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
    engine.busFilterSettings[ref.index] = {};
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
    const bool liveBusUpdate = (vintage == 3 || vintage == 4 || vintage == 5) &&
        image.Valid() && image.PreservesContent();
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

void ApplySlotPlanToProspective(SlotPlanV1& plan,
                                ProspectiveTopologyView& view) noexcept {
    view.busCount = plan.busCount;
    view.busIds = plan.busIds.data();
    view.generations = plan.generations.data();
    view.parents = plan.parents.data();
    view.alive = plan.alive.data();
    view.sends = &plan.sends;
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
        coldSlots.assign(engine.config.maxBuses, 0);
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
        std::vector<std::shared_ptr<const BusDormantWork>> sources;
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
                    preparedWork.sources.push_back(engine.buses.DormantWorkFor(host));
                    std::copy_n(row.fxSlotEffectRef, kMaxFxSlotsPerBus, refs.begin());
                }
            }
            const auto result = engine.buses.PreparePreviewEffectChain(host, view.ownerId, targets, refs,
                engine.CurrentProjectId(), effects.back());
            if (result != VN_OK || !effects.back().changes) return result;
            const auto& update = effects.back().state->update;
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
                row.state->update.targetRevision, row.releaseOwner});
        engine.preparedTopologyPlans.SetEffectUpdates(planIndex, std::move(adoptions));
    } catch (...) {
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
                engine.buses.DormantWorkFor(row.bus) != prepared.preparedCold_[i]) return false;
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
    } catch (const std::bad_alloc&) {
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    } catch (...) {
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
    } catch (const std::bad_alloc&) {
        engine.topologyPlans.Recycle(liveIndex);
        result.status = ProjectGenerationCommitStatusV1::Backpressure;
        return result;
    } catch (...) {
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
    } catch (...) {
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
        (prepared.signals_.owner && !engine.mixer.PrepareSignalBacking(*prepared.signals_.owner)) ||
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
    std::size_t effectWrites = prepared.effects_ ? prepared.effects_->SettingsWriteCount() : 0;
    for (const auto& chain : prepared.effectChains_) {
        if (!chain.changes || chain.releaseOwner) continue;
        for (const auto& target : chain.state->update.targets)
            if (target.effectType != kEffectTypeNone) ++effectWrites;
    }
    targetStorageReady &= engine.busEffects.Prepare(effectWrites);
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
        const auto& update = row.state->update;
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
