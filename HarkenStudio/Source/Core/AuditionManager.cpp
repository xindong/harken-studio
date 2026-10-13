
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
        case VNX::ProjectItem::Type::Piece:
            return HarkenEditor::Bridge::kPreviewPlayablePiece;
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

bool actionInstallFailedOrUnavailable(
    const VNX::RuntimeSyncCoordinator& coordinator)
{
    using State = VNX::RuntimeSyncCoordinator::RevisionBarrierState;
    const auto barrier = coordinator.barrier(
        coordinator.currentAuthoringRevision());
    return barrier.state == State::Failed
        || barrier.state == State::RuntimeUnavailable;
}

bool actionInstallInFlight(const VNX::RuntimeSyncCoordinator& coordinator)
{
    if (coordinator.hasOutstandingSubmission())
        return true;
    const auto installed = coordinator.installedWorld();
    if (!installed)
        return false;
    return installed->authoringRevision < coordinator.currentAuthoringRevision();
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

    if (engine_) {
        connect(engine_, &VNX::AudioEngine::playbackStarted,
                this, &AuditionManager::onEnginePlaybackStarted);
        connect(engine_, &VNX::AudioEngine::playbackStopped,
                this, &AuditionManager::onEnginePlaybackStopped);
        connect(&engine_->runtimeSyncCoordinator(),
                &VNX::RuntimeSyncCoordinator::barrierChanged,
                this, &AuditionManager::onRuntimeSyncBarrierChanged);
        connect(engine_, &VNX::AudioEngine::initializedChanged,
                 this, [this](bool initialized) {
            if (initialized)
                return;
            for (auto pending = pendingActionSeeks_.cbegin();
                 pending != pendingActionSeeks_.cend(); ++pending) {
                emit auditionSeekFinished(
                    pending->playingId, pending->requestId,
                    static_cast<int32_t>(
                        HarkenEditor::Bridge::kEngineNotInit),
                    false);
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
        });
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
    uint32_t playingId, const QString& objectName, int32_t engineResult,
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

bool AuditionManager::startTypedPreviewNow(AuditionInfo& info)
{
    const uint32_t playingId = info.playingId;
    const QString objectName = info.objectName;
    auto& coordinator = engine_->runtimeSyncCoordinator();
    info.requiredRevision = coordinator.currentAuthoringRevision();
    const auto installed = coordinator.installedWorld();
    info.previewReplacePending = false;
    info.previewStopPending = false;
    info.previewCancelling = false;
    info.previewStopStuck = false;
    info.previewStopDeadlineMs = 0;

    if (!coordinator.hasInstalledWorld() || !installed) {
        failImmediatePlay(QStringLiteral("unknown"),
                          HarkenEditor::Bridge::kEngineNotInit);
        removePreviewSlot(
            playingId, objectName,
            static_cast<int32_t>(HarkenEditor::Bridge::kEngineNotInit), true);
        return false;
    }
    info.projectSessionId = installed->projectSessionId;
    info.runtimeEpoch = installed->runtimeEpoch;

    const auto preview = engine_->playPreviewPlayable(
        info.previewPlayableKind, info.runtimeStableId);
    if (preview.status == HarkenEditor::Bridge::kPreviewBackpressure) {
        info.previewStartPending = true;
        info.revisionPending = false;
        info.playbackHandle = 0;
        info.previewReason = QStringLiteral("admission");
        info.previewEngineResult =
            static_cast<int32_t>(HarkenEditor::Bridge::kEngineBackpressure);
        info.state = 0;
        info.bIsPlaying = false;
        info.bIsDelayed = true;
        lastPlayFailure_ = HarkenEditor::Bridge::PlaybackFailure::None;
        lastPlayEngineResult_ = HarkenEditor::Bridge::kEngineBackpressure;
        lastPlayReason_ = QStringLiteral("admission");
        lastPlayPending_ = true;
        if (!previewStartRetryTimer_.isActive())
            previewStartRetryTimer_.start();
        return true;
    }
    if (preview.status != HarkenEditor::Bridge::kPreviewOk || preview.handle == 0) {
        const auto result = previewEngineResult(preview.status);
        failImmediatePlay(QStringLiteral("unknown"), result);
        removePreviewSlot(
            playingId, objectName, static_cast<int32_t>(result), true);
        return false;
    }

    info.playbackHandle = preview.handle;
    info.previewStartPending = false;
    info.revisionPending = false;
    info.previewReason.clear();
    info.previewEngineResult = 0;
    info.state = 1;
    info.bIsPlaying = true;
    info.bIsDelayed = false;
    lastPlayFailure_ = HarkenEditor::Bridge::PlaybackFailure::None;
    lastPlayEngineResult_ = HarkenEditor::Bridge::kEngineOk;
    lastPlayReason_.clear();
    lastPlayPending_ = false;
    idleZeroPolls_ = 0;
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
    startTypedPreviewNow(info);
}

void AuditionManager::requestTypedPreviewStop(AuditionInfo& info)
{
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

uint32_t AuditionManager::playTypedPreview(
    const QString& objectName, const VNX::ProjectItem& item,
    uint64_t gameObjectId, uint32_t playableKind)
{
    if (auditions_.size() >= MAX_AUDITIONS) {
        failImmediatePlay(QStringLiteral("capacity"),
                          HarkenEditor::Bridge::kEngineInUse);
        qWarning("[AuditionManager] Max auditions (%d) reached, cannot play '%s'",
                 MAX_AUDITIONS, qPrintable(objectName));
        return 0;
    }

    AuditionInfo info;
    info.playingId = nextPlayingId_++;
    info.objectName = objectName;
    info.mode = AuditionMode::Preview;
    info.previewPlayableKind = playableKind;
    info.runtimeStableId = item.runtimeId();
    info.gameObjId = gameObjectId;
    info.previewIntentGeneration = nextPreviewIntentGeneration_++;
    info.state = 0;
    info.bIsPlaying = false;
    info.bIsDelayed = true;
    auditions_[info.playingId] = info;
    idleZeroPolls_ = 0;
    auto it = auditions_.find(info.playingId);
    if (!startTypedPreviewNow(*it))
        return 0;
    return it->playingId;
}

uint32_t AuditionManager::playResolved(const QString& objectName,
                                        const VNX::ProjectItem* item,
                                        uint64_t gameObjectId)
{
    lastPlayFailure_ = HarkenEditor::Bridge::PlaybackFailure::None;
    lastPlayEngineResult_ = HarkenEditor::Bridge::kEngineOk;
    lastPlayTopologyFailure_ = {};
    lastPlayReason_.clear();
    lastPlayPending_ = false;
    if (!engine_ || !engine_->isInitialized())
    {
        failImmediatePlay(QStringLiteral("unknown"),
                          HarkenEditor::Bridge::kEngineNotInit);
        return 0;
    }
    if (!ensurePreviewOutput())
        return 0;

    const auto playableKind = item != nullptr
        ? previewKind(item->type) : std::nullopt;
    if (playableKind)
        return playTypedPreview(objectName, *item, gameObjectId, *playableKind);

    if (auditions_.size() >= MAX_AUDITIONS) {
        failImmediatePlay(QStringLiteral("capacity"),
                          HarkenEditor::Bridge::kEngineInUse);
        qWarning("[AuditionManager] Max auditions (%d) reached, cannot play '%s'",
                 MAX_AUDITIONS, qPrintable(objectName));
        return 0;
    }

    QString dispatchName = item != nullptr ? item->name : objectName;
    if (item == nullptr) {
        const int leafStart = objectName.lastIndexOf('/') + 1;
        if (leafStart > 0 && leafStart < objectName.size()) {
            dispatchName = objectName.mid(leafStart);
        }
    }
    quintptr handle = 0;
    AuditionMode mode = AuditionMode::NamedPlayback;
    bool previewStartPending = false;
    bool revisionPending = false;
    bool actionInstallPending = false;
    uint32_t previewPlayableKind = 0;
    harkenstudio::ObjectId runtimeStableId = {};
    uint64_t requiredRevision = 0;
    uint64_t projectSessionId = 0;
    uint64_t runtimeEpoch = 0;
    const bool actionItem = item != nullptr
        && item->type == VNX::ProjectItem::Type::Action;
    if (actionItem) {
        runtimeStableId = item->runtimeId();
        auto& coordinator = engine_->runtimeSyncCoordinator();
        requiredRevision = coordinator.currentAuthoringRevision();
        const auto installed = coordinator.installedWorld();
        if (installed) {
            projectSessionId = installed->projectSessionId;
            runtimeEpoch = installed->runtimeEpoch;
        }
        if (!coordinator.hasInstalledWorld()) {
            failImmediatePlay(QStringLiteral("unknown"),
                              HarkenEditor::Bridge::kEngineNotInit);
        } else if (!actionHasAuthoredPlayableStep(*item)) {
            failNotPlayable(QStringLiteral("empty"));
        } else if (engine_->hasActionProgram(runtimeStableId)) {
            const VNX::AudioEngine::PlaybackStartResult postResult =
                engine_->playByNameWithPreviewContext(dispatchName);
            if (postResult.previewVariablesPending) {
                previewStartPending = true;
                lastPlayReason_ = QStringLiteral("variables");
                lastPlayPending_ = true;
            } else if (postResult.playbackHandle == 0
                && postResult.engineResult
                    == HarkenEditor::Bridge::kEngineBackpressure
                && actionInstallInFlight(coordinator)) {
                actionInstallPending = true;
                lastPlayFailure_ = HarkenEditor::Bridge::PlaybackFailure::None;
                lastPlayEngineResult_ = HarkenEditor::Bridge::kEngineOk;
                lastPlayTopologyFailure_ = {};
                lastPlayReason_ = QStringLiteral("install");
                lastPlayPending_ = true;
            } else {
                handle = postResult.playbackHandle;
                lastPlayFailure_ = postResult.failure;
                lastPlayEngineResult_ = postResult.engineResult;
                lastPlayTopologyFailure_ = postResult.topologyFailure;
            }
        } else if (actionInstallFailedOrUnavailable(coordinator)) {
            failImmediatePlay(QStringLiteral("unknown"),
                              HarkenEditor::Bridge::kEngineNotInit);
        } else if (actionInstallInFlight(coordinator)) {
            actionInstallPending = true;
            lastPlayReason_ = QStringLiteral("install");
            lastPlayPending_ = true;
        } else {
            failNotPlayable(QStringLiteral("unavailable"));
        }
    } else {
        const VNX::AudioEngine::PlaybackStartResult postResult =
            engine_->playByNameDetailed(dispatchName);
        handle = postResult.playbackHandle;
        lastPlayFailure_ = postResult.failure;
        lastPlayEngineResult_ = postResult.engineResult;
        lastPlayTopologyFailure_ = postResult.topologyFailure;
    }
    if (handle == 0 && !previewStartPending && !revisionPending
        && !actionInstallPending)
    {
        if (lastPlayReason_.isEmpty())
            lastPlayReason_ = QStringLiteral("unknown");
        return 0;
    }

    AuditionInfo info;
    info.playingId = nextPlayingId_++;
    info.objectName = objectName;
    info.playbackHandle = handle;
    info.mode = mode;
    info.previewStartPending = previewStartPending;
    info.revisionPending = revisionPending;
    info.actionInstallPending = actionInstallPending;
    info.previewPlayableKind = previewPlayableKind;
    info.runtimeStableId = runtimeStableId;
    info.requiredRevision = requiredRevision;
    info.projectSessionId = projectSessionId;
    info.runtimeEpoch = runtimeEpoch;
    info.previewIntentGeneration = nextPreviewIntentGeneration_++;
    if (!actionInstallPending) {
        info.revisionDeadlineMs = monotonicMilliseconds()
            + kPreviewRevisionTimeoutMs;
    }
    info.gameObjId = gameObjectId;
    info.state = (previewStartPending || revisionPending
                  || actionInstallPending) ? 0 : 1;
    info.bIsPlaying = info.state == 1;
    info.bIsDelayed = previewStartPending || revisionPending
        || actionInstallPending;
    info.previewReason = lastPlayReason_;
    info.previewEngineResult = static_cast<int32_t>(lastPlayEngineResult_);
    auditions_[info.playingId] = info;
    idleZeroPolls_ = 0;

    if (previewStartPending) {
        if (!previewStartRetryTimer_.isActive())
            previewStartRetryTimer_.start();
    } else if (revisionPending) {
        if (!previewRevisionTimeoutTimer_.isActive())
            previewRevisionTimeoutTimer_.start();
    } else if (!actionInstallPending) {
        emit auditionStarted(info.playingId, objectName);
    }
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
    idleZeroPolls_ = 0;

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
    idleZeroPolls_ = 0;

    emit auditionStarted(info.playingId, objectName);
    return info.playingId;
}

void AuditionManager::stop(uint32_t playingId, float fadeMs)
{
    auto it = auditions_.find(playingId);
    if (it == auditions_.end()) return;

    if ((it->previewStartPending || it->revisionPending
         || it->actionInstallPending)
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

    if (it->mode == AuditionMode::Preview && engine_) {
        it->previewReplacePending = false;
        if (handle == 0) {
            auditions_.erase(it);
            emit auditionStateChanged(playingId, 0);
            emitIfAllStopped();
            return;
        }
        it->previewStopFadeMs = previewFadeMs(fadeMs);
        const int32_t result = engine_->stopPreviewPlayable(
            handle, it->previewStopFadeMs);
        VNX::AudioEngine::PreviewPlayableState state;
        const int32_t queryResult =
            engine_->queryPreviewPlayable(handle, state);
        applyPreviewQuery(*it, queryResult, state);
        if (previewQueryRetired(queryResult, state, true)
            || (result != HarkenEditor::Bridge::kPreviewOk && result != HarkenEditor::Bridge::kPreviewBackpressure)) {
            auditions_.erase(it);
            emit auditionStateChanged(playingId, 0);
            emitIfAllStopped();
            return;
        }
        if (it->previewStopDeadlineMs == 0) {
            it->previewStopDeadlineMs =
                monotonicMilliseconds() + kPreviewStopStuckMs;
        }
        it->bIsPlaying = false;
        it->state = 0;
        it->previewReason = QStringLiteral("occupancy");
        if (result == HarkenEditor::Bridge::kPreviewBackpressure
            && previewQueryActive(queryResult, state)) {
            it->previewStopPending = true;
            it->previewCancelling = false;
            it->bIsFading = it->previewStopFadeMs != 0;
            emit auditionStateChanged(playingId, it->state);
            return;
        }
        it->previewStopPending = false;
        it->previewCancelling = true;
        it->bIsFading = true;
        emit auditionStateChanged(playingId, it->state);
        return;
    }

    if (fadeMs > 0.0f && handle != 0) {
        it->bIsFading = true;
        emit auditionStateChanged(playingId, it->state);
        if (engine_)
            engine_->stopPlayback(handle, fadeMs);
        return;
    }

    if (engine_ && handle != 0)
        engine_->stopPlayback(handle, fadeMs);

    it = auditions_.find(playingId);
    if (it == auditions_.end())
        return;

    auditions_.erase(it);
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
            PendingActionSeek{playingId, requestId, positionMs});
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

    QMap<QString, int> rowByObject;
    for (const auto& t : auditions_) {
        const QString key = t.objectName;
        auto rowIt = rowByObject.find(key);
        if (rowIt == rowByObject.end()) {
            rowByObject.insert(key, result.size());
            result.append(t);
            continue;
        }

        AuditionInfo& aggregate = result[*rowIt];
        aggregate.bIsPlaying = aggregate.bIsPlaying || t.bIsPlaying;
        aggregate.bIsDelayed = aggregate.bIsDelayed || t.bIsDelayed;
        aggregate.bIsFading  = aggregate.bIsFading  || t.bIsFading;

        if (t.state == 1) {
            aggregate.state = 1;
        } else if (aggregate.state != 1 && t.state == 2) {
            aggregate.state = 2;
        }
    }
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
            uint32_t id = it.key();
            auditions_.erase(it);
            emit auditionStateChanged(id, 0);
            emitIfAllStopped();
            return;
        }
    }
}

void AuditionManager::retryPendingPreviewStarts()
{
    if (!engine_ || !engine_->isInitialized()) {
        QList<QPair<uint32_t, QString>> failed;
        for (auto it = auditions_.cbegin(); it != auditions_.cend(); ++it) {
            if (it->previewStartPending)
                failed.push_back({it.key(), it->objectName});
        }
        for (const auto& failure : failed) {
            auditions_.remove(failure.first);
            emit auditionStartFailed(
                failure.first, failure.second,
                static_cast<int32_t>(HarkenEditor::Bridge::kEngineNotInit));
            emit auditionStateChanged(failure.first, 0);
        }
        previewStartRetryTimer_.stop();
        if (!failed.isEmpty())
            emitIfAllStopped();
        return;
    }

    struct FailedStart {
        uint32_t playingId = 0;
        QString objectName;
        int32_t engineResult = 0;
    };
    QList<FailedStart> failed;
    QList<QPair<uint32_t, QString>> started;
    bool hasPending = false;

    for (auto it = auditions_.begin(); it != auditions_.end(); ++it) {
        if (!it->previewStartPending)
            continue;

        if (++it->previewStartAttempts > kPreviewStartRetryLimit) {
            failed.push_back({it.key(), it->objectName,
                              static_cast<int32_t>(
                                  HarkenEditor::Bridge::kEngineBackpressure)});
            continue;
        }

        quint64 handle = 0;
        bool pending = false;
        auto result = HarkenEditor::Bridge::kEngineOk;
        if (it->mode == AuditionMode::NamedPlayback) {
            const auto installed = engine_->runtimeSyncCoordinator().installedWorld();
            if (!installed || installed->projectSessionId != it->projectSessionId
                || installed->runtimeEpoch != it->runtimeEpoch) {
                failed.push_back({it.key(), it->objectName,
                                  static_cast<int32_t>(HarkenEditor::Bridge::kEngineNotInit)});
                continue;
            }
            const auto* action = VNX::ProjectManager::instance()
                .findItemByRuntimeStableId(it->runtimeStableId);
            if (action == nullptr || action->type != VNX::ProjectItem::Type::Action) {
                failed.push_back({it.key(), it->objectName,
                                  static_cast<int32_t>(HarkenEditor::Bridge::kEngineUnknownId)});
                continue;
            }
            const auto post = engine_->playByNameWithPreviewContext(action->name);
            if (!post.previewVariablesPending && post.playbackHandle == 0
                && post.engineResult == HarkenEditor::Bridge::kEngineBackpressure
                && actionInstallInFlight(engine_->runtimeSyncCoordinator())) {
                it->previewStartPending = false;
                it->actionInstallPending = true;
                it->requiredRevision = engine_->runtimeSyncCoordinator()
                    .currentAuthoringRevision();
                it->previewReason = QStringLiteral("install");
                continue;
            }
            handle = post.playbackHandle;
            pending = post.previewVariablesPending;
            result = post.engineResult;
        } else {
            const auto preview = engine_->playPreviewPlayable(
                it->previewPlayableKind, it->runtimeStableId);
            handle = preview.handle;
            pending = preview.status == HarkenEditor::Bridge::kPreviewBackpressure;
            result = previewEngineResult(preview.status);
        }
        if (pending) {
            it->previewReason = it->mode == AuditionMode::NamedPlayback
                ? QStringLiteral("variables") : QStringLiteral("admission");
            it->previewEngineResult =
                static_cast<int32_t>(HarkenEditor::Bridge::kEngineBackpressure);
            it->state = 0;
            it->bIsPlaying = false;
            it->bIsDelayed = true;
            hasPending = true;
            continue;
        }
        if (result != HarkenEditor::Bridge::kEngineOk || handle == 0) {
            failed.push_back({
                it.key(), it->objectName,
                static_cast<int32_t>(result)});
            continue;
        }

        it->playbackHandle = handle;
        it->previewStartPending = false;
        it->previewReason.clear();
        it->previewEngineResult = 0;
        it->bIsDelayed = false;
        it->bIsPlaying = true;
        it->state = 1;
        idleZeroPolls_ = 0;
        started.push_back({it.key(), it->objectName});
    }

    for (const FailedStart& failure : failed) {
        auditions_.remove(failure.playingId);
        emit auditionStartFailed(failure.playingId, failure.objectName,
                                  failure.engineResult);
        emit auditionStateChanged(failure.playingId, 0);
    }
    for (const auto& start : started) {
        emit auditionStarted(start.first, start.second);
        emit auditionStateChanged(start.first, 1);
    }

    if (!hasPending)
        previewStartRetryTimer_.stop();
    if (!failed.isEmpty())
        emitIfAllStopped();
}

void AuditionManager::onRuntimeSyncBarrierChanged()
{
    if (!engine_)
        return;

    struct FailedStart {
        uint32_t playingId;
        QString objectName;
        int32_t engineResult;
    };
    QList<FailedStart> failed;
    QList<QPair<uint32_t, QString>> started;
    bool hasRevisionPending = false;
    bool hasBackpressurePending = false;
    auto& coordinator = engine_->runtimeSyncCoordinator();
    for (auto it = auditions_.begin(); it != auditions_.end(); ++it) {
        if (it->previewStartPending) {
            const auto installed = coordinator.installedWorld();
            if (!coordinator.hasInstalledWorld() || !installed
                || installed->projectSessionId != it->projectSessionId
                || installed->runtimeEpoch != it->runtimeEpoch) {
                failed.push_back({it.key(), it->objectName,
                                  static_cast<int32_t>(
                                      HarkenEditor::Bridge::kEngineNotInit)});
                continue;
            }
            hasBackpressurePending = true;
            continue;
        }
        if (it->actionInstallPending) {
            const auto installed = coordinator.installedWorld();
            if (!coordinator.hasInstalledWorld() || !installed
                || installed->projectSessionId != it->projectSessionId
                || installed->runtimeEpoch != it->runtimeEpoch) {
                failed.push_back({it.key(), it->objectName,
                                  static_cast<int32_t>(
                                      HarkenEditor::Bridge::kEngineNotInit)});
                continue;
            }
            if (actionInstallFailedOrUnavailable(coordinator)) {
                failed.push_back({it.key(), it->objectName,
                                  static_cast<int32_t>(
                                      HarkenEditor::Bridge::kEngineNotInit)});
                continue;
            }
            if (actionInstallInFlight(coordinator)) {
                it->requiredRevision =
                    coordinator.currentAuthoringRevision();
                continue;
            }
            it->actionInstallPending = false;
            if (!engine_->hasActionProgram(it->runtimeStableId)) {
                failed.push_back({it.key(), it->objectName, 0});
                continue;
            }
            VNX::ProjectItem* action = VNX::ProjectManager::instance()
                .findItemByRuntimeStableId(it->runtimeStableId);
            if (action == nullptr
                || action->type != VNX::ProjectItem::Type::Action) {
                failed.push_back({it.key(), it->objectName, 0});
                continue;
            }
            const VNX::AudioEngine::PlaybackStartResult postResult =
                engine_->playByNameWithPreviewContext(action->name);
            if (postResult.previewVariablesPending) {
                it->previewStartPending = true;
                it->previewReason = QStringLiteral("variables");
                hasBackpressurePending = true;
                continue;
            }
            if (postResult.playbackHandle == 0) {
                const int32_t reported =
                    postResult.engineResult
                        == HarkenEditor::Bridge::kEngineUnknownId
                    ? 0
                    : static_cast<int32_t>(postResult.engineResult);
                failed.push_back({it.key(), it->objectName, reported});
                continue;
            }
            it->playbackHandle = postResult.playbackHandle;
            it->projectSessionId = installed->projectSessionId;
            it->runtimeEpoch = installed->runtimeEpoch;
            it->bIsDelayed = false;
            it->bIsPlaying = true;
            it->state = 1;
            it->previewReason.clear();
            it->previewEngineResult = 0;
            idleZeroPolls_ = 0;
            started.push_back({it.key(), it->objectName});
            continue;
        }
        if (!it->revisionPending)
            continue;
        const auto barrier = coordinator.barrier(it->requiredRevision);
        if (barrier.projectSessionId != it->projectSessionId
            || barrier.runtimeEpoch != it->runtimeEpoch) {
            failed.push_back({it.key(), it->objectName,
                              static_cast<int32_t>(
                                  HarkenEditor::Bridge::kEngineNotInit)});
            continue;
        }
        if (barrier.state
            == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Pending) {
            it->requiredRevision =
                coordinator.currentAuthoringRevision();
            hasRevisionPending = true;
            continue;
        }
        if (barrier.state
            == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Superseded) {
            hasRevisionPending = true;
            continue;
        }
        if (barrier.state
            != VNX::RuntimeSyncCoordinator::RevisionBarrierState::Ready) {
            failed.push_back({it.key(), it->objectName,
                              static_cast<int32_t>(
                                  HarkenEditor::Bridge::kEngineNotInit)});
            continue;
        }

        if (it->mode == AuditionMode::NamedPlayback) {
            VNX::ProjectItem* action = VNX::ProjectManager::instance()
                .findItemByRuntimeStableId(it->runtimeStableId);
            if (action == nullptr
                || action->type != VNX::ProjectItem::Type::Action) {
                it->revisionPending = false;
                failed.push_back({it.key(), it->objectName,
                                  static_cast<int32_t>(
                                      HarkenEditor::Bridge::kEngineUnknownId)});
                continue;
            }
            const VNX::AudioEngine::PlaybackStartResult postResult =
                engine_->playByNameWithPreviewContext(action->name);
            it->revisionPending = false;
            if (postResult.previewVariablesPending) {
                it->previewStartPending = true;
                it->previewReason = QStringLiteral("variables");
                hasBackpressurePending = true;
                continue;
            }
            if (postResult.playbackHandle == 0) {
                failed.push_back({it.key(), it->objectName,
                                  static_cast<int32_t>(
                                      postResult.engineResult)});
                continue;
            }
            it->playbackHandle = postResult.playbackHandle;
            it->revisionPending = false;
            it->bIsDelayed = false;
            it->bIsPlaying = true;
            it->state = 1;
            it->previewReason.clear();
            idleZeroPolls_ = 0;
            started.push_back({it.key(), it->objectName});
            continue;
        }

        const auto preview = engine_->playPreviewPlayable(
            it->previewPlayableKind, it->runtimeStableId);
        it->revisionPending = false;
        if (preview.status == HarkenEditor::Bridge::kPreviewBackpressure) {
            it->previewStartPending = true;
            it->previewReason = QStringLiteral("admission");
            it->previewEngineResult =
                static_cast<int32_t>(HarkenEditor::Bridge::kEngineBackpressure);
            it->state = 0;
            it->bIsPlaying = false;
            it->bIsDelayed = true;
            ++it->previewStartAttempts;
            hasBackpressurePending = true;
            continue;
        }
        if (preview.status != HarkenEditor::Bridge::kPreviewOk || preview.handle == 0) {
            failed.push_back({
                it.key(), it->objectName,
                static_cast<int32_t>(previewEngineResult(preview.status))});
            continue;
        }
        it->playbackHandle = preview.handle;
        it->previewReason.clear();
        it->previewEngineResult = 0;
        it->bIsDelayed = false;
        it->bIsPlaying = true;
        it->state = 1;
        idleZeroPolls_ = 0;
        started.push_back({it.key(), it->objectName});
    }

    for (const FailedStart& failure : failed) {
        auditions_.remove(failure.playingId);
        emit auditionStartFailed(failure.playingId, failure.objectName,
                                  failure.engineResult);
        emit auditionStateChanged(failure.playingId, 0);
    }
    for (const auto& start : started) {
        emit auditionStarted(start.first, start.second);
        emit auditionStateChanged(start.first, 1);
    }
    if (hasBackpressurePending) {
        if (!previewStartRetryTimer_.isActive())
            previewStartRetryTimer_.start();
    } else {
        previewStartRetryTimer_.stop();
    }
    if (hasRevisionPending) {
        if (!previewRevisionTimeoutTimer_.isActive())
            previewRevisionTimeoutTimer_.start();
    } else {
        previewRevisionTimeoutTimer_.stop();
    }
    if (!failed.isEmpty())
        emitIfAllStopped();
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

    if (auditions_.isEmpty()) {
        idleZeroPolls_ = 0;
        return;
    }

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
    if (auditions_.isEmpty()) {
        idleZeroPolls_ = 0;
        emit allAuditionsStopped();
        return;
    }

    bool hasNamedPlayback = false;
    for (auto it = auditions_.cbegin(); it != auditions_.cend(); ++it) {
        if (it->mode == AuditionMode::NamedPlayback
            && !it->revisionPending
            && !it->actionInstallPending) {
            hasNamedPlayback = true;
            break;
        }
    }
    if (!hasNamedPlayback) {
        idleZeroPolls_ = 0;
        return;
    }

    if (engine_->activeVoiceCount() > 0) {
        idleZeroPolls_ = 0;
        return;
    }

    // Typed preview auditions were reconciled above. Named playback has no
    // exact root query, so retire its auditions only after engine silence is
    // observed twice to debounce momentary snapshot dips.
    if (++idleZeroPolls_ < 2)
        return;

    idleZeroPolls_ = 0;
    QList<uint32_t> ids;
    for (auto it = auditions_.cbegin(); it != auditions_.cend(); ++it) {
        if (it->mode == AuditionMode::NamedPlayback
            && !it->revisionPending
            && !it->actionInstallPending)
            ids.push_back(it.key());
    }
    for (uint32_t id : ids)
    {
        auditions_.remove(id);
        emit auditionStateChanged(id, 0);
    }
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
            emit auditionSeekFinished(
                pending->playingId, pending->requestId,
                static_cast<int32_t>(queryResult), false);
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
        emit auditionSeekFinished(
            pending->playingId, pending->requestId,
            static_cast<int32_t>(info.result), applied);
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
           || info.actionInstallPending || info.previewReplacePending)
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
    return snapshot;
}

void AuditionManager::emitIfAllStopped()
{
    if (auditions_.isEmpty())
        emit allAuditionsStopped();
}

}
