#pragma once

#include "Versions/VersionScanner.h"
#include "Integrations/ProjectIntegrationExecutor.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <optional>

class QLabel;
class QAction;
class QComboBox;
class QLineEdit;
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

class EngineIntegrationsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit EngineIntegrationsPage(IntegrationStore* integrations,
                                    EngineTargetStore* targets,
                                    ProjectStore* projects,
                                    QWidget* parent = nullptr);

    void refresh();
    void beginEditForProject(
        const QString& harkenProjectPath = QString(),
        const QString& studioExecutablePath = QString(),
        const QString& engineProjectPath = QString());
    void showList();
    void refreshStudioVersion();

signals:
    void projectsChanged();

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

    enum class SdkComparison { Unknown, Different, Current, Blocked };

    struct SdkSelection
    {
        QString sourcePath;
        QString installPath;
        bool userSelected = false;
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
    };

    void beginNewEditor(const QString& harkenProjectPath = QString());
    void beginExistingEditor(const QString& integrationId);
    void editIntegration(int row);
    void removeIntegration(int row);
    void linkProjects();
    void unlinkProjects(const QString& integrationId);
    void openEngineFolder(int row);
    void browseHarkenProject();
    void browseEngineProject();
    void browseSdkSource();
    void browseSdkInstallPath();
    void beginSdkSelection();
    void cancelSdkSelection();
    void runUnityRemoval();
    void runSdkRemoval();
    void runUnrealRemoval(const EngineTargetRecord& target);
    void runSdkMigration(bool recover);
    void updateRemovalButtonState(bool recoverLocation = false);
    EngineTargetObservation observeTarget(const EngineTargetRecord& target);
    UnityIntegrationResult runUnityTask(const UnityIntegrationRequest& request);
    IntegrationPlan inspectUnrealTask(const EngineTargetRecord& target, const QString& source = {},
                                     std::optional<IntegrationOperation> operation = std::nullopt);
    void scanStopped();
    void showSdkResolution();
    void updatePrimaryAction();
    void updateSdkSourceState();
    void cancelPendingOperation();
    void setOperationRunning(bool running);
    bool acceptHarkenProject(const QString& path, bool redirectOnConflict);
    bool acceptEngineTarget(const QString& path, bool redirectOnConflict);
    bool resolveSelection(ProjectRecord* project,
                          EngineTargetRecord* target,
                          bool registerTarget,
                          bool redirectOnConflict,
                          bool requireSdkSource = true);
    static bool resolveStudioVersion(const ProjectRecord& project,
                                     const QString& launchingExecutable,
                                     InstalledVersion* version);
    void updateStudioVersionFields();
    void updateDefaultSdkPath();
    void refreshInstalledSdkSources();
    void setSdkPath(const QString& path, bool userSelected);
    void setEditorLocked(bool locked);
    void clearResults();
    void setSummaryText(const QString& text);
    void updateIdleSummary();
    void runCheck();
    void runIntegration();
    bool runCheckForSelection(const EngineTargetRecord& target,
                              const QString& sdkPath);
    bool runUnity(const EngineTargetRecord& target, const QString& sdkPath,
                  bool checkOnly);
    bool runUnreal(const EngineTargetRecord& target, const QString& sdkPath,
                   bool checkOnly);
    QVector<ResultStep> unityResultSteps(
        const UnityIntegrationResult& result, bool checkOnly = false) const;
    void showResults(const QVector<ResultStep>& steps,
                     const QString& summary,
                     const QString& preferredStep = QString());
    void showSelectedStep();
    void updateSdkCheckResults();
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
                            const QString& sdkSourcePath);

    IntegrationStore* integrations_ = nullptr;
    EngineTargetStore* targets_ = nullptr;
    ProjectStore* projects_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QWidget* listPage_ = nullptr;
    QWidget* editorPage_ = nullptr;
    QWidget* integrationForm_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLineEdit* harkenProjectEdit_ = nullptr;
    QLineEdit* engineProjectEdit_ = nullptr;
    QComboBox* sdkSourceCombo_ = nullptr;
    QLineEdit* sdkPathEdit_ = nullptr;
    QPushButton* sdkSourceBrowseButton_ = nullptr;
    QLineEdit* sdkInstallPathEdit_ = nullptr;
    QPushButton* sdkInstallBrowseButton_ = nullptr;
    QPushButton* backButton_ = nullptr;
    QPushButton* harkenBrowseButton_ = nullptr;
    QPushButton* engineBrowseButton_ = nullptr;
    QPushButton* checkButton_ = nullptr;
    QPushButton* sdkCheckButton_ = nullptr;
    QDialog* sdkSelectionDialog_ = nullptr;
    QLabel* sdkSelectionSummary_ = nullptr;
    QTabWidget* sdkResultsTabs_ = nullptr;
    QLabel* sdkDiffSummary_ = nullptr;
    QTreeWidget* sdkDiffTree_ = nullptr;
    QTextBrowser* sdkSelectionDetails_ = nullptr;
    QPushButton* integrateButton_ = nullptr;
    QPushButton* changeSdkButton_ = nullptr;
    QPushButton* cancelSdkButton_ = nullptr;
    QWidget* sdkSelectionRow_ = nullptr;
    QWidget* sdkInstallRow_ = nullptr;
    QLineEdit* installedSdkPath_ = nullptr;
    QPushButton* unlinkProjectsButton_ = nullptr;
    QPushButton* linkProjectsButton_ = nullptr;
    QLabel* sdkHeading_ = nullptr;
    QAction* unlinkProjectsAction_ = nullptr;
    QPushButton* migrateSdkButton_ = nullptr;
    QPushButton* removeInstalledSdkButton_ = nullptr;
    QLabel* installedSdkLabel_ = nullptr;
    QLabel* relationshipLabel_ = nullptr;
    QWidget* sdkIssueRow_ = nullptr;
    QLabel* sdkIssueLabel_ = nullptr;
    QPushButton* sdkResolutionButton_ = nullptr;
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
    QString installedSdkVersion_;
    QString launchingStudioExecutablePath_;
    QString highlightedIntegrationId_;
    qint64 highlightDeadlineMs_ = 0;
    bool highlightPhase_ = false;
    bool sdkPathUserSelected_ = false;
    std::optional<InstalledVersion> studioVersion_;
    quint64 versionRequest_ = 0;
    bool versionResolutionPending_ = false;
    bool operationRunning_ = false;
    bool inspectionStopped_ = false;
    bool showingIdleSummary_ = true;
    bool sdkInstalled_ = false;
    bool sdkSourceAvailable_ = false;
    bool targetBlocked_ = false;
    std::optional<SdkSelection> sdkSelection_;
    std::optional<IntegrationPlan> reviewedUnrealPlan_;
    SdkComparison sdkComparison_ = SdkComparison::Unknown;
};

}
