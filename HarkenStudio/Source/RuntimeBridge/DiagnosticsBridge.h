#pragma once

#include "Utils/ObjectId.h"

// DiagnosticsBridge -- diagnostics cluster of the runtime bridge.
//
// Scope: read-only observation of the running engine -- voice and bus
// activity, meter and capture data, debug counters, and the link/monitor
// streams the remaining diagnostic surfaces consume.
//
// Interface intent (M4 alignment): observation is snapshot-shaped -- the
// bridge hands the editor immutable, vintage-stamped snapshot values the way
// the M4 tool channel streams them (spec §6.4 snapshot subscription), rather
// than live references into engine internals. Advisory data may be dropped
// under pressure; command/reply data may not. Entity references inside
// snapshots use stable IDs (spec §2.3).
//
// All free functions below observe the bridge-wide ACTIVE engine session
// (registered by PlaybackBridge's EngineSession); with no session alive they
// return false / 0 / empty.
//
// Public bridge headers expose editor-owned types only (Qt/std/editor
// structs); the legacy engine surface never appears here.

#include <QJsonValue>
#include <QString>
#include <cstdint>
#include <vector>

namespace HarkenEditor {
namespace Bridge {

//==============================================================================
// Query status vocabulary shared by the diagnostics and IPC surfaces.
//
// Encodes the session/manager availability distinctions the IPC handlers
// translate into their wire error codes; 0 is always success. NoEngine covers
// "no active session" (and, for surfaces that require it, "session not yet
// initialized"); NoManager means the subsystem pointer was null; Failed and
// NotFound are the runtime call's own failure modes.
//==============================================================================

enum class IpcQueryStatus : uint8_t {
    Ok = 0,
    NoEngine,
    NoManager,
    Failed,
    NotFound,
    Unavailable
};

// Studio-internal availability for old diagnostic fields. Zero/default
// numeric values are not a measurement unless the matching flag is true.
enum class DebugValueAvailability : uint8_t {
    Unavailable = 0,
    Available
};

//==============================================================================
// Snapshot structs (editor-owned mirrors of the engine's debug records).
//==============================================================================

struct VoiceStateBreakdown {
    int32_t idle = 0;
    int32_t starting = 0;
    int32_t running = 0;
    int32_t virtualState = 0;
    int32_t releasing = 0;
    int32_t stopping = 0;
    int32_t paused = 0;
};

struct VoiceInfoData {
    uint32_t objectID = 0;
    harkenstudio::ObjectId busId = {};
    int32_t  priority = 0;
    int32_t  state = 0;
    float    volume = 0.0f;
    float    pitch = 0.0f;
};

struct DebugVoiceData {
    uint32_t objectID        = 0;
    harkenstudio::ObjectId busId = {};
    int32_t  priority        = 0;
    int32_t  state           = 0;
    float    volume          = 0.0f;
    float    pitch           = 0.0f;
    float    peakLevel       = 0.0f;
    uint64_t playingID       = 0;
    int32_t  containerId     = -1;
    bool     isMuted         = false;
    float    hdrGainDb       = 0.0f;
    uint8_t  insertEffectCount = 0;
    float    positionX       = 0.0f;
    float    positionY       = 0.0f;
    float    positionZ       = 0.0f;
    float    cpuPct          = 0.0f;
    uint32_t pipelineID      = 0;
    DebugValueAvailability objectIDAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability busIdAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability priorityAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability stateAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability volumeAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability pitchAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability peakLevelAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability playingIDAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability containerIdAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability isMutedAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability hdrGainDbAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability insertEffectCountAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability positionAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability cpuPctAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability pipelineIDAvailable = DebugValueAvailability::Unavailable;
};

struct DebugBusData {
    harkenstudio::ObjectId busId = {};
    QString  name;
    uint8_t  busType     = 0;
    float    volume      = 0.0f;
    float    peakL       = 0.0f;
    float    peakR       = 0.0f;
    float    rmsL        = 0.0f;
    float    rmsR        = 0.0f;
    uint32_t voiceCount  = 0;
    float    cpuPct      = 0.0f;
    bool     muted       = false;
    bool     soloed      = false;
    bool     hdrEnabled  = false;
    uint32_t effectCount = 0;
    harkenstudio::ObjectId parentBusId = {};
    DebugValueAvailability busIdAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability nameAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability busTypeAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability volumeAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability peakAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability rmsAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability voiceCountAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability cpuPctAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability mutedAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability soloedAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability hdrEnabledAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability effectCountAvailable = DebugValueAvailability::Unavailable;
    DebugValueAvailability parentBusIdAvailable = DebugValueAvailability::Unavailable;
};

struct DebugPerfData {
    float    audioCpuPercent    = 0.0f;
    uint32_t activeVoices       = 0;
    uint32_t virtualVoices      = 0;
    uint32_t maxVoices          = 0;
    uint32_t activeBuses        = 0;
    uint32_t underrunCount      = 0;
    uint32_t parameterCommitCount = 0;
    float    smoothedCpuLoadPct = 0.0f;
    uint8_t  budgetLevel        = 0;
};

struct DebugVoiceMetadataNode {
    QString  nodeType;
    uint32_t curveSnapshotID = 0;
    uint32_t voicePriority   = 0;
    uint32_t virtualState    = 0;
    uint32_t reserved        = 0;
};

struct SessionRecapData {
    float    peakCpuPercent    = 0.0f;
    uint32_t peakActiveVoices  = 0;
    uint32_t peakVirtualVoices = 0;
    uint32_t peakTotalVoices   = 0;
    uint32_t peakActiveBuses   = 0;
    uint32_t completedAndActiveVoiceCount = 0;
    uint32_t totalUnderruns    = 0;
    uint32_t totalFrames       = 0;
    float    avgCpuPercent     = 0.0f;
    float    avgActiveVoices   = 0.0f;
    double   sessionDurationSec = 0.0;
    bool     active            = false;
};

struct CpuBudgetState {
    float   smoothedLoadPct = 0.0f;
    uint8_t level           = 0;
};

struct RenderPlanInfo {
    harkenstudio::ObjectId busOrder[64] = {};
    uint32_t busCount       = 0;
    uint32_t standardBusEnd = 0;
    uint32_t receiveBusEnd      = 0;
    uint32_t version        = 0;
    bool     valid          = false;
};

struct JobStatsInfo {
    uint32_t activeWorkerCount   = 0;
    uint32_t totalVoicesProcessed = 0;
    uint32_t totalChunksStolen   = 0;
    float    maxWorkerRenderUs   = 0.0f;
    float    minWorkerRenderUs   = 0.0f;
    float    parallelEfficiency  = 0.0f;
    bool     usedParallel        = false;
    uint32_t chunkSize           = 0;
    float    totalEstimatedCost  = 0.0f;
    uint32_t activeVoiceCount    = 0;
    float    costThreshold       = 0.0f;

