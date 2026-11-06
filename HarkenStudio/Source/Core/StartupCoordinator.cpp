#include "StartupCoordinator.h"

#include "Core/ProjectTemplateLoader.h"
#include "Core/ProjectInstance.h"
#include "Core/ProjectOpenApproval.h"
#include "Core/ProjectUpgradeManager.h"
#include "Core/ProjectUpgradeMessages.h"
#include "Dialogs/StartupLoadingWindow.h"
#include "MainWindow.h"
#include "Core/IPC/StudioInstanceTransport.h"
#include "Tooling/StudioCli/Shell.h"
#include "Tooling/StudioCli/StudioSession.h"

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
    StartupArguments startupArguments,
    QString projectFile,
    QObject* parent)
    : QObject(parent),
      application_(application),
      loading_(loading),
      startupArguments_(std::move(startupArguments)),
      projectFile_(std::move(projectFile))
{
    connect(&loading_, &StartupLoadingWindow::cancelRequested,
            this, &StartupCoordinator::cancelProjectOpen);
    connect(&analysisWatcher_,
            &QFutureWatcher<ProjectUpgradeAnalysis>::finished,
            this, &StartupCoordinator::finishProjectAnalysis);
    connect(&preparationWatcher_,
            &QFutureWatcher<std::shared_ptr<PreparedProject>>::finished,
            this, [this]() {
        if (terminal_) {
            finishCancellation();
            return;
        }
        prepared_ = preparationWatcher_.result();
        projectPrepared_ = true;
        tryCommit();
    });
    connect(&cleanupWatcher_, &QFutureWatcher<void>::finished,
            this, &StartupCoordinator::finishCancellation);
}

StartupCoordinator::~StartupCoordinator()
{
    if (analysisWatcher_.isRunning())
        analysisWatcher_.waitForFinished();
    if (preparationWatcher_.isRunning())
        preparationWatcher_.waitForFinished();
    if (cleanupWatcher_.isRunning())
        cleanupWatcher_.waitForFinished();
}

void StartupCoordinator::start()
{
    if (terminal_)
        return;
    loading_.setPhase(StartupPhase::PreparingApplication);
    ProjectInstance::setActivationWindow(&loading_);
    const QString instancePath = startupArguments_.createProject
        ? QDir(QDir(startupArguments_.projectLocation.trimmed())
                   .filePath(startupArguments_.projectName.trimmed()))
              .filePath(startupArguments_.projectName.trimmed() + QStringLiteral(".hsp"))
        : projectFile_;
    auto acquisition = ProjectInstance::acquire(instancePath, startupArguments_.shell
        ? ProjectInstance::ActivationPolicy::Deny : ProjectInstance::ActivationPolicy::Allow);
    if (acquisition.status == ProjectInstance::Status::Failed) {
        fail(acquisition.error, 3, QStringLiteral("PROJECT_INSTANCE_UNAVAILABLE"));
        return;
    }
    if (acquisition.status == ProjectInstance::Status::Forwarded) {
        if (startupArguments_.createProject) {
            fail(tr("This project is already open in another Studio window."),
                 3, QStringLiteral("PROJECT_ALREADY_OPEN"));
            return;
        }
        terminal_ = true;
        loading_.dismiss();
        application_.exit(0);
        return;
    }
    projectInstance_ = std::move(acquisition.instance);
    startProjectPreparation();
    if (!terminal_)
        startMainWindowInitialization();
}

