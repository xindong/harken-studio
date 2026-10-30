#include "Core/PropertyEditSession.h"
#include "Monitor/MonitorTypes.h"
#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"

#include "MainWindow.h"
#include "Dialogs/RemoteConnectPanel.h"
#include "RuntimeBridge/RemoteConnection.h"
#include "Monitor/EventCaptureController.h"
#include <QWindow>
#include <QLocale>
#include "Core/AudioEngine.h"
#include "Core/RuntimeSyncCoordinator.h"
#include "Tooling/Harkenizer/Core/CurrentWorldProjectAdapter.h"
#include "Core/ProjectSerializer.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectInstance.h"
#include "Core/ProjectPlatform.h"
#include "Core/ProjectUpgradeManager.h"
#include "Core/ProjectOpenApproval.h"
#include "Core/ProjectUpgradeMessages.h"
#include "Core/ShortcutManager.h"
#include "Core/UndoCommands.h"
#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"
#include "Tooling/Harkenizer/Core/ProjectPackBuildService.h"
#include "Core/PackArtifactCache.h"
#include "Core/PackGenerationRecord.h"
#include "Core/BaseDialog.h"
#include "Core/ProjectPlatformActivity.h"
#include "Core/IPC/IPCServer.h"
#include "Core/AI/AIAssistantService.h"
#include "Core/AI/AIConversationStore.h"
#include "Core/AI/AIConversationSessionStore.h"
#include "Core/AI/AIDurableMemoryService.h"
#include "Core/AI/AIDurableMemoryStore.h"
#include "Core/AI/OpenAICompatibleLLMProvider.h"
#include "Core/PanelInstanceRegistry.h"
#include "Core/EditorShellRegistry.h"
#include "Core/LayoutController.h"
#include "Core/WindowPlacement.h"
#include "Core/BuiltinLayouts.h"
#include "Core/NamedLayoutStore.h"
#include "Core/SelectionRouter.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "Core/StandaloneEditorDispatch.h"
#include "Core/BusController.h"
#include "Core/WorkspaceWindowManager.h"
#include "Core/WorkspaceDockFactory.h"
#include "Core/ActionManager.h"
#include "Core/AppSettings.h"
#include "Core/Components/VAudio.h"
#include "Core/Components/VBus.h"
#include "Core/Components/VAction.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSelection.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/MediaPool.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/AssetPipeline/AudioFormatCatalog.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/AttenuationStore.h"
#include "Core/BindingContract.h"
#include "Core/BindingEditing.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/HubLauncher.h"
#include "Dialogs/Settings/Pages/LayoutPage.h"
#include "Dialogs/Settings/PreferencesPanel.h"
#include "Dialogs/Settings/ProjectSettingsPanel.h"
#include "Dialogs/Settings/Pages/ProjectPublishPage.h"
#include "Widgets/ProjectExplorer/ProjectExplorer.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/EditorLaunchMap.h"
#include "Widgets/SourceEditor/SourceEditorPanel.h"
#include "Widgets/ExternalEditor/ExternalEditorPanel.h"
#include "Widgets/GridEditor/GridEditorPanel.h"
#include "Widgets/GridEditor/GridEditorWidget.h"
#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Widgets/TransitionEditor/TransitionEditorPanel.h"
#include "Widgets/Information/InformationPanel.h"
#include "Widgets/LogPanel/LogPanel.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/Blueprint/BlueprintPanel.h"
#include "Dialogs/NewProjectDialog.h"
#include "Dialogs/AboutDialog.h"
#include "Widgets/Assistant/AssistantPanel.h"
#include "Widgets/Publisher/PublisherPanel.h"
#include "Widgets/ControlPanel/ControlPanel.h"
#include "Widgets/PlatformBar/PlatformBar.h"
#include "Widgets/PlatformBar/PlaybackControls.h"
#include "Widgets/Common/MenuComboBox.h"
#include "Monitor/MonitorAdapter.h"
#include "Monitor/MonitorCaptureControlHost.h"
#include "Monitor/MonitorHostContext.h"
#include "Monitor/MonitorPerformanceLiveHost.h"
#include "Widgets/Monitor/MonitorPanel.h"
#include "Widgets/Monitor/MonitorPanelKeys.h"
#include "Widgets/Monitor/MonitorPerformancePanel.h"
#include "Widgets/Monitor/MonitorBusesPanel.h"
#include "Widgets/Monitor/MonitorBusesSource.h"
#include "Widgets/Monitor/MonitorDspTimelinePanel.h"
#include "Widgets/Monitor/MonitorDspTimelineSource.h"
#include "Widgets/Monitor/MonitorEffectsPanel.h"
#include "Widgets/Monitor/MonitorLineagePanel.h"
#include "Widgets/Monitor/MonitorLineageSource.h"
#include "Widgets/Meter/MeterPanel.h"
#include "Widgets/Monitor/MonitorRecordingPanel.h"
#include "Widgets/Monitor/MonitorSessionComparePanel.h"
#include "Widgets/Monitor/MonitorPlaceholderPage.h"
#include "Widgets/Monitor/MonitorScopePanel.h"
#include "Widgets/Monitor/MonitorScopeSource.h"
#include "Widgets/Monitor/MonitorTapSelectPanel.h"
#include "Widgets/Monitor/MonitorTapSelectSource.h"
#include "Widgets/Monitor/MonitorWaveformPanel.h"
#include "Widgets/Monitor/MonitorWaveformSource.h"
#include "Widgets/Monitor/MonitorSpectrumPanel.h"
#include "Widgets/Monitor/MonitorSpectrumSource.h"
#include "Widgets/Monitor/MonitorEffectsSource.h"
#include "Widgets/Monitor/MonitorPlayablesPanel.h"
#include "Widgets/Monitor/MonitorPlayablesSource.h"
#include "Widgets/Monitor/MonitorVoicesPanel.h"
#include "Widgets/Monitor/MonitorVoicesSource.h"
#include "Platform/ExternalDropDispatcher.h"
#include "Core/AuditionManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "Core/QtAdsThemeAdapter.h"
#include "Core/QtControlSkin.h"
#include <QEvent>
#include <QPixmapCache>
#include "Core/EditorBootstrap.h"
#ifdef HARKEN_HAS_QWINDOWKIT
#include <QWKWidgets/widgetwindowagent.h>
#endif

#include <QApplication>
#include <QThread>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTimer>
#include <QFileInfo>
#include <QSettings>
#include <QMessageBox>
#include <QFileDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QStyle>
#include <QPalette>
#include <QVBoxLayout>
#include <QProgressDialog>
#include <QPushButton>
#include <QToolButton>
#include <QScopedValueRollback>
#include <QFontMetrics>
#include <QIcon>
#include <QSize>
#include <QList>
#include <memory>
#include <QStandardPaths>
#include <QScreen>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QPointer>
#include <QProcess>
#include <QDesktopServices>
#include <QUrl>
#include <algorithm>
#include <functional>
#include <QtConcurrent>
#include <FloatingDockContainer.h>
#include <DockWidgetTab.h>
#include <DockSplitter.h>
#include <QShowEvent>
#include <QSet>
#include "Widgets/TitleBar/FloatingTitleBar.h"

namespace VNX {

void registerTemplateActions(ActionManager& mgr, ProjectManager* projectManager);

#if defined(HARKEN_HAS_QWINDOWKIT) && defined(Q_OS_WIN)
namespace {
void applyFramelessFloatingChrome(ads::CFloatingDockContainer* fw)
{
    if (!fw || fw->property("harkenFramelessChrome").toBool())
        return;

    auto* box = qobject_cast<QBoxLayout*>(fw->layout());
    if (!box)
        return;

    fw->setProperty("harkenFramelessChrome", true);

    auto* bar = new FloatingTitleBar(fw);
    box->insertWidget(0, bar);

    auto* agent = new QWK::WidgetWindowAgent(fw);
    agent->setup(fw);
    agent->setTitleBar(bar);
    agent->setSystemButton(QWK::WindowAgentBase::Minimize, bar->minButton());
    agent->setSystemButton(QWK::WindowAgentBase::Maximize, bar->maxButton());
    agent->setSystemButton(QWK::WindowAgentBase::Close,    bar->closeButton());

    QObject::connect(bar->minButton(), &QPushButton::clicked, fw,
                     &QWidget::showMinimized, Qt::QueuedConnection);
    QObject::connect(bar->maxButton(), &QPushButton::clicked, fw, [fw]() {
        fw->isMaximized() ? fw->showNormal() : fw->showMaximized();
    }, Qt::QueuedConnection);
    QObject::connect(bar->closeButton(), &QPushButton::clicked, fw,
                     &QWidget::close, Qt::QueuedConnection);
}
}
#endif

namespace {
class PackExportDialog final : public BaseDialog
{
public:
    explicit PackExportDialog(QWidget* parent) : BaseDialog(parent)
    {
        setObjectName(QStringLiteral("PackExportDialog"));
        setWindowTitle(MainWindow::tr("Export Packs"));
        setWindowModality(Qt::ApplicationModal);
        setAttribute(Qt::WA_DeleteOnClose);
        setMinimumWidth(460);
        auto* layout = new QVBoxLayout(this);
        stage_ = new QLabel(MainWindow::tr("Checking existing packs and output permissions..."), this);
        stage_->setObjectName(QStringLiteral("packExportStage"));
        stage_->setTextFormat(Qt::PlainText);
        stage_->setWordWrap(true);
        layout->addWidget(stage_);
        unit_ = new QLabel(this);
        unit_->setObjectName(QStringLiteral("packExportUnit"));
        unit_->setTextFormat(Qt::PlainText);
        unit_->setWordWrap(true);
        layout->addWidget(unit_);
        progress_ = new QProgressBar(this);
        progress_->setObjectName(QStringLiteral("packExportProgress"));
        progress_->setRange(0, 0);
        layout->addWidget(progress_);
        setupStandardButtons(QDialogButtonBox::Cancel);
        stop_ = buttonBox()->button(QDialogButtonBox::Cancel);
        stop_->setObjectName(QStringLiteral("packExportCancelButton"));
        stop_->setAutoDefault(false);
        layout->addWidget(buttonBox());
        auto& store = PackGenerationRecordStore::instance();
        connect(&store, &PackGenerationRecordStore::observationChanged,
                this, [this](const PackBuildObservation& observation) {
            if (!stop_->isEnabled()) return;
            if (observation.unitKey.isEmpty()) {
                QString stage;
                switch (observation.stage) {
                case PackBuildObservationStage::Preflight: stage = MainWindow::tr("Checking outputs"); break;
                case PackBuildObservationStage::PreparingMedia: stage = MainWindow::tr("Preparing media"); break;
                case PackBuildObservationStage::CompilingProject: stage = MainWindow::tr("Compiling project"); break;
                case PackBuildObservationStage::WritingPacks: stage = MainWindow::tr("Writing packs"); break;
                case PackBuildObservationStage::Publishing: stage = MainWindow::tr("Validating and publishing"); break;
                case PackBuildObservationStage::Finalizing: stage = MainWindow::tr("Finalizing outputs"); break;
                default: return;
                }
                const auto* project = ProjectManager::instance().currentProject();
                const auto* platform = project ? ProjectPlatformSemantics::find(
                    project->platforms, QUuid(observation.platform)) : nullptr;
                stage_->setText(platform ? MainWindow::tr("%1: %2").arg(platform->name, stage) : stage);
                unit_->clear();
                unitKey_.clear();
                progress_->setRange(0, 0);
                return;
            }
            platform_ = observation.platform;
            language_ = observation.language;
            unitKey_ = observation.unitKey;
            unit_->setText(language_.isEmpty() ? observation.packName
                : MainWindow::tr("%1 (%2)").arg(observation.packName, language_));
            if (observation.stage == PackBuildObservationStage::Prepared
                || observation.stage == PackBuildObservationStage::ReadyToPublish)
                progress_->setRange(0, 0);
            else
                refreshProgress();
        });
        connect(&store, &PackGenerationRecordStore::progressChanged, this,
            [this](const QString& platform, const QString& language, const QString& unitKey) {
                if (!stop_->isEnabled() || platform != platform_ || language != language_
                    || unitKey != unitKey_)
                    return;
                refreshProgress();
            });
        connect(&store, &PackGenerationRecordStore::exportCancellationRequested, this, [this] {
            stage_->setText(MainWindow::tr("Canceling pack export..."));
            progress_->setRange(0, 0);
            stop_->setEnabled(false);
        });
        connect(&store, &PackGenerationRecordStore::exportActivityChanged, this,
            [this](bool exporting) {
                if (!exporting) accept();
            });
    }

    void reject() override
    {
        PackGenerationRecordStore::instance().cancelExport();
    }

    void done(int result) override
    {
        if (PackGenerationRecordStore::instance().isExporting()) reject();
        else BaseDialog::done(result);
    }

protected:
    void closeEvent(QCloseEvent* event) override
    {
        if (PackGenerationRecordStore::instance().isExporting()) {
            event->ignore();
            reject();
        } else {
            BaseDialog::closeEvent(event);
        }
    }

private:
    void refreshProgress()
    {
        const auto* progress = PackGenerationRecordStore::instance().findProgress(platform_, language_, unitKey_);
        if (!progress || (progress->stage != PackGenerationStage::Preparing
            && progress->stage != PackGenerationStage::Writing))
            return;
        progress_->setRange(0, 100);
        progress_->setValue(progress->percent);
    }

