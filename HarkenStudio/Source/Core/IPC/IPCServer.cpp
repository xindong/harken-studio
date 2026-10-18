
#include "IPCServerInternal.h"
#include "IPCBinaryMixerLevels.h"
#include "IPCBinaryVoiceSnapshot.h"
#include "ActionManager.h"
#include <QFileInfo>
#include <QSet>
#include <QThread>
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>

namespace VNX {

namespace {
struct IpcDispatchDepthGuard
{
    int& depth;
    explicit IpcDispatchDepthGuard(int& d) : depth(d) { ++depth; }
    ~IpcDispatchDepthGuard() { --depth; }
};

struct DispatchContextGuard
{
    QVector<IPCServer::IPCDispatchContext>& stack;
    explicit DispatchContextGuard(QVector<IPCServer::IPCDispatchContext>& value,
                                  const IPCServer::IPCDispatchContext& context)
        : stack(value)
    {
        stack.append(context);
    }
    ~DispatchContextGuard()
    {
        stack.removeLast();
    }
};

struct TransactionContextGuard
{
    ActionManager& actionManager;
    TransactionExecutionContext previous;
    TransactionContextGuard(ActionManager& manager,
                            const TransactionExecutionContext& context)
        : actionManager(manager),
          previous(manager.transactionExecutionContext())
    {
        actionManager.setTransactionExecutionContext(context);
    }
    ~TransactionContextGuard()
    {
        actionManager.setTransactionExecutionContext(previous);
    }
};
}


IPCServer::IPCServer(AudioEngine* engine, QObject* parent)
    : QObject(parent)
    , engine_(engine)
    , vqlEngine_(std::make_unique<VQLEngine>())
{
    registerCommandHandlers();

    if (engine_)
    {
        connect(engine_, &AudioEngine::playbackStarted, this, [this](quintptr handle, const QString& name) {
            if (handle == 0) return;
            activePlaybacks_[handle] = QJsonObject{
                {"handle", static_cast<qint64>(handle)},
                {"name", name},
                {"position", QJsonArray{0.0, 0.0, 0.0}}
            };
        });

        connect(engine_, &AudioEngine::playbackStopped, this, [this](quintptr handle) {
            activePlaybacks_.remove(handle);
        });
    }
}

VQLEngine* IPCServer::vqlEngine() const
{
    return vqlEngine_.get();
}

void IPCServer::setProjectManager(ProjectManager* projectManager)
{
    projectManager_ = projectManager;
    if (projectManager_)
    {
        connect(projectManager_, &ProjectManager::itemCreated, this,
                [this](ProjectItem*) { if (vqlEngine_) vqlEngine_->notifyProjectChanged(); });
        connect(projectManager_, &ProjectManager::itemMoved, this,
                [this](ProjectItem* item, ProjectItem*, ProjectItem*) {
            if (!vqlEngine_) return;
            vqlEngine_->indexObjectChanged(item);
            vqlEngine_->notifyProjectChanged();
        });
        connect(projectManager_, &ProjectManager::itemDeleted, this,
                [this](ProjectItem*) { if (vqlEngine_) vqlEngine_->notifyProjectChanged(); });
        connect(projectManager_, &ProjectManager::itemRenamed, this,
                [this](ProjectItem*, const QString&) { if (vqlEngine_) vqlEngine_->notifyProjectChanged(); });
        connect(projectManager_, &ProjectManager::folderMoved, this,
                [this](PhysicalFolder* folder) { if (vqlEngine_) vqlEngine_->notifyFolderChanged(folder); });
        connect(projectManager_, &ProjectManager::folderRenamed, this,
                [this](PhysicalFolder* folder, const QString&) { if (vqlEngine_) vqlEngine_->notifyFolderChanged(folder); });
        connect(projectManager_, &ProjectManager::itemPropertyChanged, this,
                [this](ProjectItem*, const QString&) { if (vqlEngine_) vqlEngine_->notifyProjectChanged(); });

    }
}

IPCServer::~IPCServer()
{
    if (transcodeThread_) transcodeThread_->wait();
    if (packBuildThread_) {
        packBuildThread_->wait();
    }
    finishPackBuild();
    stop();
}

void IPCServer::finishPackBuild()
{
    auto complete = std::move(packBuildCompletion_);
    packBuildCompletion_ = {};
    if (complete) complete();
}


bool IPCServer::start(int port)
{
    if (server_ && server_->isListening())
    {
        qWarning() << "IPCServer already running on port" << port_;
        return true;
    }

    port_ = port;
    server_ = new QTcpServer(this);

    connect(server_, &QTcpServer::newConnection, this, &IPCServer::onNewConnection);

    if (!server_->listen(QHostAddress::LocalHost, port_))
    {
        QString error = QString("Failed to start IPC server on port %1: %2")
                        .arg(port_).arg(server_->errorString());
        qCritical() << error;
        emit errorOccurred(error);
        server_->deleteLater();
        server_ = nullptr;
        return false;
    }

    port_ = static_cast<int>(server_->serverPort());
    qInfo() << "HarkenStudio IPC Server started on port" << port_;
    emit runningChanged(true);
    return true;
}

bool IPCServer::isRunning() const
{
    return server_ && server_->isListening();
}

void IPCServer::stop()
{
    if (!server_)
        return;

    for (QTcpSocket* client : clients_)
    {
        client->disconnectFromHost();
        client->deleteLater();
    }
    clients_.clear();

    server_->close();
    server_->deleteLater();
    server_ = nullptr;

    qInfo() << "HarkenStudio IPC Server stopped";
    emit runningChanged(false);
    emit clientCountChanged(0);
}

void IPCServer::onNewConnection()
{
    while (server_->hasPendingConnections())
    {
        QTcpSocket* client = server_->nextPendingConnection();

        connect(client, &QTcpSocket::readyRead, this, &IPCServer::onClientReadyRead);
        connect(client, &QTcpSocket::disconnected, this, &IPCServer::onClientDisconnected);
        connect(client, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
                this, &IPCServer::onClientError);

        clients_.append(client);

        QString address = client->peerAddress().toString();
        qInfo() << "MCP client connected from" << address;
        emit clientConnected(address);
        emit clientCountChanged(clients_.size());
    }
}

void IPCServer::onClientReadyRead()
{
    QTcpSocket* client = qobject_cast<QTcpSocket*>(sender());
    if (!client)
        return;

    while (client->bytesAvailable() > 0) {
        QByteArray peek = client->peek(1);
        if (peek.isEmpty()) break;

        uint8_t firstByte = static_cast<uint8_t>(peek[0]);

        if (firstByte == IPC_BINARY_MAGIC) {
            if (client->bytesAvailable() < static_cast<qint64>(sizeof(IPCBinaryHeader)))
                return;

            QByteArray headerBytes = client->peek(sizeof(IPCBinaryHeader));
            const auto* hdr = reinterpret_cast<const IPCBinaryHeader*>(headerBytes.constData());
            qint64 totalSize = sizeof(IPCBinaryHeader) + hdr->payloadSize;

            if (client->bytesAvailable() < totalSize)
                return;

            QByteArray frameData = client->read(totalSize);
            QPointer<QTcpSocket> safeClient(client);
            QTimer::singleShot(0, this, [this, safeClient, frameData]() {
                if (!safeClient) return;
                handleBinaryFrame(safeClient.data(), frameData);
            });
        }
        else {
            if (!client->canReadLine())
                return;

            QByteArray line = client->readLine().trimmed();
            if (line.isEmpty())
                continue;

            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);

            if (parseError.error != QJsonParseError::NoError)
            {
                qWarning() << "Invalid JSON from client:" << parseError.errorString();
                sendResponse(client, errorResponse("INVALID_JSON", parseError.errorString()));
                continue;
            }

            QJsonObject request = doc.object();
            QString cmd = request["cmd"].toString();
            QJsonObject args = request["args"].toObject();

            const QJsonValue requestId = request.value("request_id");

            if (cmd == QLatin1String("set_channel_role"))
            {
                const QString role = args.value("role").toString();
                if (role == QLatin1String("command"))
                    commandOnlyClients_.insert(client);
                else
                    commandOnlyClients_.remove(client);
                qInfo() << "IPC channel role set to" << role
                        << "for" << client->peerAddress().toString();
                QJsonObject roleResponse = successResponse(QJsonObject{{"role", role}});
                if (!requestId.isUndefined() && !requestId.isNull())
                    roleResponse.insert("request_id", requestId);
                sendResponse(client, roleResponse);
                continue;
            }

            qDebug() << "IPC command:" << cmd << "args:" << args;
            emit commandReceived(cmd, args);

            QPointer<QTcpSocket> safeClient(client);
            QTimer::singleShot(0, this, [this, safeClient, cmd, args, requestId]() {
                if (!safeClient) return;
                QJsonObject response = handleCommand(cmd, args);
                if (!safeClient) return;
                if (!requestId.isUndefined() && !requestId.isNull())
                    response.insert("request_id", requestId);
                sendResponse(safeClient.data(), response);
            });
        }
    }
}

