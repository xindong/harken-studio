#include "Core/Build/CurrentWorldCompile.h"
#include "Core/Build/PackagingBridge.h"

#include <harken_project_schema/FrontEnd.h>
#include <harken_studio_build/WorldCodec.h>
#include <harken_studio_build/MediaContentRevision.h>
#include <harken_bus_compiler/BusCompiler.h>

#include <QDateTime>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace HarkenEditor::Build {

CurrentWorldSyncRequestPtr CurrentWorldRequestCache::update(
    CurrentWorldSyncRequestPtr replacement,
    const std::vector<CurrentWorldScalarUpdate>& updates,
    Bridge::CurrentWorldSyncToken token)
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
        if (property != Property::Volume && property != Property::Pitch)
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
        default: return {};
        }
    }
    request_ = candidate;
    token_ = token;
    return candidate;
}

void CurrentWorldRequestCache::acknowledgePublication(Bridge::CurrentWorldSyncToken token)
{
    if (!request_ || !(token == token_) || !request_->soundPreparation)
        return;
    const auto& current = *request_->soundPreparation;
    if (std::none_of(current.sounds.begin(), current.sounds.end(),
                     [](const auto& sound) { return !sound.samples.empty(); }))
        return;
    auto preparation = std::make_shared<Bridge::CurrentWorldSoundPreparation>();
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
                result.status = Bridge::CurrentWorldSyncStatus::MediaUnavailable;
                result.relatedStableId = row.stableId;
                return result;
            }
            row.contentRevision = suppliedRevision->second;
            continue;
        }
        if (prepareForInstall) {
            result.status = Bridge::CurrentWorldSyncStatus::MediaUnavailable;
            result.relatedStableId = row.stableId;
            return result;
        }
        const auto source = std::find_if(
            request.soundSources.begin(), request.soundSources.end(),
            [&](const auto& candidate) {
                return candidate.stableId == row.stableId;
            });
        if (source == request.soundSources.end() || source->sourceId == 0) {
            result.status = Bridge::CurrentWorldSyncStatus::MediaUnavailable;
            result.relatedStableId = row.stableId;
            return result;
        }
        const QString path = Bridge::mediaRegistryFindPath(source->sourceId);
        std::vector<float> interleaved;
        int channelCount = 0;
        int sampleRate = 0;
        if (path.isEmpty()
            || !Bridge::decodeAudioFileInterleaved(path, interleaved, channelCount,
                                           sampleRate)
            || channelCount <= 0 || sampleRate <= 0
            || interleaved.empty()
            || interleaved.size() % static_cast<std::size_t>(channelCount)
                != 0) {
            result.status = Bridge::CurrentWorldSyncStatus::MediaUnavailable;
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
            result.status = Bridge::CurrentWorldSyncStatus::MediaUnavailable;
            result.relatedStableId = row.stableId;
            return result;
        }
    }

    auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        *frontEnd.project, request.variables, request.materializations,
        request.containers, {}, request.transitions, request.actions,
        &request.busResult->idSet,
        harkencurrentworldcompiler::CompileCapacityModeV1::AuthoredContent,
        request.propertyAuthoredOrders, request.reservedPropertyAuthoredOrders, request.duckingSends, request.includePreviewDucking);
    if (!compiled.ok() || !compiled.world) {
        result.status = Bridge::CurrentWorldSyncStatus::CompileFailed;
        result.relatedStableId = compiled.relatedStableId;
        return result;
    }
    result.status = Bridge::CurrentWorldSyncStatus::Ok;
    result.world = std::move(compiled.world);
    if (prepareForInstall) {
        result.projection = harkenbuscompiler::BuildPublishedBusProjectionV1(
            *request.busResult,
            harkenbuscompiler::BusPublicationCapabilityVintageV1::
                TopologyVolumeSendAndEffectsNeutralOtherProperties);
        if (!harkenstudiobuild::EncodeCurrentWorldPreviewBody(result.world->View(), result.body)) {
            result.status = Bridge::CurrentWorldSyncStatus::CompileFailed;
            result.world.reset();
        }
    }
    return result;
}

