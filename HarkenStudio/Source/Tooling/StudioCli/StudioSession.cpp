#include "StudioSession.h"
#include "Core/AudioEngine.h"
#include "Core/AuditionManager.h"
#include "Core/EditorCommandCatalog.h"
#include "Core/EditorCommandRegistration.h"
#include "Core/CommandSchemas.h"
#include "Core/CommandArgumentValidator.h"
#include "Core/IPC/IPCServer.h"
#include "Core/IPC/StudioInstanceTransport.h"
#include "Core/ProjectInstance.h"
#include "Core/ProjectLocator.h"
#include "Core/ProjectManager.h"
#include "Core/RuntimeSyncCoordinator.h"
#include "Core/SchemaArchive.h"

#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QThread>
#include <QTimer>

namespace HarkenStudioCli {

QJsonObject failure(const QString& code, const QString& message)
{
    return {{QStringLiteral("status"), QStringLiteral("error")},
            {QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
}

bool succeeded(const QJsonObject& response)
{
    return response.value(QStringLiteral("status")) == QStringLiteral("ok");
}

struct StudioSession::State {
    VNX::ProjectManager& project = VNX::ProjectManager::instance();
    VNX::AudioEngine& engine = VNX::AudioEngine::instance();
    HarkenEditor::AuditionManager audition{&engine};
    VNX::IPCServer dispatcher{&engine, nullptr, VNX::CommandHost::Cli};
};

StudioSession::StudioSession(const QString& instanceId, int timeoutMs, std::function<bool()> canceled)
    : timeoutMs_(timeoutMs), canceled_(std::move(canceled))
{
    if (!instanceId.isEmpty()) {
        instance_ = std::make_unique<StudioInstanceClient>(instanceId);
        return;
    }
    state_ = std::make_unique<State>();
    QObject::connect(&state_->project, &VNX::ProjectManager::errorOccurred,
        &state_->dispatcher, [](const QString& message) { qWarning().noquote() << message; });
    QObject::connect(&state_->project, &VNX::ProjectManager::warningOccurred,
        &state_->dispatcher, [](const QString& message) { qWarning().noquote() << message; });
    state_->dispatcher.setProjectManager(&state_->project);
    state_->dispatcher.setAppState(VNX::IPCServer::AppState::Ready);
    VNX::registerEditorCommands({&state_->engine, &state_->project,
        &state_->dispatcher, &state_->audition});
}

StudioSession::~StudioSession()
{
    if (!state_) return;
    state_->dispatcher.waitForPendingOperations();
    QCoreApplication::sendPostedEvents(&state_->dispatcher, QEvent::MetaCall);
    if (state_->project.isProjectOpen()) state_->project.closeProject();
    state_->engine.shutdown();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QJsonObject StudioSession::open(const QString& path, int timeoutMs, const std::function<bool()>& canceled)
{
    if (instance_) return instance_->execute(QStringLiteral("open_project"),
        {{QStringLiteral("path"), path}}, timeoutMs, canceled);
    if (path.trimmed().isEmpty())
        return failure(QStringLiteral("INVALID_ARGS"), QStringLiteral("Project path is required."));
    const auto location = VNX::ProjectLocator::locate(path);
    if (!location.ok())
        return failure(VNX::projectIssueCode(location.issue), QStringLiteral("Cannot locate a unique Harken project: %1").arg(path));
    auto acquisition = VNX::ProjectInstance::acquire(location.projectFilePath,
        VNX::ProjectInstance::ActivationPolicy::Deny);
    if (acquisition.status != VNX::ProjectInstance::Status::Acquired)
        return failure(QStringLiteral("PROJECT_IN_USE"), acquisition.error);
    QString error;
    if (!VNX::SchemaArchive::instance().verifyBuildSnapshot(&error))
        return failure(QStringLiteral("SCHEMA_UNAVAILABLE"), error);
    auto prepared = VNX::ProjectManager::prepareProject(location.projectFilePath, false, {}, {}, canceled);
    if (canceled()) return failure(QStringLiteral("CANCELED"), QStringLiteral("Interrupted while opening the project."));
    if (!prepared.project)
        return failure(QStringLiteral("OPEN_PROJECT_FAILED"), prepared.error);
    prepared.instance = acquisition.instance;
    if (state_->project.isProjectOpen() && !state_->project.closeProject())
        return failure(QStringLiteral("CLOSE_PROJECT_FAILED"), QStringLiteral("Cannot close the active project."));
    const auto closed = waitForProjectClose(timeoutMs, canceled);
    if (!succeeded(closed)) return closed;
    if (!state_->project.commitPreparedProject(std::move(prepared)))
        return failure(QStringLiteral("OPEN_PROJECT_FAILED"), QStringLiteral("Cannot activate the prepared project."));
    state_->project.startWatchingCurrentProject();
    state_->project.publishProjectOpened();
    return state_->dispatcher.dispatchCommand(QStringLiteral("get_project_info"), {});
}

QJsonObject StudioSession::describe(const QString& name) const
{
    if (instance_) return instance_->describe(name, timeoutMs_, canceled_);
    return state_->dispatcher.dispatchCommand(QStringLiteral("get_command_schema"),
        {{QStringLiteral("command"), name}});
}

QJsonObject StudioSession::commands() const
{
    if (instance_) return instance_->commands(timeoutMs_, canceled_);
    return state_->dispatcher.dispatchCommand(QStringLiteral("list_commands"), {});
}

QJsonObject StudioSession::validate(const QString& name, const QJsonObject& args) const
{
    if (instance_) return instance_->validate(name, args, timeoutMs_, canceled_);
    if (!state_->dispatcher.allCommandNames().contains(name))
        return failure(QStringLiteral("UNKNOWN_COMMAND"),
            QStringLiteral("Unknown command '%1'. Run HarkenStudioCLI commands to discover available commands.").arg(name));
    const auto schema = VNX::commandInputSchema(name);
    if (schema.isEmpty())
        return failure(QStringLiteral("COMMAND_CONTRACT_MISSING"),
            QStringLiteral("Command '%1' has no argument contract.").arg(name));
    QString error;
    if (!VNX::validateCommandArguments(args, schema, &error)) {
        auto response = failure(QStringLiteral("INVALID_ARGUMENTS"), error);
        response.insert(QStringLiteral("command"), name);
        response.insert(QStringLiteral("suggestion"),
            QStringLiteral("Run HarkenStudioCLI commands %1 for parameter requirements.").arg(name));
        return response;
    }
    return {{QStringLiteral("status"), QStringLiteral("ok")}};
}

QJsonObject StudioSession::execute(const QString& name, const QJsonObject& args,
                                  int timeoutMs, const std::function<bool()>& canceled)
{
    if (instance_) {
        auto response = instance_->execute(name, args, timeoutMs, canceled);
        if (succeeded(response)
            && (name == QStringLiteral("close_project") || name == QStringLiteral("create_project"))) {
            const auto closed = waitForProjectClose(timeoutMs, canceled);
            if (!succeeded(closed)) return closed;
            auto result = response.value(QStringLiteral("result")).toObject();
            result.insert(QStringLiteral("closing"), false);
            response.insert(QStringLiteral("result"), result);
        }
        return finishOperation(std::move(response), timeoutMs, canceled);
    }
    if (canceled()) return failure(QStringLiteral("CANCELED"), QStringLiteral("Interrupted."));
    const auto validated = validate(name, args);
    if (!succeeded(validated)) return validated;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    state_->engine.serviceRuntime();
    const auto settled = waitForProjectClose(timeoutMs, canceled);
    if (!succeeded(settled)) return settled;
    if (name == QStringLiteral("open_project"))
        return open(args.value(QStringLiteral("path")).toString(), timeoutMs, canceled);
    std::shared_ptr<VNX::ProjectInstance> creationInstance;
    if (name == QStringLiteral("create_project")) {
        QString schemaError;
        if (!VNX::SchemaArchive::instance().verifyBuildSnapshot(&schemaError))
            return failure(QStringLiteral("SCHEMA_UNAVAILABLE"), schemaError);
        const QString projectName = args.value(QStringLiteral("name")).toString();
        const QString location = args.value(QStringLiteral("location")).toString();
        if (projectName.isEmpty() || location.isEmpty())
            return failure(QStringLiteral("INVALID_ARGS"), QStringLiteral("name and location are required."));
        if (projectName == QStringLiteral(".") || projectName == QStringLiteral("..")
            || projectName.contains('/') || projectName.contains('\\'))
            return failure(QStringLiteral("INVALID_ARGS"), QStringLiteral("name must be a single directory name."));
        const QString root = QDir(location).filePath(projectName);
        if (QFileInfo::exists(root))
            return failure(QStringLiteral("PROJECT_EXISTS"), QStringLiteral("The project directory already exists: %1").arg(root));
        auto acquisition = VNX::ProjectInstance::acquire(QDir(root).filePath(projectName + QStringLiteral(".hsp")),
            VNX::ProjectInstance::ActivationPolicy::Deny);
        if (acquisition.status != VNX::ProjectInstance::Status::Acquired)
            return failure(QStringLiteral("PROJECT_IN_USE"), acquisition.error);
        creationInstance = std::move(acquisition.instance);
    }
    if (name == QStringLiteral("post_action") || name == QStringLiteral("post_action_3d")
        || name == QStringLiteral("audition_play")) {
        if (name == QStringLiteral("audition_play") && state_->engine.isInitialized()
            && !state_->engine.isPlaying() && !state_->engine.startOutput())
            return failure(QStringLiteral("START_OUTPUT_FAILED"), QStringLiteral("Failed to start audio output."));
        const auto ready = waitForCurrentWorld(timeoutMs, canceled);
        if (!succeeded(ready)) return ready;
    }
    VNX::IPCServer::IPCDispatchContext context;
    context.source = VNX::ActionSource::IPC;
    context.origin = VNX::IPCServer::DispatchOrigin::IPC;
    if (name == QStringLiteral("audition_set_position")) {
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QTimer cancellation;
        quint64 requestId = 0;
        QJsonObject completed;
        QObject::connect(&state_->audition,
            &HarkenEditor::AuditionManager::auditionSeekFinished, &loop,
            [&](uint32_t playingId, quint64 finishedId, int32_t engineResult, bool applied) {
                if (finishedId != requestId || !completed.isEmpty()) return;
                const QJsonObject result{
                    {QStringLiteral("auditionID"), static_cast<qint64>(playingId)},
                    {QStringLiteral("audition_seek_request_id"), QString::number(finishedId)},
                    {QStringLiteral("samplePosition"), args.value(QStringLiteral("samplePosition"))},
                    {QStringLiteral("engine_result"), engineResult},
                    {QStringLiteral("state"), applied ? QStringLiteral("applied") : QStringLiteral("failed")}};
                completed = applied
                    ? QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}}
                    : failure(QStringLiteral("SEEK_FAILED"), QStringLiteral("The audition seek was not applied."));
                completed.insert(QStringLiteral("result"), result);
                loop.quit();
            });
        const auto accepted = state_->dispatcher.dispatchCommand(name, args, context);
        if (!succeeded(accepted)) return accepted;
        requestId = accepted.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("audition_seek_request_id")).toString().toULongLong();
        if (requestId == 0)
            return failure(QStringLiteral("SEEK_FAILED"), QStringLiteral("The audition seek has no completion request."));
        QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
            completed = failure(QStringLiteral("TIMEOUT"), QStringLiteral("The audition seek did not complete."));
            loop.quit();
        });
        QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
            if (!canceled()) return;
            completed = failure(QStringLiteral("CANCELED"), QStringLiteral("Interrupted while seeking."));
            loop.quit();
        });
        deadline.start(timeoutMs);
        cancellation.start(10);
        loop.exec();
        return completed;
    }
    auto response = state_->dispatcher.dispatchCommand(name, args, context);
    if (name == QStringLiteral("close_project") && succeeded(response)) {
        const auto closed = waitForProjectClose(timeoutMs, canceled);
        if (!succeeded(closed)) return closed;
        auto result = response.value(QStringLiteral("result")).toObject();
        result.insert(QStringLiteral("closing"), false);
        response.insert(QStringLiteral("result"), result);
    }
    if (name == QStringLiteral("create_project") && succeeded(response)) {
        const auto closed = waitForProjectClose(timeoutMs, canceled);
        if (!succeeded(closed)) return closed;
        const auto* project = state_->project.currentProject();
        if (!project)
            return failure(QStringLiteral("CREATE_PROJECT_FAILED"), QStringLiteral("The created project could not be opened."));
        auto result = response.value(QStringLiteral("result")).toObject();
        result.insert(QStringLiteral("path"), project->projectPath);
        result.insert(QStringLiteral("closing"), false);
        response.insert(QStringLiteral("result"), result);
    }
    if (name == QStringLiteral("load_pack") || name == QStringLiteral("unload_pack")) {
        const auto backpressured = [&] {
            return !succeeded(response)
                && response.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("runtime_result")).toInteger()
                    == static_cast<qint32>(HarkenEditor::Bridge::kEngineBackpressure);
        };
        if (backpressured()) {
            QEventLoop loop;
            QTimer retry;
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&retry, &QTimer::timeout, &loop, [&] {
                if (canceled())
                    response = failure(QStringLiteral("CANCELED"), QStringLiteral("Interrupted while applying pack changes."));
                else
                    response = state_->dispatcher.dispatchCommand(name, args, context);
                if (!backpressured()) loop.quit();
            });
            QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
                response = failure(QStringLiteral("TIMEOUT"), QStringLiteral("The runtime did not finish applying pack changes."));
                loop.quit();
            });
            deadline.start(timeoutMs);
            retry.start(10);
            loop.exec();
        }
    }
    return finishOperation(std::move(response), timeoutMs, canceled);
}

