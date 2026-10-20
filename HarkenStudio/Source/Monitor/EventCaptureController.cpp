#include "EventCaptureController.h"
#include "Monitor/MonitorTypes.h"
#include "RuntimeBridge/MonitorBridge.h"

#include "MonitorDecoder.h"
#include "RuntimeBridge/RemoteConnection.h"

#include <algorithm>
#include <tuple>

namespace VNX::Monitor {
namespace {
using HarkenEditor::Bridge::RemoteConnectionState;
using HarkenEditor::Bridge::RemoteEventCaptureState;

bool before(const CapturedEvent& a, const CapturedEvent& b)
{
    return std::make_tuple(a.timestampNs(), a.header.producerId, a.header.sourceSequence) <
           std::make_tuple(b.timestampNs(), b.header.producerId, b.header.sourceSequence);
}

std::size_t eventBytes(const CapturedEvent& event)
{
    return sizeof(CapturedEvent) + static_cast<std::size_t>(
        event.objectName.size() + event.gameObjectName.size() + event.playbackDescription.size()) * sizeof(QChar);
}

QString playableDescription(const CapturedEvent& event)
{
    const auto& fact = event.execution;
    if (fact.subjectNamespace != kMonitorSubjectPlayable ||
        !monitorFactHas(fact, kMonitorFactFieldContentObject) || harkenstudio::IsNull(event.objectId())) return {};
    switch (fact.contentKind) {
    case kMonitorContentSound:
    case kMonitorContentContainer:
    case kMonitorContentTrack:
    case kMonitorContentPiece:
        break;
    default:
        return {};
    }
    const auto startKind = fact.contentKind == kMonitorContentSound
        ? kMonitorFactPlayableStarted : kMonitorFactPlayableActive;
    if (fact.factKind == startKind) return EventCaptureController::tr("Playing");
    if (fact.factKind != kMonitorFactPlayableRetired) return {};
    if (monitorFactHas(fact, kMonitorFactFieldTerminalKind)) {
        switch (fact.terminalKind) {
        case kMonitorTerminalRequestedStop: return EventCaptureController::tr("Stopped");
        case kMonitorTerminalParentCancellation:
        case kMonitorTerminalReplacementReset: return EventCaptureController::tr("Cancelled");
        case kMonitorTerminalFailure: return EventCaptureController::tr("Playback failed");
        default: break;
        }
    }
    return EventCaptureController::tr("Finished");
}
}

quint64 CapturedEvent::timestampNs() const
{
    return isExecution() ? execution.timestampNs : detail.timestampNs;
}

int CapturedEvent::level() const
{
    if (!isExecution()) return detail.level;
    if ((monitorFactHas(execution, kMonitorFactFieldResult) && execution.result != kOk) ||
        (monitorFactHas(execution, kMonitorFactFieldTerminalKind) &&
         execution.subjectNamespace == kMonitorSubjectPlayable && execution.terminalKind == kMonitorTerminalFailure) ||
        execution.factKind == kMonitorFactActionRejected || execution.factKind == kMonitorFactQueueFailed ||
        execution.factKind == kMonitorFactTransitionRejected) return kLogError;
    return kLogInfo;
}

int CapturedEvent::category() const
{
    if (!isExecution()) return detail.category;
    if (execution.factKind == kMonitorFactRequestSubmitted || execution.factKind == kMonitorFactRequestReturned ||
        execution.factKind == kMonitorFactRequestTerminal) return kMonitorEventCategoryOperation;
    return kMonitorEventCategoryLifecycle;
}

bool CapturedEvent::isDesignerEvent() const
{
    if (!playbackDescription.isEmpty()) return true;
    if (!isExecution()) {
        if (detail.category == kMonitorEventCategoryDiagnostic) return detail.level >= kLogWarn;
        if (detail.category != kMonitorEventCategoryOperation) return false;
        if (detail.result != kOk) return true;
        switch (detail.code) {
        case kMonitorOperationSetValue:
        case kMonitorOperationSetEmitterValue:
        case kMonitorOperationSetValues:
        case kMonitorOperationSetEmitterValues:
        case kMonitorOperationLoadPack:
        case kMonitorOperationUnloadPack:
            return true;
        default:
            return false;
        }
    }
    switch (execution.factKind) {
    case kMonitorFactRequestReturned:
        return monitorFactHas(execution, kMonitorFactFieldResult) && execution.result != kOk;
    case kMonitorFactActionAccepted:
    case kMonitorFactActionStopped:
    case kMonitorFactActionRejected:
    case kMonitorFactTransitionStarted:
    case kMonitorFactTransitionStopped:
    case kMonitorFactTransitionRejected:
    case kMonitorFactSourceApplied:
    case kMonitorFactSeekApplied:
        return true;
    default:
        return false;
    }
}

harkenstudio::ObjectId CapturedEvent::objectId() const
{
    if (!isExecution() || monitorFactHas(execution, kMonitorFactFieldContentObject)) return header.stableId;
    return monitorFactHas(execution, kMonitorFactFieldTargetObject) ? execution.target.objectId : harkenstudio::ObjectId{};
}

MonitorFactRequest CapturedEvent::request() const
{
    if (!isExecution()) return {};
    if (monitorFactHas(execution, kMonitorFactFieldPlaybackRequest)) return execution.playbackRequest;
    return monitorFactHas(execution, kMonitorFactFieldRequest) ? execution.request : MonitorFactRequest{};
}

EventCaptureController::EventCaptureController(HarkenEditor::Bridge::RemoteConnection* remote,
                                               QObject* parent)
    : QObject(parent), remote_(remote), pollBuffer_(65536)
{
    timer_.setInterval(50);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if ((state_ == EventCaptureState::Starting || state_ == EventCaptureState::Stopping) &&
            transitionTimer_.isValid() && transitionTimer_.elapsed() > 10000) {
            fail(tr("Capture did not finish the requested transition"));
            return;
        }
        pollLocal();
    });
    timer_.start();
    if (remote_) {
        connect(remote_, &HarkenEditor::Bridge::RemoteConnection::stateChanged,
                this, &EventCaptureController::remoteStateChanged);
        connect(remote_, &HarkenEditor::Bridge::RemoteConnection::eventCaptureChanged,
                this, &EventCaptureController::remoteCaptureChanged);
        connect(remote_, &HarkenEditor::Bridge::RemoteConnection::eventRecordsReceived,
                this, &EventCaptureController::consume);
    }
    refreshAvailability();
}