void StartupCoordinator::startProjectPreparation()
{
    loading_.setPhase(startupArguments_.createProject
                          ? StartupPhase::CreatingWorkspace
                          : StartupPhase::ReadingProject);
    if (startupArguments_.createProject) {
        ProjectConfig config;
        config.name = startupArguments_.projectName.trimmed();
        config.location = startupArguments_.projectLocation.trimmed();
        if (!startupArguments_.projectTemplatePath.isEmpty()) {
            ValidationResult validation;
            const auto projectTemplate = ProjectTemplateLoader::loadFile(
                startupArguments_.projectTemplatePath, &validation);
            if (!projectTemplate) {
                fail(validation.toUserText());
                return;
            }
            config.templateId = startupArguments_.projectTemplateId.isEmpty()
                                    ? projectTemplate->id
                                    : startupArguments_.projectTemplateId;
            config.templateData = projectTemplate->toJson();
        }
        preparationWatcher_.setFuture(QtConcurrent::run(
            [config, cancelled = cancellationRequested_]() {
                return std::make_shared<PreparedProject>(
                    ProjectManager::prepareProject(config, [cancelled]() {
                        return cancelled->load(std::memory_order_relaxed);
                    }));
            }));
        return;
    }

    if (projectFile_.isEmpty()) {
        preparationWatcher_.setFuture(QtConcurrent::run([cancelled = cancellationRequested_]() {
            return std::make_shared<PreparedProject>(
                ProjectManager::prepareProject(QString(), false, {}, {}, [cancelled]() {
                    return cancelled->load(std::memory_order_relaxed);
                }));
        }));
        return;
    }

    const QString projectFile = projectFile_;
    const ProjectUpgradeProgressCallback progress = upgradeProgressCallback();
    projectReadContext_ = std::make_shared<ProjectContractReadContext>();
    analysisWatcher_.setFuture(QtConcurrent::run(
        [projectFile, progress, context = projectReadContext_]() {
            return ProjectUpgradeManager().analyze(projectFile, progress, context.get());
        }));
}

void StartupCoordinator::finishProjectAnalysis()
{
    if (terminal_) {
        finishCancellation();
        return;
    }
    analysis_ = analysisWatcher_.result();
    if (!analysis_.ok) {
        fail(ProjectUpgradeMessages::userMessage(analysis_.issue, analysis_.detail),
             3, projectIssueCode(analysis_.issue),
             analysis_.blockingErrors.join(QLatin1Char('\n')));
        return;
    }
    if (startupArguments_.shell && analysis_.upgradeRequired) {
        fail(QStringLiteral("Open and upgrade this project in Studio before starting --shell."),
            3, QStringLiteral("PROJECT_UPGRADE_SELECTION_REQUIRED"));
        return;
    }
    const ProjectOpenDecision decision = confirmProjectOpen(analysis_, &loading_);
    if (terminal_)
        return;
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
        [projectFile, approval, progress, cancelled = cancellationRequested_,
         context = std::move(projectReadContext_)]() {
            return std::make_shared<PreparedProject>(
                ProjectManager::prepareProject(projectFile, static_cast<bool>(approval),
                                               progress, approval, [cancelled]() {
                    return cancelled->load(std::memory_order_relaxed);
                }, context.get()));
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
    if (terminal_)
        return;
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
            if (terminal_)
                return;
            windowInitialized_ = true;
            tryCommit();
        },
        [cancelled = cancellationRequested_]() {
            return cancelled->load(std::memory_order_relaxed);
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
                prepared_->error, !startupArguments_.createProject);
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
            projectFile_, startupArguments_.createProject,
            [this](StartupPhase phase) { loading_.setPhase(phase); });
        beginRuntimeSync();
    });
}