void IPCServer::onClientDisconnected()
{
    QTcpSocket* client = qobject_cast<QTcpSocket*>(sender());
    if (!client)
        return;

    QString address = client->peerAddress().toString();
    clients_.removeAll(client);
    commandOnlyClients_.remove(client);
    client->deleteLater();

    qInfo() << "MCP client disconnected:" << address;
    emit clientDisconnected(address);
    emit clientCountChanged(clients_.size());
}

void IPCServer::onClientError()
{
    QTcpSocket* client = qobject_cast<QTcpSocket*>(sender());
    if (!client)
        return;

    qWarning() << "Client socket error:" << client->error() << client->errorString();
}


void IPCServer::registerCommandHandlers()
{
    registerSystemHandlers();
    registerProjectHandlers();
    registerMixerHandlers();
    registerEditorHandlers();
    registerSpatialHandlers();
    registerAssetHandlers();
    registerContainerHandlers();
    registerAdvancedHandlers();

    VNX::setupTransactionCallbacks(this);
}

QJsonObject IPCServer::handleCommand(const QString& cmd, const QJsonObject& args)
{
    if (!dispatchContextStack_.isEmpty())
        return dispatchCommand(cmd, args, dispatchContextStack_.constLast());
    IPCDispatchContext context;
    context.source = ActionSource::IPC;
    context.origin = DispatchOrigin::IPC;
    return dispatchCommand(cmd, args, context);
}

