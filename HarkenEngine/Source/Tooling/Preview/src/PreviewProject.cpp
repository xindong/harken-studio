#include <harken_preview/PreviewProject.h>

#include "ProjectGenerationTransaction.h"
#include "HarkenApiInternal.h"
#include "Engine.h"
#include "harken/core/ChannelLayout.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/PcmRegistration.h"
#include "harken/media/PackLogicImage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace {

constexpr bool enabled = HARKEN_DEVELOPMENT_PREVIEW != 0;

struct PreparedProject {
    harken::CurrentWorldBodyImage world;
    std::vector<VnBusId> ids;
    std::vector<std::uint32_t> generations, parents, targets, feeders, emittedFeeders;
    std::vector<std::uint8_t> alive, levelApply, emitted;
    std::vector<float> levels, sendLevelDb;
    std::vector<float> authoredVolume, authoredMakeUpGain, authoredOutputVolume;
    harken::ProspectiveTopologyView topology;
    harken::PreparedProjectGenerationV1 transaction;
};

std::unordered_map<VnPreviewProjectHandle, std::unique_ptr<PreparedProject>> prepared;
VnPreviewProjectHandle nextHandle = 1;

harken::Engine* ActiveEngine() {
    std::erase_if(prepared, [](const auto& entry) { return !entry.second->transaction.Valid(); });
    return enabled ? harken::ActiveEngineForProjectGenerationV1() : nullptr;
}

VnPreviewPrepareStatus PrepareStatus(harken::ProjectGenerationPrepareStatusV1 status) {
    using Status = harken::ProjectGenerationPrepareStatusV1;
    switch (status) {
    case Status::Ok: return VN_PREVIEW_PREPARE_OK;
    case Status::InvalidView: return VN_PREVIEW_PREPARE_INVALID;
    case Status::Backpressure: return VN_PREVIEW_PREPARE_BACKPRESSURE;
    case Status::Capacity: return VN_PREVIEW_PREPARE_CAPACITY;
    case Status::CapabilityUnavailable: return VN_PREVIEW_PREPARE_UNSUPPORTED;
    case Status::DeviceError: return VN_PREVIEW_PREPARE_DEVICE_ERROR;
    case Status::InvalidWorld: return VN_PREVIEW_PREPARE_INVALID_WORLD;
    }
    return VN_PREVIEW_PREPARE_INVALID;
}

VnPreviewCommitStatus CommitStatus(harken::ProjectGenerationCommitStatusV1 status) {
    using Status = harken::ProjectGenerationCommitStatusV1;
    switch (status) {
    case Status::Ok: return VN_PREVIEW_COMMIT_OK;
    case Status::Backpressure: return VN_PREVIEW_COMMIT_BACKPRESSURE;
    case Status::InvalidPrepared: return VN_PREVIEW_COMMIT_INVALID;
    case Status::DeviceError: return VN_PREVIEW_COMMIT_DEVICE_ERROR;
    }
    return VN_PREVIEW_COMMIT_INVALID;
}

