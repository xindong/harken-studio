#pragma once

#include "Versions/VersionScanner.h"
#include "Integrations/ProjectIntegrationExecutor.h"
#include "Integrations/EngineTargetProbe.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <optional>
#include <atomic>
#include <memory>

class QLabel;
class QAction;
class QComboBox;
class QLineEdit;
class QMenu;
class QDialog;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTabWidget;
class QTextBrowser;
class QTimer;
class QToolButton;
class QTreeWidget;

namespace VNH {

class DialogTextView;

enum class EngineKind;
struct EngineTargetRecord;
struct InstalledVersion;
struct ProjectRecord;
struct UnityIntegrationResult;
struct UnityIntegrationRequest;
struct EngineTargetObservation;
class EngineTargetStore;
class IntegrationStore;
class ProjectStore;
class TableSorting;
class IntegrationTaskProgress;

class EngineIntegrationsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit EngineIntegrationsPage(IntegrationStore* integrations,
                                    EngineTargetStore* targets,
                                    ProjectStore* projects,
                                    QWidget* parent = nullptr);
    ~EngineIntegrationsPage() override;

    void refresh();
    void beginEditForProject(
        const QString& harkenProjectPath = QString(),
        const QString& studioExecutablePath = QString(),
        const QString& engineProjectPath = QString());
    void showList();
    void refreshStudioVersion();

signals:
    void projectsChanged();
    void integrationStatusChanged(const QString& targetId);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    enum class ResultStatus
    {
        Pending,
        Running,
        Succeeded,
        Warning,
        Failed,
        Skipped,
        CheckPassed,
        AwaitingConfirmation,
        NotRun,
    };

    enum class IntegrationComparison { Unknown, Different, Current, Blocked };
    enum class IntegrationPhase { Choosing, Checking, Reviewing, Applying, Finished };

    struct IntegrationSelection
    {
        QString sourcePath;
        QString installPath;
        bool userSelected = false;
    };

    struct TargetInspection
    {
        EngineTargetObservation observation;
        std::shared_ptr<std::atomic<bool>> cancelled;
        bool ready = false;
        bool recoverLocation = false;
    };

    struct ResultStep
    {
        QString id;
        QString title;
        ResultStatus status = ResultStatus::Pending;
        QString summary;
        QString cause;
        QString remediation;
        QStringList details;
        QStringList artifacts;
        qint64 elapsedMs = 0;
        bool hasFileComparison = false;
        std::vector<IntegrationFileChange> fileChanges;
        bool attentionRequired = false;
        QStringList modifiedFiles;
        QStringList missingFiles;
    };

    void beginNewEditor(const QString& harkenProjectPath = QString());
    void beginExistingEditor(const QString& integrationId);
    bool loadExistingIntegration(const QString& integrationId, bool refreshObservation = true);
    void changeIntegrationFromList(const QString& integrationId, const QString& sourcePath);
    void linkProjects();
    void unlinkProjects(const QString& integrationId);
    void browseHarkenProject();
    void browseEngineProject();
    void browseIntegrationSource();
    void browseIntegrationInstallPath();
    void beginIntegrationSelection();
    void cancelIntegrationSelection();
    void runUnityRemoval();
    void runUnityRestoration();
    void runIntegrationRemoval();
    void runUnrealRemoval(const EngineTargetRecord& target);
    void runIntegrationMigration(bool recover);
    void updateRemovalButtonState(bool recoverLocation = false);
    void populateList();
    void populateQuickIntegrationMenu(QMenu* menu, const QString& integrationId);
    EngineTargetObservation requestTargetObservation(const EngineTargetRecord& target, bool refresh = false);
    void cancelTargetInspections();
    EngineTargetObservation observeTarget(const EngineTargetRecord& target, bool verifyContent = false);
    UnityIntegrationResult runUnityTask(const UnityIntegrationRequest& request);
    IntegrationPlan inspectUnrealTask(const EngineTargetRecord& target, const QString& source = {},
                                     std::optional<IntegrationOperation> operation = std::nullopt);
    void scanStopped();
    void showIntegrationResolution();
    QString selectedIntegrationPackageName() const;
    void updatePrimaryAction();
    void refreshEngineRunningHint();
    void updateIntegrationSelectionContext();
    void updateIntegrationSourceState();
    void cancelPendingOperation();
    void setOperationRunning(bool running);
    bool acceptHarkenProject(const QString& path, bool redirectOnConflict);
    bool acceptEngineTarget(const QString& path, bool redirectOnConflict);
    bool resolveSelection(ProjectRecord* project,
                          EngineTargetRecord* target,
                          bool registerTarget,
                          bool redirectOnConflict,
                          bool requireIntegrationSource = true);
    static bool resolveStudioVersion(const ProjectRecord& project,
                                     const QString& launchingExecutable,
                                     InstalledVersion* version);
    void updateStudioVersionFields();
    void updateDefaultIntegrationPath();
    void refreshLocalIntegrationSources();
    void setIntegrationPath(const QString& path, bool userSelected);
    void setEditorLocked(bool locked);
    void clearResults();
    void setSummaryText(const QString& text, bool succeeded = false);
    void updateIdleSummary();
    void runCheck();
    void runIntegration();
    bool runCheckForSelection(const EngineTargetRecord& target,
                              const QString& integrationPath);
    bool runUnity(const EngineTargetRecord& target, const QString& integrationPath,
                  bool checkOnly);
    bool runUnreal(const EngineTargetRecord& target, const QString& integrationPath,
                   bool checkOnly);
    QVector<ResultStep> unityResultSteps(
        const UnityIntegrationResult& result, bool checkOnly = false) const;
    void showResults(const QVector<ResultStep>& steps,
                     const QString& summary,
                     const QString& preferredStep = QString(),
                     bool succeeded = false);
    void showSelectedStep();
    void updateIntegrationCheckResults();
    QString formatStepHtml(const ResultStep& step) const;
    QString formatStepDetails(const ResultStep& step) const;
    QString confirmationDetails() const;
    QString statusText(ResultStatus status) const;
    void showConflict(const QString& integrationId,
                      const QString& reason);
    QString conflictingIntegration(const QString& harkenProjectPath,
                                   const QString& engineTargetId) const;
    void highlightIntegration(const QString& integrationId);
    void advanceHighlight();
    void applyHighlight();
    bool persistIntegration(const QString& harkenProjectPath,
                            const QString& engineTargetId,
                            const QString& integrationSourcePath);

