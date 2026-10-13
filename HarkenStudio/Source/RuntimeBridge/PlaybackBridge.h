#pragma once

#include "Utils/ObjectId.h"

// PlaybackBridge -- playback / voice cluster of the runtime bridge.
//
// Scope: the editor calls that still have a runtime-backed playback path:
// posting and stopping Actions, listener updates, pack load/unload, bus sends,
// and per-voice parameter pushes driven from the authoring UI.
//
// Interface intent (M4 alignment): callers address content by the stable u64
// asset ID (spec §2.3) -- display names are an editor-side convenience and
// never the runtime identity. Verbs follow the M4 taxonomy (§2.4:
// trigger/stop/get). The uint32_t object ids on this surface are the legacy
// FNV-1a-32 name-hash currency, treated as opaque; the M4 rewire maps them
// to u64 stable IDs inside the bridge private side only.
//
// Public bridge headers expose editor-owned types only (Qt/std/editor
// structs); the legacy engine surface never appears here.

#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace HarkenEditor {
namespace Bridge {

enum class EngineDeviceMode : uint8_t {
    Real,
    Null
};

//==============================================================================
// Result codes (numeric passthrough).
//
// Engine result codes cross the bridge numerically untouched so IPC replies
// stay byte-identical. The implementation static_asserts the named constants
// against the legacy macros.
//==============================================================================

using EngineResult = uint32_t;               // mirrors the legacy result width
inline constexpr harkenstudio::ObjectId kEngineMasterBusId = harkenstudio::kMasterBusObjectId;
inline constexpr EngineResult kEngineOk      = 0;
inline constexpr EngineResult kEngineInvalidArg =
    static_cast<EngineResult>(-100);
inline constexpr EngineResult kEngineUnknownId =
    static_cast<EngineResult>(-200);
inline constexpr EngineResult kEngineStaleHandle =
    static_cast<EngineResult>(-201);
inline constexpr EngineResult kEngineNoVoice = static_cast<EngineResult>(-300);
inline constexpr EngineResult kEngineNotInit =
    static_cast<EngineResult>(-303);
inline constexpr EngineResult kEngineBackpressure = static_cast<EngineResult>(-301);
inline constexpr EngineResult kEngineInUse       = static_cast<EngineResult>(-302);
inline constexpr EngineResult kEngineSendCycle    = static_cast<EngineResult>(-315);
inline constexpr EngineResult kEngineCapabilityUnavailable =
    static_cast<EngineResult>(-327);
inline constexpr EngineResult kEngineRuntimeRecreateRequired =
    static_cast<EngineResult>(-328);
inline constexpr EngineResult kEngineDeviceError =
    static_cast<EngineResult>(-400);

//==============================================================================
// Editor-owned mirror structs.
//==============================================================================

struct EngineSessionConfig {
    int sampleRate = 0;   // 0 = keep engine default
    int bufferSize = 0;   // 0 = keep engine default
    int maxVoices  = 64;
    EngineDeviceMode deviceMode = EngineDeviceMode::Real;
};

struct BinauralStatus {
    bool     hrtfLoaded   = false;
    uint32_t measurements = 0;
    uint32_t irLength     = 0;
    float    sampleRate   = 0.0f;
};

enum class BusTopologyState : uint8_t {
    Idle,
    Syncing,
    Ready,
    Failed
};

enum class BusTopologyOperation : uint8_t {
    None,
    ApplySnapshot,
    SetVolume,
    SetMute,
    SetSolo,
    SetOutputVolume
};

struct RuntimeBusDefinition {
    harkenstudio::ObjectId id{};
    harkenstudio::ObjectId parentId = {};
    QString name;
    float volumeDb = 0.0f;
    bool mute = false;
    bool solo = false;
    float outputVolumeDb = 0.0f;
};

struct RuntimeBusSendDefinition {
    harkenstudio::ObjectId sourceId = {};
    harkenstudio::ObjectId targetId = {};
    float levelDb = 0.0f;
};

struct BusTopologyFailure {
    BusTopologyOperation operation = BusTopologyOperation::None;
    EngineResult result = kEngineOk;
    harkenstudio::ObjectId busId = {};
    harkenstudio::ObjectId relatedBusId = {};
    QString busName;
};

enum class PlaybackFailure : uint8_t {
    None,
    TopologySyncPending,
    TopologySyncFailed,
    RuntimeRefused,
    NoVoiceProduced,
    NotPlayable
};

struct PlaybackStartResult {
    uint64_t handle = 0;
    EngineResult result = kEngineOk;
    PlaybackFailure failure = PlaybackFailure::None;
    BusTopologyFailure topologyFailure;
};

enum class ActionSeekState : uint8_t {
    Accepted,
    Applied,
    Cancelled,
};

struct ActionSeekStartResult {
    uint64_t operationId = 0;
    EngineResult result = kEngineOk;
};

struct ActionSeekInfo {
    ActionSeekState state = ActionSeekState::Accepted;
    EngineResult result = kEngineOk;
    EngineResult serviceResult = kEngineOk;
    EngineResult nativeReleaseResult = kEngineInUse;
    uint64_t playbackHandle = 0;
    QString actionName;
    uint32_t targetCount = 0;
    uint32_t appliedCount = 0;
    uint32_t cancelledCount = 0;
    uint64_t acceptedLogicSequence = 0;
    uint64_t appliedAudioBlockSequence = 0;
    uint64_t minimumAppliedSourceFrame = 0;
    uint64_t maximumAppliedSourceFrame = 0;
    bool nativeReleased = false;
};

struct ModulatorPresetDesc {     // flat mirror of the runtime modulator preset
    QString name, category;
    bool isFactory = false;
    uint8_t modulatorType = 0, scope = 0;
    float lfoFrequency = 1, lfoDepth = 1, lfoOffset = 0, lfoPwm = 0.5f, lfoSmooth = 0;
    uint8_t lfoWaveform = 0, lfoUnipolar = 0;
    float envAttackTime = 0.01f, envHoldTime = 0, envDecayTime = 0.1f,
          envSustainLevel = 0.7f, envReleaseTime = 0.3f;
    uint8_t envAttackCurve = 0, envDecayCurve = 2, envReleaseCurve = 2, envAutoRelease = 0;
    float timeDurationSec = 1, timeInitialValue = 0, timeTargetValue = 1;
    uint8_t timeCurveType = 0, timeLoop = 0, timePingPong = 0;
    uint32_t stepCount = 4;
    float stepDurationMs = 250, stepTransitionMs = 5;
    uint8_t stepLoop = 1;
    float stepValues[32] = {};
    uint8_t bindTargetParam = 0, bindCurveType = 0;
    float bindMappedMin = 0, bindMappedMax = 1;
};

//==============================================================================
// EngineSession -- one live legacy engine instance (pimpl, non-copyable).
//
// Owned by the editor's AudioEngine. On construction it registers itself as
// the bridge-wide active session (the target of the ProjectSyncBridge /
// DiagnosticsBridge free functions, mirroring the global-singleton reality
// of the legacy engine); the destructor deregisters and shuts the engine
// down. Also owns the engine-notification monitor sink and the log-callback
// trampoline.
//==============================================================================

class EngineSession {
public:
    // nullptr on engine create/initialize failure (the editor then enters
    // simulation mode exactly as before).
    static std::unique_ptr<EngineSession> create(const EngineSessionConfig& config);
    ~EngineSession();

