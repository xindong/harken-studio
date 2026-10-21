#include "StartupCoordinator.h"

#include "Core/ProjectTemplateLoader.h"
#include "Core/ProjectInstance.h"
#include "Core/ProjectOpenApproval.h"
#include "Core/ProjectUpgradeManager.h"
#include "Core/ProjectUpgradeMessages.h"
#include "Dialogs/StartupLoadingWindow.h"
#include "MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent>

namespace VNX {

StartupCoordinator::StartupCoordinator(
    QApplication& application,
    StartupLoadingWindow& loading,
    HubClient::Arguments hubArguments,
    QString projectFile,
    QObject* parent)
    : QObject(parent),
      application_(application),
      loading_(loading),
      hubArguments_(std::move(hubArguments)),
      projectFile_(std::move(projectFile))
{
    connect(&analysisWatcher_,
            &QFutureWatcher<ProjectUpgradeAnalysis>::finished,
            this, &StartupCoordinator::finishProjectAnalysis);
    connect(&preparationWatcher_,
            &QFutureWatcher<std::shared_ptr<PreparedProject>>::finished,
            this, [this]() {
        prepared_ = preparationWatcher_.result();
        projectPrepared_ = true;
        tryCommit();
    });
}

StartupCoordinator::~StartupCoordinator()
{
    if (analysisWatcher_.isRunning())
        analysisWatcher_.waitForFinished();
    if (preparationWatcher_.isRunning())
        preparationWatcher_.waitForFinished();
}

void StartupCoordinator::start()
{
    loading_.setPhase(StartupPhase::PreparingApplication);
    if (hubArguments_.present) {
        hubClient_ = std::make_unique<HubClient>(hubArguments_);
        connect(hubClient_.get(), &HubClient::helloCompleted,
                this, [this](bool connected) {
            if (connected)
                hubClient_->reportStage(QStringLiteral("processStarted"));
            startAfterHubConnection();
        });
        hubClient_->connectAndHelloAsync();
        return;
    }
    startAfterHubConnection();
}

void StartupCoordinator::startAfterHubConnection()
{
    ProjectInstance::setActivationWindow(&loading_);
    const QString instancePath = hubArguments_.createProject
        ? QDir(QDir(hubArguments_.projectLocation.trimmed())
                   .filePath(hubArguments_.projectName.trimmed()))
              .filePath(hubArguments_.projectName.trimmed() + QStringLiteral(".hsp"))
        : projectFile_;
    auto acquisition = ProjectInstance::acquire(instancePath);
    if (acquisition.status == ProjectInstance::Status::Failed) {
        fail(acquisition.error, 3, QStringLiteral("PROJECT_INSTANCE_UNAVAILABLE"));
        return;
    }
    if (acquisition.status == ProjectInstance::Status::Forwarded) {
        if (hubArguments_.createProject) {
            fail(tr("This project is already open in another Studio window."),
                 3, QStringLiteral("PROJECT_ALREADY_OPEN"));
            return;
        }
        if (hubClient_) {
            connect(hubClient_.get(), &HubClient::forwardedCompleted,
                    this, [this](bool acknowledged) {
                if (!acknowledged) {
                    fail(tr("The project window was activated, but Harken Studio Hub did not confirm the request."),
                         3, QStringLiteral("PROJECT_FORWARD_UNCONFIRMED"));
                    return;
                }
                terminal_ = true;
                loading_.close();
                application_.exit(0);
            });
            hubClient_->reportForwarded();
        } else {
            terminal_ = true;
            loading_.close();
            application_.exit(0);
        }
        return;
    }
    projectInstance_ = std::move(acquisition.instance);
    startProjectPreparation();
    if (!terminal_)
        startMainWindowInitialization();
}

void StartupCoordinator::startProjectPreparation()
{
    loading_.setPhase(hubArguments_.createProject
                          ? StartupPhase::CreatingWorkspace
                          : StartupPhase::ReadingProject);
    if (hubArguments_.createProject) {
        ProjectConfig config;
        config.name = hubArguments_.projectName.trimmed();
        config.location = hubArguments_.projectLocation.trimmed();
        if (!hubArguments_.projectTemplatePath.isEmpty()) {
            ValidationResult validation;
            const auto projectTemplate = ProjectTemplateLoader::loadFile(
                hubArguments_.projectTemplatePath, &validation);
            if (!projectTemplate) {
                fail(validation.toUserText());
                return;
            }
            config.templateId = hubArguments_.projectTemplateId.isEmpty()
                                    ? projectTemplate->id
                                    : hubArguments_.projectTemplateId;
            config.templateData = projectTemplate->toJson();
        }
        preparationWatcher_.setFuture(QtConcurrent::run(
            [config]() {
                return std::make_shared<PreparedProject>(
                    ProjectManager::prepareProject(config));
            }));
        return;
    }

    if (projectFile_.isEmpty()) {
        preparationWatcher_.setFuture(QtConcurrent::run([]() {
            return std::make_shared<PreparedProject>(
                ProjectManager::prepareProject(QString()));
        }));
        return;
    }

    const QString projectFile = projectFile_;
    const bool upgradeApprovedByHub =
        hubArguments_.launchIntent == QStringLiteral("UpgradeApprovedByHub");
    const ProjectUpgradeProgressCallback progress = upgradeProgressCallback();
    analysisWatcher_.setFuture(QtConcurrent::run(
        [projectFile, upgradeApprovedByHub, progress]() {
            ProjectUpgradeManager manager;
            return upgradeApprovedByHub
                ? manager.analyzeForProjectOpen(projectFile, progress)
                : manager.analyze(projectFile, progress);
        }));
}

void StartupCoordinator::finishProjectAnalysis()
{
    if (terminal_)
        return;
    analysis_ = analysisWatcher_.result();
    if (!analysis_.ok) {
        fail(ProjectUpgradeMessages::userMessage(analysis_.issue, analysis_.detail),
             3, projectIssueCode(analysis_.issue),
             analysis_.blockingErrors.join(QLatin1Char('\n')));
        return;
    }
    std::optional<ProjectOpenApprovalBinding> suppliedApproval;
    if (hubArguments_.launchIntent == QStringLiteral("UpgradeApprovedByHub"))
        suppliedApproval = ProjectOpenApprovalBinding{
            hubArguments_.projectOpenRequestId, hubArguments_.projectOpenApproval,
            hubArguments_.expectedBuildId};
    const ProjectOpenDecision decision = confirmProjectOpen(analysis_, &loading_, suppliedApproval);
    if (decision.cancelled) {
        cancelProjectOpen();
        return;
    }
    if (!decision.proceed) {
        fail(ProjectUpgradeMessages::userMessage(decision.issue),
             3, projectIssueCode(decision.issue));
        return;
    }
    upgradeApproval_ = decision.upgradeApproval;
    startExistingProjectPreparation();
}

void StartupCoordinator::startExistingProjectPreparation()
{
    const QString projectFile = projectFile_;
    const ProjectUpgradeApprovalCallback approval = upgradeApproval_;
    const ProjectUpgradeProgressCallback progress = upgradeProgressCallback();
    preparationWatcher_.setFuture(QtConcurrent::run(
        [projectFile, approval, progress]() {
            return std::make_shared<PreparedProject>(
                ProjectManager::prepareProject(projectFile, static_cast<bool>(approval),
                                               progress, approval));
        }));
}

ProjectUpgradeProgressCallback StartupCoordinator::upgradeProgressCallback()
{
    const QPointer<StartupCoordinator> self(this);
    return [self](const ProjectUpgradeProgress& progress) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, progress]() {
                if (self)
                    self->showUpgradeProgress(progress);
            },
            Qt::QueuedConnection);
    };
}