    QLabel* stage_ = nullptr;
    QLabel* unit_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* stop_ = nullptr;
    QString platform_;
    QString language_;
    QString unitKey_;
};

QString aiConversationScope(const ProjectData* project)
{
    if (!project || project->projectPath.trimmed().isEmpty())
        return QStringLiteral("user:default");

    const QFileInfo info(project->projectPath);
    QString normalizedPath = info.canonicalFilePath();
    if (normalizedPath.isEmpty())
        normalizedPath = info.absoluteFilePath();
    normalizedPath = QDir::cleanPath(normalizedPath);
#if defined(Q_OS_WIN)
    normalizedPath = normalizedPath.toCaseFolded();
#endif
    return QStringLiteral("project:%1").arg(normalizedPath);
}

void startupTrace(const QString& message)
{
    if (!qEnvironmentVariableIsSet("HARKEN_STARTUP_TRACE"))
        return;
    QDir().mkpath(QStringLiteral(".temp"));
    QFile f(QStringLiteral(".temp/editor_startup_trace.log"));
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    const QString line = QStringLiteral("%1 %2\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), message);
    f.write(line.toUtf8());
}
}

MainWindow::MainWindow(
    QWidget* parent,
    std::function<void(StartupPhase)> startupProgress,
    InitializationMode initializationMode)
    : QMainWindow(parent)
{
    setWindowTitle("Harken Studio");
    setMinimumSize(800, 600);
    resize(kDefaultStudioWindowSize);
    setObjectName(QStringLiteral("MainWindow"));
    setAcceptDrops(true);
    qApp->installEventFilter(this);

    initializeEmptyProject_ = initializationMode == InitializationMode::Immediate;
    if (initializationMode == InitializationMode::Deferred)
        return;

    initializeStructure(startupProgress);
    initializeServices();
    initializeFinalState();
}

void MainWindow::initializeStructure(
    std::function<void(StartupPhase)> startupProgress)
{
    if (startupProgress)
        startupProgress(StartupPhase::CreatingWorkspace);
    startupTrace(QStringLiteral("MainWindow: ctor begin"));
    VNX::initializeEditorSingletons();
    VNX::registerBuiltinInspectorCustomizations();
    connect(&PackGenerationRecordStore::instance(), &PackGenerationRecordStore::exportActivityChanged,
            this, [this](bool active) {
        if (!active) return;
        auto* dialog = new PackExportDialog(this);
        dialog->show();
        dialog->raise();
    });

    panelRegistry_ = new PanelInstanceRegistry(this);
    layoutController_ = new LayoutController(this);
    selectionRouter_ = new SelectionRouter(this);
    auto* referenceNavigation = new ObjectRefNavigation(this);
    connect(referenceNavigation, &ObjectRefNavigation::locateRequested, this,
        [this](RefDomain domain, uint32_t id) {
            switch (domain) {
            case RefDomain::ProjectObject:
                locateProjectItem(id);
                break;
            case RefDomain::AudioAsset:
                if (projectExplorer_ && projectExplorerDock_
                    && AssetRegistry::instance().contains(id)) {
                    revealDock(projectExplorerDock_);
                    projectExplorer_->pingSource(id);
                }
                break;
            }
        });
    connect(referenceNavigation, &ObjectRefNavigation::navigateRequested, this,
        [this](RefDomain domain, uint32_t id) {
            switch (domain) {
            case RefDomain::ProjectObject:
                if (auto* item = ProjectManager::instance().findItemByObjectID(id))
                    selectionRouter_->selectItem(item, true);
                break;
            case RefDomain::AudioAsset:
                openSourceEditor(id);
                break;
            }
        });
    busController_ = new BusController(this);
    {
        PanelInstanceRegistry::PanelTypeInfo inspectorInfo;
        inspectorInfo.label = tr("Inspector");
        inspectorInfo.dockObjectPrefix = QStringLiteral("InspectorDock");
        inspectorInfo.create = [this]() { createInspectorPanel(); };
        inspectorInfo.recreate = [this](const VNX::DockInstanceSpec& s) {
            if (findInspectorPanel(s.instanceId)) {
                applyWorkspacePanelRestoreSpec(s);
                return;
            }
            if (createInspectorPanelWithId(s.instanceId, PreferredOpenArea::Float, true))
                applyWorkspacePanelRestoreSpec(s);
        };
        panelRegistry_->registerPanelType(QStringLiteral("Inspector"), std::move(inspectorInfo));
    }
    {
        PanelInstanceRegistry::PanelTypeInfo hierarchyInfo;
        hierarchyInfo.label = tr("Hierarchy");
        hierarchyInfo.dockObjectPrefix = QStringLiteral("HierarchyDock");
        hierarchyInfo.create = [this]() { createHierarchyPanel(); };
        hierarchyInfo.recreate = [this](const VNX::DockInstanceSpec& s) {
            if (!findHierarchyPanel(s.instanceId))
                createHierarchyPanelWithId(s.instanceId, true, s.deleteOnClose);
            applyWorkspacePanelRestoreSpec(s);
        };
        panelRegistry_->registerPanelType(QStringLiteral("Hierarchy"), std::move(hierarchyInfo));
    }
    {
        PanelInstanceRegistry::PanelTypeInfo meterInfo;
        meterInfo.label = tr("Meter");
        meterInfo.dockObjectPrefix = QStringLiteral("MeterDock");
        meterInfo.create = [this]() {
            createMeterPanelWithId(panelRegistry_->allocatePanelId(QStringLiteral("Meter")), false);
        };
        meterInfo.recreate = [this](const DockInstanceSpec& spec) {
            if (!panelRegistry_->findPanel(spec.typeId, spec.instanceId))
                createMeterPanelWithId(spec.instanceId, true);
            applyWorkspacePanelRestoreSpec(spec);
        };
        panelRegistry_->registerPanelType(QStringLiteral("Meter"), std::move(meterInfo));
    }
    {
        StandaloneEditorTypeInfo info;
        info.label = tr("Source Viewer");
        info.dockObjectPrefix = SourceEditorPanel::dockObjectPrefix();
        info.createConfigured = [this](int id) -> QWidget* {
            auto* editor = new SourceEditorPanel(this);
            editor->setPanelId(id);
            editor->setAuditionManager(auditionManager_);
            connect(editor, &SourceEditorPanel::panelFocused, this, [this](int panelId) {
                panelRegistry_->setLastFocusedPanelId(SourceEditorPanel::typeId(), panelId);
            });
            wireStandaloneEditorPin(editor);
            return editor;
        };
        info.isPinned = [](QWidget* widget) {
            auto* editor = qobject_cast<SourceEditorPanel*>(widget);
            return editor && editor->isPinned();
        };
        info.setPinned = [](QWidget* widget, bool pinned) {
            if (auto* editor = qobject_cast<SourceEditorPanel*>(widget))
                editor->setPinned(pinned);
        };
        info.snapshotParams = [](QWidget* widget) -> QVariantMap {
            auto* editor = qobject_cast<SourceEditorPanel*>(widget);
            return editor ? editor->snapshotState() : QVariantMap{};
        };
        info.restoreParams = [](QWidget* widget, const QVariantMap& params) {
            auto* editor = qobject_cast<SourceEditorPanel*>(widget);
            if (!editor)
                return;
            const uint32_t sourceId = params.value(QStringLiteral("sourceId")).toUInt();
            if (sourceId != 0)
                editor->restoreState(params);
            else
                editor->clearEditor();
        };
        registerStandaloneEditorType(SourceEditorPanel::typeId(), std::move(info));
    }
    {
        StandaloneEditorTypeInfo info;
        info.label = tr("External Editor");
        info.clearOnUnrelatedSelection = false;
        info.dockObjectPrefix = QStringLiteral("CurveEditorDock");
        info.createConfigured = [this](int id) -> QWidget* {
            auto* ed = new ExternalEditorPanel(this);
            ed->setPanelId(id);
            connect(ed, &ExternalEditorPanel::panelFocused, this, [this](int panelId) {
                panelRegistry_->setLastFocusedPanelId(QStringLiteral("ExternalEditor"), panelId);
            });
            connect(ed, &ExternalEditorPanel::navigateRequested, this, [this](const QUuid& guid) {
                if (auto* item = ProjectManager::instance().findItemByGuid(guid))
                    selectionRouter_->selectItem(item, true);
            }, Qt::QueuedConnection);
            wireStandaloneEditorPin(ed);
            return ed;
        };
        info.isPinned = [](QWidget* w) {
            auto* ed = qobject_cast<ExternalEditorPanel*>(w);
            return ed && ed->isPinned();
        };
        info.setPinned = [](QWidget* w, bool p) {
            if (auto* ed = qobject_cast<ExternalEditorPanel*>(w)) ed->setPinned(p);
        };
        info.snapshotParams = [](QWidget* w) -> QVariantMap {
            auto* ed = qobject_cast<ExternalEditorPanel*>(w);
            return ed ? ed->snapshotState() : QVariantMap{};
        };
        info.restoreParams = [](QWidget* w, const QVariantMap& p) {
            if (auto* ed = qobject_cast<ExternalEditorPanel*>(w))
                ed->restoreState(p);
        };
        info.bind = [](QWidget* w, ProjectItem* item) {
            if (auto* ed = qobject_cast<ExternalEditorPanel*>(w))
                ed->bindItem(item);
        };
        info.boundItem = [](QWidget* w) -> ProjectItem* {
            auto* ed = qobject_cast<ExternalEditorPanel*>(w);
            return ed ? ed->boundItem() : nullptr;
        };
        info.clear = [](QWidget* w) {
            if (auto* ed = qobject_cast<ExternalEditorPanel*>(w))
                ed->clearEditor();
        };
        registerStandaloneEditorType(QStringLiteral("ExternalEditor"), std::move(info));
    }
    {
        StandaloneEditorTypeInfo info;
        info.label = tr("Selection Editor");
        info.openBelowCentralArea = true;
        info.clearOnUnrelatedSelection = false;
        info.dockObjectPrefix = QStringLiteral("GridEditorDock");
        info.createConfigured = [this](int id) -> QWidget* {
            auto* ed = new GridEditorPanel(this);
            ed->setPanelId(id);
            connect(ed, &GridEditorPanel::panelFocused, this, [this](int panelId) {
                panelRegistry_->setLastFocusedPanelId(QStringLiteral("GridEditor"), panelId);
            });
            connect(ed, &GridEditorPanel::navigateToItem, this, [this](uint32_t objectID) {
                if (ProjectItem* item = ProjectManager::instance().findItemByObjectID(objectID)) {
                    if (selectionRouter_)
                        selectionRouter_->selectItem(item, true);
                    else if (projectExplorer_)
                        projectExplorer_->selectProjectItem(item);
                }
            });
            connect(ed, &GridEditorPanel::locateItem, this, &MainWindow::locateProjectItem);
            connect(ed, &GridEditorPanel::auditionTargetRequested, this,
                    [this](const QUuid& target) {
                setAuditionTarget(ProjectManager::instance().findItemByGuid(target));
            });
            connect(ed, &GridEditorPanel::sourceEditorRequested, this,
                    [this](const QUuid& soundGuid, const QUuid& entryId) {
                openExternalEditor(ExternalEditorSelection::source(soundGuid, entryId));
            });
            wireStandaloneEditorPin(ed);
            return ed;
        };
        info.bind = [this](QWidget* w, ProjectItem* item) {
            if (auto* ed = qobject_cast<GridEditorPanel*>(w))
                ed->bindItem(item);
            refreshGridEditorDockTitles();
        };
        info.boundItem = [](QWidget* w) -> ProjectItem* {
            auto* ed = qobject_cast<GridEditorPanel*>(w);
            return ed ? ed->boundItem() : nullptr;
        };
        info.isPinned = [](QWidget* w) {
            auto* ed = qobject_cast<GridEditorPanel*>(w);
            return ed && ed->isPinned();
        };
        info.setPinned = [](QWidget* w, bool pinned) {
            if (auto* ed = qobject_cast<GridEditorPanel*>(w))
                ed->setPinned(pinned);
        };
        info.accepts = [](const ProjectItem* item) {
            return GridEditorPanel::acceptsItem(item);
        };
        info.clear = [](QWidget* w) {
            if (auto* ed = qobject_cast<GridEditorPanel*>(w))
                ed->clearEditor();
        };
        info.snapshotParams = [](QWidget* w) -> QVariantMap {
            QVariantMap m;
            if (auto* ed = qobject_cast<GridEditorPanel*>(w)) {
                if (!ed->currentItemGuid().isNull())
                    m["itemGuid"] = ed->currentItemGuid().toString(QUuid::WithoutBraces);
            }
            return m;
        };
        info.restoreParams = [](QWidget* w, const QVariantMap& p) {
            auto* ed = qobject_cast<GridEditorPanel*>(w);
            if (!ed)
                return;
            const QUuid guid = QUuid::fromString(p.value("itemGuid").toString());
            if (guid.isNull())
                return;
            if (ProjectItem* item = ProjectManager::instance().findItemByGuid(guid))
                ed->bindItem(item);
        };
        registerStandaloneEditorType(QStringLiteral("GridEditor"), std::move(info));
    }
    {
        StandaloneEditorTypeInfo info;
        info.label = tr("Timeline Editor");
        info.openBelowCentralArea = true;
        info.clearOnUnrelatedSelection = false;
        info.dockObjectPrefix = QStringLiteral("TimelineEditorDock");
        info.createConfigured = [this](int id) -> QWidget* {
            auto* editor = new TimelineEditorPanel(this);
            editor->setPanelId(id);
            connect(editor, &TimelineEditorPanel::panelFocused, this, [this](int panelId) {
                panelRegistry_->setLastFocusedPanelId(QStringLiteral("TimelineEditor"), panelId);
            });
            connect(editor, &TimelineEditorPanel::laneSelectionRequested, this,
                    [this](ProjectItem* track, const PlacementContext& context) {
                if (selectionRouter_)
                    selectionRouter_->selectItem(track, false, context);
            });
            connect(editor, &TimelineEditorPanel::clipSelectionRequested, this,
                    [this](ProjectItem* target, const PlacementContext& context) {
                if (selectionRouter_)
                    selectionRouter_->selectItem(target, false, context);
            });
            connect(editor, &TimelineEditorPanel::blueprintRequested, this,
                    [this](ProjectItem* subject, const PlacementContext& context) {
                if (blueprintDock_)
                    revealDock(blueprintDock_);
                if (selectionRouter_)
                    selectionRouter_->selectItem(subject, false, context);
            });
            wireStandaloneEditorPin(editor);
            return editor;
        };
        info.bind = [this](QWidget* widget, ProjectItem* item) {
            if (auto* editor = qobject_cast<TimelineEditorPanel*>(widget))
                editor->bindItem(item);
            refreshTimelineEditorDockTitles();
        };
        info.boundItem = [](QWidget* widget) -> ProjectItem* {
            auto* editor = qobject_cast<TimelineEditorPanel*>(widget);
            return editor ? editor->boundItem() : nullptr;
        };
        info.isPinned = [](QWidget* widget) {
            auto* editor = qobject_cast<TimelineEditorPanel*>(widget);
            return editor && editor->isPinned();
        };
        info.setPinned = [](QWidget* widget, bool pinned) {
            if (auto* editor = qobject_cast<TimelineEditorPanel*>(widget))
                editor->setPinned(pinned);
        };
        info.accepts = [](const ProjectItem* item) {
            return TimelineEditorPanel::acceptsItem(item);
        };
        info.clear = [](QWidget* widget) {
            if (auto* editor = qobject_cast<TimelineEditorPanel*>(widget))
                editor->clearEditor();
        };
        info.snapshotParams = [](QWidget* widget) -> QVariantMap {
            auto* editor = qobject_cast<TimelineEditorPanel*>(widget);
            return editor ? editor->snapshotState() : QVariantMap{};
        };
        info.restoreParams = [](QWidget* widget, const QVariantMap& params) {
            if (auto* editor = qobject_cast<TimelineEditorPanel*>(widget))
                editor->restoreState(params);
        };
        registerStandaloneEditorType(QStringLiteral("TimelineEditor"), std::move(info));
    }
    {
        StandaloneEditorTypeInfo info;
        info.label = tr("Transition Editor");
        info.dockObjectPrefix = QStringLiteral("TransitionEditorDock");
        info.createConfigured = [this](int id) -> QWidget* {
            auto* editor = new TransitionEditorPanel(this);
            editor->setPanelId(id);
            connect(editor, &TransitionEditorPanel::panelFocused, this, [this](int panelId) {
                panelRegistry_->setLastFocusedPanelId(QStringLiteral("TransitionEditor"), panelId);
            });
            connect(editor, &TransitionEditorPanel::watchViewChanged, this,
                    [this, editor](const TransitionSemantics::TransitionWatchView& view) {
                if (blueprintCanvas_ && editor->isVisible())
                    blueprintCanvas_->setTransitionWatch(view);
            });
            wireStandaloneEditorPin(editor);
            return editor;
        };
        info.bind = [](QWidget* widget, ProjectItem* item) {
            if (auto* editor = qobject_cast<TransitionEditorPanel*>(widget))
                editor->bindItem(item);
        };
        info.boundItem = [](QWidget* widget) -> ProjectItem* {
            auto* editor = qobject_cast<TransitionEditorPanel*>(widget);
            return editor ? editor->boundItem() : nullptr;
        };
        info.isPinned = [](QWidget* widget) {
            auto* editor = qobject_cast<TransitionEditorPanel*>(widget);
            return editor && editor->isPinned();
        };
        info.setPinned = [](QWidget* widget, bool pinned) {
            if (auto* editor = qobject_cast<TransitionEditorPanel*>(widget))
                editor->setPinned(pinned);
        };
        info.accepts = [](const ProjectItem* item) {
            return TransitionEditorPanel::acceptsItem(item);
        };
        info.clear = [](QWidget* widget) {
            if (auto* editor = qobject_cast<TransitionEditorPanel*>(widget))
                editor->clearEditor();
        };
        info.snapshotParams = [](QWidget* widget) -> QVariantMap {
            auto* editor = qobject_cast<TransitionEditorPanel*>(widget);
            return editor ? editor->snapshotState() : QVariantMap{};
        };
        info.restoreParams = [](QWidget* widget, const QVariantMap& params) {
            if (auto* editor = qobject_cast<TransitionEditorPanel*>(widget))
                editor->restoreState(params);
        };
        registerStandaloneEditorType(QStringLiteral("TransitionEditor"), std::move(info));
    }
    applyDarkPalette();
    
    remoteConnection_ = new HarkenEditor::Bridge::RemoteConnection(this);
    eventCaptureController_ = new Monitor::EventCaptureController(remoteConnection_, this);
    setupMenuBar();
    setupStatusBar();
    setupMainToolBar();
    configureDockManager();
    setupCentralDocks();
    setupDockWidgets();
    startupTrace(QStringLiteral("MainWindow: after setupDockWidgets"));
    if (startupProgress)
        startupProgress(StartupPhase::StartingServices);

    connect(layoutController_, &LayoutController::defaultLayoutSeeded, this, [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            auto syncZoneBtn = [this](const char* objName, ads::CDockWidget* rep) {
                if (!rep) return;
                if (auto* btn = findChild<QToolButton*>(QString::fromLatin1(objName))) {
                    btn->blockSignals(true);
                    btn->setChecked(!rep->isClosed());
                    btn->blockSignals(false);
                }
            };
            syncZoneBtn("leftSidebarToggleBtn", projectExplorerDock_);
            syncZoneBtn("bottomPanelToggleBtn", controlDock_);
            syncZoneBtn("rightSidebarToggleBtn", assistantDock_);
        }, Qt::QueuedConnection);
    });
    connect(layoutController_, &LayoutController::statusMessage, this, [this](const QString& text) {
        if (statusButton_) setStatusMessage(text);
    });
    connect(layoutController_, &LayoutController::dockStateRestored, this, [this]() {
        if (!externalDropDispatcher_) return;
        QTimer::singleShot(0, externalDropDispatcher_,
                           [d = externalDropDispatcher_]() { d->refresh(); });
    });
    connect(selectionRouter_, &SelectionRouter::statusMessage, this, [this](const QString& text) {
        if (statusButton_) setStatusMessage(text);
    });
    connect(selectionRouter_, &SelectionRouter::selectionChanged, this,
            [this](ProjectItem* item) {
        if (!item || item->type != ProjectItem::Type::Bus)
            busController_->clearSelection();
        pushSelectionToStandaloneEditors(item, StandaloneEditorPushReason::SelectionChanged);
    });
    connect(selectionRouter_, &SelectionRouter::auditionTargetRequested,
            this, &MainWindow::setAuditionTarget);
    {
        LayoutController::Docks d;
        d.dockManager          = dockManager_;
        d.blueprintDock        = blueprintDock_;
        d.projectExplorerDock  = projectExplorerDock_;
        d.assistantDock        = assistantDock_;
        d.controlDock          = controlDock_;
        layoutController_->setDocks(d);
        layoutController_->setWindowDockResolver([this](const QString& typeId) -> ads::CDockWidget* {
            return workspaceWindowManager_ ? workspaceWindowManager_->singletonDock(typeId) : nullptr;
        });
    }
    {
        SelectionRouter::Views v;
        v.blueprintCanvas        = blueprintCanvas_;
        v.projectExplorer        = projectExplorer_;
        v.blueprintDock          = blueprintDock_;
        selectionRouter_->setViews(v);
    }
    {
        BusController::Views v;
        v.blueprintCanvas    = blueprintCanvas_;
        v.blueprintDock      = blueprintDock_;
        busController_->setViews(v);
    }
    layoutController_->setInstanceSnapshotProvider([this]() -> QList<VNX::DockInstanceSpec> {
        return workspaceWindowManager_
            ? workspaceWindowManager_->instanceSpecs()
            : QList<VNX::DockInstanceSpec>();
    });
    layoutController_->setInstanceRestoreFactory([this](const VNX::DockInstanceSpec& spec) {
        return workspaceWindowManager_
            ? workspaceWindowManager_->restoreInstance(spec)
            : panelRegistry_->recreate(spec);
    });
    layoutController_->setSingletonSnapshotProvider([this] {
        return workspaceWindowManager_->singletonStates();
    });
    layoutController_->setSingletonStateRestorer([this](const QVariantMap& states) {
        workspaceWindowManager_->restoreSingletonStates(states);
    });
    layoutController_->setVisibleSingletonMaterializer([this]() {
        refreshVisibleMaterializedSingletons();
    });
    layoutController_->setInstanceCloser([this]() {
        if (!workspaceWindowManager_)
            return;
        const QList<WorkspaceWindowInstanceEntry> entries =
            workspaceWindowManager_->registry().instanceEntries();
        for (const WorkspaceWindowInstanceEntry& entry : entries)
            workspaceWindowManager_->closeInstance(entry.typeId, entry.instanceId);
        qApp->sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    connect(layoutController_, &LayoutController::manageLayoutsRequested, this, [this]() {
        onOpenPreferences(QString::fromLatin1(kSettingsPageLayout));
    });
}

void MainWindow::initializeServices()
{
    startupTrace(QStringLiteral("MainWindow: before setupAudioEngine"));
    setupAudioEngine();
    startupTrace(QStringLiteral("MainWindow: after setupAudioEngine"));
    setupUndoRedo();
    connectSignals();
}

void MainWindow::initializeFinalState()
{
    externalDropDispatcher_ = new ExternalDropDispatcher(this);
    externalDropDispatcher_->install(this);

    loadSettings();

    if (initializeEmptyProject_)
        initEmptyProject();
    
    setStatusMessage(tr("Ready"));
    
    layoutController_->applyDefaultLayout();
    startupTrace(QStringLiteral("MainWindow: after applyDefaultLayout"));
    
    LogPanel::logInfo(LogCategory::GUI, tr("Harken Studio started successfully"));
    startupTrace(QStringLiteral("MainWindow: ctor ready"));

#ifdef HARKEN_HAS_QWINDOWKIT
    m_titleBar    = new VNX::TitleBarWidget(this);
    connect(m_titleBar, &QObject::destroyed, this, [this]() { m_titleBar = nullptr; });
    m_windowAgent = new QWK::WidgetWindowAgent(this);
    m_windowAgent->setup(this);
    m_windowAgent->setTitleBar(m_titleBar);
    m_windowAgent->setSystemButton(QWK::WindowAgentBase::Minimize, m_titleBar->minButton());
    m_windowAgent->setSystemButton(QWK::WindowAgentBase::Maximize, m_titleBar->maxButton());
    m_windowAgent->setSystemButton(QWK::WindowAgentBase::Close,    m_titleBar->closeButton());

    connect(m_titleBar->minButton(),   &QPushButton::clicked, this,
            &QMainWindow::showMinimized, Qt::QueuedConnection);
    connect(m_titleBar->maxButton(),   &QPushButton::clicked, this, [this]() {
        isMaximized() ? showNormal() : showMaximized();
    }, Qt::QueuedConnection);
    connect(m_titleBar->closeButton(), &QPushButton::clicked, this,
            &QMainWindow::close, Qt::QueuedConnection);

    m_titleBar->embedMenuBar(this->menuBar());
    if (auto* mb = m_titleBar->menuBar())
        m_windowAgent->setHitTestVisible(mb, true);

    setMenuWidget(m_titleBar);

    setupTitleBarToggles();
    updateWindowTitle();
#endif
}

void MainWindow::initializeDeferred(
    std::function<void(StartupPhase)> startupProgress,
    std::function<void()> finished,
    std::function<bool()> cancelled)
{
    QTimer::singleShot(0, this, [this, startupProgress, finished = std::move(finished), cancelled]() mutable {
        if (cancelled && cancelled())
            return;
        initializeStructure(startupProgress);
        QTimer::singleShot(0, this, [this, finished = std::move(finished), cancelled]() mutable {
            if (cancelled && cancelled())
                return;
            initializeServices();
            QTimer::singleShot(0, this, [this, finished = std::move(finished), cancelled]() mutable {
                if (cancelled && cancelled())
                    return;
                initializeFinalState();
                if (finished)
                    finished();
            });
        });
    });
}

MainWindow::~MainWindow()
{
    PackGenerationRecordStore::instance().cancelExport();
    for (const auto& thread : packExportThreads_)
        if (thread) thread->wait();
    delete eventCaptureController_;
    eventCaptureController_ = nullptr;
    closeMonitorSession();
    if (auto* panel = qobject_cast<MonitorPerformancePanel*>(
            monitorPerformanceChartSource_.panel()))
        panel->setSharedCursor(nullptr);
    if (auto* panel = qobject_cast<MonitorVoicesPanel*>(monitorVoicesSource_.panel()))
        panel->setSharedCursor(nullptr);
    if (auto* panel = qobject_cast<MonitorPlayablesPanel*>(monitorPlayablesSource_.panel()))
        panel->setSharedCursor(nullptr);
    if (auto* panel = qobject_cast<MonitorEffectsPanel*>(monitorEffectsSource_.panel()))
        panel->setSharedCursor(nullptr);
    if (auto* panel = qobject_cast<MonitorBusesPanel*>(monitorBusesSource_.panel()))
        panel->setSharedCursor(nullptr);
    if (auto* panel = qobject_cast<MonitorDspTimelinePanel*>(monitorDspTimelineSource_.panel()))
        panel->setSharedCursor(nullptr);
    if (auto* panel = qobject_cast<MonitorLineagePanel*>(monitorLineageSource_.panel()))
        panel->setSharedCursor(nullptr);
    if (auto* panel = qobject_cast<MonitorTapSelectPanel*>(monitorTapSelectSource_.panel()))
        panel->setSharedCursor(nullptr);
    if (auto* panel = qobject_cast<MonitorWaveformPanel*>(monitorWaveformSource_.panel()))
        panel->setSharedCursor(nullptr);
    if (auto* panel = qobject_cast<MonitorScopePanel*>(monitorScopeSource_.panel()))
        panel->setSharedCursor(nullptr);
    if (auto* panel = qobject_cast<MonitorSpectrumPanel*>(monitorSpectrumSource_.panel()))
        panel->setSharedCursor(nullptr);
    AudioEngine::instance().shutdown();
}

void MainWindow::setupAudioEngine()
{
    startupTrace(QStringLiteral("MainWindow: setupAudioEngine begin"));
    AudioEngine& engine = AudioEngine::instance();
    const auto refreshEventProject = [this] {
        eventCaptureController_->setProjectObjects(
            ProjectObjectIndex::fromProject(ProjectManager::instance().currentProject()));
    };
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened, eventCaptureController_, refreshEventProject);
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, eventCaptureController_, refreshEventProject);
    connect(&ProjectManager::instance(), &ProjectManager::itemRenamed, eventCaptureController_, refreshEventProject);
    connect(&ProjectManager::instance(), &ProjectManager::itemCreated, eventCaptureController_, refreshEventProject);
    connect(&ProjectManager::instance(), &ProjectManager::itemDeleted, eventCaptureController_,
            [this](ProjectItem* item) {
        eventCaptureController_->setProjectObjects(ProjectObjectIndex::fromProject(
            ProjectManager::instance().currentProject(), item));
    });
    connect(&ProjectManager::instance(), &ProjectManager::itemMoved, eventCaptureController_, refreshEventProject);
    connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged, eventCaptureController_, refreshEventProject);
    connect(&ProjectManager::instance(), &ProjectManager::externalFilesModified, eventCaptureController_, refreshEventProject);
    connect(&engine, &AudioEngine::initializedChanged, eventCaptureController_, [this](bool initialized) {
        eventCaptureController_->setLocalRuntimeAvailable(initialized && !AudioEngine::instance().isSimulationMode());
    });
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, eventCaptureController_, [this] {
        if (eventCaptureController_->target() == Monitor::EventCaptureTarget::Local)
            eventCaptureController_->stop();
    });
    
    connect(&engine, &AudioEngine::errorOccurred, this, &MainWindow::onAudioError, Qt::QueuedConnection);
    connect(&engine, &AudioEngine::logMessage, this, [this](int level, const QString& msg) {
        LogPanel::logEngineMessage(level, msg);
    }, Qt::QueuedConnection);
    
    startupTrace(QStringLiteral("MainWindow: before AudioEngine::initialize"));
    const bool engineInitialized = engine.initialize();
    eventCaptureController_->setLocalRuntimeAvailable(engineInitialized && !engine.isSimulationMode());
    refreshEventProject();
    startupTrace(QStringLiteral("MainWindow: after AudioEngine::initialize"));

    if (engineInitialized)
    {
        if (engine.isSimulationMode())
        {
            setStatusMessage(tr("Audio engine running in SIMULATION MODE (runtime not available)"));
            if (auto* log = logPanel())
                log->logWarning(LogCategory::Audio, tr("Audio runtime not available - audio playback disabled"));
        }
        else
        {
            setStatusMessage(tr("Audio engine initialized"));
            if (auto* log = logPanel())
                log->logInfo(LogCategory::Audio, tr("Audio runtime initialized successfully"));
        }
    }
    else
    {
        setStatusMessage(tr("Audio engine failed to initialize"));
        if (auto* log = logPanel())
            log->logError(LogCategory::Audio, tr("Failed to initialize audio engine"));
    }
    
    startupTrace(QStringLiteral("MainWindow: before editor IPC start"));
    ipcServer_ = new IPCServer(&engine, this);
    ipcServer_->setMainWindow(this);
    ipcServer_->setProjectManager(&ProjectManager::instance());
    ipcServer_->setAppState(IPCServer::AppState::Ready);
    if (ipcServer_->start())
    {
        if (auto* log = logPanel())
            log->logInfo(LogCategory::MCP, tr("IPC Server started on port %1").arg(ipcServer_->port()));
    }
    else
    {
        if (auto* log = logPanel())
            log->logWarning(LogCategory::MCP, tr("Failed to start IPC Server"));
    }
    startupTrace(QStringLiteral("MainWindow: after editor IPC start"));

    aiService_ = std::make_unique<AIAssistantService>(ipcServer_);
    aiService_->setProvider(new OpenAICompatibleLLMProvider);
    aiService_->setConversationStore(
        std::make_shared<LocalAIConversationStore>());
    aiService_->setConversationSessionStore(
        std::make_shared<LocalAIConversationSessionStore>());
    aiService_->setDurableMemoryService(
        std::make_shared<AIDurableMemoryService>(
            std::make_shared<LocalAIDurableMemoryStore>()));
    if (assistantPanel_)
        assistantPanel_->setAssistantService(aiService_.get());
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened,
            this, [this](ProjectData* project) {
        if (aiService_)
            aiService_->setConversationScope(aiConversationScope(project));
    });
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed,
            this, [this]() {
        if (aiService_)
            aiService_->setConversationScope(
                aiConversationScope(nullptr));
        setAuditionTarget(nullptr);
    });
    connect(&ProjectManager::instance(), &ProjectManager::itemDeleted,
            this, [this](ProjectItem*) {
        if (!currentStandaloneEditorSelection())
            busController_->clearSelection();
        if (!auditionTargetGuid_.isNull() && !auditionTarget())
            setAuditionTarget(nullptr);
    });
    connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged,
            this, [this](ProjectItem* item, const QString& property, const QObject*) {
        if ((property == QStringLiteral("mode") || property == QStringLiteral("playType"))
            && item && item->guid == auditionTargetGuid_)
            refreshPlaybackResetEnabled();
        if ((property == QStringLiteral("mode") || property == QStringLiteral("playType")) && item
            && item == currentStandaloneEditorSelection())
            pushSelectionToStandaloneEditors(item, StandaloneEditorPushReason::SelectionChanged);
    });
    connect(&ProjectManager::instance(), &ProjectManager::projectSaved,
            this, [this]() {
        if (aiService_)
            aiService_->setConversationScope(aiConversationScope(
                ProjectManager::instance().currentProject()));
    });
    aiService_->setConversationScope(
        aiConversationScope(ProjectManager::instance().currentProject()));

    if (ipcServer_)
    {
        registerMixerActions(ActionManager::instance(),
                             &AudioEngine::instance(),
                             &ProjectManager::instance(),
                             this);

        registerProjectActions(ActionManager::instance(),
                              &ProjectManager::instance(),
                              projectExplorer_);

        registerBindingActions(ActionManager::instance());

        registerContainerActions(ActionManager::instance(),
                                &ProjectManager::instance());

        registerEditorActions(ActionManager::instance(),
                              &ProjectManager::instance(),
                              &AudioEngine::instance(),
                              blueprintCanvas_,
                              ipcServer_);

        registerTemplateActions(ActionManager::instance(),
                                &ProjectManager::instance());

        registerAuditionActions(ActionManager::instance(),
                                auditionManager_,
                                &AudioEngine::instance());

        registerSpatialActions(ActionManager::instance(),
                              &AudioEngine::instance(),
                              &ProjectManager::instance());

        registerAdvancedActions(ActionManager::instance(),
                               &AudioEngine::instance());

        registerAssetActions(ActionManager::instance(),
                            &AudioEngine::instance(),
                            &ProjectManager::instance());

        registerSystemActions(ActionManager::instance(),
                             &AudioEngine::instance(),
                             this,
                             ipcServer_->activePlaybacksMap());

        registerReadonlyActions(ActionManager::instance(), ipcServer_);
    }
}

void MainWindow::setupMenuBar()
{
    QMenuBar* menuBar = this->menuBar();
    
    QMenu* fileMenu = menuBar->addMenu(tr("&File"));
    
    QAction* newAction = fileMenu->addAction(tr("&New Project"));
    newAction->setObjectName(QStringLiteral("fileMenu_newProject"));
    connect(newAction, &QAction::triggered, this,
            &MainWindow::onNewProjectViaHub);
    
    QAction* openAction = fileMenu->addAction(tr("&Open Project..."));
    openAction->setObjectName(QStringLiteral("fileMenu_openProject"));
    connect(openAction, &QAction::triggered, this,
            &MainWindow::onOpenProject);
    
    QAction* saveAction = fileMenu->addAction(tr("&Save Project"));
    saveAction->setShortcutContext(Qt::ApplicationShortcut);
    ShortcutManager::instance().bindAction(Shortcuts::SaveProject, saveAction);
    connect(saveAction, &QAction::triggered, this, &MainWindow::onSaveProject);
    
    QAction* saveAsAction = fileMenu->addAction(tr("Save Project &As..."));
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::onSaveProjectAs);
    
    fileMenu->addSeparator();

    QAction* projectSettingsAction = fileMenu->addAction(tr("&Project Settings..."));
    projectSettingsAction->setObjectName(QStringLiteral("fileMenu_projectSettings"));
    projectSettingsAction->setMenuRole(QAction::NoRole);
    projectSettingsAction->setEnabled(ProjectManager::instance().isProjectOpen());
    connect(projectSettingsAction, &QAction::triggered, this, &MainWindow::onOpenProjectSettings);
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened,
            this, [projectSettingsAction](ProjectData*) {
        projectSettingsAction->setEnabled(true);
    });
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed,
            this, [projectSettingsAction]() {
        projectSettingsAction->setEnabled(false);
    });

    QAction* preferencesAction = fileMenu->addAction(tr("&Preferences..."));
    preferencesAction->setObjectName(QStringLiteral("fileMenu_preferences"));
    preferencesAction->setMenuRole(QAction::PreferencesRole);
    ShortcutManager::instance().bindAction(Shortcuts::Preferences, preferencesAction);
    connect(preferencesAction, &QAction::triggered, this,
            QOverload<>::of(&MainWindow::onOpenPreferences));

    fileMenu->addSeparator();

    QAction* exitAction = fileMenu->addAction(tr("E&xit"));
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
    
    QMenu* editMenu = menuBar->addMenu(tr("&Edit"));
    
    undoAction_ = UndoRedoManager::instance().createUndoAction(this, tr("&Undo"));
    undoAction_->setShortcutContext(Qt::ApplicationShortcut);
    ShortcutManager::instance().bindAction(Shortcuts::Undo, undoAction_);
    editMenu->addAction(undoAction_);

    redoAction_ = UndoRedoManager::instance().createRedoAction(this, tr("&Redo"));
    redoAction_->setShortcutContext(Qt::ApplicationShortcut);
    ShortcutManager::instance().bindAction(Shortcuts::Redo, redoAction_);
    editMenu->addAction(redoAction_);

    editMenu->addSeparator();
    auto& shortcuts = ShortcutManager::instance();
    for (const QString& id : {Shortcuts::Cut, Shortcuts::Copy, Shortcuts::Paste,
                              Shortcuts::Duplicate, Shortcuts::Delete})
        shortcuts.createMenuAction(id, editMenu);
    editMenu->addSeparator();
    shortcuts.createMenuAction(Shortcuts::SelectAll, editMenu);

    QMenu* viewsMenu = menuBar->addMenu(tr("&View"));
    viewsMenu->setObjectName("viewsMenu");

    connect(viewsMenu, &QMenu::aboutToShow, this, [this, viewsMenu]() {
        viewsMenu->clear();

        QSet<QString> editorPanelTypes;
        for (const QString& t : standaloneEditorTypes_.keys())
            editorPanelTypes.insert(t);

        QList<QVariantMap> singletons;
        const auto listedInViewMenu = [this](const QString& typeId) {
            const auto* descriptor = workspaceWindowManager_->catalog().find(typeId);
            return descriptor && descriptor->showInViewMenu;
        };
        for (const QVariantMap& summary : workspaceWindowSummaries()) {
            if (summary.value(QStringLiteral("kind")).toString() == QLatin1String("singleton")
                && listedInViewMenu(summary.value(QStringLiteral("typeId")).toString()))
                singletons.append(summary);
        }
        std::stable_sort(singletons.begin(), singletons.end(),
            [this](const QVariantMap& a, const QVariantMap& b) {
                return QString::compare(
                    workspacePanelTitle(a.value(QStringLiteral("typeId")).toString()),
                    workspacePanelTitle(b.value(QStringLiteral("typeId")).toString()),
                    Qt::CaseInsensitive) < 0;
            });

        for (const QVariantMap& summary : singletons) {
            if (!summary.value(QStringLiteral("available")).toBool())
                continue;
            const QString typeId = summary.value(QStringLiteral("typeId")).toString();
            const QString name = summary.value(QStringLiteral("name")).toString();
            QAction* action = viewsMenu->addAction(workspacePanelTitle(typeId));
            action->setObjectName(QStringLiteral("viewToggle_%1").arg(name));
            action->setCheckable(true);
            action->setChecked(summary.value(QStringLiteral("visible")).toBool());
            connect(action, &QAction::triggered, this, [this, typeId](bool checked) {
                setWorkspaceWindowVisible(typeId, checked);
            });
        }

        struct Creatable { QString type; QString label; std::function<void()> create; };
        QList<Creatable> creatables;
        for (const QString& t : workspacePanelTypes()) {
            if (!listedInViewMenu(t))
                continue;
            const QString label = workspacePanelTitle(t);
            if (!label.isEmpty())
                creatables.append({ t, label, [this, t]() { createWorkspacePanel(t); } });
        }

        auto addCreatable = [this](QMenu* destMenu, const Creatable& c) {
            QAction* a = destMenu->addAction(c.label);
            a->setObjectName(QStringLiteral("viewsCreate_%1").arg(c.label));
            const bool comingSoon = standaloneEditorTypes_.value(c.type).comingSoon;
            if (comingSoon) {
                a->setToolTip(tr("%1 - coming soon").arg(c.label));
                a->setEnabled(false);
                return;
            }
            a->setToolTip(tr("Open a new %1 instance").arg(c.label));
            connect(a, &QAction::triggered, this, [create = c.create]() { create(); });
        };

        QList<Creatable> topLevelCreatables, editorCreatables;
        for (const auto& c : creatables)
            (editorPanelTypes.contains(c.type) ? editorCreatables : topLevelCreatables).append(c);

        if (!singletons.isEmpty() && !topLevelCreatables.isEmpty())
            viewsMenu->addSeparator();
        for (const auto& c : topLevelCreatables)
            addCreatable(viewsMenu, c);

        if (!editorCreatables.isEmpty()) {
            QMenu* editorsMenu = viewsMenu->addMenu(tr("Editors"));
            editorsMenu->setObjectName(QStringLiteral("viewsEditorsMenu"));
            for (const auto& c : editorCreatables)
                addCreatable(editorsMenu, c);
        }
    });

    QMenu* helpMenu = menuBar->addMenu(tr("&Help"));

    QAction* manageIntegrationsAction = helpMenu->addAction(
        tr("Manage Engine &Integrations..."));
    manageIntegrationsAction->setObjectName("helpMenu_manageEngineIntegrations");
    connect(manageIntegrationsAction, &QAction::triggered,
            this, &MainWindow::onEngineIntegrations);

    helpMenu->addSeparator();

    QAction* websiteAction = helpMenu->addAction(tr("Official &Website"));
    websiteAction->setObjectName("helpMenu_viewStudioWebsite");
    connect(websiteAction, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://harkenstudio.xd.com/")));
    });

    QAction* tutorialAction = helpMenu->addAction(tr("Online &Tutorial"));
    tutorialAction->setObjectName("helpMenu_onlineTutorial");
    connect(tutorialAction, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://harkenstudio.xd.com/#/tutorial")));
    });

    QAction* repositoryAction = helpMenu->addAction(tr("GitHub &Repository"));
    repositoryAction->setObjectName("helpMenu_harkenRepository");
    connect(repositoryAction, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/xindong/harken-studio")));
    });

    QAction* privacyPolicyAction = helpMenu->addAction(tr("&Privacy Policy"));
    privacyPolicyAction->setObjectName("helpMenu_privacyPolicy");
    connect(privacyPolicyAction, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://harkenstudio.xd.com/pages/zh/privacy.html")));
    });

    QAction* termsOfUseAction = helpMenu->addAction(tr("Terms of &Use"));
    termsOfUseAction->setObjectName("helpMenu_termsOfUse");
    connect(termsOfUseAction, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://harkenstudio.xd.com/pages/zh/terms.html")));
    });

    helpMenu->addSeparator();

    QAction* aboutAction = helpMenu->addAction(tr("&About"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);
}

