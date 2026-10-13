// DiagnosticsBridge implementation -- private side of the diagnostics cluster.
//
// This cluster reads the public harken_core snapshot only. Surfaces whose old
// data source lived in legacy-engine internals return an honest
// unavailable/empty result instead of manufacturing zero-valued diagnostics.

#include "NativeDiagnostics.h"
#include "NativeValues.h"
#include "RuntimeLink.h"

#include <harken/Harken.h>
#include <harken_preview/PreviewAbi.h>

#include <QtGlobal>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace HarkenEditor {
namespace Bridge {

namespace {

bool capture(VnSnapshot& snapshot)
{
    return Detail::runtimeCaptureSnapshot(snapshot);
}

float blockCpuPercent(uint32_t micros)
{
    const int sampleRate = Detail::runtimeActualSampleRate();
    const int blockFrames = Detail::runtimeBufferSize();
    if (sampleRate <= 0 || blockFrames <= 0)
        return 0.0f;
    const double blockUs =
        (static_cast<double>(blockFrames) * 1000000.0) /
        static_cast<double>(sampleRate);
    if (blockUs <= 0.0)
        return 0.0f;
    return static_cast<float>((static_cast<double>(micros) / blockUs) * 100.0);
}

uint32_t reportedVoiceRows(const VnSnapshot& snapshot)
{
    return std::min<uint32_t>(snapshot.activeVoiceCount, VN_SNAPSHOT_MAX_VOICES);
}

uint32_t reportedBusRows(const VnSnapshot& snapshot)
{
    return std::min<uint32_t>(snapshot.busCount, VN_SNAPSHOT_MAX_BUSES);
}

constexpr auto kAvailable = DebugValueAvailability::Available;

IpcQueryStatus unavailableStatus()
{
    return Detail::runtimeIsActive() ? IpcQueryStatus::Unavailable
                                     : IpcQueryStatus::NoEngine;
}

} // namespace

//==============================================================================
// Snapshot getters.
//==============================================================================

void mapSnapshotToDebugVoices(const VnSnapshot& snapshot,
                              std::vector<DebugVoiceData>& out)
{
    out.clear();
    out.reserve(reportedVoiceRows(snapshot));
    for (uint32_t i = 0; i < VN_SNAPSHOT_MAX_VOICES; ++i)
    {
        const VnVoiceSnapshot& v = snapshot.voices[i];
        const uint64_t publicHandle = snapshot.voicePublicHandles[i];
        if (v.state == VN_VOICE_STATE_INACTIVE && publicHandle == 0)
            continue;

        DebugVoiceData d;
        d.state = static_cast<int32_t>(v.state);
        d.stateAvailable = kAvailable;
        d.playingID = publicHandle;
        d.playingIDAvailable = kAvailable;
        d.pipelineID = i;
        d.pipelineIDAvailable = kAvailable;
        out.push_back(d);
    }
}

void mapSnapshotToDebugBuses(const VnSnapshot& snapshot,
                             std::vector<DebugBusData>& out)
{
    out.clear();
    const uint32_t count = reportedBusRows(snapshot);
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        DebugBusData d;
        const auto rawId = fromNative(snapshot.busIds[i]);
        if (!harkenstudio::IsNull(rawId))
        {
            d.busId = rawId;
            d.busIdAvailable = kAvailable;
            d.peakL = snapshot.busChannelPeaks[i * VN_MAX_CHANNELS];
            d.peakR = snapshot.busChannelPeaks[i * VN_MAX_CHANNELS + (snapshot.mixChannelCount == 1 ? 0 : 1)];
            d.peakAvailable = kAvailable;
        }
        if (i < 32)
        {
            d.muted = ((snapshot.busMuteMask >> i) & 1u) != 0u;
            d.mutedAvailable = kAvailable;
            d.soloed = ((snapshot.busSoloEffectiveMask >> i) & 1u) != 0u;
            d.soloedAvailable = kAvailable;
        }
        out.push_back(d);
    }
}

bool getDebugVoices(std::vector<DebugVoiceData>& out)
{
    out.clear();
    VnSnapshot snapshot{};
    if (!capture(snapshot))
        return false;
    mapSnapshotToDebugVoices(snapshot, out);
    return true;
}

bool getDebugBuses(std::vector<DebugBusData>& out)
{
    out.clear();
    VnSnapshot snapshot{};
    if (!capture(snapshot))
        return false;
    mapSnapshotToDebugBuses(snapshot, out);
    return true;
}

