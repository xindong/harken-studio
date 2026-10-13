
#pragma once

#include <QObject>
#include <QMap>
#include <QVector>
#include <QString>
#include <QJsonObject>
#include <QTimer>
#include <cstdint>

#include "RuntimeBridge/PlaybackBridge.h"

namespace VNX { class AudioEngine; struct ProjectItem; }

namespace HarkenEditor {

enum class AuditionMode { NamedPlayback, Preview, RawFile };

struct AuditionInfo
{
    uint32_t playingId      = 0;
    quintptr playbackHandle    = 0;
    uint32_t sourceID       = 0;
    AuditionMode mode      = AuditionMode::NamedPlayback;
    QString  objectName;
    int      state          = 0;
    float    positionMs     = 0.0f;
    float    durationMs     = 0.0f;
    uint64_t gameObjId      = 0;
    float    playbackSpeed  = 1.0f;
    bool     bIsPlaying     = false;
    bool     bIsDelayed     = false;
    bool     bIsFading      = false;
    bool     previewStartPending = false;
    bool     revisionPending = false;
    bool     actionInstallPending = false;
    uint32_t previewPlayableKind = 0;
    harkenstudio::ObjectId runtimeStableId = {};
    uint64_t requiredRevision = 0;
    uint64_t projectSessionId = 0;
    uint64_t runtimeEpoch = 0;
    uint64_t previewIntentGeneration = 0;
    qint64   revisionDeadlineMs = 0;
    uint32_t previewStartAttempts = 0;
    bool     previewStopPending = false;
    uint32_t previewStopFadeMs = 0;
    QString  previewReason;
    int32_t  previewEngineResult = 0;
    uint32_t previewLifecycle = 0;
    uint32_t childLiveCount = 0;
    uint32_t childAudibleRetiringCount = 0;
    uint32_t pendingFutureCount = 0;
    bool     previewCancelling = false;
    bool     previewReplacePending = false;
    bool     previewStopStuck = false;
    qint64   previewStopDeadlineMs = 0;
};

struct AuditionPosition
{
    int64_t  samplePosition = 0;
    double   timeSeconds    = 0.0;
    uint32_t beat           = 0;
    uint32_t bar            = 0;
    float    beatFraction   = 0.0f;
    bool     isMusic        = false;
};

struct AuditionSimulation
{
    QMap<QString, QString> stateValues;
    QMap<QString, QString> switchValues;
};

struct AuditionSeekStartResult
{
    uint64_t requestId = 0;
    HarkenEditor::Bridge::EngineResult result =
        HarkenEditor::Bridge::kEngineOk;
};

class AuditionManager : public QObject
{
    Q_OBJECT
public:
    static constexpr int MAX_AUDITIONS = 64;

    explicit AuditionManager(VNX::AudioEngine* engine, QObject* parent = nullptr);
    ~AuditionManager() override;

    uint32_t play(const QString& objectPath, uint64_t gameObjectId = 0);
    uint32_t playItem(const VNX::ProjectItem& item,
                      uint64_t gameObjectId = 0);
    HarkenEditor::Bridge::PlaybackFailure lastPlayFailure() const {
        return lastPlayFailure_;
    }
    HarkenEditor::Bridge::EngineResult lastPlayEngineResult() const {
        return lastPlayEngineResult_;
    }
    HarkenEditor::Bridge::BusTopologyFailure lastPlayTopologyFailure() const {
        return lastPlayTopologyFailure_;
    }
    QString lastPlayReason() const { return lastPlayReason_; }
    bool lastPlayPending() const { return lastPlayPending_; }
    QJsonObject previewIpcSnapshot(uint32_t playingId = 0) const;
    void stop(uint32_t playingId, float fadeMs = 0);
    void stopAll(float fadeMs = 0);

    uint32_t registerExternalPlayback(const QString& objectName,
                                      quintptr playbackHandle);
    uint32_t registerRawFilePlayback(const QString& objectName,
                                     quintptr playbackHandle,
                                     uint32_t sourceId = 0);


    void togglePlayback(const QString& objectPath);
    bool hasActiveAudition() const;
    bool hasActiveNonRawAudition() const;

    AuditionPosition getPositionSamples(uint32_t playingId) const;
    AuditionSeekStartResult setPositionSamples(
        uint32_t playingId, int64_t samplePos);

    void  setPlaybackSpeed(uint32_t playingId, float speed);
    float getPlaybackSpeed(uint32_t playingId) const;