void MainWindow::setupMainToolBar()
{
    auto* toolbar = new PlatformBar(this);
    addToolBar(toolbar);
    playbackControls_ = new PlaybackControls(toolbar);
    toolbar->addCenterWidget(playbackControls_);
    connect(playbackControls_, &PlaybackControls::playClicked, this, &MainWindow::onPlayRequested);
    connect(playbackControls_, &PlaybackControls::stopClicked, this, &MainWindow::onStopPressed);
    connect(playbackControls_, &PlaybackControls::resetClicked, this, &MainWindow::onResetPressed);

    auto* layoutSelector = new MenuComboBox(toolbar);
    layoutSelector->setObjectName(QStringLiteral("layoutSelector"));
    layoutSelector->setMinimumContentsLength(12);
    layoutSelector->setAccessibleName(tr("Layout"));
    toolbar->addTrailingWidget(layoutSelector);
    auto* layoutsMenu = layoutSelector->menu();
    layoutsMenu->setTitle(tr("Layout"));
    layoutsMenu->setObjectName(QStringLiteral("layoutsMenu"));
    layoutController_->setLayoutsMenu(layoutsMenu);
    connect(layoutsMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildLayoutsMenu);
    const auto refreshLayout = [this, layoutSelector] {
        const QString name = layoutController_->hasNamedLayout()
            ? layoutController_->currentNamedLayout() : tr("Default");
        layoutSelector->setDisplayText(name);
        layoutSelector->setToolTip(tr("Layout: %1").arg(name));
    };
    connect(layoutController_, &LayoutController::currentLayoutChanged, layoutSelector, refreshLayout);
    layoutController_->rebuildLayoutsMenu();
    refreshLayout();

    auto* remoteAction = new QAction(tr("Remote Connect"), toolbar);
    remoteAction->setCheckable(true);
    auto* button = toolbar->addLeadingAction(remoteAction, QStringLiteral("remoteConnectButton"));

    const auto refresh = [this, button, remoteAction] {
        using HarkenEditor::Bridge::RemoteConnectionState;
        const auto state = remoteConnection_->state();
        const bool connected = state == RemoteConnectionState::Connected;
        remoteAction->setChecked(connected);
        remoteAction->setIcon(ThemeManager::instance().resolveIcon(
            QString::fromLatin1(ThemeAssetIds::RemoteConnect)).icon);
        remoteAction->setToolTip(tr("Remote Connect\n%1").arg(remoteConnection_->statusText()));
        button->setAccessibleDescription(remoteConnection_->statusText());
    };
    connect(remoteAction, &QAction::triggered, this, [this, refresh] {
        refresh();
        showRemoteConnect();
    });
    connect(remoteConnection_, &HarkenEditor::Bridge::RemoteConnection::stateChanged,
            button, refresh);
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, button, refresh);
    refresh();

    auto* captureAction = new QAction(tr("Start Capture"), toolbar);
    captureAction->setObjectName(QStringLiteral("captureAction"));
    captureAction->setCheckable(true);
    auto* captureButton = toolbar->addLeadingAction(captureAction, QStringLiteral("captureButton"));
    const auto refreshCapture = [this, captureAction, captureButton] {
        const bool capturing = eventCaptureController_->canStop();
        captureAction->setChecked(capturing);
        captureAction->setEnabled(capturing || eventCaptureController_->canStart());
        captureAction->setIcon(ThemeManager::instance().resolveIcon(QString::fromLatin1(
            ThemeAssetIds::MonitorCapture)).icon);
        const QString action = capturing ? tr("Stop Capture") : tr("Start Capture");
        captureAction->setText(action);
        captureAction->setToolTip(tr("%1\n%2\n%3").arg(action,
            capturing ? eventCaptureController_->targetText() : eventCaptureController_->availableTargetText(),
            eventCaptureController_->statusText()));
        captureButton->setAccessibleName(action);
        captureButton->setAccessibleDescription(eventCaptureController_->statusText());
    };
    connect(captureAction, &QAction::triggered, this, [this, refreshCapture] {
        if (eventCaptureController_->canStop()) eventCaptureController_->stop();
        else eventCaptureController_->start();
        refreshCapture();
    });
    connect(eventCaptureController_, &Monitor::EventCaptureController::stateChanged, captureButton, refreshCapture);
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, captureButton, refreshCapture);
    refreshCapture();
}

void MainWindow::setupTitleBarToggles()
{
#ifdef HARKEN_HAS_QWINDOWKIT
    auto makeSidebarToggle = [&](const QString& onAssetId,
                                 const QString& offAssetId,
                                 const QString& objName,
                                 const QString& tooltip, ads::CDockWidget** repPtr) {
        QToolButton* btn = new QToolButton();
        btn->setObjectName(objName);
        btn->setCheckable(true);
        btn->setChecked(true);
        btn->setToolTip(tooltip);
        btn->setIcon(ThemeManager::instance().resolveToggleIcon(
            onAssetId, offAssetId).icon);
        btn->setIconSize(QSize(20, 20));
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setAttribute(Qt::WA_Hover, true);
        btn->setProperty("shellRole", QStringLiteral("sidebarToggle"));
        btn->setProperty("flatIconButton", true);
        m_titleBar->insertControlWidget(btn);
        m_windowAgent->setHitTestVisible(btn, true);
        connect(btn, &QToolButton::toggled, this, [this, repPtr](bool checked) {
            if (!*repPtr)
                return;
            if (checked)
                layoutController_->reanchorFloatingZoneDock(*repPtr);
            layoutController_->setZoneCollapsed(*repPtr, !checked);
        });
        return btn;
    };

    auto* leftToggle = makeSidebarToggle(
        QString::fromLatin1(ThemeAssetIds::SidebarLeftOn),
        QString::fromLatin1(ThemeAssetIds::SidebarLeftOff),
        "leftSidebarToggleBtn",
        tr("Toggle Left Sidebar (Explorer)"), &projectExplorerDock_);
    auto* bottomToggle = makeSidebarToggle(
        QString::fromLatin1(ThemeAssetIds::SidebarBottomOn),
        QString::fromLatin1(ThemeAssetIds::SidebarBottomOff),
        "bottomPanelToggleBtn",
        tr("Toggle Bottom Panel (Control)"), &controlDock_);
    auto* rightToggle = makeSidebarToggle(
        QString::fromLatin1(ThemeAssetIds::SidebarRightOn),
        QString::fromLatin1(ThemeAssetIds::SidebarRightOff),
        "rightSidebarToggleBtn",
        tr("Toggle Right Sidebar (Assistant)"), &assistantDock_);
    const bool assistantAvailable = workspaceWindowManager_->isSingletonAvailable(QStringLiteral("Assistant"));
    rightToggle->setVisible(assistantAvailable);
    rightToggle->setEnabled(assistantAvailable);

    QTimer::singleShot(0, this, [this, leftToggle, bottomToggle, rightToggle]() {
        auto syncBtn = [](QToolButton* btn, ads::CDockWidget* rep) {
            if (!rep) return;
            QObject::connect(rep, &ads::CDockWidget::viewToggled, btn, [btn](bool open) {
                btn->blockSignals(true);
                btn->setChecked(open);
                btn->blockSignals(false);
            });
            btn->blockSignals(true);
            btn->setChecked(!rep->isClosed());
            btn->blockSignals(false);
        };
        syncBtn(leftToggle, projectExplorerDock_);
        syncBtn(bottomToggle, controlDock_);
        syncBtn(rightToggle, assistantDock_);
    });
#endif
}

void MainWindow::setupStatusBar()
{
    QStatusBar* status = statusBar();
    
    statusButton_ = new QPushButton(status);
    statusButton_->setObjectName(QStringLiteral("statusLogButton"));
    statusButton_->setFlat(true);
    statusButton_->setCursor(Qt::PointingHandCursor);
    statusButton_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status->addWidget(statusButton_, 1);
    setStatusMessage(tr("Ready"));
    connect(statusButton_, &QPushButton::clicked, this, [this]() {
        const QString logType = QStringLiteral("Log");
        const bool opened = workspaceWindowManager_->isSingletonVisible(logType)
            ? workspaceWindowManager_->focusSingleton(logType)
            : workspaceWindowManager_->showSingleton(logType);
        if (!opened)
            return;
        auto* dock = workspaceWindowManager_->singletonDock(logType);
        if (auto* floating = dock->floatingDockContainer()) {
            floating->setWindowState(floating->windowState() & ~Qt::WindowMinimized);
            floating->raise();
            floating->activateWindow();
        }
    });

    progressBar_ = new QProgressBar();
    progressBar_->setObjectName(QStringLiteral("statusProgressBar"));
    progressBar_->setRange(0, 100);
    progressBar_->setFixedWidth(180);
    progressBar_->setFixedHeight(16);
    progressBar_->hide();
    progressBar_->setProperty("shellRole", QStringLiteral("statusProgress"));
    status->addPermanentWidget(progressBar_);
}

void MainWindow::setStatusMessage(const QString& message, LogEntry::Level level)
{
    statusBar()->clearMessage();
    const QString summary = message.section(QLatin1Char('\n'), 0, 0).simplified();
    QString buttonText = summary;
    statusButton_->setText(buttonText.replace(QLatin1Char('&'), QStringLiteral("&&")));
    statusButton_->setAccessibleName(summary);
    statusButton_->setToolTip(tr("Open Log"));
    QtControlSkin::setSemanticProperty(statusButton_, "logLevel", LogEntry::levelToString(level));
}

void MainWindow::setupCentralDocks()
{
    auto* blueprintPanel = new BlueprintPanel();
    blueprintCanvas_ = blueprintPanel->canvas();
    connect(blueprintPanel, &BlueprintPanel::meterDetachRequested, this,
        [this](const QVariantMap& state) {
            auto* panel = createMeterPanelWithId(panelRegistry_->allocatePanelId(QStringLiteral("Meter")), false);
            panel->restoreState(state);
        });

    blueprintDock_ = new ads::CDockWidget(tr("Blueprint"));
    blueprintDock_->setObjectName(QStringLiteral("BlueprintDock"));
    blueprintDock_->setWidget(blueprintPanel, ads::CDockWidget::ForceNoScrollArea);

    controlPanel_ = new ControlPanel(this);
}

void MainWindow::showRemoteConnect()
{
    workspaceWindowManager_->showSingleton(QStringLiteral("RemoteConnect"));
}

void MainWindow::setupDockWidgets()
{
    initializeWorkspaceWindowManager();

    registerWorkspaceSingletonFactories();
    dockManager_->addDockWidget(ads::TopDockWidgetArea, blueprintDock_);
    createPrimaryWorkspaceDocks();
    ensureWorkspaceSingletonShells();
    connectWorkspaceWindowSignals();
    configureWorkspaceDockPolicies();
}

void MainWindow::configureDockManager()
{
    ads::CDockManager::setConfigFlags(ads::CDockManager::DefaultOpaqueConfig);
    ads::CDockManager::setConfigFlag(ads::CDockManager::HoveredTabHasCloseButton, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasTabsMenuButton, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasUndockButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FloatingContainerHasWidgetIcon, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaDynamicTabsMenuButtonVisibility, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::EqualSplitOnInsertion, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FloatingContainerDockViaTabOnly, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::AlwaysShowTabs, true);
    ads::CDockOverlay::setDropAreaByCursorPositionEnabled(true);
    auto& themeManager = ThemeManager::instance();
    QtAdsThemeAdapter::applyIcons(
        themeManager.assetResolver(), themeManager.snapshot());
    dockManager_ = QtAdsThemeAdapter(themeManager.snapshot()).createDockManager(this);

#if defined(HARKEN_HAS_QWINDOWKIT) && defined(Q_OS_WIN)
    connect(dockManager_, &ads::CDockManager::floatingWidgetCreated,
            this, [](ads::CFloatingDockContainer* fw) { applyFramelessFloatingChrome(fw); });
    for (auto* fw : dockManager_->floatingWidgets())
        applyFramelessFloatingChrome(fw);
#endif
}

void MainWindow::initializeWorkspaceWindowManager()
{
    workspaceWindowManager_ = std::make_unique<WorkspaceWindowManager>(
        defaultWorkspaceWindowCatalog(),
        QtAdsWindowAdapter(dockManager_, [this](ads::CDockWidget* dock) {
            if (layoutController_)
                layoutController_->revealDock(dock);
        }));

    registerWorkspacePanelWindows();
}

void MainWindow::registerWorkspacePanelWindows()
{
    if (!workspaceWindowManager_ || !panelRegistry_)
        return;

    auto registerWorkspacePanelType = [this](const QString& type) {
        const auto* info = panelRegistry_->panelTypeInfo(type);
        if (!info)
            return;

        WorkspaceWindowDescriptor descriptor;
        descriptor.typeId = type;
        descriptor.title = info->label;
        descriptor.dockObjectName = info->dockObjectPrefix;
        descriptor.kind = WindowKind::MultiInstance;
        descriptor.placement = {DefaultDockArea::Floating, QString(), false};
        descriptor.materialization = MaterializationPolicy::OnDemand;
        descriptor.closePolicy = ClosePolicy::Destroy;
        const WorkspaceWindowDescriptor* preset = workspaceWindowManager_->catalog().find(type);
        descriptor.availability = preset ? preset->availability : AvailabilityPolicy::Always;
        descriptor.showInViewMenu = !preset || preset->showInViewMenu;
        workspaceWindowManager_->registerWindowDescriptor(std::move(descriptor));

        workspaceWindowManager_->registerMultiInstanceFactory(type, {
            [this, type, create = info->create]() -> bool {
                if (!create)
                    return false;
                const int before = workspaceWindowManager_
                    ? workspaceWindowManager_->registry().instanceEntries(type).size()
                    : 0;
                create();
                const int after = workspaceWindowManager_
                    ? workspaceWindowManager_->registry().instanceEntries(type).size()
                    : 0;
                return after > before;
            },
            [this](const WorkspaceWindowInstanceSpec& spec) {
                return panelRegistry_->recreate(spec)
                    && panelRegistry_->findPanel(spec.typeId, spec.instanceId);
            },
            [this](const WorkspaceWindowInstanceSpec& spec) {
                return applyWorkspacePanelRestoreSpec(spec);
            },
            [this, type](int id, QWidget* widget, ads::CDockWidget* dock) {
                return snapshotDockInstance(type, id, widget, dock);
            }
        });

        for (const QPair<int, QWidget*>& inst : panelRegistry_->instances(type)) {
            if (ads::CDockWidget* dock = dockOfPanel(inst.second))
                workspaceWindowManager_->registerInstance(type, inst.first, dock, inst.second);
        }
    };

    for (const QString& type : panelRegistry_->registeredPanelTypes())
        registerWorkspacePanelType(type);

    connect(panelRegistry_, &PanelInstanceRegistry::panelRegistered, this,
            [this](const QString& type, int id, QWidget* widget) {
        if (!workspaceWindowManager_ || !widget)
            return;
        if (ads::CDockWidget* dock = dockOfPanel(widget))
            workspaceWindowManager_->registerInstance(type, id, dock, widget);
    });
    connect(panelRegistry_, &PanelInstanceRegistry::panelUnregistered, this,
            [this](const QString& type, int id) {
        if (workspaceWindowManager_)
            workspaceWindowManager_->unregisterInstance(type, id);
    });
}

void MainWindow::registerWorkspaceSingletonFactories()
{
    if (!workspaceWindowManager_)
        return;

    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("ProjectSettings"), {
        []() -> QWidget* { return new ProjectSettingsPanel(); },
        {},
        {},
        [](QWidget* content) {
            return QVariantMap{{QStringLiteral("pageId"),
                qobject_cast<ProjectSettingsPanel*>(content)->currentPageId()}};
        },
        [](QWidget* content, const QVariantMap& state) {
            qobject_cast<ProjectSettingsPanel*>(content)->showPage(
                state.value(QStringLiteral("pageId")).toString());
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("Preferences"), {
        [this]() -> QWidget* {
            return new PreferencesPanel(nullptr, [this](QWidget* parent) {
                return layoutController_->saveCurrentLayout(parent);
            });
        },
        {},
        [this](QWidget* content) {
            auto* panel = qobject_cast<PreferencesPanel*>(content);
            connect(panel, &PreferencesPanel::layoutRenamed,
                    layoutController_, &LayoutController::onNamedLayoutRenamed);
            connect(panel, &PreferencesPanel::layoutDeleted,
                    layoutController_, &LayoutController::onNamedLayoutRemoved);
            connect(layoutController_, &LayoutController::namedLayoutsChanged,
                    panel, &PreferencesPanel::refreshLayouts);
            connect(panel, &PreferencesPanel::aiAgentConfigApplied, this, [this] {
                if (aiService_)
                    aiService_->abortCurrentTurn();
            });
            connect(workspaceWindowManager_->singletonDock(QStringLiteral("Preferences")),
                    &ads::CDockWidget::viewToggled, panel, [panel](bool open) {
                if (!open)
                    panel->discardConnectionChanges();
            });
        },
        [](QWidget* content) {
            return QVariantMap{{QStringLiteral("pageId"),
                qobject_cast<PreferencesPanel*>(content)->currentPageId()}};
        },
        [](QWidget* content, const QVariantMap& state) {
            qobject_cast<PreferencesPanel*>(content)->showPage(
                state.value(QStringLiteral("pageId")).toString());
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("RemoteConnect"), {
        [this]() -> QWidget* { return new RemoteConnectPanel(remoteConnection_); },
        [this](ads::CDockWidget* dock) {
            connect(dock, &ads::CDockWidget::viewToggled, remoteConnection_, [this](bool open) {
                if (open)
                    remoteConnection_->discover();
            });
        }, {}
    });

    WorkspaceWindowManager::SingletonFactory controlFactory;
    controlFactory.snapshotState = [](QWidget* content) {
        return qobject_cast<ControlPanel*>(content)->snapshotState();
    };
    controlFactory.restoreState = [](QWidget* content, const QVariantMap& state) {
        qobject_cast<ControlPanel*>(content)->restoreState(state);
    };
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("Control"), std::move(controlFactory));

    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("Log"), {
        []() -> QWidget* { return new LogPanel(); },
        {},
        [this](QWidget* content) {
            auto* panel = qobject_cast<LogPanel*>(content);
            if (!panel)
                return;
            panel->setAcceptDrops(true);
            connect(panel->model(), &LogModel::entryAdded, this, [this](const LogEntry& entry) {
                setStatusMessage(entry.message, entry.level);
            });
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("Information"), {
        []() -> QWidget* { return new InformationPanel(); },
        {},
        [this](QWidget* content) {
            auto* panel = qobject_cast<InformationPanel*>(content);
            if (!panel)
                return;
            connect(panel, &InformationPanel::locateRequested, this, [this](const QUuid& guid) {
                if (auto* item = ProjectManager::instance().findItemByGuid(guid))
                    selectionRouter_->selectItem(item, true);
            });
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("Publisher"), {
        [this]() -> QWidget* { return new PublisherPanel(this); },
        {},
        [this](QWidget* content) {
            auto* panel = qobject_cast<PublisherPanel*>(content);
            if (!panel)
                return;
            panel->setAcceptDrops(true);
            connect(panel, &PublisherPanel::exportRequested,
                    this, &MainWindow::generateProjectPacks);
            connect(panel, &PublisherPanel::publishSettingsRequested, this, [this] {
                showProjectSettings(QString::fromLatin1(kSettingsPagePublish));
            });
            connect(panel, &PublisherPanel::revealActionRequested,
                    this, [this](uint32_t objectID) {
                if (auto* item =
                        ProjectManager::instance().findItemByObjectID(objectID))
                    selectionRouter_->selectItem(item, true);
            });
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorPerformance"), {
        []() -> QWidget* { return new MonitorPerformancePanel(); },
        [this](ads::CDockWidget* dock) {
            connect(dock, &ads::CDockWidget::viewToggled, this, [this](bool visible) {
                if (visible)
                    refreshMonitorCaptureControlStrip();
                else
                    closeMonitorSession();
            });
        },
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorPerformancePanel*>(content))
            {
                monitorPerformanceChartSource_.attach(panel, &monitorSharedCursor_);
                refreshMonitorCaptureControlStrip();
            }
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("Monitor"), {
        []() -> QWidget* { return new MonitorPanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorPanel*>(content))
            {
                panel->setCaptureController(eventCaptureController_);
                panel->setJumpLocator([this](harkenstudio::ObjectId stableId) {
                    if (selectionRouter_) selectionRouter_->revealProjectObject(stableId);
                });
            }
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorVoices"), {
        []() -> QWidget* { return new MonitorVoicesPanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorVoicesPanel*>(content))
                monitorVoicesSource_.attach(panel, &monitorSharedCursor_);
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorPlayables"), {
        []() -> QWidget* { return new MonitorPlayablesPanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorPlayablesPanel*>(content))
                monitorPlayablesSource_.attach(panel, &monitorSharedCursor_);
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorEffects"), {
        []() -> QWidget* { return new MonitorEffectsPanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorEffectsPanel*>(content))
                monitorEffectsSource_.attach(panel, &monitorSharedCursor_);
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorBuses"), {
        []() -> QWidget* { return new MonitorBusesPanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorBusesPanel*>(content))
                monitorBusesSource_.attach(panel, &monitorSharedCursor_);
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorDspTimeline"), {
        []() -> QWidget* { return new MonitorDspTimelinePanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorDspTimelinePanel*>(content))
                monitorDspTimelineSource_.attach(panel, &monitorSharedCursor_);
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorLineage"), {
        []() -> QWidget* { return new MonitorLineagePanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorLineagePanel*>(content))
                monitorLineageSource_.attach(panel, &monitorSharedCursor_);
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorTapSelect"), {
        []() -> QWidget* { return new MonitorTapSelectPanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorTapSelectPanel*>(content))
                monitorTapSelectSource_.attach(panel, &monitorSharedCursor_);
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorWaveform"), {
        []() -> QWidget* { return new MonitorWaveformPanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorWaveformPanel*>(content))
                monitorWaveformSource_.attach(panel, &monitorSharedCursor_);
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorScope"), {
        []() -> QWidget* { return new MonitorScopePanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorScopePanel*>(content))
                monitorScopeSource_.attach(panel, &monitorSharedCursor_);
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorSpectrum"), {
        []() -> QWidget* { return new MonitorSpectrumPanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorSpectrumPanel*>(content))
                monitorSpectrumSource_.attach(panel, &monitorSharedCursor_);
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorRecording"), {
        []() -> QWidget* { return new MonitorRecordingPanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorRecordingPanel*>(content))
            {
                connect(panel, &MonitorRecordingPanel::recordingCommandRequested,
                        this, &MainWindow::handleMonitorRecordingCommand,
                        Qt::UniqueConnection);
                refreshMonitorRecordingPanel();
            }
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorSessionCompare"), {
        []() -> QWidget* { return new MonitorSessionComparePanel(); },
        {},
        [this](QWidget* content) {
            if (auto* panel = qobject_cast<MonitorSessionComparePanel*>(content))
            {
                Monitor::MonitorCompareUiInput input;
                input.pickerWired = true;
                panel->applyState(Monitor::projectCompareUi(input));
            }
        }
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorStreaming"), {
        []() -> QWidget* {
            return new MonitorPlaceholderPage(QStringLiteral("Streaming"), QStringLiteral("G11"));
        },
        {},
        {}
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorMemory"), {
        []() -> QWidget* {
            return new MonitorPlaceholderPage(QStringLiteral("Memory"), QStringLiteral("P6-5"));
        },
        {},
        {}
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorGameModulate"), {
        []() -> QWidget* {
            return new MonitorPlaceholderPage(QStringLiteral("Game Modulate"), QStringLiteral("G6"));
        },
        {},
        {}
    });
    workspaceWindowManager_->registerSingletonFactory(QStringLiteral("MonitorEmitters"), {
        []() -> QWidget* {
            return new MonitorPlaceholderPage(QStringLiteral("Emitters"), QStringLiteral("G5"));
        },
        {},
        {}
    });
}

void MainWindow::ensureWorkspaceSingletonShells()
{
    if (!workspaceWindowManager_)
        return;

    workspaceWindowManager_->ensureSingletons({
        QStringLiteral("Log"),
        QStringLiteral("Publisher"),
        QStringLiteral("Information"),
        QStringLiteral("Monitor"),
        QStringLiteral("ProjectSettings"),
        QStringLiteral("Preferences"),
        QStringLiteral("RemoteConnect")
    });
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened,
            this, &MainWindow::refreshProjectSettingsAvailability);
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed,
            this, &MainWindow::refreshProjectSettingsAvailability);
    refreshProjectSettingsAvailability();
    for (const auto& type : {QStringLiteral("ProjectSettings"), QStringLiteral("Preferences"),
                             QStringLiteral("RemoteConnect")}) {
        auto* floating = workspaceWindowManager_->singletonDock(type)->floatingDockContainer();
        const QRect available = screen()->availableGeometry();
        const QSize size = type == QLatin1String("RemoteConnect") ? QSize(860, 380) : QSize(800, 550);
        floating->resize(size.boundedTo(available.size()));
        floating->move(available.center() - floating->rect().center());
    }
}

void MainWindow::refreshMonitorCaptureControlStrip()
{
    if (!monitorAdapter_)
    {
        Monitor::MonitorHostContext host;
        host.principal.authenticated = true;
        host.principal.id = "studio-local";
        host.logicThreadPoster = [](std::function<void()> work) {
            if (work)
                work();
        };
        monitorAdapter_ = std::make_unique<Monitor::MonitorAdapter>(std::move(host));
        monitorCaptureControlHost_ = std::make_unique<Monitor::MonitorCaptureControlHost>(
            monitorAdapter_.get());
        Monitor::MonitorHostContext liveHost;
        liveHost.principal.authenticated = true;
        liveHost.principal.id = "studio-local-ts";
        liveHost.logicThreadPoster = [](std::function<void()> work) {
            if (work)
                work();
        };
        monitorPerformanceLiveAdapter_ =
            std::make_unique<Monitor::MonitorAdapter>(std::move(liveHost));
        monitorPerformanceLiveHost_ =
            std::make_unique<Monitor::MonitorPerformanceLiveHost>(
                monitorPerformanceLiveAdapter_.get());
        monitorPerformanceLiveHost_->setUiThreadCheck(
            []() { return QThread::currentThread() == qApp->thread(); });
        monitorPerformanceLiveHost_->setTickHandler(
            [this](const Monitor::MonitorPerformanceLiveTick& tick) {
                QMetaObject::invokeMethod(
                    this,
                    [this, tick]() { applyMonitorPerformanceLiveTick(tick); },
                    Qt::QueuedConnection);
            });
    }

    const auto state = monitorCaptureControlHost_->openLayout();
    if (monitorAdapter_)
        monitorSharedCursor_.setSessionIdentity(
            monitorAdapter_->sessionEpoch(),
            monitorAdapter_->sessionGeneration());
    if (auto* panel = qobject_cast<MonitorPerformancePanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorPerformance"))
                                    : nullptr))
    {
        monitorPerformanceChartSource_.attach(panel, &monitorSharedCursor_);
        panel->applyCaptureControlState(state);
        panel->applyRemoteStatusState(monitorCaptureControlHost_->lastRemoteState());
        if (monitorPerformanceLiveHost_
            && monitorPerformanceLiveHost_->openObservationSession())
        {
            monitorPerformanceLiveHost_->startPolling();
        }
    }
    if (auto* panel = qobject_cast<MonitorVoicesPanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorVoices"))
                                    : nullptr))
        monitorVoicesSource_.attach(panel, &monitorSharedCursor_);
    if (auto* panel = qobject_cast<MonitorPlayablesPanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorPlayables"))
                                    : nullptr))
        monitorPlayablesSource_.attach(panel, &monitorSharedCursor_);
    if (auto* panel = qobject_cast<MonitorEffectsPanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorEffects"))
                                    : nullptr))
        monitorEffectsSource_.attach(panel, &monitorSharedCursor_);
    if (auto* panel = qobject_cast<MonitorBusesPanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorBuses"))
                                    : nullptr))
        monitorBusesSource_.attach(panel, &monitorSharedCursor_);
    if (auto* panel = qobject_cast<MonitorDspTimelinePanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorDspTimeline"))
                                    : nullptr))
        monitorDspTimelineSource_.attach(panel, &monitorSharedCursor_);
    if (auto* panel = qobject_cast<MonitorLineagePanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorLineage"))
                                    : nullptr))
        monitorLineageSource_.attach(panel, &monitorSharedCursor_);
    if (auto* panel = qobject_cast<MonitorTapSelectPanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorTapSelect"))
                                    : nullptr))
        monitorTapSelectSource_.attach(panel, &monitorSharedCursor_);
    if (auto* panel = qobject_cast<MonitorWaveformPanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorWaveform"))
                                    : nullptr))
        monitorWaveformSource_.attach(panel, &monitorSharedCursor_);
    if (auto* panel = qobject_cast<MonitorScopePanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorScope"))
                                    : nullptr))
        monitorScopeSource_.attach(panel, &monitorSharedCursor_);
    if (auto* panel = qobject_cast<MonitorSpectrumPanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorSpectrum"))
                                    : nullptr))
        monitorSpectrumSource_.attach(panel, &monitorSharedCursor_);
    refreshMonitorRecordingPanel();
}

