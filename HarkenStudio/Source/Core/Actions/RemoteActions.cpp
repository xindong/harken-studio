#include "../ActionManager.h"
#include "../CommandSchemas.h"
#include "../ProjectManager.h"
#include "../ProjectObjectIndex.h"
#include "../RemoteSession.h"
#include "Monitor/EventCaptureController.h"
#include "RuntimeBridge/RemoteConnection.h"
#include "Utils/ObjectId.h"

#include <QJsonArray>

#include <algorithm>

namespace harken::studio {

namespace {

using harken::studio::bridge::RemoteConnectionState;
using Monitor::DesignerEventType;
using Monitor::EventCaptureState;

QString connectionStateName(RemoteConnectionState state)
{
    switch (state) {
    case RemoteConnectionState::Disconnected: return QStringLiteral("disconnected");
    case RemoteConnectionState::Connecting: return QStringLiteral("connecting");
    case RemoteConnectionState::Handshaking: return QStringLiteral("handshaking");
    case RemoteConnectionState::Connected: return QStringLiteral("connected");
    case RemoteConnectionState::Failed: return QStringLiteral("failed");
    }
    return {};
}

QString captureStateName(EventCaptureState state)
{
    switch (state) {
    case EventCaptureState::Unavailable: return QStringLiteral("unavailable");
    case EventCaptureState::Idle: return QStringLiteral("idle");
    case EventCaptureState::Starting: return QStringLiteral("starting");
    case EventCaptureState::Capturing: return QStringLiteral("capturing");
    case EventCaptureState::Stopping: return QStringLiteral("stopping");
    case EventCaptureState::Stopped: return QStringLiteral("stopped");
    case EventCaptureState::Failed: return QStringLiteral("failed");
    }
    return {};
}

QString eventTypeName(DesignerEventType type)
{
    switch (type) {
    case DesignerEventType::None: return QStringLiteral("none");
    case DesignerEventType::Action: return QStringLiteral("action");
    case DesignerEventType::Step: return QStringLiteral("step");
    case DesignerEventType::Playable: return QStringLiteral("playable");
    case DesignerEventType::Variable: return QStringLiteral("variable");
    case DesignerEventType::GameObject: return QStringLiteral("game_object");
    }
    return {};
}

QJsonObject targetJson(const harken::studio::bridge::RemoteTarget& target)
{
    return {{"instance_id", target.instanceId}, {"application", target.application},
            {"project", target.project}, {"machine", target.machine}, {"address", target.address},
            {"port", target.port}, {"platform", target.platform}, {"engine_version", target.engineVersion},
            {"compatible", target.compatible}};
}

QJsonObject statusJson()
{
    auto& remote = RemoteSession::instance().connection();
    auto& capture = RemoteSession::instance().capture();
    QJsonArray targets;
    for (const auto& target : remote.discoveredTargets()) targets.append(targetJson(target));
    return {{"state", connectionStateName(remote.state())}, {"status", remote.statusText()},
            {"target", targetJson(remote.target())}, {"discovered_targets", targets},
            {"capture", QJsonObject{{"state", captureStateName(capture.state())},
                                    {"status", capture.statusText()},
                                    {"retained_events", capture.rowCount()},
                                    {"recording", RemoteSession::instance().isRecording()},
                                    {"recording_bytes", static_cast<qint64>(RemoteSession::instance().recordingStatus().fileBytes)},
                                    {"next_cursor", static_cast<qint64>(capture.trimmedRecords() + capture.rowCount())}}}};
}

}

void registerRemoteActions(ActionManager& mgr)
{
    const auto add = [&mgr](const char* id, const char* name, std::function<ActionResult(const QJsonObject&)> handler) {
        ActionDef def;
        def.id = id;
        def.displayName = name;
        def.category = "system";
        def.paramSchema = commandInputSchema(def.id);
        def.undoable = false;
        def.handler = std::move(handler);
        mgr.registerAction(def);
    };

    add("remote_discover", "Discover Remote Runtimes", [](const QJsonObject& args) -> ActionResult {
        RemoteSession::instance().connection().discover(static_cast<quint16>(args.value("discovery_port").toInt(37840)));
        return {true, statusJson()};
    });
    add("remote_connect", "Connect Remote Runtime", [](const QJsonObject& args) -> ActionResult {
        RemoteSession::instance().connection().connectTarget(args.value("address").toString(),
                                                            static_cast<quint16>(args.value("port").toInt()));
        return {true, statusJson()};
    });
    add("remote_disconnect", "Disconnect Remote Runtime", [](const QJsonObject&) -> ActionResult {
        RemoteSession::instance().connection().disconnectTarget();
        return {true, statusJson()};
    });
    add("get_remote_status", "Get Remote Status", [](const QJsonObject&) -> ActionResult {
        return {true, statusJson()};
    });
    add("get_remote_catalog", "Get Remote Catalog", [](const QJsonObject&) -> ActionResult {
        const auto objects = ProjectObjectIndex::fromProject(ProjectManager::instance().currentProject());
        QJsonArray rows;
        for (const auto& entry : RemoteSession::instance().connection().catalog()) {
            const auto* found = objects.find(entry.id);
            rows.append(QJsonObject{{"id", objectIdString(entry.id)}, {"kind", static_cast<qint64>(entry.kind)},
                                    {"name", found ? found->name : QString()}});
        }
        return {true, QJsonObject{{"entries", rows}, {"count", rows.size()}}};
    });
    add("poll_remote_events", "Poll Remote Events", [](const QJsonObject& args) -> ActionResult {
        auto& capture = RemoteSession::instance().capture();
        const qint64 first = static_cast<qint64>(capture.trimmedRecords());
        const qint64 end = first + capture.rowCount();
        const qint64 cursor = args.value("cursor").toInteger(0);
        const qint64 begin = std::clamp(cursor, first, end);
        const qint64 stop = std::min(end, begin + args.value("max").toInteger(100));
        QJsonArray events;
        for (qint64 index = begin; index < stop; ++index) {
            const auto& event = capture.eventAt(static_cast<int>(index - first));
            events.append(QJsonObject{
                {"cursor", index}, {"type", eventTypeName(event.type())},
                {"timestamp_ns", static_cast<qint64>(event.timestampNs())},
                {"object_id", objectIdString(event.objectId())}, {"object_name", event.objectName},
                {"action_name", event.actionName}, {"game_object", event.gameObjectName},
                {"description", event.playbackDescription}});
        }
        return {true, QJsonObject{
            {"events", events}, {"next_cursor", stop}, {"missed", std::max<qint64>(0, first - cursor)},
            {"source_dropped", static_cast<qint64>(capture.sourceDropped())},
            {"delivery_dropped", static_cast<qint64>(capture.deliveryDropped())},
            {"transport_dropped", static_cast<qint64>(capture.transportDropped())},
            {"capture_state", captureStateName(capture.state())}}};
    });
    add("start_remote_capture", "Start Remote Capture", [](const QJsonObject& args) -> ActionResult {
        auto& session = RemoteSession::instance();
        auto& capture = session.capture();
        if (session.connection().state() != RemoteConnectionState::Connected || !capture.canStart())
            return {false, statusJson(), "CAPTURE_UNAVAILABLE", capture.availableTargetText()};
        const QString output = args.value("output_path").toString();
        if (!output.isEmpty() && !session.startRecording(output))
            return {false, statusJson(), "RECORDING_FAILED", QStringLiteral("Cannot write %1").arg(output)};
        capture.start();
        return {true, statusJson()};
    });
    add("stop_remote_capture", "Stop Remote Capture", [](const QJsonObject&) -> ActionResult {
        auto& session = RemoteSession::instance();
        session.capture().stop();
        session.stopRecording();
        return {true, statusJson()};
    });
}

}