QJsonObject IPCServer::dispatchCommand(const QString& cmd, const QJsonObject& args)
{
    if (!dispatchContextStack_.isEmpty())
        return dispatchCommand(cmd, args, dispatchContextStack_.constLast());
    return dispatchCommand(cmd, args, IPCDispatchContext{});
}

QJsonObject IPCServer::dispatchCommand(const QString& cmd, const QJsonObject& args,
                                       const IPCDispatchContext& context)
{
    // This 3-argument overload is the single point every dispatch path funnels
    // through (socket handleCommand, in-process AI calls, transaction callbacks,
    // and nested re-dispatch), so the depth guard here marks command execution
    // as "dispatching" for every source. That lets the UI suppress a
    // ProjectManager errorOccurred emitted during execution into a non-modal
    // status update instead of blocking the turn on a modal dialog.
    IpcDispatchDepthGuard dispatchGuard(ipcDispatchDepth_);
    DispatchContextGuard contextGuard(dispatchContextStack_, context);
    ActionManager& actionManager = ActionManager::instance();
    TransactionContextGuard transactionContextGuard(
        actionManager, transactionContextFor(context));
    const auto withRequestId = [&](QJsonObject response) {
        if (!context.requestId.isEmpty())
            response[QStringLiteral("request_id")] = context.requestId;
        return response;
    };

    if (context.origin == DispatchOrigin::AI)
    {
        if (auto gateError = enforceAIPolicyGate(cmd, args, context, false))
            return withRequestId(*gateError);
    }

    if (!isJsonCommandAvailableInCurrentState(cmd))
        return withRequestId(commandUnavailableInCurrentStateResponse(cmd));

    if (projectManager_ && projectManager_->isClosing()
        && cmd != QLatin1String("get_app_state")
        && cmd != QLatin1String("create_project")
        && cmd != QLatin1String("open_project"))
    {
        return withRequestId(errorResponse("PROJECT_CLOSING",
                                           "Project is being closed, command rejected."));
    }

    if (mainWindow_ && mainWindow_->isProjectOpening()
        && (cmd == QLatin1String("create_project")
            || cmd == QLatin1String("open_project"))) {
        return withRequestId(errorResponse(
            cmd == QLatin1String("create_project")
                ? "CREATE_PROJECT_FAILED" : "OPEN_PROJECT_FAILED",
            "Another project is already being opened. Please try again when it finishes."));
    }

    const bool previewRequested = context.preview || args.value("preview").toBool(false);
    if (previewRequested &&
        cmd != QLatin1String("preview_actions") &&
        cmd != QLatin1String("confirm_actions") &&
        cmd != QLatin1String("apply_actions") &&
        cmd != QLatin1String("rollback_actions") &&
        cmd != QLatin1String("get_action_tx"))
    {
        QJsonObject normalizedArgs = args;
        normalizedArgs.remove("preview");

        if (isReadOnlyCommand(cmd))
        {
            if (ActionManager::instance().hasAction(cmd))
                return withRequestId(executeActionManagerCommand(
                    cmd, normalizedArgs, context.source));
            auto legacyIt = commandHandlers_.find(cmd);
            if (legacyIt != commandHandlers_.end())
            {
                try { return withRequestId(legacyIt.value()(normalizedArgs)); }
                catch (const std::exception& e) { return withRequestId(errorResponse("COMMAND_EXCEPTION", e.what())); }
                catch (...) { return withRequestId(errorResponse("COMMAND_EXCEPTION", "Command handler threw an unknown exception.")); }
            }
            return withRequestId(errorResponse("UNKNOWN_COMMAND", QString("Unknown command: %1").arg(cmd)));
        }

        if (!isPreviewSupportedCommand(cmd))
        {
            return withRequestId(errorResponse(
                "PREVIEW_NOT_SUPPORTED",
                QString("Command '%1' does not support preview mode yet").arg(cmd),
                "Use preview_actions with supported commands, or execute command directly without preview."));
        }

        QJsonArray actions;
        actions.append(QJsonObject{
            {"cmd", cmd},
            {"args", normalizedArgs}
        });

        QJsonObject previewArgs;
        previewArgs["actions"] = actions;
        previewArgs["label"] = QString("inline_preview:%1").arg(cmd);
        previewArgs["source"] = "inline_preview";
        return withRequestId(ActionManager::instance().preview(previewArgs));
    }

    if (ActionManager::instance().hasAction(cmd))
    {
        if (context.origin == DispatchOrigin::AI)
        {
            if (auto gateError = enforceAIPolicyGate(cmd, args, context, true))
                return withRequestId(*gateError);
        }
        return withRequestId(executeActionManagerCommand(cmd, args, context.source));
    }

    if (context.origin == DispatchOrigin::AI)
    {
        if (auto gateError = enforceAIPolicyGate(cmd, args, context, true))
            return withRequestId(*gateError);
    }
    return withRequestId(invokeCommandHandler(cmd, args));
}