void MainWindow::refreshMonitorRecordingPanel()
{
    if (!workspaceWindowManager_ || !monitorCaptureControlHost_)
        return;
    auto* panel = qobject_cast<MonitorRecordingPanel*>(
        workspaceWindowManager_->singletonContent(QStringLiteral("MonitorRecording")));
    if (panel == nullptr)
        return;

    Monitor::MonitorRecordingBrowserInput input;
    input.mode = Monitor::MonitorRecordingMode::Local;
    input.captureControl = monitorCaptureControlHost_->lastState();
    input.contractPresent = true;
    input.hasRecordPermission =
        (input.captureControl.permissions & VNX::Monitor::kMonitorPermRecord)
        == VNX::Monitor::kMonitorPermRecord;
    if (monitorAdapter_ && monitorAdapter_->hasOpenSession())
    {
        VNX::Monitor::MonitorRecordingStatus status {};
        input.statusQuery =
            monitorAdapter_->getRecordingStatus(monitorAdapter_->sessionHandle(), &status);
        if (input.statusQuery == VNX::Monitor::kOk)
            input.status = status;
    }
    panel->applyState(Monitor::projectRecordingBrowser(input));
}

void MainWindow::closeMonitorSession()
{
    if (monitorPerformanceLiveHost_)
        monitorPerformanceLiveHost_->closeObservationSession();
    if (monitorCaptureControlHost_)
        monitorCaptureControlHost_->closeSession();
}

void MainWindow::applyMonitorPerformanceLiveTick(
    const Monitor::MonitorPerformanceLiveTick& tick)
{
    if (auto* panel = qobject_cast<MonitorPerformancePanel*>(
            workspaceWindowManager_ ? workspaceWindowManager_->singletonContent(
                QStringLiteral("MonitorPerformance"))
                                    : nullptr))
    {
        monitorPerformanceChartSource_.applyLiveTick(tick, panel->paused());
    }
}

void MainWindow::handleMonitorRecordingCommand(Monitor::MonitorRecordingCommand command)
{
    lastMonitorRecordingCommand_ = command;
    if (command == Monitor::MonitorRecordingCommand::None)
        return;
    if (!monitorAdapter_)
        refreshMonitorCaptureControlStrip();
    if (!monitorAdapter_ || !monitorCaptureControlHost_)
        return;

    const auto capture = monitorCaptureControlHost_->openLayout();
    if (!monitorAdapter_->hasOpenSession())
        return;

    const VNX::Monitor::MonitorSessionHandle handle = monitorAdapter_->sessionHandle();
    const uint64_t epoch = monitorAdapter_->sessionEpoch();
    uint64_t leaseGeneration = monitorAdapter_->acquiredLeaseGeneration();
    if (leaseGeneration == 0)
        leaseGeneration = capture.leaseGeneration;

    switch (command)
    {
    case Monitor::MonitorRecordingCommand::StartRecording:
    {
        if (!Monitor::canApply(capture))
        {
            VNX::Monitor::MonitorCaptureControlStatus lease {};
            if (monitorAdapter_->acquireCaptureControl(handle, &lease, epoch) != VNX::Monitor::kOk)
                break;
            leaseGeneration = lease.leaseGeneration;
            monitorCaptureControlHost_->openLayout();
        }
        const QString directory = QDir::temp().filePath(QStringLiteral("harken-monitor-p63"));
        QDir().mkpath(directory);
        const QByteArray pathUtf8 =
            QDir(directory)
                .filePath(QStringLiteral("session-%1.vnmon")
                              .arg(QDateTime::currentMSecsSinceEpoch()))
                .toUtf8();
        VNX::Monitor::MonitorRecordingDesc desc {};
        desc.utf8Path = pathUtf8.constData();
        desc.fileByteBudget = 8ull * 1024ull * 1024ull;
        monitorAdapter_->startRecording(handle, leaseGeneration, &desc, epoch);
        break;
    }
    case Monitor::MonitorRecordingCommand::StopRecording:
        monitorAdapter_->stopRecording(handle, leaseGeneration, epoch);
        break;
    case Monitor::MonitorRecordingCommand::StopCapture:
    {
        VNX::Monitor::MonitorEffectiveCapturePlan result {};
        monitorAdapter_->stopCapture(handle, leaseGeneration, &result, epoch);
        break;
    }
    case Monitor::MonitorRecordingCommand::CloseSession:
        monitorAdapter_->close(handle);
        monitorCaptureControlHost_->leaveLayout();
        break;
    case Monitor::MonitorRecordingCommand::None:
        break;
    }

    if (command != Monitor::MonitorRecordingCommand::CloseSession)
        monitorCaptureControlHost_->openLayout();
    refreshMonitorRecordingPanel();
}

void MainWindow::applyMonitorPerformanceChartModel(const TimeSeriesChartModel& model)
{
    monitorPerformanceChartSource_.apply(model);
}

void MainWindow::applyMonitorVoicesSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot)
{
    monitorVoicesSource_.apply(std::move(snapshot));
}

std::shared_ptr<const Monitor::MonitorStoreSnapshot> MainWindow::monitorVoicesSnapshot() const
{
    return monitorVoicesSource_.snapshot();
}

void MainWindow::applyMonitorPlayablesSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot)
{
    monitorPlayablesSource_.apply(std::move(snapshot));
}

std::shared_ptr<const Monitor::MonitorStoreSnapshot> MainWindow::monitorPlayablesSnapshot() const
{
    return monitorPlayablesSource_.snapshot();
}

void MainWindow::applyMonitorEffectsSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot)
{
    monitorEffectsSource_.apply(std::move(snapshot));
}

std::shared_ptr<const Monitor::MonitorStoreSnapshot> MainWindow::monitorEffectsSnapshot() const
{
    return monitorEffectsSource_.snapshot();
}

void MainWindow::applyMonitorBusesSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot)
{
    monitorBusesSource_.apply(std::move(snapshot));
}

std::shared_ptr<const Monitor::MonitorStoreSnapshot> MainWindow::monitorBusesSnapshot() const
{
    return monitorBusesSource_.snapshot();
}

void MainWindow::applyMonitorDspTimelineSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot)
{
    monitorDspTimelineSource_.apply(std::move(snapshot));
}

std::shared_ptr<const Monitor::MonitorStoreSnapshot> MainWindow::monitorDspTimelineSnapshot() const
{
    return monitorDspTimelineSource_.snapshot();
}

void MainWindow::applyMonitorLineageSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot)
{
    monitorLineageSource_.apply(std::move(snapshot));
}

std::shared_ptr<const Monitor::MonitorStoreSnapshot> MainWindow::monitorLineageSnapshot() const
{
    return monitorLineageSource_.snapshot();
}

void MainWindow::applyMonitorTapSelectSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot)
{
    monitorTapSelectSource_.apply(std::move(snapshot));
}

std::shared_ptr<const Monitor::MonitorStoreSnapshot> MainWindow::monitorTapSelectSnapshot() const
{
    return monitorTapSelectSource_.snapshot();
}

void MainWindow::applyMonitorWaveformSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot)
{
    monitorWaveformSource_.apply(std::move(snapshot));
}

std::shared_ptr<const Monitor::MonitorStoreSnapshot> MainWindow::monitorWaveformSnapshot() const
{
    return monitorWaveformSource_.snapshot();
}

void MainWindow::applyMonitorScopeSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot)
{
    monitorScopeSource_.apply(std::move(snapshot));
}

std::shared_ptr<const Monitor::MonitorStoreSnapshot> MainWindow::monitorScopeSnapshot() const
{
    return monitorScopeSource_.snapshot();
}

void MainWindow::applyMonitorSpectrumSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot)
{
    monitorSpectrumSource_.apply(std::move(snapshot));
}

std::shared_ptr<const Monitor::MonitorStoreSnapshot> MainWindow::monitorSpectrumSnapshot() const
{
    return monitorSpectrumSource_.snapshot();
}

MonitorCursorModel& MainWindow::monitorSharedCursor()
{
    return monitorSharedCursor_;
}

const TimeSeriesChartModel& MainWindow::monitorPerformanceChartModel() const
{
    return monitorPerformanceChartSource_.model();
}

void MainWindow::createPrimaryWorkspaceDocks()
{
    assistantDock_ = new ads::CDockWidget(tr("Assistant"));
    assistantDock_->setObjectName("AssistantDock");
    assistantDock_->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromContent);
    assistantPanel_ = new AssistantPanel();
    assistantDock_->setWidget(assistantPanel_);
    dockManager_->addDockWidget(ads::RightDockWidgetArea, assistantDock_);

    projectExplorerDock_ = new ads::CDockWidget(tr("Explorer"));
    projectExplorerDock_->setObjectName("ExplorerDock");
    projectExplorerDock_->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromContent);
    projectExplorer_ = new ProjectExplorer();
    projectExplorerDock_->setWidget(projectExplorer_);
    dockManager_->addDockWidget(ads::LeftDockWidgetArea, projectExplorerDock_);


    controlDock_ = new ads::CDockWidget(tr("Control"));
    controlDock_->setObjectName("ControlDock");
    controlDock_->setFeature(ads::CDockWidget::DockWidgetClosable, true);
    controlDock_->setFeature(ads::CDockWidget::DockWidgetMovable, true);
    controlDock_->setFeature(ads::CDockWidget::DockWidgetFloatable, true);
    controlDock_->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromContent);
    if (controlPanel_) {
        WorkspaceDockFactory::setContent(controlDock_, controlPanel_, ads::CDockWidget::ForceNoScrollArea);
        connect(controlPanel_, &ControlPanel::contextChanged, this, [this] {
            refreshBoundEditorDockTitles(tr("Control"), {controlPanel_},
                [](QWidget* panel) { return static_cast<ControlPanel*>(panel)->boundItem(); });
        });
    }
    dockManager_->addDockWidget(ads::BottomDockWidgetArea, controlDock_);

    if (workspaceWindowManager_) {
        workspaceWindowManager_->registerSingletons({
            {QStringLiteral("Blueprint"), blueprintDock_},
            {QStringLiteral("Assistant"), assistantDock_},
            {QStringLiteral("Explorer"), projectExplorerDock_},
            {QStringLiteral("Control"), controlDock_}
        });
    }
}

void MainWindow::connectWorkspaceWindowSignals()
{
    if (blueprintCanvas_) {
        connect(blueprintCanvas_, &BlueprintCanvasWidget::nodeSelected,
                selectionRouter_, &SelectionRouter::onBlueprintNodeSelected);
        connect(blueprintCanvas_, &BlueprintCanvasWidget::selectionCleared,
                selectionRouter_, &SelectionRouter::onBlueprintSelectionCleared);
        connect(blueprintCanvas_, &BlueprintCanvasWidget::nodeDoubleClicked,
                selectionRouter_, &SelectionRouter::onBlueprintNodeDoubleClicked);
    }

    auditionManager_ = new HarkenEditor::AuditionManager(&AudioEngine::instance(), this);
    connect(auditionManager_, &HarkenEditor::AuditionManager::auditionStateChanged,
            playbackControls_, [this](uint32_t, int state) {
        if (state == 1)
            playbackControls_->setPlaying(true);
    });
    connect(auditionManager_, &HarkenEditor::AuditionManager::allAuditionsStopped,
            playbackControls_, [this] { playbackControls_->setPlaying(false); });
    for (QWidget* widget : panelRegistry_->allPanels(SourceEditorPanel::typeId())) {
        if (auto* editor = qobject_cast<SourceEditorPanel*>(widget))
            editor->setAuditionManager(auditionManager_);
    }
    connect(auditionManager_, &HarkenEditor::AuditionManager::auditionStarted,
            this, [this](uint32_t, const QString& objectName) {
        setStatusMessage(tr("Playing: %1").arg(objectName));
    });
    connect(auditionManager_, &HarkenEditor::AuditionManager::auditionStartFailed,
            this, [this](uint32_t, const QString& objectName,
                         int32_t engineResult) {
        const QString message = engineResult == 0
            ? tr("Cannot play '%1': it is not in the current preview world.")
                  .arg(objectName)
            : tr("Playback failed for '%1' after waiting for Runtime "
                 "synchronization (engine result %2).")
                  .arg(objectName)
                  .arg(engineResult);
        setStatusMessage(message);
        if (auto* log = logPanel()) {
            if (engineResult == 0)
                log->logInfo(LogCategory::Audio, message);
            else
                log->logError(LogCategory::Audio, message);
        }
    });
    connect(auditionManager_, &HarkenEditor::AuditionManager::auditionSeekFinished,
            this, [this](uint32_t playingId, quint64 requestId,
                         int32_t engineResult, bool applied) {
        if (!ipcServer_)
            return;
        ipcServer_->broadcastEvent(
            QStringLiteral("audition_seek_finished"),
            QJsonObject{
                {QStringLiteral("auditionID"), static_cast<int>(playingId)},
                {QStringLiteral("audition_seek_request_id"),
                 QString::number(requestId)},
                {QStringLiteral("engine_result"), engineResult},
                {QStringLiteral("state"),
                 applied ? QStringLiteral("applied")
                         : QStringLiteral("cancelled")},
            });
    });
    ShortcutManager::instance().bindWidgetAction(
        Shortcuts::PlayStop, this, [this]() { onPlayPressed(); },
        Qt::ApplicationShortcut);
    ShortcutManager::instance().bindWidgetAction(
        Shortcuts::PlaybackReset, this, [this]() { onResetPressed(); },
        Qt::ApplicationShortcut);
}

void MainWindow::configureWorkspaceDockPolicies()
{
    for (const auto& type : {QStringLiteral("ProjectSettings"), QStringLiteral("Preferences"),
                             QStringLiteral("RemoteConnect")})
        layoutController_->setPlacementHint(
            workspaceWindowManager_->singletonDock(type)->objectName(), PreferredOpenArea::RestoreSaved);

    const bool assistantAvailable = workspaceWindowManager_->isSingletonAvailable(QStringLiteral("Assistant"));
    assistantDock_->toggleViewAction()->setEnabled(assistantAvailable);
    assistantDock_->toggleViewAction()->setVisible(assistantAvailable);
    if (!assistantAvailable)
        assistantDock_->toggleView(false);

    if (assistantPanel_)
        assistantPanel_->setAcceptDrops(true);
    for (auto* p : allInspectorPanels())
        if (p) p->setAcceptDrops(true);

    if (blueprintDock_)
        layoutController_->setPlacementHint(blueprintDock_->objectName(),
                                            PreferredOpenArea::CentralTab);

    ads::CDockWidget* informationDock = workspaceWindowManager_
        ? workspaceWindowManager_->singletonDock(QStringLiteral("Information"))
        : nullptr;
    for (ads::CDockWidget* dock : {assistantDock_, projectExplorerDock_, controlDock_,
                                   informationDock})
        if (dock)
            layoutController_->setPlacementHint(dock->objectName(),
                                                PreferredOpenArea::RestoreSaved);
}




bool MainWindow::isDockLocked(ads::CDockWidget* dock) const
{
    return layoutController_->isDockLocked(dock);
}

void MainWindow::revealDock(ads::CDockWidget* dock)
{
    layoutController_->revealDock(dock);
}

bool MainWindow::revealPanel(const QString& objectName, bool visible)
{
    if (workspaceWindowManager_) {
        const QString typeId = workspaceWindowManager_->singletonTypeForDockObjectName(objectName);
        if (!typeId.isEmpty())
            return workspaceWindowManager_->setSingletonVisible(typeId, visible);
    }
    return layoutController_->revealPanel(objectName, visible);
}

QStringList MainWindow::workspacePanelTypes() const
{
    QStringList types;
    if (!workspaceWindowManager_)
        return types;
    for (const WorkspaceWindowDescriptor& descriptor : workspaceWindowManager_->catalog().descriptors()) {
        if (descriptor.kind == WindowKind::MultiInstance
            && descriptor.availability != AvailabilityPolicy::FeatureDisabled)
            types.append(descriptor.typeId);
    }
    return types;
}

QString MainWindow::workspacePanelTitle(const QString& typeId) const
{
    if (!workspaceWindowManager_)
        return QString();
    const WorkspaceWindowDescriptor* descriptor = workspaceWindowManager_->catalog().find(typeId);
    return descriptor ? descriptor->title : QString();
}

bool MainWindow::createWorkspacePanel(const QString& typeId)
{
    return workspaceWindowManager_ && workspaceWindowManager_->createInstance(typeId);
}

QList<int> MainWindow::workspacePanelInstanceIds(const QString& typeId) const
{
    QList<int> ids;
    if (!workspaceWindowManager_)
        return ids;
    const QList<WorkspaceWindowInstanceEntry> entries =
        workspaceWindowManager_->registry().instanceEntries(typeId);
    ids.reserve(entries.size());
    for (const WorkspaceWindowInstanceEntry& entry : entries)
        ids.append(entry.instanceId);
    return ids;
}

QWidget* MainWindow::workspacePanelContent(const QString& typeId, int panelId) const
{
    return workspaceWindowManager_
        ? workspaceWindowManager_->instanceContent(typeId, panelId)
        : nullptr;
}

QList<QWidget*> MainWindow::workspacePanelContents(const QString& typeId) const
{
    QList<QWidget*> widgets;
    if (!workspaceWindowManager_)
        return widgets;
    const QList<WorkspaceWindowInstanceEntry> entries =
        workspaceWindowManager_->registry().instanceEntries(typeId);
    widgets.reserve(entries.size());
    for (const WorkspaceWindowInstanceEntry& entry : entries) {
        if (entry.content)
            widgets.append(entry.content.data());
    }
    return widgets;
}

int MainWindow::lastFocusedWorkspacePanelId(const QString& typeId) const
{
    return panelRegistry_ ? panelRegistry_->lastFocusedPanelId(typeId) : PanelInstanceRegistry::kNoFocus;
}

QList<VNX::DockInstanceSpec> MainWindow::workspaceWindowInstanceSpecs() const
{
    return workspaceWindowManager_
        ? workspaceWindowManager_->instanceSpecs()
        : QList<VNX::DockInstanceSpec>();
}

QList<QVariantMap> MainWindow::workspaceWindowSummaries() const
{
    QList<QVariantMap> summaries;
    if (!workspaceWindowManager_)
        return summaries;

    auto appendDock = [&summaries](const QString& kind, const QString& typeId,
                                   int instanceId, ads::CDockWidget* dock,
                                   bool available) {
        if (!dock)
            return;
        QVariantMap summary;
        summary.insert(QStringLiteral("kind"), kind);
        summary.insert(QStringLiteral("typeId"), typeId);
        summary.insert(QStringLiteral("instanceId"), instanceId);
        summary.insert(QStringLiteral("name"), dock->objectName());
        summary.insert(QStringLiteral("title"), dock->windowTitle());
        summary.insert(QStringLiteral("visible"), !dock->isClosed());
        summary.insert(QStringLiteral("floating"), dock->isFloating());
        summary.insert(QStringLiteral("available"), available);
        summaries.append(summary);
    };

    const QList<QString> singletonIds = workspaceWindowManager_->registeredSingletonTypeIds();
    for (const QString& typeId : singletonIds) {
        appendDock(QStringLiteral("singleton"), typeId, 0,
                   workspaceWindowManager_->singletonDock(typeId),
                   workspaceWindowManager_->isSingletonAvailable(typeId));
    }

    const QList<WorkspaceWindowInstanceEntry> entries =
        workspaceWindowManager_->registry().instanceEntries();
    for (const WorkspaceWindowInstanceEntry& entry : entries) {
        appendDock(QStringLiteral("multi_instance"), entry.typeId, entry.instanceId,
                   entry.dock.data(), true);
    }

    return summaries;
}

