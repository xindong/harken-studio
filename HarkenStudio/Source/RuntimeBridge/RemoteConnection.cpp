#include "RemoteConnection.h"
#include "NativeValues.h"

#include <harken_remote/Protocol.h>
#include <harken/Harken.h>

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace HarkenEditor::Bridge {
namespace {

namespace Wire = harken::remote;
constexpr qint64 kRequestTimeoutMs = 5000;
constexpr qint64 kHeartbeatMs = 10000;
constexpr qint64 kSilenceTimeoutMs = 15000;
constexpr qint64 kSnapshotExpiryMs = 2000;

QString text(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

RemoteTarget targetFromIdentity(const Wire::Identity& identity, const QString& address)
{
    RemoteTarget target;
    target.instanceId = QString::number(identity.instanceId, 16);
    target.application = text(identity.applicationName);
    target.project = text(identity.projectName);
    target.machine = text(identity.machineName);
    target.address = address;
    target.engineVersion = text(identity.engineVersion);
    target.buildId = text(identity.buildId);
    target.platform = text(identity.platform);
    target.processId = identity.processId;
    target.port = static_cast<quint16>(identity.tcpPort);
    target.protocolMajor = identity.major;
    target.protocolMinor = identity.minor;
    target.capabilities = identity.capabilities;
    target.sampleRate = identity.sampleRate;
    target.blockFrames = identity.blockFrames;
    target.compatible = identity.result == 0 && identity.major == Wire::kProtocolMajor;
    return target;
}

QString resultText(qint32 result)
{
    QString message;
    switch (result) {
    case VN_OK: return RemoteConnection::tr("Completed");
    case VN_INVALID_ARG: message = RemoteConnection::tr("Invalid command arguments"); break;
    case VN_UNKNOWN_ID: message = RemoteConnection::tr("Unknown runtime object"); break;
    case VN_STALE_HANDLE: message = RemoteConnection::tr("Runtime object or playback is no longer current"); break;
    case VN_CONTENT_NOT_LOADED: message = RemoteConnection::tr("Required content is not loaded"); break;
    case VN_NOT_INITIALIZED: message = RemoteConnection::tr("Remote engine is not initialized"); break;
    case VN_NO_VOICE: message = RemoteConnection::tr("Voice capacity reached"); break;
    case VN_NO_ACTION: message = RemoteConnection::tr("Action capacity reached"); break;
    case VN_BACKPRESSURE: message = RemoteConnection::tr("Remote queue capacity reached"); break;
    case VN_MONITOR_PERMISSION_DENIED: message = RemoteConnection::tr("Remote capture permission denied"); break;
    case VN_MONITOR_UNSUPPORTED: message = RemoteConnection::tr("Monitor streams are unavailable"); break;
    case VN_VARIABLE_WRITER_FORBIDDEN: message = RemoteConnection::tr("Variable does not permit game writes"); break;
    case VN_VARIABLE_SCOPE_MISMATCH: message = RemoteConnection::tr("Variable scope does not match the request"); break;
    default: return RemoteConnection::tr("Engine result %1").arg(result);
    }
    return RemoteConnection::tr("%1 (%2)").arg(message).arg(result);
}

}

struct RemoteConnection::Impl
{
    struct Pending
    {
        Wire::Message expected = Wire::Message::Error;
        Wire::Operation operation = Wire::Operation::StopAll;
        QString label;
        qint64 deadline = 0;
    };

    RemoteConnection* owner;
    QTcpSocket* socket = nullptr;
    QUdpSocket* discovery = nullptr;
    QTimer* timer = nullptr;
    RemoteConnectionState state = RemoteConnectionState::Disconnected;
    QString status = RemoteConnection::tr("Disconnected");
    RemoteTarget target;
    QVector<RemoteTarget> targets;
    QVector<RemoteCatalogEntry> catalog;
    QVector<RemoteCatalogEntry> catalogStaging;
    RemoteRuntimeSnapshot snapshot;
    QByteArray receive;
    QHash<quint32, Pending> pending;
    quint32 nextRequestId = 1;
    quint32 discoveryRequestId = 0;
    quint64 monitorEpoch = 0;
    quint64 playbackHandle = 0;
    quint64 catalogGeneration = 0;
    quint64 catalogStagingGeneration = 0;
    quint32 catalogOffset = 0;
    quint32 catalogRestarts = 0;
    QElapsedTimer clock;
    qint64 connectedDeadline = 0;
    qint64 lastReceive = 0;
    qint64 lastPing = 0;
    qint64 lastSnapshot = 0;
    qint64 lastSnapshotRequest = 0;
    bool capture = false;
    bool captureRequested = false;
    bool receiveScheduled = false;
    RemoteEventCaptureStatus eventCapture;
    bool eventCaptureUsesExecution = false;

    explicit Impl(RemoteConnection* object) : owner(object)
    {
        clock.start();
        discovery = new QUdpSocket(owner);
        QObject::connect(discovery, &QUdpSocket::readyRead, owner, [this] { readDiscovery(); });
        timer = new QTimer(owner);
        timer->setInterval(100);
        QObject::connect(timer, &QTimer::timeout, owner, [this] { tick(); });
        timer->start();
    }

    quint32 requestId()
    {
        if (nextRequestId == std::numeric_limits<quint32>::max()) {
            fail(RemoteConnection::tr("Request sequence exhausted. Reopen Remote Connect."));
            return 0;
        }
        return nextRequestId++;
    }

    bool connected() const { return state == RemoteConnectionState::Connected; }

    void setState(RemoteConnectionState value, const QString& message)
    {
        state = value;
        status = message;
        emit owner->stateChanged();
    }

    void resetSession(const QString& reason)
    {
        if (eventCapture.state == RemoteEventCaptureState::Starting ||
            eventCapture.state == RemoteEventCaptureState::Capturing ||
            eventCapture.state == RemoteEventCaptureState::Stopping) {
            eventCapture.state = RemoteEventCaptureState::Failed;
            eventCapture.error = reason;
            emit owner->eventCaptureChanged();
        }
        if (socket) {
            QObject::disconnect(socket, nullptr, owner, nullptr);
            socket->abort();
            socket->deleteLater();
            socket = nullptr;
        }
        const auto abandoned = pending;
        pending.clear();
        receive.clear();
        receiveScheduled = false;
        catalog.clear();
        catalogStaging.clear();
        catalogOffset = 0;
        catalogGeneration = 0;
        catalogStagingGeneration = 0;
        catalogRestarts = 0;
        snapshot = {};
        capture = false;
        captureRequested = false;
        monitorEpoch = 0;
        playbackHandle = 0;
        for (const auto& request : abandoned) {
            if (!request.label.isEmpty())
                emit owner->commandFinished(request.label, false, reason);
        }
        emit owner->catalogChanged();
        emit owner->snapshotChanged();
        emit owner->captureChanged();
        emit owner->commandStateChanged();
    }

    void fail(const QString& reason)
    {
        state = RemoteConnectionState::Failed;
        resetSession(reason);
        setState(RemoteConnectionState::Failed, reason);
    }

    bool write(const Wire::Frame& frame)
    {
        if (!socket || socket->state() != QAbstractSocket::ConnectedState)
            return false;
        if (socket->bytesToWrite() > Wire::kMaxPayloadBytes) {
            fail(RemoteConnection::tr("Remote send queue exceeded its limit"));
            return false;
        }
        const auto bytes = Wire::EncodeFrame(frame);
        if (bytes.empty() || socket->write(reinterpret_cast<const char*>(bytes.data()),
                                           static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size())) {
            if (state != RemoteConnectionState::Failed)
                fail(RemoteConnection::tr("Could not send remote request"));
            return false;
        }
        return true;
    }

    bool request(Wire::Frame frame, Wire::Message expected,
                 QString label = {}, Wire::Operation operation = Wire::Operation::StopAll)
    {
        const quint32 id = requestId();
        if (!id)
            return false;
        frame.requestId = id;
        pending.insert(id, {expected, operation, std::move(label),
                           clock.elapsed() + kRequestTimeoutMs});
        if (!write(frame)) {
            pending.remove(id);
            return false;
        }
        emit owner->commandStateChanged();
        return true;
    }

    bool hasPending(Wire::Message expected) const
    {
        return std::any_of(pending.cbegin(), pending.cend(), [expected](const Pending& p) {
            return p.expected == expected;
        });
    }

    void readSocket()
    {
        if (!socket)
            return;
        unsigned processed = 0;
        do {
            const qint64 room = static_cast<qint64>(Wire::kMaxPayloadBytes + Wire::kHeaderBytes) - receive.size();
            if (room < 0) {
                fail(RemoteConnection::tr("Remote frame exceeded its limit"));
                return;
            }
            if (socket->bytesAvailable() > 0 && room > 0)
                receive.append(socket->read(room));
            while (receive.size() >= Wire::kHeaderBytes) {
                Wire::Header header;
                if (!Wire::DecodeHeader(receive.constData(), receive.size(), header)) {
                    fail(RemoteConnection::tr("Invalid remote frame header"));
                    return;
                }
                const qsizetype size = Wire::kHeaderBytes + header.payloadBytes;
                if (receive.size() < size)
                    return;
                Wire::Frame frame;
                frame.type = header.type;
                frame.requestId = header.requestId;
                const auto* begin = reinterpret_cast<const std::uint8_t*>(receive.constData()) + Wire::kHeaderBytes;
                frame.payload.assign(begin, begin + header.payloadBytes);
                receive.remove(0, size);
                lastReceive = clock.elapsed();
                dispatch(frame);
                if (!socket)
                    return;
                if (++processed >= 16) {
                    if (!receiveScheduled) {
                        receiveScheduled = true;
                        const QTcpSocket* source = socket;
                        QTimer::singleShot(0, owner, [this, source] {
                            if (socket != source)
                                return;
                            receiveScheduled = false;
                            readSocket();
                        });
                    }
                    return;
                }
            }
        } while (socket && socket->bytesAvailable() > 0);
    }

    void dispatch(const Wire::Frame& frame)
    {
        if (frame.type == Wire::Message::Error && frame.requestId == 0
            && (state == RemoteConnectionState::Connecting || state == RemoteConnectionState::Handshaking)) {
            Wire::Reader reader(frame.payload);
            std::int32_t result = 0;
            if (reader.I32(result) && reader.Done() && result == VN_IN_USE)
                fail(RemoteConnection::tr("Another Studio is connected to this runtime"));
            else
                fail(RemoteConnection::tr("Invalid remote handshake rejection"));
            return;
        }
        const bool streaming = frame.requestId == 0
            && (frame.type == Wire::Message::Snapshot || frame.type == Wire::Message::MonitorRecords
                || frame.type == Wire::Message::MonitorStatus || frame.type == Wire::Message::EventStatus
                || frame.type == Wire::Message::EventStatusV2 || frame.type == Wire::Message::EventRecords);
        if (!streaming && !pending.contains(frame.requestId)) {
            fail(RemoteConnection::tr("Unexpected remote response"));
            return;
        }
        const Pending request = pending.value(frame.requestId);
        if (!streaming && frame.type != request.expected && frame.type != Wire::Message::Error) {
            fail(RemoteConnection::tr("Remote response type does not match the request"));
            return;
        }
        if (!streaming)
            pending.remove(frame.requestId);
        if (frame.type == Wire::Message::Error) {
            Wire::Reader reader(frame.payload);
            std::int32_t result = 0;
            if (!reader.I32(result) || !reader.Done()) {
                fail(RemoteConnection::tr("Invalid remote error response"));
                return;
            }
            if (request.expected == Wire::Message::EventStatus || request.expected == Wire::Message::EventStatusV2) {
                eventCapture.state = RemoteEventCaptureState::Failed;
                eventCapture.result = result;
                eventCapture.error = resultText(result);
                emit owner->eventCaptureChanged();
                return;
            }
            fail(RemoteConnection::tr("Remote request rejected: %1").arg(resultText(result)));
            return;
        }
        if (frame.type == Wire::Message::Welcome) {
            Wire::Identity identity;
            if (!Wire::DecodePayload(frame, identity) || identity.tcpPort > 65535 || !identity.instanceId) {
                fail(RemoteConnection::tr("Invalid remote handshake"));
                return;
            }
            const QString endpoint = target.address;
            target = targetFromIdentity(identity, endpoint);
            if (!target.compatible) {
                fail(identity.major != Wire::kProtocolMajor
                    ? RemoteConnection::tr("Incompatible remote protocol %1.%2 (Studio requires %3.x), engine %4")
                        .arg(identity.major).arg(identity.minor).arg(Wire::kProtocolMajor).arg(target.engineVersion)
                    : RemoteConnection::tr("Remote handshake rejected: %1").arg(resultText(identity.result)));
                return;
            }
            setState(RemoteConnectionState::Connected, RemoteConnection::tr("Connected to %1 at %2:%3")
                .arg(target.application, target.address).arg(target.port));
            owner->refreshCatalog();
        } else if (!connected()) {
            fail(RemoteConnection::tr("Remote handshake has not completed"));
            return;
        } else if (frame.type == Wire::Message::Pong) {
            if (!frame.payload.empty()) {
                fail(RemoteConnection::tr("Invalid remote heartbeat"));
                return;
            }
        } else if (frame.type == Wire::Message::Catalog) {
            Wire::Catalog result;
            if (!Wire::DecodePayload(frame, result)) {
                fail(RemoteConnection::tr("Invalid remote object catalog"));
                return;
            }
            if (result.result == 0 && (result.offset != catalogOffset || result.totalCount > 65536
                || result.offset + result.entries.size() > result.totalCount
                || (result.offset < result.totalCount && result.entries.empty()))) {
                fail(RemoteConnection::tr("Invalid remote catalog page"));
                return;
            }
            if (result.result == 0 && catalogOffset != 0 && result.generation != catalogStagingGeneration) {
                if (++catalogRestarts > 3) {
                    emit owner->commandFinished(RemoteConnection::tr("Runtime objects"), false,
                        RemoteConnection::tr("Runtime objects are changing; refresh again when loading finishes"));
                    return;
                }
                catalogStaging.clear();
                catalogOffset = 0;
                requestCatalogPage();
                return;
            }
            catalogStagingGeneration = result.generation;
            for (const auto& entry : result.entries)
                catalogStaging.push_back({fromNative(entry.id), text(entry.name), static_cast<quint32>(entry.kind)});
            catalogOffset += static_cast<quint32>(result.entries.size());
            if (result.result == 0 && catalogOffset < result.totalCount) {
                requestCatalogPage();
                return;
            }
            catalog = result.result == 0 ? std::move(catalogStaging) : QVector<RemoteCatalogEntry>{};
            catalogGeneration = result.result == 0 ? catalogStagingGeneration : 0;
            emit owner->catalogChanged();
            emit owner->commandFinished(RemoteConnection::tr("Runtime objects"), result.result == 0,
                result.result != 0 ? resultText(result.result)
                    : RemoteConnection::tr("%1 of %2 objects").arg(catalog.size()).arg(result.totalCount));
        } else if (frame.type == Wire::Message::Snapshot) {
            Wire::Snapshot result;
            if (!Wire::DecodePayload(frame, result)) {
                fail(RemoteConnection::tr("Invalid remote runtime status"));
                return;
            }
            if (!capture)
                return;
            snapshot = {};
            snapshot.valid = result.result == 0;
            snapshot.blockIndex = result.blockIndex;
            snapshot.activeVoices = result.activeVoices;
            snapshot.busCount = result.busCount;
            snapshot.callbackCpuUs = result.callbackCpuUs;
            snapshot.updateCpuUs = result.updateCpuUs;
            snapshot.backpressureCount = result.commandBackpressure;
            for (const auto& bus : result.buses) {
                if (!std::isfinite(bus.peak)) {
                    fail(RemoteConnection::tr("Invalid remote meter value"));
                    return;
                }
                snapshot.peak = std::max(snapshot.peak, bus.peak);
            }
            lastSnapshot = clock.elapsed();
            emit owner->snapshotChanged();
        } else if (frame.type == Wire::Message::EventStatus || frame.type == Wire::Message::EventStatusV2) {
            const bool execution = frame.type == Wire::Message::EventStatusV2;
            Wire::EventCaptureStatusV2 status;
            const bool decoded = execution ? Wire::DecodePayload(frame, status) : Wire::DecodePayload(frame, status.base);
            const auto& result = status.base;
            if (execution != eventCaptureUsesExecution || !decoded ||
                (result.result == VN_OK && (!result.sessionEpoch || !result.startedTimestampNs ||
                    (execution && (status.acceptedExecutionFactVersion != 2 ||
                        (status.acceptedStreamKindMask & VN_MONITOR_STREAM_EVENT) == 0)) ||
                    ((result.state == VN_MONITOR_EVENT_SESSION_DRAINED || result.stoppedTimestampNs != 0) &&
                     result.stoppedTimestampNs < result.startedTimestampNs)))) {
                fail(RemoteConnection::tr("Invalid event capture status"));
                return;
            }
            const bool starting = eventCapture.state == RemoteEventCaptureState::Starting;
            const bool stopping = eventCapture.state == RemoteEventCaptureState::Stopping;
            if (streaming && (starting ||
                (eventCapture.state != RemoteEventCaptureState::Capturing && !stopping) ||
                result.sessionEpoch != eventCapture.sessionEpoch)) return;
            if (!starting && result.result == VN_OK && result.sessionEpoch != eventCapture.sessionEpoch) {
                fail(RemoteConnection::tr("Event capture session changed unexpectedly"));
                return;
            }
            eventCapture.acceptedExecutionFactVersion = status.acceptedExecutionFactVersion;
            eventCapture.baselineAvailability = status.baselineAvailability;
            eventCapture.requestedPermissionMask = status.requestedPermissionMask;
            eventCapture.acceptedPermissionMask = status.acceptedPermissionMask;
            eventCapture.deniedPermissionMask = status.deniedPermissionMask;
            eventCapture.unsupportedPermissionMask = status.unsupportedPermissionMask;
            eventCapture.acceptedStreamKindMask = status.acceptedStreamKindMask;
            eventCapture.symbolsDecision = status.symbolsDecision;
            eventCapture.symbolAvailability = status.symbolAvailability;
            eventCapture.identityUnavailableCount = status.identityUnavailableCount;
            eventCapture.baselineStateCutId = status.baselineStateCutId;
            std::copy_n(status.supportedFactKinds, 2, eventCapture.supportedFactKinds);
            eventCapture.result = result.result;
            eventCapture.sessionEpoch = result.sessionEpoch;
            eventCapture.startedTimestampNs = result.startedTimestampNs;
            eventCapture.stoppedTimestampNs = result.stoppedTimestampNs;
            eventCapture.sourceDroppedRecords = result.sourceDroppedRecords;
            eventCapture.deliveryDroppedRecords = result.deliveryDroppedRecords;
            eventCapture.transportDroppedRecords = result.transportDroppedRecords;
            eventCapture.error = result.result == VN_OK ? QString{} : resultText(result.result);
            eventCapture.state = result.result != VN_OK ? RemoteEventCaptureState::Failed
                : result.state == VN_MONITOR_EVENT_SESSION_DRAINED ? RemoteEventCaptureState::Stopped
                : stopping || result.state == VN_MONITOR_EVENT_SESSION_SEALED ? RemoteEventCaptureState::Stopping
                : RemoteEventCaptureState::Capturing;
            emit owner->eventCaptureChanged();
        } else if (frame.type == Wire::Message::EventRecords) {
            Wire::MonitorRecords records;
            if (!Wire::DecodePayload(frame, records)) {
                fail(RemoteConnection::tr("Invalid event capture records"));
                return;
            }
            if ((eventCapture.state == RemoteEventCaptureState::Capturing ||
                 eventCapture.state == RemoteEventCaptureState::Stopping) &&
                records.sessionEpoch == eventCapture.sessionEpoch) {
                emit owner->eventRecordsReceived(
                    QByteArray(reinterpret_cast<const char*>(records.bytes.data()),
                               static_cast<qsizetype>(records.bytes.size())),
                    records.recordCount, records.sessionEpoch);
            }
        } else if (frame.type == Wire::Message::MonitorStatus) {
            Wire::MonitorStatus result;
            if (!Wire::DecodePayload(frame, result)) {
                fail(RemoteConnection::tr("Invalid remote capture response"));
                return;
            }
            capture = captureRequested;
            monitorEpoch = result.result == 0 && capture && result.streamMask != 0 ? result.sessionEpoch : 0;
            if (!capture) {
                snapshot = {};
                emit owner->snapshotChanged();
            }
            emit owner->captureChanged();
            emit owner->commandFinished(RemoteConnection::tr("Capture"), result.result == 0,
                result.result == 0 ? (capture ? RemoteConnection::tr("Started") : RemoteConnection::tr("Stopped"))
                                   : resultText(result.result));
        } else if (frame.type == Wire::Message::MonitorRecords) {
            Wire::MonitorRecords records;
            if (!Wire::DecodePayload(frame, records)) {
                fail(RemoteConnection::tr("Invalid remote monitor records"));
                return;
            }
            if (capture && records.sessionEpoch == monitorEpoch)
                emit owner->monitorRecordsReceived(
                    QByteArray(reinterpret_cast<const char*>(records.bytes.data()), static_cast<qsizetype>(records.bytes.size())), records.recordCount);
        } else if (frame.type == Wire::Message::CommandResult) {
            Wire::CommandResult result;
            if (!Wire::DecodePayload(frame, result) || result.operation != request.operation) {
                fail(RemoteConnection::tr("Invalid remote command response"));
                return;
            }
            if (result.result == 0) {
                if (result.operation == Wire::Operation::PostAction)
                    playbackHandle = result.handle;
                else if (result.operation == Wire::Operation::StopAction || result.operation == Wire::Operation::StopAll)
                    playbackHandle = 0;
            }
            emit owner->commandFinished(request.label, result.result == 0,
                result.result == 0 && result.handle != 0
                    ? RemoteConnection::tr("Completed, playback %1").arg(result.handle) : resultText(result.result));
        } else {
            fail(RemoteConnection::tr("Unsupported remote message"));
            return;
        }
        emit owner->commandStateChanged();
    }

    void readDiscovery()
    {
        unsigned processed = 0;
        bool changed = false;
        while (discovery->hasPendingDatagrams() && ++processed <= 64) {
            const auto datagram = discovery->receiveDatagram(4096);
            if (kLocalConnectionsOnly && !datagram.senderAddress().isLoopback())
                continue;
            const auto bytes = datagram.data();
            Wire::Header header;
            if (!Wire::DecodeHeader(bytes.constData(), bytes.size(), header)
                || header.type != Wire::Message::Announcement || header.requestId != discoveryRequestId
                || bytes.size() != Wire::kHeaderBytes + header.payloadBytes)
                continue;
            Wire::Reader reader(bytes.constData() + Wire::kHeaderBytes, header.payloadBytes);
            Wire::Identity identity;
            if (!Wire::Decode(reader, identity) || identity.tcpPort == 0 || identity.tcpPort > 65535
                || identity.instanceId == 0)
                continue;
            QHostAddress address = datagram.senderAddress();
            if (!identity.bindAddress.empty() && identity.bindAddress != "0.0.0.0") {
                if (!address.setAddress(text(identity.bindAddress))
                    || address.protocol() != QAbstractSocket::IPv4Protocol)
                    continue;
            }
            if (kLocalConnectionsOnly && !address.isLoopback())
                continue;
            auto target = targetFromIdentity(identity, address.toString());
            auto previous = std::find_if(targets.begin(), targets.end(), [&](const auto& row) {
                return row.instanceId == target.instanceId;
            });
            if (previous == targets.end() && targets.size() >= 1024)
                continue;
            if (previous == targets.end())
                targets.push_back(target);
            else {
                const QHostAddress previousAddress(previous->address);
                const QHostAddress candidateAddress(target.address);
                const bool preferCandidate = candidateAddress.isLoopback()
                    && (!previousAddress.isLoopback() || candidateAddress == QHostAddress::LocalHost);
                if (!preferCandidate)
                    target.address = previous->address;
                *previous = target;
            }
            changed = true;
        }
        if (changed)
            emit owner->targetsChanged();
        if (discovery->hasPendingDatagrams())
            QTimer::singleShot(0, owner, [this] { readDiscovery(); });
    }

    void tick()
    {
        const qint64 now = clock.elapsed();
        if ((state == RemoteConnectionState::Connecting || state == RemoteConnectionState::Handshaking)
            && now >= connectedDeadline) {
            fail(RemoteConnection::tr("Remote connection timed out"));
            return;
        }
        for (const auto& request : std::as_const(pending)) {
            if (now >= request.deadline) {
                fail(request.expected == Wire::Message::CommandResult
                    ? RemoteConnection::tr("Remote command timed out; execution outcome is unknown")
                    : RemoteConnection::tr("Remote response timed out"));
                return;
            }
        }
        if (!connected())
            return;
        if (now - lastReceive >= kSilenceTimeoutMs) {
            fail(RemoteConnection::tr("Remote application stopped responding"));
            return;
        }
        if (snapshot.valid && now - lastSnapshot >= kSnapshotExpiryMs) {
            snapshot.valid = false;
            emit owner->snapshotChanged();
        }
        if (capture && now - lastSnapshotRequest >= 250 && !hasPending(Wire::Message::Snapshot)) {
            lastSnapshotRequest = now;
            request({Wire::Message::SnapshotRequest, 0, {}}, Wire::Message::Snapshot);
        }
        if (now - lastPing >= kHeartbeatMs && !hasPending(Wire::Message::Pong)) {
            lastPing = now;
            request({Wire::Message::Ping, 0, {}}, Wire::Message::Pong);
        }
    }

    void requestCatalogPage()
    {
        Wire::Writer writer;
        writer.U32(catalogOffset);
        request({Wire::Message::CatalogRequest, 0, std::move(writer.bytes)}, Wire::Message::Catalog);
    }

    void command(const Wire::Command& value, const QString& label)
    {
        if (!connected() || !(target.capabilities & Wire::kCapabilityControl)) {
            emit owner->commandFinished(label, false, RemoteConnection::tr("Remote control is unavailable"));
            return;
        }
        if (hasPending(Wire::Message::CommandResult)) {
            emit owner->commandFinished(label, false, RemoteConnection::tr("A remote command is still pending"));
            return;
        }
        request(Wire::MakeFrame(Wire::Message::Command, 0, value),
                Wire::Message::CommandResult, label, value.operation);
    }
};

RemoteConnection::RemoteConnection(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>(this)) {}

RemoteConnection::~RemoteConnection()
{
    if (impl_->socket) {
        QObject::disconnect(impl_->socket, nullptr, this, nullptr);
        impl_->socket->abort();
    }
}

RemoteConnectionState RemoteConnection::state() const { return impl_->state; }
QString RemoteConnection::statusText() const { return impl_->status; }
RemoteTarget RemoteConnection::target() const { return impl_->target; }
QVector<RemoteTarget> RemoteConnection::discoveredTargets() const { return impl_->targets; }
QVector<RemoteCatalogEntry> RemoteConnection::catalog() const { return impl_->catalog; }
RemoteRuntimeSnapshot RemoteConnection::snapshot() const { return impl_->snapshot; }
bool RemoteConnection::captureEnabled() const { return impl_->capture; }
bool RemoteConnection::capturePending() const { return impl_->hasPending(Wire::Message::MonitorStatus); }
bool RemoteConnection::canControl() const { return impl_->connected() && (impl_->target.capabilities & Wire::kCapabilityControl); }
bool RemoteConnection::canCapture() const { return impl_->connected() && (impl_->target.capabilities & Wire::kCapabilitySnapshot); }
bool RemoteConnection::commandPending() const { return impl_->hasPending(Wire::Message::CommandResult); }
quint64 RemoteConnection::lastPlaybackHandle() const { return impl_->playbackHandle; }
bool RemoteConnection::canCaptureEvents() const
{
    return impl_->connected() &&
        (impl_->target.capabilities & (Wire::kCapabilityEventCapture | Wire::kCapabilityExecutionObservation)) != 0;
}
RemoteEventCaptureStatus RemoteConnection::eventCaptureStatus() const { return impl_->eventCapture; }

void RemoteConnection::startEventCapture()
{
    if (!canCaptureEvents() || (impl_->hasPending(Wire::Message::EventStatus) || impl_->hasPending(Wire::Message::EventStatusV2)) ||
        impl_->eventCapture.state == RemoteEventCaptureState::Capturing ||
        impl_->eventCapture.state == RemoteEventCaptureState::Stopping) return;
    impl_->eventCapture = {};
    impl_->eventCapture.state = RemoteEventCaptureState::Starting;
    emit eventCaptureChanged();
    impl_->eventCaptureUsesExecution = (impl_->target.capabilities & Wire::kCapabilityExecutionObservation) != 0;
    if (impl_->eventCaptureUsesExecution) {
        Wire::EventCaptureRequestV2 request;
        request.executionFactVersion = 2;
        request.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
        request.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_SYMBOLS;
        request.requestSymbols = 1;
        request.consumerQueueBytes = 1048576;
        impl_->request(Wire::MakeFrame(Wire::Message::EventStartV2, 0, request), Wire::Message::EventStatusV2);
    } else {
        impl_->request({Wire::Message::EventStart, 0, {}}, Wire::Message::EventStatus);
    }
}

void RemoteConnection::stopEventCapture()
{
    if (!impl_->connected() || impl_->eventCapture.state != RemoteEventCaptureState::Capturing ||
        (impl_->hasPending(Wire::Message::EventStatus) || impl_->hasPending(Wire::Message::EventStatusV2))) return;
    impl_->eventCapture.state = RemoteEventCaptureState::Stopping;
    emit eventCaptureChanged();
    Wire::Writer writer;
    writer.U64(impl_->eventCapture.sessionEpoch);
    impl_->request({Wire::Message::EventSeal, 0, std::move(writer.bytes)},
        impl_->eventCaptureUsesExecution ? Wire::Message::EventStatusV2 : Wire::Message::EventStatus);
}

void RemoteConnection::discover(quint16 discoveryPort)
{
    impl_->discovery->close();
    impl_->discoveryRequestId = 0;
    impl_->targets.clear();
    emit targetsChanged();
    if (!impl_->discovery->bind(kLocalConnectionsOnly ? QHostAddress::LocalHost : QHostAddress::AnyIPv4, 0)) {
        emit discoveryFailed(tr("Discovery failed: %1").arg(impl_->discovery->errorString()));
        return;
    }
    impl_->discoveryRequestId = impl_->requestId();
    if (!impl_->discoveryRequestId)
        return;
    const auto packed = Wire::EncodeFrame({Wire::Message::Discover, impl_->discoveryRequestId, {}});
    const QByteArray bytes(reinterpret_cast<const char*>(packed.data()), static_cast<qsizetype>(packed.size()));
    impl_->discovery->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
    impl_->discovery->setSocketOption(QAbstractSocket::MulticastTtlOption, kLocalConnectionsOnly ? 0 : 1);
    bool sent = impl_->discovery->writeDatagram(bytes, QHostAddress::LocalHost, discoveryPort) == bytes.size();
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp)
            || (kLocalConnectionsOnly && !(interface.flags() & QNetworkInterface::IsLoopBack))
            || (!(interface.flags() & QNetworkInterface::CanMulticast)
                && !(interface.flags() & QNetworkInterface::IsLoopBack)))
            continue;
        impl_->discovery->setMulticastInterface(interface);
        sent = impl_->discovery->writeDatagram(bytes, QHostAddress(QString::fromLatin1(Wire::kDiscoveryGroup)),
                                               discoveryPort) == bytes.size() || sent;
    }
    if (!sent)
        emit discoveryFailed(tr("Discovery failed: %1").arg(impl_->discovery->errorString()));
}

