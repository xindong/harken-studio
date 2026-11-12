#include "RemoteConnection.h"
#include "NativeValues.h"

#include <harken/HarkenDevelopment.h>

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace HarkenEditor::Bridge {
namespace {

constexpr qint64 kSnapshotExpiryMs = 2000;
constexpr unsigned kEventsPerTick = 256;

template <std::size_t N>
QString text(const char (&value)[N])
{
    return QString::fromUtf8(value, static_cast<qsizetype>(qstrnlen(value, N)));
}

RemoteTarget targetFromNative(const VnRemoteClientTarget& native)
{
    RemoteTarget target;
    target.instanceId = QString::number(native.instanceId, 16);
    target.application = text(native.applicationName);
    target.project = text(native.projectName);
    target.machine = text(native.machineName);
    target.address = text(native.address);
    target.engineVersion = text(native.engineVersion);
    target.buildId = text(native.buildId);
    target.platform = text(native.platform);
    target.processId = native.processId;
    target.port = static_cast<quint16>(native.tcpPort);
    target.protocolMajor = native.protocolMajor;
    target.protocolMinor = native.protocolMinor;
    target.capabilities = native.capabilities;
    target.sampleRate = native.sampleRate;
    target.blockFrames = native.blockFrames;
    target.compatible = native.compatible != 0;
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
    enum class Kind { Snapshot, Catalog, Command, MonitorStatus, CaptureStatus };

    struct Pending
    {
        Kind kind = Kind::Command;
        quint32 operation = 0;
        QString label;
    };

    RemoteConnection* owner;
    VnRemoteClient client = 0;
    QTimer* timer = nullptr;
    RemoteConnectionState state = RemoteConnectionState::Disconnected;
    QString status = RemoteConnection::tr("Disconnected");
    RemoteTarget target;
    QVector<RemoteTarget> targets;
    QVector<RemoteCatalogEntry> catalog;
    QVector<RemoteCatalogEntry> catalogStaging;
    RemoteRuntimeSnapshot snapshot;
    QByteArray buffer;
    QHash<quint32, Pending> pending;
    quint64 monitorEpoch = 0;
    quint64 playbackHandle = 0;
    quint64 catalogGeneration = 0;
    quint64 catalogStagingGeneration = 0;
    quint32 catalogOffset = 0;
    quint32 catalogRestarts = 0;
    QElapsedTimer clock;
    qint64 lastSnapshot = 0;
    qint64 lastSnapshotRequest = 0;
    bool capture = false;
    bool captureRequested = false;
    RemoteEventCaptureStatus eventCapture;
    bool eventCaptureUsesExecution = false;

    explicit Impl(RemoteConnection* object) : owner(object)
    {
        clock.start();
        VnRemoteClientDesc desc{};
        desc.structSize = sizeof(desc);
        desc.flags = kLocalConnectionsOnly ? VN_REMOTE_CLIENT_LOCAL_ONLY : 0u;
        desc.clientName = "Harken Studio";
        if (Vn_RemoteClientCreate(&desc, &client) != VN_OK)
            client = 0;
        buffer.resize(65536);
        timer = new QTimer(owner);
        timer->setInterval(16);
        QObject::connect(timer, &QTimer::timeout, owner, [this] { tick(); });
        timer->start();
    }

    ~Impl()
    {
        if (client)
            Vn_RemoteClientDestroy(client);
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
        if (client)
            Vn_RemoteClientDisconnect(client);
        const auto abandoned = pending;
        pending.clear();
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

    bool request(VnRemoteClientRequest value, Kind kind, QString label = {})
    {
        value.structSize = sizeof(value);
        quint32 id = 0;
        if (!client || Vn_RemoteClientRequest(client, &value, &id) != VN_OK || id == 0) {
            if (!label.isEmpty())
                emit owner->commandFinished(label, false, RemoteConnection::tr("Could not send remote request"));
            return false;
        }
        pending.insert(id, {kind, value.operation, std::move(label)});
        emit owner->commandStateChanged();
        return true;
    }

    bool request(quint32 requestKind, Kind kind)
    {
        VnRemoteClientRequest value{};
        value.kind = requestKind;
        return request(value, kind);
    }

    bool hasPending(Kind kind) const
    {
        return std::any_of(pending.cbegin(), pending.cend(), [kind](const Pending& p) { return p.kind == kind; });
    }

    QString failureText(const VnRemoteClientEvent& event) const
    {
        switch (event.failure) {
        case VN_REMOTE_CLIENT_FAILURE_CONNECT:
            return RemoteConnection::tr("Remote connection failed (system error %1)").arg(event.osError);
        case VN_REMOTE_CLIENT_FAILURE_DISCONNECTED: return RemoteConnection::tr("Remote application disconnected");
        case VN_REMOTE_CLIENT_FAILURE_CONNECT_TIMEOUT: return RemoteConnection::tr("Remote connection timed out");
        case VN_REMOTE_CLIENT_FAILURE_RESPONSE_TIMEOUT: return RemoteConnection::tr("Remote response timed out");
        case VN_REMOTE_CLIENT_FAILURE_COMMAND_TIMEOUT:
            return RemoteConnection::tr("Remote command timed out; execution outcome is unknown");
        case VN_REMOTE_CLIENT_FAILURE_SILENT: return RemoteConnection::tr("Remote application stopped responding");
        case VN_REMOTE_CLIENT_FAILURE_IN_USE: return RemoteConnection::tr("Another Studio is connected to this runtime");
        case VN_REMOTE_CLIENT_FAILURE_INCOMPATIBLE:
            return event.target.protocolMajor != VN_REMOTE_PROTOCOL_MAJOR
                ? RemoteConnection::tr("Incompatible remote protocol %1.%2 (Studio requires %3.x), engine %4")
                      .arg(event.target.protocolMajor).arg(event.target.protocolMinor)
                      .arg(VN_REMOTE_PROTOCOL_MAJOR).arg(text(event.target.engineVersion))
                : RemoteConnection::tr("Remote handshake rejected: %1").arg(resultText(event.result));
        case VN_REMOTE_CLIENT_FAILURE_SEND: return RemoteConnection::tr("Could not send remote request");
        case VN_REMOTE_CLIENT_FAILURE_SEQUENCE_EXHAUSTED:
            return RemoteConnection::tr("Request sequence exhausted. Reopen Remote Connect.");
        default: return RemoteConnection::tr("Invalid remote response");
        }
    }

    void poll()
    {
        if (!client)
            return;
        for (unsigned processed = 0; processed < kEventsPerTick; ++processed) {
            VnRemoteClientEvent event{};
            event.structSize = sizeof(event);
            VnResult result = Vn_RemoteClientPoll(client, &event, buffer.data(), static_cast<quint64>(buffer.size()));
            if (result == VN_REMOTE_CLIENT_BUFFER_TOO_SMALL) {
                buffer.resize(static_cast<qsizetype>(event.dataBytes));
                result = Vn_RemoteClientPoll(client, &event, buffer.data(), static_cast<quint64>(buffer.size()));
            }
            if (result != VN_OK)
                return;
            dispatch(event, buffer.constData());
        }
    }

    void dispatch(const VnRemoteClientEvent& event, const char* data)
    {
        switch (event.kind) {
        case VN_REMOTE_CLIENT_EVENT_TARGET_FOUND: targetFound(targetFromNative(event.target)); return;
        case VN_REMOTE_CLIENT_EVENT_FAILED:
            if (state != RemoteConnectionState::Disconnected && state != RemoteConnectionState::Failed)
                fail(failureText(event));
            return;
        case VN_REMOTE_CLIENT_EVENT_HANDSHAKING:
            if (state == RemoteConnectionState::Connecting)
                setState(RemoteConnectionState::Handshaking, RemoteConnection::tr("Checking remote compatibility"));
            return;
        case VN_REMOTE_CLIENT_EVENT_CONNECTED: {
            const QString endpoint = target.address;
            target = targetFromNative(event.target);
            if (target.address.isEmpty())
                target.address = endpoint;
            setState(RemoteConnectionState::Connected, RemoteConnection::tr("Connected to %1 at %2:%3")
                .arg(target.application, target.address).arg(target.port));
            owner->refreshCatalog();
            return;
        }
        default: break;
        }
        if (!connected())
            return;
        const bool streaming = event.requestId == 0;
        Pending request;
        if (!streaming) {
            if (!pending.contains(event.requestId))
                return;
            request = pending.take(event.requestId);
        }
        if (event.kind == VN_REMOTE_CLIENT_EVENT_REJECTED) {
            if (request.kind == Kind::CaptureStatus) {
                eventCapture.state = RemoteEventCaptureState::Failed;
                eventCapture.result = event.result;
                eventCapture.error = resultText(event.result);
                emit owner->eventCaptureChanged();
                emit owner->commandStateChanged();
                return;
            }
            fail(RemoteConnection::tr("Remote request rejected: %1").arg(resultText(event.result)));
            return;
        }
        switch (event.kind) {
        case VN_REMOTE_CLIENT_EVENT_CATALOG: catalogPage(event, data); break;
        case VN_REMOTE_CLIENT_EVENT_SNAPSHOT: if (!snapshotReceived(event, data)) return; break;
        case VN_REMOTE_CLIENT_EVENT_CAPTURE_STATUS: if (!captureStatus(event.capture, streaming)) return; break;
        case VN_REMOTE_CLIENT_EVENT_CAPTURE_RECORDS:
            if ((eventCapture.state == RemoteEventCaptureState::Capturing ||
                 eventCapture.state == RemoteEventCaptureState::Stopping) &&
                event.sessionEpoch == eventCapture.sessionEpoch)
                emit owner->eventRecordsReceived(QByteArray(data, static_cast<qsizetype>(event.dataBytes)),
                    event.recordCount, event.sessionEpoch);
            break;
        case VN_REMOTE_CLIENT_EVENT_MONITOR_STATUS:
            capture = captureRequested;
            monitorEpoch = event.result == VN_OK && capture && event.streamMask != 0 ? event.sessionEpoch : 0;
            if (!capture) {
                snapshot = {};
                emit owner->snapshotChanged();
            }
            emit owner->captureChanged();
            emit owner->commandFinished(RemoteConnection::tr("Capture"), event.result == VN_OK,
                event.result == VN_OK ? (capture ? RemoteConnection::tr("Started") : RemoteConnection::tr("Stopped"))
                                      : resultText(event.result));
            break;
        case VN_REMOTE_CLIENT_EVENT_MONITOR_RECORDS:
            if (capture && event.sessionEpoch == monitorEpoch)
                emit owner->monitorRecordsReceived(QByteArray(data, static_cast<qsizetype>(event.dataBytes)),
                    event.recordCount);
            break;
        case VN_REMOTE_CLIENT_EVENT_COMMAND_RESULT:
            if (event.result == VN_OK) {
                if (event.operation == VN_REMOTE_OPERATION_POST_ACTION)
                    playbackHandle = event.handle;
                else if (event.operation == VN_REMOTE_OPERATION_STOP_ACTION ||
                         event.operation == VN_REMOTE_OPERATION_STOP_ALL)
                    playbackHandle = 0;
            }
            emit owner->commandFinished(request.label, event.result == VN_OK,
                event.result == VN_OK && event.handle != 0
                    ? RemoteConnection::tr("Completed, playback %1").arg(event.handle) : resultText(event.result));
            break;
        default:
            fail(RemoteConnection::tr("Unsupported remote message"));
            return;
        }
        emit owner->commandStateChanged();
    }

    void catalogPage(const VnRemoteClientEvent& event, const char* data)
    {
        if (event.result == VN_OK && (event.catalogOffset != catalogOffset || event.catalogTotal > 65536
            || event.catalogOffset + event.catalogRows > event.catalogTotal
            || (event.catalogOffset < event.catalogTotal && event.catalogRows == 0))) {
            fail(RemoteConnection::tr("Invalid remote catalog page"));
            return;
        }
        if (event.result == VN_OK && catalogOffset != 0 && event.catalogGeneration != catalogStagingGeneration) {
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
        catalogStagingGeneration = event.catalogGeneration;
        for (quint32 i = 0; i < event.catalogRows; ++i) {
            VnRemoteClientCatalogRow row{};
            std::memcpy(&row, data + i * sizeof(row), sizeof(row));
            catalogStaging.push_back({fromNative(row.id), row.kind});
        }
        catalogOffset += event.catalogRows;
        if (event.result == VN_OK && catalogOffset < event.catalogTotal) {
            requestCatalogPage();
            return;
        }
        catalog = event.result == VN_OK ? std::move(catalogStaging) : QVector<RemoteCatalogEntry>{};
        catalogGeneration = event.result == VN_OK ? catalogStagingGeneration : 0;
        emit owner->catalogChanged();
        emit owner->commandFinished(RemoteConnection::tr("Runtime objects"), event.result == VN_OK,
            event.result != VN_OK ? resultText(event.result)
                : RemoteConnection::tr("%1 of %2 objects").arg(catalog.size()).arg(event.catalogTotal));
    }

    bool snapshotReceived(const VnRemoteClientEvent& event, const char* data)
    {
        if (!capture)
            return true;
        const auto& native = event.snapshot;
        snapshot = {};
        snapshot.valid = native.result == VN_OK;
        snapshot.blockIndex = native.blockIndex;
        snapshot.activeVoices = native.activeVoices;
        snapshot.busCount = native.busCount;
        snapshot.callbackCpuUs = native.callbackCpuUs;
        snapshot.updateCpuUs = native.updateCpuUs;
        snapshot.backpressureCount = native.commandBackpressure;
        const char* buses = data + native.voiceRows * sizeof(VnRemoteClientVoice);
        for (quint32 i = 0; i < native.busRows; ++i) {
            VnRemoteClientBus bus{};
            std::memcpy(&bus, buses + i * sizeof(bus), sizeof(bus));
            if (!std::isfinite(bus.peak)) {
                fail(RemoteConnection::tr("Invalid remote meter value"));
                return false;
            }
            snapshot.peak = std::max(snapshot.peak, bus.peak);
        }
        lastSnapshot = clock.elapsed();
        emit owner->snapshotChanged();
        return true;
    }

    bool captureStatus(const VnRemoteClientCaptureStatus& status, bool streaming)
    {
        const bool execution = status.execution != 0;
        if (execution != eventCaptureUsesExecution ||
            (status.result == VN_OK && (!status.sessionEpoch || !status.startedTimestampNs ||
                (execution && (status.acceptedExecutionFactVersion != 2 ||
                    (status.acceptedStreamKindMask & VN_MONITOR_STREAM_EVENT) == 0)) ||
                ((status.state == VN_MONITOR_EVENT_SESSION_DRAINED || status.stoppedTimestampNs != 0) &&
                 status.stoppedTimestampNs < status.startedTimestampNs)))) {
            fail(RemoteConnection::tr("Invalid event capture status"));
            return false;
        }
        const bool starting = eventCapture.state == RemoteEventCaptureState::Starting;
        const bool stopping = eventCapture.state == RemoteEventCaptureState::Stopping;
        if (streaming && (starting ||
            (eventCapture.state != RemoteEventCaptureState::Capturing && !stopping) ||
            status.sessionEpoch != eventCapture.sessionEpoch)) return true;
        if (!starting && status.result == VN_OK && status.sessionEpoch != eventCapture.sessionEpoch) {
            fail(RemoteConnection::tr("Event capture session changed unexpectedly"));
            return false;
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
        eventCapture.result = status.result;
        eventCapture.sessionEpoch = status.sessionEpoch;
        eventCapture.startedTimestampNs = status.startedTimestampNs;
        eventCapture.stoppedTimestampNs = status.stoppedTimestampNs;
        eventCapture.sourceDroppedRecords = status.sourceDroppedRecords;
        eventCapture.deliveryDroppedRecords = status.deliveryDroppedRecords;
        eventCapture.transportDroppedRecords = status.transportDroppedRecords;
        eventCapture.error = status.result == VN_OK ? QString{} : resultText(status.result);
        eventCapture.state = status.result != VN_OK ? RemoteEventCaptureState::Failed
            : status.state == VN_MONITOR_EVENT_SESSION_DRAINED ? RemoteEventCaptureState::Stopped
            : stopping || status.state == VN_MONITOR_EVENT_SESSION_SEALED ? RemoteEventCaptureState::Stopping
            : RemoteEventCaptureState::Capturing;
        emit owner->eventCaptureChanged();
        return true;
    }

    void targetFound(RemoteTarget found)
    {
        auto previous = std::find_if(targets.begin(), targets.end(), [&](const auto& row) {
            return row.instanceId == found.instanceId;
        });
        if (previous == targets.end() && targets.size() >= 1024)
            return;
        if (previous == targets.end())
            targets.push_back(found);
        else {
            const QHostAddress previousAddress(previous->address);
            const QHostAddress candidateAddress(found.address);
            const bool preferCandidate = candidateAddress.isLoopback()
                && (!previousAddress.isLoopback() || candidateAddress == QHostAddress::LocalHost);
            if (!preferCandidate)
                found.address = previous->address;
            *previous = found;
        }
        emit owner->targetsChanged();
    }

    void tick()
    {
        poll();
        if (!connected())
            return;
        const qint64 now = clock.elapsed();
        if (snapshot.valid && now - lastSnapshot >= kSnapshotExpiryMs) {
            snapshot.valid = false;
            emit owner->snapshotChanged();
        }
        if (capture && now - lastSnapshotRequest >= 250 && !hasPending(Kind::Snapshot)) {
            lastSnapshotRequest = now;
            request(VN_REMOTE_CLIENT_REQUEST_SNAPSHOT, Kind::Snapshot);
        }
    }

    void requestCatalogPage()
    {
        VnRemoteClientRequest value{};
        value.kind = VN_REMOTE_CLIENT_REQUEST_CATALOG;
        value.offset = catalogOffset;
        request(value, Kind::Catalog);
    }

    void command(VnRemoteClientRequest value, const QString& label)
    {
        if (!connected() || !(target.capabilities & VN_REMOTE_CAPABILITY_CONTROL)) {
            emit owner->commandFinished(label, false, RemoteConnection::tr("Remote control is unavailable"));
            return;
        }
        if (hasPending(Kind::Command)) {
            emit owner->commandFinished(label, false, RemoteConnection::tr("A remote command is still pending"));
            return;
        }
        value.kind = VN_REMOTE_CLIENT_REQUEST_COMMAND;
        request(value, Kind::Command, label);
    }
};

RemoteConnection::RemoteConnection(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>(this)) {}

RemoteConnection::~RemoteConnection() = default;

RemoteConnectionState RemoteConnection::state() const { return impl_->state; }
QString RemoteConnection::statusText() const { return impl_->status; }
RemoteTarget RemoteConnection::target() const { return impl_->target; }
QVector<RemoteTarget> RemoteConnection::discoveredTargets() const { return impl_->targets; }
QVector<RemoteCatalogEntry> RemoteConnection::catalog() const { return impl_->catalog; }
RemoteRuntimeSnapshot RemoteConnection::snapshot() const { return impl_->snapshot; }
bool RemoteConnection::captureEnabled() const { return impl_->capture; }
bool RemoteConnection::capturePending() const { return impl_->hasPending(Impl::Kind::MonitorStatus); }
bool RemoteConnection::canControl() const { return impl_->connected() && (impl_->target.capabilities & VN_REMOTE_CAPABILITY_CONTROL); }
bool RemoteConnection::canCapture() const { return impl_->connected() && (impl_->target.capabilities & VN_REMOTE_CAPABILITY_SNAPSHOT); }
bool RemoteConnection::commandPending() const { return impl_->hasPending(Impl::Kind::Command); }
quint64 RemoteConnection::lastPlaybackHandle() const { return impl_->playbackHandle; }
bool RemoteConnection::canCaptureEvents() const
{
    return impl_->connected() &&
        (impl_->target.capabilities & (VN_REMOTE_CAPABILITY_EVENT_CAPTURE | VN_REMOTE_CAPABILITY_EXECUTION_OBSERVATION)) != 0;
}
RemoteEventCaptureStatus RemoteConnection::eventCaptureStatus() const { return impl_->eventCapture; }

void RemoteConnection::startEventCapture()
{
    if (!canCaptureEvents() || impl_->hasPending(Impl::Kind::CaptureStatus) ||
        impl_->eventCapture.state == RemoteEventCaptureState::Capturing ||
        impl_->eventCapture.state == RemoteEventCaptureState::Stopping) return;
    impl_->eventCapture = {};
    impl_->eventCapture.state = RemoteEventCaptureState::Starting;
    emit eventCaptureChanged();
    impl_->eventCaptureUsesExecution = (impl_->target.capabilities & VN_REMOTE_CAPABILITY_EXECUTION_OBSERVATION) != 0;
    if (impl_->eventCaptureUsesExecution) {
        VnRemoteClientRequest request{};
        request.kind = VN_REMOTE_CLIENT_REQUEST_EVENT_START_V2;
        request.executionFactVersion = 2;
        request.streamMask = VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
        request.permissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_SYMBOLS;
        request.requestSymbols = 1;
        request.consumerQueueBytes = 1048576;
        impl_->request(request, Impl::Kind::CaptureStatus);
    } else {
        impl_->request(VN_REMOTE_CLIENT_REQUEST_EVENT_START, Impl::Kind::CaptureStatus);
    }
}

void RemoteConnection::stopEventCapture()
{
    if (!impl_->connected() || impl_->eventCapture.state != RemoteEventCaptureState::Capturing ||
        impl_->hasPending(Impl::Kind::CaptureStatus)) return;
    impl_->eventCapture.state = RemoteEventCaptureState::Stopping;
    emit eventCaptureChanged();
    VnRemoteClientRequest request{};
    request.kind = VN_REMOTE_CLIENT_REQUEST_EVENT_SEAL;
    request.sessionEpoch = impl_->eventCapture.sessionEpoch;
    impl_->request(request, Impl::Kind::CaptureStatus);
}

void RemoteConnection::discover(quint16 discoveryPort)
{
    impl_->targets.clear();
    emit targetsChanged();
    if (!impl_->client || Vn_RemoteClientDiscover(impl_->client, discoveryPort) != VN_OK)
        emit discoveryFailed(tr("Discovery failed"));
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
    const QByteArray utf8 = host.toUtf8();
    if (!impl_->client || Vn_RemoteClientConnect(impl_->client, utf8.constData(), port) != VN_OK) {
        impl_->fail(tr("Remote connection failed"));
        return;
    }
    impl_->setState(RemoteConnectionState::Connecting, tr("Connecting to %1:%2").arg(host).arg(port));
}

void RemoteConnection::disconnectTarget()
{
    impl_->state = RemoteConnectionState::Disconnected;
    impl_->resetSession(tr("Disconnected before the command result arrived; execution outcome is unknown"));
    impl_->setState(RemoteConnectionState::Disconnected, tr("Disconnected"));
}

void RemoteConnection::refreshCatalog()
{
    if (impl_->connected() && (impl_->target.capabilities & VN_REMOTE_CAPABILITY_CATALOG)
        && !impl_->hasPending(Impl::Kind::Catalog))
    {
        impl_->catalogStaging.clear();
        impl_->catalogOffset = 0;
        impl_->catalogRestarts = 0;
        impl_->requestCatalogPage();
    }
}

void RemoteConnection::setCaptureEnabled(bool enabled)
{
    if (!canCapture() || impl_->hasPending(Impl::Kind::MonitorStatus)) {
        emit captureChanged();
        return;
    }
    impl_->captureRequested = enabled;
    if (enabled) {
        impl_->capture = true;
        impl_->lastSnapshotRequest = 0;
        if (impl_->target.capabilities & VN_REMOTE_CAPABILITY_MONITOR_RECORDS) {
            VnRemoteClientRequest request{};
            request.kind = VN_REMOTE_CLIENT_REQUEST_MONITOR_START;
            request.streamMask = 4;
            request.cadenceHz = 20;
            request.bytesPerSecond = 262144;
            impl_->request(request, Impl::Kind::MonitorStatus);
        }
        emit captureChanged();
    }
    else {
        impl_->capture = false;
        impl_->monitorEpoch = 0;
        impl_->snapshot = {};
        emit snapshotChanged();
        emit captureChanged();
        if (impl_->target.capabilities & VN_REMOTE_CAPABILITY_MONITOR_RECORDS)
            impl_->request(VN_REMOTE_CLIENT_REQUEST_MONITOR_STOP, Impl::Kind::MonitorStatus);
    }
}

void RemoteConnection::postAction(harkenstudio::ObjectId actionId)
{
    if (harkenstudio::IsNull(actionId))
        return;
    VnRemoteClientRequest request{};
    request.operation = VN_REMOTE_OPERATION_POST_ACTION;
    request.expectedGeneration = impl_->catalogGeneration;
    request.target = toNative(actionId);
    impl_->command(request, tr("Play remote action"));
}

void RemoteConnection::stopPlayback(quint64 playbackHandle)
{
    if (!playbackHandle)
        return;
    VnRemoteClientRequest request{};
    request.operation = VN_REMOTE_OPERATION_STOP_ACTION;
    request.instanceHandle = playbackHandle;
    impl_->command(request, tr("Stop remote playback"));
}

void RemoteConnection::stopAll()
{
    VnRemoteClientRequest request{};
    request.operation = VN_REMOTE_OPERATION_STOP_ALL;
    impl_->command(request, tr("Stop all remote playback"));
}

void RemoteConnection::setParameter(harkenstudio::ObjectId variableId, float value)
{
    if (harkenstudio::IsNull(variableId) || !std::isfinite(value))
        return;
    VnRemoteClientRequest request{};
    request.operation = VN_REMOTE_OPERATION_SET_GLOBAL_VARIABLE;
    request.expectedGeneration = impl_->catalogGeneration;
    request.target = toNative(variableId);
    request.value = value;
    impl_->command(request, tr("Set remote variable"));
}

}