bool MainWindow::setWorkspaceWindowVisible(const QString& selector, bool visible,
                                           QVariantMap* summary)
{
    if (!workspaceWindowManager_)
        return false;

    const QString needle = selector.trimmed();
    if (needle.isEmpty())
        return false;

    auto matches = [&needle](const QString& typeId, ads::CDockWidget* dock, int instanceId) {
        return workspaceWindowSelectorMatches(
            needle, typeId,
            dock ? dock->objectName() : QString(),
            dock ? dock->windowTitle() : QString(),
            instanceId);
    };

    auto fillSummary = [summary](const QString& kind, const QString& typeId,
                                 int instanceId, ads::CDockWidget* dock,
                                 bool available) {
        if (!summary || !dock)
            return;
        summary->clear();
        summary->insert(QStringLiteral("kind"), kind);
        summary->insert(QStringLiteral("typeId"), typeId);
        summary->insert(QStringLiteral("instanceId"), instanceId);
        summary->insert(QStringLiteral("name"), dock->objectName());
        summary->insert(QStringLiteral("title"), dock->windowTitle());
        summary->insert(QStringLiteral("visible"), !dock->isClosed());
        summary->insert(QStringLiteral("floating"), dock->isFloating());
        summary->insert(QStringLiteral("available"), available);
    };

    for (const QString& typeId : workspaceWindowManager_->registeredSingletonTypeIds()) {
        ads::CDockWidget* dock = workspaceWindowManager_->singletonDock(typeId);
        if (!matches(typeId, dock, 0))
            continue;
        const bool ok = workspaceWindowManager_->setSingletonVisible(typeId, visible);
        fillSummary(QStringLiteral("singleton"), typeId, 0, dock,
                    workspaceWindowManager_->isSingletonAvailable(typeId));
        return ok;
    }

    const QList<WorkspaceWindowInstanceEntry> entries =
        workspaceWindowManager_->registry().instanceEntries();
    for (const WorkspaceWindowInstanceEntry& entry : entries) {
        ads::CDockWidget* dock = entry.dock.data();
        if (!matches(entry.typeId, dock, entry.instanceId))
            continue;
        const bool ok = workspaceWindowManager_->setInstanceVisible(
            entry.typeId, entry.instanceId, visible);
        fillSummary(QStringLiteral("multi_instance"), entry.typeId, entry.instanceId,
                    dock, true);
        return ok;
    }

    return false;
}

QWidget* MainWindow::workspaceSingletonContent(const QString& typeId, bool materialize) const
{
    if (!workspaceWindowManager_)
        return nullptr;
    return materialize
        ? workspaceWindowManager_->materializeSingletonContent(typeId)
        : workspaceWindowManager_->singletonContent(typeId);
}

void MainWindow::refreshVisibleMaterializedSingletons()
{
    if (!workspaceWindowManager_)
        return;

    workspaceWindowManager_->materializeVisibleSingletons();
}

LogPanel* MainWindow::logPanel() const
{
    return qobject_cast<LogPanel*>(workspaceSingletonContent(QStringLiteral("Log"), true));
}

PublisherPanel* MainWindow::publisherPanel() const
{
    return qobject_cast<PublisherPanel*>(
        workspaceSingletonContent(QStringLiteral("Publisher"), true));
}


DockInstanceSpec MainWindow::snapshotDockInstance(const QString& type, int id, QWidget* widget,
                                                  ads::CDockWidget* dock) const
{
    DockInstanceSpec spec;
    spec.typeId = type;
    spec.instanceId = id;

    if (dock)
        spec.deleteOnClose =
            dock->features().testFlag(ads::CDockWidget::DockWidgetDeleteOnClose);

    if (type == QLatin1String("Inspector")) {
        if (auto* inspector = qobject_cast<InspectorPanel*>(widget)) {
            if (inspector->isPinned()) {
                spec.pinnedObjectGuid = inspector->pinnedObjectGuid();
                if (spec.pinnedObjectGuid.isEmpty())
                    LogPanel::logInfo(LogCategory::Layout,
                        tr("Pinned panel Inspector_%1 shows a non-ProjectItem page; "
                           "its content cannot be persisted").arg(id));
            }
        }
    } else if (type == QLatin1String("Hierarchy")) {
        if (auto* hierarchy = qobject_cast<Hierarchy*>(widget))
            spec.params["filter"] = static_cast<int>(hierarchy->currentFilter());
    } else if (type == QLatin1String("Meter")) {
        if (auto* meter = qobject_cast<MeterPanel*>(widget))
            spec.params = meter->saveState();
    } else if (standaloneEditorTypes_.contains(type)) {
        const StandaloneEditorTypeInfo info = standaloneEditorTypes_.value(type);
        if (info.snapshotParams) {
            spec.params = info.snapshotParams(widget);
        } else if (info.boundItem) {
            if (ProjectItem* item = info.boundItem(widget))
                spec.params["bindingGuid"] = item->guid.toString(QUuid::WithoutBraces);
        }
        if (info.isPinned)
            spec.params["pinned"] = info.isPinned(widget);
    }

    return spec;
}

bool MainWindow::applyWorkspacePanelRestoreSpec(const DockInstanceSpec& spec)
{
    if (!panelRegistry_ || spec.typeId.isEmpty() || spec.instanceId <= 0)
        return false;

    QWidget* widget = panelRegistry_->findPanel(spec.typeId, spec.instanceId);
    if (!widget)
        return false;

    if (spec.typeId == QLatin1String("Inspector")) {
        auto* panel = qobject_cast<InspectorPanel*>(widget);
        if (!panel)
            return false;
        panel->setPinned(false);
        if (!spec.pinnedObjectGuid.isEmpty()) {
            if (ProjectItem* item = ProjectManager::instance().findItemByGuid(
                    QUuid::fromString(spec.pinnedObjectGuid))) {
                showInspectorForItem(panel, item);
                panel->setPinned(true);
            }
        }
        return true;
    }

    if (spec.typeId == QLatin1String("Hierarchy")) {
        auto* hierarchy = qobject_cast<Hierarchy*>(widget);
        if (!hierarchy)
            return false;
        if (spec.params.contains("filter")) {
            hierarchy->setFilter(static_cast<ItemTypeCatalog::Bucket>(
                spec.params.value("filter").toInt()));
        }
        return true;
    }

    if (spec.typeId == QLatin1String("Meter")) {
        auto* meter = qobject_cast<MeterPanel*>(widget);
        if (!meter)
            return false;
        meter->restoreState(spec.params);
        return true;
    }

    if (standaloneEditorTypes_.contains(spec.typeId)) {
        const StandaloneEditorTypeInfo info = standaloneEditorTypes_.value(spec.typeId);
        if (info.restoreParams) {
            info.restoreParams(widget, spec.params);
        } else {
            const QString guid = spec.params.value("bindingGuid").toString();
            if (!guid.isEmpty() && info.bind) {
                if (ProjectItem* item = ProjectManager::instance().findItemByGuid(
                        QUuid::fromString(guid))) {
                    info.bind(widget, item);
                }
            }
        }
        if (info.setPinned)
            info.setPinned(widget, spec.params.value("pinned").toBool());
        if (spec.typeId == QLatin1String("TransitionEditor"))
            refreshTransitionEditorDockTitles();
        else if (spec.typeId == QLatin1String("GridEditor"))
            refreshGridEditorDockTitles();
        else if (spec.typeId == QLatin1String("TimelineEditor"))
            refreshTimelineEditorDockTitles();
        return true;
    }

    return true;
}


MeterPanel* MainWindow::createMeterPanelWithId(int id, bool deferShow)
{
    const auto* info = panelRegistry_->panelTypeInfo(QStringLiteral("Meter"));
    auto* panel = new MeterPanel(this);
    const QString dockName = QStringLiteral("%1_%2").arg(info->dockObjectPrefix).arg(id);
    auto* dock = WorkspaceDockFactory::createDock(dockManager_, {
        info->label, dockName, panel, true, !deferShow, true
    });
    layoutController_->setPlacementHint(dockName, PreferredOpenArea::RestoreSaved);
    panelRegistry_->registerPanel(QStringLiteral("Meter"), id, panel);
    connect(dock, &QObject::destroyed, panelRegistry_, [this, id]() {
        panelRegistry_->unregisterPanel(QStringLiteral("Meter"), id);
    });
    return panel;
}

void MainWindow::registerStandaloneEditorType(const QString& type, StandaloneEditorTypeInfo info)
{
    PanelInstanceRegistry::PanelTypeInfo pinfo;
    pinfo.label = info.label;
    pinfo.dockObjectPrefix = info.dockObjectPrefix;
    pinfo.create = [this, type]() { createStandaloneEditorPanel(type); };
    pinfo.recreate = [this, type](const VNX::DockInstanceSpec& s) {
        if (panelRegistry_->findPanel(type, s.instanceId)) {
            applyWorkspacePanelRestoreSpec(s);
            return;
        }
        QWidget* w = createStandaloneEditorPanelWithId(type, s.instanceId, true);
        if (!w)
            return;
        applyWorkspacePanelRestoreSpec(s);
    };
    panelRegistry_->registerPanelType(type, std::move(pinfo));

    standaloneEditorTypes_.insert(type, std::move(info));
}

QWidget* MainWindow::createStandaloneEditorPanel(const QString& type)
{
    QWidget* editor = createStandaloneEditorPanelWithId(
        type, panelRegistry_->allocatePanelId(type), false);
    if (editor && (type == QLatin1String("TimelineEditor")
                   || type == QLatin1String("GridEditor"))) {
        const StandaloneEditorTypeInfo ti = standaloneEditorTypes_.value(type);
        ProjectItem* item = currentStandaloneEditorSelection();
        ProjectItem* track = type == QLatin1String("TimelineEditor") && item
            && item->type == ProjectItem::Type::Track ? item : nullptr;
        if (type == QLatin1String("TimelineEditor")) item = TimelineEditorPanel::timelineOwner(item);
        if (ti.accepts && ti.accepts(item) && ti.bind)
            ti.bind(editor, item);
        if (track)
            if (auto* timeline = qobject_cast<TimelineEditorPanel*>(editor)) timeline->selectLane(track->guid);
    }
    return editor;
}

QWidget* MainWindow::createStandaloneEditorPanelWithId(const QString& type, int id, bool deferShow)
{
    if (workspaceWindowManager_) {
        const auto* descriptor = workspaceWindowManager_->catalog().find(type);
        if (descriptor && descriptor->availability == AvailabilityPolicy::FeatureDisabled)
            return nullptr;
    }
    const auto* pinfo = panelRegistry_->panelTypeInfo(type);
    if (!pinfo)
        return nullptr;
    StandaloneEditorTypeInfo ti = standaloneEditorTypes_.value(type);
    if (!ti.createConfigured)
        return nullptr;

    QWidget* editor = ti.createConfigured(id);
    if (!editor)
        return nullptr;

    const QString dockObjectName =
        QStringLiteral("%1_%2").arg(pinfo->dockObjectPrefix).arg(id);
    ads::DockWidgetArea area = ads::NoDockWidgetArea;
    ads::CDockAreaWidget* anchorArea = nullptr;
    ads::CDockAreaWidget* inspectorArea = nullptr;
    bool seedBelowHeight = false;
    if (type == QLatin1String("ExternalEditor")) {
        layoutController_->setPlacementHint(dockObjectName, PreferredOpenArea::RestoreSaved);
    } else if (ti.openBelowCentralArea) {
        for (QWidget* panel : panelRegistry_->allPanels(type)) {
            auto* existing = dockOfPanel(panel);
            if (existing && !existing->isClosed() && !existing->isInFloatingContainer()) {
                area = ads::CenterDockWidgetArea;
                anchorArea = existing->dockAreaWidget();
                break;
            }
        }
        layoutController_->setPlacementHint(dockObjectName, PreferredOpenArea::RestoreSaved);
    } else if (type == SourceEditorPanel::typeId()) {
        layoutController_->setPlacementHint(dockObjectName, PreferredOpenArea::Float);
    } else {
        inspectorArea = anyInspectorDockArea();
        if (ads::CDockAreaWidget* below = inspectorBelowStandaloneEditorArea()) {
            area = ads::CenterDockWidgetArea;
            anchorArea = below;
        } else if (inspectorArea) {
            area = ads::BottomDockWidgetArea;
            anchorArea = inspectorArea;
            seedBelowHeight = !deferShow;
        } else if (blueprintDock_ && !blueprintDock_->isClosed()) {
            area = ads::BottomDockWidgetArea;
            anchorArea = blueprintDock_->dockAreaWidget();
        }
        layoutController_->setPlacementHint(dockObjectName, PreferredOpenArea::RestoreSaved);
    }

    const WorkspaceDockCreateSpec spec{
        pinfo->label,
        dockObjectName,
        editor,
        true,
        !deferShow,
        false,
        area,
        anchorArea
    };
    auto* dock = ti.openBelowCentralArea && !anchorArea
        ? layoutController_->createEditorDock(spec, ads::BottomDockWidgetArea)
        : WorkspaceDockFactory::createDock(dockManager_, spec);

    if (seedBelowHeight && dock && inspectorArea) {
        QPointer<ads::CDockAreaWidget> inspectorGuard(inspectorArea);
        QPointer<ads::CDockWidget> dockGuard(dock);
        QTimer::singleShot(0, this, [this, inspectorGuard, dockGuard]() {
            if (inspectorGuard && dockGuard)
                seedInspectorBelowEditorHeight(inspectorGuard, dockGuard);
        });
    }

    panelRegistry_->registerPanel(type, id, editor);

    connect(dock, &QObject::destroyed, panelRegistry_, [this, type, id]() {
        if (id > 0 && panelRegistry_)
            panelRegistry_->unregisterPanel(type, id);
    });

    return editor;
}

QWidget* MainWindow::activeStandaloneEditorPanel(const QString& type,
                                                 bool includeClosed) const
{
    const StandaloneEditorTypeInfo ti = standaloneEditorTypes_.value(type);
    const int lastId = panelRegistry_->lastFocusedPanelId(type);
    QWidget* firstEligible = nullptr;
    for (const QPair<int, QWidget*>& inst : panelRegistry_->instances(type)) {
        QWidget* ed = inst.second;
        if (!ed) continue;
        if (ti.isPinned && ti.isPinned(ed)) continue;
        ads::CDockWidget* d = dockOfPanel(ed);
        if (!d || (!includeClosed && d->isClosed())) continue;
        if (!firstEligible) firstEligible = ed;
        if (inst.first == lastId) return ed;
    }
    return firstEligible;
}

QWidget* MainWindow::openStandaloneEditor(const QString& type, ProjectItem* item)
{
    ProjectItem* track = nullptr;
    if (type == QLatin1String("TimelineEditor") && item) {
        if (item->type == ProjectItem::Type::Track) track = item;
        item = TimelineEditorPanel::timelineOwner(item);
        if (!item) return nullptr;
    }
    if (standaloneEditorTypes_.value(type).comingSoon) {
        statusBar()->showMessage(
            tr("The %1 is coming soon.").arg(standaloneEditorTypes_.value(type).label), 4000);
        return nullptr;
    }
    QWidget* editor = nullptr;
    if (type == QLatin1String("TimelineEditor") && item)
        for (auto* timeline : allTimelineEditorPanels())
            if (timeline->boundItem() == item) {
                editor = timeline;
                break;
            }
    if (!editor) editor = activeStandaloneEditorPanel(type);
    if (!editor)
        editor = createStandaloneEditorPanelWithId(
            type, panelRegistry_->allocatePanelId(type), false);
    if (!editor)
        return nullptr;
    const StandaloneEditorTypeInfo ti = standaloneEditorTypes_.value(type);
    if (item && ti.bind)
        ti.bind(editor, item);
    if (track)
        if (auto* timeline = qobject_cast<TimelineEditorPanel*>(editor)) timeline->selectLane(track->guid);
    setStandaloneEditorVisible(editor, true);
    editor->setFocus(Qt::OtherFocusReason);
    return editor;
}

void MainWindow::pushSelectionToStandaloneEditors(ProjectItem* item,
                                                  StandaloneEditorPushReason reason)
{
    for (const QString& type : standaloneEditorTypes_.keys()) {
        const StandaloneEditorTypeInfo ti = standaloneEditorTypes_.value(type);
        if (type == QLatin1String("TimelineEditor") && item && item->type == ProjectItem::Type::Track) {
            auto* owner = TimelineEditorPanel::timelineOwner(item);
            ads::CDockWidget* matchingDock = nullptr;
            for (auto* timeline : allTimelineEditorPanels()) {
                auto* dock = dockOfPanel(timeline);
                if (!owner || timeline->boundItem() != owner || !dock || dock->isClosed()) continue;
                timeline->selectLane(item->guid);
                if (!matchingDock || timeline->panelId() == panelRegistry_->lastFocusedPanelId(type))
                    matchingDock = dock;
            }
            if (matchingDock) {
                matchingDock->setAsCurrentTab();
                continue;
            }
        }
        const bool accepted = item && ti.accepts && ti.accepts(item);
        if (accepted) {
            QWidget* ed = activeStandaloneEditorPanel(type, false);
            if (ed && ti.bind)
                ti.bind(ed, item);
            continue;
        }
        if (ti.clearOnUnrelatedSelection) {
            for (QWidget* w : panelRegistry_->allPanels(type))
                collapseUnrelatedStandaloneEditor(type, w, reason);
        }
    }
    refreshTransitionEditorDockTitles();
    refreshGridEditorDockTitles();
    refreshTimelineEditorDockTitles();
}

InspectorPanel* MainWindow::createInspectorPanel(PreferredOpenArea placement)
{
    return createInspectorPanelWithId(
        panelRegistry_->allocatePanelId(QStringLiteral("Inspector")), placement, false);
}

InspectorPanel* MainWindow::createInspectorPanelWithId(int id, PreferredOpenArea placement, bool deferShow)
{
    auto* panel = new InspectorPanel();
    panel->setPanelId(id);
    panel->showEmpty();
    panel->setAcceptDrops(true);

    ads::CDockAreaWidget* inspectorArea = anyInspectorDockArea(false);

    ads::DockWidgetArea area = ads::NoDockWidgetArea;
    ads::CDockAreaWidget* anchorArea = nullptr;
    if (placement == PreferredOpenArea::CentralTab) {
        if (inspectorArea) {
            area = ads::CenterDockWidgetArea;
            anchorArea = inspectorArea;
        }
    }

    const WorkspaceDockCreateSpec spec{
        tr("Inspector"),
        QString("InspectorDock_%1").arg(id),
        panel,
        true,
        !deferShow,
        true,
        area,
        anchorArea
    };
    auto* dock = placement == PreferredOpenArea::CentralTab && !anchorArea
        ? layoutController_->createEditorDock(spec, ads::RightDockWidgetArea)
        : WorkspaceDockFactory::createDock(dockManager_, spec);

    layoutController_->setPlacementHint(dock->objectName(), PreferredOpenArea::RestoreSaved);

    panelRegistry_->registerPanel(QStringLiteral("Inspector"), id, panel);

    connect(panel, &InspectorPanel::panelFocused,
            this, &MainWindow::setLastFocusedInspectorPanelId);
    wireInspectorDockCloseHandler(dock);
    wireInspectorPanelSignals(panel);

    refreshInspectorDockTitles();

    return panel;
}

void MainWindow::openItemInNewInspector(ProjectItem* item, PreferredOpenArea placement,
                                       bool syncSelection, const QObject* source)
{
    if (!item) return;

    InspectorPanel* panel = createInspectorPanel(placement);
    if (!panel) return;

    setLastFocusedInspectorPanelId(panel->panelId());
    if (syncSelection)
        selectionRouter_->routeSelection(item, false, source);
    else
        panel->showItemInspector(item);
}

InspectorPanel* MainWindow::inspectorShowingItem(ProjectItem* item, bool includeClosed) const
{
    if (!item) return nullptr;
    InspectorPanel* fallback = nullptr;
    for (InspectorPanel* p : allInspectorPanels()) {
        if (!p) continue;
        ads::CDockWidget* d = dockOfPanel(p);
        if (!d || (!includeClosed && d->isClosed())) continue;
        ProjectItem* ctx = p->currentContextItem();
        if (!ctx || ctx->guid != item->guid) continue;
        if (p->isPinned()) return p;
        if (!fallback) fallback = p;
    }
    return fallback;
}

void MainWindow::openOrFocusInspectorForItem(ProjectItem* item, const PlacementContext* placement)
{
    if (!item || !ItemTypeCatalog::isUserFacingType(item->type)) return;

    if (InspectorPanel* existing = inspectorShowingItem(item, true)) {
        if (placement) existing->showItemInspector(item, *placement);
        else existing->showItemInspector(item);
        if (ads::CDockWidget* d = dockOfPanel(existing)) {
            revealDock(d);
            d->setAsCurrentTab();
        }
        setLastFocusedInspectorPanelId(existing->panelId());
        return;
    }

    InspectorPanel* panel = createInspectorPanel(PreferredOpenArea::CentralTab);
    if (!panel) return;

    setLastFocusedInspectorPanelId(panel->panelId());
    if (placement) panel->showItemInspector(item, *placement);
    else panel->showItemInspector(item);
}

void MainWindow::wireInspectorDockCloseHandler(ads::CDockWidget* dock)
{
    auto* ip = qobject_cast<InspectorPanel*>(dock->widget());
    const int panelId = ip ? ip->panelId() : 0;
    connect(dock, &QObject::destroyed, panelRegistry_, [this, panelId]() {
        if (panelId > 0 && panelRegistry_)
            panelRegistry_->unregisterPanel(QStringLiteral("Inspector"), panelId);
    });
}

void MainWindow::wireInspectorPanelSignals(InspectorPanel* panel)
{
    if (!panel) return;

    connect(panel, &InspectorPanel::navigateToAction, this, [this](uint32_t actionObjectID) {
        auto* project = ProjectManager::instance().currentProject();
        if (!project || !projectExplorer_) return;

        std::function<ProjectItem*(ProjectItem*)> findByID = [&](ProjectItem* pi) -> ProjectItem* {
            if (pi->objectID == actionObjectID) return pi;
            for (auto& child : pi->children) {
                if (auto* found = findByID(child.get())) return found;
            }
            return nullptr;
        };

        for (auto* binding : project->allFileBindings()) {
            if (binding && binding->rootItem) {
                if (auto* actionItem = findByID(binding->rootItem.get())) {
                    projectExplorer_->selectProjectItem(actionItem);
                    return;
                }
            }
        }
    });

    connect(panel, &InspectorPanel::navigateToAttenuation, this,
        [this](const QString& attenuationName) {
        auto* project = ProjectManager::instance().currentProject();
        const auto* attenuation = AttenuationStore::find(project, QUuid(), attenuationName);
        ProjectItem* item = attenuation && !attenuation->hostItem()->guid.isNull()
            ? ProjectManager::instance().findItemByGuid(attenuation->hostItem()->guid)
            : nullptr;
        if (item)
            selectionRouter_->selectItem(item, true);
    });

    connect(panel, &InspectorPanel::curveSelectionRequested, this,
            [this](const CurveSelection& selection) {
        selectionRouter_->selectCurve(selection);
        openCurveEditor(selection);
    });
    connect(panel, &InspectorPanel::externalEditorRequested, this,
            [this](const ExternalEditorSelection& selection, bool addBinding) {
        auto* editor = openExternalEditor(selection);
        if (editor && addBinding)
            QTimer::singleShot(0, editor, &ExternalEditorPanel::showBindingAddMenu);
    });

    connect(panel, &InspectorPanel::titleContextChanged, this,
        [this](int, const QString&, const QString&) { refreshInspectorDockTitles(); });

    connect(panel, &InspectorPanel::editorLaunchRequested, this,
        [this](int, const QString& slot, ProjectItem* item,
               const PlacementContext* placement) {
            openEditorForSlot(slot, item, placement);
        });
}

void MainWindow::locateProjectItem(uint32_t objectID)
{
    auto* item = ProjectManager::instance().findItemByObjectID(objectID);
    if (!item) return;
    if (projectExplorer_ && projectExplorerDock_) {
        revealDock(projectExplorerDock_);
        projectExplorer_->pingProjectItem(item);
    }
}

void MainWindow::openEditorForSlot(const QString& slot, ProjectItem* item,
                                   const PlacementContext*)
{
    if (EditorLaunchMap::isSlotDisabled(slot)) {
        if (statusBar())
            statusBar()->showMessage(tr("The editor for this section is coming soon."), 4000);
        return;
    }

    const QString type = EditorLaunchMap::editorPanelTypeForSlot(slot);
    if (type.isEmpty() || !panelRegistry_->hasPanelType(type)) {
        if (statusBar())
            statusBar()->showMessage(tr("The editor for this section is coming soon."), 4000);
        return;
    }
    openStandaloneEditor(type, item);
}

void MainWindow::showInspectorForItem(InspectorPanel* panel, ProjectItem* item)
{
    if (!panel) return;
    panel->showItemInspector(item);
}

void MainWindow::closeInspectorPanel(int panelId)
{
    QWidget* w = panelRegistry_->findPanel(QStringLiteral("Inspector"), panelId);
    while (w && !qobject_cast<ads::CDockWidget*>(w))
        w = w->parentWidget();
    if (auto* dock = qobject_cast<ads::CDockWidget*>(w))
        dock->closeDockWidget();
}

QList<InspectorPanel*> MainWindow::allInspectorPanels() const
{
    QList<InspectorPanel*> panels;
    for (QWidget* w : panelRegistry_->allPanels(QStringLiteral("Inspector")))
    {
        if (auto* ip = qobject_cast<InspectorPanel*>(w))
            panels.append(ip);
    }
    return panels;
}

QList<TransitionEditorPanel*> MainWindow::allTransitionEditorPanels() const
{
    QList<TransitionEditorPanel*> panels;
    for (QWidget* w : panelRegistry_->allPanels(QStringLiteral("TransitionEditor")))
    {
        if (auto* editor = qobject_cast<TransitionEditorPanel*>(w))
            panels.append(editor);
    }
    return panels;
}

QList<GridEditorPanel*> MainWindow::allGridEditorPanels() const
{
    QList<GridEditorPanel*> panels;
    for (QWidget* w : panelRegistry_->allPanels(QStringLiteral("GridEditor")))
    {
        if (auto* editor = qobject_cast<GridEditorPanel*>(w))
            panels.append(editor);
    }
    return panels;
}

QList<TimelineEditorPanel*> MainWindow::allTimelineEditorPanels() const
{
    QList<TimelineEditorPanel*> panels;
    for (QWidget* w : panelRegistry_->allPanels(QStringLiteral("TimelineEditor")))
    {
        if (auto* editor = qobject_cast<TimelineEditorPanel*>(w))
            panels.append(editor);
    }
    return panels;
}

InspectorPanel* MainWindow::findInspectorPanel(int panelId) const
{
    return qobject_cast<InspectorPanel*>(
        panelRegistry_->findPanel(QStringLiteral("Inspector"), panelId));
}

int MainWindow::lastFocusedInspectorPanelId() const
{
    return panelRegistry_->lastFocusedPanelId(QStringLiteral("Inspector"));
}

void MainWindow::setLastFocusedInspectorPanelId(int panelId)
{
    panelRegistry_->setLastFocusedPanelId(QStringLiteral("Inspector"), panelId);
}

