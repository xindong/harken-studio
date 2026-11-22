#include "RemoteSession.h"

#include "AudioEngine.h"
#include "ProjectManager.h"
#include "ProjectObjectIndex.h"
#include "Monitor/EventCaptureController.h"
#include "RuntimeBridge/RemoteConnection.h"

namespace harken::studio {

RemoteSession& RemoteSession::instance()
{
    static auto* session = new RemoteSession();
    return *session;
}

harken::studio::bridge::RemoteConnection& RemoteSession::connection()
{
    ensure();
    return *connection_;
}

Monitor::EventCaptureController& RemoteSession::capture()
{
    ensure();
    return *capture_;
}

bool RemoteSession::startRecording(const QString& path)
{
    const auto target = connection().target();
    Monitor::MonitorSessionSource source;
    source.engineVersion = target.engineVersion.toStdString();
    source.remoteProtocolMajor = target.protocolMajor;
    source.engineMonitorInstanceId = target.instanceId.toULongLong(nullptr, 16);
    closeRecorderWhenStopped_ = false;
    recordedCaptureStarted_ = false;
    return recorder_.open(path.toStdString(), source, 0);
}

void RemoteSession::stopRecording()
{
    closeRecorderWhenStopped_ = true;
    closeRecorderIfStopped();
}

void RemoteSession::closeRecorderIfStopped()
{
    if (!recorder_.isOpen())
        return;
    using State = Monitor::EventCaptureState;
    const State state = capture_ ? capture_->state() : State::Unavailable;
    if (state == State::Starting || state == State::Capturing) {
        recordedCaptureStarted_ = true;
        return;
    }
    const bool failed = state == State::Failed || state == State::Unavailable;
    if (state == State::Stopping || (!closeRecorderWhenStopped_ && !recordedCaptureStarted_ && !failed))
        return;
    recorder_.close();
    closeRecorderWhenStopped_ = false;
    recordedCaptureStarted_ = false;
}

void RemoteSession::shutdown()
{
    recorder_.close();
    delete capture_;
    capture_ = nullptr;
    delete connection_;
    connection_ = nullptr;
}

void RemoteSession::ensure()
{
    if (connection_) return;
    connection_ = new harken::studio::bridge::RemoteConnection(this);
    capture_ = new Monitor::EventCaptureController(connection_, this);
    connect(connection_, &harken::studio::bridge::RemoteConnection::eventRecordsReceived, this,
            [this](const QByteArray& records) {
        if (recorder_.isOpen())
            recorder_.append(reinterpret_cast<const uint8_t*>(records.constData()),
                             static_cast<uint64_t>(records.size()));
    });
    connect(capture_, &Monitor::EventCaptureController::stateChanged, this,
            [this] { closeRecorderIfStopped(); });
    auto& project = ProjectManager::instance();
    auto& engine = AudioEngine::instance();
    const auto refreshProject = [this] {
        capture_->setProjectObjects(ProjectObjectIndex::fromProject(ProjectManager::instance().currentProject()));
    };
    const auto refreshRuntime = [this] {
        const auto& audio = AudioEngine::instance();
        capture_->setLocalRuntimeAvailable(audio.isInitialized() && !audio.isSimulationMode());
    };
    connect(&project, &ProjectManager::projectOpened, capture_, refreshProject);
    connect(&project, &ProjectManager::projectClosed, capture_, refreshProject);
    connect(&project, &ProjectManager::itemRenamed, capture_, refreshProject);
    connect(&project, &ProjectManager::itemCreated, capture_, refreshProject);
    connect(&project, &ProjectManager::itemDeleted, capture_, [this](ProjectItem* item) {
        capture_->setProjectObjects(ProjectObjectIndex::fromProject(ProjectManager::instance().currentProject(), item));
    });
    connect(&project, &ProjectManager::itemMoved, capture_, refreshProject);
    connect(&project, &ProjectManager::itemPropertyChanged, capture_, refreshProject);
    connect(&project, &ProjectManager::externalFilesModified, capture_, refreshProject);
    connect(&project, &ProjectManager::projectClosed, capture_, [this] {
        if (capture_->target() == Monitor::EventCaptureTarget::Local) capture_->stop();
    });
    connect(&engine, &AudioEngine::initializedChanged, capture_, refreshRuntime);
    refreshRuntime();
    refreshProject();
}

}