void RemoteConnection::connectTarget(const QString& address, quint16 port)
{
    if (impl_->state != RemoteConnectionState::Disconnected && impl_->state != RemoteConnectionState::Failed)
        return;
    QString host = address.trimmed();
    if (host.isEmpty() || port == 0) {
        impl_->fail(tr("A host and TCP port are required"));
        return;
    }
    if constexpr (kLocalConnectionsOnly) {
        QHostAddress endpoint;
        if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0)
            endpoint = QHostAddress::LocalHost;
        else if (!endpoint.setAddress(host) || !endpoint.isLoopback()) {
            impl_->fail(tr("Only connections to this computer are available"));
            return;
        }
        host = endpoint.toString();
    }
    impl_->resetSession(tr("Disconnected before the command result arrived; execution outcome is unknown"));
    impl_->target = {};
    impl_->target.address = host;
    impl_->target.port = port;
    auto* socket = new QTcpSocket(this);
    impl_->socket = socket;
    socket->setReadBufferSize(Wire::kMaxPayloadBytes + Wire::kHeaderBytes);
    connect(socket, &QTcpSocket::connected, this, [this, socket] {
        if (impl_->socket != socket)
            return;
        impl_->lastReceive = impl_->lastPing = impl_->clock.elapsed();
        impl_->setState(RemoteConnectionState::Handshaking, tr("Checking remote compatibility"));
        Wire::Hello hello;
        hello.clientName = "Harken Studio";
        impl_->request(Wire::MakeFrame(Wire::Message::Hello, 0, hello), Wire::Message::Welcome);
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
        if (impl_->socket == socket)
            impl_->readSocket();
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (impl_->socket == socket && socket->bytesAvailable() > 0)
            impl_->readSocket();
        if (impl_->socket == socket)
            impl_->fail(tr("Remote application disconnected"));
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
        if (impl_->socket == socket && socket->bytesAvailable() > 0)
            impl_->readSocket();
        if (impl_->socket == socket)
            impl_->fail(tr("Remote connection failed: %1").arg(socket->errorString()));
    });
    impl_->connectedDeadline = impl_->clock.elapsed() + kRequestTimeoutMs;
    impl_->setState(RemoteConnectionState::Connecting, tr("Connecting to %1:%2").arg(host).arg(port));
    if constexpr (kLocalConnectionsOnly)
        socket->connectToHost(QHostAddress(host), port);
    else
        socket->connectToHost(host, port);
}

