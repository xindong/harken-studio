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
        event.objectName.size() + event.actionName.size() + event.gameObjectName.size() + event.playbackDescription.size()) * sizeof(QChar);
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
    const auto finishKind = fact.contentKind == kMonitorContentSound
        ? kMonitorFactPlayableFinished : kMonitorFactPlayableRetired;
    if (fact.factKind != finishKind) return {};
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
    if (monitorFactHas(execution, kMonitorFactFieldActionOutcome) && activity.actionOutcome == 2) return kLogWarn;
    if ((monitorFactHas(execution, kMonitorFactFieldResult) && execution.result != kOk) ||
        (monitorFactHas(execution, kMonitorFactFieldTerminalKind) &&
         execution.terminalKind == kMonitorTerminalFailure) ||
        (monitorFactHas(execution, kMonitorFactFieldActionOutcome) && activity.actionOutcome == 3) ||
        (monitorFactHas(execution, kMonitorFactFieldActivityVariable) && activity.variableOutcome == 7) ||
        execution.factKind == kMonitorFactActionRejected || execution.factKind == kMonitorFactQueueFailed ||
        execution.factKind == kMonitorFactTransitionRejected) return kLogError;
    return kLogInfo;
}

DesignerEventType CapturedEvent::type() const
{
    const auto operationType = [](quint32 operation) {
        switch (operation) {
        case kMonitorOperationPostAction:
        case kMonitorOperationPostActionByName:
        case kMonitorOperationPostActionPinned:
        case kMonitorOperationPostActionResolved:
        case kMonitorOperationStopAction: return DesignerEventType::Action;
        case kMonitorOperationPlayPreview:
        case kMonitorOperationPlayRawPreview:
        case kMonitorOperationStopPreview:
        case kMonitorOperationStopVoice:
        case kMonitorOperationResetPreview: return DesignerEventType::Playable;
        case kMonitorOperationSetValue:
        case kMonitorOperationSetEmitterValue:
        case kMonitorOperationSetValues:
        case kMonitorOperationResetValues:
        case kMonitorOperationSetEmitterValues: return DesignerEventType::Variable;
        default: return DesignerEventType::None;
        }
    };
    if (!isExecution()) return detail.category == kMonitorEventCategoryOperation
        ? operationType(detail.code) : diagnosticType;
    switch (execution.factKind) {
    case kMonitorFactRequestSubmitted:
    case kMonitorFactRequestReturned:
    case kMonitorFactRequestTerminal: return operationType(execution.request.operation);
    case kMonitorFactStepExecuted: return DesignerEventType::Step;
    case kMonitorFactVariableResult: return DesignerEventType::Variable;
    default: break;
    }
    switch (execution.subjectNamespace) {
    case kMonitorSubjectAction: return DesignerEventType::Action;
    case kMonitorSubjectPlayable:
    case kMonitorSubjectTransition: return DesignerEventType::Playable;
    case kMonitorSubjectEmitter: return DesignerEventType::GameObject;
    default: return DesignerEventType::None;
    }
}