std::optional<QJsonObject> IPCServer::enforceAIPolicyGate(const QString& cmd,
                                                          const QJsonObject& args,
                                                          const IPCDispatchContext& context,
                                                          bool consumeGrant)
{
    // The thread contract holds for every AI dispatch, including unrestricted ones.
    assertAIPolicyThread();
    if (context.unrestricted)
        return std::nullopt;
    const AIDispatchPolicy::Decision decision = aiDispatchPolicy_.evaluate({
        cmd, args, context.permissionProfile, context.toolProfile,
        context.requestId, context.grantId
    }, consumeGrant);
    if (!decision.allowed)
        return errorResponse(decision.code, decision.message);
    return std::nullopt;
}

QByteArray IPCServer::aiBindingFor(const QString& cmd, const QJsonObject& args,
                                   const IPCDispatchContext& context) const
{
    assertAIPolicyThread();
    return aiDispatchPolicy_.bindingFor({
        cmd, args, context.permissionProfile, context.toolProfile,
        context.requestId, context.grantId
    });
}

QString IPCServer::issueAIGrant(const QString& cmd, const QJsonObject& args,
                                const IPCDispatchContext& context, int ttlMs)
{
    assertAIPolicyThread();
    return aiDispatchPolicy_.issueGrant({
        cmd, args, context.permissionProfile, context.toolProfile,
        context.requestId, {}
    }, ttlMs);
}