EventCaptureController::~EventCaptureController()
{
    closeLocal();
    if (target_ == EventCaptureTarget::Remote && remote_) remote_->stopEventCapture();
}

bool EventCaptureController::canStart() const
{
    if (state_ == EventCaptureState::Starting || state_ == EventCaptureState::Capturing ||
        state_ == EventCaptureState::Stopping) return false;
    if (remote_) {
        const auto connection = remote_->state();
        if (connection == RemoteConnectionState::Connected) {
            const auto capture = remote_->eventCaptureStatus().state;
            return remote_->canCaptureEvents() && capture != RemoteEventCaptureState::Starting &&
                capture != RemoteEventCaptureState::Capturing && capture != RemoteEventCaptureState::Stopping;
        }
        if (connection == RemoteConnectionState::Connecting || connection == RemoteConnectionState::Handshaking)
            return false;
    }
    return localAvailable_;
}

QString EventCaptureController::availableTargetText() const
{
    if (remote_ && remote_->state() == RemoteConnectionState::Connected)
        return tr("Remote: %1").arg(remote_->target().application);
    return tr("Local preview");
}

QString EventCaptureController::statusText() const
{
    switch (state_) {
    case EventCaptureState::Unavailable:
        if (remote_ && remote_->state() == RemoteConnectionState::Connected && !remote_->canCaptureEvents())
            return tr("The connected runtime does not support event capture");
        if (remote_ && (remote_->state() == RemoteConnectionState::Connecting ||
                        remote_->state() == RemoteConnectionState::Handshaking))
            return tr("Waiting for remote connection");
        return tr("No runtime available");
    case EventCaptureState::Idle: return tr("Ready");
    case EventCaptureState::Starting: return tr("Starting - %1").arg(pendingTargetText_);
    case EventCaptureState::Capturing: return tr("Capturing - %1").arg(targetText_);
    case EventCaptureState::Stopping: return tr("Stopping - %1").arg(targetText_);
    case EventCaptureState::Stopped:
        return stopReason_.isEmpty() ? tr("Stopped - %1").arg(targetText_) : stopReason_;
    case EventCaptureState::Failed: return error_;
    }
    return {};
}

