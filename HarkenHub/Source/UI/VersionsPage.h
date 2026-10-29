#pragma once

#include <QWidget>
#include "Download/DownloadLease.h"

#include "Transactions/InstallIndex.h"
#include "Transactions/InstallRequest.h"
#include "Update/ServerVersionCatalog.h"
#include "Versions/VersionScanner.h"

class QFileSystemWatcher;
class QComboBox;
class QLabel;
class QFrame;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTimer;

namespace VNH {

class ProjectStore;
class ButtonPulseAnimator;
class StudioReleaseNotes;

class VersionsPage : public QWidget
{
    Q_OBJECT

public:
    explicit VersionsPage(ProjectStore* store, QWidget* parent = nullptr);
    ~VersionsPage() override;

    void refresh();
    void beginGetFromServer();
    void showServerCheckPending();
    void setAutoDownloadStudio(bool enabled);
    void highlightVersion(const QString& executablePath);
    void highlightServerUpdate();
    std::vector<InstalledVersion> installedVersions() const { return versions_; }

    static QString watchDirectoryFor(const QString& versionsRoot);
    static void sortForDisplay(std::vector<InstalledVersion>& versions);

signals:
    void launchTargetChanged();
    void versionsRefreshed();
    void projectLaunchGuidanceRequested(const QString& buildId);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    enum class ElevatedMode
    {
        None,
        Install,
        Uninstall,
        Repair
    };

    bool operationBusy() const;
    void showRowMenu(const InstalledVersion& version, const QPoint& globalPos);
    void showAddVersionMenu();
    void addVersionRow(int row, const InstalledVersion& version,
                       int linkedProjects);
    void beginAddVersion();
    void beginAddLocalBuild();
    void removeLocalBuild(const InstalledVersion& version);
    void setManagementButtonsEnabled(bool enabled);
    void fetchManifestForPackage(const QString& packagePath);
    void onCatalogFinished(const ServerVersionList& result);
    void beginInstallFromFiles(const QString& packagePath,
                               const QString& manifestPath,
                               bool explicitInstall,
                               std::shared_ptr<DownloadLease> lease = {});
    void beginUninstall(const InstalledVersion& version);
    void beginRepair(const QString& buildId);
    void beginHubUpdate();
    void beginStudioUpdateInstall();
    void onPrepared(const InstallPreparation& preparation);
    void onElevatedPoll();
    void finishElevatedFlow(const QString& status);
    void runSelfTest(const QString& buildId);
    void updateRepairBanner(const std::vector<InstalledVersionRecord>& broken);
    void updateSelectedIssue();
    void updateHubUpdateBanner();
    void updateStudioUpdateBanner();
    void updateAutoProgress();
    void updateGetFromServerButton();
    void updateVersionsWatch();
    void tryStartVersionHighlight();
    void advanceVersionHighlight();
    void applyVersionHighlight();
    bool versionMatchesHighlight(const InstalledVersion& version) const;
    const InstalledVersion* versionForRow(int row) const;

    ProjectStore* store_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QLabel* installStatusLabel_ = nullptr;
    QProgressBar* busyBar_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* getFromServerButton_ = nullptr;
    QFrame* repairBanner_ = nullptr;
    QComboBox* repairSelector_ = nullptr;
    QLabel* repairLabel_ = nullptr;
    QPushButton* repairButton_ = nullptr;
    QPushButton* repairUninstallButton_ = nullptr;
    QFrame* hubUpdateBanner_ = nullptr;
    QLabel* hubUpdateLabel_ = nullptr;
    QPushButton* hubUpdateButton_ = nullptr;
    QFrame* studioUpdateBanner_ = nullptr;
    QLabel* studioUpdateLabel_ = nullptr;
    QPushButton* studioUpdateButton_ = nullptr;
    QFrame* autoProgressFrame_ = nullptr;
    QLabel* autoProgressLabel_ = nullptr;
    QProgressBar* autoProgressBar_ = nullptr;
    QTimer* elevatedPollTimer_ = nullptr;
    QTimer* statusPollTimer_ = nullptr;
    QFileSystemWatcher* versionsWatcher_ = nullptr;
    QTimer* watchDebounceTimer_ = nullptr;
    QTimer* versionHighlightTimer_ = nullptr;
    ButtonPulseAnimator* serverUpdatePulse_ = nullptr;
    ServerVersionCatalog* catalog_ = nullptr;
    StudioReleaseNotes* releaseNotes_ = nullptr;
    void* elevatedProcessHandle_ = nullptr;
    ElevatedMode elevatedMode_ = ElevatedMode::None;
    bool preparing_ = false;
    bool activeExplicitInstall_ = false;
    bool autoDownloadStudio_ = true;
    QString activeTransactionId_;
    QString activeBuildId_;
    QString pendingManifestPackagePath_;
    QString pendingManifestBuildId_;
    QString highlightedExecutablePath_;
    QString highlightedRuntimeBuildId_;
    qint64 versionHighlightDeadlineMs_ = 0;
    bool versionHighlightPhase_ = false;
    std::vector<InstalledVersion> versions_;
};

}