    int32_t resetPreviewContainer(harkenstudio::ObjectId runtimeStableId);
    static bool previewResetSucceeded(int32_t status);
    static bool previewResetBackpressured(int32_t status);

    void simulateState(const QString& groupName, const QString& stateName);
    void simulateSwitch(const QString& groupName, const QString& stateName,
                        uint64_t gameObjectId = 0);
    void resetAllSimulations();
    QMap<QString, QString> getCurrentStates() const { return simulatedStates_; }
    QMap<QString, QString> getCurrentSwitches(uint64_t gameObjectId = 0) const;

    AuditionSimulation getCurrentSimulation() const;

    QVector<AuditionInfo> getActiveAuditions() const;
    AuditionInfo getAudition(uint32_t playingId) const;
    int activeAuditionCount() const { return auditions_.size(); }

    uint32_t getAuditionForObject(const QString& objectPath) const;
    void closeAudition(uint32_t playingId);

signals:
    void auditionStarted(uint32_t playingId, const QString& objectName);
    void auditionStartFailed(uint32_t playingId, const QString& objectName,
                              int32_t engineResult);
    void auditionStateChanged(uint32_t playingId, int newState);
    void auditionPositionChanged(uint32_t playingId, double timeSeconds);
    void auditionSeekFinished(uint32_t playingId, quint64 requestId,
                              int32_t engineResult, bool applied);
    void auditionSpeedChanged(uint32_t playingId, float speed);
    void stateSimulated(const QString& group, const QString& state);
    void switchSimulated(const QString& group, const QString& state, uint64_t gameObjId);
    void allAuditionsStopped();

private slots:
    void pollAuditionStates();
    void retryPendingPreviewStarts();
    void onRuntimeSyncBarrierChanged();
    void expirePendingRevisionStarts();
    void onEnginePlaybackStarted(quintptr handle, const QString& name);
    void onEnginePlaybackStopped(quintptr handle);

private:
    struct PendingActionSeek {
        uint32_t playingId = 0;
        uint64_t requestId = 0;
        double requestedPositionMs = 0.0;
    };

    void pollPendingActionSeeks();
    uint64_t allocateSeekRequestId();

    VNX::AudioEngine* engine_ = nullptr;
    QTimer pollTimer_;
    QTimer previewStartRetryTimer_;
    QTimer previewRevisionTimeoutTimer_;
    uint32_t nextPlayingId_ = 1;
    uint64_t nextPreviewIntentGeneration_ = 1;
    uint64_t nextSeekRequestId_ = 1;
    QMap<uint64_t, PendingActionSeek> pendingActionSeeks_;

    int idleZeroPolls_ = 0;

    QMap<uint32_t, AuditionInfo> auditions_;
    QMap<QString, QString> simulatedStates_;

    struct SwitchKey {
        QString group;
        uint64_t gameObjId;
        bool operator<(const SwitchKey& o) const {
            if (group != o.group) return group < o.group;
            return gameObjId < o.gameObjId;
        }
    };
    QMap<SwitchKey, QString> simulatedSwitches_;
    HarkenEditor::Bridge::PlaybackFailure lastPlayFailure_ =
        HarkenEditor::Bridge::PlaybackFailure::None;
    HarkenEditor::Bridge::EngineResult lastPlayEngineResult_ =
        HarkenEditor::Bridge::kEngineOk;
    HarkenEditor::Bridge::BusTopologyFailure lastPlayTopologyFailure_;
    QString lastPlayReason_;
    bool lastPlayPending_ = false;

    uint32_t playResolved(const QString& objectName,
                          const VNX::ProjectItem* item,
                          uint64_t gameObjectId);
    bool ensurePreviewOutput();
    uint32_t playTypedPreview(const QString& objectName,
                              const VNX::ProjectItem& item,
                              uint64_t gameObjectId,
                              uint32_t playableKind);
    void requestTypedPreviewStop(AuditionInfo& info);
    void startLatchedTypedPreview(AuditionInfo& info);
    bool startTypedPreviewNow(AuditionInfo& info);
    void failImmediatePlay(const QString& reason,
                           HarkenEditor::Bridge::EngineResult result);
    void failNotPlayable(const QString& reason);
    void removePreviewSlot(uint32_t playingId, const QString& objectName,
                           int32_t engineResult, bool emitStartFailed);
    void markPreviewStopStuck(AuditionInfo& info);
    void emitIfAllStopped();
};

}