void RemoteConnection::disconnectTarget()
{
    impl_->state = RemoteConnectionState::Disconnected;
    impl_->resetSession(tr("Disconnected before the command result arrived; execution outcome is unknown"));
    impl_->setState(RemoteConnectionState::Disconnected, tr("Disconnected"));
}

void RemoteConnection::refreshCatalog()
{
    if (impl_->connected() && (impl_->target.capabilities & Wire::kCapabilityCatalog)
        && !impl_->hasPending(Wire::Message::Catalog))
    {
        impl_->catalogStaging.clear();
        impl_->catalogOffset = 0;
        impl_->catalogRestarts = 0;
        impl_->requestCatalogPage();
    }
}

void RemoteConnection::setCaptureEnabled(bool enabled)
{
    if (!canCapture()
        || impl_->hasPending(Wire::Message::MonitorStatus)) {
        emit captureChanged();
        return;
    }
    impl_->captureRequested = enabled;
    if (enabled) {
        impl_->capture = true;
        impl_->lastSnapshotRequest = 0;
        if (impl_->target.capabilities & Wire::kCapabilityMonitorRecords)
            impl_->request(Wire::MakeFrame(Wire::Message::MonitorStart, 0, Wire::MonitorRequest{}), Wire::Message::MonitorStatus);
        emit captureChanged();
    }
    else {
        impl_->capture = false;
        impl_->monitorEpoch = 0;
        impl_->snapshot = {};
        emit snapshotChanged();
        emit captureChanged();
        if (impl_->target.capabilities & Wire::kCapabilityMonitorRecords)
            impl_->request({Wire::Message::MonitorStop, 0, {}}, Wire::Message::MonitorStatus);
    }
}