bool CapturedEvent::isDesignerEvent() const
{
    if (type() == DesignerEventType::None) return false;
    if (!playbackDescription.isEmpty()) return true;
    if (!isExecution()) return detail.category == kMonitorEventCategoryDiagnostic && detail.level >= kLogWarn;
    switch (execution.factKind) {
    case kMonitorFactRequestReturned:
        return monitorFactHas(execution, kMonitorFactFieldResult) && execution.result != kOk;
    case kMonitorFactActionTerminal:
        return header.recordVersion == 2 && monitorFactHas(execution, kMonitorFactFieldActionOutcome);
    case kMonitorFactActionAccepted:
    case kMonitorFactStepExecuted:
    case kMonitorFactVariableResult:
    case kMonitorFactEmitterCreated:
    case kMonitorFactEmitterDestroyed:
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
    if (isExecution() && execution.factKind == kMonitorFactStepExecuted &&
        monitorFactHas(execution, kMonitorFactFieldActivityStep)) return activity.stepTargetId;
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

    desc.requestedStreamKindMask = kMonitorStreamEvent | kMonitorStreamState;
    desc.requestedPermissionMask = kMonitorPermObserveEvents | kMonitorPermObserveState | kMonitorPermSymbols;
    desc.requestSymbols = 1;
    desc.consumerQueueBytes = 1048576;
    desc.requestedExecutionFactVersion = 2;
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
    hostNames_.clear();
    hostNameOrder_.clear();
    emitterIdentities_.clear();
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
    EmitterIdentities changedEmitters;
    auto sequences = sourceSequences_;
    const auto rememberEmitter = [&](const MonitorRecordHeader& header, const MonitorExecutionFactV1& fact, bool pinIdentity) {
        const EmitterKey key{header.engineMonitorInstanceId, fact.originEmitter};
        auto& identity = emitterIdentities_[key];
        if (identity.hostSession != fact.hostSession || identity.hostSequence != fact.hostLocalSequence ||
            identity.hostRevision != fact.originHostRevision) {
            identity = {fact.hostSession, fact.hostLocalSequence, fact.originHostRevision, fact.hostOwnership, {}};
        }
        identity.ownership = fact.hostOwnership;
        const auto name = hostNames_.find({header.engineMonitorInstanceId, fact.originEmitter,
            identity.hostSession, identity.hostSequence, identity.hostRevision});
        if (name != hostNames_.end()) identity.name = name->second;
        auto& firstIdentity = changedEmitters[key];
        if (pinIdentity && firstIdentity.hostRevision == 0) firstIdentity = identity;
    };
    for (const auto& record : batch.records) {
        if (record.unknownOptional || (record.header.recordKind != kMonitorRecordEventDetail &&
                                      record.header.recordKind != kMonitorRecordExecutionFact &&
                                      record.header.recordKind != kMonitorRecordIdentityFact &&
                                      record.header.recordKind != kMonitorRecordHostObjectSymbol)) continue;
        CapturedEvent event;
        event.header = record.header;
        auto& sequence = sequences[{event.header.engineMonitorInstanceId, event.header.producerId}];
        if (event.header.monitorSessionEpoch != epoch || event.header.sourceSequence <= sequence) {
            fail(tr("Invalid event identity or payload"));
            return;
        }
        if (record.header.recordKind == kMonitorRecordIdentityFact) {
            MonitorExecutionFactV1 identity{};
            if (!decodeIdentityFact(record, &identity)) {
                fail(tr("Invalid game object identity"));
                return;
            }
            sequence = event.header.sourceSequence;
            if (identity.subjectNamespace == kMonitorSubjectEmitter &&
                identity.factKind == kMonitorFactIdentityUpsert &&
                monitorFactHas(identity, kMonitorFactFieldOriginEmitter) &&
                monitorFactHas(identity, kMonitorFactFieldHostKey) &&
                monitorFactHas(identity, kMonitorFactFieldHostRevision) &&
                !emitterIdentities_.contains({event.header.engineMonitorInstanceId, identity.originEmitter})) {
                rememberEmitter(event.header, identity, false);
            }
            continue;
        }
        if (record.header.recordKind == kMonitorRecordHostObjectSymbol) {
            MonitorHostObjectSymbolV1 symbol{};
            if (!decodeHostObjectSymbol(record, &symbol)) {
                fail(tr("Invalid game object identity"));
                return;
            }
            sequence = event.header.sourceSequence;
            if (symbol.field == kMonitorHostSymbolName) {
                const HostSymbolKey key{event.header.engineMonitorInstanceId, event.header.instanceHandle,
                    symbol.hostSession, symbol.hostSequence, symbol.registrationRevision};
                if (!hostNames_.contains(key)) hostNameOrder_.push_back(key);
                hostNames_[key] = QString::fromUtf8(reinterpret_cast<const char*>(symbol.text), symbol.textBytes);
                auto& firstIdentity = changedEmitters[{event.header.engineMonitorInstanceId, event.header.instanceHandle}];
                if (firstIdentity.hostSession == symbol.hostSession && firstIdentity.hostSequence == symbol.hostSequence &&
                    firstIdentity.hostRevision == symbol.registrationRevision) firstIdentity.name = hostNames_[key];
                const auto emitter = emitterIdentities_.find({event.header.engineMonitorInstanceId, event.header.instanceHandle});
                if (emitter != emitterIdentities_.end() && emitter->second.hostSession == symbol.hostSession &&
                    emitter->second.hostSequence == symbol.hostSequence &&
                    emitter->second.hostRevision == symbol.registrationRevision) emitter->second.name = hostNames_[key];
                while (hostNameOrder_.size() > kMaxRows) {
                    hostNames_.erase(hostNameOrder_.front());
                    hostNameOrder_.pop_front();
                }
            }
            continue;
        }
        if (event.isExecution()) {
            if (!decodeExecutionFact(record, &event.execution, &event.activity)) {
                fail(tr("Invalid execution record"));
                return;
            }
            const auto& fact = event.execution;
            event.playbackDescription = playableDescription(event);
            if (monitorFactHas(fact, kMonitorFactFieldPlaybackRequest))
                event.playbackRequestSequence = fact.playbackRequest.sequence;
            else if (monitorFactHas(fact, kMonitorFactFieldRequest))
                event.playbackRequestSequence = fact.request.sequence;
            if (fact.subjectNamespace == kMonitorSubjectEmitter &&
                monitorFactHas(fact, kMonitorFactFieldOriginEmitter) &&
                monitorFactHas(fact, kMonitorFactFieldHostKey) &&
                monitorFactHas(fact, kMonitorFactFieldHostRevision)) {
                rememberEmitter(event.header, fact, true);
            }
            resolveGameObject(event);
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
            if (event.isExecution() && event.execution.factKind == kMonitorFactStepExecuted &&
                monitorFactHas(event.execution, kMonitorFactFieldContentObject)) {
                if (const auto* found = projectObjects_.find(event.header.stableId)) event.actionName = found->name;
            }
            if (const auto* found = projectObjects_.find(event.objectId())) {
                event.objectName = found->name;
                if (!event.isExecution() && event.detail.category == kMonitorEventCategoryDiagnostic) {
                    switch (found->businessKind) {
                    case ProjectObjectLocation::BusinessKind::Action: event.diagnosticType = DesignerEventType::Action; break;
                    case ProjectObjectLocation::BusinessKind::Playable: event.diagnosticType = DesignerEventType::Playable; break;
                    case ProjectObjectLocation::BusinessKind::Variable: event.diagnosticType = DesignerEventType::Variable; break;
                    default: break;
                    }
                }
            }
        }
        sequence = event.header.sourceSequence;
        incoming.push_back(event);
    }
    sourceSequences_ = std::move(sequences);
    for (auto& event : incoming) {
        const auto changed = changedEmitters.find({event.header.engineMonitorInstanceId, event.execution.originEmitter});
        if (changed != changedEmitters.end()) resolveGameObject(event, &changed->second);
    }
    refreshGameObjects(changedEmitters);
    for (const auto& event : incoming)
        if (event.isExecution() && event.execution.factKind == kMonitorFactEmitterDestroyed)
            emitterIdentities_.erase({event.header.engineMonitorInstanceId, event.execution.originEmitter});
    append(std::move(incoming));
}

void EventCaptureController::resolveGameObject(CapturedEvent& event, const EmitterIdentity* firstIdentity)
{
    if (!event.isExecution()) return;
    const auto& fact = event.execution;
    if (!monitorFactHas(fact, kMonitorFactFieldOriginEmitter)) return;
    if (event.emitterObjectId == 0) event.emitterObjectId = fact.originEmitter;
    auto ownership = fact.hostOwnership;
    if (event.hostRevision == 0) {
        if (monitorFactHas(fact, kMonitorFactFieldHostKey) && monitorFactHas(fact, kMonitorFactFieldHostRevision)) {
            event.hostSession = fact.hostSession;
            event.hostSequence = fact.hostLocalSequence;
            event.hostRevision = fact.originHostRevision;
        } else if (fact.factKind == kMonitorFactEmitterCreated || fact.factKind == kMonitorFactEmitterDestroyed) {
            const auto* identity = firstIdentity;
            if (!identity || identity->hostRevision == 0) {
                const auto found = emitterIdentities_.find({event.header.engineMonitorInstanceId, fact.originEmitter});
                if (found != emitterIdentities_.end()) identity = &found->second;
            }
            if (identity) {
                event.hostSession = identity->hostSession;
                event.hostSequence = identity->hostSequence;
                event.hostRevision = identity->hostRevision;
                ownership = identity->ownership;
            }
        }
    }
    if (event.hostRevision == 0) return;
    if (ownership == kMonitorHostStudioDefaultEmitter || event.emitterObjectId == kMonitorStudioDefaultGameObjectId) {
        event.emitterObjectId = kMonitorStudioDefaultGameObjectId;
        event.gameObjectName = QStringLiteral("Emitter");
        return;
    }
    const auto found = hostNames_.find({event.header.engineMonitorInstanceId, fact.originEmitter,
        event.hostSession, event.hostSequence, event.hostRevision});
    if (found != hostNames_.end()) {
        event.gameObjectName = found->second;
    } else if (firstIdentity && firstIdentity->hostSession == event.hostSession &&
        firstIdentity->hostSequence == event.hostSequence && firstIdentity->hostRevision == event.hostRevision) {
        event.gameObjectName = firstIdentity->name;
    } else {
        const auto emitter = emitterIdentities_.find({event.header.engineMonitorInstanceId, fact.originEmitter});
        if (emitter != emitterIdentities_.end() && emitter->second.hostSession == event.hostSession &&
            emitter->second.hostSequence == event.hostSequence && emitter->second.hostRevision == event.hostRevision)
            event.gameObjectName = emitter->second.name;
    }
}

void EventCaptureController::refreshGameObjects(const EmitterIdentities& changedEmitters)
{
    if (changedEmitters.empty()) return;
    int first = -1;
    int last = -1;
    for (int row = 0; row < rowCount(); ++row) {
        auto& event = records_[static_cast<std::size_t>(row)];
        const auto changed = changedEmitters.find({event.header.engineMonitorInstanceId, event.execution.originEmitter});
        if (changed == changedEmitters.end()) continue;
        const auto previousName = event.gameObjectName;
        const auto previousId = event.emitterObjectId;
        resolveGameObject(event, &changed->second);
        if (previousName == event.gameObjectName && previousId == event.emitterObjectId) continue;
        retainedBytes_ -= previousName.size() * sizeof(QChar);
        retainedBytes_ += event.gameObjectName.size() * sizeof(QChar);
        if (first < 0) first = row;
        last = row;
    }
    if (first >= 0) emit recordsUpdated(first, last);
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