void IPCServer::revokeAIGrants(const QString& requestId)
{
    assertAIPolicyThread();
    aiDispatchPolicy_.revokeRequest(requestId);
}

void IPCServer::assertAIPolicyThread() const
{
    Q_ASSERT(QThread::currentThread() == qApp->thread());
}

TransactionExecutionContext IPCServer::transactionContextFor(
    const IPCDispatchContext& context) const
{
    return {std::make_shared<const IPCDispatchContext>(context)};
}

IPCServer::IPCDispatchContext IPCServer::dispatchContextFrom(
    const TransactionExecutionContext& context) const
{
    if (!context.token)
        return {};
    return *std::static_pointer_cast<const IPCDispatchContext>(context.token);
}

QJsonObject IPCServer::executeActionManagerCommand(const QString& cmd, const QJsonObject& args,
                                                   ActionSource source)
{
    try
    {
        ActionResult result = ActionManager::instance().execute(
            cmd, args, source);
        if (result.success)
        {
            QJsonObject response;
            response["status"] = "ok";
            response["result"] = result.data;
            return response;
        }
        QJsonObject error = errorResponse(result.errorCode, result.errorMessage);
        if (!result.data.isEmpty())
            error[QStringLiteral("result")] = result.data;
        return error;
    }
    catch (const std::exception& e)
    {
        return errorResponse("COMMAND_EXCEPTION", e.what());
    }
    catch (...)
    {
        return errorResponse("COMMAND_EXCEPTION", "Command handler threw an unknown exception.");
    }
}

QJsonObject IPCServer::invokeCommandHandler(const QString& cmd, const QJsonObject& args)
{
    auto it = commandHandlers_.find(cmd);
    if (it == commandHandlers_.end())
    {
        return errorResponse("UNKNOWN_COMMAND",
                             QString("Unknown command: %1").arg(cmd),
                             "Check the available commands in the documentation.");
    }

    try
    {
        return it.value()(args);
    }
    catch (const std::exception& e)
    {
        return errorResponse("COMMAND_EXCEPTION", e.what());
    }
}

QStringList IPCServer::commandNames() const
{
    return commandHandlers_.keys();
}

QStringList IPCServer::allCommandNames() const
{
    QSet<QString> names;
    for (const QString& name : ActionManager::instance().actionNames())
        names.insert(name);
    for (auto it = commandHandlers_.constBegin(); it != commandHandlers_.constEnd(); ++it)
        names.insert(it.key());
    QStringList sorted(names.constBegin(), names.constEnd());
    sorted.sort();
    return sorted;
}