void RemoteConnection::postAction(harkenstudio::ObjectId actionId)
{
    if (harkenstudio::IsNull(actionId))
        return;
    Wire::Command command;
    command.operation = Wire::Operation::PostAction;
    command.expectedGeneration = impl_->catalogGeneration;
    command.target = toNative(actionId);
    impl_->command(command, tr("Play remote action"));
}

void RemoteConnection::stopPlayback(quint64 playbackHandle)
{
    if (!playbackHandle)
        return;
    Wire::Command command;
    command.operation = Wire::Operation::StopAction;
    command.instanceHandle = playbackHandle;
    impl_->command(command, tr("Stop remote playback"));
}

void RemoteConnection::stopAll()
{
    Wire::Command command;
    command.operation = Wire::Operation::StopAll;
    impl_->command(command, tr("Stop all remote playback"));
}

void RemoteConnection::setParameter(harkenstudio::ObjectId variableId, float value)
{
    if (harkenstudio::IsNull(variableId) || !std::isfinite(value))
        return;
    Wire::Command command;
    command.operation = Wire::Operation::SetGlobalVariable;
    command.expectedGeneration = impl_->catalogGeneration;
    command.target = toNative(variableId);
    command.value = value;
    impl_->command(command, tr("Set remote variable"));
}

}