    EngineSession(const EngineSession&) = delete;
    EngineSession& operator=(const EngineSession&) = delete;

    // Invoked on an ARBITRARY thread (the runtime's calling thread); the
    // handler must be thread-safe.
    void setLogHandler(std::function<void(int level, const QString& msg)> handler);

    // -- monitor pump (UI thread) --
    void update();
    int  activeVoiceCount() const;
    void masterMeterPeak(float& l, float& r) const;
    float audioCpuPercent() const;

    // -- output / device --
    EngineResult startOutput();
    void stopOutput();
    void setMasterFade(float volume, int fadeMs);
    double outputLatency() const;
    int actualSampleRate() const;
    EngineResult reconfigureBufferSize(int32_t newBlockSize);
    int32_t bufferSize() const;

    PlaybackStartResult playByNameDetailed(const QString& name,
                                          uint64_t borrowedEmitter = 0);
    PlaybackStartResult playByName3DDetailed(const QString& name,
                                              float x, float y, float z);
    ActionSeekStartResult seekAction(uint64_t playbackHandle,
                                     const QString& expectedActionName,
                                     double positionMs, bool relative);
    EngineResult queryActionSeek(uint64_t operationId,
                                 ActionSeekInfo& outInfo) const;
    EngineResult releaseActionSeek(uint64_t operationId);
    uint64_t playByName(const QString& name);
    uint64_t playByName3D(const QString& name, float x, float y, float z);
    bool hasActionProgram(harkenstudio::ObjectId actionId) const;
    void stopPlayback(uint64_t handle, float fadeMs);
    void stopAll(float fadeMs);
    bool hasCurrentWorldPlayableConsumers() const;
    void pumpNullMixer(std::uint32_t blockCount);
    void setPlaybackPosition(uint64_t handle, float x, float y, float z);
    void setVoicePitchByName(const QString& soundName, float cents);
    void setVoiceVolumeByName(const QString& soundName, float volumeDb);
    void setVoiceOutputVolumeByName(const QString& soundName, float volumeDb);
    void setVoiceSpatMixByName(const QString& soundName, float mix);
    void setVoiceDistanceScaleByName(const QString& soundName, float scale);
    void setVoiceSendsByName(const QString& soundName,
                             const std::vector<std::pair<harkenstudio::ObjectId, float>>& sends);
    void setListenerTransform(float px, float py, float pz,
                              float fx, float fy, float fz,
                              float ux, float uy, float uz);