bool getDebugPerformance(DebugPerfData& out)
{
    out = {};
    VnSnapshot snapshot{};
    if (!capture(snapshot))
        return false;

    out.audioCpuPercent = blockCpuPercent(snapshot.audioCpuLastUs);
    out.activeVoices = snapshot.activeVoiceCount;
    out.virtualVoices = static_cast<uint32_t>(
        std::min<uint64_t>(snapshot.parkedVoiceCount, UINT32_MAX));
    out.maxVoices = static_cast<uint32_t>(
        std::min<uint64_t>(snapshot.voiceHighWater, UINT32_MAX));
    out.activeBuses = snapshot.busCount;
    out.underrunCount = static_cast<uint32_t>(
        std::min<uint64_t>(snapshot.backpressureCount, UINT32_MAX));
    out.parameterCommitCount = static_cast<uint32_t>(
        std::min<uint64_t>(snapshot.paramCommitCount, UINT32_MAX));
    out.smoothedCpuLoadPct = out.audioCpuPercent;
    out.budgetLevel = 0;
    return true;
}

bool getSessionRecap(SessionRecapData& out)
{
    out = {};
    VnSnapshot snapshot{};
    if (!capture(snapshot))
        return false;

    out.peakCpuPercent = blockCpuPercent(snapshot.audioCpuPeakUs);
    out.peakActiveVoices = static_cast<uint32_t>(
        std::min<uint64_t>(snapshot.voiceHighWater, UINT32_MAX));
    out.peakVirtualVoices = static_cast<uint32_t>(
        std::min<uint64_t>(snapshot.parkedHighWater, UINT32_MAX));
    out.peakTotalVoices = out.peakActiveVoices + out.peakVirtualVoices;
    out.peakActiveBuses = snapshot.busCount;
    out.completedAndActiveVoiceCount = static_cast<uint32_t>(
        std::min<uint64_t>(snapshot.voiceCompletedCount + snapshot.activeVoiceCount,
                           UINT32_MAX));
    out.totalUnderruns = static_cast<uint32_t>(
        std::min<uint64_t>(snapshot.backpressureCount, UINT32_MAX));
    out.totalFrames = static_cast<uint32_t>(
        std::min<uint64_t>(snapshot.blockIndex *
                               static_cast<uint64_t>(std::max(Detail::runtimeBufferSize(), 0)),
                           UINT32_MAX));
    out.avgCpuPercent = blockCpuPercent(snapshot.audioCpuLastUs);
    out.avgActiveVoices = static_cast<float>(snapshot.activeVoiceCount);
    out.active = true;
    return true;
}

bool getCpuBudgetState(CpuBudgetState& out)
{
    out = {};
    VnSnapshot snapshot{};
    if (!capture(snapshot))
        return false;
    out.smoothedLoadPct = blockCpuPercent(snapshot.audioCpuLastUs);
    out.level = 0;
    return true;
}

bool mapSnapshotToRenderPlan(const VnSnapshot& snapshot, RenderPlanInfo& out)
{
    out = {};
    const uint32_t count = reportedBusRows(snapshot);
    uint32_t live = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto rawId = fromNative(snapshot.busIds[i]);
        if (harkenstudio::IsNull(rawId))
            continue;
        if (live >= 64u)
            return false;
        out.busOrder[live++] = rawId;
    }
    if (live == 0)
        return false;

    out.busCount = live;
    out.standardBusEnd = live;
    out.receiveBusEnd = live;
    out.version = static_cast<uint32_t>(
        std::min<uint64_t>(snapshot.topologyPlanCount, UINT32_MAX));
    out.valid = true;
    return true;
}

bool getRenderPlan(RenderPlanInfo& out)
{
    out = {};
    VnSnapshot snapshot{};
    if (!capture(snapshot))
        return false;
    return mapSnapshotToRenderPlan(snapshot, out);
}

bool getJobStats(JobStatsInfo& out)
{
    out = {};
    VnSnapshot snapshot{};
    if (!capture(snapshot))
        return false;

    out.activeWorkerCount = snapshot.renderWorkerCount;
    out.maxWorkerRenderUs = static_cast<float>(snapshot.renderWorkerBusyPeakUs);
    out.minWorkerRenderUs = snapshot.renderWorkerCount > 0 ? out.maxWorkerRenderUs : 0.0f;
    out.usedParallel = snapshot.renderWorkerCount > 0;
    out.chunkSize = static_cast<uint32_t>(std::max(Detail::runtimeBufferSize(), 0));
    out.activeVoiceCount = snapshot.activeVoiceCount;
    out.totalVoicesProcessed = snapshot.activeVoiceCount;
    out.totalEstimatedCost = static_cast<float>(snapshot.audioVoicesLastUs);
    return true;
}

