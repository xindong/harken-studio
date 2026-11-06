#include "StudioInstanceTransport.h"
#include "IPCServer.h"
#include "Core/CommandArgumentValidator.h"
#include "Core/ProjectManager.h"
#include "harken_buildinfo.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

namespace {

constexpr qint64 maxRequestBytes = 16 * 1024 * 1024;
constexpr qint64 maxResponseBytes = 64 * 1024 * 1024;
constexpr int protocolVersion = 1;

QString version()
{
    return QString::fromUtf8(Harken::BuildInfo::Version.data(), Harken::BuildInfo::Version.size());
}

QString registryDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("Harken/Studio/instances"));
}

QString registryFile(const QString& id)
{
    return QDir(registryDirectory()).filePath(id + QStringLiteral(".json"));
}

bool validId(const QString& id)
{
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id;
}

QJsonObject failure(const QString& code, const QString& message)
{
    return {{QStringLiteral("status"), QStringLiteral("error")},
            {QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
}

QJsonObject success(const QJsonObject& result = {})
{
    return {{QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("result"), result}};
}

bool succeeded(const QJsonObject& response)
{
    return response.value(QStringLiteral("status")) == QStringLiteral("ok");
}

QJsonObject readRegistry(const QString& id)
{
    if (!validId(id)) return {};
    QFile file(registryFile(id));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return {};
    const auto result = QJsonDocument::fromJson(file.readAll()).object();
    if (result.value(QStringLiteral("id")) != id) return {};
    return result;
}

bool canceledNow(const std::function<bool()>& canceled)
{
    return canceled && canceled();
}

int waitInterval(const QDeadlineTimer& deadline)
{
    return static_cast<int>(qMin<qint64>(50, qMax<qint64>(1, deadline.remainingTime())));
}

QJsonObject readResponse(QLocalSocket& socket, QDeadlineTimer& deadline,
                         const std::function<bool()>& canceled)
{
    QByteArray bytes;
    while (!deadline.hasExpired() && !canceledNow(canceled)) {
        bytes.append(socket.readAll());
        if (bytes.size() > maxResponseBytes)
            return failure(QStringLiteral("INVALID_RESPONSE"), QStringLiteral("Studio response exceeds the size limit."));
        const auto newline = bytes.indexOf('\n');
        if (newline >= 0) {
            if (newline != bytes.size() - 1)
                return failure(QStringLiteral("INVALID_RESPONSE"), QStringLiteral("Unexpected extra Studio response data."));
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(bytes, &error);
            const auto response = document.object();
            if (error.error != QJsonParseError::NoError || !document.isObject()
                || response.value(QStringLiteral("protocolVersion")).toInt() != protocolVersion
                || (response.value(QStringLiteral("status")) != QStringLiteral("ok")
                    && response.value(QStringLiteral("status")) != QStringLiteral("error")))
                return failure(QStringLiteral("INVALID_RESPONSE"), QStringLiteral("Studio returned an invalid command response."));
            return response;
        }
        if (socket.state() != QLocalSocket::ConnectedState)
            return failure(QStringLiteral("INSTANCE_DISCONNECTED"), QStringLiteral("The Studio instance disconnected."));
        socket.waitForReadyRead(waitInterval(deadline));
    }
    return canceledNow(canceled)
        ? failure(QStringLiteral("CANCELED"), QStringLiteral("Interrupted while waiting for Studio."))
        : failure(QStringLiteral("TIMEOUT"), QStringLiteral("Timed out waiting for Studio."));
}

QJsonObject connectInstance(QLocalSocket& socket, const QString& id, QDeadlineTimer& deadline,
                           const std::function<bool()>& canceled, QJsonObject* descriptor,
                           bool requireSameVersion = true)
{
    if (!validId(id))
        return failure(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Instance ID must be an ID returned by instances."));
    const auto entry = readRegistry(id);
    if (entry.isEmpty())
        return failure(QStringLiteral("INSTANCE_NOT_FOUND"), QStringLiteral("Studio instance not found. Run HarkenStudioCLI instances."));
    if (requireSameVersion && entry.value(QStringLiteral("version")) != version())
        return failure(QStringLiteral("VERSION_MISMATCH"), QStringLiteral("The Studio instance uses a different software version."));
    const auto endpoint = entry.value(QStringLiteral("endpoint")).toString();
    if (endpoint.isEmpty())
        return failure(QStringLiteral("INSTANCE_NOT_FOUND"), QStringLiteral("The Studio instance has no command endpoint."));
    socket.setReadBufferSize(maxResponseBytes + 1);
    socket.connectToServer(endpoint);
    while (socket.state() == QLocalSocket::ConnectingState && !deadline.hasExpired() && !canceledNow(canceled))
        socket.waitForConnected(waitInterval(deadline));
    if (socket.state() != QLocalSocket::ConnectedState) {
        if (canceledNow(canceled))
            return failure(QStringLiteral("CANCELED"), QStringLiteral("Interrupted while connecting to Studio."));
        if (deadline.hasExpired())
            return failure(QStringLiteral("TIMEOUT"), QStringLiteral("Timed out connecting to Studio."));
        return failure(QStringLiteral("INSTANCE_NOT_FOUND"), QStringLiteral("The Studio instance is no longer available: %1").arg(socket.errorString()));
    }
    const auto hello = readResponse(socket, deadline, canceled);
    if (!succeeded(hello)) return hello;
    const auto live = hello.value(QStringLiteral("result")).toObject().value(QStringLiteral("instance")).toObject();
    if (live.value(QStringLiteral("id")) != id)
        return failure(QStringLiteral("INSTANCE_CHANGED"), QStringLiteral("The connected Studio instance does not match the selected instance."));
    if (requireSameVersion && live.value(QStringLiteral("version")) != version())
        return failure(QStringLiteral("VERSION_MISMATCH"), QStringLiteral("The connected Studio uses a different software version."));
    *descriptor = live;
    return success();
}

void sendResponse(QLocalSocket* socket, QJsonObject response)
{
    response.insert(QStringLiteral("protocolVersion"), protocolVersion);
    auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > maxResponseBytes) {
        const auto requestId = response.value(QStringLiteral("request_id"));
        response = failure(QStringLiteral("RESPONSE_TOO_LARGE"), QStringLiteral("Command response exceeds the size limit. Narrow the query."));
        response.insert(QStringLiteral("protocolVersion"), protocolVersion);
        if (!requestId.isUndefined()) response.insert(QStringLiteral("request_id"), requestId);
        bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
    }
    socket->write(bytes);
    socket->flush();
}

}

namespace VNX {

struct StudioInstanceServer::State {
    QPointer<IPCServer> dispatcher;
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString endpoint;
    QString registry;
    QSet<QLocalSocket*> clients;
    QLocalServer server;
};

StudioInstanceServer::StudioInstanceServer(IPCServer* dispatcher, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>())
{
    state_->dispatcher = dispatcher;
    state_->server.setSocketOptions(QLocalServer::UserAccessOption);
    state_->server.setMaxPendingConnections(32);
    connect(&state_->server, &QLocalServer::newConnection, this, [this]() {
        while (auto* client = state_->server.nextPendingConnection()) {
            if (state_->clients.size() >= 32) {
                client->abort();
                client->deleteLater();
                continue;
            }
            state_->clients.insert(client);
            client->setReadBufferSize(maxRequestBytes + 1);
            connect(client, &QLocalSocket::disconnected, this, [this, client]() {
                state_->clients.remove(client);
                client->deleteLater();
            });
            connect(client, &QLocalSocket::readyRead, this, [this, client]() {
                if (client->bytesAvailable() > maxRequestBytes) {
                    sendResponse(client, failure(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Command request exceeds the size limit.")));
                    client->disconnectFromServer();
                    return;
                }
                if (!client->canReadLine()) return;
                if (client->property("dispatching").toBool()) {
                    sendResponse(client, failure(QStringLiteral("REQUEST_IN_PROGRESS"), QStringLiteral("Wait for the current command response before sending another command.")));
                    client->disconnectFromServer();
                    return;
                }
                const auto bytes = client->readLine();
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(bytes, &parseError);
                const auto request = document.object();
                const QJsonObject schema{
                    {"type", "object"}, {"properties", QJsonObject{
                        {"protocolVersion", QJsonObject{{"const", protocolVersion}}},
                        {"version", QJsonObject{{"type", "string"}}},
                        {"instance", QJsonObject{{"type", "string"}}},
                        {"cmd", QJsonObject{{"type", "string"}, {"minLength", 1}}},
                        {"args", QJsonObject{{"type", "object"}}},
                        {"request_id", QJsonObject{{"type", "string"}, {"minLength", 1}}}
                    }}, {"required", QJsonArray{"protocolVersion", "version", "instance", "cmd", "args", "request_id"}},
                    {"additionalProperties", false}
                };
                QString error;
                QJsonObject rejected;
                if (parseError.error != QJsonParseError::NoError || !document.isObject()
                    || !validateCommandArguments(request, schema, &error))
                    rejected = failure(QStringLiteral("INVALID_ARGUMENTS"), error.isEmpty()
                        ? QStringLiteral("Expected a valid JSON command envelope.") : error);
                else if (request.value(QStringLiteral("version")) != version())
                    rejected = failure(QStringLiteral("VERSION_MISMATCH"), QStringLiteral("CLI and Studio software versions must match."));
                else if (request.value(QStringLiteral("instance")) != state_->id)
                    rejected = failure(QStringLiteral("INSTANCE_CHANGED"), QStringLiteral("The request targets a different Studio instance."));
                if (!rejected.isEmpty()) {
                    rejected.insert(QStringLiteral("request_id"), request.value(QStringLiteral("request_id")));
                    sendResponse(client, rejected);
                    client->disconnectFromServer();
                    return;
                }
                client->setProperty("dispatching", true);
                const QPointer<QLocalSocket> guardedClient(client);
                QTimer::singleShot(0, this, [this, guardedClient, request]() {
                    if (!guardedClient || guardedClient->state() != QLocalSocket::ConnectedState) return;
                    IPCServer::IPCDispatchContext context;
                    context.source = ActionSource::IPC;
                    context.origin = IPCServer::DispatchOrigin::CLI;
                    context.requestId = request.value(QStringLiteral("request_id")).toString();
                    auto response = state_->dispatcher
                        ? state_->dispatcher->dispatchCommand(request.value(QStringLiteral("cmd")).toString(),
                            request.value(QStringLiteral("args")).toObject(), context)
                        : failure(QStringLiteral("INSTANCE_CLOSING"), QStringLiteral("Studio is closing."));
                    if (!guardedClient) return;
                    response.insert(QStringLiteral("request_id"), context.requestId);
                    sendResponse(guardedClient, response);
                    guardedClient->setProperty("dispatching", false);
                    if (guardedClient->bytesAvailable() > 0) guardedClient->disconnectFromServer();
                });
            });
            sendResponse(client, success({{QStringLiteral("instance"), descriptor()}}));
        }
    });
}

StudioInstanceServer::~StudioInstanceServer()
{
    stop();
}

bool StudioInstanceServer::start(QString* error)
{
    if (state_->server.isListening()) return true;
    const auto directory = registryDirectory();
    if (!QDir().mkpath(directory)) {
        if (error) *error = QStringLiteral("Cannot create the Studio instance registry: %1").arg(directory);
        return false;
    }
    const auto user = QCryptographicHash::hash(directory.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    state_->endpoint = QStringLiteral("harken-studio-%1-%2").arg(QString::fromLatin1(user), state_->id);
    if (!state_->server.listen(state_->endpoint)) {
        if (error) *error = state_->server.errorString();
        return false;
    }
    state_->registry = registryFile(state_->id);
    QSaveFile file(state_->registry);
    const auto bytes = QJsonDocument(descriptor()).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("Cannot publish the Studio instance: %1").arg(file.errorString());
        state_->server.close();
        state_->registry.clear();
        return false;
    }
    return true;
}

void StudioInstanceServer::stop()
{
    if (!state_->registry.isEmpty()) QFile::remove(state_->registry);
    state_->registry.clear();
    const auto clients = state_->clients;
    for (auto* client : clients) {
        disconnect(client, nullptr, this, nullptr);
        client->abort();
        client->deleteLater();
    }
    state_->clients.clear();
    state_->server.close();
}

QJsonObject StudioInstanceServer::descriptor() const
{
    QJsonObject result{
        {QStringLiteral("id"), state_->id}, {QStringLiteral("version"), version()},
        {QStringLiteral("buildId"), QString::fromUtf8(Harken::BuildInfo::BuildId.data(), Harken::BuildInfo::BuildId.size())},
        {QStringLiteral("buildChannel"), QString::fromUtf8(Harken::BuildInfo::BuildChannel.data(), Harken::BuildInfo::BuildChannel.size())},
        {QStringLiteral("pid"), QCoreApplication::applicationPid()},
        {QStringLiteral("executable"), QCoreApplication::applicationFilePath()},
        {QStringLiteral("endpoint"), state_->endpoint}
    };
    const auto* project = ProjectManager::instance().currentProject();
    result.insert(QStringLiteral("project"), project ? QJsonValue(QJsonObject{
        {QStringLiteral("name"), project->name}, {QStringLiteral("path"), project->projectPath}}) : QJsonValue::Null);
    result.insert(QStringLiteral("state"), !state_->dispatcher ? QStringLiteral("closing")
        : state_->dispatcher->appState() == IPCServer::AppState::Starting ? QStringLiteral("starting")
        : state_->dispatcher->appState() == IPCServer::AppState::ProjectPicker ? QStringLiteral("project_picker")
        : QStringLiteral("ready"));
    return result;
}

}

namespace HarkenStudioCli {

struct StudioInstanceClient::State {
    QString id;
    QJsonObject descriptor;
    QLocalSocket socket;
};

StudioInstanceClient::StudioInstanceClient(QString instanceId) : state_(std::make_unique<State>())
{
    state_->id = std::move(instanceId);
}

StudioInstanceClient::~StudioInstanceClient() = default;

QJsonObject StudioInstanceClient::discover(int timeoutMs, const std::function<bool()>& canceled)
{
    QJsonArray instances;
    const QDir directory(registryDirectory());
    for (const auto& file : directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        if (canceledNow(canceled)) return failure(QStringLiteral("CANCELED"), QStringLiteral("Instance discovery interrupted."));
        const auto id = file.completeBaseName();
        QLocalSocket socket;
        QDeadlineTimer deadline(timeoutMs);
        QJsonObject descriptor;
        const auto connected = connectInstance(socket, id, deadline, canceled, &descriptor, false);
        if (succeeded(connected)) {
            const bool available = descriptor.value(QStringLiteral("version")) == version();
            descriptor.insert(QStringLiteral("available"), available);
            descriptor.insert(QStringLiteral("unavailableReason"), available ? QString()
                : QStringLiteral("VERSION_MISMATCH: Use HarkenStudioCLI version %1 to operate this instance.")
                    .arg(descriptor.value(QStringLiteral("version")).toString()));
            instances.append(descriptor);
        }
        else if (socket.error() == QLocalSocket::ServerNotFoundError
            || socket.error() == QLocalSocket::ConnectionRefusedError)
            QFile::remove(file.absoluteFilePath());
    }
    if (canceledNow(canceled)) return failure(QStringLiteral("CANCELED"), QStringLiteral("Instance discovery interrupted."));
    return success({{QStringLiteral("instances"), instances}});
}

QJsonObject StudioInstanceClient::execute(const QString& name, const QJsonObject& args,
                                         int timeoutMs, const std::function<bool()>& canceled)
{
    if (canceledNow(canceled)) return failure(QStringLiteral("CANCELED"), QStringLiteral("Interrupted."));
    QDeadlineTimer deadline(timeoutMs);
    if (state_->socket.state() != QLocalSocket::ConnectedState) {
        state_->socket.abort();
        const auto connected = connectInstance(state_->socket, state_->id, deadline, canceled, &state_->descriptor);
        if (!succeeded(connected)) {
            state_->socket.abort();
            return connected;
        }
    }
    const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject request{
        {QStringLiteral("protocolVersion"), protocolVersion},
        {QStringLiteral("version"), version()}, {QStringLiteral("instance"), state_->id},
        {QStringLiteral("cmd"), name}, {QStringLiteral("args"), args},
        {QStringLiteral("request_id"), requestId}
    };
    const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > maxRequestBytes)
        return failure(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Command request exceeds the size limit."));
    if (state_->socket.write(bytes) != bytes.size()) {
        state_->socket.abort();
        return failure(QStringLiteral("INSTANCE_DISCONNECTED"), QStringLiteral("Cannot send the command to Studio."));
    }
    while (state_->socket.bytesToWrite() > 0 && !deadline.hasExpired() && !canceledNow(canceled)
        && state_->socket.state() == QLocalSocket::ConnectedState)
        state_->socket.waitForBytesWritten(waitInterval(deadline));
    auto response = readResponse(state_->socket, deadline, canceled);
    if (response.value(QStringLiteral("request_id")) != requestId) {
        if (response.contains(QStringLiteral("request_id")) || succeeded(response))
            response = failure(QStringLiteral("INVALID_RESPONSE"), QStringLiteral("Studio returned a response for a different request."));
        state_->socket.abort();
        response.insert(QStringLiteral("suggestion"), QStringLiteral("The command may still complete in Studio. Check its state before retrying."));
    }
    return response;
}

QJsonObject StudioInstanceClient::commands(int timeoutMs, const std::function<bool()>& canceled)
{
    return execute(QStringLiteral("list_commands"), {}, timeoutMs, canceled);
}

QJsonObject StudioInstanceClient::describe(const QString& name, int timeoutMs,
                                          const std::function<bool()>& canceled)
{
    return execute(QStringLiteral("get_command_schema"), {{QStringLiteral("command"), name}}, timeoutMs, canceled);
}

QJsonObject StudioInstanceClient::validate(const QString& name, const QJsonObject& args,
                                          int timeoutMs, const std::function<bool()>& canceled)
{
    const auto response = describe(name, timeoutMs, canceled);
    if (!succeeded(response)) return response;
    const auto schema = response.value(QStringLiteral("result")).toObject().value(QStringLiteral("inputSchema")).toObject();
    if (schema.isEmpty())
        return failure(QStringLiteral("COMMAND_CONTRACT_MISSING"), QStringLiteral("The Studio command has no argument contract."));
    QString error;
    if (!VNX::validateCommandArguments(args, schema, &error)) {
        auto invalid = failure(QStringLiteral("INVALID_ARGUMENTS"), error);
        invalid.insert(QStringLiteral("command"), name);
        return invalid;
    }
    return success();
}

QJsonObject StudioInstanceClient::descriptor() const
{
    return state_->descriptor;
}

}
