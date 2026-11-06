#include "StudioSession.h"
#include "Core/AudioEngine.h"
#include "Core/AuditionManager.h"
#include "Core/EditorCommandCatalog.h"
#include "Core/EditorCommandRegistration.h"
#include "Core/IPC/IPCServer.h"
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
    std::shared_ptr<VNX::ProjectInstance> instance;
};

StudioSession::StudioSession() : state_(std::make_unique<State>())
{
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
    state_->dispatcher.waitForPendingOperations();
    QCoreApplication::sendPostedEvents(&state_->dispatcher, QEvent::MetaCall);
    state_->project.closeProject();
    state_->engine.shutdown();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QJsonObject StudioSession::open(const QString& path)
{
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
    auto prepared = VNX::ProjectManager::prepareProject(location.projectFilePath);
    if (!prepared.project)
        return failure(QStringLiteral("OPEN_PROJECT_FAILED"), prepared.error);
    prepared.instance = acquisition.instance;
    if (!state_->project.commitPreparedProject(std::move(prepared)))
        return failure(QStringLiteral("OPEN_PROJECT_FAILED"), QStringLiteral("Cannot activate the prepared project."));
    state_->instance = std::move(acquisition.instance);
    state_->project.publishProjectOpened();
    return {{QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("result"), QJsonObject{{QStringLiteral("path"), location.projectFilePath}}}};
}

QJsonObject StudioSession::describe(const QString& name) const
{
    auto description = VNX::describeEditorCommand(name, &state_->dispatcher);
    if (!description.value(QStringLiteral("registered")).toBool())
        return failure(QStringLiteral("UNKNOWN_COMMAND"), QStringLiteral("Unknown CLI command: %1").arg(name));
    return {{QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("result"), description}};
}

QJsonObject StudioSession::commands() const
{
    QJsonArray commands;
    for (const QString& name : state_->dispatcher.allCommandNames())
        commands.append(describe(name).value(QStringLiteral("result")));
    return {{QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("result"), QJsonObject{
                {QStringLiteral("protocolVersion"), 1},
                {QStringLiteral("host"), QStringLiteral("headless")},
                {QStringLiteral("commands"), commands}}}};
}

QJsonObject StudioSession::execute(const QString& name, const QJsonObject& args,
                                  int timeoutMs, const std::function<bool()>& canceled)
{
    if (canceled()) return failure(QStringLiteral("CANCELED"), QStringLiteral("Interrupted."));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    state_->engine.serviceRuntime();
    if (name == QStringLiteral("open_project"))
        return open(args.value(QStringLiteral("path")).toString());
    if (name == QStringLiteral("close_project")) {
        if (!state_->project.closeProject())
            return failure(QStringLiteral("CLOSE_PROJECT_FAILED"), QStringLiteral("Cannot close the active project."));
        state_->instance.reset();
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("result"), QJsonObject{{QStringLiteral("success"), true}}}};
    }
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
        state_->instance = std::move(acquisition.instance);
    }
    if (name == QStringLiteral("post_action") || name == QStringLiteral("post_action_3d")
        || name == QStringLiteral("trigger_action") || name == QStringLiteral("audition_play")) {
        if (name == QStringLiteral("audition_play") && state_->engine.isInitialized()
            && !state_->engine.isPlaying() && !state_->engine.startOutput())
            return failure(QStringLiteral("START_OUTPUT_FAILED"), QStringLiteral("Failed to start audio output."));
        const auto ready = waitForCurrentWorld(timeoutMs, canceled);
        if (!succeeded(ready)) return ready;
    }
    if (name == QStringLiteral("batch_convert_media")) {
        auto sources = args.value(QStringLiteral("source_ids")).toArray();
        if (sources.isEmpty()) sources = args.value(QStringLiteral("sourceIDs")).toArray();
        if (sources.isEmpty())
            return failure(QStringLiteral("HARKEN_ERR_MISSING_PARAM"), QStringLiteral("source_ids array is required."));
        QElapsedTimer timer;
        timer.start();
        QJsonArray results;
        int completed = 0;
        int failed = 0;
        QString failureCode;
        for (const auto& source : sources) {
            auto request = args;
            request.remove(QStringLiteral("source_ids"));
            request.remove(QStringLiteral("sourceIDs"));
            request.insert(QStringLiteral("source_id"), source);
            const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
            const auto response = remaining > 0
                ? execute(QStringLiteral("transcode_asset"), request, remaining, canceled)
                : failure(QStringLiteral("TIMEOUT"), QStringLiteral("The batch conversion did not complete."));
            results.append(QJsonObject{{QStringLiteral("source_id"), source},
                {QStringLiteral("response"), response}});
            if (succeeded(response)) {
                ++completed;
            } else {
                ++failed;
                const auto code = response.value(QStringLiteral("code")).toString();
                if (code == QStringLiteral("CANCELED") || code == QStringLiteral("TIMEOUT")) {
                    failureCode = code;
                    break;
                }
            }
        }
        const QJsonObject result{{QStringLiteral("success"), failed == 0},
            {QStringLiteral("succeeded"), completed}, {QStringLiteral("failed"), failed},
            {QStringLiteral("platform_id"), args.value(QStringLiteral("platform_id"))},
            {QStringLiteral("results"), results}};
        if (failed == 0)
            return {{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("result"), result}};
        auto response = failure(failureCode.isEmpty() ? QStringLiteral("BATCH_CONVERSION_FAILED") : failureCode,
            QStringLiteral("One or more sources could not be converted."));
        response.insert(QStringLiteral("result"), result);
        return response;
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
    while (true) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const auto status = state_->dispatcher.dispatchCommand(statusCommand, statusArgs, context);
        if (!succeeded(status)) return status;
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
        if (canceled() || timer.elapsed() >= timeoutMs) {
            const QString cancel = operation.value(QStringLiteral("cancel_command")).toString();
            if (!cancel.isEmpty()) state_->dispatcher.dispatchCommand(cancel, {}, context);
            return failure(canceled() ? QStringLiteral("CANCELED") : QStringLiteral("TIMEOUT"),
                           QStringLiteral("The operation did not complete."));
        }
        QThread::msleep(10);
    }
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
    if (!state_->project.isProjectOpen())
        return failure(QStringLiteral("NO_PROJECT_OPEN"), QStringLiteral("No project is open to save."));
    return state_->dispatcher.dispatchCommand(QStringLiteral("save_project"), {});
}

}
