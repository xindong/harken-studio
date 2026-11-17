
#include "AuditionManager.h"
#include "ActionStepUtils.h"
#include "AudioEngine.h"
#include "Components/VAction.h"
#include "ProjectManager.h"
#include "RuntimeSyncCoordinator.h"

#include "RuntimeBridge/PreviewBridge.h"

#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace VNX { ProjectItem* findItemByPathInProject(ProjectData* project, const QString& path); }

namespace {

std::optional<uint32_t> previewKind(VNX::ProjectItem::Type type)
{
    switch (type) {
        case VNX::ProjectItem::Type::Sound:
            return HarkenEditor::Bridge::kPreviewPlayableSound;
        case VNX::ProjectItem::Type::Container:
            return HarkenEditor::Bridge::kPreviewPlayableContainer;
        case VNX::ProjectItem::Type::Track:
            return HarkenEditor::Bridge::kPreviewPlayableTrack;
        case VNX::ProjectItem::Type::Segment:
            return HarkenEditor::Bridge::kPreviewPlayableSegment;
        default:
            return std::nullopt;
    }
}

uint32_t previewFadeMs(float value)
{
    if (!std::isfinite(value) || value <= 0.0f)
        return 0;
    return static_cast<uint32_t>(std::min<double>(
        value, std::numeric_limits<uint32_t>::max()));
}

constexpr int kPreviewStartRetryIntervalMs = 10;
constexpr uint32_t kPreviewStartRetryLimit = 16;
constexpr int kPreviewRevisionTimeoutMs = 5000;
constexpr int kPreviewStopStuckMs = 1000;
constexpr uint32_t kAuditionStopAttemptLimit = 10;

bool actionHasAuthoredPlayableStep(const VNX::ProjectItem& item)
{
    if (item.type != VNX::ProjectItem::Type::Action)
        return false;
    const auto* action = item.comp<VNX::VAction>();
    if (action == nullptr || action->steps.empty())
        return false;
    auto* project = VNX::ProjectManager::instance().currentProject();
    if (project == nullptr)
        return false;
    const auto lookup = VNX::EditorUtils::makeProjectTargetLookup(*project);
    for (const auto& step : action->steps) {
        const auto resolved = VNX::EditorUtils::resolveActionStepTarget(
            step.type, step.targetObjectID, step.targetName, lookup,
            item.objectID);
        if (resolved.state != VNX::EditorUtils::ActionTargetState::Empty
            && resolved.state != VNX::EditorUtils::ActionTargetState::Dangling)
            return true;
    }
    return false;
}

bool previewQueryRetired(
    int32_t queryResult, const VNX::AudioEngine::PreviewPlayableState& state,
    bool stopOccupancy)
{
    if (queryResult == HarkenEditor::Bridge::kPreviewStaleHandle
        || queryResult == HarkenEditor::Bridge::kPreviewUnknownHandle)
        return true;
    if (stopOccupancy)
        return false;
    return queryResult == HarkenEditor::Bridge::kPreviewOk
        && state.lifecycle == HarkenEditor::Bridge::kPreviewLifecycleLogicalComplete
        && state.childLiveCount == 0
        && state.childAudibleRetiringCount == 0
        && state.pendingFutureCount == 0;
}

bool previewQueryActive(
    int32_t queryResult, const VNX::AudioEngine::PreviewPlayableState& state)
{
    return queryResult == HarkenEditor::Bridge::kPreviewOk
        && state.lifecycle == HarkenEditor::Bridge::kPreviewLifecycleActive;
}

void applyPreviewQuery(
    HarkenEditor::AuditionInfo& info, int32_t queryResult,
    const VNX::AudioEngine::PreviewPlayableState& state)
{
    if (queryResult != HarkenEditor::Bridge::kPreviewOk)
        return;
    info.previewLifecycle = state.lifecycle;
    info.childLiveCount = state.childLiveCount;
    info.childAudibleRetiringCount = state.childAudibleRetiringCount;
    info.pendingFutureCount = state.pendingFutureCount;
}

qint64 monotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

HarkenEditor::Bridge::EngineResult previewEngineResult(int32_t status)
{
    if (status == HarkenEditor::Bridge::kPreviewOk)
        return HarkenEditor::Bridge::kEngineOk;
    if (status == HarkenEditor::Bridge::kPreviewBackpressure)
        return HarkenEditor::Bridge::kEngineBackpressure;
    if (status == HarkenEditor::Bridge::kPreviewHostError)
        return HarkenEditor::Bridge::kEngineNotInit;
    return HarkenEditor::Bridge::kEngineUnknownId;
}
}