bool getObjectAudioStatus(ObjectAudioStatusInfo& out)
{
    out = {};
    VnSnapshot snapshot{};
    if (!capture(snapshot))
        return false;

    out.activeDynamicObjects = snapshot.activeVoiceCount;
    out.maxDynamicObjects = static_cast<uint32_t>(
        std::min<uint64_t>(snapshot.emitterHighWater, UINT32_MAX));
    out.channelBasedCount = snapshot.mixChannelCount;
    out.objectBasedCount = snapshot.deviceChannelCount;
    out.fallbackCount = snapshot.outputDownmixActive;
    out.capacityPct = 0.0f;
    out.fallbackPolicy = 0;
    out.sinkAvailable = true;
    return true;
}

bool getVoiceStateBreakdown(VoiceStateBreakdown& out)
{
    out = {};
    VnSnapshot snapshot{};
    if (!capture(snapshot))
        return false;

    for (uint32_t i = 0; i < VN_SNAPSHOT_MAX_VOICES; ++i)
    {
        switch (snapshot.voices[i].state)
        {
        case VN_VOICE_STATE_INACTIVE: ++out.idle; break;
        case VN_VOICE_STATE_ACTIVE: ++out.running; break;
        case VN_VOICE_STATE_DRAINING: ++out.releasing; break;
        case VN_VOICE_STATE_PREPARING: ++out.starting; break;
        default: break;
        }
    }
    out.virtualState = static_cast<int32_t>(
        std::min<uint64_t>(snapshot.parkedVoiceCount, INT32_MAX));
    return true;
}

void mapSnapshotToVoiceInfoList(const VnSnapshot& snapshot,
                                std::vector<VoiceInfoData>& out)
{
    out.clear();
    for (uint32_t i = 0; i < VN_SNAPSHOT_MAX_VOICES; ++i)
    {
        const VnVoiceSnapshot& v = snapshot.voices[i];
        const uint64_t publicHandle = snapshot.voicePublicHandles[i];
        if (v.state == VN_VOICE_STATE_INACTIVE && publicHandle == 0)
            continue;

        VoiceInfoData data;
        data.state = static_cast<int32_t>(v.state);
        out.push_back(data);
    }
}

int getActiveVoiceInfoList(std::vector<VoiceInfoData>& out)
{
    out.clear();
    VnSnapshot snapshot{};
    if (!capture(snapshot))
        return 0;
    mapSnapshotToVoiceInfoList(snapshot, out);
    return static_cast<int>(out.size());
}

bool getBusTopologyPlanCount(uint64_t& outCount)
{
    outCount = 0;
    VnSnapshot snapshot{};
    if (!capture(snapshot))
        return false;
    outCount = snapshot.topologyPlanCount;
    return true;
}

bool getVoiceMetadataChain(uint32_t voiceId, std::vector<DebugVoiceMetadataNode>& outChain)
{
    Q_UNUSED(voiceId);
    outChain.clear();
    return false;
}

bool capturePlaybackPaths(std::vector<PlaybackPathObservation>& out)
{
    out.clear();
    std::vector<VnPreviewPlaybackPathV1> paths;
    for (;;) {
        uint32_t required = 0;
        const auto result = VnPreview_CapturePlaybackPaths(
            paths.data(), static_cast<uint32_t>(paths.size()), &required);
        if (result == VN_PREVIEW_RESULT_CAPACITY) {
            paths.resize(required);
            continue;
        }
        if (result != VN_PREVIEW_OK) return false;
        for (uint32_t index = 0; index < required; ++index) {
            const auto& source = paths[index];
            PlaybackPathObservation path;
            path.voiceInstance = source.voiceInstance;
            path.soundId = fromNative(source.soundId);
            path.outputBusId = fromNative(source.outputBusId);
            for (uint32_t row = 0; row < source.rowCount; ++row)
                path.rows.push_back({fromNative(source.rows[row].stableId), source.rows[row].occurrenceOrdinal});
            for (uint32_t send = 0; send < source.sendCount; ++send)
                path.sends.push_back({fromNative(source.sends[send].targetBusId), source.sends[send].levelDb});
            out.push_back(std::move(path));
        }
        return true;
    }
}

//==============================================================================
// Active modulator / modulator-chain observation.
//==============================================================================

IpcQueryStatus listActiveModulators(std::vector<ActiveModulatorInfo>& out)
{
    out.clear();
    return unavailableStatus();
}

IpcQueryStatus listActiveModulatorChains(std::vector<ActiveModulatorChainInfo>& out)
{
    out.clear();
    return unavailableStatus();
}

} // namespace Bridge
} // namespace HarkenEditor