QJsonObject StudioSession::finishOperation(QJsonObject response, int timeoutMs,
                                          const std::function<bool()>& canceled)
{
    if (!succeeded(response)) return response;
    const auto result = response.value(QStringLiteral("result")).toObject();
    if (result.value(QStringLiteral("success")).isBool()
        && !result.value(QStringLiteral("success")).toBool()) {
        response.insert(QStringLiteral("status"), QStringLiteral("error"));
        response.insert(QStringLiteral("code"), QStringLiteral("COMMAND_FAILED"));
        return response;
    }
    if (!result.value(QStringLiteral("async")).toBool()) return response;
    const auto operation = result.value(QStringLiteral("operation")).toObject();
    const QString statusCommand = operation.value(QStringLiteral("status_command")).toString();
    const auto statusArgs = operation.value(QStringLiteral("status_args")).toObject();
    if (statusCommand.isEmpty())
        return failure(QStringLiteral("ASYNC_PROTOCOL_REQUIRED"), QStringLiteral("The command has no discoverable completion query."));
    QElapsedTimer timer;
    timer.start();
    VNX::IPCServer::IPCDispatchContext context;
    context.source = VNX::ActionSource::IPC;
    context.origin = VNX::IPCServer::DispatchOrigin::IPC;
    const auto unfinished = [&](QJsonObject error) {
        if (instance_) {
            error.insert(QStringLiteral("result"), result);
            error.insert(QStringLiteral("suggestion"), QStringLiteral(
                "The operation may still complete in Studio. Query %1 with the returned operation.status_args before retrying.")
                    .arg(statusCommand));
        }
        return error;
    };
    while (true) {
        if (canceled() || timer.elapsed() >= timeoutMs) {
            if (!instance_) {
                const QString cancel = operation.value(QStringLiteral("cancel_command")).toString();
                if (!cancel.isEmpty())
                    state_->dispatcher.dispatchCommand(cancel, operation.value(QStringLiteral("cancel_args")).toObject(), context);
                state_->dispatcher.waitForPendingOperations();
                QCoreApplication::sendPostedEvents(&state_->dispatcher, QEvent::MetaCall);
            }
            return unfinished(failure(canceled() ? QStringLiteral("CANCELED") : QStringLiteral("TIMEOUT"),
                QStringLiteral("The operation did not complete before waiting stopped.")));
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const auto status = instance_
            ? instance_->execute(statusCommand, statusArgs,
                qMax(1, timeoutMs - static_cast<int>(timer.elapsed())), canceled)
            : state_->dispatcher.dispatchCommand(statusCommand, statusArgs, context);
        if (!succeeded(status)) return unfinished(status);
        const auto progress = status.value(QStringLiteral("result")).toObject();
        if (!progress.value(QStringLiteral("in_progress")).toBool()) {
            const auto finished = progress.value(QStringLiteral("result")).toObject();
            if (finished.isEmpty())
                return failure(QStringLiteral("CANCELED"), QStringLiteral("The operation ended without a completed result."));
            auto completed = finished;
            if (succeeded(completed)
                && completed.value(QStringLiteral("result")).toObject().value(QStringLiteral("success")) == false) {
                completed.insert(QStringLiteral("status"), QStringLiteral("error"));
                completed.insert(QStringLiteral("code"), QStringLiteral("COMMAND_FAILED"));
                completed.insert(QStringLiteral("message"), QStringLiteral("One or more operation targets failed."));
            }
            return completed;
        }
        QThread::msleep(10);
    }
}

QJsonObject StudioSession::waitForProjectClose(int timeoutMs, const std::function<bool()>& canceled)
{
    QElapsedTimer timer;
    timer.start();
    while (true) {
        if (instance_) {
            const auto info = instance_->execute(QStringLiteral("get_project_info"), {},
                qMax(1, timeoutMs - static_cast<int>(timer.elapsed())), canceled);
            if (!succeeded(info)) return info;
            if (!info.value(QStringLiteral("result")).toObject().value(QStringLiteral("closing")).toBool()) break;
        } else if (!state_->project.isClosing()) {
            break;
        }
        if (canceled())
            return failure(QStringLiteral("CANCELED"), QStringLiteral("Interrupted while closing the project."));
        if (timer.elapsed() >= timeoutMs)
            return failure(QStringLiteral("TIMEOUT"), QStringLiteral("The project did not finish closing."));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (state_) state_->engine.serviceRuntime();
        if (instance_ || state_->project.isClosing()) QThread::msleep(10);
    }
    return {{QStringLiteral("status"), QStringLiteral("ok")}};
}

QJsonObject StudioSession::waitForCurrentWorld(int timeoutMs, const std::function<bool()>& canceled)
{
    if (!state_->project.isProjectOpen() || !state_->engine.isInitialized()
        || state_->engine.isSimulationMode())
        return {{QStringLiteral("status"), QStringLiteral("ok")}};

    auto& coordinator = state_->engine.runtimeSyncCoordinator();
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QTimer cancellation;
    QJsonObject response;
    const auto check = [&] {
        if (!response.isEmpty()) return;
        if (canceled()) {
            response = failure(QStringLiteral("CANCELED"), QStringLiteral("Interrupted while preparing playback."));
        } else {
            using Barrier = VNX::RuntimeSyncCoordinator::RevisionBarrierState;
            const auto barrier = coordinator.barrier(coordinator.currentAuthoringRevision());
            if (barrier.state == Barrier::Ready) {
                response = {{QStringLiteral("status"), QStringLiteral("ok")}};
            } else if (barrier.state != Barrier::Pending) {
                response = failure(QStringLiteral("RUNTIME_SYNC_FAILED"),
                    QStringLiteral("The current project could not be prepared for playback."));
                response.insert(QStringLiteral("result"), QJsonObject{
                    {QStringLiteral("runtime_sync_status"), static_cast<int>(barrier.failure)}});
            }
        }
        if (!response.isEmpty()) loop.quit();
    };
    QObject::connect(&coordinator, &VNX::RuntimeSyncCoordinator::barrierChanged, &loop, check);
    QObject::connect(&cancellation, &QTimer::timeout, &loop, check);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        if (!response.isEmpty()) return;
        response = failure(QStringLiteral("TIMEOUT"), QStringLiteral("The current project did not become ready for playback."));
        loop.quit();
    });
    coordinator.flushPendingMutationNow();
    check();
    if (response.isEmpty()) {
        deadline.start(timeoutMs);
        cancellation.start(10);
        loop.exec();
    }
    return response;
}

QJsonObject StudioSession::save()
{
    if (instance_) return instance_->execute(QStringLiteral("save_project"), {}, timeoutMs_, canceled_);
    if (!state_->project.isProjectOpen())
        return failure(QStringLiteral("NO_PROJECT_OPEN"), QStringLiteral("No project is open to save."));
    return state_->dispatcher.dispatchCommand(QStringLiteral("save_project"), {});
}

}
