#pragma once

#include "Utils/ObjectId.h"

// IpcBridge -- IPC command + snapshot surface of the runtime bridge.
//
// Scope: the terminal interface the editor's IPC handlers program against
// (IPCHandlers_*, IPCServerInternal). Handlers decode wire requests into
// bridge commands and encode bridge results/snapshots into replies; they see
// this header only -- never a runtime type, never another bridge cluster's
// internals reached around this surface.
//
// Interface intent (M4 alignment): the M4 remote channel mirrors the public
// C API through a generated command table (spec §6.3) and streams
// vintage-stamped snapshots plus droppable advisory notifications (§6.4).
// This surface is shaped as that same command/snapshot pair so that when the
// generated mirror lands, only the bridge's private side changes -- handlers
// keep their call shapes. Commands address content by stable u64 asset IDs
// (spec §2.3); replies are never dropped, advisory streams may be.
//
// All functions operate on the bridge-wide ACTIVE session unless noted
// (global / sessionless). Status fidelity: IpcQueryStatus distinguishes
// no-session from missing-subsystem so handlers keep their exact wire error
// code + message pairs; the AudioEngine-object null check stays handler-side.
//
// Public bridge headers expose editor-owned types only (Qt/std/editor
// structs); the legacy engine surface never appears here.

#include "DiagnosticsBridge.h"   // IpcQueryStatus (single home) + snapshot structs

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <vector>