namespace HarkenEditor {

AuditionManager::AuditionManager(VNX::AudioEngine* engine, QObject* parent)
    : QObject(parent)
    , engine_(engine)
{
    pollTimer_.setInterval(100);
    connect(&pollTimer_, &QTimer::timeout, this, &AuditionManager::pollAuditionStates);
    pollTimer_.start();

    previewStartRetryTimer_.setInterval(kPreviewStartRetryIntervalMs);
    previewStartRetryTimer_.setTimerType(Qt::PreciseTimer);
    connect(&previewStartRetryTimer_, &QTimer::timeout,
            this, &AuditionManager::retryPendingPreviewStarts);

    previewRevisionTimeoutTimer_.setInterval(100);
    connect(&previewRevisionTimeoutTimer_, &QTimer::timeout,
            this, &AuditionManager::expirePendingRevisionStarts);

    const auto invalidateMedia = [this](const QUuid& soundGuid) {
        const auto active = getActiveAuditions();
        for (const auto& audition : active)
            if (audition.previewSounds.contains(VNX::runtimeObjectId(soundGuid)))
                stop(audition.playingId);
    };
    connect(&VNX::ProjectManager::instance(), &VNX::ProjectManager::previewMediaInvalidated,
            this, invalidateMedia);
    connect(&VNX::ProjectManager::instance(), &VNX::ProjectManager::itemPropertyChanged,
            this, [invalidateMedia](VNX::ProjectItem* item, const QString& property) {
        if (item && property == QStringLiteral("sources")) invalidateMedia(item->guid);
    });

    if (engine_) {
        connect(engine_, &VNX::AudioEngine::playbackStarted,
                this, &AuditionManager::onEnginePlaybackStarted);
        connect(engine_, &VNX::AudioEngine::playbackStopped,
                this, &AuditionManager::onEnginePlaybackStopped);
        connect(&engine_->runtimeSyncCoordinator(),
                &VNX::RuntimeSyncCoordinator::barrierChanged,
                this, &AuditionManager::onRuntimeSyncBarrierChanged);
        const auto clearAuditionsWhenInactive = [this](bool active) {
            if (active)
                return;
            for (auto pending = pendingActionSeeks_.cbegin();
                 pending != pendingActionSeeks_.cend(); ++pending) {
                finishActionSeek(*pending, HarkenEditor::Bridge::kEngineNotInit, false);
            }
            pendingActionSeeks_.clear();
            if (auditions_.isEmpty())
                return;
            QList<uint32_t> ids = auditions_.keys();
            auditions_.clear();
            previewStartRetryTimer_.stop();
            previewRevisionTimeoutTimer_.stop();
            for (uint32_t id : ids)
                emit auditionStateChanged(id, 0);
            emitIfAllStopped();
        };
        connect(engine_, &VNX::AudioEngine::initializedChanged,
                this, clearAuditionsWhenInactive);
        connect(engine_, &VNX::AudioEngine::playingChanged,
                this, clearAuditionsWhenInactive);
    }
}

AuditionManager::~AuditionManager()
{
    pollTimer_.stop();
    previewStartRetryTimer_.stop();
    previewRevisionTimeoutTimer_.stop();
}


uint32_t AuditionManager::play(const QString& objectPath, uint64_t gameObjectId)
{
    VNX::ProjectItem* item = nullptr;
    auto* project = VNX::ProjectManager::instance().currentProject();
    if (project)
        item = VNX::findItemByPathInProject(project, objectPath);
    return playResolved(objectPath, item, gameObjectId);
}

uint32_t AuditionManager::playItem(const VNX::ProjectItem& item,
                                    uint64_t gameObjectId)
{
    return playResolved(item.name, &item, gameObjectId);
}

void AuditionManager::failImmediatePlay(
    const QString& reason, HarkenEditor::Bridge::EngineResult result)
{
    lastPlayFailure_ = HarkenEditor::Bridge::PlaybackFailure::RuntimeRefused;
    lastPlayEngineResult_ = result;
    lastPlayReason_ = reason;
    lastPlayPending_ = false;
}

void AuditionManager::failNotPlayable(const QString& reason)
{
    lastPlayFailure_ = HarkenEditor::Bridge::PlaybackFailure::NotPlayable;
    lastPlayEngineResult_ = HarkenEditor::Bridge::kEngineOk;
    lastPlayReason_ = reason;
    lastPlayPending_ = false;
}

void AuditionManager::removePreviewSlot(
    uint32_t playingId, QString objectName, int32_t engineResult,
    bool emitStartFailed)
{
    auditions_.remove(playingId);
    if (emitStartFailed)
        emit auditionStartFailed(playingId, objectName, engineResult);
    emit auditionStateChanged(playingId, 0);
    emitIfAllStopped();
}

bool AuditionManager::ensurePreviewOutput()
{
    if (engine_->isPlaying())
        return true;
    if (engine_->startOutput())
        return true;
    failImmediatePlay(QStringLiteral("unknown"),
                      HarkenEditor::Bridge::kEngineNotInit);
    return false;
}

void AuditionManager::markPreviewStopStuck(AuditionInfo& info)
{
    info.previewStopStuck = true;
    info.previewReplacePending = false;
    info.previewStartPending = false;
    info.revisionPending = false;
    info.bIsPlaying = false;
    info.bIsDelayed = false;
    info.state = 0;
    info.previewReason = QStringLiteral("stop-stuck");
    info.previewEngineResult =
        static_cast<int32_t>(HarkenEditor::Bridge::kEngineBackpressure);
    emit auditionStartFailed(
        info.playingId, info.objectName,
        static_cast<int32_t>(HarkenEditor::Bridge::kEngineBackpressure));
}

bool AuditionManager::queuePreviewStart(AuditionInfo& info, bool awaitingRevision,
                                         const QString& reason)
{
    info.revisionPending = awaitingRevision;
    info.previewStartPending = !awaitingRevision;
    info.playbackHandle = 0;
    info.previewReason = reason;
    info.previewEngineResult = Bridge::kEngineBackpressure;
    info.state = 0;
    info.bIsPlaying = false;
    info.bIsDelayed = true;
    lastPlayFailure_ = Bridge::PlaybackFailure::None;
    lastPlayEngineResult_ = Bridge::kEngineBackpressure;
    lastPlayReason_ = reason;
    lastPlayPending_ = true;
    if (awaitingRevision) {
        info.previewStartAttempts = 0;
        if (!previewRevisionTimeoutTimer_.isActive())
            previewRevisionTimeoutTimer_.start();
    } else if (!previewStartRetryTimer_.isActive()) {
        previewStartRetryTimer_.start();
    }
    return true;
}

bool AuditionManager::startPreviewNow(AuditionInfo& info)
{
    const uint32_t playingId = info.playingId;
    const bool reportFailure = info.mode == AuditionMode::Preview
        || info.revisionPending || info.previewStartPending;
    const auto fail = [&](const QString& reason, Bridge::EngineResult result) {
        failImmediatePlay(reason, result);
        removePreviewSlot(playingId, info.objectName, result, reportFailure);
        return false;
    };
    if (!engine_ || !engine_->isInitialized())
        return fail(QStringLiteral("runtime"), Bridge::kEngineNotInit);

    const auto barrier = engine_->currentWorldPlaybackBarrier();
    if (barrier.projectSessionId != info.projectSessionId
        || barrier.runtimeEpoch != info.runtimeEpoch)
        return fail(QStringLiteral("runtime"), Bridge::kEngineNotInit);
    if (info.revisionPending && monotonicMilliseconds() >= info.revisionDeadlineMs)
        return fail(QStringLiteral("revision"), Bridge::kEngineBackpressure);

    const auto* item = VNX::ProjectManager::instance()
        .findItemByRuntimeStableId(info.runtimeStableId);
    const bool named = info.mode == AuditionMode::NamedPlayback;
    if (!item || (named ? item->type != VNX::ProjectItem::Type::Action
                       : previewKind(item->type) != info.previewPlayableKind))
        return fail(QStringLiteral("unavailable"), Bridge::kEngineUnknownId);
    info.requiredRevision = barrier.requiredRevision;
    using State = VNX::RuntimeSyncCoordinator::RevisionBarrierState;
    if (barrier.state == State::Pending)
        return queuePreviewStart(info, true, QStringLiteral("revision"));
    if (barrier.state != State::Ready)
        return fail(QStringLiteral("synchronization"), Bridge::kEngineNotInit);

    quint64 handle = 0;
    if (named) {
        const bool hasPlayableStep = actionHasAuthoredPlayableStep(*item);
        if (!hasPlayableStep || !engine_->hasActionProgram(info.runtimeStableId)) {
            failNotPlayable(hasPlayableStep
                ? QStringLiteral("unavailable") : QStringLiteral("empty"));
            removePreviewSlot(playingId, info.objectName, 0, reportFailure);
            return false;
        }
        const auto result = engine_->playByNameWithPreviewContext(item->name);
        if (result.previewVariablesPending
            || result.engineResult == Bridge::kEngineBackpressure)
            return queuePreviewStart(info, false, result.previewVariablesPending
                ? QStringLiteral("variables") : QStringLiteral("admission"));
        if (result.engineResult != Bridge::kEngineOk || result.playbackHandle == 0) {
            lastPlayFailure_ = result.failure;
            lastPlayEngineResult_ = result.engineResult;
            lastPlayTopologyFailure_ = result.topologyFailure;
            lastPlayReason_ = QStringLiteral("unknown");
            lastPlayPending_ = false;
            removePreviewSlot(playingId, info.objectName, result.engineResult, reportFailure);
            return false;
        }
        handle = result.playbackHandle;
    } else {
        const auto result = engine_->playPreviewPlayable(
            info.previewPlayableKind, info.runtimeStableId);
        if (result.status == Bridge::kPreviewBackpressure)
            return queuePreviewStart(info, false, QStringLiteral("admission"));
        if (result.status != Bridge::kPreviewOk || result.handle == 0)
            return fail(QStringLiteral("unknown"), previewEngineResult(result.status));
        handle = result.handle;
    }

    info.playbackHandle = handle;
    info.previewStartPending = false;
    info.revisionPending = false;
    info.previewReplacePending = false;
    info.previewStopPending = false;
    info.previewCancelling = false;
    info.previewStopStuck = false;
    info.previewStopDeadlineMs = 0;
    info.stopState = AuditionStopState::None;
    info.stopResult = Bridge::kEngineOk;
    info.stopAttempts = 0;
    info.previewReason.clear();
    info.previewEngineResult = 0;
    info.state = 1;
    info.bIsPlaying = true;
    info.bIsDelayed = false;
    lastPlayFailure_ = Bridge::PlaybackFailure::None;
    lastPlayEngineResult_ = Bridge::kEngineOk;
    lastPlayTopologyFailure_ = {};
    lastPlayReason_.clear();
    lastPlayPending_ = false;
    const QString objectName = info.objectName;
    emit auditionStarted(playingId, objectName);
    emit auditionStateChanged(playingId, 1);
    return true;
}

void AuditionManager::startLatchedTypedPreview(AuditionInfo& info)
{
    if (info.previewStopStuck)
        return;
    info.playbackHandle = 0;
    info.previewStopPending = false;
    info.previewCancelling = false;
    info.previewReplacePending = false;
    info.previewStopDeadlineMs = 0;
    info.previewStartAttempts = 0;
    info.revisionDeadlineMs = monotonicMilliseconds() + kPreviewRevisionTimeoutMs;
    startPreviewNow(info);
}

void AuditionManager::requestTypedPreviewStop(AuditionInfo& info)
{
    info.stopState = AuditionStopState::None;
    info.stopResult = Bridge::kEngineOk;
    if (info.playbackHandle == 0) {
        if (info.previewReplacePending && !info.previewStopStuck)
            startLatchedTypedPreview(info);
        return;
    }

    info.previewStopFadeMs = 0;
    const int32_t stopResult =
        engine_->stopPreviewPlayable(info.playbackHandle, 0);
    VNX::AudioEngine::PreviewPlayableState state;
    const int32_t queryResult =
        engine_->queryPreviewPlayable(info.playbackHandle, state);
    applyPreviewQuery(info, queryResult, state);

    if (previewQueryRetired(queryResult, state, true)
        || (stopResult != HarkenEditor::Bridge::kPreviewOk
            && stopResult != HarkenEditor::Bridge::kPreviewBackpressure)) {
        info.previewStopPending = false;
        info.previewCancelling = false;
        if (info.previewReplacePending && !info.previewStopStuck)
            startLatchedTypedPreview(info);
        return;
    }

    if (info.previewStopDeadlineMs == 0) {
        info.previewStopDeadlineMs =
            monotonicMilliseconds() + kPreviewStopStuckMs;
    }
    info.bIsPlaying = false;
    info.bIsDelayed = true;
    info.state = 0;
    info.previewReason = QStringLiteral("occupancy");
    lastPlayReason_ = QStringLiteral("occupancy");
    lastPlayPending_ = true;

    if (stopResult == HarkenEditor::Bridge::kPreviewBackpressure
        && previewQueryActive(queryResult, state)) {
        info.previewStopPending = true;
        info.previewCancelling = false;
        info.previewEngineResult =
            static_cast<int32_t>(HarkenEditor::Bridge::kEngineBackpressure);
        lastPlayEngineResult_ = HarkenEditor::Bridge::kEngineBackpressure;
        return;
    }

    info.previewStopPending = false;
    info.previewCancelling = true;
    info.bIsFading = true;
    info.previewEngineResult = 0;
    lastPlayEngineResult_ = HarkenEditor::Bridge::kEngineOk;
}

uint32_t AuditionManager::playAuthoredPreview(
    const QString& objectName, const VNX::ProjectItem& item,
    uint64_t gameObjectId)
{
    if (auditions_.size() >= MAX_AUDITIONS) {
        failImmediatePlay(QStringLiteral("capacity"), Bridge::kEngineInUse);
        qWarning("[AuditionManager] Max auditions (%d) reached, cannot play '%s'",
                 MAX_AUDITIONS, qPrintable(objectName));
        return 0;
    }

    const auto previewSounds = VNX::ProjectManager::instance().prepareRuntimePreview(item);
    const auto barrier = engine_->currentWorldPlaybackBarrier();
    const auto kind = previewKind(item.type);
    AuditionInfo info;
    info.playingId = nextPlayingId_++;
    info.objectName = objectName;
    info.mode = kind ? AuditionMode::Preview : AuditionMode::NamedPlayback;
    info.previewPlayableKind = kind.value_or(0);
    info.runtimeStableId = item.runtimeId();
    info.previewSounds = previewSounds;
    info.gameObjId = gameObjectId;
    info.projectSessionId = barrier.projectSessionId;
    info.runtimeEpoch = barrier.runtimeEpoch;
    info.requiredRevision = barrier.requiredRevision;
    info.revisionDeadlineMs = monotonicMilliseconds() + kPreviewRevisionTimeoutMs;
    info.previewIntentGeneration = nextPreviewIntentGeneration_++;
    info.bIsDelayed = true;
    const auto playingId = info.playingId;
    auditions_[playingId] = info;
    auto it = auditions_.find(playingId);
    return startPreviewNow(*it) ? playingId : 0;
}

uint32_t AuditionManager::playResolved(const QString& objectName,
                                        const VNX::ProjectItem* item,
                                        uint64_t gameObjectId)
{
    lastPlayFailure_ = Bridge::PlaybackFailure::None;
    lastPlayEngineResult_ = Bridge::kEngineOk;
    lastPlayTopologyFailure_ = {};
    lastPlayReason_.clear();
    lastPlayPending_ = false;
    if (!engine_ || !engine_->isInitialized()) {
        failImmediatePlay(QStringLiteral("unknown"), Bridge::kEngineNotInit);
        return 0;
    }
    if (!ensurePreviewOutput())
        return 0;

    if (item && (previewKind(item->type) || item->type == VNX::ProjectItem::Type::Action))
        return playAuthoredPreview(objectName, *item, gameObjectId);

    if (auditions_.size() >= MAX_AUDITIONS) {
        failImmediatePlay(QStringLiteral("capacity"), Bridge::kEngineInUse);
        qWarning("[AuditionManager] Max auditions (%d) reached, cannot play '%s'",
                 MAX_AUDITIONS, qPrintable(objectName));
        return 0;
    }

    QString dispatchName = item != nullptr ? item->name : objectName;
    if (item == nullptr) {
        const int leafStart = objectName.lastIndexOf('/') + 1;
        if (leafStart > 0 && leafStart < objectName.size())
            dispatchName = objectName.mid(leafStart);
    }
    const auto result = engine_->playByNameDetailed(dispatchName);
    lastPlayFailure_ = result.failure;
    lastPlayEngineResult_ = result.engineResult;
    lastPlayTopologyFailure_ = result.topologyFailure;
    if (result.playbackHandle == 0) {
        lastPlayReason_ = QStringLiteral("unknown");
        return 0;
    }
    AuditionInfo info;
    info.playingId = nextPlayingId_++;
    info.objectName = objectName;
    info.playbackHandle = result.playbackHandle;
    info.gameObjId = gameObjectId;
    info.previewIntentGeneration = nextPreviewIntentGeneration_++;
    info.revisionDeadlineMs = monotonicMilliseconds() + kPreviewRevisionTimeoutMs;
    info.state = 1;
    info.bIsPlaying = true;
    auditions_[info.playingId] = info;
    emit auditionStarted(info.playingId, objectName);
    return info.playingId;
}

uint32_t AuditionManager::registerExternalPlayback(const QString& objectName,
                                                    quintptr playbackHandle)
{
    if (playbackHandle == 0 || auditions_.size() >= MAX_AUDITIONS)
        return 0;

    AuditionInfo info;
    info.playingId = nextPlayingId_++;
    info.objectName = objectName;
    info.playbackHandle = playbackHandle;
    info.state = 1;
    info.bIsPlaying = true;
    auditions_[info.playingId] = info;

    emit auditionStarted(info.playingId, objectName);
    return info.playingId;
}

uint32_t AuditionManager::registerRawFilePlayback(const QString& objectName,
                                                   quintptr playbackHandle,
                                                   uint32_t sourceId)
{
    if (playbackHandle == 0 || auditions_.size() >= MAX_AUDITIONS)
        return 0;

    AuditionInfo info;
    info.playingId = nextPlayingId_++;
    info.objectName = objectName;
    info.playbackHandle = playbackHandle;
    info.sourceID = sourceId;
    info.mode = AuditionMode::RawFile;
    info.state = 1;
    info.bIsPlaying = true;
    auditions_[info.playingId] = info;

    emit auditionStarted(info.playingId, objectName);
    return info.playingId;
}

void AuditionManager::stop(uint32_t playingId, float fadeMs)
{
    auto it = auditions_.find(playingId);
    if (it == auditions_.end()) return;

    if ((it->previewStartPending || it->revisionPending)
        && it->playbackHandle == 0
        && !it->previewStopPending
        && !it->previewCancelling) {
        auditions_.erase(it);
        emit auditionStateChanged(playingId, 0);
        emitIfAllStopped();
        return;
    }

    const quintptr handle = it->playbackHandle;

    if (it->mode == AuditionMode::RawFile) {
        if (engine_ && handle != 0)
            engine_->stopRawPcm(static_cast<uint64_t>(handle), previewFadeMs(fadeMs));
        auditions_.erase(it);
        emit auditionStateChanged(playingId, 0);
        emitIfAllStopped();
        return;
    }

    it->previewReplacePending = false;
    if (it->stopState == AuditionStopState::Pending
        || it->stopState == AuditionStopState::Stopping)
        return;
    if (handle == 0) {
        finishAudition(playingId);
        return;
    }
    it->stopFadeMs = previewFadeMs(fadeMs);
    it->stopAttempts = 0;
    it->stopState = AuditionStopState::Pending;
    if (it->mode == AuditionMode::Preview) {
        it->previewStopPending = false;
        it->previewStopStuck = false;
        it->previewStopDeadlineMs = 0;
        if (it->previewCancelling) {
            it->stopState = AuditionStopState::Stopping;
            emit auditionStateChanged(playingId, it->state);
            return;
        }
    }
    tryStopAudition(playingId);
}

void AuditionManager::tryStopAudition(uint32_t playingId)
{
    auto it = auditions_.find(playingId);
    if (it == auditions_.end() || it->stopState != AuditionStopState::Pending)
        return;
    ++it->stopAttempts;
    auto result = Bridge::kEngineNotInit;
    if (engine_) {
        if (it->mode == AuditionMode::Preview) {
            const int32_t stopped = engine_->stopPreviewPlayable(
                it->playbackHandle, it->stopFadeMs);
            result = stopped == Bridge::kPreviewStaleHandle
                    || stopped == Bridge::kPreviewUnknownHandle
                ? Bridge::kEngineStaleHandle : previewEngineResult(stopped);
        } else {
            result = engine_->stopPlayback(it->playbackHandle, it->stopFadeMs);
        }
    }
    it->stopResult = result;
    if (result == Bridge::kEngineStaleHandle) {
        finishAudition(playingId);
        return;
    }
    if (result == Bridge::kEngineOk) {
        it->stopState = AuditionStopState::Stopping;
        it->bIsFading = it->stopFadeMs != 0;
        if (it->mode == AuditionMode::Preview)
            it->previewCancelling = true;
    } else if (result != Bridge::kEngineBackpressure
               || it->stopAttempts >= kAuditionStopAttemptLimit) {
        it->stopState = AuditionStopState::Failed;
        const QString objectName = it->objectName;
        emit auditionStopFailed(playingId, objectName, static_cast<int32_t>(result));
    }
    it = auditions_.find(playingId);
    if (it != auditions_.end())
        emit auditionStateChanged(playingId, it->state);
}

void AuditionManager::finishAudition(uint32_t playingId)
{
    if (!auditions_.remove(playingId))
        return;
    emit auditionStateChanged(playingId, 0);
    emitIfAllStopped();
}


void AuditionManager::stopAll(float fadeMs)
{
    QList<uint32_t> ids;
    ids.reserve(auditions_.size());
    for (auto it = auditions_.cbegin(); it != auditions_.cend(); ++it) {
        if (it->mode != AuditionMode::RawFile)
            ids.push_back(it.key());
    }
    for (uint32_t id : ids)
        stop(id, fadeMs);
}


bool AuditionManager::hasActiveAudition() const
{
    return !auditions_.isEmpty();
}

bool AuditionManager::hasActiveNonRawAudition() const
{
    for (auto it = auditions_.cbegin(); it != auditions_.cend(); ++it) {
        if (it->mode != AuditionMode::RawFile)
            return true;
    }
    return false;
}

void AuditionManager::togglePlayback(const QString& objectPath)
{
    if (hasActiveNonRawAudition()) {
        stopAll();
        return;
    }
    if (objectPath.isEmpty()) return;
    play(objectPath);
}


AuditionPosition AuditionManager::getPositionSamples(uint32_t playingId) const
{
    AuditionPosition pos;
    auto it = auditions_.find(playingId);
    if (it == auditions_.end() || !engine_)
        return pos;

    const double sampleRate = engine_ ? static_cast<double>(engine_->actualSampleRate()) : 48000.0;
    pos.timeSeconds    = static_cast<double>(it->positionMs) / 1000.0;
    pos.samplePosition = static_cast<int64_t>(pos.timeSeconds * sampleRate);

    return pos;
}

AuditionSeekStartResult
AuditionManager::setPositionSamples(uint32_t playingId, int64_t samplePos)
{
    auto it = auditions_.find(playingId);
    if (it == auditions_.end() || !engine_)
        return {0, HarkenEditor::Bridge::kEngineStaleHandle};
    if (it->mode == AuditionMode::Preview || it->mode == AuditionMode::RawFile)
        return {0, HarkenEditor::Bridge::kEngineCapabilityUnavailable};
    for (auto pending = pendingActionSeeks_.cbegin();
         pending != pendingActionSeeks_.cend(); ++pending) {
        if (pending->playingId == playingId)
            return {0, HarkenEditor::Bridge::kEngineInUse};
    }

    const double sampleRate = static_cast<double>(engine_->actualSampleRate());
    const double positionMs = static_cast<double>(samplePos) / sampleRate * 1000.0;
    const auto result = engine_->seekAction(
        it->playbackHandle, {}, positionMs, false);
    if (result.result == HarkenEditor::Bridge::kEngineOk) {
        const uint64_t requestId = allocateSeekRequestId();
        pendingActionSeeks_.insert(
            result.operationId,
            PendingActionSeek{playingId, requestId, positionMs, samplePos});
        return {requestId, result.result};
    }
    return {0, result.result};
}

uint64_t AuditionManager::allocateSeekRequestId()
{
    for (;;) {
        const uint64_t id = nextSeekRequestId_++;
        if (nextSeekRequestId_ == 0)
            nextSeekRequestId_ = 1;
        if (id != 0)
            return id;
    }
}

std::optional<AuditionSeekStatus> AuditionManager::seekStatus(uint64_t requestId) const
{
    const auto completed = completedActionSeeks_.constFind(requestId);
    if (completed != completedActionSeeks_.cend()) return *completed;
    for (const auto& pending : pendingActionSeeks_) {
        if (pending.requestId == requestId)
            return AuditionSeekStatus{pending.playingId, pending.samplePosition};
    }
    return std::nullopt;
}

void AuditionManager::finishActionSeek(const PendingActionSeek& pending,
                                       HarkenEditor::Bridge::EngineResult result, bool applied)
{
    completedActionSeeks_.insert(pending.requestId,
        AuditionSeekStatus{pending.playingId, pending.samplePosition, result, false, applied});
    while (completedActionSeeks_.size() > 128)
        completedActionSeeks_.erase(completedActionSeeks_.begin());
    emit auditionSeekFinished(pending.playingId, pending.requestId, static_cast<int32_t>(result), applied);
}


void AuditionManager::setPlaybackSpeed(uint32_t playingId, float speed)
{
    auto it = auditions_.find(playingId);
    if (it == auditions_.end()) return;

    speed = std::clamp(speed, 0.1f, 4.0f);
    it->playbackSpeed = speed;

    emit auditionSpeedChanged(playingId, speed);
}

float AuditionManager::getPlaybackSpeed(uint32_t playingId) const
{
    auto it = auditions_.find(playingId);
    if (it != auditions_.end()) return it->playbackSpeed;
    return 1.0f;
}


int32_t AuditionManager::resetPreviewContainer(harkenstudio::ObjectId runtimeStableId)
{
    if (!engine_ || harkenstudio::IsNull(runtimeStableId))
        return HarkenEditor::Bridge::kPreviewHostError;
    return engine_->resetPreviewPlayable(
        HarkenEditor::Bridge::kPreviewPlayableContainer, runtimeStableId);
}

bool AuditionManager::previewResetSucceeded(int32_t status)
{
    return status == HarkenEditor::Bridge::kPreviewOk;
}

bool AuditionManager::previewResetBackpressured(int32_t status)
{
    return status == HarkenEditor::Bridge::kPreviewBackpressure;
}


void AuditionManager::simulateState(const QString& groupName, const QString& stateName)
{
    simulatedStates_[groupName] = stateName;
    emit stateSimulated(groupName, stateName);
}

void AuditionManager::simulateSwitch(const QString& groupName, const QString& stateName,
                                       uint64_t gameObjectId)
{
    SwitchKey key{groupName, gameObjectId};
    simulatedSwitches_[key] = stateName;
    emit switchSimulated(groupName, stateName, gameObjectId);
}

void AuditionManager::resetAllSimulations()
{
    simulatedStates_.clear();
    simulatedSwitches_.clear();
}

QMap<QString, QString> AuditionManager::getCurrentSwitches(uint64_t gameObjectId) const
{
    QMap<QString, QString> result;
    for (auto it = simulatedSwitches_.begin(); it != simulatedSwitches_.end(); ++it) {
        if (gameObjectId == 0 || it.key().gameObjId == gameObjectId)
            result[it.key().group] = it.value();
    }
    return result;
}

AuditionSimulation AuditionManager::getCurrentSimulation() const
{
    AuditionSimulation sim;
    sim.stateValues = simulatedStates_;
    for (auto it = simulatedSwitches_.begin(); it != simulatedSwitches_.end(); ++it) {
        sim.switchValues[it.key().group] = it.value();
    }
    return sim;
}


QVector<AuditionInfo> AuditionManager::getActiveAuditions() const
{
    QVector<AuditionInfo> result;
    result.reserve(auditions_.size());

    for (const auto& info : auditions_)
        result.append(info);
    return result;
}

AuditionInfo AuditionManager::getAudition(uint32_t playingId) const
{
    auto it = auditions_.find(playingId);
    if (it != auditions_.end()) return *it;
    return AuditionInfo{};
}

uint32_t AuditionManager::getAuditionForObject(const QString& objectPath) const
{
    for (auto it = auditions_.begin(); it != auditions_.end(); ++it) {
        if (it->objectName == objectPath && it->state != 0)
            return it.key();
    }
    return 0;
}

void AuditionManager::closeAudition(uint32_t playingId)
{
    stop(playingId, 50.0f);
}


void AuditionManager::onEnginePlaybackStarted(quintptr handle, const QString& )
{
    if (handle == 0)
        return;
    for (auto it = auditions_.begin(); it != auditions_.end(); ++it) {
        if (it->mode == AuditionMode::NamedPlayback && it->playbackHandle == handle) {
            it->state = 1;
            emit auditionStateChanged(it.key(), 1);
            return;
        }
    }
}

void AuditionManager::onEnginePlaybackStopped(quintptr handle)
{
    for (auto it = auditions_.begin(); it != auditions_.end(); ++it) {
        if ((it->mode == AuditionMode::NamedPlayback || it->mode == AuditionMode::RawFile)
            && it->playbackHandle == handle) {
            finishAudition(it.key());
            return;
        }
    }
}

void AuditionManager::retryPendingPreviewStarts()
{
    const auto ids = auditions_.keys();
    for (uint32_t id : ids) {
        auto it = auditions_.find(id);
        if (it == auditions_.end() || !it->previewStartPending)
            continue;
        if (++it->previewStartAttempts > kPreviewStartRetryLimit) {
            removePreviewSlot(id, it->objectName, Bridge::kEngineBackpressure, true);
            continue;
        }
        startPreviewNow(*it);
    }
    const bool hasPending = std::any_of(auditions_.cbegin(), auditions_.cend(),
        [](const auto& info) { return info.previewStartPending; });
    if (!hasPending)
        previewStartRetryTimer_.stop();
}

void AuditionManager::onRuntimeSyncBarrierChanged()
{
    if (!engine_)
        return;
    const auto ids = auditions_.keys();
    for (uint32_t id : ids) {
        auto it = auditions_.find(id);
        if (it != auditions_.end() && (it->revisionPending || it->previewStartPending))
            startPreviewNow(*it);
    }
    const bool hasRevisionPending = std::any_of(auditions_.cbegin(), auditions_.cend(),
        [](const auto& info) { return info.revisionPending; });
    const bool hasBackpressurePending = std::any_of(auditions_.cbegin(), auditions_.cend(),
        [](const auto& info) { return info.previewStartPending; });
    if (!hasRevisionPending)
        previewRevisionTimeoutTimer_.stop();
    if (!hasBackpressurePending)
        previewStartRetryTimer_.stop();
}

void AuditionManager::expirePendingRevisionStarts()
{
    const qint64 now = monotonicMilliseconds();
    QList<QPair<uint32_t, QString>> expired;
    bool hasPending = false;
    for (auto it = auditions_.cbegin(); it != auditions_.cend(); ++it) {
        if (!it->revisionPending)
            continue;
        if (now >= it->revisionDeadlineMs)
            expired.push_back({it.key(), it->objectName});
        else
            hasPending = true;
    }
    for (const auto& item : expired) {
        auditions_.remove(item.first);
        emit auditionStartFailed(
            item.first, item.second,
            static_cast<int32_t>(HarkenEditor::Bridge::kEngineBackpressure));
        emit auditionStateChanged(item.first, 0);
    }
    if (!hasPending)
        previewRevisionTimeoutTimer_.stop();
    if (!expired.isEmpty())
        emitIfAllStopped();
}


void AuditionManager::pollAuditionStates()
{
    if (!engine_ || !engine_->isInitialized())
        return;

    pollPendingActionSeeks();

    const auto ids = auditions_.keys();
    for (uint32_t id : ids)
        tryStopAudition(id);
    if (auditions_.isEmpty())
        return;

    QList<uint32_t> completedPreviews;
    QList<uint32_t> retiredThenStart;
    const qint64 now = monotonicMilliseconds();
    for (auto it = auditions_.begin(); it != auditions_.end(); ++it) {
        if (it->mode != AuditionMode::Preview)
            continue;
        if (it->revisionPending)
            continue;
        if (it->previewStartPending)
            continue;
        const bool occupancyWait = it->previewStopPending
            || it->previewCancelling
            || it->previewReplacePending
            || it->previewStopStuck;
        if (it->previewStopPending && it->playbackHandle != 0) {
            const int32_t stopResult = engine_->stopPreviewPlayable(
                it->playbackHandle, it->previewStopFadeMs);
            if (stopResult == HarkenEditor::Bridge::kPreviewOk) {
                it->previewStopPending = false;
                it->previewCancelling = true;
                it->bIsFading = true;
            } else if (stopResult != HarkenEditor::Bridge::kPreviewBackpressure) {
                if (it->previewReplacePending && !it->previewStopStuck)
                    retiredThenStart.push_back(it.key());
                else
                    completedPreviews.push_back(it.key());
                continue;
            }
        }
        if (it->playbackHandle == 0) {
            if (it->previewReplacePending && !it->previewStopStuck)
                retiredThenStart.push_back(it.key());
            continue;
        }
        VNX::AudioEngine::PreviewPlayableState state;
        const int32_t result =
            engine_->queryPreviewPlayable(it->playbackHandle, state);
        applyPreviewQuery(*it, result, state);
        if (previewQueryRetired(
                result, state,
                it->previewStopPending || it->previewCancelling)) {
            if (it->previewReplacePending && !it->previewStopStuck)
                retiredThenStart.push_back(it.key());
            else
                completedPreviews.push_back(it.key());
            continue;
        }
        if (occupancyWait
            && it->previewStopDeadlineMs != 0
            && now >= it->previewStopDeadlineMs
            && !it->previewStopStuck) {
            markPreviewStopStuck(*it);
        }
    }
    for (uint32_t id : completedPreviews) {
        auditions_.remove(id);
        emit auditionStateChanged(id, 0);
    }
    for (uint32_t id : retiredThenStart) {
        auto it = auditions_.find(id);
        if (it != auditions_.end())
            startLatchedTypedPreview(*it);
    }
    if (!completedPreviews.isEmpty())
        emitIfAllStopped();
}

void AuditionManager::pollPendingActionSeeks()
{
    QList<uint64_t> completed;
    for (auto pending = pendingActionSeeks_.begin();
         pending != pendingActionSeeks_.end(); ++pending) {
        HarkenEditor::Bridge::ActionSeekInfo info;
        const auto queryResult = engine_->queryActionSeek(pending.key(), info);
        if (queryResult != HarkenEditor::Bridge::kEngineOk) {
            finishActionSeek(*pending, queryResult, false);
            completed.push_back(pending.key());
            continue;
        }
        if (info.state == HarkenEditor::Bridge::ActionSeekState::Accepted
            || (!info.nativeReleased
                && info.serviceResult == HarkenEditor::Bridge::kEngineOk)) {
            continue;
        }

        const bool applied =
            info.state == HarkenEditor::Bridge::ActionSeekState::Applied
            && info.result == HarkenEditor::Bridge::kEngineOk;
        auto audition = auditions_.find(pending->playingId);
        if (applied && audition != auditions_.end()) {
            audition->positionMs = static_cast<float>(
                pending->requestedPositionMs);
            emit auditionPositionChanged(
                pending->playingId,
                pending->requestedPositionMs / 1000.0);
        }
        const auto releaseResult = engine_->releaseActionSeek(pending.key());
        if (releaseResult != HarkenEditor::Bridge::kEngineOk
            && releaseResult != HarkenEditor::Bridge::kEngineStaleHandle
            && info.serviceResult == HarkenEditor::Bridge::kEngineOk) {
            continue;
        }
        finishActionSeek(*pending, info.result, applied);
        completed.push_back(pending.key());
    }
    for (uint64_t operationId : completed)
        pendingActionSeeks_.remove(operationId);
}


QJsonObject AuditionManager::previewIpcSnapshot(uint32_t playingId) const
{
    const AuditionInfo info = playingId != 0 ? getAudition(playingId)
                                            : AuditionInfo{};
    const bool have = playingId != 0 && info.playingId != 0;
    const bool pending = have
        ? (info.previewStartPending || info.revisionPending
           || info.previewReplacePending)
        : lastPlayPending_;
    const QString reason = have && !info.previewReason.isEmpty()
        ? info.previewReason : lastPlayReason_;
    const int32_t engineResult = have
        ? info.previewEngineResult
        : static_cast<int32_t>(lastPlayEngineResult_);
    const quintptr handle = have ? info.playbackHandle : 0;
    QJsonObject snapshot;
    snapshot.insert(
        QStringLiteral("engine_result"), static_cast<int>(engineResult));
    snapshot.insert(QStringLiteral("reason"), reason);
    snapshot.insert(QStringLiteral("pending"), pending);
    snapshot.insert(
        QStringLiteral("playbackHandle"), static_cast<qint64>(handle));
    snapshot.insert(
        QStringLiteral("lifecycle"),
        static_cast<int>(have ? info.previewLifecycle : 0));
    snapshot.insert(
        QStringLiteral("childLiveCount"),
        static_cast<int>(have ? info.childLiveCount : 0));
    snapshot.insert(
        QStringLiteral("childAudibleRetiringCount"),
        static_cast<int>(have ? info.childAudibleRetiringCount : 0));
    snapshot.insert(
        QStringLiteral("pendingFutureCount"),
        static_cast<int>(have ? info.pendingFutureCount : 0));
    snapshot.insert(
        QStringLiteral("state"), have ? info.state : 0);
    QString stopState = QStringLiteral("none");
    if (info.stopState == AuditionStopState::Pending)
        stopState = QStringLiteral("pending");
    else if (info.stopState == AuditionStopState::Stopping)
        stopState = QStringLiteral("stopping");
    else if (info.stopState == AuditionStopState::Failed)
        stopState = QStringLiteral("failed");
    snapshot.insert(QStringLiteral("stop_state"), stopState);
    snapshot.insert(QStringLiteral("stop_result"), static_cast<int32_t>(info.stopResult));
    return snapshot;
}

QJsonObject AuditionManager::stopIpcSnapshot(uint32_t playingId) const
{
    int pending = 0;
    int failed = 0;
    auto result = Bridge::kEngineOk;
    for (auto it = auditions_.cbegin(); it != auditions_.cend(); ++it) {
        if (playingId != 0 ? it.key() != playingId : it->mode == AuditionMode::RawFile)
            continue;
        if (it->stopState == AuditionStopState::Failed) {
            ++failed;
            result = it->stopResult;
        } else {
            ++pending;
        }
    }
    return {{"state", failed != 0 ? "stop_failed" : pending != 0 ? "stopping" : "stopped"},
            {"pending", pending != 0}, {"stopping_count", pending}, {"failed_count", failed},
            {"engine_result", static_cast<int32_t>(result)}};
}

void AuditionManager::emitIfAllStopped()
{
    if (auditions_.isEmpty())
        emit allAuditionsStopped();
}

}
