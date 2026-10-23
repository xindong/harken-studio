#pragma once

#include <QMainWindow>
#include <QStringList>

class QAction;
class QListWidget;
class QScreen;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace VNH {

class EngineIntegrationsPage;
class EngineTargetStore;
class IntegrationStore;
class LogConsoleDialog;
class ButtonPulseAnimator;
class LocalBuildRefreshCoordinator;
class ProjectIntakeConsumer;
class ProjectStore;
class ProjectsPage;
class ReleaseNotesDialog;
class TitleBar;
class UpdateChecker;
struct UpdateCheckResult;
class VersionsPage;

class HubWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit HubWindow(ProjectStore* store, QWidget* parent = nullptr);

    ~HubWindow() override;

    void openProjectFromPath(const QString& path);
    void registerLocalBuild(const QString& executablePath);
    void revealLocalBuild(const QString& executablePath);
    void showProjectsPage();
    void showNewProjectPage();
    void showVersionsPage();
    void showEngineIntegrationsPage(
        const QString& harkenProjectPath = QString(),
        const QString& studioExecutablePath = QString(),
        const QString& engineProjectPath = QString());
    void showReleaseNotesAfterUpdate();
    void restoreAfterUpdate();

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void installChrome();
    void beginStartupFlow();
    void kickBackgroundCheck(bool manual = true);
    void requestUpdateCheck();
    void handleManualUpdateCheckResult(UpdateChecker* checker,
                                       bool hubProduct,
                                       const UpdateCheckResult& result);
    void finishManualUpdateCheck();
    void refreshHubUpdateButton();
    void applyAutoSilentlyUpdateHubPreference(bool enabled);
    void applyAutoDownloadStudioPreference(bool enabled);
    void beginHubUpdateLanding();
    void showLogConsole();
    bool showReleaseNotes();
    void persistWindowGeometry();
    void persistNormalGeometry(const QRect& geometry);
    void flushPendingWindowGeometry();
    void restoreWindowGeometry();
    QScreen* visibleScreenFor(const QRect& frame) const;
    void clampToVisibleScreen();

    IntegrationStore* integrationStore_ = nullptr;
    EngineTargetStore* engineTargetStore_ = nullptr;
    EngineIntegrationsPage* engineIntegrationsPage_ = nullptr;
    ProjectStore* store_ = nullptr;
    ProjectIntakeConsumer* intakeConsumer_ = nullptr;
    QListWidget* sidebar_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    ProjectsPage* projectsPage_ = nullptr;
    VersionsPage* versionsPage_ = nullptr;
    TitleBar* titleBar_ = nullptr;
    QToolButton* settingsButton_ = nullptr;
    QAction* checkUpdatesAction_ = nullptr;
    QAction* autoHubUpdateYesAction_ = nullptr;
    QAction* autoHubUpdateNoAction_ = nullptr;
    QAction* autoDownloadYesAction_ = nullptr;
    QAction* autoDownloadNoAction_ = nullptr;
    QToolButton* hubUpdateButton_ = nullptr;
    LogConsoleDialog* logConsole_ = nullptr;
    ReleaseNotesDialog* releaseNotes_ = nullptr;
    LocalBuildRefreshCoordinator* localBuildRefresh_ = nullptr;
    QTimer* updateStatePollTimer_ = nullptr;
    ButtonPulseAnimator* hubUpdatePulse_ = nullptr;
    QTimer* windowSizeSaveTimer_ = nullptr;
    UpdateChecker* manualHubUpdateChecker_ = nullptr;
    UpdateChecker* manualStudioUpdateChecker_ = nullptr;
    int manualUpdateChecksPending_ = 0;
    bool manualHubUpdateAvailable_ = false;
    bool manualStudioUpdateAvailable_ = false;
    QStringList manualUpdateCheckErrors_;
};

}