bool BuildTopology(const VnPreviewProjectDescV1& desc, const float* sendLevelsDb,
                   const harken::Engine& engine, PreparedProject& project) {
    const auto maxBuses = harken::LiveMaxBusesV1(engine);
    const auto sendSlots = harken::LiveBusSendSlotsPerBusV1(engine);
    if (maxBuses == 0 || sendSlots == 0 || desc.busCount == 0
        || desc.busCount > maxBuses || !desc.buses
        || desc.sendCount > static_cast<std::uint64_t>(desc.busCount) * sendSlots
        || (desc.sendCount != 0 && !desc.sends)) return false;
    project.ids.resize(maxBuses);
    project.generations.assign(maxBuses, 1);
    project.parents.assign(maxBuses, UINT32_MAX);
    project.alive.assign(maxBuses, 0);
    project.targets.assign(static_cast<std::size_t>(maxBuses) * sendSlots, UINT32_MAX);
    project.levels.assign(project.targets.size(), 0);
    project.sendLevelDb.assign(project.targets.size(), 0);
    project.levelApply.assign(project.targets.size(), 0);
    project.feeders.assign(maxBuses, 0);
    project.emittedFeeders.assign(maxBuses, 0);
    project.emitted.assign(maxBuses, 0);
    for (std::uint32_t i = 0; i < desc.busCount; ++i) {
        if (harken::IsNull(desc.buses[i].id)
            || std::find(project.ids.begin(), project.ids.begin() + i, desc.buses[i].id)
                != project.ids.begin() + i) return false;
        project.ids[i] = desc.buses[i].id;
        project.alive[i] = 1;
    }
    const auto index = [&](VnBusId id) {
        const auto end = project.ids.begin() + desc.busCount;
        const auto it = std::find(project.ids.begin(), end, id);
        return it == end ? UINT32_MAX : static_cast<std::uint32_t>(it - project.ids.begin());
    };
    if (index(VN_BUS_MASTER) == UINT32_MAX) return false;
    for (std::uint32_t i = 0; i < desc.busCount; ++i) {
        const auto parent = project.ids[i] == VN_BUS_MASTER ? i : index(desc.buses[i].parent);
        if (parent == UINT32_MAX || (parent == i && project.ids[i] != VN_BUS_MASTER)) return false;
        project.parents[i] = parent;
    }
    std::vector<std::uint32_t> used(desc.busCount);
    for (std::uint32_t i = 0; i < desc.sendCount; ++i) {
        const auto from = index(desc.sends[i].source), to = index(desc.sends[i].target);
        if (from == UINT32_MAX || to == UINT32_MAX || from == to || used[from] == sendSlots)
            return false;
        const auto first = project.targets.begin() + static_cast<std::size_t>(from) * sendSlots;
        if (std::find(first, first + used[from], to) != first + used[from]) return false;
        const auto flat = static_cast<std::size_t>(from) * sendSlots + used[from]++;
        first[used[from] - 1] = to;
        if (sendLevelsDb != nullptr) {
            const float db = sendLevelsDb[i];
            if (!std::isfinite(db) || db < -96.0f || db > 24.0f) return false;
            const float gain = std::pow(10.0f, db / 20.0f);
            if (!std::isfinite(gain)) return false;
            project.sendLevelDb[flat] = db == 0.0f ? 0.0f : db;
            project.levels[flat] = gain;
            project.levelApply[flat] = 1;
        }
    }
    project.topology = {desc.busCount, project.ids.data(), project.generations.data(),
        project.parents.data(), project.alive.data(), project.targets.data(),
        project.levels.data(), project.levelApply.data(), project.feeders.data(),
        project.emittedFeeders.data(), project.emitted.data()};
    project.topology.sendLevelsDb = sendLevelsDb != nullptr ? project.sendLevelDb.data() : nullptr;
    return true;
}