void StartupCoordinator::showUpgradeProgress(
    const ProjectUpgradeProgress& progress)
{
    const int upgradePercent = ProjectUpgradeMessages::progressPercent(progress);
    const int startupPercent = 48 + (upgradePercent * 16) / 100;
    loading_.setProgress(startupPercent,
                         ProjectUpgradeMessages::progressText(progress.phase));
}

void StartupCoordinator::startMainWindowInitialization()
{
    mainWindow_ = std::make_unique<MainWindow>(
        nullptr, std::function<void(StartupPhase)>(),
        MainWindow::InitializationMode::Deferred);
    mainWindow_->initializeDeferred(
        [this](StartupPhase phase) { loading_.setPhase(phase); },
        [this]() {
            windowInitialized_ = true;
            tryCommit();
        });
}

void StartupCoordinator::tryCommit()
{
    if (terminal_ || !projectPrepared_ || !windowInitialized_)
        return;
    if (!prepared_ || !prepared_->project) {
        if (!prepared_ || prepared_->error.isEmpty()) {
            fail(tr("Harken Studio could not open the project"));
        } else {
            const QString message = ProjectUpgradeMessages::userMessage(
                prepared_->error, !hubArguments_.createProject);
            const QString writeAccessCode = projectIssueCode(ProjectIssue::ProjectUpgradeFilesNotWritable);
            if (prepared_->error.startsWith(writeAccessCode + QLatin1Char(':')))
                fail(message, 3, writeAccessCode, prepared_->error.mid(writeAccessCode.size() + 1));
            else
                fail(message);
        }
        return;
    }

    loading_.setPhase(StartupPhase::LoadingProjectContent);
    ProjectManager& manager = ProjectManager::instance();
    prepared_->instance = std::move(projectInstance_);
    if (!manager.commitPreparedProject(std::move(*prepared_))) {
        fail(tr("Harken Studio could not open the project"));
        return;
    }

    QTimer::singleShot(0, this, [this]() {
        if (terminal_)
            return;
        ProjectManager& manager = ProjectManager::instance();
        projectFile_ = manager.currentProject()->projectPath;
        mainWindow_->finishPreparedProject(
            projectFile_, hubArguments_.createProject,
            [this](StartupPhase phase) { loading_.setPhase(phase); });
        beginRuntimeSync();
    });
}