void EventCaptureController::refreshAvailability()
{
    if (state_ == EventCaptureState::Idle || state_ == EventCaptureState::Unavailable)
        state_ = canStart() ? EventCaptureState::Idle : EventCaptureState::Unavailable;
    emit stateChanged();
}

void EventCaptureController::setLocalRuntimeAvailable(bool available)
{
    localAvailable_ = available;
    if (!available && localSession_ != 0) {
        fail(tr("Local runtime ended; capture is incomplete"));
    }
    refreshAvailability();
}

void EventCaptureController::start()
{
    if (!canStart()) return;
    target_ = remote_ && remote_->state() == RemoteConnectionState::Connected
        ? EventCaptureTarget::Remote : EventCaptureTarget::Local;
    pendingTargetText_ = availableTargetText();
    startedThisAttempt_ = false;
    error_.clear();
    stopReason_.clear();
    state_ = EventCaptureState::Starting;
    transitionTimer_.restart();
    emit stateChanged();
    if (target_ == EventCaptureTarget::Remote) {
        remote_->startEventCapture();
        return;
    }
    MonitorSessionDesc desc{};

    desc.requestedStreamKindMask = kMonitorStreamEvent;
    desc.requestedPermissionMask = kMonitorPermObserveEvents;
    desc.consumerQueueBytes = 1048576;
    desc.requestedExecutionFactVersion = 1;
    const auto opened = HarkenEditor::Bridge::Monitoring::openEventSession(&desc, &localSession_);
    MonitorEventSessionStatus status{};

    const auto queried = opened == kOk ? HarkenEditor::Bridge::Monitoring::getEventSessionStatus(localSession_, &status) : opened;
    if (queried != kOk) {
        fail(tr("Could not start local capture (%1)").arg(queried));
        return;
    }
    begin(status.monitorSessionEpoch, status.startedTimestampNs, status.identityUnavailableCount);
}

void EventCaptureController::begin(quint64 epoch, quint64 timestamp, quint64 identityUnavailable)
{
    clear();
    sessionEpoch_ = epoch;
    startedNs_ = timestamp;
    targetText_ = pendingTargetText_;
    sourceSequences_.clear();
    sourceDropped_ = deliveryDropped_ = transportDropped_ = trimmed_ = 0;
    identityUnavailable_ = identityUnavailable;
    interrupted_ = false;
    startedThisAttempt_ = true;
    state_ = EventCaptureState::Capturing;
    emit captureStarted();
    emit stateChanged();
}

void EventCaptureController::stop()
{
    if (!canStop()) return;
    state_ = EventCaptureState::Stopping;
    transitionTimer_.restart();
    emit stateChanged();
    if (target_ == EventCaptureTarget::Remote) remote_->stopEventCapture();
    else {
        const auto result = HarkenEditor::Bridge::Monitoring::sealEventSession(localSession_);
        if (result != kOk) fail(tr("Could not stop local capture (%1)").arg(result));
        else pollLocal();
    }
}

void EventCaptureController::closeLocal()
{
    if (localSession_ != 0) HarkenEditor::Bridge::Monitoring::closeSession(localSession_);
    localSession_ = 0;
}

void EventCaptureController::fail(const QString& reason)
{
    if (startedThisAttempt_) interrupted_ = true;
    closeLocal();
    if (target_ == EventCaptureTarget::Remote && remote_ &&
        remote_->eventCaptureStatus().state == RemoteEventCaptureState::Capturing)
        remote_->stopEventCapture();
    state_ = EventCaptureState::Failed;
    error_ = reason;
    emit stateChanged();
}