bool ValidateVolumePublication(const VnPreviewProjectDescV2& desc,
                                const harken::Engine& engine,
                                const float* sendLevelsDb = nullptr,
                                bool authoredSends = false) {
    const auto& base = desc.base;
    if (base.busCount < 2 || base.busCount > engine.config.maxBuses ||
        base.busCount > 4096 || base.buses == nullptr || desc.busVolumes == nullptr ||
        desc.busVolumeCount != base.busCount ||
        base.sendCount > static_cast<std::uint64_t>(base.busCount) *
            engine.config.busSendSlotsPerBus ||
        (base.sendCount != 0 && base.sends == nullptr) ||
        desc.profileVintage != 1 || desc.renderSampleRateHz == 0 ||
        desc.renderSampleRateHz > 384000 || desc.mixLayout == 0 ||
        desc.deviceLayout == 0 ||
        (desc.runtimeCapabilityMask & ~harken::busfingerprint::kKnownTargetCapabilityMaskV1) != 0 ||
        (desc.runtimeCapabilityMask & harken::busfingerprint::kRequiredBusCapabilityMaskV1) !=
            harken::busfingerprint::kRequiredBusCapabilityMaskV1 ||
        desc.renderSampleRateHz != engine.config.mixRate.SampleRate() ||
        desc.mixLayout != engine.config.mixChannelLayout ||
        desc.deviceLayout != engine.config.deviceChannelLayout ||
        base.compileTargetProfileFingerprint != harken::busfingerprint::ProfileFingerprintV1(
            desc.profileVintage, desc.renderSampleRateHz, desc.mixLayout,
            desc.deviceLayout, desc.runtimeCapabilityMask) ||
        base.buses[base.busCount - 1].id != VN_BUS_MASTER ||
        !harken::IsNull(base.buses[base.busCount - 1].parent)) return false;

    std::map<VnBusId, std::uint32_t> indexes;
    for (std::uint32_t i = 0; i < base.busCount; ++i)
        if (harken::IsNull(base.buses[i].id) ||
            !indexes.emplace(base.buses[i].id, i).second) return false;
    const auto busIndex = [&](VnBusId id) {
        const auto found = indexes.find(id);
        return found == indexes.end() ? UINT32_MAX : found->second;
    };
    const auto convertible = [](float db) {
        return std::isfinite(db) && std::isfinite(std::pow(10.0f, db / 20.0f));
    };
    for (std::uint32_t i = 0; i < base.busCount; ++i) {
        const auto& bus = base.buses[i];
        const auto& volume = desc.busVolumes[i];
        if (volume.id != bus.id || volume.reserved0 != 0 ||
            !std::isfinite(volume.volumeDb) || volume.volumeDb < -96.0f ||
            volume.volumeDb > 24.0f ||
            !convertible(volume.makeUpGainDb) ||
            !convertible(volume.outputBusVolumeDb) ||
            !convertible(volume.volumeDb + volume.makeUpGainDb) ||
            !convertible(volume.volumeDb + volume.makeUpGainDb +
                         volume.outputBusVolumeDb) ||
            (i + 1 < base.busCount &&
                (harken::IsNull(bus.parent) || busIndex(bus.parent) == UINT32_MAX)) ||
            (i + 1 == base.busCount &&
                (volume.volumeDb != 0.0f || volume.makeUpGainDb != 0.0f ||
                 volume.outputBusVolumeDb != 0.0f))) return false;
    }
    for (std::uint32_t i = 0; i < base.sendCount; ++i) {
        const auto& send = base.sends[i];
        if (authoredSends && (!std::isfinite(sendLevelsDb[i]) ||
            sendLevelsDb[i] < -96.0f || sendLevelsDb[i] > 24.0f ||
            !std::isfinite(std::pow(10.0f, sendLevelsDb[i] / 20.0f))))
            return false;
        if (busIndex(send.source) >= base.busCount - 1 ||
            busIndex(send.target) >= base.busCount - 1 ||
            send.source == send.target ||
            (i != 0 && !(base.sends[i - 1].source < send.source ||
                (base.sends[i - 1].source == send.source &&
                 base.sends[i - 1].target < send.target)))) return false;
    }
    std::vector<std::uint32_t> indegree(base.busCount, 0);
    for (std::uint32_t i = 0; i + 1 < base.busCount; ++i) {
        const auto parent = busIndex(base.buses[i].parent);
        if (parent < base.busCount - 1) ++indegree[parent];
    }
    for (std::uint32_t i = 0; i < base.sendCount; ++i)
        ++indegree[busIndex(base.sends[i].target)];
    // The compiler emits the smallest ready bus at each topological step.
    for (std::uint32_t i = 0; i + 1 < base.busCount; ++i) {
        VnBusId smallest{};
        bool found = false;
        for (std::uint32_t candidate = i; candidate + 1 < base.busCount; ++candidate) {
            const auto id = base.buses[candidate].id;
            if (indegree[candidate] == 0 && (!found || id < smallest)) {
                smallest = id;
                found = true;
            }
        }
        if (!found || base.buses[i].id != smallest) return false;
        const auto parent = busIndex(base.buses[i].parent);
        if (parent < base.busCount - 1) --indegree[parent];
        for (std::uint32_t send = 0; send < base.sendCount; ++send)
            if (base.sends[send].source == smallest)
                --indegree[busIndex(base.sends[send].target)];
    }

    harken::PackCanonicalImage image;
    image.hasBusDefinitionSnapshot = true;
    auto& header = image.busDefinitionSnapshotHeader;
    header.schemaVersion = 2;
    header.storageModelVersion = 2;
    header.publicationCapabilityVintage = authoredSends ? 4 : 3;
    header.profileVintage = desc.profileVintage;
    header.renderSampleRateHz = desc.renderSampleRateHz;
    header.mixLayout = desc.mixLayout;
    header.deviceLayout = desc.deviceLayout;
    header.runtimeCapabilityMask = desc.runtimeCapabilityMask;
    header.compileTargetProfileFingerprint = base.compileTargetProfileFingerprint;
    header.busCount = base.busCount;
    header.sendCount = base.sendCount;
    header.downmixCount = base.busCount;
    header.coefficientCount = base.busCount * 64u;
    image.busDefinitionBusesV2.reserve(base.busCount);
    image.busDefinitionSendsV2.reserve(base.sendCount);
    image.busDefinitionDownmixesV2.reserve(base.busCount);
    image.busDefinitionCoefficients.reserve(static_cast<std::size_t>(base.busCount) * 64u);
    for (std::uint32_t i = 0; i < base.busCount; ++i) {
        const auto& bus = base.buses[i];
        const auto& volume = desc.busVolumes[i];
        const bool root = i + 1 == base.busCount;
        const auto output = root ? desc.deviceLayout : desc.mixLayout;
        image.busDefinitionBusesV2.push_back({bus.id, bus.parent, i,
            volume.volumeDb, volume.makeUpGainDb, volume.outputBusVolumeDb,
            harken::busfingerprint::kNeutralFaderAfterSlotV1,
            output, 0, 0, i, 1});
        image.busDefinitionDownmixesV2.push_back({bus.id, desc.mixLayout,
            output, i * 64u, 64});
        std::array<float, 64> coefficients{};
        harken::DefaultDownmixMatrix(desc.mixLayout, output, coefficients.data());
        image.busDefinitionCoefficients.insert(image.busDefinitionCoefficients.end(),
            coefficients.begin(), coefficients.end());
    }
    for (std::uint32_t i = 0; i < base.sendCount; ++i)
        image.busDefinitionSendsV2.push_back({base.sends[i].source,
             base.sends[i].target, authoredSends
                 ? (sendLevelsDb[i] == 0.0f ? 0.0f : sendLevelsDb[i]) : 0.0f});
    return harken::CanonicalVolumeBusFingerprint(image) == base.publishedBusFingerprint;
}

}