bool IPCServer::isJsonCommandAvailableInCurrentState(const QString& cmd) const
{
    if (appState_ != AppState::ProjectPicker)
        return true;

    static const QSet<QString> kProjectPickerCommands = {
        QStringLiteral("ping"),
        QStringLiteral("get_version"),
        QStringLiteral("get_status"),
        QStringLiteral("get_app_state"),
        QStringLiteral("open_project"),
        QStringLiteral("create_project"),
    };
    return kProjectPickerCommands.contains(cmd);
}

bool IPCServer::isBinaryCommandAvailableInCurrentState(IPCBinaryCommandType) const
{
    return appState_ == AppState::Ready;
}

QJsonObject IPCServer::commandUnavailableInCurrentStateResponse(const QString& cmd)
{
    return errorResponse("INVALID_STATE",
                         QString("Command '%1' is not available in the current application state").arg(cmd),
                         "Open or create a project before using this command.");
}

bool IPCServer::isReadOnlyCommand(const QString& cmd) const
{
    return cmd == QLatin1String("ping") ||
           cmd.startsWith(QLatin1String("get_")) ||
           cmd.startsWith(QLatin1String("list_")) ||
           cmd.startsWith(QLatin1String("can_")) ||
           cmd.startsWith(QLatin1String("link_get_"));
}

bool IPCServer::isPreviewSupportedCommand(const QString& cmd) const
{
    static const QSet<QString> kSupported = {
        QStringLiteral("set_bus_properties"),
        QStringLiteral("set_bus_lpf"),
        QStringLiteral("set_bus_hpf"),
        QStringLiteral("create_bus"),
        QStringLiteral("set_send"),
        QStringLiteral("remove_send"),
        QStringLiteral("set_send_level"),
        QStringLiteral("set_send_filter"),
        QStringLiteral("get_send_filter"),
        QStringLiteral("add_child_to_container"),
        QStringLiteral("remove_child_from_container"),
        QStringLiteral("set_container_property"),
        QStringLiteral("reorder_container_children"),
    };
    return kSupported.contains(cmd);
}


void IPCServer::sendResponse(QTcpSocket* client, const QJsonObject& response)
{
    if (!client || client->state() != QAbstractSocket::ConnectedState) return;

    QJsonObject normalized = response;
    const QString status = normalized.value("status").toString();
    if (status == "ok") {
        if (!normalized.contains("action_id")) {
            normalized["action_id"] = QString("act_%1").arg(++actionCounter_);
        }
    } else if (status == "error") {
        if (!normalized.contains("code") || !normalized.contains("message")) {
            const QJsonObject legacy = normalized.value("result").toObject();
            if (!normalized.contains("code") && legacy.contains("code")) {
                normalized["code"] = legacy.value("code");
            }
            if (!normalized.contains("message") && legacy.contains("message")) {
                normalized["message"] = legacy.value("message");
            }
        }

        if (!normalized.contains("hint")) {
            if (normalized.contains("suggestion")) {
                normalized["hint"] = normalized.value("suggestion");
            } else {
                normalized["hint"] = "Check command arguments and current application state.";
            }
        }
        if (!normalized.contains("suggestion") && normalized.contains("hint")) {
            normalized["suggestion"] = normalized.value("hint");
        }
    }

    QJsonDocument doc(normalized);
    QByteArray data = doc.toJson(QJsonDocument::Compact) + "\n";
    client->write(data);
    client->flush();
}

QJsonObject IPCServer::successResponse(const QJsonObject& result)
{
    return QJsonObject{
        {"status", "ok"},
        {"result", result}
    };
}

