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
    return std::tie(a.detail.timestampNs, a.header.producerId, a.header.sourceSequence) <
           std::tie(b.detail.timestampNs, b.header.producerId, b.header.sourceSequence);
}

std::size_t eventBytes(const CapturedEvent& event)
{
    return sizeof(CapturedEvent) + static_cast<std::size_t>(event.objectName.size()) * sizeof(QChar);
}
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
    const auto opened = HarkenEditor::Bridge::Monitoring::openEventSession(&desc, &localSession_);
    MonitorEventSessionStatus status{};

    const auto queried = opened == kOk ? HarkenEditor::Bridge::Monitoring::getEventSessionStatus(localSession_, &status) : opened;
    if (queried != kOk) {
        fail(tr("Could not start local capture (%1)").arg(queried));
        return;
    }
    begin(status.monitorSessionEpoch, status.startedTimestampNs);
}

void EventCaptureController::begin(quint64 epoch, quint64 timestamp)
{
    clear();
    sessionEpoch_ = epoch;
    startedNs_ = timestamp;
    targetText_ = pendingTargetText_;
    lastSourceSequence_ = 0;
    sourceDropped_ = deliveryDropped_ = transportDropped_ = trimmed_ = 0;
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
    }
    if (current.state == RemoteEventCaptureState::Failed) {
        fail(current.error);
        return;
    }
    if (state_ == EventCaptureState::Starting && current.state == RemoteEventCaptureState::Capturing)
        begin(current.sessionEpoch, current.startedTimestampNs);
    if (!startedThisAttempt_ || current.sessionEpoch != sessionEpoch_) return;
    sourceDropped_ = current.sourceDroppedRecords;
    deliveryDropped_ = current.deliveryDroppedRecords;
    transportDropped_ = current.transportDroppedRecords;
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
    auto lastSequence = lastSourceSequence_;
    for (const auto& record : batch.records) {
        if (record.unknownOptional || record.header.recordKind != kMonitorRecordEventDetail) continue;
        CapturedEvent event;
        event.header = record.header;
        MonitorEventDetailV2 identity{};
        if (!decodeEventDetail(record, &event.detail, &identity) || event.header.monitorSessionEpoch != epoch ||
            event.header.sourceSequence <= lastSequence || event.detail.timestampNs < startedNs_) {
            fail(tr("Invalid event identity or payload"));
            return;
        }
        event.projectGuid = QUuid::fromRfc4122(QByteArrayView(
            reinterpret_cast<const char*>(identity.projectGuid), sizeof(identity.projectGuid)));
        event.objectGuid = authoringUuid(event.header.stableId);
        if (!event.projectGuid.isNull() && event.projectGuid == projectObjects_.projectGuid()) {
            if (const auto* found = projectObjects_.find(event.header.stableId)) {
                event.objectName = found->name;
            }
        }
        lastSequence = event.header.sourceSequence;
        incoming.push_back(event);
    }
    lastSourceSequence_ = lastSequence;
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
        event.objectGuid.isNull() || runtimeObjectId(event.objectGuid) != event.header.stableId) return 0;
    const auto* found = projectObjects_.find(event.header.stableId);
    return found ? found->ownerObjectId : 0;
}

}