Bridge::CurrentWorldArtifactResult makeCurrentWorldArtifact(
    const CurrentWorldSyncRequest& request, CurrentWorldCompileResult compiled)
{
    Bridge::CurrentWorldArtifactResult result;
    result.status = compiled.status;
    result.relatedStableId = compiled.relatedStableId;
    if (!compiled.ok())
        return result;
    if (!request.busResult) {
        result.status = Bridge::CurrentWorldSyncStatus::CompileFailed;
        return result;
    }
    auto& artifact = result.artifact;
    auto projection = std::move(compiled.projection);
    artifact.body = std::move(compiled.body);
    if (artifact.body.empty()) {
        projection = harkenbuscompiler::BuildPublishedBusProjectionV1(
            *request.busResult,
            harkenbuscompiler::BusPublicationCapabilityVintageV1::
                TopologyVolumeSendAndEffectsNeutralOtherProperties);
        if (!harkenstudiobuild::EncodeCurrentWorldPreviewBody(compiled.world->View(), artifact.body)) {
            result.status = Bridge::CurrentWorldSyncStatus::CompileFailed;
            return result;
        }
    }
    artifact.bodyVintage = compiled.world->View().sourceBodyVintage;
    for (const auto& bus : projection.buses)
        artifact.buses.push_back({bus.busId, bus.parentId, bus.faderDb, bus.outputBusVolumeDb,
            bus.faderAfterSlot});
    for (const auto& send : projection.sends)
        artifact.sends.push_back({send.sourceBusId, send.targetBusId, send.sendLevelDb});
    for (const auto& effect : projection.effects) {
        const auto compiledEffect = std::find_if(request.busResult->effects.begin(),
            request.busResult->effects.end(),
            [&](const auto& candidate) { return candidate.effectId == effect.effectId; });
        artifact.effects.push_back({effect.busId, effect.effectId,
            compiledEffect == request.busResult->effects.end()
                ? harkenstudio::ObjectId{} : compiledEffect->definitionGuid,
            effect.slot, effect.effectType, effect.descriptorVintage, effect.flags,
            effect.parameterFirst, effect.parameterCount});
    }
    for (const auto& parameter : projection.effectParameters)
        artifact.effectParameters.push_back({parameter.index, parameter.value});
    artifact.voiceEffects = request.previewEffects;
    artifact.publishedBusFingerprint = projection.fingerprint;
    artifact.compileTargetProfileFingerprint = request.compileTargetProfile
        ? harkenprojectschema::compileTargetProfileFingerprintV1(*request.compileTargetProfile)
        : 0;
    if (projection.compileTargetProfile) {
        const auto& profile = *projection.compileTargetProfile;
        artifact.profile = Bridge::CurrentWorldArtifact::Profile{profile.vintage, profile.sampleRate,
            profile.mixLayout, profile.deviceLayout, profile.capabilityMask};
    }
    artifact.ownerBaseRevision = request.ownerBaseRevision;
    artifact.soundPreparation = request.soundPreparation;
    return result;
}

Bridge::CurrentWorldSyncResult syncCurrentWorld(CurrentWorldSyncRequestPtr request,
                                        Bridge::CurrentWorldSyncToken token)
{
    if (!request) {
        Bridge::CurrentWorldSyncResult result;
        result.token = token;
        result.status = Bridge::CurrentWorldSyncStatus::InvalidProject;
        return result;
    }
    return submitCurrentWorld(token, [&request] {
        return makeCurrentWorldArtifact(*request, compileCurrentWorldRequest(*request));
    });
}

Bridge::CurrentWorldSyncResult installCompiledCurrentWorld(
    CurrentWorldSyncRequestPtr request, CurrentWorldCompileResult compiled,
    Bridge::CurrentWorldSyncToken token)
{
    if (!request) {
        Bridge::CurrentWorldSyncResult result;
        result.token = token;
        result.status = compiled.status;
        result.relatedStableId = compiled.relatedStableId;
        return result;
    }
    return submitCompiledCurrentWorld(makeCurrentWorldArtifact(*request, std::move(compiled)), token);
}

Bridge::CurrentWorldSyncResult clearCurrentWorld(Bridge::CurrentWorldSyncToken token)
{
    return closeCurrentWorld(token, [] {
        CurrentWorldSyncRequest request;
        request.busResult = std::make_shared<const harkenbuscompiler::CompiledBusResultV1>();
        CurrentWorldCompileResult compiled;
        auto world = harkencurrentworldcompiler::CompileCurrentWorldV1(
            harkenprojectschema::CanonicalProject{}, {}, {}, {});
        if (!world.ok() || !world.world) {
            Bridge::CurrentWorldArtifactResult failed;
            failed.status = Bridge::CurrentWorldSyncStatus::CompileFailed;
            return failed;
        }
        compiled.status = Bridge::CurrentWorldSyncStatus::Ok;
        compiled.world = std::move(world.world);
        return makeCurrentWorldArtifact(request, std::move(compiled));
    });
}

}