QJsonObject IPCServer::errorResponse(const QString& code, const QString& message,
                                      const QString& suggestion)
{
    QJsonObject response{
        {"status", "error"},
        {"code", code},
        {"message", message}
    };

    if (!suggestion.isEmpty())
    {
        response["suggestion"] = suggestion;
        response["hint"] = suggestion;
    }
    else
    {
        const QString defaultHint = "Check command arguments and current application state.";
        response["hint"] = defaultHint;
        response["suggestion"] = defaultHint;
    }

    return response;
}


void IPCServer::broadcastEvent(const QString& eventName, const QJsonObject& data)
{
    if (clients_.isEmpty())
        return;

    QJsonObject event;
    event["type"] = "event";
    event["name"] = eventName;
    event["data"] = data;

    QJsonDocument doc(event);
    QByteArray message = doc.toJson(QJsonDocument::Compact) + "\n";

    qDebug() << "Broadcasting event:" << eventName << "to" << clients_.size() << "clients";

    for (QTcpSocket* client : clients_)
    {
        if (commandOnlyClients_.contains(client))
            continue;
        if (client->state() == QAbstractSocket::ConnectedState)
        {
            client->write(message);
            client->flush();
        }
    }
}


void IPCServer::handleBinaryFrame(QTcpSocket* client, const QByteArray& frameData)
{
    if (frameData.size() < static_cast<int>(sizeof(IPCBinaryHeader))) return;

    const auto* hdr = reinterpret_cast<const IPCBinaryHeader*>(frameData.constData());
    const char* payload = frameData.constData() + sizeof(IPCBinaryHeader);
    uint32_t payloadSize = hdr->payloadSize;
    uint32_t seqId = hdr->sequenceId;

    const auto commandType = static_cast<IPCBinaryCommandType>(hdr->commandType);
    if (!isBinaryCommandAvailableInCurrentState(commandType))
    {
        sendBinaryResponse(client, IPCBinaryStatus::InvalidState, seqId, {});
        return;
    }

    switch (commandType)
    {
        case IPCBinaryCommandType::SetPosition:
        {
            if (payloadSize >= sizeof(IPCSetPositionPayload))
            {
                const auto* p = reinterpret_cast<const IPCSetPositionPayload*>(payload);
                if (engine_) engine_->setGameObjectPosition(p->objectID, p->x, p->y, p->z);
                sendBinaryResponse(client, IPCBinaryStatus::Ok, seqId, {});
            }
            break;
        }
        case IPCBinaryCommandType::SetMixerVolume:
        {
            if (payloadSize >= sizeof(IPCSetMixerVolumePayload))
            {
                const auto* p = reinterpret_cast<const IPCSetMixerVolumePayload*>(payload);
                if (engine_) engine_->setMixerVolume(harkenstudio::ObjectIdFromBytes(p->busId), p->volumeDb);
                sendBinaryResponse(client, IPCBinaryStatus::Ok, seqId, {});
            }
            break;
        }
        case IPCBinaryCommandType::SetSwitch:
        {
            sendBinaryResponse(client, IPCBinaryStatus::Error, seqId, {});
            break;
        }
        case IPCBinaryCommandType::StopAll:
        {
            if (engine_) engine_->stopAll();
            sendBinaryResponse(client, IPCBinaryStatus::Ok, seqId, {});
            break;
        }
        case IPCBinaryCommandType::GetMixerLevels:
        {
            const bool engineReady = engine_
                && engine_->isInitialized()
                && !engine_->isSimulationMode();
            std::vector<AudioEngine::DebugBusData> buses;
            const bool snapshotOk = engineReady && engine_->getDebugBuses(buses);
            QByteArray resp;
            const auto outcome = BinaryMixerLevels::encode(
                engineReady, snapshotOk, buses, resp);
            sendBinaryResponse(
                client,
                outcome == BinaryMixerLevels::Outcome::Ok
                    ? IPCBinaryStatus::Ok
                    : IPCBinaryStatus::Error,
                seqId,
                resp);
            break;
        }
        case IPCBinaryCommandType::GetVoiceSnapshot:
        {
            const bool engineReady = engine_
                && engine_->isInitialized()
                && !engine_->isSimulationMode();
            QByteArray resp;
            const auto outcome = BinaryVoiceSnapshot::encode(
                engineReady, engineReady, false, resp);
            sendBinaryResponse(
                client,
                outcome == BinaryVoiceSnapshot::Outcome::Ok
                    ? IPCBinaryStatus::Ok
                    : IPCBinaryStatus::Error,
                seqId,
                resp);
            break;
        }
        case IPCBinaryCommandType::Batch:
        {
            int offset = 0;
            while (offset + static_cast<int>(sizeof(IPCBinaryHeader)) <= static_cast<int>(payloadSize))
            {
                const auto* subHdr = reinterpret_cast<const IPCBinaryHeader*>(payload + offset);
                int subFrameSize = sizeof(IPCBinaryHeader) + subHdr->payloadSize;
                if (offset + subFrameSize > static_cast<int>(payloadSize))
                    break;
                QByteArray subFrame(payload + offset, subFrameSize);
                handleBinaryFrame(client, subFrame);
                offset += subFrameSize;
            }
            break;
        }
        default:
            sendBinaryResponse(client, IPCBinaryStatus::Error, seqId, {});
            break;
    }
}

