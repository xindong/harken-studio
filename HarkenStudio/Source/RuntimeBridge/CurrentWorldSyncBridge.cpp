#include <harken_project_schema/FrontEnd.h>
#include <harken_studio_build/WorldCodec.h>
#include <harken_studio_build/MediaContentRevision.h>
#include <harken_studio_build/VariableExecutionFingerprint.h>
#include <harken_studio_build/VariableDefinition.h>
#include <harken_bus_compiler/BusCompiler.h>

#include "CurrentWorldSyncBridge.h"
#include "NativeValues.h"
#include "CurrentWorldSyncBridgeInternal.h"
#include "PackagingBridge.h"
#include "PlaybackBridge.h"
#include "RuntimeLink.h"
#include "RuntimeLinkInternal.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <optional>
#include <vector>
#include <QFileInfo>
#include <QDateTime>

namespace HarkenEditor::Bridge {
namespace {

struct PreparedProjectGenerationHold {
    Detail::RuntimePreparedProject prepared;
    CurrentWorldSyncToken token;
    std::unique_ptr<harkencurrentworldcompiler::OwningCurrentWorldV1> world;
    VnPreviewProjectDescV1 view{};
    VnPreviewProjectDescV2 volumeView{};
    VnPreviewProjectDescV3 sendView{};
    std::vector<std::uint8_t> body;
    std::vector<VnPreviewBusV1> buses;
    std::vector<VnPreviewBusVolumeV2> volumes;
    std::vector<VnPreviewBusSendV1> sends;
    std::vector<float> sendLevelsDb;
    std::shared_ptr<const harkenbuscompiler::CompiledBusResultV1> busResult;
    std::shared_ptr<const CurrentWorldSoundPreparation> soundPreparation;
    harkenbuscompiler::PublishedBusProjectionV1 projection;
    std::uint64_t installedWorldGeneration = 0;
    bool committed = false;
    bool testOpen = false;
    bool soundPcmRegistered = false;
    bool emptyWorldClose = false;
};

std::optional<PreparedProjectGenerationHold> generationHold;
std::uint64_t publishedOwnerId = 0;
std::uint64_t publishedRuntimeEpoch = 0;
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
    if (!hold.busResult)
        return;
    std::vector<RuntimeBusDefinition> buses;
    buses.reserve(hold.projection.buses.size());
    for (const auto& bus : hold.projection.buses) {
        RuntimeBusDefinition definition;
        definition.id = bus.busId;
        definition.parentId = bus.parentId;
        buses.push_back(std::move(definition));
    }
    std::vector<RuntimeBusSendDefinition> sends;
    sends.reserve(hold.projection.sends.size());
    for (const auto& send : hold.projection.sends) {
        RuntimeBusSendDefinition definition;
        definition.sourceId = send.sourceBusId;
        definition.targetId = send.targetBusId;
        definition.levelDb = send.sendLevelDb;
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
    if (!hold.soundPreparation || hold.soundPcmRegistered)
        return result;
    for (const auto& sound : hold.soundPreparation->sounds) {
        const auto revision = hold.soundPreparation->contentRevisions.find(sound.binding.soundId);
        if (revision != hold.soundPreparation->contentRevisions.end()
            && revision->second != 0
            && Detail::runtimeRegisteredSoundRevision(sound.binding.soundId) == revision->second)
            continue;
        const auto registered = Detail::runtimeRegisterSoundPcm(
            sound.binding.soundId, sound.samples, sound.channels, sound.sampleRate,
            sound.loopStartFrame, sound.loopEndFrame);
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
    hold.soundPcmRegistered = true;
    return result;
}

CurrentWorldSyncResult prepareAndCommitHold(PreparedProjectGenerationHold& hold)
{
    CurrentWorldSyncResult result;
    result.token = hold.token;
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
        const harkenstudiobuild::WorldInstallResult installed =
            installBackend(hold.world->View());
        if (installed.status == harkenstudiobuild::WorldInstallStatus::Backpressure) {
            result.status = CurrentWorldSyncStatus::Pending;
            return result;
        }
        if (installed.status != harkenstudiobuild::WorldInstallStatus::Ok) {
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
            : Detail::runtimePrepareProjectGeneration(hold.sendView, hold.prepared);
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
    std::unique_ptr<harkencurrentworldcompiler::OwningCurrentWorldV1> world,
    CurrentWorldSyncRequestPtr request,
    CurrentWorldSyncToken token,
    std::uint64_t ownerId,
    bool emptyWorldClose = false,
    CurrentWorldCompileResult* compiled = nullptr,
    std::optional<std::uint64_t> ownerBaseRevision = std::nullopt)
{
    CurrentWorldSyncResult result;
    result.token = token;
    if (!request || !request->busResult) {
        result.status = CurrentWorldSyncStatus::CompileFailed;
        return result;
    }

    PreparedProjectGenerationHold hold;
    hold.token = token;
    hold.emptyWorldClose = emptyWorldClose;
    hold.world = std::move(world);
    hold.busResult = request->busResult;
    hold.soundPreparation = request->soundPreparation;
    if (compiled && !compiled->body.empty()) {
        hold.projection = std::move(compiled->projection);
        hold.body = std::move(compiled->body);
    } else {
        hold.projection = harkenbuscompiler::BuildPublishedBusProjectionV1(
            *request->busResult,
            harkenbuscompiler::BusPublicationCapabilityVintageV1::
                TopologyVolumeAndSendNeutralOtherProperties);
    }
    if (hold.body.empty()
        && !harkenstudiobuild::EncodeCurrentWorldShippingBody(hold.world->View(), hold.body)) {
        result.status = CurrentWorldSyncStatus::CompileFailed;
        return result;
    }
    for (const auto& bus : hold.projection.buses) {
        hold.buses.push_back({toNative(bus.busId), toNative(bus.parentId)});
        hold.volumes.push_back({toNative(bus.busId), bus.faderDb,
            bus.makeUpGainDb, bus.outputBusVolumeDb, 0});
    }
    for (const auto& send : hold.projection.sends) {
        hold.sends.push_back({toNative(send.sourceBusId), toNative(send.targetBusId)});
        hold.sendLevelsDb.push_back(send.sendLevelDb);
    }
    hold.view.structSize = sizeof(hold.view);
    hold.view.bodyVintage = harkenstudiobuild::kCurrentWorldShippingBodyVintage;
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
    hold.view.publishedBusFingerprint = hold.projection.fingerprint;
    hold.view.compileTargetProfileFingerprint =
        request->compileTargetProfile
            ? harkenprojectschema::compileTargetProfileFingerprintV1(
                  *request->compileTargetProfile)
            : 0;
    hold.view.ownerId = ownerId;
    hold.view.ownerBaseRevision = ownerBaseRevision.value_or(request->ownerBaseRevision);
    hold.view.ownerRevision = token.authoringRevision;
    hold.volumeView.base = hold.view;
    hold.volumeView.base.structSize = sizeof(hold.volumeView);
    hold.volumeView.busVolumes = hold.volumes.data();
    hold.volumeView.busVolumeCount = static_cast<std::uint32_t>(hold.volumes.size());
    if (hold.projection.compileTargetProfile) {
        const auto& profile = *hold.projection.compileTargetProfile;
        hold.volumeView.profileVintage = profile.vintage;
        hold.volumeView.renderSampleRateHz = profile.sampleRate;
        hold.volumeView.mixLayout = profile.mixLayout;
        hold.volumeView.deviceLayout = profile.deviceLayout;
        hold.volumeView.runtimeCapabilityMask = profile.capabilityMask;
    }
    hold.sendView.base = hold.volumeView;
    hold.sendView.base.base.structSize = sizeof(hold.sendView);
    hold.sendView.busSendLevelsDb = hold.sendLevelsDb.data();
    hold.sendView.busSendLevelCount = static_cast<std::uint32_t>(hold.sendLevelsDb.size());

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

CurrentWorldSyncRequestPtr CurrentWorldRequestCache::update(
    CurrentWorldSyncRequestPtr replacement,
    const std::vector<CurrentWorldScalarUpdate>& updates,
    CurrentWorldSyncToken token)
{
    if (replacement) {
        request_ = std::move(replacement);
        nodes_.clear();
        for (std::size_t index = 0; index < request_->project.nodes.size(); ++index)
            nodes_.emplace(request_->project.nodes[index].guid, index);
    } else if (!request_ || token.projectSessionId != token_.projectSessionId
               || token.runtimeEpoch != token_.runtimeEpoch) {
        return {};
    }
    auto candidate = std::make_shared<CurrentWorldSyncRequest>(*request_);
    if (candidate->busResult
        && candidate->busResult->projectGeneration != token.authoringRevision) {
        auto buses = std::make_shared<harkenbuscompiler::CompiledBusResultV1>(
            *candidate->busResult);
        buses->projectGeneration = token.authoringRevision;
        buses->deterministicBytes = harkenbuscompiler::SerializeCompiledBusResultV1(*buses);
        candidate->busResult = std::move(buses);
    }
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    for (const auto& update : updates) {
        const auto* claim = candidate->identityIndex
            ? candidate->identityIndex->find(update.stableId) : nullptr;
        if (!claim || claim->authoringGuid != update.guid
            || !std::isfinite(update.value))
            return {};
        const auto property = static_cast<Property>(update.propertyId);
        if (property != Property::Volume && property != Property::Pitch
            && property != Property::MakeUpGain)
            return {};
        const auto found = nodes_.find(update.guid);
        if (found == nodes_.end())
            continue;
        auto& node = candidate->project.nodes[found->second];
        if (update.present) node.presence.Set(property);
        else node.presence.Clear(property);
        switch (property) {
        case Property::Volume: node.volume = update.value; break;
        case Property::Pitch: node.pitch = update.value; break;
        case Property::MakeUpGain: node.makeUpGain = update.value; break;
        default: return {};
        }
    }
    request_ = candidate;
    token_ = token;
    return candidate;
}

void CurrentWorldRequestCache::acknowledgePublication(CurrentWorldSyncToken token)
{
    if (!request_ || !(token == token_) || !request_->soundPreparation)
        return;
    const auto& current = *request_->soundPreparation;
    if (std::none_of(current.sounds.begin(), current.sounds.end(),
                     [](const auto& sound) { return !sound.samples.empty(); }))
        return;
    auto preparation = std::make_shared<CurrentWorldSoundPreparation>();
    preparation->sources = current.sources;
    preparation->contentRevisions = current.contentRevisions;
    preparation->sounds.reserve(current.sounds.size());
    for (const auto& sound : current.sounds) {
        preparation->sounds.push_back({sound.binding, sound.sourcePath, {},
            sound.channels, sound.sampleRate, sound.loopStartFrame, sound.loopEndFrame});
    }
    auto request = std::make_shared<CurrentWorldSyncRequest>(*request_);
    request->soundPreparation = std::move(preparation);
    request_ = std::move(request);
}

CurrentWorldCompileResult compileCurrentWorldRequest(
    CurrentWorldSyncRequest& request, bool prepareForInstall)
{
    CurrentWorldCompileResult result;
    result.identityIndex = request.identityIndex;
    result.compileTargetProfile = request.compileTargetProfile;
    result.busResult = request.busResult;
    result.compileTargetProfileFingerprint = request.compileTargetProfile
        ? harkenprojectschema::compileTargetProfileFingerprintV1(
              *request.compileTargetProfile)
        : 0;
    if (!result.identityIndex || !result.compileTargetProfile
        || !request.busResult
        || request.busResult->identityIndex.get() != result.identityIndex.get()
        || request.busResult->compileTargetProfile.get()
            != result.compileTargetProfile.get())
        return result;
    if (prepareForInstall && request.soundPreparation) {
        for (const auto& source : request.soundPreparation->sources) {
            const QFileInfo file(source.path);
            if (file.isFile() != source.fileExists || file.size() != source.size
                || file.lastModified().toMSecsSinceEpoch() != source.lastModifiedMs) {
                result.requiresFullRefresh = true;
                return result;
            }
        }
    }
    const auto frontEnd = harkenprojectschema::CompileFrontEnd(request.project);
    if (!frontEnd.ok() || !frontEnd.project)
        return result;

    for (auto& row : request.materializations) {
        if (row.kind != harkenplayableproperties::ContentPropertyKindV1::Sound)
            continue;
        const auto suppliedRevision = request.soundContentRevisions.find(
            row.stableId);
        if (suppliedRevision != request.soundContentRevisions.end()) {
            if (suppliedRevision->second == 0) {
                result.status = CurrentWorldSyncStatus::MediaUnavailable;
                result.relatedStableId = row.stableId;
                return result;
            }
            row.contentRevision = suppliedRevision->second;
            continue;
        }
        if (prepareForInstall) {
            result.status = CurrentWorldSyncStatus::MediaUnavailable;
            result.relatedStableId = row.stableId;
            return result;
        }
        const auto source = std::find_if(
            request.soundSources.begin(), request.soundSources.end(),
            [&](const auto& candidate) {
                return candidate.stableId == row.stableId;
            });
        if (source == request.soundSources.end() || source->sourceId == 0) {
            result.status = CurrentWorldSyncStatus::MediaUnavailable;
            result.relatedStableId = row.stableId;
            return result;
        }
        const QString path = mediaRegistryFindPath(source->sourceId);
        std::vector<float> interleaved;
        int channelCount = 0;
        int sampleRate = 0;
        if (path.isEmpty()
            || !decodeAudioFileInterleaved(path, interleaved, channelCount,
                                           sampleRate)
            || channelCount <= 0 || sampleRate <= 0
            || interleaved.empty()
            || interleaved.size() % static_cast<std::size_t>(channelCount)
                != 0) {
            result.status = CurrentWorldSyncStatus::MediaUnavailable;
            result.relatedStableId = row.stableId;
            return result;
        }
        const std::uint64_t frameCount = interleaved.size()
            / static_cast<std::size_t>(channelCount);
        std::vector<const float*> channels(
            static_cast<std::size_t>(channelCount));
        for (int channel = 0; channel < channelCount; ++channel) {
            channels[static_cast<std::size_t>(channel)] =
                interleaved.data() + channel;
        }
        if (!harkenstudiobuild::ComputeMediaPcmContentRevisionV1(
                static_cast<std::uint32_t>(sampleRate),
                static_cast<std::uint32_t>(channelCount), frameCount,
                channels.data(), static_cast<std::uint32_t>(channelCount),
                &row.contentRevision)) {
            result.status = CurrentWorldSyncStatus::MediaUnavailable;
            result.relatedStableId = row.stableId;
            return result;
        }
    }

    auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        *frontEnd.project, request.variables, request.materializations,
        request.containers, {}, request.transitions, request.actions,
        &request.busResult->idSet,
        harkencurrentworldcompiler::CompileCapacityModeV1::AuthoredContent,
        request.propertyAuthoredOrders, request.reservedPropertyAuthoredOrders);
    if (!compiled.ok() || !compiled.world) {
        result.status = CurrentWorldSyncStatus::CompileFailed;
        result.relatedStableId = compiled.relatedStableId;
        return result;
    }
    result.status = CurrentWorldSyncStatus::Ok;
    result.world = std::move(compiled.world);
    if (prepareForInstall) {
        result.projection = harkenbuscompiler::BuildPublishedBusProjectionV1(
            *request.busResult,
            harkenbuscompiler::BusPublicationCapabilityVintageV1::
                TopologyVolumeAndSendNeutralOtherProperties);
        if (!harkenstudiobuild::EncodeCurrentWorldShippingBody(result.world->View(), result.body)) {
            result.status = CurrentWorldSyncStatus::CompileFailed;
            result.world.reset();
        }
    }
    return result;
}

CurrentWorldSyncResult installCompiledCurrentWorld(
    CurrentWorldSyncRequestPtr request, CurrentWorldCompileResult compiled,
    CurrentWorldSyncToken token)
{
    CurrentWorldSyncResult result;
    result.token = token;
    if (!request || !compiled.ok()) {
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
    return installCompiledWorld(std::move(compiled.world), std::move(request),
        token, token.projectSessionId, false, &compiled,
        live.found ? live.currentRevision : 0);
}

CurrentWorldSyncResult syncCurrentWorld(CurrentWorldSyncRequestPtr request,
                                        CurrentWorldSyncToken token)
{
    CurrentWorldSyncResult result;
    result.token = token;
    if (!request) {
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

    CurrentWorldCompileResult compiled = compileCurrentWorldRequest(*request);
    if (!compiled.ok()) {
        result.status = compiled.status;
        result.relatedStableId = compiled.relatedStableId;
        return result;
    }

    return installCompiledWorld(std::move(compiled.world), std::move(request),
                                token, token.projectSessionId, false, &compiled);
}

CurrentWorldSyncResult clearCurrentWorld(CurrentWorldSyncToken token)
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
    auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        harkenprojectschema::CanonicalProject{}, {}, {}, {});
    if (!compiled.ok() || !compiled.world) {
        result.status = CurrentWorldSyncStatus::CompileFailed;
        return result;
    }
    auto request = std::make_shared<CurrentWorldSyncRequest>();
    const auto ownerId = publishedOwnerId != 0 && publishedRuntimeEpoch == token.runtimeEpoch
        ? publishedOwnerId : token.projectSessionId;
    const auto live = Detail::runtimeLiveOwnerRevision(ownerId);
    request->ownerBaseRevision = live.found ? live.currentRevision : 0;
    request->busResult =
        std::make_shared<const harkenbuscompiler::CompiledBusResultV1>();
    return installCompiledWorld(std::move(compiled.world), std::move(request),
                                token, ownerId, true);
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

} // namespace HarkenEditor::Bridge