    IntegrationStore* integrations_ = nullptr;
    EngineTargetStore* targets_ = nullptr;
    ProjectStore* projects_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QWidget* listPage_ = nullptr;
    QWidget* editorPage_ = nullptr;
    QWidget* integrationForm_ = nullptr;
    QTableWidget* table_ = nullptr;
    TableSorting* sorting_ = nullptr;
    QLineEdit* harkenProjectEdit_ = nullptr;
    QLineEdit* engineProjectEdit_ = nullptr;
    QComboBox* integrationSourceCombo_ = nullptr;
    QLabel* integrationSourceLabel_ = nullptr;
    QLineEdit* integrationPathEdit_ = nullptr;
    QPushButton* integrationSourceBrowseButton_ = nullptr;
    QLineEdit* integrationInstallPathEdit_ = nullptr;
    QLabel* integrationInstallLabel_ = nullptr;
    QPushButton* integrationInstallBrowseButton_ = nullptr;
    QPushButton* backButton_ = nullptr;
    QPushButton* harkenBrowseButton_ = nullptr;
    QPushButton* engineBrowseButton_ = nullptr;
    QPushButton* checkButton_ = nullptr;
    QPushButton* restoreIntegrationButton_ = nullptr;
    QPushButton* integrationCheckButton_ = nullptr;
    QDialog* integrationSelectionDialog_ = nullptr;
    DialogTextView* integrationSelectionContext_ = nullptr;
    DialogTextView* integrationSelectionSummary_ = nullptr;
    DialogTextView* integrationSafetyHint_ = nullptr;
    QTimer* engineStatusTimer_ = nullptr;
    QPlainTextEdit* integrationSelectionPaths_ = nullptr;
    IntegrationTaskProgress* integrationProgress_ = nullptr;
    QPushButton* integrationReportButton_ = nullptr;
    QTabWidget* integrationResultsTabs_ = nullptr;
    DialogTextView* integrationDiffSummary_ = nullptr;
    QTreeWidget* integrationDiffTree_ = nullptr;
    QTextBrowser* integrationSelectionDetails_ = nullptr;
    QPushButton* integrateButton_ = nullptr;
    QPushButton* changeIntegrationButton_ = nullptr;
    QPushButton* cancelIntegrationButton_ = nullptr;
    QWidget* integrationSelectionRow_ = nullptr;
    QWidget* integrationInstallRow_ = nullptr;
    QLineEdit* installedIntegrationPath_ = nullptr;
    QPushButton* unlinkProjectsButton_ = nullptr;
    QPushButton* linkProjectsButton_ = nullptr;
    QLabel* integrationHeading_ = nullptr;
    QAction* unlinkProjectsAction_ = nullptr;
    QPushButton* migrateIntegrationButton_ = nullptr;
    QPushButton* removeInstalledIntegrationButton_ = nullptr;
    QLabel* installedIntegrationLabel_ = nullptr;
    QLabel* relationshipLabel_ = nullptr;
    QWidget* integrationIssueRow_ = nullptr;
    QLabel* integrationIssueLabel_ = nullptr;
    QPushButton* integrationResolutionButton_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* storageErrorLabel_ = nullptr;
    QTableWidget* stepsTable_ = nullptr;
    QPlainTextEdit* detailEdit_ = nullptr;
    QTimer* highlightTimer_ = nullptr;
    QVector<ResultStep> resultSteps_;
    QString editingIntegrationId_;
    QString selectedHarkenProjectPath_;
    QString selectedEngineTargetId_;
    QString installPathTargetId_;
    QString installedIntegrationVersion_;
    QString launchingStudioExecutablePath_;
    QString highlightedIntegrationId_;
    qint64 highlightDeadlineMs_ = 0;
    bool highlightPhase_ = false;
    bool integrationPathUserSelected_ = false;
    std::optional<InstalledVersion> studioVersion_;
    quint64 versionRequest_ = 0;
    bool versionResolutionPending_ = false;
    bool operationRunning_ = false;
    bool engineStatusPending_ = false;
    bool engineRunning_ = false;
    bool quickIntegrationChange_ = false;
    bool listMenuOpen_ = false;
    bool refreshingList_ = false;
    bool observationRefreshPending_ = false;
    bool versionRefreshPending_ = false;
    bool inspectionStopped_ = false;
    bool showingIdleSummary_ = true;
    bool integrationInstalled_ = false;
    bool integrationSourceAvailable_ = false;
    bool targetBlocked_ = false;
    std::optional<IntegrationSelection> integrationSelection_;
    std::optional<IntegrationPlan> reviewedUnrealPlan_;
    QByteArray reviewedIntegrationHash_;
    QString integrationPreviousVersion_;
    QString integrationReportPath_;
    bool integrationReviewInvalidated_ = false;
    IntegrationPhase integrationPhase_ = IntegrationPhase::Choosing;
    IntegrationComparison integrationComparison_ = IntegrationComparison::Unknown;
    QHash<QString, TargetInspection> targetInspections_;
};

}
