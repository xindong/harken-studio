#include "Launch/ProjectCreateFlow.h"
#include "Versions/LocalBuildRegistry.h"

#include "Launch/StudioLaunchMonitor.h"
#include "Launch/StudioLauncher.h"
#include "Platform/PlatformPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QTimer>
#include <QWidget>

#include <algorithm>

namespace VNH {

namespace {

QString hubText(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

bool flowActive = false;
constexpr int kHandshakeTimeoutMs = 15000;
constexpr int kCreationTimeoutMs = 120000;

class ProjectCreateSession : public QObject
{
public:
    ProjectCreateSession(ProjectCreateRequest request, QWidget* parent,
                         std::function<void(const ProjectCreateOutcome&)> completed)
        : QObject(parent ? static_cast<QObject*>(parent) : QCoreApplication::instance()),
          request_(std::move(request)), parent_(parent), completed_(std::move(completed))
    {
        flowActive = true;
        progress_ = new QProgressDialog(hubText("Studio is creating the project."),
                                       hubText("Hide"), 0, 0, parent);
        progress_->setWindowTitle(hubText("New Project"));
        progress_->setWindowModality(Qt::NonModal);
        progress_->setMinimumDuration(0);
        progress_->setAutoClose(false);
        connect(&monitor_, &StudioLaunchMonitor::ready, this, [this]() {
            const QString path = QDir(request_.location).filePath(
                request_.name + QStringLiteral("/") + request_.name + QStringLiteral(".hsp"));
            if (!QFileInfo(path).isFile()) {
                fail(hubText("HarkenStudio reported success, but the project file was not found."));
                return;
            }
            finish(ProjectCreateOutcome::Result::Created, QFileInfo(path).canonicalFilePath());
        }, Qt::QueuedConnection);
        connect(&monitor_, &StudioLaunchMonitor::ready, &liveness_, &QTimer::stop);
        connect(&monitor_, &StudioLaunchMonitor::failed, &liveness_, &QTimer::stop);
        connect(&monitor_, &StudioLaunchMonitor::failed, this, [this](const QString& error) {
            if (monitor_.failureCode().isEmpty() && launcher_.stillRunning())
                unknown(hubText("%1\n\nThe project creation result is unknown.").arg(error));
            else
                fail(error);
        }, Qt::QueuedConnection);
        connect(&liveness_, &QTimer::timeout, this, [this]() {
            if (!launcher_.stillRunning()) {
                if (unmonitored_)
                    finish(ProjectCreateOutcome::Result::Unknown);
                else
                    fail(hubText("HarkenStudio exited before it finished creating the project"));
            }
        });
    }

    ~ProjectCreateSession() override
    {
        flowActive = false;
        delete progress_.data();
    }

    void start()
    {
        if (!monitor_.listen()) {
            fail(hubText("Cannot open the project creation progress channel."));
            return;
        }
        const auto& version = request_.version;
        monitor_.setExpectedBuildId(version.expectedBuildId());
        monitor_.setExpectedInstallPath(version.installPath);
        monitor_.setExpectedOperation(QStringLiteral("createProject"));
        monitor_.setExpectedLaunchIntent(launchIntentName(LaunchIntent::DirectOpenOnly));

        StudioCreateSpec spec;
        spec.launch.installPath = version.installPath;
        spec.launch.executablePath = version.executablePath;
        spec.launch.serverName = monitor_.serverName();
        spec.launch.sessionToken = monitor_.sessionToken();
        spec.launch.expectedBuildId = version.expectedBuildId();
        spec.launch.capabilitiesVersion = version.capabilitiesVersion;
        spec.projectName = request_.name;
        spec.projectLocation = request_.location;
        spec.templateId = request_.templateId;
        spec.templatePath = request_.templatePath;
        QString error;
        if (!launcher_.startCreate(spec, &error)) {
            fail(error);
            return;
        }
        monitor_.setExpectedProcess(launcher_.pid(), version.executablePath.isEmpty()
            ? QDir(version.installPath).filePath(studioBinaryRelativePath())
            : version.executablePath);
        progress_->show();
        liveness_.start(1000);
        QTimer::singleShot(kHandshakeTimeoutMs, this, [this]() {
            if (!monitor_.helloAccepted())
                unknown(hubText("Studio has not connected to Hub. Continue in Studio; the creation result is not yet known."));
        });
        QTimer::singleShot(kCreationTimeoutMs, this, [this]() {
            if (!finished_ && !unmonitored_ && progress_)
                progress_->setLabelText(hubText("Studio is still creating the project. You can continue working in Hub."));
        });
    }

private:
    void fail(const QString& reason)
    {
        if (finished_)
            return;
        const auto parent = parent_;
        finish(ProjectCreateOutcome::Result::Failed);
        QMessageBox::warning(parent, hubText("New Project"), reason);
    }

    void unknown(const QString& reason)
    {
        if (finished_ || unmonitored_)
            return;
        unmonitored_ = true;
        monitor_.close();
        liveness_.start(1000);
        progress_->hide();
        QMessageBox::information(parent_, hubText("New Project"), reason);
    }

    void finish(ProjectCreateOutcome::Result result, const QString& path = {})
    {
        if (finished_)
            return;
        finished_ = true;
        liveness_.stop();
        monitor_.close();
        progress_->hide();
        const auto completed = std::move(completed_);
        deleteLater();
        if (completed)
            completed({result, path});
    }

    ProjectCreateRequest request_;
    QPointer<QWidget> parent_;
    std::function<void(const ProjectCreateOutcome&)> completed_;
    QPointer<QProgressDialog> progress_;
    StudioLaunchMonitor monitor_;
    StudioLauncher launcher_;
    QTimer liveness_;
    bool unmonitored_ = false;
    bool finished_ = false;
};

}

ProjectCreateOutcome ProjectCreateFlow::create(
    const ProjectCreateRequest& request, QWidget* dialogParent,
    std::function<void(const ProjectCreateOutcome&)> onFinished)
{
    if (flowActive)
        return {ProjectCreateOutcome::Result::Busy, QString()};

    InstalledVersion selectedVersion = request.version;
    if (selectedVersion.capabilitiesVersion != 1
        && selectedVersion.capabilitiesVersion != kStudioCapabilitiesVersion) {
        QMessageBox::information(dialogParent, hubText("New Project"),
            hubText("This Studio can open existing projects, but project creation from Hub is unavailable."));
        return {ProjectCreateOutcome::Result::Failed, QString()};
    }
    if (selectedVersion.isLocalBuild()) {
        const std::vector<InstalledVersion> localBuilds =
            VersionScanner::scanLocalBuilds();
        const auto current = std::find_if(
            localBuilds.begin(), localBuilds.end(),
            [&selectedVersion](const InstalledVersion& candidate) {
                return candidate.buildId == selectedVersion.buildId;
            });
        if (current == localBuilds.end() || !current->available
            || current->executableFingerprint.isEmpty()
            || LocalBuildRegistry::fileFingerprint(current->executablePath) != current->executableFingerprint) {
            QString error;
            if (current != localBuilds.end() && current->available)
                LocalBuildRegistry::requestRecheck(current->executablePath, current->executableFingerprint, &error);
            QMessageBox::warning(
                dialogParent, hubText("New Project"),
                error.isEmpty() ? hubText("This local build changed and is being checked again. "
                                         "Try again shortly.") : error);
            return {ProjectCreateOutcome::Result::Failed, QString()};
        }
        selectedVersion = *current;
    }

    ProjectCreateRequest checked = request;
    checked.version = selectedVersion;
    auto* session = new ProjectCreateSession(std::move(checked), dialogParent, std::move(onFinished));
    QTimer::singleShot(0, session, &ProjectCreateSession::start);
    return {ProjectCreateOutcome::Result::Started, QString()};
}

}