ads::CDockWidget* MainWindow::dockOfPanel(QWidget* w) const
{
    while (w && !qobject_cast<ads::CDockWidget*>(w))
        w = w->parentWidget();
    return qobject_cast<ads::CDockWidget*>(w);
}

InspectorPanel* MainWindow::activeInspectorPanel() const
{
    const int lastId = panelRegistry_->lastFocusedPanelId(QStringLiteral("Inspector"));
    InspectorPanel* firstVisible = nullptr;
    for (InspectorPanel* p : allInspectorPanels()) {
        if (!p) continue;
        if (p->isPinned()) continue;
        ads::CDockWidget* d = dockOfPanel(p);
        if (!d || d->isClosed()) continue;
        if (!firstVisible) firstVisible = p;
        if (p->panelId() == lastId) return p;
    }
    return firstVisible;
}

ads::CDockWidget* MainWindow::activeInspectorDock() const
{
    InspectorPanel* p = activeInspectorPanel();
    return p ? dockOfPanel(p) : nullptr;
}

ads::CDockAreaWidget* MainWindow::anyInspectorDockArea(bool includeFloating) const
{
    for (InspectorPanel* p : allInspectorPanels()) {
        if (!p) continue;
        ads::CDockWidget* d = dockOfPanel(p);
        if (d && !d->isClosed() && d->dockAreaWidget()
            && (includeFloating || !d->isInFloatingContainer()))
            return d->dockAreaWidget();
    }
    return nullptr;
}

ads::CDockAreaWidget* MainWindow::inspectorBelowStandaloneEditorArea() const
{
    ads::CDockAreaWidget* inspectorArea = anyInspectorDockArea();
    if (!inspectorArea)
        return nullptr;
    ads::CDockSplitter* splitter = inspectorArea->parentSplitter();
    if (!splitter || splitter->orientation() != Qt::Vertical)
        return nullptr;
    const int inspectorIndex = splitter->indexOf(inspectorArea);
    if (inspectorIndex < 0)
        return nullptr;

    auto matchesStandaloneEditor = [this](ads::CDockWidget* dock) {
        if (!dock || dock->isClosed())
            return false;
        const QString name = dock->objectName();
        for (const QString& type : standaloneEditorTypes_.keys()) {
            const StandaloneEditorTypeInfo ti = standaloneEditorTypes_.value(type);
            if (ti.openBelowCentralArea || ti.dockObjectPrefix.isEmpty())
                continue;
            if (name.startsWith(ti.dockObjectPrefix + QLatin1Char('_')))
                return true;
        }
        return false;
    };

    for (int i = inspectorIndex + 1; i < splitter->count(); ++i) {
        auto* area = qobject_cast<ads::CDockAreaWidget*>(splitter->widget(i));
        if (!area)
            continue;
        for (ads::CDockWidget* dock : area->dockWidgets()) {
            if (matchesStandaloneEditor(dock))
                return area;
        }
    }
    return nullptr;
}

void MainWindow::seedInspectorBelowEditorHeight(ads::CDockAreaWidget* inspectorArea,
                                                ads::CDockWidget* dock)
{
    if (!inspectorArea || !dock)
        return;
    ads::CDockAreaWidget* editorArea = dock->dockAreaWidget();
    if (!editorArea || editorArea == inspectorArea)
        return;
    ads::CDockSplitter* splitter = editorArea->parentSplitter();
    if (!splitter || splitter->orientation() != Qt::Vertical)
        return;
    if (inspectorArea->parentSplitter() != splitter)
        return;

    const int inspectorIndex = splitter->indexOf(inspectorArea);
    const int editorIndex = splitter->indexOf(editorArea);
    if (inspectorIndex < 0 || editorIndex < 0)
        return;

    QList<int> sizes = splitter->sizes();
    if (sizes.size() != splitter->count())
        return;
    int total = 0;
    for (int size : sizes)
        total += size;
    if (total <= 0)
        return;

    const int pair = sizes[inspectorIndex] + sizes[editorIndex];
    if (pair <= 0)
        return;
    const int editorSize = pair / 3;
    sizes[editorIndex] = editorSize;
    sizes[inspectorIndex] = pair - editorSize;
    splitter->setSizes(sizes);
}

bool MainWindow::isStandaloneEditorAtDefaultSlot(QWidget* editor) const
{
    ads::CDockWidget* dock = dockOfPanel(editor);
    if (!dock || dock->isClosed() || dock->isInFloatingContainer())
        return false;
    ads::CDockAreaWidget* area = dock->dockAreaWidget();
    if (!area)
        return false;
    if (ads::CDockAreaWidget* below = inspectorBelowStandaloneEditorArea())
        return area == below;
    ads::CDockAreaWidget* inspectorArea = anyInspectorDockArea();
    if (!inspectorArea)
        return false;
    ads::CDockSplitter* splitter = inspectorArea->parentSplitter();
    return splitter && splitter->orientation() == Qt::Vertical
        && area->parentSplitter() == splitter
        && splitter->indexOf(area) > splitter->indexOf(inspectorArea);
}

void MainWindow::setStandaloneEditorVisible(QWidget* editor, bool visible)
{
    ads::CDockWidget* dock = dockOfPanel(editor);
    if (!dock)
        return;
    if (visible)
        revealDock(dock);
    else
        dock->toggleView(false);
}

void MainWindow::wireStandaloneEditorPin(QWidget* editor)
{
    if (!editor)
        return;
    auto onPinToggled = [this](bool) {
        pushSelectionToStandaloneEditors(currentStandaloneEditorSelection(),
                                         StandaloneEditorPushReason::PinToggled);
    };
    if (auto* source = qobject_cast<SourceEditorPanel*>(editor))
        connect(source, &SourceEditorPanel::pinToggled, this, onPinToggled);
    else if (auto* curve = qobject_cast<ExternalEditorPanel*>(editor))
        connect(curve, &ExternalEditorPanel::pinToggled, this, onPinToggled);
    else if (auto* grid = qobject_cast<GridEditorPanel*>(editor))
        connect(grid, &GridEditorPanel::pinToggled, this, onPinToggled);
    else if (auto* timeline = qobject_cast<TimelineEditorPanel*>(editor))
        connect(timeline, &TimelineEditorPanel::pinToggled, this, onPinToggled);
    else if (auto* transition = qobject_cast<TransitionEditorPanel*>(editor))
        connect(transition, &TransitionEditorPanel::pinToggled, this, onPinToggled);
}

ProjectItem* MainWindow::currentStandaloneEditorSelection() const
{
    return selectionRouter_->activeItem();
}

void MainWindow::collapseUnrelatedStandaloneEditor(const QString& type, QWidget* editor,
                                                  StandaloneEditorPushReason reason)
{
    if (!editor)
        return;
    const StandaloneEditorTypeInfo ti = standaloneEditorTypes_.value(type);
    if (!StandaloneEditorDispatch::shouldCollapse(
            false, ti.isPinned && ti.isPinned(editor),
            isStandaloneEditorAtDefaultSlot(editor)))
        return;
    if (ti.clear)
        ti.clear(editor);
    if (StandaloneEditorDispatch::shouldHide(reason))
        setStandaloneEditorVisible(editor, false);
}

bool MainWindow::anyInspectorVisible() const
{
    for (InspectorPanel* p : allInspectorPanels()) {
        if (!p) continue;
        ads::CDockWidget* d = dockOfPanel(p);
        if (d && !d->isClosed())
            return true;
    }
    return false;
}

namespace {

QString tailPath(const QStringList& path, int k)
{
    const int start = qMax(0, int(path.size()) - k);
    QStringList seg;
    for (int j = start; j < path.size(); ++j)
        seg << path[j];
    return seg.join(QLatin1Char('/'));
}

QStringList instanceTitles(const QString& base, const QList<QStringList>& paths)
{
    const int n = paths.size();
    QList<int> depth(n, 1);

    QHash<QString, QList<int>> byLeaf;
    for (int i = 0; i < n; ++i)
        if (!paths[i].isEmpty())
            byLeaf[paths[i].last()] << i;

    for (auto it = byLeaf.constBegin(); it != byLeaf.constEnd(); ++it) {
        const QList<int>& idx = it.value();
        if (idx.size() < 2) continue;
        int maxLen = 1;
        for (int i : idx) maxLen = qMax(maxLen, int(paths[i].size()));
        int chosen = maxLen;
        for (int k = 1; k <= maxLen; ++k) {
            QStringList seen;
            bool uniq = true;
            for (int i : idx) {
                const QString t = tailPath(paths[i], k);
                if (seen.contains(t)) { uniq = false; break; }
                seen << t;
            }
            if (uniq) { chosen = k; break; }
        }
        for (int i : idx) depth[i] = chosen;
    }

    QStringList out;
    out.reserve(n);
    for (int i = 0; i < n; ++i)
        out << (paths[i].isEmpty()
                    ? base
                    : base + QStringLiteral(" - ") + tailPath(paths[i], depth[i]));
    return out;
}

}

void MainWindow::refreshInspectorDockTitles()
{
    const QString base = tr("Inspector");

    QList<ads::CDockWidget*> docks;
    QList<QStringList> paths;
    for (InspectorPanel* p : allInspectorPanels()) {
        if (!p) continue;
        ads::CDockWidget* d = dockOfPanel(p);
        if (!d) continue;
        docks << d;

        QStringList path;
        const QString leaf = p->titleObjectName();
        if (!leaf.isEmpty()) {
            QStringList anc;
            for (ProjectItem* a = p->currentContextItem() ? p->currentContextItem()->parent : nullptr;
                 a; a = a->parent)
                anc << a->name;
            for (int j = anc.size() - 1; j >= 0; --j)
                path << anc[j];
            path << leaf;
        }
        paths << path;
    }

    const QStringList titles = instanceTitles(base, paths);
    for (int i = 0; i < docks.size(); ++i) {
        if (docks[i]->windowTitle() != titles[i])
            docks[i]->setWindowTitle(titles[i]);
    }
}

void MainWindow::refreshBoundEditorDockTitles(
    const QString& base, const QList<QWidget*>& panels,
    const std::function<ProjectItem*(QWidget*)>& boundItem)
{
    QList<ads::CDockWidget*> docks;
    QList<QStringList> paths;
    for (QWidget* p : panels) {
        if (!p) continue;
        ads::CDockWidget* d = dockOfPanel(p);
        if (!d) continue;
        docks << d;

        QStringList path;
        if (ProjectItem* item = boundItem(p)) {
            QStringList anc;
            for (ProjectItem* a = item->parent; a; a = a->parent)
                anc << a->name;
            for (int j = anc.size() - 1; j >= 0; --j)
                path << anc[j];
            path << item->name;
        }
        paths << path;
    }

    const QStringList titles = instanceTitles(base, paths);
    for (int i = 0; i < docks.size(); ++i) {
        if (docks[i]->windowTitle() != titles[i])
            docks[i]->setWindowTitle(titles[i]);
    }
}

void MainWindow::refreshTransitionEditorDockTitles()
{
    QList<QWidget*> panels;
    for (TransitionEditorPanel* p : allTransitionEditorPanels())
        panels << p;
    refreshBoundEditorDockTitles(tr("Transition Editor"), panels,
        [](QWidget* w) -> ProjectItem* {
            auto* p = qobject_cast<TransitionEditorPanel*>(w);
            return p ? p->boundItem() : nullptr;
        });
}

void MainWindow::refreshGridEditorDockTitles()
{
    QList<QWidget*> panels;
    for (GridEditorPanel* p : allGridEditorPanels())
        panels << p;
    refreshBoundEditorDockTitles(tr("Selection Editor"), panels,
        [](QWidget* w) -> ProjectItem* {
            auto* p = qobject_cast<GridEditorPanel*>(w);
            return p ? p->boundItem() : nullptr;
        });
}

void MainWindow::refreshTimelineEditorDockTitles()
{
    QList<QWidget*> panels;
    for (TimelineEditorPanel* p : allTimelineEditorPanels())
        panels << p;
    refreshBoundEditorDockTitles(tr("Timeline Editor"), panels,
        [](QWidget* w) -> ProjectItem* {
            auto* p = qobject_cast<TimelineEditorPanel*>(w);
            return p ? p->boundItem() : nullptr;
        });
}


Hierarchy* MainWindow::createHierarchyPanel()
{
    return createHierarchyPanelWithId(
        panelRegistry_->allocatePanelId(QStringLiteral("Hierarchy")), false);
}

Hierarchy* MainWindow::createHierarchyPanelWithId(int id, bool deferShow, bool deleteOnClose)
{
    auto* panel = new Hierarchy();
    panel->setPanelId(id);

    auto* dock = WorkspaceDockFactory::createDock(dockManager_, {
        tr("Hierarchy"),
        QString("HierarchyDock_%1").arg(id),
        panel,
        deleteOnClose,
        !deferShow,
        true
    });

    panelRegistry_->registerPanel(QStringLiteral("Hierarchy"), id, panel);

    connect(panel, &Hierarchy::panelFocused,
            this, &MainWindow::setLastFocusedHierarchyPanelId);
    connect(panel, &Hierarchy::itemSelected,
            selectionRouter_, &SelectionRouter::onProjectItemSelected);
    connect(panel, &Hierarchy::itemDoubleClicked,
            selectionRouter_, &SelectionRouter::onProjectItemDoubleClicked);
    connect(panel, &Hierarchy::audioFileSelected,
            selectionRouter_, &SelectionRouter::onAudioFileSelected);
    connect(panel, &Hierarchy::actionSelected,
            selectionRouter_, &SelectionRouter::onActionSelected);
    connect(panel, &Hierarchy::openInNewTabRequested, this, [this](ProjectItem* item) {
        openItemInNewInspector(item, PreferredOpenArea::CentralTab, false);
    });
    connect(panel, &Hierarchy::openInNewWindowRequested, this, [this, panel](ProjectItem* item) {
        openItemInNewInspector(item, PreferredOpenArea::Float, true, panel);
    });
    connect(panel, &Hierarchy::openContainingFolderRequested, this, [this](ProjectItem* item) {
        selectionRouter_->revealItemSourceInExplorer(item);
    });
    if (auto* project = ProjectManager::instance().currentProject()) {
        panel->onProjectOpened(project);
        panel->rebuildBusTree();
    }

    wireHierarchyDockCloseHandler(dock);

    return panel;
}

void MainWindow::wireHierarchyDockCloseHandler(ads::CDockWidget* dock)
{
    auto* hp = qobject_cast<Hierarchy*>(dock->widget());
    const int panelId = hp ? hp->panelId() : 0;
    connect(dock, &QObject::destroyed, panelRegistry_, [this, panelId]() {
        if (panelId > 0 && panelRegistry_)
            panelRegistry_->unregisterPanel(QStringLiteral("Hierarchy"), panelId);
    });
}

void MainWindow::closeHierarchyPanel(int panelId)
{
    QWidget* w = panelRegistry_->findPanel(QStringLiteral("Hierarchy"), panelId);
    while (w && !qobject_cast<ads::CDockWidget*>(w))
        w = w->parentWidget();
    if (auto* dock = qobject_cast<ads::CDockWidget*>(w))
        dock->closeDockWidget();
}

QList<Hierarchy*> MainWindow::allHierarchyPanels() const
{
    QList<Hierarchy*> panels;
    for (QWidget* w : panelRegistry_->allPanels(QStringLiteral("Hierarchy")))
    {
        if (auto* hp = qobject_cast<Hierarchy*>(w))
            panels.append(hp);
    }
    return panels;
}

Hierarchy* MainWindow::findHierarchyPanel(int panelId) const
{
    return qobject_cast<Hierarchy*>(
        panelRegistry_->findPanel(QStringLiteral("Hierarchy"), panelId));
}

int MainWindow::lastFocusedHierarchyPanelId() const
{
    return panelRegistry_->lastFocusedPanelId(QStringLiteral("Hierarchy"));
}

void MainWindow::setLastFocusedHierarchyPanelId(int panelId)
{
    panelRegistry_->setLastFocusedPanelId(QStringLiteral("Hierarchy"), panelId);
}

void MainWindow::connectSignals()
{
    connect(projectExplorer_, &ProjectExplorer::itemSelected,
            selectionRouter_, &SelectionRouter::onProjectItemSelected);
    connect(projectExplorer_, &ProjectExplorer::itemDoubleClicked,
            selectionRouter_, &SelectionRouter::onProjectItemDoubleClicked);
    connect(projectExplorer_, &ProjectExplorer::audioFileSelected,
            selectionRouter_, &SelectionRouter::onAudioFileSelected);
    connect(projectExplorer_, &ProjectExplorer::actionSelected,
            selectionRouter_, &SelectionRouter::onActionSelected);
    connect(projectExplorer_, &ProjectExplorer::openInNewTabRequested,
            this, [this](ProjectItem* item) {
        openItemInNewInspector(item, PreferredOpenArea::CentralTab, false);
    });
    connect(projectExplorer_, &ProjectExplorer::openInNewWindowRequested,
            this, [this](ProjectItem* item) {
        openItemInNewInspector(item, PreferredOpenArea::Float, true, projectExplorer_);
    });
    connect(projectExplorer_, &ProjectExplorer::openContainingFolderRequested,
            this, [this](ProjectItem* item) {
        selectionRouter_->revealItemSourceInExplorer(item);
    });
    connect(projectExplorer_, &ProjectExplorer::sourceEditorRequested,
            this, [this](uint32_t sourceId) {
        openSourceEditor(sourceId);
    });

    connect(&ProjectManager::instance(), &ProjectManager::projectClosed,
            this, [this]() {
        busController_->clearSelection();
        for (auto* ip : allInspectorPanels())
            if (ip) ip->showEmpty();
    });

    connect(&ProjectManager::instance(), &ProjectManager::externalFilesAdded,
            this, [this](const QStringList& paths) {
        statusBar()->showMessage(tr("%1 file(s) added externally").arg(paths.size()), 5000);
    });
    connect(&ProjectManager::instance(), &ProjectManager::externalFilesRemoved,
            this, [this](const QStringList& paths) {
        statusBar()->showMessage(tr("%1 file(s) removed externally").arg(paths.size()), 5000);
    });

    connect(&ProjectManager::instance(), &ProjectManager::externalFilesModified,
            this, [this](const QStringList& paths) {
        int swapCount = 0;
        for (const QString& filePath : paths) {
            QString ext = QFileInfo(filePath).suffix().toLower();
            if (!AudioFormatCatalog::containsExtension(ext))
                continue;
            uint32_t sourceID =
                HarkenEditor::Bridge::mediaRegistryFindSourceIdByPath(filePath);
            if (sourceID == 0) {
                const AssetInfo* asset = AssetRegistry::instance().findAssetByPath(filePath);
                sourceID = asset ? asset->sourceID : 0;
            }
            if (sourceID == 0)
                continue;
            bool ok = AudioEngine::instance().swapMedia(sourceID, filePath.toStdString());
            if (ok) ++swapCount;
        }
        if (swapCount > 0)
            statusBar()->showMessage(
                tr("Hot-swapped %1 audio file(s)").arg(swapCount), 5000);
    });
    auto* externalChangeTimer = new QTimer(this);
    externalChangeTimer->setSingleShot(true);
    const auto externalChangesPending = std::make_shared<bool>(false);
    connect(&ProjectManager::instance(), &ProjectManager::packExternallyModified,
            this, [externalChangeTimer, externalChangesPending](const QString&) {
        *externalChangesPending = true;
        if (!externalChangeTimer->isActive()) externalChangeTimer->start(0);
    });
    const auto clearExternalChanges = [externalChangeTimer, externalChangesPending] {
        externalChangeTimer->stop();
        *externalChangesPending = false;
    };
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed,
            externalChangeTimer, clearExternalChanges);
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened,
            externalChangeTimer, clearExternalChanges);
    connect(&ProjectManager::instance(), &ProjectManager::projectSaved,
            externalChangeTimer, clearExternalChanges);
    connect(&PackGenerationRecordStore::instance(), &PackGenerationRecordStore::exportActivityChanged,
            externalChangeTimer, [externalChangeTimer, externalChangesPending](bool exporting) {
        if (!exporting && *externalChangesPending) externalChangeTimer->start(0);
    });
    connect(externalChangeTimer, &QTimer::timeout,
            this, [this, externalChangesPending, promptOpen = false]() mutable {
        auto& manager = ProjectManager::instance();
        const auto* project = manager.currentProject();
        if (promptOpen || !project || PackGenerationRecordStore::instance().isExporting()) return;
        *externalChangesPending = false;
        const QString projectPath = project->projectPath;
        promptOpen = true;
        QMessageBox dialog(QMessageBox::Question, tr("External Project Changes"),
            tr("Project files or folders changed outside Studio.\n\n"
               "Reload discards all unsaved changes and reloads the entire project. "
               "Keep Editing retains the current project; the next save will overwrite external project changes."),
            QMessageBox::Yes | QMessageBox::No, this);
        dialog.setObjectName(QStringLiteral("externalProjectChangeDialog"));
        dialog.button(QMessageBox::Yes)->setText(tr("Reload"));
        dialog.button(QMessageBox::No)->setText(tr("Keep Editing"));
        dialog.setDefaultButton(QMessageBox::No);
        const auto answer = dialog.exec();
        promptOpen = false;
        if (answer == QMessageBox::Yes && manager.currentProject()
            && manager.currentProject()->projectPath == projectPath)
            manager.loadProject(projectPath);
    });

    connect(&ProjectManager::instance(), &ProjectManager::busChanged,
            this, [this](uint32_t busID, const QString& parameterName, const QObject* origin) {
        busController_->handleBusParameterChanged(busID, parameterName, origin);
    });

    auto scheduleBusRefreshOnLifecycle = [this]() {
        busController_->scheduleBusRefresh();
    };
    connect(&ProjectManager::instance(), &ProjectManager::itemCreated,
            this, [scheduleBusRefreshOnLifecycle](ProjectItem*) { scheduleBusRefreshOnLifecycle(); });
    connect(&ProjectManager::instance(), &ProjectManager::itemDeleted,
            this, [scheduleBusRefreshOnLifecycle](ProjectItem*) { scheduleBusRefreshOnLifecycle(); });
    connect(&ProjectManager::instance(), &ProjectManager::itemMoved,
            this, [scheduleBusRefreshOnLifecycle](ProjectItem*, ProjectItem*, ProjectItem*) { scheduleBusRefreshOnLifecycle(); });
    connect(&ProjectManager::instance(), &ProjectManager::itemRenamed,
            this, [scheduleBusRefreshOnLifecycle](ProjectItem*, const QString&) { scheduleBusRefreshOnLifecycle(); });


    connect(&ProjectManager::instance(), &ProjectManager::errorOccurred,
            this, [this](const QString& error) {
                const QString userError = ProjectUpgradeMessages::userMessage(error);
                if (ipcServer_ && ipcServer_->isDispatchingCommand()) {
                    qWarning("[ProjectManager] error during IPC dispatch "
                             "(modal suppressed): %s", qUtf8Printable(error));
                    if (statusButton_)
                        setStatusMessage(userError, LogEntry::Error);
                    return;
                }
                QMessageBox::warning(this, tr("Error"), userError);
            });
    connect(&ProjectManager::instance(), &ProjectManager::warningOccurred,
            this, [this](const QString& warning) {
                LogPanel::logWarning(LogCategory::File, warning);
            });
}

void MainWindow::finishStartupPresentation(bool shown)
{
    if (startupPresentationDone_)
        return;
    startupPresentationDone_ = true;
    startupPresentationCompleting_ = false;
    emit startupPresentationFinished(shown);
}

void MainWindow::completeStartupPresentation()
{
    if (startupPresentationDone_ || startupPresentationCompleting_)
        return;
    startupPresentationCompleting_ = true;
    if (!isVisible() || !windowHandle()) {
        finishStartupPresentation(false);
        return;
    }
    if (!startupStateChanged_ && !isMaximized() && !isFullScreen() && !isMinimized()) {
        resize(startupNormalSize_);
        const QSize normalSize = centerWindowOnScreen(this, startupScreen_);
        if (normalSize.isValid() && !normalSize.isEmpty())
            normalWindowSize_ = normalSize;
        if (startupState_ != Qt::WindowNoState)
            setWindowState(startupState_);
    }
    finishStartupPresentation(true);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (startupPresentationDone_ && !isMaximized() && !isFullScreen() && !isMinimized())
        normalWindowSize_ = size();
}

void MainWindow::prepareWindowForStartup(QScreen* screen)
{
    if (startupPresentationPrepared_)
        return;
    startupScreen_ = startupTargetScreen(screen);
    QSettings settings;
    settings.beginGroup("MainWindow");
    const WindowPresentation presentation = readWindowPresentation(settings);
    startupNormalSize_ = presentation.normalSize;
    startupState_ = presentation.state;
    normalWindowSize_ = presentation.normalSize;
    setWindowState(Qt::WindowNoState);
    if (startupScreen_)
        setScreen(startupScreen_);
    resize(startupNormalSize_);
    if (startupScreen_) {
        const QRect position = centeredWindowFrame(size(), startupScreen_->availableGeometry());
        if (position.isValid())
            move(position.topLeft());
    }
    startupPresentationPrepared_ = true;
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (!startupPresentationPrepared_)
        prepareWindowForStartup(screen());
    if (!layoutController_->firstShowDone()) {
        layoutController_->setFirstShowDone(true);
        dockManager_->setFocus(Qt::OtherFocusReason);
        layoutController_->relayoutDockSplitters(false);
        QTimer::singleShot(0, this, [this]() {
            layoutController_->relayoutDockSplitters(!layoutController_->currentLayoutFromCache());
        });
    }
    if (!startupPresentationScheduled_ && !startupTargetScreen(startupScreen_)) {
        startupPresentationScheduled_ = true;
        QTimer::singleShot(0, this, [this]() { completeStartupPresentation(); });
    }
}


void MainWindow::relayoutDockSplitters(bool seedSizes)
{
    layoutController_->relayoutDockSplitters(seedSizes);
}





void MainWindow::enforceDisabledDocks()
{
    layoutController_->enforceDisabledDocks();
}

void MainWindow::applyDarkPalette()
{
    qApp->setPalette(QtControlSkin::current().applicationPalette());

    QPixmapCache::clear();
    qApp->postEvent(qApp, new QEvent(QEvent::ApplicationPaletteChange));
}

