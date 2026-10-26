
#pragma once

#include <QMainWindow>
#include <DockManager.h>
#include <DockWidget.h>
#include <DockAreaWidget.h>
#include <DockOverlay.h>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QTabWidget>
#include <QUndoStack>
#include <QStackedWidget>
#include <QPointer>
#include <QThread>
#include <QMap>
#include <QHash>
#include <QVariant>
#include <QDir>
#include <QProgressBar>
#include <QStringList>
#include <QUuid>
#include <QVariantMap>
#include <cstdint>
#include <memory>
#include <limits>
#include <functional>
#include <optional>

#include "Core/WorkspaceWindowTypes.h"
#include "Widgets/LogPanel/LogPanel.h"
#include "Widgets/Monitor/MonitorPerformanceChartSource.h"
#include "Widgets/Monitor/MonitorBusesSource.h"
#include "Widgets/Monitor/MonitorDspTimelineSource.h"
#include "Widgets/Monitor/MonitorEffectsSource.h"
#include "Widgets/Monitor/MonitorLineageSource.h"
#include "Widgets/Monitor/MonitorPlayablesSource.h"
#include "Monitor/MonitorPerformanceLiveHost.h"
#include "Monitor/MonitorRecordingBrowser.h"
#include "Widgets/Monitor/MonitorScopeSource.h"
#include "Widgets/Monitor/MonitorTapSelectSource.h"
#include "Widgets/Monitor/MonitorWaveformSource.h"
#include "Widgets/Monitor/MonitorSpectrumSource.h"
#include "Widgets/Monitor/MonitorVoicesSource.h"
#include "Widgets/TimeSeries/MonitorCursorModel.h"
#include "Core/StandaloneEditorDispatch.h"
#include "Core/ExternalEditorSelection.h"
#include "Core/StartupProgress.h"

#ifdef HARKEN_HAS_QWINDOWKIT
#include "Widgets/TitleBar/TitleBarWidget.h"
namespace QWK { class WidgetWindowAgent; }
#endif

namespace VNX {
    struct ProjectConfig;
    class ProjectExplorer;
    class Hierarchy;
    class InspectorPanel;
    class AudioEngine;
    class ProjectSerializer;
    class SourceEditorPanel;
    class ExternalEditorPanel;
    class TimelineEditorPanel;
    class TransitionEditorPanel;
    class GridEditorPanel;
    class MeterPanel;
    class LogPanel;
    class PublisherPanel;
    class BlueprintCanvasWidget;
    class AssistantPanel;
    class AIAssistantService;
    class IPCServer;
    class PanelInstanceRegistry;
    class EditorShellRegistry;
    class LayoutController;
    class SelectionRouter;
    struct CurveSelection;
    class BusController;
    class ExternalDropDispatcher;
    class WorkspaceWindowManager;
    struct ProjectItem;
    struct ProjectData;
    class PlacementContext;
    class PlaybackControls;
    class RemoteConnectDialog;
    namespace Monitor {
        class MonitorAdapter;
        class MonitorCaptureControlHost;
        class MonitorPerformanceLiveHost;
    }
}

class ControlPanel;
class QPushButton;

namespace HarkenEditor {
    class AuditionManager;
    namespace Bridge { class RemoteConnection; }
}

namespace VNX {

namespace Monitor { class EventCaptureController; }

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    void showRemoteConnect();
    HarkenEditor::Bridge::RemoteConnection* remoteConnection() const { return remoteConnection_; }
    Monitor::EventCaptureController* eventCaptureController() const { return eventCaptureController_; }
    enum class PreferredOpenArea { CentralTab, Float, RestoreSaved };

    enum class InitializationMode { Immediate, Deferred };

    explicit MainWindow(
        QWidget* parent = nullptr,
        std::function<void(StartupPhase)> startupProgress = {},
        InitializationMode initializationMode = InitializationMode::Immediate);
    void prepareWindowForStartup(QScreen* screen);
    void initializeDeferred(std::function<void(StartupPhase)> startupProgress,
                            std::function<void()> finished);
    ~MainWindow() override;
    