void StartupCoordinator::beginRuntimeSync()
{
    loading_.setPhase(StartupPhase::PreparingRuntime);
    ProjectManager& manager = ProjectManager::instance();
    ProjectManager::syncProjectToRuntimeAsync(
        manager.currentProject(), this, [this]() {
            ProjectManager::instance().startWatchingCurrentProject();
            ProjectManager::instance().publishProjectOpened();
            finish();
        });
}

void StartupCoordinator::finish()
{
    if (terminal_ || presentationStarted_)
        return;
    presentationStarted_ = true;
    loading_.setPhase(StartupPhase::ShowingEditor);
    connect(mainWindow_.get(), &MainWindow::startupPresentationFinished,
            this, [this](bool shown) {
        if (terminal_)
            return;
        if (!shown) {
            cancelProjectOpen();
            return;
        }
        terminal_ = true;
        ProjectInstance::setActivationWindow(mainWindow_.get());
        loading_.setPhase(StartupPhase::Ready);
        loading_.close();
        if (hubClient_) {
            hubClient_->reportStage(hubArguments_.createProject
                                        ? QStringLiteral("projectCreated")
                                        : QStringLiteral("projectRead"));
            hubClient_->reportStage(QStringLiteral("ready"));
        }
    });
    mainWindow_->prepareWindowForStartup(loading_.screen());
    mainWindow_->show();
}

void StartupCoordinator::cancelProjectOpen()
{
    terminal_ = true;
    loading_.close();
    if (hubClient_)
        hubClient_->reportFailed(QStringLiteral("PROJECT_OPEN_CANCELLED"),
                                 tr("Project opening was cancelled."));
    QTimer::singleShot(0, &application_, [this]() { application_.exit(0); });
}

void StartupCoordinator::fail(const QString& message, int exitCode,
                              const QString& errorCode,
                              const QString& technicalDetails)
{
    if (terminal_)
        return;
    terminal_ = true;
    const QString userMessage = ProjectUpgradeMessages::userMessage(message);
    const bool hubConnected = hubClient_ && hubClient_->isConnected();
    if (hubConnected)
        hubClient_->reportFailed(errorCode,
            errorCode == projectIssueCode(ProjectIssue::ProjectUpgradeFilesNotWritable)
                ? ProjectUpgradeMessages::userMessage(ProjectIssue::ProjectUpgradeFilesNotWritable)
                : userMessage, technicalDetails);
    if (hubConnected) {
        loading_.close();
    } else {
        loading_.showFailure(userMessage);
        QMessageBox box(QMessageBox::Critical, tr("Harken Studio"),
                        userMessage, QMessageBox::Ok, &loading_);
        const QString details = technicalDetails.trimmed().isEmpty()
            ? errorCode : errorCode + QLatin1Char('\n') + technicalDetails;
        if (!details.trimmed().isEmpty())
            box.setDetailedText(details);
        box.exec();
    }
    QTimer::singleShot(0, &application_, [this, exitCode]() {
        application_.exit(exitCode);
    });
}

}