VnPreviewPrepareStatus PrepareProjectImpl(
    const VnPreviewProjectDescV1* desc, const VnPreviewBusVolumeV2* busVolumes,
    std::uint32_t volumeCount, std::uint32_t expectedSize,
    const float* sendLevelsDb, bool authoredSends,
    VnPreviewProjectHandle* outHandle) {
    if (outHandle) *outHandle = 0;
    auto* engine = ActiveEngine();
    if (!engine) return VN_PREVIEW_PREPARE_UNAVAILABLE;
    if (!desc || !outHandle || desc->structSize != expectedSize || !desc->body
        || desc->bodyBytes == 0 || desc->bodyBytes > UINT64_C(67108864)
        || nextHandle == std::numeric_limits<VnPreviewProjectHandle>::max())
        return VN_PREVIEW_PREPARE_INVALID;
    try {
        auto project = std::make_unique<PreparedProject>();
        const auto decoded = project->world.DecodeWithResult(desc->body,
            static_cast<std::size_t>(desc->bodyBytes), desc->bodyVintage,
            harken::CurrentWorldDecodeMode::StrictArtifact);
        if (decoded == harken::CurrentWorldBodyDecodeResult::Capacity)
            return VN_PREVIEW_PREPARE_CAPACITY;
        if (decoded != harken::CurrentWorldBodyDecodeResult::Ok)
            return VN_PREVIEW_PREPARE_INVALID_WORLD;
        if (!BuildTopology(*desc, sendLevelsDb, *engine, *project))
            return VN_PREVIEW_PREPARE_INVALID;
        if (busVolumes != nullptr) {
            if (volumeCount != desc->busCount) return VN_PREVIEW_PREPARE_INVALID;
            project->authoredVolume.reserve(volumeCount);
            project->authoredMakeUpGain.reserve(volumeCount);
            project->authoredOutputVolume.reserve(volumeCount);
            for (std::uint32_t i = 0; i < volumeCount; ++i) {
                const auto& row = busVolumes[i];
                if (row.id != desc->buses[i].id || row.reserved0 != 0 ||
                    !std::isfinite(row.volumeDb) || !std::isfinite(row.makeUpGainDb) ||
                    !std::isfinite(row.outputBusVolumeDb))
                    return VN_PREVIEW_PREPARE_INVALID;
                project->authoredVolume.push_back(row.volumeDb);
                project->authoredMakeUpGain.push_back(row.makeUpGainDb);
                project->authoredOutputVolume.push_back(row.outputBusVolumeDb);
            }
        }
        harken::ProjectGenerationInstallViewV1 view;
        view.token = {desc->projectSessionId, desc->authoringRevision,
            desc->runtimeEpoch, desc->submissionSerial};
        view.publishedBusFingerprint = desc->publishedBusFingerprint;
        view.compileTargetProfileFingerprint = desc->compileTargetProfileFingerprint;
        view.ownerId = desc->ownerId;
        view.ownerBaseRevision = desc->ownerBaseRevision;
        view.ownerRevision = desc->ownerRevision;
        view.compiledWorld = *project->world.ProductionView();
        view.prospective = &project->topology;
        if (busVolumes != nullptr) {
            view.publicationVintage = authoredSends ? 4 : 3;
            view.authoredBusIds = project->ids.data();
            view.authoredBusVolumeDb = project->authoredVolume.data();
            view.authoredBusMakeUpGainDb = project->authoredMakeUpGain.data();
            view.authoredBusOutputVolumeDb = project->authoredOutputVolume.data();
            view.authoredBusCount = volumeCount;
        }
        const auto result = harken::PrepareProjectGenerationV1(*engine, view, project->transaction);
        if (result.status != harken::ProjectGenerationPrepareStatusV1::Ok)
            return PrepareStatus(result.status);
        const auto handle = nextHandle++;
        prepared.emplace(handle, std::move(project));
        *outHandle = handle;
        return VN_PREVIEW_PREPARE_OK;
    } catch (const std::bad_alloc&) {
        return VN_PREVIEW_PREPARE_CAPACITY;
    } catch (...) {
        return VN_PREVIEW_PREPARE_INVALID;
    }
}

