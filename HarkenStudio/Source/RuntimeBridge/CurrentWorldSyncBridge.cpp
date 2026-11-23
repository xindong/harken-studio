#include "CurrentWorldSyncBridge.h"
#include "NativeValues.h"
#include "CurrentWorldSyncBridgeInternal.h"
#include "MediaRegistry.h"
#include "PlaybackBridge.h"
#include "RuntimeLink.h"
#include "RuntimeLinkInternal.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

namespace harken::studio::bridge {
namespace {

struct PreparedProjectGenerationHold {
    Detail::RuntimePreparedProject prepared;
    CurrentWorldSyncToken token;
    VnPreviewProjectDescV1 view{};
    VnPreviewProjectDescV4 effectView{};
    std::vector<std::uint8_t> body;
    std::vector<VnPreviewBusV1> buses;
    std::vector<VnPreviewBusVolumeV2> volumes;
    std::vector<VnPreviewBusSendV1> sends;
    std::vector<float> sendLevelsDb;
    std::vector<CurrentWorldArtifact::Bus> artifactBuses;
    std::vector<CurrentWorldArtifact::Send> artifactSends;
    std::vector<CurrentWorldArtifact::Effect> artifactEffects;
    std::vector<CurrentWorldArtifact::EffectParameter> artifactEffectParameters;
    std::vector<VnPreviewBusEffectV1> effects;
    std::vector<VnPreviewBusEffectParameterV1> effectParameters;
    std::vector<std::uint32_t> faderAfterSlots;
    std::shared_ptr<const CurrentWorldSoundPreparation> soundPreparation;
    std::uint64_t installedWorldGeneration = 0;
    bool committed = false;
    bool testOpen = false;
    bool soundMediaRegistered = false;
    bool emptyWorldClose = false;
    bool effectDefinitionChanged = false;
    PreviewEffectDefinitions voiceEffects;
};

std::optional<PreparedProjectGenerationHold> generationHold;
std::uint64_t publishedOwnerId = 0;
std::uint64_t publishedRuntimeEpoch = 0;
std::shared_ptr<const std::vector<CurrentWorldArtifact::Effect>> publishedEffects;
struct PublishedBusEffectValues {
    std::vector<float> parameters;
    bool bypass = false;
};
std::map<harken::studio::types::ObjectId, PublishedBusEffectValues> publishedBusEffectValues;
std::map<std::uint64_t, PreviewEffectDefinitions> publishedVoiceEffects;
std::uint64_t publishedVoiceEffectEpoch = 0;
std::uint64_t publishedVoiceEffectGeneration = 0;
std::deque<CurrentWorldSyncResult> completedWorlds;
CurrentWorldInstallBackend installBackend;
CurrentWorldDiscardObserver discardObserver;
ProjectGenerationTestBackend* generationTestBackend = nullptr;

void enqueueCompletion(CurrentWorldSyncResult result)
{
    completedWorlds.push_back(std::move(result));
}

CurrentWorldSyncStatus mapPrepareStatus(
    Detail::RuntimeProjectGenerationPrepareStatus status)
{
    using Prepare = Detail::RuntimeProjectGenerationPrepareStatus;
    switch (status) {
    case Prepare::Ok:
        return CurrentWorldSyncStatus::Ok;
    case Prepare::Backpressure:
        return CurrentWorldSyncStatus::Pending;
    case Prepare::Capacity:
        return CurrentWorldSyncStatus::Pending;
    case Prepare::RuntimeUnavailable:
        return CurrentWorldSyncStatus::RuntimeUnavailable;
    case Prepare::InvalidView:
    case Prepare::InvalidWorld:
        return CurrentWorldSyncStatus::CompileFailed;
    case Prepare::CapabilityUnavailable:
    case Prepare::DeviceError:
        return CurrentWorldSyncStatus::InstallFailed;
    }
    return CurrentWorldSyncStatus::InstallFailed;
}

CurrentWorldSyncStatus mapCommitStatus(
    Detail::RuntimeProjectGenerationCommitStatus status)
{
    using Commit = Detail::RuntimeProjectGenerationCommitStatus;
    switch (status) {
    case Commit::Ok:
        return CurrentWorldSyncStatus::Pending;
    case Commit::Backpressure:
        return CurrentWorldSyncStatus::Pending;
    case Commit::RuntimeUnavailable:
        return CurrentWorldSyncStatus::RuntimeUnavailable;
    case Commit::InvalidPrepared:
    case Commit::DeviceError:
        return CurrentWorldSyncStatus::InstallFailed;
    }
    return CurrentWorldSyncStatus::InstallFailed;
}

void confirmPublishedBuses(const PreparedProjectGenerationHold& hold)
{
    std::vector<RuntimeBusDefinition> buses;
    buses.reserve(hold.artifactBuses.size());
    for (const auto& bus : hold.artifactBuses) {
        RuntimeBusDefinition definition;
        definition.id = bus.id;
        definition.parentId = bus.parentId;
        buses.push_back(std::move(definition));
    }
    std::vector<RuntimeBusSendDefinition> sends;
    sends.reserve(hold.artifactSends.size());
    for (const auto& send : hold.artifactSends) {
        RuntimeBusSendDefinition definition;
        definition.sourceId = send.sourceId;
        definition.targetId = send.targetId;
        definition.levelDb = send.levelDb;
        sends.push_back(std::move(definition));
    }
    Detail::runtimeConfirmBusTopologyWithoutApply(buses, sends);
}

CurrentWorldSyncStatus mapStaleOwnerCapacity()
{
    return CurrentWorldSyncStatus::InstallFailed;
}

CurrentWorldSyncResult registerPreparedSounds(PreparedProjectGenerationHold& hold)
{
    CurrentWorldSyncResult result;
    result.token = hold.token;
    result.status = CurrentWorldSyncStatus::Ok;
    if (!hold.soundPreparation || hold.soundMediaRegistered)
        return result;
    for (const auto& sound : hold.soundPreparation->sounds) {
        const auto revision = hold.soundPreparation->contentRevisions.find(sound.binding.soundId);
        if (sound.media && (revision == hold.soundPreparation->contentRevisions.end()
            || revision->second == 0 || revision->second != sound.media->contentRevision)) {
            result.relatedStableId = sound.binding.soundId;
            result.status = CurrentWorldSyncStatus::InvalidProject;
            return result;
        }
        if (revision != hold.soundPreparation->contentRevisions.end()
            && revision->second != 0
            && Detail::runtimeRegisteredSoundRevision(sound.binding.soundId) == revision->second)
            continue;
        const auto registered = sound.media
            ? Detail::runtimeRegisterPreviewMedia(sound.binding.soundId, *sound.media)
            : Detail::runtimeRegisterSoundPcm(sound.binding.soundId, sound.samples,
                sound.channels, sound.sampleRate, sound.loopStartFrame, sound.loopEndFrame);
        if (registered == VN_OK)
            continue;
        result.relatedStableId = sound.binding.soundId;
        result.status = registered == VN_BACKPRESSURE
            ? CurrentWorldSyncStatus::Pending
            : registered == VN_NOT_INITIALIZED
                ? CurrentWorldSyncStatus::RuntimeUnavailable
                : CurrentWorldSyncStatus::InstallFailed;
        return result;
    }
    hold.soundMediaRegistered = true;
    return result;
}

CurrentWorldSyncResult prepareAndCommitHold(PreparedProjectGenerationHold& hold)
{
    CurrentWorldSyncResult result;
    result.token = hold.token;
    if (hold.effectDefinitionChanged
        && Detail::runtimeHasEffectChainConsumers()) {
        result.status = CurrentWorldSyncStatus::Pending;
        return result;
    }
    if (generationTestBackend) {
        ++generationTestBackend->prepareCalls;
        if (generationTestBackend->prepareStatus
            != ProjectGenerationTestPrepareStatus::Ok) {
            result.status = mapPrepareStatus(
                static_cast<Detail::RuntimeProjectGenerationPrepareStatus>(
                    generationTestBackend->prepareStatus));
            if (generationTestBackend->prepareStatus
                == ProjectGenerationTestPrepareStatus::Capacity)
                result.status = mapStaleOwnerCapacity();
            if (generationTestBackend->prepareStatus
                == ProjectGenerationTestPrepareStatus::Backpressure)
                result.status = CurrentWorldSyncStatus::Pending;
            return result;
        }
        ++generationTestBackend->commitCalls;
        if (generationTestBackend->commitStatus
            == ProjectGenerationTestCommitStatus::Backpressure) {
            result.status = CurrentWorldSyncStatus::Pending;
            return result;
        }
        if (generationTestBackend->commitStatus
            != ProjectGenerationTestCommitStatus::Ok) {
            result.status = CurrentWorldSyncStatus::InstallFailed;
            return result;
        }
        result = registerPreparedSounds(hold);
        if (!result.ok())
            return result;
        hold.committed = true;
        hold.installedWorldGeneration =
            generationTestBackend->installedWorldGeneration;
        hold.testOpen = false;
        result.status = CurrentWorldSyncStatus::Pending;
        result.worldGeneration = hold.installedWorldGeneration;
        return result;
    }
    if (installBackend) {
        const CurrentWorldTestInstallResult installed = installBackend();
        if (installed.status == CurrentWorldTestInstallResult::Status::Backpressure) {
            result.status = CurrentWorldSyncStatus::Pending;
            return result;
        }
        if (installed.status != CurrentWorldTestInstallResult::Status::Ok) {
            result.status = CurrentWorldSyncStatus::InstallFailed;
            return result;
        }
        result = registerPreparedSounds(hold);
        if (!result.ok())
            return result;
        hold.committed = true;
        hold.installedWorldGeneration = installed.resultGeneration;
        hold.testOpen = true;
        result.status = CurrentWorldSyncStatus::Pending;
        result.worldGeneration = hold.installedWorldGeneration;
        return result;
    }

    const auto prepareStatus = hold.prepared.Valid()
        ? Detail::RuntimeProjectGenerationPrepareStatus::Ok
        : hold.emptyWorldClose
            ? Detail::runtimePrepareProjectGeneration(hold.view, hold.prepared)
            : Detail::runtimePrepareProjectGeneration(hold.effectView, hold.prepared);
    if (prepareStatus == Detail::RuntimeProjectGenerationPrepareStatus::Capacity) {
        const auto live = Detail::runtimeLiveOwnerRevision(hold.view.ownerId);
        if (live.found && hold.view.ownerBaseRevision != live.currentRevision) {
            result.status = CurrentWorldSyncStatus::InstallFailed;
            return result;
        }
        result.status = CurrentWorldSyncStatus::Pending;
        return result;
    }
    if (prepareStatus != Detail::RuntimeProjectGenerationPrepareStatus::Ok) {
        result.status = mapPrepareStatus(prepareStatus);
        return result;
    }
    result = registerPreparedSounds(hold);
    if (!result.ok()) {
        if (!result.pending())
            Detail::runtimeCancelPreparedProjectGeneration(std::move(hold.prepared));
        return result;
    }
    std::uint64_t generation = 0;
    const auto commitStatus =
        Detail::runtimeCommitPreparedProjectGeneration(hold.prepared, &generation);
    if (commitStatus == Detail::RuntimeProjectGenerationCommitStatus::Backpressure) {
        result.status = CurrentWorldSyncStatus::Pending;
        return result;
    }
    if (commitStatus != Detail::RuntimeProjectGenerationCommitStatus::Ok) {
        Detail::runtimeCancelPreparedProjectGeneration(std::move(hold.prepared));
        result.status = mapCommitStatus(commitStatus);
        return result;
    }
    hold.committed = true;
    hold.installedWorldGeneration = generation;
    publishedOwnerId = hold.view.ownerId;
    publishedRuntimeEpoch = hold.token.runtimeEpoch;
    publishedEffects = std::make_shared<const std::vector<CurrentWorldArtifact::Effect>>(
        hold.artifactEffects);
    publishedBusEffectValues.clear();
    for (const auto& effect : hold.artifactEffects) {
        auto& values = publishedBusEffectValues[effect.effectId];
        values.bypass = (effect.flags & 1u) != 0;
        for (std::uint32_t i = 0; i < effect.parameterCount; ++i)
            values.parameters.push_back(hold.artifactEffectParameters[effect.parameterFirst + i].value);
    }
    result.status = CurrentWorldSyncStatus::Pending;
    result.worldGeneration = generation;
    return result;
}

CurrentWorldSyncResult finishOpenIfReady(PreparedProjectGenerationHold& hold)
{
    CurrentWorldSyncResult result;
    result.token = hold.token;
    result.worldGeneration = hold.installedWorldGeneration;
    const bool open = hold.testOpen
        || Detail::runtimeCurrentWorldBusAdmissionIsOpen(
               hold.installedWorldGeneration);
    if (!open) {
        result.status = CurrentWorldSyncStatus::Pending;
        return result;
    }
    result.status = CurrentWorldSyncStatus::Ok;
    confirmPublishedBuses(hold);
    if (publishedVoiceEffectEpoch != hold.token.runtimeEpoch) {
        publishedVoiceEffects.clear();
        publishedVoiceEffectEpoch = hold.token.runtimeEpoch;
    }
    for (auto it = publishedVoiceEffects.begin(); it != publishedVoiceEffects.end();) {
        if (!Detail::runtimeHasPreviewWorldGeneration(it->first))
            it = publishedVoiceEffects.erase(it);
        else ++it;
    }
    publishedVoiceEffects[hold.installedWorldGeneration] = hold.voiceEffects;
    publishedVoiceEffectGeneration = hold.installedWorldGeneration;
    if (hold.soundPreparation) {
        for (const auto& sound : hold.soundPreparation->sounds) {
            syncSound(sound.binding);
            mediaRegistryRegister(sound.binding.sourceId, sound.sourcePath);
        }
        for (const auto& [id, revision] : hold.soundPreparation->contentRevisions)
            if (revision == 0)
                removeRuntimeObject(id);
    }
    return result;
}

CurrentWorldSyncResult installCompiledWorld(
    CurrentWorldArtifact artifact,
    CurrentWorldSyncToken token,
    std::uint64_t ownerId,
    bool emptyWorldClose,
    std::uint64_t ownerBaseRevision)
{
    CurrentWorldSyncResult result;
    result.token = token;

    PreparedProjectGenerationHold hold;
    hold.token = token;
    hold.emptyWorldClose = emptyWorldClose;
    hold.soundPreparation = std::move(artifact.soundPreparation);
    hold.body = std::move(artifact.body);
    hold.artifactBuses = std::move(artifact.buses);
    hold.artifactSends = std::move(artifact.sends);
    hold.artifactEffects = std::move(artifact.effects);
    hold.artifactEffectParameters = std::move(artifact.effectParameters);
    hold.voiceEffects = std::move(artifact.voiceEffects);
    if (publishedEffects && publishedOwnerId == ownerId
        && publishedRuntimeEpoch == token.runtimeEpoch) {
        std::map<harken::studio::types::ObjectId, harken::studio::types::ObjectId> definitions;
        for (const auto& effect : *publishedEffects)
            definitions.emplace(effect.effectId, effect.definitionId);
        hold.effectDefinitionChanged = std::any_of(hold.artifactEffects.begin(),
            hold.artifactEffects.end(), [&](const auto& effect) {
                const auto previous = definitions.find(effect.effectId);
                const auto limiterOnly = [&](const auto& rows) {
                    return std::all_of(rows.begin(), rows.end(), [&](const auto& row) {
                        return row.busId != effect.busId || row.effectType == VN_EFFECT_LIMITER;
                    });
                };
                return previous != definitions.end()
                    && previous->second != effect.definitionId
                    && !(limiterOnly(*publishedEffects) && limiterOnly(hold.artifactEffects));
            });
    }
    for (const auto& bus : hold.artifactBuses) {
        hold.buses.push_back({toNative(bus.id), toNative(bus.parentId)});
        hold.volumes.push_back({toNative(bus.id), bus.faderDb, bus.outputBusVolumeDb});
        hold.faderAfterSlots.push_back(bus.faderAfterSlot);
    }
    for (const auto& send : hold.artifactSends) {
        hold.sends.push_back({toNative(send.sourceId), toNative(send.targetId)});
        hold.sendLevelsDb.push_back(send.levelDb);
    }
    for (const auto& effect : hold.artifactEffects) {
        hold.effects.push_back({toNative(effect.busId), toNative(effect.effectId),
            effect.slot, effect.effectType, effect.descriptorVintage, effect.flags,
            effect.parameterFirst, effect.parameterCount});
    }
    for (const auto& parameter : hold.artifactEffectParameters)
        hold.effectParameters.push_back({parameter.index, parameter.value});
    hold.view.structSize = sizeof(hold.view);
    hold.view.bodyVintage = artifact.bodyVintage;
    hold.view.projectSessionId = token.projectSessionId;
    hold.view.authoringRevision = token.authoringRevision;
    hold.view.runtimeEpoch = token.runtimeEpoch;
    hold.view.submissionSerial = token.submissionSerial;
    hold.view.body = hold.body.data();
    hold.view.bodyBytes = hold.body.size();
    hold.view.buses = hold.buses.data();
    hold.view.busCount = static_cast<std::uint32_t>(hold.buses.size());
    hold.view.sends = hold.sends.data();
    hold.view.sendCount = static_cast<std::uint32_t>(hold.sends.size());
    hold.view.publishedBusFingerprint = artifact.publishedBusFingerprint;
    hold.view.compileTargetProfileFingerprint = artifact.compileTargetProfileFingerprint;
    hold.view.ownerId = ownerId;
    hold.view.ownerBaseRevision = ownerBaseRevision;
    hold.view.ownerRevision = token.authoringRevision;
    hold.effectView.base.base.base = hold.view;
    hold.effectView.base.base.busVolumes = hold.volumes.data();
    hold.effectView.base.base.busVolumeCount = static_cast<std::uint32_t>(hold.volumes.size());
    if (artifact.profile) {
        hold.effectView.base.base.profileVintage = artifact.profile->vintage;
        hold.effectView.base.base.renderSampleRateHz = artifact.profile->sampleRate;
        hold.effectView.base.base.mixLayout = artifact.profile->mixLayout;
        hold.effectView.base.base.deviceLayout = artifact.profile->deviceLayout;
        hold.effectView.base.base.runtimeCapabilityMask = artifact.profile->capabilityMask;
    }
    hold.effectView.base.busSendLevelsDb = hold.sendLevelsDb.data();
    hold.effectView.base.busSendLevelCount = static_cast<std::uint32_t>(hold.sendLevelsDb.size());
    hold.effectView.base.base.base.structSize = sizeof(hold.effectView);
    hold.effectView.busEffects = hold.effects.data();
    hold.effectView.busEffectCount = static_cast<std::uint32_t>(hold.effects.size());
    hold.effectView.busEffectParameters = hold.effectParameters.data();
    hold.effectView.busEffectParameterCount = static_cast<std::uint32_t>(hold.effectParameters.size());
    hold.effectView.busFaderAfterSlots = hold.faderAfterSlots.data();
    hold.effectView.busFaderCount = static_cast<std::uint32_t>(hold.faderAfterSlots.size());
    const auto previousEffects = publishedVoiceEffects.find(publishedVoiceEffectGeneration);
    if (publishedVoiceEffectEpoch == token.runtimeEpoch && previousEffects != publishedVoiceEffects.end()) {
        for (const auto& [soundId, effects] : hold.voiceEffects) {
            const auto sound = previousEffects->second.find(soundId);
            if (sound == previousEffects->second.end()) continue;
            for (const auto& effect : effects) {
                const auto old = std::find_if(sound->second.begin(), sound->second.end(),
                    [&](const auto& candidate) { return candidate.id == effect.id; });
                if (old != sound->second.end() && old->presetId != effect.presetId)
                    hold.effectView.forceWorldReplacement = 1;
            }
        }
    }

    result = prepareAndCommitHold(hold);
    if (result.status == CurrentWorldSyncStatus::Pending) {
        generationHold = std::move(hold);
        if (generationHold->committed) {
            const auto opened = finishOpenIfReady(*generationHold);
            if (opened.ok()) {
                generationHold.reset();
                enqueueCompletion(opened);
                return opened;
            }
        }
        return result;
    }
    if (result.status != CurrentWorldSyncStatus::Ok)
        return result;
    generationHold = std::move(hold);
    return result;
}

} // namespace

CurrentWorldSyncResult submitCompiledCurrentWorld(
    CurrentWorldArtifactResult compiled, CurrentWorldSyncToken token)
{
    CurrentWorldSyncResult result;
    result.token = token;
    if (!compiled.ok()) {
        result.status = compiled.status;
        result.relatedStableId = compiled.relatedStableId;
        return result;
    }
    if (!Detail::runtimeIsActive()) {
        result.status = CurrentWorldSyncStatus::RuntimeUnavailable;
        return result;
    }
    if (generationHold) {
        if (generationHold->token == token) {
            result.status = CurrentWorldSyncStatus::Pending;
            return result;
        }
        discardPendingCurrentWorld(PendingCurrentWorldDisposition::Superseded);
    }
    const auto live = Detail::runtimeLiveOwnerRevision(token.projectSessionId);
    return installCompiledWorld(std::move(compiled.artifact), token,
        token.projectSessionId, false, live.found ? live.currentRevision : 0);
}

CurrentWorldSyncResult submitCurrentWorld(CurrentWorldSyncToken token,
                                          const CurrentWorldArtifactBuilder& build)
{
    CurrentWorldSyncResult result;
    result.token = token;
    if (!build) {
        result.status = CurrentWorldSyncStatus::InvalidProject;
        return result;
    }
    if (generationHold && !(generationHold->token == token)) {
        if (generationHold->prepared.Valid())
            Detail::runtimeCancelPreparedProjectGeneration(
                std::move(generationHold->prepared));
        CurrentWorldSyncResult superseded;
        superseded.token = generationHold->token;
        superseded.status = CurrentWorldSyncStatus::Superseded;
        if (discardObserver)
            discardObserver(superseded.token, Detail::runtimeIsActive());
        generationHold.reset();
        enqueueCompletion(std::move(superseded));
    } else if (generationHold) {
        result.status = CurrentWorldSyncStatus::Pending;
        result.token = generationHold->token;
        return result;
    }
    if (!Detail::runtimeIsActive()) {
        result.status = CurrentWorldSyncStatus::RuntimeUnavailable;
        return result;
    }

    CurrentWorldArtifactResult compiled = build();
    if (!compiled.ok()) {
        result.status = compiled.status;
        result.relatedStableId = compiled.relatedStableId;
        return result;
    }

    const auto ownerBaseRevision = compiled.artifact.ownerBaseRevision;
    return installCompiledWorld(std::move(compiled.artifact), token,
                                token.projectSessionId, false, ownerBaseRevision);
}

CurrentWorldSyncResult closeCurrentWorld(CurrentWorldSyncToken token,
                                         const CurrentWorldArtifactBuilder& buildEmpty)
{
    CurrentWorldSyncResult result;
    result.token = token;
    if (generationHold && !(generationHold->token == token)) {
        if (generationHold->prepared.Valid())
            Detail::runtimeCancelPreparedProjectGeneration(
                std::move(generationHold->prepared));
        completedWorlds.clear();
        generationHold.reset();
    } else if (generationHold) {
        result.status = CurrentWorldSyncStatus::Pending;
        result.token = generationHold->token;
        return result;
    }
    if (!Detail::runtimeIsActive()) {
        result.status = CurrentWorldSyncStatus::RuntimeUnavailable;
        return result;
    }
    auto compiled = buildEmpty ? buildEmpty() : CurrentWorldArtifactResult{};
    if (!compiled.ok()) {
        result.status = CurrentWorldSyncStatus::CompileFailed;
        return result;
    }
    const auto ownerId = publishedOwnerId != 0 && publishedRuntimeEpoch == token.runtimeEpoch
        ? publishedOwnerId : token.projectSessionId;
    const auto live = Detail::runtimeLiveOwnerRevision(ownerId);
    return installCompiledWorld(std::move(compiled.artifact), token, ownerId, true,
                                live.found ? live.currentRevision : 0);
}

void servicePendingCurrentWorld() noexcept
{
    if (!generationHold)
        return;
    if (!Detail::runtimeIsActive() && generationTestBackend == nullptr)
        return;
    if (!generationHold->committed) {
        const auto progressed = prepareAndCommitHold(*generationHold);
        if (progressed.status != CurrentWorldSyncStatus::Pending) {
            if (progressed.status != CurrentWorldSyncStatus::Ok) {
                enqueueCompletion(progressed);
                generationHold.reset();
            }
            return;
        }
        if (!generationHold->committed)
            return;
    }
    if (generationTestBackend
        && generationTestBackend->openImmediatelyOnCommitOk)
        generationHold->testOpen = true;
    if (!generationHold->testOpen && generationTestBackend == nullptr
        && installBackend == nullptr) {
        Detail::runtimePumpNullMixer(1);
        Detail::runtimeDrainProjectGenerationAcks();
    }
    const auto opened = finishOpenIfReady(*generationHold);
    if (opened.pending())
        return;
    enqueueCompletion(opened);
    generationHold.reset();
}

std::optional<CurrentWorldSyncResult> discardPendingCurrentWorld(
    PendingCurrentWorldDisposition disposition) noexcept
{
    if (!generationHold && completedWorlds.empty())
        return std::nullopt;

    CurrentWorldSyncResult result;
    if (generationHold) {
        result.token = generationHold->token;
        if (generationHold->prepared.Valid())
            Detail::runtimeCancelPreparedProjectGeneration(
                std::move(generationHold->prepared));
        if (generationTestBackend)
            generationTestBackend->cancelCalled = true;
        generationHold.reset();
    } else {
        result.token = completedWorlds.back().token;
    }
    result.status = disposition == PendingCurrentWorldDisposition::Superseded
        ? CurrentWorldSyncStatus::Superseded
        : CurrentWorldSyncStatus::Cancelled;
    if (discardObserver)
        discardObserver(result.token, Detail::runtimeIsActive());
    completedWorlds.clear();
    return result;
}

std::optional<CurrentWorldSyncResult> takeCurrentWorldSyncCompletion() noexcept
{
    if (completedWorlds.empty())
        return std::nullopt;
    auto completion = completedWorlds.front();
    completedWorlds.pop_front();
    return completion;
}

void setCurrentWorldInstallBackendForTesting(
    CurrentWorldInstallBackend backend) noexcept
{
    installBackend = backend;
}

void setProjectGenerationTestBackendForTesting(
    ProjectGenerationTestBackend* backend) noexcept
{
    generationTestBackend = backend;
}

void resetCurrentWorldSyncStateForTesting() noexcept
{
    if (generationHold && generationHold->prepared.Valid())
        Detail::runtimeCancelPreparedProjectGeneration(
            std::move(generationHold->prepared));
    generationHold.reset();
    publishedOwnerId = 0;
    publishedRuntimeEpoch = 0;
    publishedEffects.reset();
    publishedVoiceEffects.clear();
    publishedVoiceEffectEpoch = 0;
    publishedVoiceEffectGeneration = 0;
    publishedBusEffectValues.clear();
    completedWorlds.clear();
    installBackend = nullptr;
    discardObserver = nullptr;
    generationTestBackend = nullptr;
}

void setCurrentWorldDiscardObserverForTesting(
    CurrentWorldDiscardObserver observer) noexcept
{
    discardObserver = observer;
}

bool queryRuntimeLiveOwnerRevision(std::uint64_t ownerId,
                                   std::uint64_t* outRevision) noexcept
{
    const auto live = Detail::runtimeLiveOwnerRevision(ownerId);
    if (!live.found)
        return false;
    if (outRevision)
        *outRevision = live.currentRevision;
    return true;
}

bool updateLivePreviewBusEffect(std::uint64_t runtimeEpoch, std::uint64_t authoringRevision,
    harken::studio::types::ObjectId busId,
    harken::studio::types::ObjectId definitionId, const PreviewEffectSource& effect)
{
    if (!publishedEffects || runtimeEpoch != publishedRuntimeEpoch) return false;
    const auto installed = std::find_if(publishedEffects->begin(),
        publishedEffects->end(), [&](const auto& candidate) {
            return candidate.effectId == effect.id && candidate.busId == busId;
        });
    if (installed == publishedEffects->end()) return false;
    auto& applied = publishedBusEffectValues.at(effect.id);
    const auto& parameters = installed->definitionId == definitionId && installed->effectType == effect.type
        ? effect.parameters : applied.parameters;
    if (parameters == applied.parameters && effect.bypass == applied.bypass) return true;
    if (Detail::runtimeSetPreviewBusEffect(publishedOwnerId, busId, effect.id,
            installed->effectType, parameters, effect.bypass) != kEngineOk) return false;
    if (generationHold && !generationHold->committed) {
        if (generationHold->token.authoringRevision < authoringRevision) {
            const auto superseded = discardPendingCurrentWorld(PendingCurrentWorldDisposition::Superseded);
            if (superseded) enqueueCompletion(*superseded);
        } else if (generationHold->prepared.Valid()) {
            Detail::runtimeCancelPreparedProjectGeneration(std::move(generationHold->prepared));
        }
    }
    applied.parameters = parameters;
    applied.bypass = effect.bypass;
    return true;
}

bool captureInstalledPreviewEffects(std::uint64_t generation,
    harken::studio::types::ObjectId soundId, std::vector<PreviewEffectSource>& effects)
{
    effects.clear();
    for (auto it = publishedVoiceEffects.begin(); it != publishedVoiceEffects.end();) {
        if (!Detail::runtimeHasPreviewWorldGeneration(it->first))
            it = publishedVoiceEffects.erase(it);
        else ++it;
    }
    const auto world = publishedVoiceEffects.find(generation);
    if (world == publishedVoiceEffects.end()) return false;
    const auto sound = world->second.find(soundId);
    if (sound == world->second.end()) return false;
    effects = sound->second;
    return true;
}

} // namespace harken::studio::bridge