void MainWindow::loadSettings()
{
    QSettings settings;
    settings.beginGroup("MainWindow");

    if (settings.contains("windowState"))
        restoreState(settings.value("windowState").toByteArray());

    settings.endGroup();

    QRect screenGeom;
    if (auto* screen = QGuiApplication::primaryScreen())
        screenGeom = screen->availableGeometry();
    if (dockManager_) {
        for (auto* floatingContainer : dockManager_->floatingWidgets()) {
            if (!screenGeom.isNull() && !screenGeom.intersects(floatingContainer->geometry())) {
                floatingContainer->move(screenGeom.topLeft());
            }
        }
    }

    enforceDisabledDocks();
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.beginGroup("MainWindow");
    saveWindowPresentation(settings, *this, normalWindowSize_);
    settings.setValue("windowState", saveState());
    settings.endGroup();
}


void MainWindow::loadProjectLayout()
{
    refreshProjectSettingsAvailability();
    layoutController_->loadProjectLayout();
    pushSelectionToStandaloneEditors(currentStandaloneEditorSelection(),
                                     StandaloneEditorPushReason::RestoreSync);
}

void MainWindow::saveProjectLayout()
{
    layoutController_->saveProjectLayout();
}

bool MainWindow::saveNamedLayout(const QString& name, QString* pathOut)
{
    return layoutController_->saveNamedLayout(name, pathOut);
}

bool MainWindow::loadNamedLayout(const QString& name)
{
    const QString trimmed = name.trimmed();
    return layoutController_->loadNamedLayout(trimmed);
}

bool MainWindow::deleteNamedLayout(const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return false;
    if (!NamedLayoutStore::remove(trimmed))
        return false;
    layoutController_->onNamedLayoutRemoved(trimmed);
    return true;
}