void EventCaptureController::pollLocal()
{
    if (localSession_ == 0 || (state_ != EventCaptureState::Capturing && state_ != EventCaptureState::Stopping))
        return;
    MonitorPollDesc desc{};

    desc.buffer = pollBuffer_.data();
    desc.bufferBytes = pollBuffer_.size();
    MonitorPollResult result{};

    const auto polled = HarkenEditor::Bridge::Monitoring::poll(localSession_, &desc, &result);
    if (polled != kOk) {
        fail(tr("Local capture read failed (%1)").arg(polled));
        return;
    }
    if (result.bytesWritten != 0) consume(
        QByteArray::fromRawData(reinterpret_cast<const char*>(pollBuffer_.data()),
                               static_cast<qsizetype>(result.bytesWritten)),
        result.recordsWritten, sessionEpoch_);
    if (localSession_ == 0) return;
    MonitorEventSessionStatus status{};

    const auto queried = HarkenEditor::Bridge::Monitoring::getEventSessionStatus(localSession_, &status);
    if (queried != kOk) {
        fail(tr("Local capture status failed (%1)").arg(queried));
        return;
    }
    sourceDropped_ = status.sourceDroppedRecords;
    deliveryDropped_ = status.deliveryDroppedRecords;
    identityUnavailable_ = status.identityUnavailableCount;
    if (status.state == kMonitorEventSessionDrained) {
        closeLocal();
        state_ = EventCaptureState::Stopped;
    }
    emit stateChanged();
}

void EventCaptureController::remoteStateChanged()
{
    if (target_ == EventCaptureTarget::Local && state_ == EventCaptureState::Capturing &&
        remote_->state() == RemoteConnectionState::Connected) {
        stopReason_ = tr("Target changed; capture stopped");
        stop();
    }
    refreshAvailability();
}

void EventCaptureController::remoteCaptureChanged()
{
    if (target_ != EventCaptureTarget::Remote ||
        (state_ != EventCaptureState::Starting && state_ != EventCaptureState::Capturing &&
         state_ != EventCaptureState::Stopping)) return;
    const auto current = remote_->eventCaptureStatus();
    if (startedThisAttempt_ && current.sessionEpoch == sessionEpoch_) {
        sourceDropped_ = current.sourceDroppedRecords;
        deliveryDropped_ = current.deliveryDroppedRecords;
        transportDropped_ = current.transportDroppedRecords;
        identityUnavailable_ = current.identityUnavailableCount;
    }
    if (current.state == RemoteEventCaptureState::Failed) {
        fail(current.error);
        return;
    }
    if (state_ == EventCaptureState::Starting && current.state == RemoteEventCaptureState::Capturing)
        begin(current.sessionEpoch, current.startedTimestampNs, current.identityUnavailableCount);
    if (!startedThisAttempt_ || current.sessionEpoch != sessionEpoch_) return;
    sourceDropped_ = current.sourceDroppedRecords;
    deliveryDropped_ = current.deliveryDroppedRecords;
    transportDropped_ = current.transportDroppedRecords;
    identityUnavailable_ = current.identityUnavailableCount;
    if (current.state == RemoteEventCaptureState::Stopped) state_ = EventCaptureState::Stopped;
    emit stateChanged();
}

void EventCaptureController::clear()
{
    emit recordsAboutToReset();
    records_.clear();
    retainedBytes_ = 0;
    emit recordsReset();
    emit stateChanged();
}

