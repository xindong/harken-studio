#pragma once

#include "Core/HubClient/HubClient.h"
#include "Core/ProjectManager.h"

#include <QObject>
#include <QFutureWatcher>
#include <memory>

class QApplication;

namespace VNX {

class MainWindow;
class StartupLoadingWindow;

class StartupCoordinator final : public QObject
{
    Q_OBJECT

public:
    StartupCoordinator(QApplication& application,
                       StartupLoadingWindow& loading,
                       HubClient::Arguments hubArguments,
                       QString projectFile,
                       QObject* parent = nullptr);
    ~StartupCoordinator() override;

    void start();

private:
    void startAfterHubConnection();
    void startProjectPreparation();
    void finishProjectAnalysis();
    void startExistingProjectPreparation();
    ProjectUpgradeProgressCallback upgradeProgressCallback();
    void showUpgradeProgress(const ProjectUpgradeProgress& progress);
    void startMainWindowInitialization();
    void tryCommit();
    void beginRuntimeSync();
    void finish();
    void cancelProjectOpen();
    void fail(const QString& message, int exitCode = 3,
              const QString& errorCode = QStringLiteral("INTERNAL_FAILURE"),
              const QString& technicalDetails = {});

    QApplication& application_;
    StartupLoadingWindow& loading_;
    HubClient::Arguments hubArguments_;
    QString projectFile_;
    std::shared_ptr<ProjectInstance> projectInstance_;
    std::unique_ptr<HubClient> hubClient_;
    std::unique_ptr<MainWindow> mainWindow_;
    QFutureWatcher<ProjectUpgradeAnalysis> analysisWatcher_;
    QFutureWatcher<std::shared_ptr<PreparedProject>> preparationWatcher_;
    ProjectUpgradeAnalysis analysis_;
    std::shared_ptr<PreparedProject> prepared_;
    bool projectPrepared_ = false;
    ProjectUpgradeApprovalCallback upgradeApproval_;
    bool windowInitialized_ = false;
    bool terminal_ = false;
};

}