namespace HarkenEditor {
namespace Bridge {

//==============================================================================
// Plugins. The old-runtime plugin-host command surface is gone; this listing
// query remains as the capability-map probe (reports NoEngine/Unavailable,
// never fabricates plugins).
//==============================================================================

struct PluginDesc {
    QString  name, vendor;
    uint32_t typeId = 0, version = 0;
    bool     isSource = false;
    uint32_t paramCount = 0;
    struct Param {
        QString name, unit;
        float defaultValue = 0, minValue = 0, maxValue = 0;
    };
    std::vector<Param> params;
};

IpcQueryStatus pluginList(std::vector<PluginDesc>& out, uint32_t& outCount);

//==============================================================================
// Clock / codecs / streams / memory.
//==============================================================================

IpcQueryStatus setClockPll(bool enabled);
struct ClockSyncStatus { double actualSampleRate = 0, driftPpm = 0; };
IpcQueryStatus clockSyncStatus(ClockSyncStatus& out);

struct CodecDesc {
    uint32_t formatId = 0;
    QString  name, fileExtension;
    uint32_t version = 0, preferredBufferFrames = 0, maxChannels = 0,
             capabilities = 0, activeStreams = 0;
    // Extended descriptor fields (single-codec query form).
    uint32_t minBufferFrames = 0, maxBufferFrames = 0;
    std::vector<uint32_t> supportedSampleRates;
    uint32_t supportedBitDepths = 0, channelLayoutMask = 0,
             optimalConcurrentStreams = 0;
    float    estimatedCpuCostPerFrame = 0;
    uint32_t codecFlags = 0;
};
IpcQueryStatus listCodecs(std::vector<CodecDesc>& out);               // 16-slot cap inside
IpcQueryStatus codecInfo(uint32_t formatId, CodecDesc& out);          // NotFound on runtime error

struct StreamStatusData {
    uint32_t activeStreams = 0, pausedStreams = 0;
    uint64_t totalBufferedFrames = 0;
    uint32_t deviceBlockSize = 0;
    bool     deviceSupportsSeek = false, deviceSupportsAsync = false;
};
IpcQueryStatus streamStatus(StreamStatusData& out);

struct StreamPositionData {
    uint64_t decodedFramePos = 0, totalFileFrames = 0;
    float    progress = 0;
    int32_t  streamResult = 0;
};
IpcQueryStatus streamPositionForVoice(uint32_t voiceId, StreamPositionData& out);  // NotFound

struct StreamDeviceDesc {
    uint32_t deviceIndex = 0;
    QString  name;
    uint32_t deviceId = 0, blockSize = 0, maxConcurrentIo = 0;
    bool     supportsSeek = false, supportsAsync = false, supportsMmap = false,
             isRemovable = false;
    uint64_t totalBytesRead = 0;
    uint32_t activeIoCount = 0;
};
IpcQueryStatus streamDeviceInfo(uint32_t index, StreamDeviceDesc& out);  // NotFound
IpcQueryStatus streamDeviceCount(uint32_t& out);

struct StreamThroughputData {
    double   avgThroughputBps = 0;
    uint32_t adaptiveResizeCount = 0;
    bool     adaptiveEnabled = false;
    uint32_t targetLatencyMs = 0;
};
IpcQueryStatus streamThroughput(StreamThroughputData& out);

struct MmapStatusData {
    bool     enabled = false;
    uint32_t totalAttempts = 0, successes = 0, fallbacks = 0;
    int32_t  lastErrorCode = 0;
};
IpcQueryStatus mmapStatus(MmapStatusData& out);

struct DeviceFairnessData {
    uint32_t activeCounts[8] = {};
    uint32_t allocatedDevices = 0, routeCount = 0;
};
IpcQueryStatus deviceFairness(DeviceFairnessData& out);

struct MemoryBudgetCategory {
    uint64_t budgetBytes = 0, heapUsageBytes = 0, mmapUsageBytes = 0,
             totalUsageBytes = 0;
    bool     isOverBudget = false;
};
struct MemoryBudgetStatusData {
    MemoryBudgetCategory categories[8];
    uint64_t totalUsageBytes = 0, totalBudgetBytes = 0;
    uint32_t overBudgetCount = 0;
};
IpcQueryStatus memoryBudgetStatus(MemoryBudgetStatusData& out);       // Failed on runtime error

struct ListenerBusBindingData {
    uint32_t listenerId = 0;
    harkenstudio::ObjectId outputBusId = {};
    bool     isActive = false;
};
IpcQueryStatus listenerOutputBus(uint32_t listenerId, ListenerBusBindingData& out);  // Failed

// Mixer format introspection (buffer-config reply's sample-rate half; 0 when
// no session, as before).
int32_t mixerSampleRate();

// True while an engine session is live. Handlers use this only to keep their
// wire error-precedence (session-missing checked before parameter
// validation); it carries no other meaning.
bool engineSessionActive();

// Probe for the CPU-budget manager chain (session -> mixer thread) so the
// set-config handler keeps its distinct not-initialized / not-available
// errors ahead of parameter validation; NoManager = mixer thread missing.
IpcQueryStatus cpuBudgetManagerStatus();

//==============================================================================
// Output device introspection.
//==============================================================================

struct DeviceCapabilityData {
    QString  name;
    int32_t  deviceIndex = 0;
    uint32_t nativeSampleRate = 0, nativeChannels = 0;
    bool     supportsExclusive = false, supportsSpatial = false, isAvailable = false;
    std::vector<uint32_t> supportedRates;
};
IpcQueryStatus deviceCapabilities(int32_t deviceIndex, DeviceCapabilityData& out);  // -1 = default; Failed on query error

struct ActiveFormatData {
    int      sampleRate = 0, bufferSize = 0;
    double   latencyMs = 0;
    int      state = 0;
    uint32_t deviceCount = 0;
};
IpcQueryStatus activeOutputFormat(ActiveFormatData& out);             // NotFound => NO_DEVICE

IpcQueryStatus hotplugCounts(uint32_t& outEventCount, uint32_t& outDeviceCount);

//==============================================================================
// Monitor / live-parameter introspection.
//==============================================================================

struct WatchedParamEntry {
    uint32_t slot = 0, objectId = 0, paramId = 0;
    int      watchType = 0;
};
IpcQueryStatus watchedParameters(std::vector<WatchedParamEntry>& out);  // NoManager => inspector unavailable

struct LiveParamSnapshotEntry {
    uint32_t objectId = 0, paramId = 0;
    int      watchType = 0;
    float    value = 0, prevValue = 0;
    bool     changed = false;
    int64_t  timestampUs = 0;
};
IpcQueryStatus liveParameters(std::vector<LiveParamSnapshotEntry>& out);

uint64_t monitorGlobalFilterMask();                             // global -- works sessionless

//==============================================================================
// Voice pipeline diagnostics.
//==============================================================================

struct VoiceContributionData {
    float baseVolumeDb = 0, variableVolumeDb = 0, actionPropVolumeDb = 0,
          stateVolumeDb = 0, modulatorVolumeDb = 0, spatialVolumeDb = 0,
          busHierarchyVolumeDb = 0, fadeVolumeDb = 0, finalVolumeDb = 0;
    float basePitch = 0, variablePitch = 0, actionPropPitch = 0, statePitch = 0,
          modulatorPitch = 0, busHierarchyPitch = 0, finalPitch = 0;
    float variableLpf = 0, stateLpf = 0, finalLpf = 0;
    float variableHpf = 0, stateHpf = 0, finalHpf = 0;
};
IpcQueryStatus voiceContribution(uint32_t voiceId, VoiceContributionData& out);  // NotFound

struct VoicePathLatencyData {
    uint32_t voiceId = 0;
    uint32_t insertFxSamples = 0, busFxSamples = 0, totalSamples = 0;
};
// NotFound when the voice is unknown; Failed carries the raw engine result
// in outRawResult (the handler's error message quotes it).
IpcQueryStatus voicePathLatency(uint32_t voiceId, VoicePathLatencyData& out,
                                uint32_t& outRawResult);

//==============================================================================
// Pack queries (pack manager; NoManager => "PackManager not available").
//==============================================================================

struct PackMetricsData {
    uint64_t loadTimeUs = 0, parseTimeUs = 0, ioTimeUs = 0,
             loadTimestamp = 0, lastAccessTime = 0, peakMemoryBytes = 0,
             memoryUsage = 0;
    uint32_t objectCount = 0, mediaCount = 0,
             streamingMediaCount = 0, inMemoryMediaCount = 0;
    bool     isLazyLoaded = false;
    uint32_t activeVoices = 0;
};
IpcQueryStatus packMetrics(const QString& packName, PackMetricsData& out);

struct PackMetricsSummary {
    QString  packName;
    uint64_t loadTimeUs = 0, parseTimeUs = 0, ioTimeUs = 0,
             peakMemoryBytes = 0, loadTimestamp = 0;
};
IpcQueryStatus allPackMetrics(std::vector<PackMetricsSummary>& out);

struct PackLoadHistoryEntry {
    QString  packName;
    uint64_t timestamp = 0, loadTimeUs = 0, memorySizeBytes = 0;
    int      status = 0;
};
IpcQueryStatus packLoadHistory(std::vector<PackLoadHistoryEntry>& out);

struct ActionRoutingEntry {
    uint32_t packId = 0;
    QString  packName;          // resolved inside (id -> name map)
    bool     packNameKnown = false;
    int      languageId = 0, priority = 0;
    bool     isVoicePack = false;
};
IpcQueryStatus actionRouting(uint32_t actionNameHash, std::vector<ActionRoutingEntry>& out);

IpcQueryStatus resolveActionPack(uint32_t actionNameHash, uint32_t& outPackId,
                                 QString& outPackName);

struct PackCollisionEntry {
    uint32_t actionNameHash = 0;
    QStringList packNames;
};
IpcQueryStatus multiPackCollisions(std::vector<PackCollisionEntry>& out);

//==============================================================================
// Replay verification (sessionless file utility).
//==============================================================================

struct ReplayCompareResult {
    float    maxDiff = 0;
    double   psnrDb = 0;
    bool     isBitExact = false, lengthMatch = false;
    uint64_t totalSamples = 0, diffSamples = 0;
};
bool replayCompare(const QString& referencePath, const QString& replayPath,
                   ReplayCompareResult& out);

} // namespace Bridge
} // namespace HarkenEditor