void EventCaptureController::consume(const QByteArray& bytes, quint32 count, quint64 epoch)
{
    if (!startedThisAttempt_ || epoch != sessionEpoch_ ||
        (state_ != EventCaptureState::Capturing && state_ != EventCaptureState::Stopping)) return;
    const auto batch = decodePackedRecords(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                           bytes.size(), count);
    if (batch.status != kOk) {
        fail(tr("Invalid event record batch (%1)").arg(batch.status));
        return;
    }
    std::vector<CapturedEvent> incoming;
    auto sequences = sourceSequences_;
    for (const auto& record : batch.records) {
        if (record.unknownOptional || (record.header.recordKind != kMonitorRecordEventDetail &&
                                      record.header.recordKind != kMonitorRecordExecutionFact)) continue;
        CapturedEvent event;
        event.header = record.header;
        auto& sequence = sequences[{event.header.engineMonitorInstanceId, event.header.producerId}];
        if (event.header.monitorSessionEpoch != epoch || event.header.sourceSequence <= sequence) {
            fail(tr("Invalid event identity or payload"));
            return;
        }
        if (event.isExecution()) {
            if (!decodeExecutionFact(record, &event.execution)) {
                fail(tr("Invalid execution record"));
                return;
            }
            const auto& fact = event.execution;
            event.playbackDescription = playableDescription(event);
            if (monitorFactHas(fact, kMonitorFactFieldPlaybackRequest))
                event.playbackRequestSequence = fact.playbackRequest.sequence;
            if (monitorFactHas(fact, kMonitorFactFieldOriginEmitter))
                event.emitterObjectId = fact.originEmitter;
            if (monitorFactHas(fact, kMonitorFactFieldOriginEmitter) &&
                monitorFactHas(fact, kMonitorFactFieldHostKey) &&
                monitorFactHas(fact, kMonitorFactFieldHostRevision) &&
                monitorFactHas(fact, kMonitorFactFieldHostOwnership) &&
                fact.hostOwnership == kMonitorHostStudioDefaultEmitter) {
                event.emitterObjectId = kMonitorStudioDefaultGameObjectId;
                event.gameObjectName = QStringLiteral("Emitter");
            }
            if (fact.factKind == kMonitorFactRequestReturned &&
                monitorFactHas(fact, kMonitorFactFieldResult) && fact.result == kOk &&
                monitorFactHas(fact, kMonitorFactFieldRequestOperation)) {
                switch (fact.request.operation) {
                case kMonitorOperationStopAll: event.playbackDescription = tr("Stop all"); break;
                case kMonitorOperationStopEmitter: event.playbackDescription = tr("Stop emitter"); break;
                default: break;
                }
            }
            if (monitorFactHas(fact, kMonitorFactFieldContentObject)) {
                if (monitorFactHas(fact, kMonitorFactFieldProject)) event.projectGuid = authoringUuid(fact.projectId);
            } else if (monitorFactHas(fact, kMonitorFactFieldTargetProject)) {
                event.projectGuid = authoringUuid(fact.target.projectId);
            }
        } else {
            MonitorEventDetailV2 identity{};
            if (!decodeEventDetail(record, &event.detail, &identity)) {
                fail(tr("Invalid event identity or payload"));
                return;
            }
            event.projectGuid = QUuid::fromRfc4122(QByteArrayView(
                reinterpret_cast<const char*>(identity.projectGuid), sizeof(identity.projectGuid)));
        }
        event.objectGuid = authoringUuid(event.objectId());
        if (!event.projectGuid.isNull() && event.projectGuid == projectObjects_.projectGuid()) {
            if (const auto* found = projectObjects_.find(event.objectId())) {
                event.objectName = found->name;
            }
        }
        sequence = event.header.sourceSequence;
        incoming.push_back(event);
    }
    sourceSequences_ = std::move(sequences);
    append(std::move(incoming));
}

void EventCaptureController::append(std::vector<CapturedEvent> incoming)
{
    for (const auto& event : incoming) retainedBytes_ += eventBytes(event);
    std::sort(incoming.begin(), incoming.end(), before);
    auto cursor = incoming.begin();
    while (cursor != incoming.end()) {
        const auto position = std::lower_bound(records_.begin(), records_.end(), *cursor, before);
        const int first = static_cast<int>(position - records_.begin());
        auto end = position == records_.end() ? incoming.end()
            : std::lower_bound(cursor, incoming.end(), *position, before);
        if (end == cursor) ++end;
        const int count = static_cast<int>(end - cursor);
        emit recordsAboutToInsert(first, first + count - 1);
        records_.insert(position, cursor, end);
        emit recordsInserted();
        cursor = end;
    }
    int excess = 0;
    while (rowCount() - excess > kMaxRows || retainedBytes_ > kMaxBytes) {
        retainedBytes_ -= eventBytes(records_[static_cast<std::size_t>(excess)]);
        ++excess;
    }
    if (excess > 0) {
        emit recordsAboutToRemove(0, excess - 1);
        records_.erase(records_.begin(), records_.begin() + excess);
        trimmed_ += excess;
        emit recordsRemoved();
    }
}

void EventCaptureController::setProjectObjects(ProjectObjectIndex objects)
{
    projectObjects_ = std::move(objects);
    emit projectObjectsChanged();
}

quint32 EventCaptureController::matchingProjectObject(const CapturedEvent& event) const
{
    if (event.projectGuid.isNull() || event.projectGuid != projectObjects_.projectGuid() ||
        event.objectGuid.isNull() || runtimeObjectId(event.objectGuid) != event.objectId()) return 0;
    const auto* found = projectObjects_.find(event.objectId());
    return found ? found->ownerObjectId : 0;
}

}