    Q_INVOKABLE bool switchToView(const QString& viewName);
    Q_INVOKABLE bool revealPanel(const QString& objectName, bool visible);
    Q_INVOKABLE QString currentView() const;
    Q_INVOKABLE bool switchToLayout(const QString& layoutName);
    Q_INVOKABLE QString currentLayoutName() const;
    void applyMonitorPerformanceChartModel(const TimeSeriesChartModel& model);
    const TimeSeriesChartModel& monitorPerformanceChartModel() const;
    void applyMonitorVoicesSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> monitorVoicesSnapshot() const;
    void applyMonitorPlayablesSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> monitorPlayablesSnapshot() const;
    void applyMonitorEffectsSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> monitorEffectsSnapshot() const;
    void applyMonitorBusesSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> monitorBusesSnapshot() const;
    void applyMonitorDspTimelineSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> monitorDspTimelineSnapshot() const;
    void applyMonitorLineageSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> monitorLineageSnapshot() const;
    void applyMonitorTapSelectSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> monitorTapSelectSnapshot() const;
    void applyMonitorWaveformSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> monitorWaveformSnapshot() const;
    void applyMonitorScopeSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> monitorScopeSnapshot() const;
    void applyMonitorSpectrumSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot);
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> monitorSpectrumSnapshot() const;
    MonitorCursorModel& monitorSharedCursor();
    Q_INVOKABLE QString getRecentLogs(int count = 100, const QString& levelFilter = QString()) const;
    Q_INVOKABLE bool saveNamedLayout(const QString& name, QString* pathOut = nullptr);
    Q_INVOKABLE bool loadNamedLayout(const QString& name);
    Q_INVOKABLE bool deleteNamedLayout(const QString& name);
    Q_INVOKABLE void rebuildLayoutsMenu();
    Q_INVOKABLE void selectBus(uint32_t busID, bool reveal = false);

    void revealDock(ads::CDockWidget* dock);
    bool isDockLocked(ads::CDockWidget* dock) const;
    void loadBusSignalChain(uint32_t busID);
    void syncBusSelectionViews(uint32_t busID);

    bool loadProject(
        const QString& path,
        std::function<void(StartupPhase)> startupProgress = {},
        QString* error = nullptr,
        bool* forwarded = nullptr);
    bool isProjectOpening() const { return projectOpenInProgress_; }

    bool createProjectFromConfig(
        const ProjectConfig& config,
        std::function<void(StartupPhase)> startupProgress = {});

    void finishPreparedProject(const QString& path,
                               bool created,
                               std::function<void(StartupPhase)> startupProgress = {});

    IPCServer* ipcServer() const { return ipcServer_; }

    HarkenEditor::AuditionManager* auditionManager() const { return auditionManager_; }
    ProjectItem* auditionTarget() const;

    BlueprintCanvasWidget* blueprintCanvas() const { return blueprintCanvas_; }

    struct StandaloneEditorTypeInfo {
        QString label;
        QString dockObjectPrefix;
        bool comingSoon = false;
        bool openBelowCentralArea = false;
        bool clearOnUnrelatedSelection = true;
        std::function<QWidget*(int id)> createConfigured;
        std::function<void(QWidget*, ProjectItem*)> bind;
        std::function<ProjectItem*(QWidget*)> boundItem;
        std::function<bool(QWidget*)> isPinned;
        std::function<void(QWidget*, bool)> setPinned;
        std::function<bool(const ProjectItem*)> accepts;
        std::function<void(QWidget*)> clear;
        std::function<QVariantMap(QWidget*)> snapshotParams;
        std::function<void(QWidget*, const QVariantMap&)> restoreParams;
    };
    void registerStandaloneEditorType(const QString& type, StandaloneEditorTypeInfo info);
    QWidget* createStandaloneEditorPanel(const QString& type);
    QWidget* activeStandaloneEditorPanel(const QString& type,
                                         bool includeClosed = true) const;
    QWidget* openStandaloneEditor(const QString& type, ProjectItem* item);
    void pushSelectionToStandaloneEditors(ProjectItem* item,
                                          StandaloneEditorPushReason reason);

    InspectorPanel* createInspectorPanel(PreferredOpenArea placement = PreferredOpenArea::CentralTab);
    void closeInspectorPanel(int panelId);
    QList<InspectorPanel*> allInspectorPanels() const;
    InspectorPanel* findInspectorPanel(int panelId) const;
    QList<TransitionEditorPanel*> allTransitionEditorPanels() const;
    QList<GridEditorPanel*> allGridEditorPanels() const;
    QList<TimelineEditorPanel*> allTimelineEditorPanels() const;

    void openItemInNewInspector(ProjectItem* item, PreferredOpenArea placement, bool syncSelection = true);
    InspectorPanel* inspectorShowingItem(ProjectItem* item, bool includeClosed = false) const;
    void openOrFocusInspectorForItem(ProjectItem* item, const PlacementContext* placement = nullptr);

    SourceEditorPanel* openSourceEditor(uint32_t sourceId);

    ExternalEditorPanel* openCurveEditor(const CurveSelection& selection);
    ExternalEditorPanel* openExternalEditor(const ExternalEditorSelection& selection);

    Hierarchy* createHierarchyPanel();
    void closeHierarchyPanel(int panelId);
    QList<Hierarchy*> allHierarchyPanels() const;
    Hierarchy* findHierarchyPanel(int panelId) const;

    QStringList workspacePanelTypes() const;
    QString workspacePanelTitle(const QString& typeId) const;
    bool createWorkspacePanel(const QString& typeId);
    QList<int> workspacePanelInstanceIds(const QString& typeId) const;
    QWidget* workspacePanelContent(const QString& typeId, int panelId) const;
    QList<QWidget*> workspacePanelContents(const QString& typeId) const;
    int lastFocusedWorkspacePanelId(const QString& typeId) const;
    QList<VNX::DockInstanceSpec> workspaceWindowInstanceSpecs() const;
    QList<QVariantMap> workspaceWindowSummaries() const;
    bool setWorkspaceWindowVisible(const QString& selector, bool visible,
                                   QVariantMap* summary = nullptr);

    int lastFocusedInspectorPanelId() const;
    InspectorPanel* activeInspectorPanel() const;
    ads::CDockWidget* activeInspectorDock() const;
    ads::CDockAreaWidget* anyInspectorDockArea(bool includeFloating = true) const;
    ads::CDockWidget* dockOfPanel(QWidget* w) const;
    bool anyInspectorVisible() const;

    int lastFocusedHierarchyPanelId() const;
    bool resetCurrentLayout();