    // -- packs / prepare --
    bool loadPack(const QString& path);
    void unloadPack(const QString& path);   // stop-all-referencing before unload
    uint32_t loadedPackCount() const;
    bool prepareMedia(uint32_t sourceId, const QString& absolutePath, uint64_t fileSize);
    bool swapMedia(uint32_t sourceId, const QString& newPath, uint64_t fileSize);
    void clearPreparedMedia();   // preparer clear, else media-registry clear

    struct RawPcmSliceRegistration {
        harkenstudio::ObjectId soundId = {};
        uint32_t channels = 0;
        uint32_t sampleRate = 0;
        uint64_t startFrame = 0;
        uint64_t frameCount = 0;
        uint64_t windowStartFrame = 0;
        uint64_t windowFrameCount = 0;
        bool loops = false;
        EngineResult result = kEngineOk;
    };
    harkenstudio::ObjectId mintRawPcmSoundId();
    RawPcmSliceRegistration registerRawPcmSlice(harkenstudio::ObjectId soundId,
                                                const QString& absolutePath,
                                                uint64_t startFrame,
                                                uint64_t frameCount,
                                                bool loops = false);
    RawPcmSliceRegistration relocateRawPcmSlice(harkenstudio::ObjectId soundId,
                                                uint64_t startFrame);
    EngineResult unregisterRawPcm(harkenstudio::ObjectId soundId);
    uint64_t playRawPcm(harkenstudio::ObjectId soundId, bool loops,
                        EngineResult* outResult = nullptr);
    bool stopRawPcm(uint64_t playbackHandle, uint32_t fadeMs = 0);

    // -- buses / mixer --
    void setBusMuteById(harkenstudio::ObjectId busId, bool mute);
    void setBusSoloById(harkenstudio::ObjectId busId, bool solo);
    // Returns false without writing l/r when the snapshot has no peak for the
    // canonical Bus identity. Never falls back to master.
    bool busMeterPeakById(harkenstudio::ObjectId busId, float& l, float& r) const;
    void setBusVolumeDbById(harkenstudio::ObjectId busId, float db);
    void setBusOutputVolumeDbById(harkenstudio::ObjectId busId, float db);
    float busVolumeDbById(harkenstudio::ObjectId busId) const;
    void setBusPanById(harkenstudio::ObjectId busId, float pan);
    void synchronizeBusTopology(
        const std::vector<RuntimeBusDefinition>& buses,
        const std::vector<RuntimeBusSendDefinition>& sends);
    void confirmBusTopologyWithoutApply(
        const std::vector<RuntimeBusDefinition>& buses,
        const std::vector<RuntimeBusSendDefinition>& sends);
    static void confirmActiveBusTopologyWithoutApply(
        const std::vector<RuntimeBusDefinition>& buses,
        const std::vector<RuntimeBusSendDefinition>& sends);
    BusTopologyState busTopologyState() const;
    BusTopologyFailure busTopologyFailure() const;
    void renameBus(harkenstudio::ObjectId busId, const QString& newName);
    std::vector<harkenstudio::ObjectId> runtimeBusIds() const;
    void setMixerVolume(harkenstudio::ObjectId busId, float volumeDb);

    // -- spatial --
    uint32_t addListener();
    void removeListener(uint32_t listenerId);
    bool isListenerActive(uint32_t listenerId) const;
    void setListenerPositionById(uint32_t listenerId,
                                 float x, float y, float z,
                                 float fwdX, float fwdY, float fwdZ,
                                 float upX, float upY, float upZ);
    void setGameObjectListeners(uint64_t gameObjectId, const uint32_t* listenerIds,
                                uint32_t count);
    void setListenerSpatialization(uint32_t listenerId, bool spatialized,
                                   uint8_t panningRule, uint8_t speakerConfigType);
    bool listenerInfo(uint32_t listenerId,
                      float* outX, float* outY, float* outZ,
                      float* outFwdX, float* outFwdY, float* outFwdZ,
                      int* outSpatialized, uint32_t* outPanningRule);
    void setMultiPosition(uint64_t gameObjectId, const float* positions, uint32_t count,
                          uint8_t type);
    void clearMultiPosition(uint64_t gameObjectId);
    void setAutomationPath(uint64_t gameObjectId, const float* points, uint32_t count,
                           uint8_t interp, uint8_t loop);
    void startAutomation(uint64_t gameObjectId);
    void stopAutomation(uint64_t gameObjectId);

private:
    EngineSession();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

//==============================================================================
// Free functions over process-global engine registries (no session needed).
//==============================================================================

// Modulator preset listing; typeFilter -1 = all types (lfo/envelope/time/step).
std::vector<ModulatorPresetDesc> listModulatorPresets(int typeFilter);
uint32_t modulatorPresetTotalCount();

bool findModulatorPreset(const QString& name, ModulatorPresetDesc& out);
bool saveModulatorUserPreset(const ModulatorPresetDesc& desc);
bool deleteModulatorUserPreset(uint8_t modulatorType, const QString& name);

} // namespace Bridge
} // namespace HarkenEditor