void StartupCoordinator::beginRuntimeSync()
{
    if (terminal_)
        return;
    loading_.setPhase(StartupPhase::PreparingRuntime);
    ProjectManager& manager = ProjectManager::instance();
    runtimeSyncContext_ = std::make_unique<QObject>();
    ProjectManager::syncProjectToRuntimeAsync(
        manager.currentProject(), runtimeSyncContext_.get(), [this]() {
            if (terminal_)
                return;
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
        if (!terminal_ && !shown)
            cancelProjectOpen();
    });
    mainWindow_->prepareWindowForStartup(loading_.screen());
    mainWindow_->show();
    QTimer::singleShot(0, this, [this]() {
        if (terminal_)
            return;
        terminal_ = true;
        ProjectInstance::setActivationWindow(mainWindow_.get());
        loading_.setPhase(StartupPhase::Ready);
        loading_.dismiss();
        auto* instanceServer = new StudioInstanceServer(mainWindow_->ipcServer(), mainWindow_.get());
        QString instanceError;
        if (!instanceServer->start(&instanceError))
            qWarning().noquote() << "Studio instance commands unavailable:" << instanceError;
        if (startupArguments_.shell) {
            while (true) {
                const auto result = HarkenStudioCli::runDesktopShell(
                    *mainWindow_->ipcServer(), startupArguments_.shellJson);
                using Exit = HarkenStudioCli::ShellExitRequest;
                if (result.exitRequest == Exit::None) break;
                const bool discarded = result.exitRequest == Exit::Discard
                    && ProjectManager::instance().hasUnsavedChanges();
                const bool closed = result.exitRequest == Exit::Discard
                    ? mainWindow_->closeWithDiscard() : mainWindow_->close();
                if (closed) {
                    if (result.exitRequest != Exit::EndOfInput)
                        HarkenStudioCli::writeShellResponse({{"status", "ok"}, {"command", "exit"},
                            {"result", QJsonObject{{"exiting", true}, {"discarded", discarded}}}});
                    application_.quit();
                    break;
                }
                auto error = HarkenStudioCli::failure(QStringLiteral("EXIT_NOT_COMPLETED"),
                    QStringLiteral("Studio did not close. Resolve the active operation or canceled close request before retrying."));
                error.insert(QStringLiteral("command"), QStringLiteral("exit"));
                HarkenStudioCli::writeShellResponse(error);
                if (result.exitRequest == Exit::EndOfInput) break;
            }
        }
    });
}

void StartupCoordinator::cancelProjectOpen()
{
    if (terminal_)
        return;
    terminal_ = true;
    cancelling_ = true;
    cancellationRequested_->store(true, std::memory_order_relaxed);
    loading_.showCancelling();
    runtimeSyncContext_.reset();
    if (mainWindow_)
        mainWindow_->hide();
    finishCancellation();
}

void StartupCoordinator::finishCancellation()
{
    if (!cancelling_ || analysisWatcher_.isRunning()
        || preparationWatcher_.isRunning() || cleanupWatcher_.isRunning())
        return;
    if (!prepared_ && preparationWatcher_.future().resultCount() > 0)
        prepared_ = preparationWatcher_.result();
    if (prepared_ && prepared_->upgradePending) {
        cleanupWatcher_.setFuture(QtConcurrent::run([prepared = prepared_]() {
            prepared->rollbackUpgrade();
        }));
        return;
    }
    cancelling_ = false;
    QTimer::singleShot(0, this, [this]() {
        loading_.dismiss();
        if (startupArguments_.shell)
            HarkenStudioCli::writeShellResponse(HarkenStudioCli::failure(QStringLiteral("CANCELED"),
                QStringLiteral("Project opening was canceled before the shell became ready.")));
        application_.exit(startupArguments_.shell ? 130 : 0);
    });
}

void StartupCoordinator::fail(const QString& message, int exitCode,
                              const QString& errorCode,
                              const QString& technicalDetails)
{
    if (terminal_)
        return;
    terminal_ = true;
    const QString userMessage = ProjectUpgradeMessages::userMessage(message);
    if (startupArguments_.shell) {
        HarkenStudioCli::writeShellResponse(HarkenStudioCli::failure(errorCode, userMessage));
        loading_.dismiss();
        QTimer::singleShot(0, &application_, [this, exitCode]() { application_.exit(exitCode); });
        return;
    }
    loading_.showFailure(userMessage);
    QMessageBox box(QMessageBox::Critical, tr("Harken Studio"),
                    userMessage, QMessageBox::Ok, &loading_);
    const QString details = technicalDetails.trimmed().isEmpty()
        ? errorCode : errorCode + QLatin1Char('\n') + technicalDetails;
    if (!details.trimmed().isEmpty())
        box.setDetailedText(details);
    box.exec();
    QTimer::singleShot(0, &application_, [this, exitCode]() {
        application_.exit(exitCode);
    });
}

}