void IPCServer::sendBinaryResponse(QTcpSocket* client, IPCBinaryStatus status,
                                    uint32_t seqId, const QByteArray& payload)
{
    if (!client) return;

    IPCBinaryResponse resp{};
    resp.magic       = IPC_BINARY_MAGIC;
    resp.version     = IPC_BINARY_VERSION;
    resp.status      = static_cast<uint16_t>(status);
    resp.payloadSize = static_cast<uint32_t>(payload.size());
    resp.sequenceId  = seqId;

    client->write(reinterpret_cast<const char*>(&resp), sizeof(resp));
    if (!payload.isEmpty())
        client->write(payload);
    client->flush();
}


void setupTransactionCallbacks(IPCServer* ipc)
{
    TransactionCallbacks cb;

    cb.commandExecutor = [ipc](const QString& cmd, const QJsonObject& args,
                               const TransactionExecutionContext& context) -> QJsonObject {
        return ipc->dispatchCommand(cmd, args, ipc->dispatchContextFrom(context));
    };

    cb.rollbackPreparer = [ipc](const QString& cmd, const QJsonObject& args,
                                QJsonObject* rollbackStep, QJsonObject* previewEntry,
                                QString* error,
                                const TransactionExecutionContext&) -> bool {
        return ipc->prepareRollbackStepForAction(cmd, args, rollbackStep, previewEntry, error);
    };

    cb.rollbackExecutor = [ipc](const QJsonObject& step, QString* error,
                                const TransactionExecutionContext& context) -> bool {
        if (step.value("kind").toString() == QLatin1String("command"))
        {
            const QString cmd = step.value("cmd").toString();
            QJsonObject args = step.value("args").toObject();
            args.remove("preview");
            const QJsonObject response = ipc->dispatchCommand(
                cmd, args, ipc->dispatchContextFrom(context));
            if (response.value("status").toString() == QLatin1String("ok"))
                return true;
            if (error)
            {
                *error = QString("Rollback command '%1' failed: [%2] %3")
                             .arg(cmd, response.value("code").toString(),
                                  response.value("message").toString());
            }
            return false;
        }
        return ipc->executeRollbackStep(step, error);
    };

    cb.isPreviewSupported = [ipc](const QString& cmd) -> bool {
        return ipc->isPreviewSupportedCommand(cmd);
    };

    cb.isKnownCommand = [ipc](const QString& cmd) -> bool {
        return ActionManager::instance().hasAction(cmd) || ipc->commandHandlers_.contains(cmd);
    };

    ActionManager::instance().setTransactionCallbacks(cb);
}

}