extern "C" {

VnPreviewPrepareStatus VnPreview_PrepareProject(
    const VnPreviewProjectDescV1* desc, VnPreviewProjectHandle* outHandle) {
    return PrepareProjectImpl(desc, nullptr, 0, sizeof(VnPreviewProjectDescV1),
                              nullptr, false, outHandle);
}

VnPreviewPrepareStatus VnPreview_PrepareProjectV2(
    const VnPreviewProjectDescV2* desc, VnPreviewProjectHandle* outHandle) {
    if (outHandle != nullptr) *outHandle = 0;
    if (desc == nullptr || desc->base.structSize != sizeof(VnPreviewProjectDescV2))
        return VN_PREVIEW_PREPARE_INVALID;
    if (desc->reserved0 != 0 || desc->busVolumes == nullptr)
        return VN_PREVIEW_PREPARE_INVALID;
    auto* engine = ActiveEngine();
    if (engine == nullptr) return VN_PREVIEW_PREPARE_UNAVAILABLE;
    try {
        if (!ValidateVolumePublication(*desc, *engine)) return VN_PREVIEW_PREPARE_INVALID;
    } catch (...) {
        return VN_PREVIEW_PREPARE_INVALID;
    }
    return PrepareProjectImpl(&desc->base, desc->busVolumes, desc->busVolumeCount,
                               sizeof(VnPreviewProjectDescV2), nullptr, false, outHandle);
}

VnPreviewPrepareStatus VnPreview_PrepareProjectV3(
    const VnPreviewProjectDescV3* desc, VnPreviewProjectHandle* outHandle) {
    if (outHandle != nullptr) *outHandle = 0;
    if (desc == nullptr || desc->base.base.structSize != sizeof(VnPreviewProjectDescV3) ||
        desc->base.reserved0 != 0 || desc->reserved1 != 0 ||
        desc->base.busVolumes == nullptr ||
        desc->busSendLevelCount != desc->base.base.sendCount ||
        (desc->busSendLevelCount != 0 && desc->busSendLevelsDb == nullptr))
        return VN_PREVIEW_PREPARE_INVALID;
    auto* engine = ActiveEngine();
    if (engine == nullptr) return VN_PREVIEW_PREPARE_UNAVAILABLE;
    try {
        if (!ValidateVolumePublication(desc->base, *engine,
                desc->busSendLevelsDb, true))
            return VN_PREVIEW_PREPARE_INVALID;
    } catch (...) {
        return VN_PREVIEW_PREPARE_INVALID;
    }
    return PrepareProjectImpl(&desc->base.base, desc->base.busVolumes,
        desc->base.busVolumeCount, sizeof(VnPreviewProjectDescV3),
        desc->busSendLevelsDb, true, outHandle);
}

VnPreviewCommitStatus VnPreview_CommitProject(VnPreviewProjectHandle handle,
                                            uint64_t* outGeneration) {
    if (outGeneration) *outGeneration = 0;
    auto* engine = ActiveEngine();
    if (!engine) return VN_PREVIEW_COMMIT_UNAVAILABLE;
    const auto found = prepared.find(handle);
    if (found == prepared.end() || !outGeneration) return VN_PREVIEW_COMMIT_INVALID;
    const auto result = harken::CommitPreparedProjectGenerationV1(*engine, found->second->transaction);
    if (result.status == harken::ProjectGenerationCommitStatusV1::Ok) {
        *outGeneration = result.installedWorldGeneration;
        prepared.erase(found);
    }
    return CommitStatus(result.status);
}

void VnPreview_CancelProject(VnPreviewProjectHandle handle) { prepared.erase(handle); }

uint32_t VnPreview_DrainProjectAcks(void) {
    auto* engine = ActiveEngine();
    return engine ? harken::DrainProjectGenerationInstallAcksV1(*engine) : 0;
}

uint32_t VnPreview_ProjectIsOpen(uint64_t generation) {
    auto* engine = ActiveEngine();
    return engine && generation != 0 && harken::CurrentWorldBusAdmissionIsOpenV1(*engine, generation);
}

uint32_t VnPreview_QueryOwnerRevision(uint64_t ownerId, uint64_t* outRevision) {
    if (outRevision) *outRevision = 0;
    auto* engine = ActiveEngine();
    return engine && outRevision && harken::QueryLiveTopologyOwnerRevisionV1(*engine, ownerId, outRevision);
}

uint32_t VnPreview_HasPlayableConsumers(void) {
    auto* engine = ActiveEngine();
    return engine && harken::HasCurrentWorldPlayableConsumers(*engine);
}

uint32_t VnPreview_HasAction(VnObjectId actionId) {
    return ActiveEngine() && harken::detail::DirectWorldHasActionProgram(actionId);
}

void VnPreview_PumpNullMixer(uint32_t blockCount) {
    if (auto* engine = ActiveEngine()) harken::PumpNullMixerForProjectGenerationV1(*engine, blockCount);
}

VnResult VnPreview_RegisterPcm(VnSoundId id, const VnPCMDesc* desc,
    uint64_t loopStartFrame, uint64_t loopEndFrame, uint64_t* revision) {
    if (revision) *revision = 0;
    if (!ActiveEngine()) return VN_NOT_INITIALIZED;
    if (!desc || !revision) return VN_INVALID_ARG;
    return harken::RegisterPcmWithLoopRegionV1(id, *desc, {loopStartFrame, loopEndFrame}, revision);
}

VnResult VnPreview_PlayVoiceRaw(const VnPlayDesc* desc, VnVoiceHandle* outHandle) {
    if (!ActiveEngine()) return VN_NOT_INITIALIZED;
    return harken::detail::PlayVoiceRaw(desc, outHandle);
}

VnResult VnPreview_RefreshVoiceBindings(uint64_t voiceInstance) {
    return enabled ? harken::RefreshPreviewVoiceBindings(voiceInstance) : VN_NOT_INITIALIZED;
}

VnResult VnPreview_SetVoiceProperty(uint64_t voiceInstance,
    uint32_t property, float value, VnBusId sendBusId) {
    if (!ActiveEngine()) return VN_NOT_INITIALIZED;
    if (property > UINT8_MAX) return VN_INVALID_ARG;
    return harken::SetPreviewVoiceProperty(voiceInstance,
        static_cast<harken::playableproperties::PlayablePropertyIdV1>(property), value, sendBusId);
}

}