void MainWindow::rebuildLayoutsMenu()
{
    if (layoutController_)
        layoutController_->rebuildLayoutsMenu();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (PackGenerationRecordStore::instance().isExporting()) {
        PackGenerationRecordStore::instance().cancelExport();
        event->ignore();
        return;
    }
    if (maybeSave())
    {
        saveProjectLayout();
        saveSettings();
        event->accept();
        if (startupPresentationPrepared_)
            finishStartupPresentation(false);
    }
    else
    {
        event->ignore();
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    const auto type = event->type();
    if (startupPresentationPrepared_ && !startupPresentationDone_
        && !startupPresentationCompleting_) {
        if (watched == this && type == QEvent::WindowStateChange) {
            startupStateChanged_ = true;
            if (!startupPresentationScheduled_) {
                startupPresentationScheduled_ = true;
                QTimer::singleShot(0, this, [this]() { completeStartupPresentation(); });
            }
        }
        if (watched == this && type == QEvent::Hide && !isMinimized())
            finishStartupPresentation(false);
        if (watched == windowHandle() && type == QEvent::Expose
            && windowHandle()->isExposed() && !startupPresentationScheduled_) {
            startupPresentationScheduled_ = true;
            QTimer::singleShot(0, this, [this]() { completeStartupPresentation(); });
        }
    }

    if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease
        || type == QEvent::Wheel) {
        auto* w = qobject_cast<QWidget*>(watched);
        if (w) {
            QWidget* named = w;
            while (named && named->objectName().isEmpty())
                named = named->parentWidget();
            if (named) {
                lastInteractedWidget_ = named;
                lastInteractionType_ = (type == QEvent::MouseButtonPress)   ? QStringLiteral("mouse_press")
                                     : (type == QEvent::MouseButtonRelease) ? QStringLiteral("mouse_release")
                                                                            : QStringLiteral("wheel");
                lastInteractionTimeMs_ = QDateTime::currentMSecsSinceEpoch();
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::maybeSave()
{
    finishProjectSettingsEditing();
    if (!projectModified_)
        return true;
    
    QMessageBox::StandardButton ret = QMessageBox::warning(this, tr("Harken Studio"),
        tr("The project has been modified.\nDo you want to save your changes?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    
    if (ret == QMessageBox::Save)
    {
        onSaveProject();
        return !projectModified_;
    }
    else if (ret == QMessageBox::Cancel)
    {
        return false;
    }
    
    return true;
}

void MainWindow::updateWindowTitle()
{
    QString projectName;
    
    ProjectData* proj = ProjectManager::instance().currentProject();
    if (proj && !proj->name.isEmpty())
    {
        projectName = proj->name;
    }
    else if (!currentProjectPath_.isEmpty())
    {
        projectName = QFileInfo(currentProjectPath_).baseName();
    }

    QString title = "Harken Studio";
    if (!projectName.isEmpty())
        title += " - " + projectName;
    
    if (projectModified_)
    {
        title += " *";
    }
    
    setWindowTitle(title);
#ifdef HARKEN_HAS_QWINDOWKIT
    if (m_titleBar)
        m_titleBar->setProjectTitle(projectName, projectModified_);
#endif
}

void MainWindow::setProjectModified(bool modified)
{
    if (projectModified_ != modified)
    {
        projectModified_ = modified;
        updateWindowTitle();
    }
}

void MainWindow::initEmptyProject()
{
    currentProjectPath_.clear();
    projectExplorer_->newProject("Untitled");

    UndoRedoManager::instance().clear();

    setProjectModified(false);
    updateWindowTitle();
}

bool MainWindow::createProjectFromConfig(
    const ProjectConfig& config,
    std::function<void(StartupPhase)> startupProgress)
{
    if (projectOpenInProgress_)
        return false;
    auto channelConfigKey = [](AudioChannelConfig c) -> QString {
        switch (c) {
            case AudioChannelConfig::Mono:         return QStringLiteral("mono");
            case AudioChannelConfig::Stereo_2_0:   return QStringLiteral("stereo");
            case AudioChannelConfig::Surround_5_1: return QStringLiteral("5_1");
            case AudioChannelConfig::Surround_7_1: return QStringLiteral("7_1");
            case AudioChannelConfig::Atmos_7_1_4:  return QStringLiteral("atmos_7_1_4");
        }
        return QStringLiteral("stereo");
    };
    QJsonArray platforms;
    for (const auto& p : config.platforms) platforms.append(p.toJson());

    QJsonObject args{
        {"name",             config.name},
        {"location",         config.location},
        {"sample_rate",      config.sampleRate},
        {"author",           config.author},
        {"description",      config.description},
        {"channel_config",   channelConfigKey(config.channelConfig)},
        {"platforms", platforms},
        {"template_id",      config.templateId},
        {"template_data",    config.templateData},
    };

    auto res = ActionManager::instance().execute(
        QStringLiteral("create_project"), args, ActionSource::GUI);
    if (!res.success)
    {
        QMessageBox::warning(this, tr("Error"), tr("Failed to create project \"%1\"").arg(config.name));
        LogPanel::logError(LogCategory::File, tr("Failed to create new project: %1").arg(config.name));
        return false;
    }

    ProjectData* pmProject = ProjectManager::instance().currentProject();
    if (pmProject)
        currentProjectPath_ = pmProject->projectPath;

    projectExplorer_->newProject(pmProject ? pmProject->name : config.name);

    UndoRedoManager::instance().clearHistory();

    setProjectModified(false);
    updateWindowTitle();
    addRecentProject(currentProjectPath_);

    loadProjectLayout();

    if (startupProgress)
        startupProgress(StartupPhase::RestoringWorkspace);

    setStatusMessage(tr("New project created: %1").arg(currentProjectPath_));
    LogPanel::logInfo(LogCategory::File, tr("New project created: %1").arg(currentProjectPath_));
    return true;
}

void MainWindow::onNewProject()
{
    if (!maybeSave())
        return;

    VNX::NewProjectDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const auto dlgResult = dlg.result();
    const QString projectName = dlgResult.projectName;

    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Project Location"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (dir.isEmpty())
        return;

    saveProjectLayout();

    ProjectConfig config;
    config.name = projectName;
    config.location = dir;
    config.author = "";
    config.description = "";
    config.templateId   = dlgResult.templateId;
    config.templateData = dlgResult.templateData.toJson();

    createProjectFromConfig(config);
}

void MainWindow::onOpenProject()
{
    onOpenProjectViaHub();
}

void MainWindow::onNewProjectViaHub()
{
    QString error;
    if (!HubLauncher::open(HubPage::NewProject, &error))
        QMessageBox::warning(this, tr("Harken Hub"), error);
}

void MainWindow::onOpenProjectViaHub()
{
    QString error;
    if (!HubLauncher::open(HubPage::Projects, &error))
        QMessageBox::warning(this, tr("Harken Hub"), error);
}

void MainWindow::onSaveProject()
{
    finishProjectSettingsEditing();
    if (currentProjectPath_.isEmpty())
    {
        onSaveProjectAs();
        return;
    }
    
    if (ProjectManager::instance().saveProject())
    {
        setProjectModified(false);

        saveProjectLayout();

        setStatusMessage(tr("Saved: %1").arg(currentProjectPath_));
    }
    else
    {
        LogPanel::logError(LogCategory::File, tr("Failed to save project"));
    }
}

void MainWindow::onSaveProjectAs()
{
    finishProjectSettingsEditing();
    ProjectData* pmProject = ProjectManager::instance().currentProject();
    QString defaultName = pmProject ? pmProject->name : QStringLiteral("Untitled");

    QString fileName = QFileDialog::getSaveFileName(
        this, tr("Save Project"),
        defaultName + ".hsp",
        tr("Harken Studio Projects (*.hsp);;All Files (*)"));

    if (fileName.isEmpty())
        return;

    auto acquisition = ProjectInstance::acquire(fileName);
    if (acquisition.status == ProjectInstance::Status::Forwarded)
        return;
    if (acquisition.status == ProjectInstance::Status::Failed) {
        QMessageBox::warning(this, tr("Save Project"), acquisition.error);
        return;
    }

    const QString previousPath = currentProjectPath_;
    const QString previousProjectPath = pmProject ? pmProject->projectPath : QString();
    const QString previousName = pmProject ? pmProject->name : QString();
    currentProjectPath_ = fileName;

    if (pmProject)
    {
        pmProject->projectPath = fileName;
        pmProject->name = QFileInfo(fileName).baseName();
    }

    auto& manager = ProjectManager::instance();
    const bool wasWatching = manager.fileWatcher()->isWatching();
    if (!manager.saveProject()) {
        currentProjectPath_ = previousPath;
        if (pmProject) {
            pmProject->projectPath = previousProjectPath;
            pmProject->name = previousName;
        }
        if (wasWatching)
            manager.startWatchingCurrentProject();
        LogPanel::logError(LogCategory::File, tr("Failed to save project"));
        return;
    }
    manager.m_projectInstance = std::move(acquisition.instance);
    setProjectModified(false);
    saveProjectLayout();
    setStatusMessage(tr("Saved: %1").arg(currentProjectPath_));
    updateWindowTitle();
}

void MainWindow::onEngineIntegrations()
{
    const ProjectData* project = ProjectManager::instance().currentProject();
    const QString projectPath = project != nullptr ? project->projectPath
                                                    : QString();
    QString error;
    if (!HubLauncher::open(HubPage::EditIntegration, &error, false,
                           projectPath)) {
        QMessageBox::warning(this, tr("Engine Integrations"), error);
    }
}

static QString packBuildSourceLabel(ProjectPackExporter::PackBuildRecord::Source source)
{
    switch (source) {
        case ProjectPackExporter::PackBuildRecord::Source::Init:   return QStringLiteral("Init");
        case ProjectPackExporter::PackBuildRecord::Source::Shared: return QStringLiteral("Shared");
        case ProjectPackExporter::PackBuildRecord::Source::Group:  return QStringLiteral("Group");
        case ProjectPackExporter::PackBuildRecord::Source::Action: return QStringLiteral("Action");
    }
    return QString();
}

void MainWindow::recordPackGenerationResult(const QString& packName, uint32_t objectID,
                                            bool success, const QString& outputPath,
                                            const QString& error, qint64 bytes,
                                            qint64 elapsedMs, const QString& platform,
                                            const QString& source,
                                            const QString& warnings,
                                            const QString& statusOverride,
                                            const QString& language,
                                            const QJsonObject* platformDefinition,
                                            const QString& unitKey, bool canceled)
{
    PackGenerationRecord row;
    row.packName   = packName;
    row.objectID   = objectID;
    row.source     = source;
    row.platform   = platform;
    row.language   = language;
    row.success    = success;
    row.canceled   = canceled;
    row.status     = !statusOverride.isEmpty() ? statusOverride
                                               : (success ? tr("Built") : tr("Failed"));
    row.outputPath = outputPath;
    row.warnings   = warnings;
    row.errors     = error;
    row.bytes      = bytes;
    row.elapsedMs  = elapsedMs;
    row.unitKey = unitKey;
    if (platformDefinition && !platformDefinition->isEmpty()) {
        row.platformDefinition = *platformDefinition;
        row.platformName = platformDefinition->value(QStringLiteral("name")).toString();
        row.basePlatform = ProjectPlatformSemantics::parseBasePlatform(
            platformDefinition->value(QStringLiteral("basePlatform")).toString()).value();
    }
    PackGenerationRecordStore::instance().add(row);
    QStringList details;
    if (!row.platformName.isEmpty()) details << row.platformName;
    if (!row.language.isEmpty()) details << row.language;
    if (row.bytes > 0) details << QLocale().formattedDataSize(row.bytes);
    if (row.elapsedMs >= 0) details << tr("%1 ms").arg(row.elapsedMs);
    const QString suffix = details.isEmpty() ? QString()
        : QStringLiteral(" (%1)").arg(details.join(QStringLiteral(", ")));
    if (row.canceled) {
        LogPanel::logInfo(LogCategory::Pack, tr("Export canceled: %1%2").arg(row.packName, suffix));
    } else if (!row.success) {
        LogPanel::logError(LogCategory::Pack,
            tr("Export failed: %1%2 - %3").arg(row.packName, suffix,
                row.errors.isEmpty() ? tr("(no detail)") : row.errors));
    } else if (!row.warnings.isEmpty()) {
        LogPanel::logWarning(LogCategory::Pack,
            tr("%1: %2%3 - %4").arg(row.status, row.packName, suffix, row.warnings));
    } else {
        LogPanel::logInfo(LogCategory::Pack,
            tr("%1: %2%3").arg(row.status, row.packName, suffix));
    }
}

void MainWindow::generateProjectPacks(const QStringList& platforms,
                                      const QStringList& packPaths,
                                      bool includeUnchecked,
                                      bool forceReexport, bool selfCleaning)
{
    finishProjectSettingsEditing();
    auto& progressStore = PackGenerationRecordStore::instance();
    if (progressStore.isExporting()) return;
    const auto projectLease = ProjectManager::instance().currentProjectLease();
    if (!projectLease) return;
    PropertyEditSession::finishAllActive(true);
    auto exportSelection = ProjectManager::instance().packExportSelection();
    if (includeUnchecked)
        exportSelection.actionTargeting.clear();
    QString snapshotError;
    const auto snapshot = ProjectBuildSnapshot::capture(*projectLease, &snapshotError);
    if (!snapshot) {
        LogPanel::logError(LogCategory::Pack, snapshotError);
        return;
    }
    const ProjectData* pmProject = &snapshot->project;

    const auto platformActivity = ProjectPlatformActivity::acquire();

    const QStringList genPlatforms = platforms;
    if (genPlatforms.isEmpty()) {
        LogPanel::logWarning(LogCategory::Pack, tr("Export skipped: no platform selected"));
        return;
    }

    for (const auto& id : genPlatforms) {
        const auto* definition = ProjectPlatformSemantics::find(pmProject->platforms, QUuid::fromString(id));
        if (!definition) {
            LogPanel::logError(LogCategory::Pack, tr("Unknown project platform ID: %1").arg(id));
            return;
        }
    }
    const QString baseDir = PathUtils::packBuildOutputRoot(
        pmProject->projectPath, pmProject->packBuildOutputDir);
    if (PathUtils::isProtectedPackOutputPath(baseDir, pmProject->projectPath)) {
        LogPanel::logError(LogCategory::Pack, PathUtils::packBuildOutputLocationError());
        return;
    }

    QString outputLockError;
    const auto outputLock = ProjectPackExporter::LockPackOutputRoot(baseDir, &outputLockError);
    if (!outputLock) {
        LogPanel::logError(LogCategory::Pack, outputLockError);
        return;
    }
    {
        const auto readiness =
            ProjectPackExporter::ValidatePackBuildReadiness(*pmProject, baseDir);
        for (const auto& issue : readiness.issues) {
            if (issue.severity == ProjectPackExporter::ValidationIssue::Info)
                continue;
            const QString line =
                issue.message
                + (issue.objectID != 0
                       ? QStringLiteral(" (object 0x%1)")
                             .arg(issue.objectID, 8, 16, QChar('0'))
                       : QString());
            if (issue.severity == ProjectPackExporter::ValidationIssue::Error)
                LogPanel::logError(LogCategory::Pack, line);
            else
                LogPanel::logWarning(LogCategory::Pack, line);
        }
        if (readiness.errorCount > 0) {
            QString indexError;
            if (!ProjectPackExporter::FinalizePackOutputs(baseDir, projectLease->platforms, genPlatforms, {}, &indexError))
                LogPanel::logError(LogCategory::Pack, indexError);
            QMessageBox::critical(
                this, tr("Export Readiness"),
                tr("%1 error(s) found in the project pack configuration. "
                   "See the Log panel and fix them before exporting.")
                    .arg(readiness.errorCount));
            return;
        }
    }

    LogPanel::logInfo(LogCategory::Pack,
        packPaths.isEmpty()
            ? tr("Export started — all packs (%1)")
                  .arg(genPlatforms.join(QStringLiteral(", ")))
            : tr("Export started — %1 selected pack(s) (%2)")
                  .arg(packPaths.size())
                  .arg(genPlatforms.join(QStringLiteral(", "))));

    const auto cancel = std::make_shared<std::atomic_bool>(false);
    const quint64 runId = progressStore.beginProgress(cancel);
    if (runId == 0) return;
    const QPointer<MainWindow> window(this);
    auto* store = &progressStore;
    ProjectPackExporter::beginBackgroundExport();
    auto* thread = QThread::create(
        [window, store, projectLease, snapshot, genPlatforms, packPaths,
         exportSelection, baseDir, includeUnchecked, forceReexport, selfCleaning, cancel, runId,
         platformActivity, outputLock]() {
        struct ExportGate {
            ~ExportGate() { ProjectPackExporter::endBackgroundExport(); }
        } gate;
        int failedCount = 0;
        int builtCount = 0;
        int skippedCount = 0;
        int omittedCount = 0;
        int warningCount = 0;
        int removedCount = 0;
        ProjectPackExporter::ExportOptions opts;
        opts.snapshot = snapshot;
        opts.selection = exportSelection;
        opts.includeUnchecked = includeUnchecked;
        opts.debugStrings = true;
        opts.forceReexport = forceReexport;
        opts.selfCleaning = selfCleaning && packPaths.isEmpty();
        opts.updateSelectedOnly = !packPaths.isEmpty();
        for (const QString& unit : packPaths)
            opts.onlyPackPaths.insert(unit);
        opts.cancelCheck = [cancel] { return cancel->load(std::memory_order_relaxed); };
        opts.progressCallback = [store, projectLease, runId](const PackGenerationProgress& progress) {
            QMetaObject::invokeMethod(store, [store, projectLease, runId, progress] {
                if (ProjectManager::instance().currentProject() == projectLease.get())
                    store->updateProgress(runId, progress);
            }, Qt::QueuedConnection);
        };
        opts.observationCallback = [store, projectLease, runId](const PackBuildObservation& observation) {
            QMetaObject::invokeMethod(store, [store, projectLease, runId, observation] {
                if (ProjectManager::instance().currentProject() == projectLease.get())
                    store->updateObservation(runId, observation);
            }, Qt::QueuedConnection);
        };
        ProjectPackBuildService::Request buildRequest;
        buildRequest.outputRoot = baseDir;
        buildRequest.platforms = genPlatforms;
        buildRequest.options = opts;
        buildRequest.outputLock = outputLock;
        buildRequest.preflightFinished = [store, runId] {
            QMetaObject::invokeMethod(store, [store, runId] {
                store->finishPreflight(runId);
            }, Qt::QueuedConnection);
        };
        buildRequest.platformFinished = [&](const ProjectPackBuildService::PlatformResult& completed) {
            const auto& platform = completed.platform;
            const auto& definition = completed.definition;
            const auto& platformDir = completed.outputDir;
            const auto& records = completed.records;
            const auto& result = completed.result;
            if (result.success) removedCount += result.removedPackCount;
            for (const auto& rec : records) {
                if (rec.omitted)
                    ++omittedCount;
                else if (rec.success && rec.skipped)
                    ++skippedCount;
                else if (rec.success)
                    ++builtCount;
                else if (!result.canceled)
                    ++failedCount;
            }
            warningCount += result.warnings.size();
            if (!result.canceled && !result.success
                && std::none_of(records.begin(), records.end(), [](const auto& record) { return !record.success; }))
                ++failedCount;
            QMetaObject::invokeMethod(store,
                [window, store, projectLease, runId, platform, platformDir, definition, result, records,
                 updateSelectedOnly = opts.updateSelectedOnly] {
                if (!store->isCurrentRun(runId)
                    || ProjectManager::instance().currentProject() != projectLease.get())
                    return;
                if (result.success)
                    PackArtifactCache::instance().publish(platform, result.artifacts, updateSelectedOnly);
                if (!window) return;
                for (const auto& rec : records) {
                    window->recordPackGenerationResult(rec.packName, rec.objectID, rec.success,
                        rec.outputPath, rec.error, rec.fileSize, rec.elapsedMs, rec.platform,
                        packBuildSourceLabel(rec.source), rec.warnings.join(QStringLiteral("\n")),
                        rec.omitted ? tr("Skipped") : result.canceled && !rec.success ? tr("Canceled")
                            : rec.success && !rec.warnings.isEmpty()
                                ? (rec.skipped ? tr("Up to date with warnings") : tr("Built with warnings"))
                            : rec.success && rec.skipped ? tr("Up to date") : QString(),
                        rec.language, &rec.platformDefinition, rec.unitKey, result.canceled && !rec.success);
                }
                if (records.empty() || (!result.success
                    && std::none_of(records.begin(), records.end(), [](const auto& record) { return !record.success; }))) {
                    const auto platformJson = definition.toJson();
                    window->recordPackGenerationResult(tr("All Packs"), 0, result.success,
                        result.outputPath, result.error, result.fileSize, -1, platform,
                        {}, result.warnings.join(QStringLiteral("\n")),
                        result.canceled ? tr("Canceled") : QString(), {}, &platformJson, {}, result.canceled);
                }
                if (!result.canceled && !result.success)
                    LogPanel::logError(LogCategory::Pack,
                        result.error.isEmpty() ? tr("Pack export failed") : result.error);
                for (const QString& warning : result.warnings)
                    LogPanel::logWarning(LogCategory::Pack, warning);
                if (result.success)
                    LogPanel::logInfo(LogCategory::Pack,
                        tr("%1: exported %2 pack(s) to %3").arg(definition.name)
                            .arg(result.builtPackCount + result.skippedPackCount)
                            .arg(QDir::toNativeSeparators(platformDir)));
            }, Qt::QueuedConnection);
        };
        const auto build = ProjectPackBuildService::run(buildRequest);
        if (!build.preflightSucceeded) {
            const QString writeAccessError = build.preflightError;
            const QString writeAccessSummary = build.preflightSummary;
            QMetaObject::invokeMethod(store,
                [window, store, projectLease, runId, cancel, writeAccessError, writeAccessSummary,
                 platformActivity, outputLock] {
                if (!store->isCurrentRun(runId)) return;
                const bool canceled = cancel->load(std::memory_order_relaxed);
                store->finishProgress(runId, canceled);
                if (!window || ProjectManager::instance().currentProject() != projectLease.get()) return;
                if (canceled) {
                    LogPanel::logWarning(LogCategory::Pack, tr("Pack export canceled"));
                } else {
                    LogPanel::logError(LogCategory::Pack, writeAccessError);
                    QMessageBox::warning(window, tr("Export Readiness"),
                        tr("%1\n\nSee the Log panel for affected paths and details.").arg(writeAccessSummary));
                }
            }, Qt::QueuedConnection);
            return;
        }
        const bool canceled = build.canceled;
        QString publicationError;
        QStringList publicationWarnings;
        bool cleanedOutput = false;
        if (build.finalization == ProjectPackBuildService::Finalization::Complete) {
            const auto& publication = build.finalizationResult;
            publicationWarnings = publication.warnings;
            warningCount += publicationWarnings.size();
            if (publication.success) {
                removedCount += publication.removedPackCount;
                cleanedOutput = selfCleaning && packPaths.isEmpty();
            } else if (!canceled) {
                ++failedCount;
                publicationError = publication.error;
            }
        } else if (build.finalization == ProjectPackBuildService::Finalization::Partial
                   && !build.finalizationResult.success) {
            publicationError = build.finalizationResult.error;
            if (canceled) publicationError.clear();
            else ++failedCount;
        }
        QMetaObject::invokeMethod(store,
            [window, store, projectLease, runId, canceled, failedCount, builtCount,
             skippedCount, omittedCount, warningCount, removedCount, baseDir, cleanedOutput,
             genPlatforms, publicationError, publicationWarnings,
             platformActivity, outputLock]() {
            if (!store->isCurrentRun(runId)) return;
            if (ProjectManager::instance().currentProject() != projectLease.get()) {
                store->finishProgress(runId, true);
                return;
            }
            if (cleanedOutput)
                PackArtifactCache::instance().removeUnselectedPlatforms(baseDir, genPlatforms);
            store->finishProgress(runId, canceled);
            if (!window) return;
            for (const QString& warning : publicationWarnings)
                LogPanel::logWarning(LogCategory::Pack, warning);
            if (!publicationError.isEmpty())
                LogPanel::logError(LogCategory::Pack, publicationError);
            if (canceled) {
                LogPanel::logWarning(LogCategory::Pack, tr("Pack export canceled"));
            } else if (failedCount > 0) {
                LogPanel::logError(LogCategory::Pack,
                    tr("Export finished — %1 error(s)").arg(failedCount));
            } else {
                QString message = tr("Export finished — %1 built, %2 up to date")
                    .arg(builtCount).arg(skippedCount);
                if (omittedCount > 0) message += tr(", %1 skipped").arg(omittedCount);
                if (removedCount > 0) message += tr(", %1 removed").arg(removedCount);
                if (warningCount > 0) {
                    message += tr(", %1 warning(s)").arg(warningCount);
                    LogPanel::logWarning(LogCategory::Pack, message);
                } else {
                    LogPanel::logInfo(LogCategory::Pack, message);
                }
            }
        }, Qt::QueuedConnection);
    });
    packExportThreads_.append(thread);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread] {
        packExportThreads_.removeAll(thread);
    });
    thread->start();
}

void MainWindow::setupUndoRedo()
{
    UndoRedoManager& manager = UndoRedoManager::instance();

    connect(&manager, &UndoRedoManager::cleanChanged, this, [this](bool clean) {
        setProjectModified(!clean);
    });

    manager.clear();
}

void MainWindow::onOpenPreferences()
{
    onOpenPreferences(QString());
}

void MainWindow::onOpenPreferences(const QString& pageId)
{
    const QString type = QStringLiteral("Preferences");
    if (workspaceWindowManager_->showSingleton(type) && !pageId.isEmpty())
        qobject_cast<PreferencesPanel*>(workspaceWindowManager_->singletonContent(type))->showPage(pageId);
}

void MainWindow::onOpenProjectSettings()
{
    showProjectSettings();
}

void MainWindow::showProjectSettings(const QString& pageId)
{
    const QString type = QStringLiteral("ProjectSettings");
    if (workspaceWindowManager_->showSingleton(type) && !pageId.isEmpty())
        qobject_cast<ProjectSettingsPanel*>(workspaceWindowManager_->singletonContent(type))->showPage(pageId);
}

void MainWindow::finishProjectSettingsEditing()
{
    if (auto* panel = qobject_cast<ProjectSettingsPanel*>(
            workspaceSingletonContent(QStringLiteral("ProjectSettings"), false)))
        panel->finishEditing();
}

void MainWindow::refreshProjectSettingsAvailability()
{
    auto* dock = workspaceWindowManager_->singletonDock(QStringLiteral("ProjectSettings"));
    const bool available = ProjectManager::instance().isProjectOpen();
    dock->toggleViewAction()->setEnabled(available);
    if (!available)
        dock->toggleView(false);
}

void MainWindow::onAbout()
{
    AboutDialog dialog(this, AppSettings::instance().developerMode());
    dialog.exec();
}

SourceEditorPanel* MainWindow::openSourceEditor(uint32_t sourceId)
{
    if (sourceId == 0)
        return nullptr;

    SourceEditorPanel* editor = nullptr;
    int editorId = 0;
    for (const QPair<int, QWidget*>& instance :
         panelRegistry_->instances(SourceEditorPanel::typeId())) {
        auto* candidate = qobject_cast<SourceEditorPanel*>(instance.second);
        if (!candidate || candidate->isPinned() || candidate->sourceId() != sourceId)
            continue;
        editor = candidate;
        editorId = instance.first;
        break;
    }

    if (!editor) {
        editor = qobject_cast<SourceEditorPanel*>(
            activeStandaloneEditorPanel(SourceEditorPanel::typeId()));
        if (editor)
            editorId = editor->panelId();
    }

    if (!editor) {
        editor = qobject_cast<SourceEditorPanel*>(
            createStandaloneEditorPanel(SourceEditorPanel::typeId()));
        if (editor)
            editorId = editor->panelId();
    }

    if (!editor)
        return nullptr;

    editor->bindSource(sourceId);
    if (editorId > 0)
        panelRegistry_->setLastFocusedPanelId(SourceEditorPanel::typeId(), editorId);
    if (ads::CDockWidget* dock = dockOfPanel(editor))
        revealDock(dock);
    editor->setFocus(Qt::OtherFocusReason);
    return editor;
}

ExternalEditorPanel* MainWindow::openCurveEditor(const CurveSelection& selection)
{
    auto* project = ProjectManager::instance().currentProject();

    CurveSelection resolved = selection;
    if (selection.hasBinding()) {
        if (ProjectItem* host = ProjectManager::instance().findItemByGuid(selection.hostGuid)) {
            resolved.hostGuid = host->guid;
            if (const auto* mix = BindingEditing::ledgerHost(host, selection.address)) {
                const ParameterBindingDef* binding = !harkenstudio::IsNull(selection.bindingStableId)
                    ? mix->bindings.find(selection.bindingStableId)
                    : mix->bindings.findFirst(selection.address);
                if (binding) {
                    resolved.address = binding->address;
                    resolved.bindingStableId = binding->runtimeId();
                }
            }
        }
    } else {
        QUuid resolvedGuid = selection.attenuationGuid;
        QString resolvedName = selection.attenuationName.trimmed();
        if (const auto* attenuation = AttenuationStore::find(project, resolvedGuid, resolvedName)) {
            resolvedGuid = attenuation->hostItem()->guid;
            resolvedName = attenuation->hostItem()->name;
        }
        resolved = CurveSelection::attenuation(resolvedGuid, resolvedName, selection.dimension);
    }

    return openExternalEditor(ExternalEditorSelection::fromCurve(resolved));
}

ExternalEditorPanel* MainWindow::openExternalEditor(const ExternalEditorSelection& selection)
{
    if (selection.isEmpty()) return nullptr;
    ExternalEditorPanel* ed = nullptr;
    int editorId = 0;
    for (const auto& instance : panelRegistry_->instances(QStringLiteral("ExternalEditor"))) {
        auto* candidate = qobject_cast<ExternalEditorPanel*>(instance.second);
        if (candidate && candidate->selection().sameContext(selection)) {
            ed = candidate;
            editorId = instance.first;
            break;
        }
    }

    if (!ed) {
        ed = qobject_cast<ExternalEditorPanel*>(
            activeStandaloneEditorPanel(QStringLiteral("ExternalEditor")));
        if (ed)
            editorId = ed->panelId();
    }

    if (!ed) {
        ed = qobject_cast<ExternalEditorPanel*>(
            createStandaloneEditorPanel(QStringLiteral("ExternalEditor")));
        if (ed)
            editorId = ed->panelId();
    }

    if (!ed)
        return nullptr;

    ed->bindSelection(selection);

    if (editorId > 0)
        panelRegistry_->setLastFocusedPanelId(QStringLiteral("ExternalEditor"), editorId);
    if (ads::CDockWidget* d = dockOfPanel(ed))
        revealDock(d);
    ed->setFocus(Qt::OtherFocusReason);
    return ed;
}

void MainWindow::onResetLayout()
{
    resetCurrentLayout();
}

bool MainWindow::resetCurrentLayout()
{
    const bool ok = layoutController_->resetCurrentLayout();
    if (!ok)
        return false;
    if (layoutController_->hasNamedLayout())
        setStatusMessage(tr("Layout '%1' reloaded").arg(layoutController_->currentNamedLayout()));
    else
        setStatusMessage(tr("Layout reset to default"));
    return true;
}


SourceEditorPanel* MainWindow::focusedSourceEditorPanel() const
{
    if (SourceEditorPanel* panel = SourceEditorPanel::panelContaining(QApplication::focusWidget()))
        return panel;
    QWidget* focus = QApplication::focusWidget();
    if (!focus || !panelRegistry_)
        return nullptr;
    for (QWidget* widget : panelRegistry_->allPanels(SourceEditorPanel::typeId())) {
        auto* panel = qobject_cast<SourceEditorPanel*>(widget);
        if (!panel)
            continue;
        if (ads::CDockWidget* dock = dockOfPanel(panel)) {
            if (dock == focus || dock->isAncestorOf(focus))
                return panel;
        }
    }
    return nullptr;
}

void MainWindow::onPlayPressed()
{
    if (SourceEditorPanel* editor = focusedSourceEditorPanel()) {
        const bool wasPlaying = editor->isPreviewPlayheadActive();
        if (editor->handlePlayStopShortcut()) {
            setStatusMessage(wasPlaying
                                      ? tr("Stopped")
                                      : tr("Playing: %1").arg(editor->previewObjectName()));
            return;
        }
        setStatusMessage(editor->sourceId() == 0
                                  ? tr("No audio source selected")
                                  : tr("Cannot play audio source"));
        return;
    }

    if (auditionManager_->hasActiveNonRawAudition())
    {
        onStopPressed();
        return;
    }
    onPlayRequested();
}

void MainWindow::onPlayRequested()
{
    AudioEngine& engine = AudioEngine::instance();

    if (engine.isSimulationMode())
    {
        setStatusMessage(tr("Cannot play: audio runtime unavailable (simulation mode)"));
        if (auto* log = logPanel())
            log->logWarning(LogCategory::Audio, tr("Playback disabled in simulation mode"));
        return;
    }

    ProjectItem* selectedItem = auditionTarget();
    if (!selectedItem)
    {
        setStatusMessage(tr("No audition target selected"));
        return;
    }

    if (selectedItem->type == ProjectItem::Type::Container) {
        ProjectData* project = ProjectManager::instance().currentProject();
        if (project == nullptr
            || !isCurrentWorldPlayable(*project, *selectedItem)) {
            const QString message =
                tr("Cannot play '%1': the Container is empty.")
                    .arg(selectedItem->name);
            setStatusMessage(message);
            if (auto* log = logPanel())
                log->logInfo(LogCategory::Audio, message);
            return;
        }
    }

    if (ItemTypeCatalog::isTriggerable(selectedItem->type)
        && ItemTypeCatalog::isUserFacingType(selectedItem->type))
    {
        uint32_t id = auditionManager_->playItem(*selectedItem);
        if (id != 0)
        {
            const auto audition = auditionManager_->getAudition(id);
            if (!audition.bIsPlaying) {
                setStatusMessage(
                    tr("Waiting for Runtime synchronization: %1")
                        .arg(selectedItem->name));
                if (auto* log = logPanel())
                    log->logInfo(
                        LogCategory::Audio,
                        tr("Preview queued while Runtime synchronizes: %1")
                            .arg(selectedItem->name));
            } else {
                setStatusMessage(
                    tr("Playing: %1").arg(selectedItem->name));
                if (auto* log = logPanel())
                    log->logInfo(
                        LogCategory::Audio,
                        tr("Play triggered: %1").arg(selectedItem->name));
            }
        }
        else
        {
            using HarkenEditor::Bridge::PlaybackFailure;
            const PlaybackFailure failure =
                auditionManager_->lastPlayFailure();
            QString message;
            if (failure == PlaybackFailure::TopologySyncPending) {
                message = tr("Bus topology is still synchronizing; preview is not ready.");
            } else if (failure == PlaybackFailure::TopologySyncFailed) {
                const auto detail =
                    auditionManager_->lastPlayTopologyFailure();
                message = tr("Bus topology failed for '%1' (engine result %2).")
                              .arg(detail.busName.isEmpty()
                                       ? objectIdString(detail.busId)
                                       : detail.busName)
                              .arg(static_cast<int32_t>(detail.result));
            } else if (failure == PlaybackFailure::RuntimeRefused) {
                message = tr("Playback was refused for '%1' (engine result %2).")
                              .arg(selectedItem->name)
                              .arg(static_cast<int32_t>(
                                  auditionManager_->lastPlayEngineResult()));
            } else if (failure == PlaybackFailure::NotPlayable) {
                message = auditionManager_->lastPlayReason() == QLatin1String("empty")
                    ? tr("Cannot play '%1': the Action has no playable step.")
                          .arg(selectedItem->name)
                    : tr("Cannot play '%1': it is not in the current preview world.")
                          .arg(selectedItem->name);
            } else {
                message = tr("No voice produced: %1").arg(selectedItem->name);
            }
            setStatusMessage(message);
            if (auto* log = logPanel()) {
                if (failure == PlaybackFailure::NotPlayable) {
                    log->logInfo(LogCategory::Audio, message);
                } else if (failure == PlaybackFailure::NoVoiceProduced ||
                    failure == PlaybackFailure::None) {
                    log->logInfo(
                        LogCategory::Audio,
                        tr("Action produced no voice: %1 (empty or unresolved "
                           "step target, target-object instance limit, or queued "
                           "with delay)").arg(selectedItem->name));
                } else {
                    log->logError(LogCategory::Audio, message);
                }
            }
        }
    }
    else
    {
        setStatusMessage(tr("Selected item is not playable"));
    }
}

void MainWindow::onStopPressed()
{
    auditionManager_->stopAll(100);
    setStatusMessage(tr("Stopped"));
}

ProjectItem* MainWindow::auditionTarget() const
{
    return ProjectManager::instance().findItemByGuid(auditionTargetGuid_);
}

void MainWindow::setAuditionTarget(ProjectItem* item)
{
    auditionTargetGuid_ = item ? item->guid : QUuid();
    if (playbackControls_)
        playbackControls_->setAuditionTarget(item);
    refreshPlaybackResetEnabled();
    if (controlPanel_)
        controlPanel_->setItem(item);
}

void MainWindow::refreshPlaybackResetEnabled()
{
    if (!playbackControls_)
        return;
    const auto* item = auditionTarget();
    const auto* selection = item ? item->comp<VSelection>() : nullptr;
    const bool enabled = item
        && item->type == ProjectItem::Type::Container
        && selection
        && selection->mode != SelectionMode::Match
        && selection->mode != SelectionMode::Blend
        && !harkenstudio::IsNull(item->runtimeId());
    playbackControls_->setResetEnabled(enabled);
}

void MainWindow::onResetPressed()
{
    ProjectItem* selectedItem = auditionTarget();
    refreshPlaybackResetEnabled();
    if (!playbackControls_ || !playbackControls_->isResetEnabled() || !selectedItem)
        return;
    const ActionResult result = ActionManager::instance().execute(
        QStringLiteral("audition_reset"),
        {{QStringLiteral("authoring_handle"),
          static_cast<qint64>(selectedItem->objectID)}},
        ActionSource::GUI, this);
    if (result.success) {
        setStatusMessage(tr("Reset: %1").arg(selectedItem->name));
        return;
    }
    const QString message = result.errorMessage.isEmpty()
        ? tr("Reset failed: %1").arg(selectedItem->name)
        : result.errorMessage;
    setStatusMessage(message);
}

void MainWindow::onAudioError(const QString& error)
{
    setStatusMessage(tr("Audio Error: %1").arg(error), LogEntry::Error);
    QMessageBox::warning(this, tr("Audio Error"), error);
}

void MainWindow::onSaveProgress(int percent, const QString& message)
{
    setStatusMessage(QString("[%1%] %2").arg(percent).arg(message));

    if (progressBar_) {
        if (percent <= 0) {
            progressBar_->setValue(0);
            progressBar_->show();
        } else if (percent >= 100) {
            progressBar_->setValue(100);
            QTimer::singleShot(1000, this, [this]() {
                if (progressBar_) progressBar_->hide();
            });
        } else {
            progressBar_->setValue(percent);
            if (!progressBar_->isVisible())
                progressBar_->show();
        }
    }
}

void MainWindow::addRecentProject(const QString& path)
{
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                       QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio"));
    QStringList recent = settings.value("RecentProjects").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 10)
        recent.removeLast();
    settings.setValue("RecentProjects", recent);
}

void MainWindow::syncWindowToOpenedProject(const QString& path)
{
    currentProjectPath_ = path;

    if (ProjectData* project = ProjectManager::instance().currentProject())
        projectExplorer_->newProject(project->name);

    UndoRedoManager::instance().clearHistory();

    setProjectModified(false);
    updateWindowTitle();
    addRecentProject(path);

    loadProjectLayout();

    setStatusMessage(tr("Opened: %1").arg(QFileInfo(path).baseName()));
    LogPanel::logInfo(LogCategory::File, tr("Project opened: %1").arg(path));
}

void MainWindow::finishPreparedProject(
    const QString& path,
    bool created,
    std::function<void(StartupPhase)> startupProgress)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    currentProjectPath_ = project ? project->projectPath : path;
    projectExplorer_->newProject(project ? project->name
                                         : QFileInfo(path).completeBaseName());
    UndoRedoManager::instance().clearHistory();
    setProjectModified(false);
    updateWindowTitle();
    addRecentProject(currentProjectPath_);
    loadProjectLayout();
    if (startupProgress)
        startupProgress(StartupPhase::RestoringWorkspace);
    setStatusMessage(created
                              ? tr("New project created: %1").arg(currentProjectPath_)
                              : tr("Opened: %1").arg(QFileInfo(path).baseName()));
}

bool MainWindow::loadProject(
    const QString& path,
    std::function<void(StartupPhase)> startupProgress,
    QString* error,
    bool* forwarded)
{
    if (error)
        error->clear();
    if (forwarded)
        *forwarded = false;
    if (PackGenerationRecordStore::instance().isExporting()) {
        if (error)
            *error = tr("The project cannot change while packs are being exported.");
        return false;
    }
    if (projectOpenInProgress_) {
        if (error)
            *error = tr("Another project is already being opened. Please try again when it finishes.");
        return false;
    }
    // Nested event loops must not let another open replace this request's result.
    QScopedValueRollback<bool> openGuard(projectOpenInProgress_, true);
    auto acquisition = ProjectInstance::acquire(path);
    if (acquisition.status != ProjectInstance::Status::Acquired) {
        if (forwarded)
            *forwarded = acquisition.status == ProjectInstance::Status::Forwarded;
        if (error)
            *error = acquisition.error;
        if (acquisition.status == ProjectInstance::Status::Failed)
            QMessageBox::warning(this, tr("Open Project"), acquisition.error);
        return false;
    }
    ProjectData* existingProject = ProjectManager::instance().currentProject();
    if (existingProject
        && ProjectInstance::identityForProject(existingProject->projectPath)
            == ProjectInstance::identityForProject(path)) {
        if (currentProjectPath_ != existingProject->projectPath)
            syncWindowToOpenedProject(existingProject->projectPath);
        ProjectInstance::activateWindow();
        return true;
    }

    if (!maybeSave())
        return false;

    saveProjectLayout();

    const auto showOpenFailure = [this, error](const QString& internalError) {
        if (error)
            *error = internalError;
        const ProjectUpgradeMessages::Presentation message =
            ProjectUpgradeMessages::presentation(internalError, true);
        LogPanel::logError(LogCategory::File, message.primaryText());
        QMessageBox box(QMessageBox::Critical, tr("Open Project"),
                        message.primaryText(), QMessageBox::Ok, this);
        if (!message.technicalDetails.isEmpty())
            box.setDetailedText(message.technicalDetails);
        box.exec();
    };

    QProgressDialog analysisProgress(
        tr("Checking the project..."), QString(), 0, 100, this);
    analysisProgress.setWindowTitle(tr("Open Project"));
    analysisProgress.setWindowModality(Qt::WindowModal);
    analysisProgress.setMinimumDuration(250);
    analysisProgress.setAutoClose(false);
    analysisProgress.setAutoReset(false);
    QFutureWatcher<ProjectUpgradeAnalysis> analysisWatcher;
    QEventLoop analysisLoop;
    connect(&analysisWatcher,
            &QFutureWatcher<ProjectUpgradeAnalysis>::finished,
            &analysisLoop, &QEventLoop::quit);
    const QPointer<QProgressDialog> analysisGuard(&analysisProgress);
    const ProjectUpgradeProgressCallback analysisCallback =
        [analysisGuard](const ProjectUpgradeProgress& value) {
            if (!analysisGuard)
                return;
            QMetaObject::invokeMethod(
                analysisGuard,
                [analysisGuard, value]() {
                    if (!analysisGuard)
                        return;
                    analysisGuard->setLabelText(
                        ProjectUpgradeMessages::progressText(value.phase)
                        + QStringLiteral("..."));
                    analysisGuard->setValue(qMax(
                        analysisGuard->value(),
                        ProjectUpgradeMessages::progressPercent(value)));
                },
                Qt::QueuedConnection);
        };
    analysisWatcher.setFuture(QtConcurrent::run(
        [path, analysisCallback]() {
            return ProjectUpgradeManager().analyze(path, analysisCallback);
        }));
    analysisLoop.exec();
    analysisProgress.hide();
    const ProjectUpgradeAnalysis analysis = analysisWatcher.result();
    if (!analysis.ok) {
        showOpenFailure(analysis.blockingErrors.isEmpty()
                            ? tr("Project analysis failed")
                            : analysis.blockingErrors.first());
        return false;
    }
    const ProjectOpenDecision decision = confirmProjectOpen(analysis, this);
    if (decision.cancelled)
        return false;
    if (!decision.proceed) {
        showOpenFailure(projectIssueCode(decision.issue));
        return false;
    }
    const ProjectUpgradeApprovalCallback approval = decision.upgradeApproval;
    const bool allowUpgrade = static_cast<bool>(approval);

    QProgressDialog projectProgress(
        allowUpgrade ? tr("Updating the project...") : tr("Opening the project..."),
        QString(), 0, allowUpgrade ? 100 : 0, this);
    projectProgress.setWindowTitle(
        allowUpgrade ? tr("Project Update") : tr("Open Project"));
    projectProgress.setWindowModality(Qt::WindowModal);
    projectProgress.setMinimumDuration(allowUpgrade ? 0 : 250);
    projectProgress.setAutoClose(false);
    projectProgress.setAutoReset(false);
    QFutureWatcher<std::shared_ptr<PreparedProject>> preparationWatcher;
    QEventLoop preparationLoop;
    connect(&preparationWatcher,
            &QFutureWatcher<std::shared_ptr<PreparedProject>>::finished,
            &preparationLoop, &QEventLoop::quit);
    const QPointer<QProgressDialog> projectProgressGuard(&projectProgress);
    const ProjectUpgradeProgressCallback upgradeCallback =
        allowUpgrade
        ? ProjectUpgradeProgressCallback(
            [projectProgressGuard](const ProjectUpgradeProgress& value) {
                if (!projectProgressGuard)
                    return;
                QMetaObject::invokeMethod(
                    projectProgressGuard,
                    [projectProgressGuard, value]() {
                        if (!projectProgressGuard)
                            return;
                        projectProgressGuard->setLabelText(
                            ProjectUpgradeMessages::progressText(value.phase)
                            + QStringLiteral("..."));
                        projectProgressGuard->setValue(qMax(
                            projectProgressGuard->value(),
                            ProjectUpgradeMessages::progressPercent(value)));
                    },
                    Qt::QueuedConnection);
            })
        : ProjectUpgradeProgressCallback();
    preparationWatcher.setFuture(QtConcurrent::run(
        [path, allowUpgrade, upgradeCallback, approval]() {
            return std::make_shared<PreparedProject>(
                ProjectManager::prepareProject(path, allowUpgrade,
                                               upgradeCallback, approval));
        }));
    projectProgress.show();
    preparationLoop.exec();
    std::shared_ptr<PreparedProject> prepared = preparationWatcher.result();
    if (prepared)
        prepared->instance = std::move(acquisition.instance);
    auto& projectManager = ProjectManager::instance();
    QEventLoop commitLoop;
    connect(&AudioEngine::instance().runtimeSyncCoordinator(),
            &RuntimeSyncCoordinator::barrierChanged, &commitLoop,
            [&]() {
                if (!projectManager.isClosing())
                    commitLoop.quit();
            }, Qt::QueuedConnection);
    if (!prepared || !prepared->project
        || !projectManager.commitPreparedProject(
            std::move(*prepared))) {
        projectProgress.hide();
        const QString error = prepared && !prepared->error.isEmpty()
            ? prepared->error
            : QStringLiteral("Project preparation failed");
        showOpenFailure(error);
        return false;
    }
    // A queued replacement is committed only after the old world's close barrier.
    // Keep the open operation pending until that lifecycle transition completes.
    const bool queued = projectManager.isClosing();
    if (queued) {
        projectProgress.setLabelText(tr("Closing the previous project..."));
        projectProgress.setRange(0, 0);
        projectProgress.setCancelButtonText(tr("Cancel"));
        connect(&projectProgress, &QProgressDialog::canceled,
                &commitLoop, &QEventLoop::quit);
        QTimer commitTimeout;
        commitTimeout.setSingleShot(true);
        commitTimeout.setTimerType(Qt::PreciseTimer);
        connect(&commitTimeout, &QTimer::timeout,
                &commitLoop, &QEventLoop::quit);
        commitTimeout.start(10000);
        commitLoop.exec();
        commitTimeout.stop();
        if (projectManager.isClosing()) {
            projectManager.cancelQueuedPreparedProject(analysis.projectFilePath);
            projectProgress.hide();
            if (projectProgress.wasCanceled()) {
                if (error)
                    *error = tr("Project opening was canceled.");
            } else {
                showOpenFailure(QStringLiteral(
                    "Timed out waiting for the previous project's Runtime close barrier"));
            }
            return false;
        }
    }
    projectProgress.hide();
    if (!queued) {
        projectManager.startWatchingCurrentProject();
        projectManager.publishProjectOpened();
    }

    ProjectData* project = projectManager.currentProject();
    if (project && project->projectPath == analysis.projectFilePath) {
        syncWindowToOpenedProject(project->projectPath);
        if (startupProgress)
            startupProgress(StartupPhase::RestoringWorkspace);
        return true;
    }

    LogPanel::logError(
        LogCategory::File,
        tr("Harken Studio could not open this project. Your project was not changed."));
    return false;
}

bool MainWindow::switchToView(const QString& viewName)
{
    if (!layoutController_)
        return false;
    if (const BuiltinLayouts::Entry* builtin = BuiltinLayouts::findByToken(viewName)) {
        if (layoutController_->selectBuiltinLayout(builtin->id))
            return true;
    }

    qWarning() << "[MainWindow] switchToView: unknown layout" << viewName
               << "- valid layouts:" << BuiltinLayouts::ids().join(QLatin1Char(' '));
    return false;
}

QString MainWindow::currentView() const
{
    if (layoutController_ && layoutController_->hasNamedLayout())
        return layoutController_->currentNamedLayout();
    return QStringLiteral("default_layout");
}

bool MainWindow::switchToLayout(const QString& layoutName)
{
    return switchToView(layoutName);
}

QString MainWindow::currentLayoutName() const
{
    return currentView();
}

QString MainWindow::getRecentLogs(int count, const QString& levelFilter) const
{
    auto* log = logPanel();
    if (!log || !log->model())
        return "[Log panel not available]";
    
    LogEntry::Level minLevel = LogEntry::Trace;
    if (!levelFilter.isEmpty()) {
        QString filter = levelFilter.toUpper();
        if (filter == "ERROR") minLevel = LogEntry::Error;
        else if (filter == "WARNING") minLevel = LogEntry::Warning;
        else if (filter == "INFO") minLevel = LogEntry::Info;
        else if (filter == "DEBUG") minLevel = LogEntry::Debug;
    }
    
    auto entries = log->model()->entries(minLevel);
    
    QStringList lines;
    int start = std::max(0, static_cast<int>(entries.size()) - count);
    for (int i = start; i < static_cast<int>(entries.size()); ++i) {
        lines.append(entries[i].formatted());
    }
    
    return lines.join("\n");
}

void MainWindow::selectBus(uint32_t busID, bool reveal)
{
    if (auto* item = ProjectManager::instance().findBusItem(busID))
        selectionRouter_->selectItem(item, reveal);
}

void MainWindow::loadBusSignalChain(uint32_t busID)
{
    busController_->loadBusSignalChain(busID);
}

void MainWindow::syncBusSelectionViews(uint32_t busID)
{
    busController_->selectBus(busID);
}

}