    struct WorkerInfo {
        uint32_t voicesProcessed = 0;
        uint32_t chunksStolen    = 0;
        float    renderUs        = 0.0f;
    };
    WorkerInfo workers[8] = {};
};

struct ObjectAudioStatusInfo {
    uint32_t activeDynamicObjects  = 0;
    uint32_t maxDynamicObjects     = 0;
    uint32_t channelBasedCount     = 0;
    uint32_t objectBasedCount      = 0;
    uint32_t fallbackCount         = 0;
    float    capacityPct           = 0.0f;
    uint8_t  fallbackPolicy        = 0;
    bool     sinkAvailable         = false;
};

//==============================================================================
// Snapshot getters (active session; false / 0 / empty with no session).
//==============================================================================

struct VoiceCountStatusText {
    QString text;
    QString voiceLoadRole;
};

inline VoiceCountStatusText formatVoiceCountAvailable(int count)
{
    VoiceCountStatusText out;
    out.text = QStringLiteral("Voices: %1/64").arg(count);
    out.voiceLoadRole = count > 48 ? QStringLiteral("critical")
        : (count > 32 ? QStringLiteral("warning") : QStringLiteral("safe"));
    return out;
}

inline VoiceCountStatusText formatVoiceCountUnavailable()
{
    VoiceCountStatusText out;
    out.text = QStringLiteral("Voices: —");
    return out;
}

inline constexpr const char kUnavailableDisplay[] = "—";

inline bool isAvailable(DebugValueAvailability availability)
{
    return availability == DebugValueAvailability::Available;
}

inline QJsonValue jsonNumberIfAvailable(DebugValueAvailability availability, double value)
{
    return isAvailable(availability) ? QJsonValue(value) : QJsonValue();
}

inline QJsonValue jsonIntIfAvailable(DebugValueAvailability availability, int value)
{
    return isAvailable(availability) ? QJsonValue(value) : QJsonValue();
}

inline QJsonValue jsonBoolIfAvailable(DebugValueAvailability availability, bool value)
{
    return isAvailable(availability) ? QJsonValue(value) : QJsonValue();
}

inline QJsonValue jsonStringIfAvailable(DebugValueAvailability availability, const QString& value)
{
    return isAvailable(availability) ? QJsonValue(value) : QJsonValue();
}

inline QString displayDashIfUnavailable(DebugValueAvailability availability, const QString& value)
{
    return isAvailable(availability) ? value : QString::fromUtf8(kUnavailableDisplay);
}

bool getDebugVoices(std::vector<DebugVoiceData>& out);
bool getDebugBuses(std::vector<DebugBusData>& out);
bool getDebugPerformance(DebugPerfData& out);
bool getSessionRecap(SessionRecapData& out);
bool getCpuBudgetState(CpuBudgetState& out);
bool getRenderPlan(RenderPlanInfo& out);
bool getJobStats(JobStatsInfo& out);
bool getObjectAudioStatus(ObjectAudioStatusInfo& out);
bool getVoiceStateBreakdown(VoiceStateBreakdown& out);
int  getActiveVoiceInfoList(std::vector<VoiceInfoData>& out);
bool getBusTopologyPlanCount(uint64_t& outCount);
bool getVoiceMetadataChain(uint32_t voiceId, std::vector<DebugVoiceMetadataNode>& outChain);

struct PlaybackPathRow {
    harkenstudio::ObjectId stableId = {};
    uint32_t occurrenceOrdinal = UINT32_MAX;
    bool operator==(const PlaybackPathRow&) const = default;
};

struct PlaybackPathSend {
    harkenstudio::ObjectId targetBusId{};
    float levelDb = 0.0f;
    bool operator==(const PlaybackPathSend&) const = default;
};

struct PlaybackPathObservation {
    uint64_t voiceInstance = 0;
    harkenstudio::ObjectId soundId = {};
    harkenstudio::ObjectId outputBusId = {};
    std::vector<PlaybackPathRow> rows;
    std::vector<PlaybackPathSend> sends;
    bool operator==(const PlaybackPathObservation&) const = default;
};

bool capturePlaybackPaths(std::vector<PlaybackPathObservation>& out);

//==============================================================================
// Active modulator / modulator-chain observation (active session).
//==============================================================================

struct ActiveModulatorInfo {
    uint64_t ownerId = 0;
    uint8_t  scope = 0, type = 0, state = 0;
    float    outputValue = 0.0f;
    int      voiceCount = 0, bindingCount = 0;
    uint64_t elapsedFrames = 0;
};
IpcQueryStatus listActiveModulators(std::vector<ActiveModulatorInfo>& out);

struct ModulatorChainNodeInfo {
    bool    hasCtx = false;
    uint8_t type = 0, mode = 0;
    float   output = 0.0f, depth = 1.0f;
};
struct ActiveModulatorChainInfo {
    uint64_t chainId = 0, ownerId = 0;
    uint8_t  scope = 0;
    int      nodeCount = 0, bindingCount = 0;
    float    finalOutput = 0.0f;
    std::vector<ModulatorChainNodeInfo> nodes;
};
IpcQueryStatus listActiveModulatorChains(std::vector<ActiveModulatorChainInfo>& out);

//==============================================================================
// Engine notification poll.
//
// Drains the bridge-owned monitor sink of the active session. The sink is
// filled during EngineSession::update() on the UI thread; call this on the
// same thread right after update(). Unknown engine error codes are skipped.
// Name resolution and Qt signal emission stay editor-side.
//==============================================================================

struct EngineNotification {
    enum class Kind : uint8_t {
        ExecError,
        PlaybackEnded,
        VoiceStolen,
        VoiceParked,
        VoiceReturned
    };
    Kind kind = Kind::ExecError;
    uint8_t domain = 0;            // exec-error domain ordinal; Unclassified is 5
    uint8_t severity = 2;
    uint32_t objectId = 0;
    uint64_t playingId = 0;        // already handle-converted
    uint64_t voiceHandle = 0;
    uint64_t blockTimestamp = 0;
    QString errorType;             // e.g. "PACK_FILE_NOT_FOUND"
    QString message;
};

constexpr uint8_t kExecErrorDomainUnclassified = 5;

void pollEngineNotifications(std::vector<EngineNotification>& out);

// Test hook. Copies type / voiceHandle / blockTimestamp / generation,
// then hops to the UI thread. ENDED / STOPPED_BY_FADE stay on the
// existing drain path and are not queued here. Shutdown increments
// the generation and discards undelivered items.
void injectRuntimeNotificationForTest(uint32_t type,
                                      uint64_t voiceHandle,
                                      uint64_t blockTimestamp);
void invalidateRuntimeNotificationsForTest();

} // namespace Bridge
} // namespace HarkenEditor