signals:
    void startupPresentationFinished(bool shown);

public slots:
    void setLastFocusedInspectorPanelId(int panelId);
    void setLastFocusedHierarchyPanelId(int panelId);
public:

    QWidget* lastInteractedWidget() const { return lastInteractedWidget_.data(); }
    QString lastInteractionType() const { return lastInteractionType_; }
    qint64 lastInteractionTimeMs() const { return lastInteractionTimeMs_; }

    static void addRecentProject(const QString& path);

private slots:
    void onNewProject();
    void onOpenProject();
    void onNewProjectViaHub();
    void onOpenProjectViaHub();
    void onSaveProject();
    void onSaveProjectAs();
    void onEngineIntegrations();
    void onAbout();
    
    void onOpenPreferences();
    void onOpenPreferences(const QString& pageId);
    void onOpenProjectSettings();

    void onResetLayout();

    void relayoutDockSplitters(bool seedSizes);

    void onPlayPressed();
    void onPlayRequested();
    void onStopPressed();
    void onResetPressed();
    void refreshPlaybackResetEnabled();
    void onAudioError(const QString& error);
    
    void onSaveProgress(int percent, const QString& message);

private:
    void setupMenuBar();
    void setupMainToolBar();
    void setupTitleBarToggles();
    void setupStatusBar();
    void setStatusMessage(const QString& message, LogEntry::Level level = LogEntry::Info);
    void setupCentralDocks();
    void setupDockWidgets();
    void configureDockManager();
    void initializeWorkspaceWindowManager();
    void initializeStructure(std::function<void(StartupPhase)> startupProgress);
    void initializeServices();
    void initializeFinalState();
    void registerWorkspacePanelWindows();
    void registerWorkspaceSingletonFactories();
    void ensureWorkspaceSingletonShells();
    void refreshMonitorCaptureControlStrip();
    void refreshMonitorRecordingPanel();
    void closeMonitorSession();
    void applyMonitorPerformanceLiveTick(const Monitor::MonitorPerformanceLiveTick& tick);
    void handleMonitorRecordingCommand(Monitor::MonitorRecordingCommand command);
    void createPrimaryWorkspaceDocks();
    void connectWorkspaceWindowSignals();
    void configureWorkspaceDockPolicies();
    void setupAudioEngine();
    void setupUndoRedo();
    void applyDarkPalette();
    void loadSettings();
    void saveSettings();
    void completeStartupPresentation();
    void finishStartupPresentation(bool shown);
    void loadProjectLayout();
    void saveProjectLayout();
    void connectSignals();
    void enforceDisabledDocks();
    SourceEditorPanel* focusedSourceEditorPanel() const;
    void setAuditionTarget(ProjectItem* item);
    
    void initEmptyProject();
    bool maybeSave();
    void updateWindowTitle();
    void setProjectModified(bool modified);
    void syncWindowToOpenedProject(const QString& path);
    
    void generateProjectPacks(const QStringList& platforms = {},
                              const QStringList& packPaths = {},
                               bool includeUnchecked = false,
                               bool forceReexport = false, bool selfCleaning = false);
    void recordPackGenerationResult(const QString& packName, uint32_t objectID,
                                    bool success, const QString& outputPath,
                                    const QString& error, qint64 bytes,
                                    qint64 elapsedMs, const QString& platform,
                                    const QString& source = QString(),
                                    const QString& warnings = QString(),
                                    const QString& statusOverride = QString(),
                                    const QString& language = QString(),
                                    const QJsonObject* platformDefinition = nullptr,
                                    const QString& unitKey = QString(),
                                    bool canceled = false);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool startupPresentationPrepared_ = false;
    bool startupPresentationScheduled_ = false;
    bool startupPresentationCompleting_ = false;
    bool startupPresentationDone_ = false;
    bool startupStateChanged_ = false;
    QPointer<QScreen> startupScreen_;
    QSize startupNormalSize_;
    Qt::WindowState startupState_ = Qt::WindowNoState;
    QSize normalWindowSize_;
    bool initializeEmptyProject_ = true;
    QString currentProjectPath_;
    bool projectModified_ = false;
    bool projectOpenInProgress_ = false;
    
    ads::CDockWidget* blueprintDock_ = nullptr;

    BlueprintCanvasWidget* blueprintCanvas_ = nullptr;

    ads::CDockManager* dockManager_ = nullptr;

    ads::CDockWidget* projectExplorerDock_ = nullptr;
    ads::CDockWidget* assistantDock_ = nullptr;

    QPointer<PanelInstanceRegistry> panelRegistry_;
    std::unique_ptr<EditorShellRegistry> editorShellRegistry_;
    LayoutController* layoutController_ = nullptr;
    SelectionRouter* selectionRouter_ = nullptr;
    BusController* busController_ = nullptr;
    std::unique_ptr<WorkspaceWindowManager> workspaceWindowManager_;

    QHash<QString, StandaloneEditorTypeInfo> standaloneEditorTypes_;
    ProjectItem* standaloneEditorSelection_ = nullptr;

    InspectorPanel* createInspectorPanelWithId(int id, PreferredOpenArea placement, bool deferShow);
    Hierarchy* createHierarchyPanelWithId(int id, bool deferShow, bool deleteOnClose = true);
    MeterPanel* createMeterPanelWithId(int id, bool deferShow);

    QWidget* createStandaloneEditorPanelWithId(const QString& type, int id, bool deferShow);
    ads::CDockAreaWidget* inspectorBelowStandaloneEditorArea() const;
    void seedInspectorBelowEditorHeight(ads::CDockAreaWidget* inspectorArea,
                                        ads::CDockWidget* dock);
    bool isStandaloneEditorAtDefaultSlot(QWidget* editor) const;
    void setStandaloneEditorVisible(QWidget* editor, bool visible);
    void wireStandaloneEditorPin(QWidget* editor);
    ProjectItem* currentStandaloneEditorSelection() const;
    void collapseUnrelatedStandaloneEditor(const QString& type, QWidget* editor,
                                          StandaloneEditorPushReason reason);
    QWidget* workspaceSingletonContent(const QString& typeId, bool materialize) const;
    void refreshVisibleMaterializedSingletons();
    bool applyWorkspacePanelRestoreSpec(const DockInstanceSpec& spec);
    LogPanel* logPanel() const;
    PublisherPanel* publisherPanel() const;

    void wireInspectorDockCloseHandler(ads::CDockWidget* dock);

    void wireInspectorPanelSignals(InspectorPanel* panel);
    void locateProjectItem(uint32_t objectID);

    void openEditorForSlot(const QString& slot, ProjectItem* item,
                           const PlacementContext* placement);

    void showInspectorForItem(InspectorPanel* panel, ProjectItem* item);

    void refreshInspectorDockTitles();
    void refreshBoundEditorDockTitles(
        const QString& base, const QList<QWidget*>& panels,
        const std::function<ProjectItem*(QWidget*)>& boundItem);
    void refreshTransitionEditorDockTitles();
    void refreshGridEditorDockTitles();
    void refreshTimelineEditorDockTitles();

    DockInstanceSpec snapshotDockInstance(const QString& type, int id, QWidget* widget,
                                          ads::CDockWidget* dock) const;

    void wireHierarchyDockCloseHandler(ads::CDockWidget* dock);

    ProjectExplorer* projectExplorer_ = nullptr;
    AssistantPanel* assistantPanel_ = nullptr;
    QList<QPointer<QThread>> packExportThreads_;

    HarkenEditor::AuditionManager* auditionManager_ = nullptr;
    PlaybackControls* playbackControls_ = nullptr;
    ControlPanel* controlPanel_ = nullptr;
    ads::CDockWidget* controlDock_ = nullptr;
    QUuid auditionTargetGuid_;
    
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;

    IPCServer* ipcServer_ = nullptr;
    std::unique_ptr<AIAssistantService> aiService_;

    QPushButton* statusButton_ = nullptr;
    QProgressBar* progressBar_ = nullptr;

    ExternalDropDispatcher* externalDropDispatcher_ = nullptr;


    QMenu* panelsSubMenu_ = nullptr;
    HarkenEditor::Bridge::RemoteConnection* remoteConnection_ = nullptr;
    Monitor::EventCaptureController* eventCaptureController_ = nullptr;
    QPointer<RemoteConnectDialog> remoteConnectDialog_;

    MonitorPerformanceChartSource monitorPerformanceChartSource_;
    MonitorVoicesSource monitorVoicesSource_;
    MonitorPlayablesSource monitorPlayablesSource_;
    MonitorEffectsSource monitorEffectsSource_;
    MonitorBusesSource monitorBusesSource_;
    MonitorDspTimelineSource monitorDspTimelineSource_;
    MonitorLineageSource monitorLineageSource_;
    MonitorTapSelectSource monitorTapSelectSource_;
    MonitorWaveformSource monitorWaveformSource_;
    MonitorScopeSource monitorScopeSource_;
    MonitorSpectrumSource monitorSpectrumSource_;
    MonitorCursorModel monitorSharedCursor_;
    std::unique_ptr<Monitor::MonitorAdapter> monitorAdapter_;
    std::unique_ptr<Monitor::MonitorCaptureControlHost> monitorCaptureControlHost_;
    std::unique_ptr<Monitor::MonitorAdapter> monitorPerformanceLiveAdapter_;
    std::unique_ptr<Monitor::MonitorPerformanceLiveHost> monitorPerformanceLiveHost_;
    Monitor::MonitorRecordingCommand lastMonitorRecordingCommand_ =
        Monitor::MonitorRecordingCommand::None;
    QPointer<QWidget> lastInteractedWidget_;
    QString lastInteractionType_;
    qint64 lastInteractionTimeMs_ = 0;

#ifdef HARKEN_HAS_QWINDOWKIT
    VNX::TitleBarWidget*         m_titleBar{nullptr};
    QWK::WidgetWindowAgent*      m_windowAgent{nullptr};
#endif
};

}
