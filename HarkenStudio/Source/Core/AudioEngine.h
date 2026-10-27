
#pragma once

#include <QObject>
#include <QVector3D>
#include <QString>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <QUuid>
#include <atomic>
#include <array>
#include <memory>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "BindingRuntimeLowering.h"
#include "PlatformContext.h"
#include "RuntimeBridge/PlaybackBridge.h"
#include "RuntimeBridge/PreviewBridge.h"
#include "RuntimeBridge/DiagnosticsBridge.h"
#include "Utils/ObjectId.h"
#include "VariableSemantics.h"

namespace VNX {

struct ProjectItem;
struct ProjectData;
class RuntimeSyncCoordinator;
class VariableRehearsal;
class ProjectManager;
struct LivePropertyPreview;
struct CurrentWorldCompilation;

class AudioEngine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool initialized READ isInitialized NOTIFY initializedChanged)
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(int activeVoiceCount READ activeVoiceCount NOTIFY voiceCountChanged)
    Q_PROPERTY(float cpuUsage READ cpuUsage NOTIFY cpuUsageChanged)

public:
    static AudioEngine& instance();
    void bindProjectPropertyPreview(ProjectManager& project);

    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    bool initialize(int sampleRate = 0, int bufferSize = 0, int maxVoices = 64);
    bool initialize(const HarkenEditor::Bridge::EngineSessionConfig& config);
    void shutdown();

    void resetListenerTransformCache();

    bool isInitialized() const { return initialized_; }
    bool isSimulationMode() const { return initialized_ && session_ == nullptr; }

    bool startOutput();
    void stopOutput();
    bool isPlaying() const { return playing_; }
    RuntimeSyncCoordinator& runtimeSyncCoordinator() noexcept;
    const RuntimeSyncCoordinator& runtimeSyncCoordinator() const noexcept;
    void bindVariableRehearsal(VariableRehearsal& rehearsal);

    enum class RehearsalPushPath : uint8_t { None = 0, Global = 1, Emitter = 2 };
    struct RehearsalPushRecord
    {
        QUuid variableId;
        RehearsalPushPath path = RehearsalPushPath::None;
        float value = 0.0f;
        bool clearing = false;
    };
    const std::vector<RehearsalPushRecord>& rehearsalPushLog() const {
        return rehearsalPushLog_;
    }
    void clearRehearsalPushLog() { rehearsalPushLog_.clear(); }

    void setMasterVolume(float volume, int fadeMs = 0);
    float masterVolume() const { return masterVolume_; }

    harkenstudio::ObjectId mintRawPcmSoundId();
    HarkenEditor::Bridge::EngineSession::RawPcmSliceRegistration registerRawPcmSlice(
        harkenstudio::ObjectId soundId, const QString& absolutePath,
        uint64_t startFrame, uint64_t frameCount, bool loops = false);
    HarkenEditor::Bridge::EngineSession::RawPcmSliceRegistration relocateRawPcmSlice(
        harkenstudio::ObjectId soundId, uint64_t startFrame);
    HarkenEditor::Bridge::EngineResult unregisterRawPcm(harkenstudio::ObjectId soundId);
    uint64_t playRawPcm(harkenstudio::ObjectId soundId, bool loops);
    bool stopRawPcm(uint64_t playbackHandle, uint32_t fadeMs = 0);

    quintptr playByName(const QString& name);
    quintptr playByName3D(const QString& name, const QVector3D& position);
    void stopPlayback(quintptr playbackHandle, float fadeMs = 0);
    void stopAll(float fadeMs = 0);

    struct PlaybackStartResult {
        quintptr playbackHandle    = 0;
        bool     hasSound       = false;
        bool     previewVariablesPending = false;
        HarkenEditor::Bridge::EngineResult engineResult =
            HarkenEditor::Bridge::kEngineOk;
        HarkenEditor::Bridge::PlaybackFailure failure =
            HarkenEditor::Bridge::PlaybackFailure::None;
        HarkenEditor::Bridge::BusTopologyFailure topologyFailure;
    };

    PlaybackStartResult playByNameDetailed(const QString& name);
    PlaybackStartResult playByNameWithPreviewContext(const QString& name);

    void setPlaybackPosition(quintptr playbackHandle, const QVector3D& position);

    void setListenerPosition(const QVector3D& position);
    void setListenerOrientation(const QVector3D& forward, const QVector3D& up);
    void setListenerTransform(const QVector3D& position, const QVector3D& forward, const QVector3D& up);
    QVector3D listenerPosition() const { return listenerPosition_; }

    bool loadPack(const QString& path);
    void unloadPack(const QString& path);
    uint32_t getLoadedPackCount() const;

    void setVoicePitchByName(const QString& soundName, float cents);

    void setVoiceVolumeByName(const QString& soundName, float volumeDb);
    void setVoiceOutputVolumeByName(const QString& soundName, float volumeDb);

    void setVoiceSpatMixByName(const QString& soundName, float mix);

    void setVoiceDistanceScaleByName(const QString& soundName, float scale);

    void setVoiceSendsByName(const QString& soundName,
                             const std::vector<std::pair<harkenstudio::ObjectId, float>>& sends);

    void removeRuntimeObject(harkenstudio::ObjectId objectID);
    void clearRuntimeObjects();
    uint32_t getRuntimeObjectCount() const;

    void registerRuntimeMedia(uint32_t mediaID, const std::string& path, uint64_t fileSize);
    bool prepareMedia(uint32_t mediaID, const std::string& path, uint64_t fileSize);
    bool swapMedia(uint32_t sourceID, const std::string& newPath, uint64_t fileSize = 0);
    void clearRuntimeMedia();
    uint32_t getRuntimeMediaCount() const;

    int activeVoiceCount() const { return activeVoiceCount_; }
    float cpuUsage() const { return cpuUsage_; }
    double outputLatency() const;
    int actualSampleRate() const;

    void getMasterMeterPeak(float& peakL, float& peakR) const;
    float masterMeterPeakL() const { return masterMeterPeakL_; }
    float masterMeterPeakR() const { return masterMeterPeakR_; }

    void setBusVolumeDbById(harkenstudio::ObjectId busId, float db);
    void setBusOutputVolumeDbById(harkenstudio::ObjectId busId, float db);
    float getBusVolumeDbById(harkenstudio::ObjectId busId) const;
    void setBusPanById(harkenstudio::ObjectId busId, float pan);
    void setBusMuteById(harkenstudio::ObjectId busId, bool mute);
    void setBusSoloById(harkenstudio::ObjectId busId, bool solo);
    HarkenEditor::Bridge::EngineResult applyInstalledBusMuteOverlay(
        harkenstudio::ObjectId busId, bool mute, uint32_t fadeMs = 0);
    HarkenEditor::Bridge::EngineResult applyInstalledBusSoloOverlay(
        harkenstudio::ObjectId busId, bool solo, uint32_t fadeMs = 0);
    HarkenEditor::Bridge::EngineResult applyInstalledBusOutputVolumeOverlay(
        harkenstudio::ObjectId busId, float db, uint32_t fadeMs = 0);
    void applyPendingAuditionOverlay();
    void retryBackpressuredAuditionOverlay();
    HarkenEditor::Bridge::IpcQueryStatus getBusMeterPeakById(
        harkenstudio::ObjectId busId, float& peakL, float& peakR) const;

    using VoiceStateBreakdown = HarkenEditor::Bridge::VoiceStateBreakdown;
    bool getVoiceStateBreakdown(VoiceStateBreakdown& out) const;

    using VoiceInfoData = HarkenEditor::Bridge::VoiceInfoData;
    int getActiveVoiceInfoList(std::vector<VoiceInfoData>& outInfos) const;

    void synchronizeBusTopology(
        const std::vector<HarkenEditor::Bridge::RuntimeBusDefinition>& buses,
        const std::vector<HarkenEditor::Bridge::RuntimeBusSendDefinition>& sends);
    HarkenEditor::Bridge::BusTopologyState busTopologyState() const;
    HarkenEditor::Bridge::BusTopologyFailure busTopologyFailure() const;
    void renameBus(harkenstudio::ObjectId busId, const QString& newName);
    QList<harkenstudio::ObjectId> getRuntimeBusIds() const;

    void setBusSendFilter(const QString& srcBusName, int32_t slot,
                          float lpfHz, float hpfHz);
    bool busSendFilter(const QString& srcBusName, int32_t slot,
                       float& lpfHz, float& hpfHz) const;

    uint32_t addListener();
    void removeListener(uint32_t listenerId);
    bool isListenerActive(uint32_t listenerId) const;
    void setListenerPositionById(uint32_t listenerId,
                                 float x, float y, float z,
                                 float fwdX, float fwdY, float fwdZ,
                                 float upX, float upY, float upZ);
    void setGameObjectListeners(uint64_t gameObjectId,
                                const uint32_t* listenerIds, uint32_t count);
    void setListenerSpatialization(uint32_t listenerId, bool spatialized,
                                   uint8_t panningRule, uint8_t speakerConfigType);
    bool getListenerInfo(uint32_t listenerId,
                         float* outX, float* outY, float* outZ,
                         float* outFwdX, float* outFwdY, float* outFwdZ,
                         int* outSpatialized, uint32_t* outPanningRule);
    void setMultiPosition(uint64_t gameObjectId,
                          const float* positions, uint32_t count, uint8_t type);
    void clearMultiPosition(uint64_t gameObjectId);
    void setAutomationPath(uint64_t gameObjectId,
                           const float* points, uint32_t count,
                           uint8_t interp, uint8_t loop);
    void startAutomation(uint64_t gameObjectId);
    void stopAutomation(uint64_t gameObjectId);

    using DebugVoiceData = HarkenEditor::Bridge::DebugVoiceData;
    using DebugBusData = HarkenEditor::Bridge::DebugBusData;
    using DebugPerfData = HarkenEditor::Bridge::DebugPerfData;

    bool getDebugVoices(std::vector<DebugVoiceData>& out) const;
    bool getDebugBuses(std::vector<DebugBusData>& out) const;
    bool getDebugPerformance(DebugPerfData& out) const;

    bool getBusTopologyPlanCount(uint64_t& outCount) const;

    using DebugVoiceMetadataNode = HarkenEditor::Bridge::DebugVoiceMetadataNode;
    bool getVoiceMetadataChain(uint32_t voiceId,
                               std::vector<DebugVoiceMetadataNode>& outChain) const;

    using SessionRecapData = HarkenEditor::Bridge::SessionRecapData;
    bool getSessionRecap(SessionRecapData& out) const;

    using CpuBudgetState = HarkenEditor::Bridge::CpuBudgetState;
    bool getCpuBudgetState(CpuBudgetState& out) const;

    using RenderPlanInfo = HarkenEditor::Bridge::RenderPlanInfo;
    bool getRenderPlan(RenderPlanInfo& out) const;

    using JobStatsInfo = HarkenEditor::Bridge::JobStatsInfo;
    bool getJobStats(JobStatsInfo& out) const;

    using ObjectAudioStatusInfo = HarkenEditor::Bridge::ObjectAudioStatusInfo;
    bool getObjectAudioStatus(ObjectAudioStatusInfo& out) const;
    bool setSpatialFallbackPolicy(uint8_t policy);

    bool initSpatialAudio(uint32_t maxPropDepth = 4, uint32_t numRays = 100,
                          uint32_t maxReflOrder = 2, uint32_t maxDiffOrder = 4);
    void setImageSource(uint32_t srcID, uint32_t sourceID, float posX, float posY, float posZ,
                        float distScale, float level, float diffraction, float occlusion, uint32_t textureID);
    void removeImageSource(uint32_t srcID);
    void clearImageSources();

    void setDiffractionOrder(uint32_t order, bool updatePaths);
    void setLoadBalancingSpread(uint32_t numFrames);
    void setSmoothingConstant(float smoothMs, uint64_t gameObjectID);
    void setTransmissionOperation(uint8_t operation);

    bool loadHRTFDatabase(const uint8_t* data, uint32_t dataSize);
    void unloadHRTFDatabase();
    void setHRTFEnabled(bool enabled);
    void setGameObjectRadius(uint64_t gameObjectID, float outerRadius, float innerRadius);

    using BinauralStatus = HarkenEditor::Bridge::BinauralStatus;
    BinauralStatus getBinauralStatus() const;

    bool reconfigureBufferSize(int32_t newBlockSize);
    int32_t getBufferSize() const;

    HarkenEditor::Bridge::ActionSeekStartResult seekAction(
        quintptr actionHandle, const QString& expectedActionName,
        double positionMs, bool isRelative = false);
    HarkenEditor::Bridge::EngineResult queryActionSeek(
        uint64_t operationId,
        HarkenEditor::Bridge::ActionSeekInfo& outInfo) const;
    HarkenEditor::Bridge::EngineResult releaseActionSeek(uint64_t operationId);
    void setGameObjectPosition(uint64_t gameObjectId, float x, float y, float z);
    void setGameObjectOrientation(uint64_t gameObjectId, float fwdX, float fwdY, float fwdZ);
    void setMixerVolume(harkenstudio::ObjectId busId, float volumeDb);
    bool hasInstalledCurrentWorld() const;
    bool hasActionProgram(harkenstudio::ObjectId actionId) const;
    bool hasCurrentWorldPlayableConsumers() const;
    void cancelCurrentWorldPlayableConsumers();

    struct PreviewPlayableStartResult {
        int32_t status = -1;
        quint64 handle = 0;
        bool rehearsalBlocked = false;
    };

    using PreviewPlayableState = HarkenEditor::Bridge::PreviewPlayableState;

    PreviewPlayableStartResult playPreviewPlayable(
        uint32_t playableKind, harkenstudio::ObjectId stableId, quint64 emitterHandle = 0);
    int32_t resetPreviewPlayable(uint32_t playableKind, harkenstudio::ObjectId stableId);
    int32_t stopPreviewPlayable(quint64 handle, uint32_t fadeMs = 0);
    int32_t queryPreviewPlayable(quint64 handle,
                                 PreviewPlayableState& outState) const;

    HarkenEditor::Bridge::EngineResult setGameObjectVolume(uint64_t gameObjectId, float volume);
    HarkenEditor::Bridge::EngineResult setGameObjectMute(uint64_t gameObjectId, bool mute);
    // false when the pack manager is unavailable
    bool setPackRoutingPriority(const QString& packName, int priority);

    // -- system / debug control (thin session forwards) --
    bool setMonitorFilterMask(uint64_t mask);
    // INT32_MIN when the inspector (or session) is unavailable; negative on full list.
    int32_t watchParameter(uint32_t objectId, uint32_t paramId, uint8_t watchType);
    void unwatchParameter(uint32_t objectId, uint32_t paramId);
    void setMeteringInterval(uint32_t ms);
    bool setCpuBudgetConfig(float escalationPct, float recoveryPct);
    HarkenEditor::Bridge::EngineResult setMemoryBudgetLimit(uint32_t category, uint64_t maxBytes);
    HarkenEditor::Bridge::EngineResult setListenerOutputBus(uint32_t listenerId, harkenstudio::ObjectId busId);
    void setRuntimeLanguage(const QString& code);

    enum class ExecErrorDomain : uint8_t { Music = 0, Stop, Lifecycle, Dispatch, Gate, Unclassified };

    struct ExecError {
        ExecErrorDomain domain = ExecErrorDomain::Music;
        uint8_t  severity = 2;
        uint32_t objectID = 0;
        quint64  playingID = 0;
        QString  objectName;
        QString  errorType;
        QString  message;
        qint64   timestampMs = 0;
    };
    void addExecError(ExecErrorDomain domain, uint8_t severity, uint32_t objectID,
                      quint64 playingID, const QString& objectName,
                      const QString& errorType, const QString& message);
    QVector<ExecError> recentExecErrors(int maxCount, int domainFilter ) const;
    void clearExecErrors(int domainFilter );
    void consumeRuntimeNotificationsForTest();
    void emitLogMessageForTest(int level, const QString& message);
    const std::vector<HarkenEditor::Bridge::PlaybackPathObservation>& playbackPaths() const {
        return playbackPaths_;
    }

signals:
    void initializedChanged(bool initialized);
    void playingChanged(bool playing);
    void voiceCountChanged(int count);
    void voiceCountUnavailable();
    void playbackPathsChanged();
    void cpuUsageChanged(float cpu);
    void meterLevelsUpdated();
    void playbackStarted(quintptr playbackHandle, const QString& name);
    void playbackStopped(quintptr playbackHandle);
    void errorOccurred(const QString& error);
    void logMessage(int level, const QString& message);
    void meterDataReady(int meterId);
    void execErrorOccurred(int domain, QString errorType, QString message);

private slots:
    void onRuntimeServiceTimer();
    void onMeterTimer();
    void onMonitorTimer();

private:
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    PlaybackStartResult playByNameInternal(const QString& name, bool usePreviewContext);
    quint64 ensurePreviewEmitter(HarkenEditor::Bridge::EngineResult* outResult = nullptr);
    void releasePreviewEmitter();
    void capturePreviewVariableOverride(const QUuid& variableId);
    void applyPreviewVariableOverrides();
    void invalidatePreviewVariableAcceptance();
    void syncRuntimeBindings();
    bool hasPendingPreviewVariableOverrides() const;
    void restoreCurrentProjectAfterRuntimeRestart();
    void stopRuntimeSynchronization();
    void updateRuntimeServiceInterval();
    void consumeRuntimeNotifications();
    void refreshPlaybackPaths();
    void applyLivePropertyPreview();
    void resetLivePropertyPreview();
    void bindCurrentWorldPlatformContext();

    friend class CurrentWorldPrimaryScope;

    std::unique_ptr<HarkenEditor::Bridge::EngineSession> session_;
    std::shared_ptr<LivePropertyPreview> livePropertyPreview_;
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> playbackPaths_;
    std::unique_ptr<RuntimeSyncCoordinator> runtimeSyncCoordinator_;
    std::unique_ptr<CurrentWorldCompilation> currentWorldCompilation_;
    std::uint64_t currentWorldRequestSession_ = 0;
    std::uint64_t currentWorldRequestEpoch_ = 0;
    std::vector<std::pair<harkenstudio::ObjectId, std::uint8_t>> currentWorldPreviewMutations_;
    ProjectPlatformId lastCurrentWorldPrimary_;
    bool currentWorldPlatformBound_ = false;
    VariableRehearsal* variableRehearsal_ = nullptr;
    QMetaObject::Connection variableRehearsalConnection_;
    bool runtimeSessionActive_ = false;
    bool initialized_ = false;
    bool playing_ = false;
    bool runtimeProjectRestoreRequired_ = false;
    quint64 previewEmitterHandle_ = 0;
    struct PreviewVariableOverrideState {
        bool hasValue = false;
        float value = 0.0f;
        float submittedValue = 0.0f;
        bool submittedHasValue = false;
        quint64 acceptedEmitter = 0;
        bool acceptedGlobal = false;
        bool mustResubmit = false;
        bool inFlight = false;
        quint64 inFlightSequence = 0;
        quint64 inFlightWorld = 0;
        quint64 inFlightTransport = 0;
        VariableScope scope = VariableScope::Emitter;
    };
    QHash<QUuid, PreviewVariableOverrideState> previewVariableOverrides_;
    std::vector<RehearsalPushRecord> rehearsalPushLog_;
    static constexpr size_t kRehearsalPushLogCap = 8;
    std::vector<harkenstudio::ObjectId> runtimeBindingIds_;
    std::vector<BindingRuntimeAttachment> runtimeBindingAttachments_;
    QHash<harkenstudio::ObjectId, BindingRuntimeProgram> runtimeBindingPrograms_;
    QHash<harkenstudio::ObjectId, QVector<std::pair<QUuid, BindingRuntimeProgram>>> runtimeBindingPreviews_;
    std::array<uint64_t, 3> runtimeBindingSyncIdentity_{};
    bool runtimeBindingsPending_ = true;
    float masterVolume_ = 1.0f;

    QVector3D listenerPosition_;
    QVector3D listenerForward_ = QVector3D(0, 0, -1);
    QVector3D listenerUp_ = QVector3D(0, 1, 0);

    QTimer* runtimeServiceTimer_ = nullptr;
    QTimer* meterTimer_ = nullptr;
    QTimer* monitorTimer_ = nullptr;
    int activeVoiceCount_ = 0;
    bool voiceCountKnown_ = false;
    float cpuUsage_ = 0.0f;
    float masterMeterPeakL_ = 0.0f;
    float masterMeterPeakR_ = 0.0f;
    QSet<harkenstudio::ObjectId> overlayMutePendingIds_;
    QSet<harkenstudio::ObjectId> overlaySoloPendingIds_;
    QHash<harkenstudio::ObjectId, float> overlayOutputVolumePendingValues_;

    QVector<ExecError> execErrors_;
    static constexpr int kMaxExecErrors = 100;

    static AudioEngine* instance_;
};

}
