#include "Core/StandaloneEditorDispatch.h"
#include "Widgets/Inspector/EditorLaunchMap.h"

#include "Components/VSelection.h"
#include "Components/VMixNode.h"
#include "Components/VPiece.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "AssetPipeline/AssetRegistry.h"
#include "Core/ActionManager.h"
#include "Core/AuditionManager.h"
#include "Core/CurrentWorldProjectAdapter.h"
#include "Core/IPC/IPCServer.h"
#include "Core/LayoutController.h"
#include "Core/PanelInstanceRegistry.h"
#include "Core/SelectionRouter.h"
#include "Core/ShortcutManager.h"
#include "Core/ToastManager.h"
#include "Core/UndoCommands.h"
#include "MainWindow.h"
#include "Monitor/EventCaptureController.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/Blueprint/BlueprintPanel.h"
#include "Widgets/Meter/MeterPanel.h"
#include "Widgets/ExternalEditor/ExternalEditorPanel.h"
#include "Widgets/GridEditor/GridEditorPanel.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/ProjectExplorer/ProjectExplorer.h"
#include "Widgets/PlatformBar/PlaybackControls.h"
#include "Widgets/PreviewPanel/PreviewPanel.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Widgets/TransitionEditor/TransitionEditorPanel.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <DockSplitter.h>

#include <array>
#include <tuple>

#include <QApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <QAbstractSocket>
#include <QComboBox>
#include <QGraphicsScene>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTcpSocket>
#include <QToolButton>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class StandaloneEditorDispatchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("StandaloneDispatchProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(pm.createProject(config));

        attenuation_ = pm.createItem(nullptr, QStringLiteral("Attenuation"),
                                     VNX::ProjectItem::Type::Attenuation);
        sound_ = pm.createItem(nullptr, QStringLiteral("Sound"),
                               VNX::ProjectItem::Type::Sound);
        container_ = pm.createItem(nullptr, QStringLiteral("Container"),
                                   VNX::ProjectItem::Type::Container);
        auto* owner = pm.createItem(nullptr, QStringLiteral("TrackPiece"), VNX::ProjectItem::Type::Piece);
        track_ = pm.createItem(owner, QStringLiteral("Track"),
                               VNX::ProjectItem::Type::Track);
        piece_ = pm.createItem(nullptr, QStringLiteral("Piece"),
                               VNX::ProjectItem::Type::Piece);
        ASSERT_NE(attenuation_, nullptr);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(track_, nullptr);
        ASSERT_NE(piece_, nullptr);
        setMode(VNX::SelectionMode::Match);
    }

    void TearDown() override
    {
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
    }

    void setMode(VNX::SelectionMode mode,
                 VNX::SelectionPlayType playType = VNX::SelectionPlayType::Single)
    {
        container_->compOrCreate<VNX::VSelection>()->mode = mode;
        container_->comp<VNX::VSelection>()->playType = playType;
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* attenuation_ = nullptr;
    VNX::ProjectItem* sound_ = nullptr;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* track_ = nullptr;
    VNX::ProjectItem* piece_ = nullptr;
};

class EditorDockLayoutTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        ASSERT_TRUE(directory_.isValid());
        window_ = std::make_unique<VNX::MainWindow>();
        VNX::ProjectConfig config;
        config.name = QStringLiteral("EditorDockLayout");
        config.location = directory_.path();
        ASSERT_TRUE(pm.createProject(config));
        controller_ = window_->findChild<VNX::LayoutController*>();
        ASSERT_NE(controller_, nullptr);
        controller_->loadProjectLayout();
        window_->resize(1800, 1000);
        window_->show();
        drainEvents();
        auto* owner = pm.createItem(nullptr, QStringLiteral("TrackPiece"), VNX::ProjectItem::Type::Piece);
        track_ = pm.createItem(owner, QStringLiteral("Track"), VNX::ProjectItem::Type::Track);
        piece_ = pm.createItem(nullptr, QStringLiteral("Piece"), VNX::ProjectItem::Type::Piece);
        ASSERT_NE(track_, nullptr);
        ASSERT_NE(piece_, nullptr);
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        if (window_)
            QObject::disconnect(&VNX::UndoRedoManager::instance(), nullptr, window_.get(), nullptr);
        VNX::ToastManager::instance().setParentWidget(nullptr);
        window_.reset();
        VNX::ActionManager::instance().setTransactionCallbacks({});
        VNX::ActionManager::instance().clearAllForTesting();
        drainEvents();
    }

    static void drainEvents()
    {
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    }

    ads::CDockWidget* dock(const QString& name) const
    {
        return window_->findChild<ads::CDockManager*>()->findDockWidget(name);
    }

    ads::CDockWidget* addContent(const QString& name, ads::DockWidgetArea edge,
                                ads::CDockWidget* anchor)
    {
        auto* content = new ads::CDockWidget(name);
        content->setObjectName(name);
        content->setWidget(new QWidget());
        content->setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, true);
        window_->findChild<ads::CDockManager*>()->addDockWidget(
            edge, content, anchor ? anchor->dockAreaWidget() : nullptr);
        content->toggleView(true);
        drainEvents();
        return content;
    }

    QRect areaRect(ads::CDockWidget* content) const
    {
        auto* area = content->dockAreaWidget();
        return {area->mapTo(window_.get(), QPoint()), area->size()};
    }

    static QWidget* splitChildContaining(QWidget* content, ads::CDockSplitter* splitter)
    {
        while (content && content->parentWidget() != splitter)
            content = content->parentWidget();
        return content;
    }

    void expectThirdOf(ads::CDockWidget* content, const QRect& original) const
    {
        auto* splitter = content->dockAreaWidget()->parentSplitter();
        const bool horizontal = splitter->orientation() == Qt::Horizontal;
        const int extent = horizontal ? areaRect(content).width() : areaRect(content).height();
        const int available = horizontal ? original.width() : original.height();
        EXPECT_NEAR(extent, (available - splitter->handleWidth()) / 3.0, 2.0);
    }

    VNX::TimelineEditorPanel* launchTimeline(VNX::ProjectItem* item)
    {
        window_->openOrFocusInspectorForItem(item);
        drainEvents();
        auto* inspector = window_->activeInspectorPanel();
        EXPECT_NE(inspector, nullptr);
        if (!inspector) return nullptr;
        inspector->requestEditorLaunch(QStringLiteral("timeline"));
        drainEvents();
        return qobject_cast<VNX::TimelineEditorPanel*>(
            window_->activeStandaloneEditorPanel(QStringLiteral("TimelineEditor")));
    }

    void setExtent(ads::CDockWidget* target, int extent)
    {
        auto* area = target->dockAreaWidget();
        auto* splitter = area->parentSplitter();
        QWidget* center = dock(QStringLiteral("BlueprintCanvasDock"))->dockAreaWidget();
        while (center && center->parentWidget() != splitter)
            center = center->parentWidget();
        ASSERT_NE(center, nullptr);
        const int targetIndex = splitter->indexOf(area);
        const int centerIndex = splitter->indexOf(center);
        ASSERT_NE(targetIndex, centerIndex);
        QList<int> sizes = splitter->sizes();
        sizes[centerIndex] += sizes[targetIndex] - extent;
        sizes[targetIndex] = extent;
        splitter->setSizes(sizes);
        drainEvents();
    }

    QTemporaryDir directory_;
    std::unique_ptr<VNX::MainWindow> window_;
    VNX::LayoutController* controller_ = nullptr;
    VNX::ProjectItem* track_ = nullptr;
    VNX::ProjectItem* piece_ = nullptr;
};

class ObjectRefStudioNavigationTest : public EditorDockLayoutTest,
    public ::testing::WithParamInterface<std::tuple<VNX::RefDomain, bool, bool, bool, bool>>
{
};

} // namespace

TEST_P(ObjectRefStudioNavigationTest, LocatesOnClickAndOpensOnDoubleClickWithoutPanelWiring)
{
    const auto [domain, floating, explorerOpen, hierarchyOpen, blueprintOpen] = GetParam();
    VNX::AssetInfo asset;
    asset.sourceID = piece_->objectID;
    asset.relativePath = QStringLiteral("reference.wav");
    asset.metadata.originalFormat = QStringLiteral("WAV");
    asset.metadata.sampleRate = 48000;
    asset.metadata.channels = 2;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));

    auto* containerDock = addContent(QStringLiteral("ObjectReferenceContainer"),
        ads::RightDockWidgetArea, dock(QStringLiteral("BlueprintCanvasDock")));
    auto* layout = new QVBoxLayout(containerDock->widget());
    auto* nestedContainer = new QWidget(containerDock->widget());
    layout->addWidget(nestedContainer);
    auto* nestedLayout = new QVBoxLayout(nestedContainer);
    auto* reference = new VNX::ObjectRefWidget(nestedContainer);
    reference->setRefDomain(domain);
    reference->setCurrentObjectID(piece_->objectID);
    reference->setReadOnly(true);
    reference->setCompact(true);
    nestedLayout->addWidget(reference);
    if (floating)
        containerDock->setFloating();
    window_->openOrFocusInspectorForItem(track_);
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    auto* explorer = window_->findChild<VNX::ProjectExplorer*>();
    auto* explorerDock = dock(QStringLiteral("ExplorerDock"));
    auto* blueprintDock = dock(QStringLiteral("BlueprintCanvasDock"));
    auto* canvas = window_->findChild<VNX::BlueprintCanvasWidget*>();
    auto* hierarchy = window_->createHierarchyPanel();
    ASSERT_NE(router, nullptr);
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(explorerDock, nullptr);
    ASSERT_NE(blueprintDock, nullptr);
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(hierarchy, nullptr);
    auto* hierarchyDock = dock(QStringLiteral("HierarchyDock_%1").arg(hierarchy->panelId()));
    ASSERT_NE(hierarchyDock, nullptr);
    router->onProjectItemSelected(track_);
    explorerDock->toggleView(explorerOpen);
    hierarchyDock->toggleView(hierarchyOpen);
    blueprintDock->toggleView(blueprintOpen);
    drainEvents();
    const auto canvasTransform = canvas->transform();
    const auto canvasOrigin = canvas->mapToScene(QPoint());
    const auto canvasSelection = canvas->selectedProjectItems();
    auto* blueprintPing = canvas->findChild<QVariantAnimation*>(QStringLiteral("blueprintLocationPing"));
    ASSERT_NE(blueprintPing, nullptr);
    ASSERT_EQ(containerDock->isInFloatingContainer(), floating);
    const auto inspectorCount = window_->allInspectorPanels().size();
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    int changes = 0;
    QObject::connect(reference, &VNX::ObjectRefWidget::objectChanged,
                     reference, [&] { ++changes; });
    const QPoint position(8, reference->height() / 2);
    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent click(type, position, reference->mapToGlobal(position), Qt::LeftButton,
            type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(reference, &click);
    }
    auto* tree = domain == VNX::RefDomain::ProjectObject
        ? explorer->treeWidget() : explorer->sourcesTree();
    EXPECT_EQ(tree->viewport()->findChild<QWidget*>(QStringLiteral("treeRowPing")) != nullptr,
              explorerOpen);
    EXPECT_EQ(hierarchy->treeWidget()->viewport()->findChild<QWidget*>(QStringLiteral("treeRowPing")) != nullptr,
              hierarchyOpen && domain == VNX::RefDomain::ProjectObject);
    EXPECT_EQ(blueprintPing->state() == QAbstractAnimation::Running,
              blueprintOpen && domain == VNX::RefDomain::ProjectObject);
    drainEvents();
    EXPECT_EQ(explorerDock->isClosed(), !explorerOpen);
    EXPECT_EQ(hierarchyDock->isClosed(), !hierarchyOpen);
    EXPECT_EQ(blueprintDock->isClosed(), !blueprintOpen);
    EXPECT_EQ(canvas->transform(), canvasTransform);
    EXPECT_EQ(canvas->mapToScene(QPoint()), canvasOrigin);
    EXPECT_EQ(canvas->selectedProjectItems(), canvasSelection);
    EXPECT_EQ(explorer->selectedItem(), track_);
    EXPECT_EQ(window_->allInspectorPanels().size(), inspectorCount);
    ASSERT_NE(window_->activeInspectorPanel(), nullptr);
    EXPECT_EQ(window_->activeInspectorPanel()->currentContextItem(), track_);
    EXPECT_EQ(window_->activeStandaloneEditorPanel(QStringLiteral("TimelineEditor")), nullptr);
    EXPECT_EQ(window_->activeStandaloneEditorPanel(QStringLiteral("SourceEditor")), nullptr);

    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, position, reference->mapToGlobal(position),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(reference, &doubleClick);
    drainEvents();
    EXPECT_EQ(explorerDock->isClosed(), !explorerOpen);
    EXPECT_EQ(hierarchyDock->isClosed(), !hierarchyOpen);
    EXPECT_EQ(blueprintDock->isClosed(), !blueprintOpen);
    if (domain == VNX::RefDomain::ProjectObject) {
        ASSERT_NE(window_->activeInspectorPanel(), nullptr);
        EXPECT_EQ(window_->activeInspectorPanel()->currentContextItem(), piece_);
        auto* editor = qobject_cast<VNX::TimelineEditorPanel*>(
            window_->activeStandaloneEditorPanel(QStringLiteral("TimelineEditor")));
        ASSERT_NE(editor, nullptr);
        EXPECT_EQ(editor->boundItem(), piece_);
        EXPECT_EQ(window_->activeStandaloneEditorPanel(QStringLiteral("SourceEditor")), nullptr);
    } else {
        EXPECT_EQ(window_->activeStandaloneEditorPanel(QStringLiteral("SourceEditor")), nullptr);
        EXPECT_EQ(window_->activeInspectorPanel()->currentContextItem(), track_);
        EXPECT_EQ(window_->activeStandaloneEditorPanel(QStringLiteral("TimelineEditor")), nullptr);
    }
    EXPECT_EQ(reference->currentObjectID(), piece_->objectID);
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
}

INSTANTIATE_TEST_SUITE_P(Containers, ObjectRefStudioNavigationTest,
    ::testing::Values(
        std::make_tuple(VNX::RefDomain::ProjectObject, false, true, true, true),
        std::make_tuple(VNX::RefDomain::ProjectObject, true, true, true, true),
        std::make_tuple(VNX::RefDomain::AudioAsset, false, true, true, true),
        std::make_tuple(VNX::RefDomain::AudioAsset, true, true, true, true),
        std::make_tuple(VNX::RefDomain::ProjectObject, false, false, false, false),
        std::make_tuple(VNX::RefDomain::AudioAsset, true, false, false, false),
        std::make_tuple(VNX::RefDomain::ProjectObject, true, true, false, false),
        std::make_tuple(VNX::RefDomain::ProjectObject, false, false, true, false),
        std::make_tuple(VNX::RefDomain::ProjectObject, false, false, false, true),
        std::make_tuple(VNX::RefDomain::AudioAsset, false, false, true, true)));

TEST_F(EditorDockLayoutTest, PlaybackControlsFollowSelectionWithPreviewHiddenOrFloating)
{
    auto* preview = dock(QStringLiteral("PreviewDock"));
    auto* toolbar = window_->findChild<QToolBar*>(QStringLiteral("platformBar"));
    auto* controls = window_->findChild<VNX::PlaybackControls*>();
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    auto* status = window_->findChild<QLabel*>(QStringLiteral("statusLabel"));
    auto* audition = window_->findChild<HarkenEditor::AuditionManager*>();
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(controls, nullptr);
    ASSERT_NE(router, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(audition, nullptr);
    ASSERT_NE(qobject_cast<PreviewPanel*>(preview->widget()), nullptr);
    EXPECT_FALSE(preview->isAncestorOf(controls));
    EXPECT_TRUE(toolbar->isAncestorOf(controls));
    auto* play = controls->findChild<QToolButton*>(QStringLiteral("playBtn"));
    auto* stop = controls->findChild<QToolButton*>(QStringLiteral("stopBtn"));
    auto* reset = controls->findChild<QToolButton*>(QStringLiteral("resetBtn"));
    ASSERT_NE(play, nullptr);
    ASSERT_NE(stop, nullptr);
    ASSERT_NE(reset, nullptr);
    QAction* playShortcut = nullptr;
    for (auto* action : VNX::ShortcutManager::instance().info(VNX::Shortcuts::PlayStop).actions)
        if (action->parent() == window_.get()) playShortcut = action;
    ASSERT_NE(playShortcut, nullptr);
    EXPECT_EQ(playShortcut->shortcutContext(), Qt::ApplicationShortcut);

    auto& manager = VNX::ProjectManager::instance();
    for (const bool floating : {false, true}) {
        if (floating) {
            preview->toggleView(true);
            preview->setFloating();
        } else {
            preview->toggleView(false);
        }
        drainEvents();
        EXPECT_TRUE(controls->isVisible());
        EXPECT_EQ(controls->window(), window_.get());
        EXPECT_EQ(preview->isInFloatingContainer(), floating);
        auto* selected = manager.createItem(nullptr, floating ? QStringLiteral("FloatingSelection")
            : QStringLiteral("HiddenSelection"), VNX::ProjectItem::Type::Container);
        ASSERT_NE(selected, nullptr);
        auto* selection = selected->comp<VNX::VSelection>();
        ASSERT_NE(selection, nullptr);
        selection->mode = VNX::SelectionMode::Sequence;
        router->selectItem(selected, false);
        drainEvents();
        EXPECT_TRUE(reset->isEnabled());
        const QString beforePlay = status->text();
        play->click();
        const QString afterPlay = status->text();
        EXPECT_NE(afterPlay, beforePlay);
        stop->click();
        EXPECT_NE(status->text(), afterPlay);
        playShortcut->trigger();
        EXPECT_EQ(status->text(), afterPlay);
        emit audition->auditionStateChanged(1u, 1);
        EXPECT_TRUE(play->property("playing").toBool());
        emit audition->allAuditionsStopped();
        EXPECT_FALSE(play->property("playing").toBool());
        selection->mode = VNX::SelectionMode::Match;
        emit manager.itemPropertyChanged(selected, QStringLiteral("mode"), nullptr);
        EXPECT_FALSE(reset->isEnabled());
        router->selectItem(track_, false);
        EXPECT_FALSE(reset->isEnabled());
    }
    manager.closeProject();
    drainEvents();
    EXPECT_TRUE(controls->isVisible());
    EXPECT_FALSE(reset->isEnabled());
}

TEST_F(EditorDockLayoutTest, DoubleClickTrackAndPieceOpensBoundTimeline)
{
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    const QRect original = areaRect(dock(QStringLiteral("BlueprintCanvasDock")));
    for (auto* item : {track_, piece_}) {
        router->onProjectItemDoubleClicked(item);
        drainEvents();
        auto* timeline = qobject_cast<VNX::TimelineEditorPanel*>(
            window_->activeStandaloneEditorPanel(QStringLiteral("TimelineEditor")));
        ASSERT_NE(timeline, nullptr);
        EXPECT_EQ(timeline->boundItem(), VNX::TimelineEditorPanel::timelineOwner(item));
        EXPECT_EQ(timeline->viewKind(), VNX::TimelineEditorPanel::ViewKind::Piece);
        auto* timelineDock = dock(QStringLiteral("TimelineEditorDock_%1").arg(timeline->panelId()));
        ASSERT_NE(timelineDock, nullptr);
        EXPECT_FALSE(timelineDock->isClosed());
        EXPECT_EQ(timelineDock->dockAreaWidget()->currentDockWidget(), timelineDock);
        EXPECT_TRUE(timelineDock->windowTitle().contains(VNX::TimelineEditorPanel::timelineOwner(item)->name));
        ASSERT_NE(window_->activeInspectorPanel(), nullptr);
        EXPECT_EQ(window_->activeInspectorPanel()->currentContextItem(), item);
        expectThirdOf(window_->activeInspectorDock(), original);
        expectThirdOf(timelineDock, original);
        EXPECT_EQ(window_->allTimelineEditorPanels().size(), 1);
        auto* blueprintArea = dock(QStringLiteral("BlueprintCanvasDock"))->dockAreaWidget();
        auto* timelineArea = timelineDock->dockAreaWidget();
        auto* inspectorArea = window_->activeInspectorDock()->dockAreaWidget();
        auto* column = timelineArea->parentSplitter();
        auto* center = splitChildContaining(blueprintArea, column);
        ASSERT_NE(center, nullptr);
        EXPECT_EQ(column->orientation(), Qt::Vertical);
        EXPECT_GT(column->indexOf(timelineArea), column->indexOf(center));
        auto* row = inspectorArea->parentSplitter();
        ASSERT_EQ(column->parentWidget(), row);
        EXPECT_EQ(row->orientation(), Qt::Horizontal);
        EXPECT_GT(row->indexOf(inspectorArea), row->indexOf(column));
        EXPECT_EQ(timelineArea->width(), blueprintArea->width());
        EXPECT_EQ(inspectorArea->height(), column->height());
    }
}

TEST_F(EditorDockLayoutTest, TimelineLaunchReusesUnboundPanel)
{
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    router->onBlueprintSelectionCleared();
    auto* empty = qobject_cast<VNX::TimelineEditorPanel*>(
        window_->createStandaloneEditorPanel(QStringLiteral("TimelineEditor")));
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(empty->boundItem(), nullptr);
    EXPECT_EQ(launchTimeline(track_), empty);
    EXPECT_EQ(empty->boundItem(), track_->parent);
    EXPECT_EQ(empty->laneCount(), 1);
    EXPECT_EQ(empty->laneGuid(0), track_->guid);
    EXPECT_EQ(window_->allTimelineEditorPanels().size(), 1);
    auto* orphan = VNX::ProjectManager::instance().createItem(
        nullptr, QStringLiteral("OrphanTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(orphan, nullptr);
    router->onProjectItemDoubleClicked(orphan);
    EXPECT_EQ(window_->allTimelineEditorPanels().size(), 1);
    EXPECT_EQ(empty->boundItem(), orphan);
    EXPECT_EQ(empty->viewKind(), VNX::TimelineEditorPanel::ViewKind::Track);
    EXPECT_EQ(window_->activeInspectorPanel()->currentContextItem(), orphan);
    EXPECT_EQ(window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), orphan), empty);
    empty->setPinned(true);
    router->onProjectItemDoubleClicked(orphan);
    EXPECT_EQ(window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), orphan), empty);
    auto* pieceTimeline = launchTimeline(track_);
    ASSERT_NE(pieceTimeline, nullptr);
    EXPECT_NE(pieceTimeline, empty);
    EXPECT_EQ(pieceTimeline->boundItem(), track_->parent);
    EXPECT_EQ(pieceTimeline->laneGuid(pieceTimeline->headerSelectedLane()), track_->guid);
    EXPECT_EQ(empty->boundItem(), orphan);
    EXPECT_EQ(window_->allTimelineEditorPanels().size(), 2);
}

TEST_F(EditorDockLayoutTest, MatchAndBlendActivationOpenSelectionEditorRespectingPins)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* firstItem = manager.createItem(
        nullptr, QStringLiteral("FirstSelection"), VNX::ProjectItem::Type::Container);
    auto* secondItem = manager.createItem(
        nullptr, QStringLiteral("SecondSelection"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(firstItem, nullptr);
    ASSERT_NE(secondItem, nullptr);
    secondItem->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Blend;
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    router->onProjectItemDoubleClicked(firstItem);
    drainEvents();
    EXPECT_TRUE(window_->allGridEditorPanels().isEmpty());
    auto* inspector = window_->activeInspectorPanel();
    ASSERT_NE(inspector, nullptr);
    auto* mode = inspector->findChild<QComboBox*>(QStringLiteral("prop_mode"));
    ASSERT_NE(mode, nullptr);
    mode->setCurrentIndex(mode->findData(static_cast<int>(VNX::SelectionMode::Match)));
    drainEvents();
    ASSERT_EQ(window_->allGridEditorPanels().size(), 1);
    auto* first = window_->allGridEditorPanels().front();
    EXPECT_EQ(first->boundItem(), firstItem);
    auto* selectionDock = dock(QStringLiteral("GridEditorDock_%1").arg(first->panelId()));
    ASSERT_NE(selectionDock, nullptr);
    EXPECT_FALSE(selectionDock->isClosed());
    EXPECT_TRUE(selectionDock->windowTitle().contains(QStringLiteral("Selection Editor")));
    EXPECT_TRUE(selectionDock->windowTitle().contains(firstItem->name));
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    EXPECT_GT(areaRect(selectionDock).top(), areaRect(blueprint).bottom());

    first->setPinned(true);
    router->onProjectItemSelected(secondItem);
    EXPECT_EQ(first->boundItem(), firstItem);
    EXPECT_EQ(window_->allGridEditorPanels().size(), 1);
    router->onProjectItemDoubleClicked(secondItem);
    drainEvents();
    auto* second = qobject_cast<VNX::GridEditorPanel*>(
        window_->activeStandaloneEditorPanel(QStringLiteral("GridEditor")));
    ASSERT_NE(second, nullptr);
    ASSERT_NE(second, first);
    EXPECT_EQ(first->boundItem(), firstItem);
    EXPECT_EQ(second->boundItem(), secondItem);
    EXPECT_EQ(window_->allGridEditorPanels().size(), 2);
    auto* secondDock = dock(QStringLiteral("GridEditorDock_%1").arg(second->panelId()));
    ASSERT_NE(secondDock, nullptr);
    EXPECT_FALSE(secondDock->isClosed());
    EXPECT_EQ(selectionDock->dockAreaWidget(), secondDock->dockAreaWidget());
    EXPECT_EQ(secondDock->dockAreaWidget()->currentDockWidget(), secondDock);

    router->onProjectItemSelected(track_);
    EXPECT_EQ(second->boundItem(), secondItem);
    EXPECT_FALSE(secondDock->isClosed());
    secondDock->toggleView(false);
    drainEvents();
    router->onProjectItemSelected(firstItem);
    EXPECT_TRUE(secondDock->isClosed());
    EXPECT_EQ(second->boundItem(), secondItem);
    router->onProjectItemDoubleClicked(firstItem);
    drainEvents();
    EXPECT_FALSE(secondDock->isClosed());
    EXPECT_EQ(second->boundItem(), firstItem);
    EXPECT_EQ(window_->allGridEditorPanels().size(), 2);
}

TEST_F(EditorDockLayoutTest, TimelineFollowsSelectionWithoutOpeningClosedPanels)
{
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    router->onProjectItemSelected(track_);
    EXPECT_TRUE(window_->allTimelineEditorPanels().isEmpty());
    ASSERT_TRUE(window_->createWorkspacePanel(QStringLiteral("TimelineEditor")));
    ASSERT_EQ(window_->allTimelineEditorPanels().size(), 1);
    auto* timeline = window_->allTimelineEditorPanels().front();
    EXPECT_EQ(timeline->boundItem(), track_->parent);
    drainEvents();
    auto* timelineDock = dock(QStringLiteral("TimelineEditorDock_%1").arg(timeline->panelId()));
    ASSERT_NE(timelineDock, nullptr);
    setExtent(timelineDock, 180);

    router->onProjectItemSelected(piece_);
    EXPECT_EQ(timeline->boundItem(), piece_);
    EXPECT_EQ(timeline->viewKind(), VNX::TimelineEditorPanel::ViewKind::Piece);
    EXPECT_TRUE(timelineDock->windowTitle().contains(piece_->name));
    EXPECT_EQ(timelineDock->dockAreaWidget()->height(), 180);
    auto* child = VNX::ProjectManager::instance().createItem(
        piece_, QStringLiteral("ChildTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(child, nullptr);
    drainEvents();
    EXPECT_EQ(timeline->boundItem(), piece_);
    ASSERT_EQ(timeline->laneCount(), 1);
    EXPECT_EQ(timeline->laneGuid(0), child->guid);

    timelineDock->toggleView(false);
    drainEvents();
    router->onProjectItemSelected(track_);
    EXPECT_TRUE(timelineDock->isClosed());
    EXPECT_EQ(timeline->boundItem(), piece_);
    EXPECT_EQ(window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), track_), timeline);
    drainEvents();
    EXPECT_EQ(timelineDock->dockAreaWidget()->height(), 180);
    timelineDock->toggleView(false);
    drainEvents();
    router->onProjectItemDoubleClicked(track_);
    drainEvents();
    EXPECT_FALSE(timelineDock->isClosed());
    EXPECT_EQ(timeline->boundItem(), track_->parent);
    EXPECT_EQ(window_->allTimelineEditorPanels().size(), 1);
    EXPECT_EQ(timelineDock->dockAreaWidget()->height(), 180);

    auto* orphan = VNX::ProjectManager::instance().createItem(
        nullptr, QStringLiteral("IndependentTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(orphan, nullptr);
    router->onProjectItemSelected(orphan);
    EXPECT_EQ(timeline->boundItem(), orphan);
    EXPECT_EQ(timeline->viewKind(), VNX::TimelineEditorPanel::ViewKind::Track);
    EXPECT_FALSE(timelineDock->isClosed());
    timelineDock->toggleView(false);
    router->onProjectItemSelected(piece_);
    EXPECT_TRUE(timelineDock->isClosed());
    EXPECT_EQ(timeline->boundItem(), orphan);
    router->onProjectItemDoubleClicked(orphan);
    EXPECT_FALSE(timelineDock->isClosed());
    EXPECT_EQ(timeline->boundItem(), orphan);
    EXPECT_EQ(window_->allTimelineEditorPanels().size(), 1);

    auto& manager = VNX::ProjectManager::instance();
    for (int i = 0; i < 12; ++i) {
        child = manager.createItem(piece_, QStringLiteral("Track%1").arg(i), VNX::ProjectItem::Type::Track);
        ASSERT_NE(child, nullptr);
    }
    for (int entry = 0; entry < 3; ++entry) {
        SCOPED_TRACE(entry);
        router->onProjectItemSelected(orphan);
        if (entry == 0) router->onProjectItemSelected(child);
        else if (entry == 1) router->onProjectItemDoubleClicked(child);
        else EXPECT_EQ(launchTimeline(child), timeline);
        drainEvents();
        ASSERT_EQ(timeline->boundItem(), piece_);
        EXPECT_EQ(timeline->viewKind(), VNX::TimelineEditorPanel::ViewKind::Piece);
        EXPECT_EQ(timeline->laneGuid(timeline->headerSelectedLane()), child->guid);
        EXPECT_TRUE(timelineDock->windowTitle().contains(piece_->name));
        auto* viewport = timeline->findChild<QScrollArea*>(QStringLiteral("timelineViewport"));
        ASSERT_NE(viewport, nullptr);
        EXPECT_GT(viewport->verticalScrollBar()->value(), 0);
        const QRect selectedRow = timeline->laneRect(timeline->headerSelectedLane())
            .translated(0, -viewport->verticalScrollBar()->value());
        EXPECT_TRUE(viewport->viewport()->rect().contains(selectedRow.center()));
        for (auto* row : timeline->findChildren<QWidget*>(QStringLiteral("timelineLane"))) {
            const bool selected = QUuid(row->property("trackGuid").toString()) == child->guid;
            EXPECT_EQ(row->property("selected").toBool(), selected);
            const auto* number = row->findChild<QLabel*>(QStringLiteral("tableRowNumber"));
            ASSERT_NE(number, nullptr);
            EXPECT_EQ(number->property("selected").toBool(), selected);
        }
        router->onProjectItemSelected(track_);
        drainEvents();
        EXPECT_EQ(timeline->boundItem(), track_->parent);
        EXPECT_EQ(timeline->laneGuid(timeline->headerSelectedLane()), track_->guid);
        EXPECT_EQ(window_->allTimelineEditorPanels().size(), 1);
    }
}

TEST_F(EditorDockLayoutTest, TimelineSelectionRespectsPinAndLastFocusedPanel)
{
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    auto* registry = window_->findChild<VNX::PanelInstanceRegistry*>();
    ASSERT_NE(router, nullptr);
    ASSERT_NE(registry, nullptr);
    auto* first = qobject_cast<VNX::TimelineEditorPanel*>(
        window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), track_));
    ASSERT_NE(first, nullptr);
    first->setPinned(true);
    router->onProjectItemSelected(piece_);
    EXPECT_EQ(first->boundItem(), track_->parent);
    EXPECT_EQ(window_->allTimelineEditorPanels().size(), 1);
    router->onProjectItemDoubleClicked(piece_);
    drainEvents();
    auto* second = qobject_cast<VNX::TimelineEditorPanel*>(
        window_->activeStandaloneEditorPanel(QStringLiteral("TimelineEditor")));
    ASSERT_NE(second, nullptr);
    ASSERT_NE(second, first);
    EXPECT_EQ(first->boundItem(), track_->parent);
    EXPECT_EQ(second->boundItem(), piece_);

    router->onProjectItemSelected(track_);
    EXPECT_EQ(first->boundItem(), track_->parent);
    EXPECT_EQ(first->laneGuid(first->headerSelectedLane()), track_->guid);
    EXPECT_EQ(second->boundItem(), piece_);
    auto* firstDock = dock(QStringLiteral("TimelineEditorDock_%1").arg(first->panelId()));
    ASSERT_NE(firstDock, nullptr);
    EXPECT_EQ(firstDock->dockAreaWidget()->currentDockWidget(), firstDock);

    registry->setLastFocusedPanelId(QStringLiteral("TimelineEditor"), first->panelId());
    router->onProjectItemSelected(piece_);
    first->setPinned(false);
    EXPECT_EQ(first->boundItem(), piece_);
    router->onProjectItemSelected(track_);
    EXPECT_EQ(first->boundItem(), track_->parent);
    EXPECT_EQ(second->boundItem(), piece_);
    registry->setLastFocusedPanelId(QStringLiteral("TimelineEditor"), second->panelId());
    router->onProjectItemSelected(track_);
    EXPECT_EQ(second->boundItem(), piece_);
    router->onProjectItemDoubleClicked(track_);
    drainEvents();
    EXPECT_EQ(first->boundItem(), track_->parent);
    EXPECT_EQ(first->laneGuid(first->headerSelectedLane()), track_->guid);
    EXPECT_EQ(second->boundItem(), piece_);
    second->setFocus();
    router->onProjectItemSelected(track_);
    EXPECT_EQ(first->boundItem(), track_->parent);
    EXPECT_EQ(first->laneGuid(first->headerSelectedLane()), track_->guid);
    EXPECT_EQ(second->boundItem(), piece_);
    EXPECT_EQ(window_->allTimelineEditorPanels().size(), 2);

    firstDock->toggleView(false);
    registry->setLastFocusedPanelId(QStringLiteral("TimelineEditor"), second->panelId());
    router->onProjectItemSelected(track_);
    drainEvents();
    EXPECT_TRUE(firstDock->isClosed());
    EXPECT_EQ(second->boundItem(), track_->parent);
    EXPECT_EQ(second->laneGuid(second->headerSelectedLane()), track_->guid);
}

TEST_F(EditorDockLayoutTest, TimelineInternalSelectionPreservesBindingAndDrillPath)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* outerTrack = manager.createItem(
        piece_, QStringLiteral("OuterTrack"), VNX::ProjectItem::Type::Track);
    auto* nested = manager.createItem(
        nullptr, QStringLiteral("NestedPiece"), VNX::ProjectItem::Type::Piece);
    ASSERT_NE(outerTrack, nullptr);
    ASSERT_NE(nested, nullptr);
    auto* innerTrack = manager.createItem(
        nested, QStringLiteral("InnerTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(innerTrack, nullptr);
    auto* sound = manager.createItem(
        innerTrack, QStringLiteral("InnerSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    const QUuid clipGuid = QUuid::createUuid();
    outerTrack->comp<VNX::VTrack>()->clips.items.push_back(
        {clipGuid, nested->guid, 0, 1000000000LL});
    auto* timeline = launchTimeline(piece_);
    ASSERT_NE(timeline, nullptr);
    auto* inspector = window_->activeInspectorPanel();
    ASSERT_NE(inspector, nullptr);
    const auto selectRow = [](QWidget* row) {
        const QPoint point = row->rect().center();
        QMouseEvent press(QEvent::MouseButtonPress, point, row->mapToGlobal(point),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(row, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, row->mapToGlobal(point),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(row, &release);
    };
    auto* row = timeline->findChild<QWidget*>(QStringLiteral("timelineLane"));
    ASSERT_NE(row, nullptr);
    selectRow(row);
    EXPECT_EQ(inspector->currentContextItem(), outerTrack);
    EXPECT_EQ(timeline->boundItem(), piece_);
    EXPECT_TRUE(timeline->inspectClip(0, clipGuid));
    EXPECT_EQ(inspector->currentContextItem(), nested);
    EXPECT_EQ(timeline->boundItem(), piece_);
    EXPECT_TRUE(timeline->requestBlueprintForClip(0, clipGuid));
    EXPECT_EQ(timeline->boundItem(), piece_);
    ASSERT_TRUE(timeline->drillIntoClip(0, clipGuid));
    drainEvents();
    row = timeline->findChild<QWidget*>(QStringLiteral("timelineLane"));
    ASSERT_NE(row, nullptr);
    selectRow(row);
    EXPECT_EQ(inspector->currentContextItem(), innerTrack);
    EXPECT_EQ(timeline->boundItem(), nested);
    EXPECT_EQ(timeline->drillDepth(), 1);
    EXPECT_TRUE(timeline->requestBlueprintForLane(0));
    EXPECT_EQ(timeline->boundItem(), nested);
    EXPECT_EQ(timeline->drillDepth(), 1);
    EXPECT_TRUE(timeline->inspectItem(sound, timeline->placementContextForLane(0)));
    EXPECT_EQ(inspector->currentContextItem(), sound);
    EXPECT_EQ(timeline->boundItem(), nested);
    EXPECT_EQ(timeline->drillDepth(), 1);
    EXPECT_TRUE(timeline->drillBack());
    EXPECT_EQ(timeline->boundItem(), piece_);
}

TEST_F(EditorDockLayoutTest, InspectorThenTimelineSplitsTheWholeRemainderByThirds)
{
    auto* explorer = dock(QStringLiteral("ExplorerDock"));
    auto* ai = dock(QStringLiteral("AIDock"));
    auto* information = dock(QStringLiteral("InformationDock"));
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(ai, nullptr);
    ASSERT_NE(information, nullptr);
    ASSERT_NE(blueprint, nullptr);
    EXPECT_NEAR(explorer->dockAreaWidget()->width(), 320, 2);
    EXPECT_NEAR(ai->dockAreaWidget()->width(), 320, 2);
    const QRect original = areaRect(blueprint);

    window_->openOrFocusInspectorForItem(track_);
    drainEvents();
    auto* inspector = window_->activeInspectorDock();
    ASSERT_NE(inspector, nullptr);
    auto* row = inspector->dockAreaWidget()->parentSplitter();
    auto* center = splitChildContaining(blueprint->dockAreaWidget(), row);
    ASSERT_NE(center, nullptr);
    EXPECT_EQ(row->orientation(), Qt::Horizontal);
    EXPECT_GT(row->indexOf(inspector->dockAreaWidget()), row->indexOf(center));
    expectThirdOf(inspector, original);

    auto* timeline = launchTimeline(track_);
    ASSERT_NE(timeline, nullptr);
    EXPECT_EQ(timeline->boundItem(), track_->parent);
    EXPECT_EQ(timeline->viewKind(), VNX::TimelineEditorPanel::ViewKind::Piece);
    auto* timelineDock = dock(QStringLiteral("TimelineEditorDock_%1").arg(timeline->panelId()));
    ASSERT_NE(timelineDock, nullptr);
    auto* column = timelineDock->dockAreaWidget()->parentSplitter();
    row = inspector->dockAreaWidget()->parentSplitter();
    ASSERT_EQ(column, row->parentWidget());
    EXPECT_EQ(column->orientation(), Qt::Vertical);
    EXPECT_GT(column->indexOf(timelineDock->dockAreaWidget()),
              column->indexOf(row));
    expectThirdOf(timelineDock, original);
    expectThirdOf(inspector, original);
    EXPECT_EQ(areaRect(timelineDock).right(), areaRect(inspector).right());
    EXPECT_GT(areaRect(timelineDock).top(), areaRect(inspector).bottom());
    EXPECT_NEAR(explorer->dockAreaWidget()->width(), 320, 2);
    EXPECT_NEAR(ai->dockAreaWidget()->width(), 320, 2);
}

TEST_F(EditorDockLayoutTest, InspectorOpenedAfterTimelineKeepsTheTimelineHeight)
{
    auto* timeline = qobject_cast<VNX::TimelineEditorPanel*>(
        window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), track_));
    ASSERT_NE(timeline, nullptr);
    drainEvents();
    auto* timelineDock = dock(QStringLiteral("TimelineEditorDock_%1").arg(timeline->panelId()));
    const int height = timelineDock->dockAreaWidget()->height();
    const QRect original = areaRect(timelineDock).united(
        areaRect(dock(QStringLiteral("BlueprintCanvasDock"))));
    window_->openOrFocusInspectorForItem(track_);
    drainEvents();
    ASSERT_NE(window_->activeInspectorDock(), nullptr);
    expectThirdOf(window_->activeInspectorDock(), original);
    EXPECT_EQ(timelineDock->dockAreaWidget()->height(), height);
}

TEST_F(EditorDockLayoutTest, EditorsUseTheWholeSplitContent)
{
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    auto* right = addContent(QStringLiteral("DocumentA"), ads::RightDockWidgetArea, blueprint);
    auto* lower = addContent(QStringLiteral("DocumentB"), ads::BottomDockWidgetArea, blueprint);
    const QRect before = areaRect(blueprint).united(areaRect(right)).united(areaRect(lower));
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    router->onProjectItemDoubleClicked(track_);
    drainEvents();
    auto* inspector = window_->activeInspectorDock();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    ASSERT_NE(inspector, nullptr);
    ASSERT_NE(timeline, nullptr);
    const QRect remaining = areaRect(blueprint).united(areaRect(right)).united(areaRect(lower));
    EXPECT_EQ(areaRect(inspector).right(), before.right());
    EXPECT_EQ(areaRect(inspector).top(), before.top());
    EXPECT_EQ(areaRect(inspector).bottom(), before.bottom());
    EXPECT_GT(areaRect(inspector).left(), remaining.right());
    expectThirdOf(inspector, before);
    EXPECT_EQ(areaRect(timeline).left(), remaining.left());
    EXPECT_EQ(areaRect(timeline).right(), remaining.right());
    EXPECT_GT(areaRect(timeline).top(), remaining.bottom());
    EXPECT_EQ(areaRect(timeline).bottom(), before.bottom());
    expectThirdOf(timeline, before);
    EXPECT_GT(areaRect(right).left(), areaRect(blueprint).right());
    EXPECT_GT(areaRect(lower).top(), areaRect(blueprint).bottom());
}

TEST_F(EditorDockLayoutTest, InspectorWrapsContentAndAnExistingTimeline)
{
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    auto* lower = addContent(QStringLiteral("DocumentA"), ads::BottomDockWidgetArea, blueprint);
    auto* right = addContent(QStringLiteral("DocumentB"), ads::RightDockWidgetArea, blueprint);
    ASSERT_NE(window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), track_), nullptr);
    drainEvents();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    ASSERT_NE(timeline, nullptr);
    const int height = areaRect(timeline).height();
    const QRect original = areaRect(timeline).united(areaRect(blueprint)).united(
        areaRect(right)).united(areaRect(lower));
    window_->openOrFocusInspectorForItem(track_);
    drainEvents();
    auto* inspector = window_->activeInspectorDock();
    ASSERT_NE(inspector, nullptr);
    const QRect content = areaRect(blueprint).united(areaRect(right)).united(areaRect(lower));
    EXPECT_EQ(areaRect(timeline).left(), content.left());
    EXPECT_EQ(areaRect(timeline).right(), content.right());
    EXPECT_EQ(areaRect(timeline).height(), height);
    EXPECT_GT(areaRect(inspector).left(), content.right());
    EXPECT_EQ(areaRect(inspector).top(), content.top());
    EXPECT_EQ(areaRect(inspector).bottom(), areaRect(timeline).bottom());
    expectThirdOf(inspector, original);
}

TEST_F(EditorDockLayoutTest, ClosedBlueprintDoesNotReopenOrChooseTheAnchor)
{
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    auto* document = addContent(QStringLiteral("DocumentA"), ads::RightDockWidgetArea, blueprint);
    blueprint->toggleView(false);
    drainEvents();
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    router->onProjectItemDoubleClicked(piece_);
    drainEvents();
    auto* inspector = window_->activeInspectorDock();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    ASSERT_NE(inspector, nullptr);
    ASSERT_NE(timeline, nullptr);
    EXPECT_TRUE(blueprint->isClosed());
    EXPECT_FALSE(inspector->isInFloatingContainer());
    EXPECT_FALSE(timeline->isInFloatingContainer());
    EXPECT_GT(areaRect(inspector).left(), areaRect(document).right());
    EXPECT_EQ(areaRect(inspector).top(), areaRect(document).top());
    EXPECT_EQ(areaRect(inspector).bottom(), areaRect(timeline).bottom());
    EXPECT_EQ(areaRect(timeline).left(), areaRect(document).left());
    EXPECT_EQ(areaRect(timeline).right(), areaRect(document).right());
    EXPECT_GT(areaRect(timeline).top(), areaRect(document).bottom());
}

TEST_F(EditorDockLayoutTest, FloatingBlueprintDoesNotPullNewEditorsOutOfTheMainWindow)
{
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    auto* document = addContent(QStringLiteral("DocumentA"), ads::RightDockWidgetArea, blueprint);
    blueprint->setFloating();
    drainEvents();
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    router->onProjectItemDoubleClicked(track_);
    drainEvents();
    auto* inspector = window_->activeInspectorDock();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    ASSERT_NE(inspector, nullptr);
    ASSERT_NE(timeline, nullptr);
    EXPECT_TRUE(blueprint->isInFloatingContainer());
    EXPECT_FALSE(inspector->isInFloatingContainer());
    EXPECT_FALSE(timeline->isInFloatingContainer());
    EXPECT_GT(areaRect(inspector).left(), areaRect(document).right());
    EXPECT_EQ(areaRect(timeline).left(), areaRect(document).left());
    EXPECT_EQ(areaRect(timeline).right(), areaRect(document).right());
}

TEST_F(EditorDockLayoutTest, EmptyRemainderFillsThenSplitsInCreationOrder)
{
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    blueprint->toggleView(false);
    drainEvents();
    ASSERT_NE(window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), track_), nullptr);
    drainEvents();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    ASSERT_NE(timeline, nullptr);
    EXPECT_TRUE(blueprint->isClosed());
    EXPECT_FALSE(timeline->isInFloatingContainer());
    const QRect full = areaRect(timeline);
    EXPECT_EQ(full.top(), areaRect(dock(QStringLiteral("ExplorerDock"))).top());
    EXPECT_EQ(full.bottom(), areaRect(dock(QStringLiteral("InformationDock"))).bottom());
    EXPECT_NEAR(areaRect(dock(QStringLiteral("ExplorerDock"))).width(), 320, 2);
    EXPECT_NEAR(areaRect(dock(QStringLiteral("AIDock"))).width(), 320, 2);
    window_->openOrFocusInspectorForItem(track_);
    drainEvents();
    auto* inspector = window_->activeInspectorDock();
    ASSERT_NE(inspector, nullptr);
    expectThirdOf(inspector, full);
    EXPECT_EQ(areaRect(timeline).united(areaRect(inspector)), full);
    EXPECT_GT(areaRect(inspector).left(), areaRect(timeline).right());
    EXPECT_EQ(areaRect(inspector).height(), full.height());
    EXPECT_EQ(areaRect(timeline).height(), full.height());
}

TEST_F(EditorDockLayoutTest, EmptyRemainderAlsoSupportsInspectorThenTimeline)
{
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    blueprint->toggleView(false);
    drainEvents();
    window_->openOrFocusInspectorForItem(track_);
    drainEvents();
    auto* inspector = window_->activeInspectorDock();
    ASSERT_NE(inspector, nullptr);
    EXPECT_TRUE(blueprint->isClosed());
    EXPECT_FALSE(inspector->isInFloatingContainer());
    const QRect full = areaRect(inspector);
    ASSERT_NE(window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), track_), nullptr);
    drainEvents();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    ASSERT_NE(timeline, nullptr);
    expectThirdOf(timeline, full);
    EXPECT_EQ(areaRect(timeline).united(areaRect(inspector)), full);
    EXPECT_GT(areaRect(timeline).top(), areaRect(inspector).bottom());
    EXPECT_EQ(areaRect(inspector).width(), full.width());
    EXPECT_EQ(areaRect(timeline).width(), full.width());
}

TEST_F(EditorDockLayoutTest, NoDockedContentUsesTheGapBetweenZones)
{
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    blueprint->setFloating();
    drainEvents();
    window_->findChild<VNX::SelectionRouter*>()->onProjectItemDoubleClicked(track_);
    drainEvents();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    auto* inspector = window_->activeInspectorDock();
    ASSERT_NE(timeline, nullptr);
    ASSERT_NE(inspector, nullptr);
    EXPECT_TRUE(blueprint->isInFloatingContainer());
    EXPECT_FALSE(timeline->isInFloatingContainer());
    EXPECT_FALSE(inspector->isInFloatingContainer());
    const QRect combined = areaRect(timeline).united(areaRect(inspector));
    EXPECT_GT(combined.left(), areaRect(dock(QStringLiteral("ExplorerDock"))).right());
    EXPECT_LT(combined.right(), areaRect(dock(QStringLiteral("AIDock"))).left());
    EXPECT_EQ(combined.top(), areaRect(dock(QStringLiteral("ExplorerDock"))).top());
    expectThirdOf(inspector, combined);
}

TEST_F(EditorDockLayoutTest, ClosedSplitContentDoesNotReserveBlankSpace)
{
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    auto* document = addContent(QStringLiteral("DocumentA"), ads::RightDockWidgetArea, blueprint);
    auto* lower = addContent(QStringLiteral("DocumentB"), ads::BottomDockWidgetArea, blueprint);
    for (auto* content : {blueprint, document, lower})
        content->toggleView(false);
    drainEvents();
    ASSERT_NE(window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), track_), nullptr);
    drainEvents();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    ASSERT_NE(timeline, nullptr);
    EXPECT_TRUE(blueprint->isClosed());
    EXPECT_TRUE(document->isClosed());
    EXPECT_TRUE(lower->isClosed());
    EXPECT_EQ(areaRect(timeline).top(), areaRect(dock(QStringLiteral("ExplorerDock"))).top());
    EXPECT_EQ(areaRect(timeline).bottom(), areaRect(dock(QStringLiteral("InformationDock"))).bottom());
    auto* ai = dock(QStringLiteral("AIDock"));
    const int gap = ai->dockAreaWidget()->parentSplitter()->handleWidth() + 1;
    EXPECT_EQ(areaRect(timeline).left(), areaRect(dock(QStringLiteral("ExplorerDock"))).right() + gap);
    EXPECT_EQ(areaRect(ai).left(), areaRect(timeline).right() + gap);
}

TEST_F(EditorDockLayoutTest, ClosedContentAndFloatingZonesLeaveNoBlankDockTree)
{
    for (const QString& name : {QStringLiteral("ExplorerDock"), QStringLiteral("InformationDock"),
                               QStringLiteral("AIDock"), QStringLiteral("PreviewDock")})
        dock(name)->setFloating();
    dock(QStringLiteral("BlueprintCanvasDock"))->toggleView(false);
    drainEvents();
    ASSERT_NE(window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), track_), nullptr);
    drainEvents();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    ASSERT_NE(timeline, nullptr);
    auto* manager = window_->findChild<ads::CDockManager*>();
    EXPECT_EQ(areaRect(timeline), QRect(manager->mapTo(window_.get(), QPoint()), manager->size()));
    EXPECT_TRUE(dock(QStringLiteral("BlueprintCanvasDock"))->isClosed());
}

TEST_F(EditorDockLayoutTest, CrossingContentIsLeftOutsideTheZoneBoundaries)
{
    auto* outside = addContent(QStringLiteral("DocumentA"), ads::BottomDockWidgetArea, nullptr);
    const QRect before = areaRect(outside);
    window_->findChild<VNX::SelectionRouter*>()->onProjectItemDoubleClicked(track_);
    drainEvents();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    auto* inspector = window_->activeInspectorDock();
    ASSERT_NE(timeline, nullptr);
    ASSERT_NE(inspector, nullptr);
    EXPECT_EQ(areaRect(outside), before);
    EXPECT_LT(areaRect(timeline).bottom(), before.top());
    EXPECT_LT(areaRect(inspector).bottom(), before.top());
    EXPECT_GT(areaRect(timeline).left(), areaRect(dock(QStringLiteral("ExplorerDock"))).right());
    EXPECT_LT(areaRect(inspector).right(), areaRect(dock(QStringLiteral("AIDock"))).left());
}

TEST_F(EditorDockLayoutTest, ClosedCrossingContentDoesNotBecomeTheEmptyRemainder)
{
    auto* outside = addContent(QStringLiteral("DocumentA"), ads::BottomDockWidgetArea, nullptr);
    outside->toggleView(false);
    dock(QStringLiteral("BlueprintCanvasDock"))->setFloating();
    drainEvents();
    window_->findChild<VNX::SelectionRouter*>()->onProjectItemDoubleClicked(track_);
    drainEvents();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    auto* inspector = window_->activeInspectorDock();
    ASSERT_NE(timeline, nullptr);
    ASSERT_NE(inspector, nullptr);
    EXPECT_TRUE(outside->isClosed());
    EXPECT_GT(areaRect(timeline).left(), areaRect(dock(QStringLiteral("ExplorerDock"))).right());
    EXPECT_LT(areaRect(inspector).right(), areaRect(dock(QStringLiteral("AIDock"))).left());
    EXPECT_EQ(areaRect(timeline).top(), areaRect(dock(QStringLiteral("ExplorerDock"))).top());
    EXPECT_EQ(areaRect(inspector).top(), areaRect(timeline).top());
}

TEST_F(EditorDockLayoutTest, ZoneContentsAndCollapsedZonesRemainOutsideNewEditors)
{
    auto* explorer = dock(QStringLiteral("ExplorerDock"));
    auto* ai = dock(QStringLiteral("AIDock"));
    auto* audition = dock(QStringLiteral("PreviewDock"));
    auto* sideContent = addContent(QStringLiteral("DocumentA"), ads::BottomDockWidgetArea, ai);
    controller_->setZoneCollapsed(explorer, true);
    controller_->setZoneCollapsed(ai, true);
    drainEvents();
    window_->findChild<VNX::SelectionRouter*>()->onProjectItemDoubleClicked(track_);
    drainEvents();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    auto* inspector = window_->activeInspectorDock();
    ASSERT_NE(timeline, nullptr);
    ASSERT_NE(inspector, nullptr);
    EXPECT_TRUE(explorer->isClosed());
    EXPECT_TRUE(ai->isClosed());
    EXPECT_TRUE(sideContent->isClosed());
    EXPECT_FALSE(timeline->isInFloatingContainer());
    EXPECT_FALSE(inspector->isInFloatingContainer());
    controller_->setZoneCollapsed(explorer, false);
    controller_->setZoneCollapsed(ai, false);
    audition->toggleView(true);
    drainEvents();
    EXPECT_FALSE(sideContent->isClosed());
    EXPECT_GT(areaRect(timeline).left(), areaRect(explorer).right());
    EXPECT_LT(areaRect(inspector).right(), areaRect(ai).left());
    EXPECT_LT(areaRect(inspector).right(), areaRect(sideContent).left());
    EXPECT_LT(areaRect(timeline).bottom(), areaRect(audition).top());
    EXPECT_LT(areaRect(inspector).bottom(), areaRect(audition).top());
}

TEST_F(EditorDockLayoutTest, FloatingZonesLeaveTheRemainingDockTreeAsTheAnchor)
{
    for (const QString& name : {QStringLiteral("ExplorerDock"), QStringLiteral("InformationDock"),
                               QStringLiteral("AIDock"), QStringLiteral("PreviewDock")})
        dock(name)->setFloating();
    drainEvents();
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    const QRect original = areaRect(blueprint);
    window_->findChild<VNX::SelectionRouter*>()->onProjectItemDoubleClicked(track_);
    drainEvents();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    auto* inspector = window_->activeInspectorDock();
    ASSERT_NE(timeline, nullptr);
    ASSERT_NE(inspector, nullptr);
    EXPECT_FALSE(timeline->isInFloatingContainer());
    EXPECT_FALSE(inspector->isInFloatingContainer());
    expectThirdOf(timeline, original);
    expectThirdOf(inspector, original);
    EXPECT_EQ(areaRect(blueprint).united(areaRect(timeline)).united(areaRect(inspector)), original);
    timeline->closeDockWidget();
    inspector->closeDockWidget();
    drainEvents();
    EXPECT_EQ(areaRect(blueprint), original);
}

TEST_F(EditorDockLayoutTest, ConsecutiveInspectorAndTimelineCreationKeepsBothRatios)
{
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    const QRect original = areaRect(blueprint);
    window_->openOrFocusInspectorForItem(track_);
    window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), track_);
    drainEvents();
    auto* timeline = dock(QStringLiteral("TimelineEditorDock_1"));
    auto* inspector = window_->activeInspectorDock();
    ASSERT_NE(timeline, nullptr);
    ASSERT_NE(inspector, nullptr);
    expectThirdOf(timeline, original);
    expectThirdOf(inspector, original);
    EXPECT_EQ(areaRect(timeline).right(), areaRect(inspector).right());
    EXPECT_GT(areaRect(timeline).top(), areaRect(inspector).bottom());
}

TEST_F(EditorDockLayoutTest, NewObjectsShareAreasAndRelaunchKeepsUserSizes)
{
    auto* first = launchTimeline(track_);
    ASSERT_NE(first, nullptr);
    auto* firstDock = dock(QStringLiteral("TimelineEditorDock_%1").arg(first->panelId()));
    auto* inspectorDock = window_->activeInspectorDock();
    setExtent(inspectorDock, 390);
    setExtent(firstDock, 180);
    first->setPinned(true);
    auto* second = launchTimeline(piece_);
    ASSERT_NE(second, nullptr);
    auto* secondDock = dock(QStringLiteral("TimelineEditorDock_%1").arg(second->panelId()));
    EXPECT_NE(first, second);
    EXPECT_EQ(firstDock->dockAreaWidget(), secondDock->dockAreaWidget());
    EXPECT_EQ(inspectorDock->dockAreaWidget(), window_->activeInspectorDock()->dockAreaWidget());
    EXPECT_EQ(firstDock->dockAreaWidget()->height(), 180);
    EXPECT_EQ(inspectorDock->dockAreaWidget()->width(), 390);
    EXPECT_EQ(launchTimeline(track_), second);
    EXPECT_EQ(window_->allTimelineEditorPanels().size(), 2);
    window_->pushSelectionToStandaloneEditors(piece_, VNX::StandaloneEditorPushReason::SelectionChanged);
    EXPECT_EQ(first->boundItem(), track_->parent);
    EXPECT_EQ(second->boundItem(), piece_);
    EXPECT_EQ(firstDock->dockAreaWidget()->height(), 180);
}

TEST_F(EditorDockLayoutTest, CachedPanelsKeepTheirSizesAfterRestoreAndRelaunch)
{
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    addContent(QStringLiteral("DocumentA"), ads::RightDockWidgetArea, blueprint);
    addContent(QStringLiteral("DocumentB"), ads::BottomDockWidgetArea, blueprint);
    auto* timeline = launchTimeline(track_);
    ASSERT_NE(timeline, nullptr);
    const QString timelineName = QStringLiteral("TimelineEditorDock_%1").arg(timeline->panelId());
    const QString inspectorName = window_->activeInspectorDock()->objectName();
    setExtent(window_->activeInspectorDock(), 390);
    setExtent(dock(timelineName), 180);
    controller_->saveProjectLayout();
    dock(timelineName)->closeDockWidget();
    dock(inspectorName)->closeDockWidget();
    drainEvents();
    controller_->loadProjectLayout();
    drainEvents();
    ASSERT_TRUE(controller_->currentLayoutFromCache());
    ASSERT_NE(dock(timelineName), nullptr);
    ASSERT_NE(dock(inspectorName), nullptr);
    ASSERT_NE(dock(timelineName)->dockAreaWidget(), nullptr);
    ASSERT_NE(dock(inspectorName)->dockAreaWidget(), nullptr);
    EXPECT_EQ(dock(timelineName)->dockAreaWidget()->height(), 180);
    EXPECT_EQ(dock(inspectorName)->dockAreaWidget()->width(), 390);
    auto* restored = launchTimeline(track_);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->boundItem(), track_->parent);
    EXPECT_EQ(window_->allTimelineEditorPanels().size(), 1);
    ASSERT_NE(dock(timelineName), nullptr);
    ASSERT_NE(dock(inspectorName), nullptr);
    ASSERT_NE(dock(timelineName)->dockAreaWidget(), nullptr);
    ASSERT_NE(dock(inspectorName)->dockAreaWidget(), nullptr);
    EXPECT_EQ(dock(timelineName)->dockAreaWidget()->height(), 180);
    EXPECT_EQ(dock(inspectorName)->dockAreaWidget()->width(), 390);
}

TEST_F(EditorDockLayoutTest, RelaunchPreservesACachedCentralTabPlacement)
{
    auto* timeline = launchTimeline(track_);
    ASSERT_NE(timeline, nullptr);
    const QString name = QStringLiteral("TimelineEditorDock_%1").arg(timeline->panelId());
    auto* manager = window_->findChild<ads::CDockManager*>();
    manager->addDockWidget(ads::CenterDockWidgetArea, dock(name),
                          dock(QStringLiteral("BlueprintCanvasDock"))->dockAreaWidget());
    drainEvents();
    controller_->saveProjectLayout();
    dock(name)->closeDockWidget();
    drainEvents();
    controller_->loadProjectLayout();
    drainEvents();
    ASSERT_NE(launchTimeline(track_), nullptr);
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    router->onProjectItemDoubleClicked(track_);
    drainEvents();
    EXPECT_EQ(dock(name)->dockAreaWidget(),
              dock(QStringLiteral("BlueprintCanvasDock"))->dockAreaWidget());
    EXPECT_EQ(dock(name)->dockAreaWidget()->currentDockWidget(), dock(name));
    EXPECT_EQ(window_->allTimelineEditorPanels().size(), 1);
}

TEST_F(EditorDockLayoutTest, BlueprintPeakResizesAsOverlayAndRestoresWidthAfterCollapse)
{
    auto* meter = window_->findChild<VNX::MeterPanel*>(QStringLiteral("BlueprintPeakMeter"));
    auto* panel = window_->findChild<VNX::BlueprintPanel*>();
    auto* handle = window_->findChild<QWidget*>(QStringLiteral("BlueprintMeterResizeHandle"));
    ASSERT_NE(meter, nullptr);
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(handle, nullptr);
    auto* canvas = panel->canvas();
    auto canvasGeometry = canvas->geometry();
    auto sceneOrigin = canvas->mapToScene(QPoint());
    const auto transform = canvas->transform();
    const auto expectStableCanvas = [&] {
        EXPECT_EQ(canvas->geometry(), canvasGeometry);
        EXPECT_EQ(canvas->mapToScene(QPoint()), sceneOrigin);
        EXPECT_EQ(canvas->transform(), transform);
    };
    auto* expand = meter->findChild<QToolButton*>(QStringLiteral("meterExpandButton"));
    ASSERT_NE(expand, nullptr);
    const int collapsedWidth = meter->width();
    EXPECT_FALSE(handle->isVisible());
    expand->click();
    drainEvents();
    ASSERT_TRUE(handle->isVisible());
    expectStableCanvas();
    const int expandedWidth = meter->width();
    EXPECT_GT(expandedWidth, collapsedWidth);
    const QPoint pressPosition = handle->rect().center();
    const QPoint globalPress = handle->mapToGlobal(pressPosition);
    QMouseEvent press(QEvent::MouseButtonPress, pressPosition, globalPress,
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &press);
    const QPoint globalMove = globalPress + QPoint(96, 0);
    QMouseEvent move(QEvent::MouseMove, handle->mapFromGlobal(globalMove), globalMove,
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, handle->mapFromGlobal(globalMove), globalMove,
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &release);
    drainEvents();
    const int resizedWidth = meter->width();
    EXPECT_NEAR(resizedWidth, expandedWidth + 96, 3);
    expectStableCanvas();
    window_->resize(window_->width() + 100, window_->height());
    drainEvents();
    EXPECT_EQ(meter->width(), resizedWidth);
    canvasGeometry = canvas->geometry();
    sceneOrigin = canvas->mapToScene(QPoint());
    expand->click();
    drainEvents();
    EXPECT_EQ(meter->width(), collapsedWidth);
    EXPECT_FALSE(handle->isVisible());
    expectStableCanvas();
    expand->click();
    drainEvents();
    EXPECT_EQ(meter->width(), resizedWidth);
    EXPECT_TRUE(handle->isVisible());
    expectStableCanvas();

    window_->openOrFocusInspectorForItem(track_);
    drainEvents();
    EXPECT_EQ(meter->width(), resizedWidth);
    const QSize panelSize = panel->size();
    ASSERT_LT(panel->minimumWidth(), panelSize.width());
    panel->resize(panel->minimumWidth(), panelSize.height());
    drainEvents();
    EXPECT_TRUE(expand->isChecked());
    EXPECT_TRUE(handle->isVisible());
    EXPECT_EQ(meter->width(), meter->minimumWidth());
    panel->resize(panelSize);
    drainEvents();
    EXPECT_TRUE(expand->isChecked());
    EXPECT_EQ(meter->width(), resizedWidth);

    expand->click();
    drainEvents();
    EXPECT_FALSE(expand->isChecked());
    EXPECT_FALSE(handle->isVisible());
    panel->resize(panel->minimumWidth(), panelSize.height());
    drainEvents();
    panel->resize(panelSize);
    drainEvents();
    EXPECT_FALSE(expand->isChecked());
    EXPECT_FALSE(handle->isVisible());
}

TEST_F(EditorDockLayoutTest, LegacySingletonLayoutsRestoreThroughTheInstanceRegistry)
{
    auto* manager = window_->findChild<ads::CDockManager*>();
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    for (const auto& oldName : {QStringLiteral("MeterDock"), QStringLiteral("MonitorMeterDock")}) {
        for (bool visible : {false, true}) {
            SCOPED_TRACE(oldName.toStdString());
            SCOPED_TRACE(visible);
            auto* legacy = addContent(oldName, ads::BottomDockWidgetArea, blueprint);
            legacy->toggleView(visible);
            controller_->saveProjectLayout();
            manager->removeDockWidget(legacy);
            legacy->deleteLater();
            drainEvents();
            controller_->loadProjectLayout();
            drainEvents();
            auto* restored = dock(QStringLiteral("MeterDock_1"));
            ASSERT_NE(restored, nullptr);
            EXPECT_EQ(restored->isClosed(), !visible);
            EXPECT_EQ(window_->workspacePanelInstanceIds(QStringLiteral("Meter")), QList<int>{1});
            EXPECT_EQ(restored->widget(), window_->workspacePanelContent(QStringLiteral("Meter"), 1));
            if (visible) {
                EXPECT_FALSE(restored->isInFloatingContainer());
                EXPECT_GT(areaRect(restored).top(), areaRect(blueprint).bottom());
            }
            controller_->saveProjectLayout();
            controller_->loadProjectLayout();
            drainEvents();
            EXPECT_EQ(window_->workspacePanelInstanceIds(QStringLiteral("Meter")), QList<int>{1});
            restored->closeDockWidget();
            drainEvents();
        }
    }
}

TEST_F(EditorDockLayoutTest, DetachedMetersHaveIndependentStateAndRestoreTheirLayout)
{
    auto* blueprint = dock(QStringLiteral("BlueprintCanvasDock"));
    auto* manager = window_->findChild<ads::CDockManager*>();
    auto* registry = window_->findChild<VNX::PanelInstanceRegistry*>();
    ASSERT_NE(registry, nullptr);
    const auto expectTitles = [&] {
        for (int id : window_->workspacePanelInstanceIds(QStringLiteral("Meter"))) {
            auto* meterDock = dock(QStringLiteral("MeterDock_%1").arg(id));
            ASSERT_NE(meterDock, nullptr);
            EXPECT_EQ(meterDock->windowTitle(), window_->workspacePanelTitle(QStringLiteral("Meter")));
        }
    };
    const QRect original = areaRect(blueprint);
    auto* embedded = window_->findChild<VNX::MeterPanel*>(QStringLiteral("BlueprintPeakMeter"));
    ASSERT_NE(embedded, nullptr);
    auto* canvas = embedded->findChild<QWidget*>(QStringLiteral("meterCanvas"));
    ASSERT_NE(canvas, nullptr);
    auto& projects = VNX::ProjectManager::instance();
    auto* bus = projects.createItem(projects.getMasterBusItem(), QStringLiteral("Meter Bus"),
                                   VNX::ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    embedded->setBus(bus->objectID);
    embedded->setAmplitudeExponent(0.6);
    const QPoint point = embedded->plotBounds().center();
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, point, canvas->mapToGlobal(point),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &doubleClick);
    drainEvents();
    auto* first = qobject_cast<VNX::MeterPanel*>(window_->workspacePanelContent(QStringLiteral("Meter"), 1));
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->busObjectID(), bus->objectID);
    EXPECT_EQ(first->mode(), VNX::MeterPanel::Mode::Peak);
    EXPECT_DOUBLE_EQ(first->amplitudeExponent(), 0.6);
    EXPECT_TRUE(dock(QStringLiteral("MeterDock_1"))->isInFloatingContainer());
    embedded->findChild<QToolButton*>(QStringLiteral("meterExpandButton"))->click();
    drainEvents();
    auto* selector = embedded->findChild<QWidget*>(QStringLiteral("meterBusSelector"));
    auto* detach = embedded->findChild<QToolButton*>(QStringLiteral("meterDetachButton"));
    ASSERT_TRUE(selector->isVisible());
    ASSERT_TRUE(detach->isVisible());
    embedded->setMode(VNX::MeterPanel::Mode::Loudness);
    embedded->setLoudnessMeasurement(VNX::MeterPanel::LoudnessMeasurement::Lufs);
    detach->click();
    drainEvents();
    auto* second = qobject_cast<VNX::MeterPanel*>(window_->workspacePanelContent(QStringLiteral("Meter"), 2));
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->mode(), VNX::MeterPanel::Mode::Loudness);
    EXPECT_EQ(second->loudnessMeasurement(), VNX::MeterPanel::LoudnessMeasurement::Lufs);
    first->setMode(VNX::MeterPanel::Mode::Spectrogram);
    first->setSpectrogramSpeed(3.25);
    second->setBus(0);
    second->setAmplitudeExponent(0.1);
    EXPECT_EQ(first->busObjectID(), bus->objectID);
    EXPECT_EQ(embedded->busObjectID(), bus->objectID);
    EXPECT_EQ(embedded->mode(), VNX::MeterPanel::Mode::Loudness);
    EXPECT_EQ(window_->workspacePanelInstanceIds(QStringLiteral("Meter")).size(), 2);
    expectTitles();
    manager->addDockWidget(ads::BottomDockWidgetArea, dock(QStringLiteral("MeterDock_1")), blueprint->dockAreaWidget());
    controller_->saveProjectLayout();
    const QPointer<QWidget> firstBeforeRestore = first;
    const QPointer<QWidget> secondBeforeRestore = second;
    dock(QStringLiteral("MeterDock_1"))->closeDockWidget();
    dock(QStringLiteral("MeterDock_2"))->closeDockWidget();
    drainEvents();
    EXPECT_TRUE(firstBeforeRestore.isNull());
    EXPECT_TRUE(secondBeforeRestore.isNull());
    EXPECT_TRUE(registry->allPanels(QStringLiteral("Meter")).isEmpty());
    EXPECT_TRUE(window_->workspacePanelInstanceIds(QStringLiteral("Meter")).isEmpty());
    controller_->loadProjectLayout();
    drainEvents();
    EXPECT_EQ(window_->workspacePanelInstanceIds(QStringLiteral("Meter")).size(), 2);
    expectTitles();
    first = qobject_cast<VNX::MeterPanel*>(window_->workspacePanelContent(QStringLiteral("Meter"), 1));
    second = qobject_cast<VNX::MeterPanel*>(window_->workspacePanelContent(QStringLiteral("Meter"), 2));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->busObjectID(), bus->objectID);
    EXPECT_EQ(first->mode(), VNX::MeterPanel::Mode::Spectrogram);
    EXPECT_DOUBLE_EQ(first->spectrogramSpeed(), 3.25);
    EXPECT_EQ(second->busObjectID(), 0u);
    EXPECT_EQ(second->mode(), VNX::MeterPanel::Mode::Loudness);
    EXPECT_EQ(second->loudnessMeasurement(), VNX::MeterPanel::LoudnessMeasurement::Lufs);
    EXPECT_DOUBLE_EQ(second->amplitudeExponent(), 0.1);
    EXPECT_FALSE(dock(QStringLiteral("MeterDock_1"))->isInFloatingContainer());
    EXPECT_GT(areaRect(dock(QStringLiteral("MeterDock_1"))).top(), areaRect(blueprint).bottom());
    EXPECT_TRUE(dock(QStringLiteral("MeterDock_2"))->isInFloatingContainer());
    controller_->selectDefaultLayout();
    drainEvents();
    EXPECT_TRUE(window_->workspacePanelInstanceIds(QStringLiteral("Meter")).isEmpty());
    EXPECT_EQ(areaRect(blueprint), original);
}

TEST_F(EditorDockLayoutTest, CachedLayoutWithoutEditorsStillSeedsNewPanels)
{
    controller_->saveProjectLayout();
    controller_->loadProjectLayout();
    drainEvents();
    ASSERT_TRUE(controller_->currentLayoutFromCache());
    const QRect original = areaRect(dock(QStringLiteral("BlueprintCanvasDock")));
    auto* timeline = launchTimeline(piece_);
    ASSERT_NE(timeline, nullptr);
    EXPECT_EQ(timeline->viewKind(), VNX::TimelineEditorPanel::ViewKind::Piece);
    expectThirdOf(window_->activeInspectorDock(), original);
    expectThirdOf(dock(QStringLiteral("TimelineEditorDock_%1").arg(timeline->panelId())), original);
}

TEST_F(EditorDockLayoutTest, ViewAndIpcUseTheRightAreaWhileExplicitWindowsFloat)
{
    const QRect original = areaRect(dock(QStringLiteral("BlueprintCanvasDock")));
    window_->openItemInNewInspector(track_, VNX::MainWindow::PreferredOpenArea::Float);
    drainEvents();
    ASSERT_NE(window_->activeInspectorDock(), nullptr);
    EXPECT_TRUE(window_->activeInspectorDock()->isInFloatingContainer());
    ASSERT_TRUE(window_->createWorkspacePanel(QStringLiteral("Inspector")));
    drainEvents();
    auto* inspectorDock = dock(QStringLiteral("InspectorDock_2"));
    ASSERT_NE(inspectorDock, nullptr);
    EXPECT_FALSE(inspectorDock->isInFloatingContainer());
    expectThirdOf(inspectorDock, original);
    const int width = inspectorDock->dockAreaWidget()->width();
    auto* row = inspectorDock->dockAreaWidget()->parentSplitter();
    auto* center = splitChildContaining(dock(QStringLiteral("BlueprintCanvasDock"))->dockAreaWidget(), row);
    ASSERT_NE(center, nullptr);
    EXPECT_EQ(row->orientation(), Qt::Horizontal);
    EXPECT_GT(row->indexOf(inspectorDock->dockAreaWidget()), row->indexOf(center));

    auto* server = window_->ipcServer();
    ASSERT_NE(server, nullptr);
    server->setAppState(VNX::IPCServer::AppState::Ready);
    for (const QJsonObject& args : {QJsonObject{}, QJsonObject{{"path", track_->name}}}) {
        const auto response = server->dispatchCommand(QStringLiteral("create_inspector_panel"), args);
        ASSERT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const int id = response.value(QStringLiteral("result")).toObject()
                           .value(QStringLiteral("panel_id")).toInt();
        drainEvents();
        auto* created = dock(QStringLiteral("InspectorDock_%1").arg(id));
        ASSERT_NE(created, nullptr);
        EXPECT_FALSE(created->isInFloatingContainer());
        EXPECT_EQ(created->dockAreaWidget(), inspectorDock->dockAreaWidget());
        EXPECT_EQ(created->dockAreaWidget()->width(), width);
        if (!args.isEmpty())
            EXPECT_EQ(window_->findInspectorPanel(id)->currentContextItem(), track_);
    }

    window_->openItemInNewInspector(track_, VNX::MainWindow::PreferredOpenArea::Float);
    drainEvents();
    ASSERT_NE(window_->activeInspectorDock(), nullptr);
    EXPECT_TRUE(window_->activeInspectorDock()->isInFloatingContainer());
    EXPECT_EQ(inspectorDock->dockAreaWidget()->width(), width);
}

TEST_F(EditorDockLayoutTest, TreeBatchTabsKeepBothSelectionsAndBindEveryInspector)
{
    auto* hierarchy = window_->createHierarchyPanel();
    auto* explorer = window_->findChild<VNX::ProjectExplorer*>();
    ASSERT_NE(hierarchy, nullptr);
    ASSERT_NE(explorer, nullptr);
    hierarchy->setSelectedProjectItems({track_, piece_});
    emit hierarchy->objectSelectionChanged(hierarchy->selectedItems());
    drainEvents();
    ASSERT_EQ(explorer->selectedItems().size(), 2);
    const int before = window_->allInspectorPanels().size();
    auto* tree = hierarchy->treeWidget();
    ASSERT_NE(tree->currentItem(), nullptr);
    tree->scrollToItem(tree->currentItem());
    QTimer::singleShot(0, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_NE(menu, nullptr);
        QAction* open = nullptr;
        for (auto* action : menu->actions()) {
            if (action->text() == QStringLiteral("Open in New Tab")) open = action;
        }
        if (open) open->trigger();
        menu->close();
        EXPECT_NE(open, nullptr);
    });
    ASSERT_TRUE(QMetaObject::invokeMethod(hierarchy, "onContextMenu", Qt::DirectConnection,
        Q_ARG(QPoint, tree->visualItemRect(tree->currentItem()).center())));
    drainEvents();
    EXPECT_EQ(window_->allInspectorPanels().size(), before + 2);
    EXPECT_EQ(hierarchy->selectedItems().size(), 2);
    EXPECT_EQ(explorer->selectedItems().size(), 2);
    for (auto* item : {track_, piece_}) {
        auto* inspector = window_->inspectorShowingItem(item);
        ASSERT_NE(inspector, nullptr);
        EXPECT_EQ(inspector->currentContextItem(), item);
        EXPECT_TRUE(hierarchy->selectedItems().contains(item));
        EXPECT_TRUE(explorer->selectedItems().contains(item));
    }
}

TEST(StandaloneEditorSelectionTest, CreatingObjectsOnlyRefreshesOpenPanels)
{
    using namespace VNX;
    ensureApp();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto& pm = ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    ActionManager::instance().clearAllForTesting();
    auto window = std::make_unique<MainWindow>();
    const auto cleanup = qScopeGuard([&]() {
        UndoRedoManager::instance().clear();
        if (pm.isProjectOpen()) pm.closeProject();
        QObject::disconnect(&UndoRedoManager::instance(), nullptr, window.get(), nullptr);
        ToastManager::instance().setParentWidget(nullptr);
        window.reset();
        ActionManager::instance().setTransactionCallbacks({});
        ActionManager::instance().clearAllForTesting();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    ProjectConfig config;
    config.name = QStringLiteral("PassiveCreationSelection");
    config.location = directory.path();
    ASSERT_TRUE(pm.createProject(config));
    auto* hierarchy = window->createHierarchyPanel();
    ASSERT_NE(hierarchy, nullptr);
    ASSERT_TRUE(window->allInspectorPanels().isEmpty());

    auto* layer = pm.createItem(nullptr, QStringLiteral("Layer"), ProjectItemType::Layer);
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(hierarchy->selectProjectItem(layer));
    EXPECT_TRUE(window->allInspectorPanels().isEmpty());
    auto* container = pm.createItem(layer, QStringLiteral("Container"), ProjectItemType::Container);
    ASSERT_NE(container, nullptr);
    ASSERT_TRUE(hierarchy->selectProjectItem(container));
    EXPECT_TRUE(window->allInspectorPanels().isEmpty());
    EXPECT_EQ(window->activeStandaloneEditorPanel(QStringLiteral("TransitionEditor")), nullptr);
    container->compOrCreate<VSelection>()->mode = SelectionMode::Match;
    window->pushSelectionToStandaloneEditors(container, StandaloneEditorPushReason::SelectionChanged);
    EXPECT_EQ(window->activeStandaloneEditorPanel(QStringLiteral("GridEditor")), nullptr);
    auto* attenuation = pm.createItem(nullptr, QStringLiteral("Attenuation"), ProjectItemType::Attenuation);
    ASSERT_NE(attenuation, nullptr);
    window->pushSelectionToStandaloneEditors(attenuation, StandaloneEditorPushReason::SelectionChanged);
    EXPECT_EQ(window->activeStandaloneEditorPanel(QStringLiteral("ExternalEditor")), nullptr);
    EXPECT_TRUE(window->allInspectorPanels().isEmpty());

    const auto selectInBlueprint = [&](ProjectItem* item) {
        auto* canvas = window->blueprintCanvas();
        canvas->rebuildFromProject();
        canvas->scene()->clearSelection();
        const auto& graph = canvas->activeSignalPathSource()->graph();
        for (size_t index = 0; index < graph.nodes.size(); ++index) {
            if (graph.nodes[index].key.objectGuid == item->guid) {
                canvas->signalPathNode(index)->setSelected(true);
                return;
            }
        }
        ADD_FAILURE() << "Created object is missing from Blueprint";
    };
    auto* nested = pm.createItem(container, QStringLiteral("Nested"), ProjectItemType::Container);
    ASSERT_NE(nested, nullptr);
    selectInBlueprint(nested);
    EXPECT_TRUE(window->allInspectorPanels().isEmpty());
    EXPECT_EQ(window->activeStandaloneEditorPanel(QStringLiteral("TransitionEditor")), nullptr);

    EXPECT_FALSE(window->workspacePanelTypes().contains(QStringLiteral("TransitionEditor")));
    EXPECT_FALSE(window->createWorkspacePanel(QStringLiteral("TransitionEditor")));
    EXPECT_EQ(window->openStandaloneEditor(QStringLiteral("TransitionEditor"), container), nullptr);
    EXPECT_EQ(window->createStandaloneEditorPanel(QStringLiteral("TransitionEditor")), nullptr);
    auto* registry = window->findChild<PanelInstanceRegistry*>();
    ASSERT_NE(registry, nullptr);
    DockInstanceSpec savedTransition;
    savedTransition.typeId = QStringLiteral("TransitionEditor");
    savedTransition.instanceId = 1;
    registry->recreate(savedTransition);
    window->pushSelectionToStandaloneEditors(container, StandaloneEditorPushReason::SelectionChanged);
    EXPECT_TRUE(window->allTransitionEditorPanels().isEmpty());
    EXPECT_EQ(window->findChild<ads::CDockWidget*>(QStringLiteral("TransitionEditorDock_1")), nullptr);

    auto* inspector = window->createInspectorPanel(MainWindow::PreferredOpenArea::CentralTab);
    ASSERT_NE(inspector, nullptr);
    auto* sound = pm.createItem(nested, QStringLiteral("Sound"), ProjectItemType::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(hierarchy->selectProjectItem(sound));
    EXPECT_EQ(inspector->currentContextItem(), sound);
    selectInBlueprint(container);
    EXPECT_EQ(inspector->currentContextItem(), container);
    ASSERT_EQ(window->allInspectorPanels().size(), 1);

    auto* dock = window->activeInspectorDock();
    ASSERT_NE(dock, nullptr);
    dock->toggleView(false);
    auto* track = pm.createItem(layer, QStringLiteral("Track"), ProjectItemType::Track);
    ASSERT_NE(track, nullptr);
    selectInBlueprint(track);
    EXPECT_TRUE(dock->isClosed());
    EXPECT_EQ(window->activeInspectorPanel(), nullptr);
    EXPECT_EQ(window->allInspectorPanels().size(), 1);
    EXPECT_EQ(window->activeStandaloneEditorPanel(QStringLiteral("TimelineEditor")), nullptr);
    EXPECT_EQ(window->activeStandaloneEditorPanel(QStringLiteral("SourceEditor")), nullptr);
}

TEST_F(StandaloneEditorDispatchTest, CollapseOnlyUnpinnedDefaultSlotWhenUnrelated)
{
    EXPECT_TRUE(VNX::StandaloneEditorDispatch::shouldCollapse(false, false, true));
    EXPECT_FALSE(VNX::StandaloneEditorDispatch::shouldCollapse(true, false, true));
    EXPECT_FALSE(VNX::StandaloneEditorDispatch::shouldCollapse(false, true, true));
    EXPECT_FALSE(VNX::StandaloneEditorDispatch::shouldCollapse(false, false, false));
}

TEST_F(StandaloneEditorDispatchTest, HideExceptOnRestoreSync)
{
    EXPECT_TRUE(VNX::StandaloneEditorDispatch::shouldHide(
        VNX::StandaloneEditorPushReason::SelectionChanged));
    EXPECT_TRUE(VNX::StandaloneEditorDispatch::shouldHide(
        VNX::StandaloneEditorPushReason::PinToggled));
    EXPECT_FALSE(VNX::StandaloneEditorDispatch::shouldHide(
        VNX::StandaloneEditorPushReason::RestoreSync));
}

TEST_F(StandaloneEditorDispatchTest, RestoreSyncClearsUnpinnedDefaultSlotWithoutReveal)
{
    EXPECT_FALSE(VNX::StandaloneEditorDispatch::shouldHide(
        VNX::StandaloneEditorPushReason::RestoreSync));
    EXPECT_TRUE(VNX::StandaloneEditorDispatch::shouldCollapse(false, false, true));
    EXPECT_FALSE(VNX::StandaloneEditorDispatch::shouldCollapse(false, true, true));
    EXPECT_FALSE(VNX::StandaloneEditorDispatch::shouldCollapse(false, false, false));

    VNX::TransitionEditorPanel unpinned;
    VNX::TransitionEditorPanel pinned;
    VNX::TransitionEditorPanel floated;
    unpinned.bindItem(container_);
    pinned.bindItem(container_);
    floated.bindItem(container_);
    pinned.setPinned(true);

    auto applyRestoreSync = [](VNX::TransitionEditorPanel& editor, bool atDefaultSlot) {
        const bool accepted = false;
        if (VNX::StandaloneEditorDispatch::shouldCollapse(
                accepted, editor.isPinned(), atDefaultSlot))
            editor.clearEditor();
    };

    applyRestoreSync(unpinned, true);
    applyRestoreSync(pinned, true);
    applyRestoreSync(floated, false);

    EXPECT_EQ(unpinned.boundItem(), nullptr);
    EXPECT_TRUE(unpinned.currentItemGuid().isNull());
    EXPECT_EQ(unpinned.mountState(), VNX::TransitionMountState::None);
    EXPECT_EQ(pinned.boundItem(), container_);
    EXPECT_TRUE(pinned.isPinned());
    EXPECT_EQ(floated.boundItem(), container_);
    EXPECT_FALSE(floated.isPinned());
}

TEST_F(StandaloneEditorDispatchTest, UnpinRetriggersSelectionDispatch)
{
    VNX::ExternalEditorPanel editor;
    int dispatchCount = 0;
    bool lastCollapsed = false;
    QObject::connect(&editor, &VNX::ExternalEditorPanel::pinToggled,
                     [&](bool) {
                         ++dispatchCount;
                         lastCollapsed = VNX::StandaloneEditorDispatch::shouldCollapse(
                             false, editor.isPinned(), true);
                     });

    editor.setPinned(true);
    ASSERT_EQ(dispatchCount, 1);
    EXPECT_FALSE(lastCollapsed);

    editor.setPinned(false);
    ASSERT_EQ(dispatchCount, 2);
    EXPECT_TRUE(lastCollapsed);
}

TEST_F(StandaloneEditorDispatchTest, CollapseSkipsFloatedInstanceWhenDefaultSlotAlsoExists)
{
    EXPECT_TRUE(VNX::StandaloneEditorDispatch::shouldCollapse(false, false, true));
    EXPECT_FALSE(VNX::StandaloneEditorDispatch::shouldCollapse(false, false, false));
}

TEST_F(StandaloneEditorDispatchTest, LaunchMapKeepsOnlyLiveSlots)
{
    EXPECT_EQ(VNX::EditorLaunchMap::editorPanelTypeForSlot(QStringLiteral("timeline")),
              QStringLiteral("TimelineEditor"));
    EXPECT_EQ(VNX::EditorLaunchMap::editorPanelTypeForSlot(QStringLiteral("attenuation")),
              QStringLiteral("ExternalEditor"));
    EXPECT_EQ(VNX::EditorLaunchMap::editorPanelTypeForSlot(QStringLiteral("selections")),
              QStringLiteral("GridEditor"));
    EXPECT_TRUE(VNX::EditorLaunchMap::editorPanelTypeForSlot(QStringLiteral("matches")).isEmpty());
    EXPECT_TRUE(VNX::EditorLaunchMap::editorPanelTypeForSlot(QStringLiteral("blends")).isEmpty());
    EXPECT_TRUE(VNX::EditorLaunchMap::editorPanelTypeForSlot(QStringLiteral("selection")).isEmpty());
    EXPECT_TRUE(VNX::EditorLaunchMap::editorPanelTypeForSlot(QStringLiteral("transition")).isEmpty());
}

TEST_F(EditorDockLayoutTest, RuntimeUuidNavigationOpensEffectOwnerAndTimelineSelection)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* sound = manager.createItem(nullptr, QStringLiteral("Tone"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    VNX::EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.parameters = {0.0f};
    ASSERT_TRUE(sound->comp<VNX::VMixNode>()->effects.replace({effect}));
    VNX::Clip clip{QUuid::createUuid(), sound->guid, 0, 1000000000};
    ASSERT_TRUE(track_->comp<VNX::VTrack>()->clips.mintOccurrenceOrdinal(&clip));
    track_->comp<VNX::VTrack>()->clips.items.push_back(clip);
    VNX::Marker marker{QUuid::createUuid(), VNX::MarkerKind::Custom, QStringLiteral("Accent"), 0};
    piece_->comp<VNX::VPiece>()->markers.items.push_back(marker);
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    ASSERT_TRUE(router->revealProjectObject(effect.runtimeId()));
    ASSERT_NE(window_->activeInspectorPanel(), nullptr);
    EXPECT_EQ(window_->activeInspectorPanel()->currentContextItem(), sound);
    ASSERT_TRUE(router->revealProjectObject(clip.runtimeId()));
    auto* timeline = qobject_cast<VNX::TimelineEditorPanel*>(
        window_->activeStandaloneEditorPanel(QStringLiteral("TimelineEditor")));
    ASSERT_NE(timeline, nullptr);
    EXPECT_EQ(timeline->currentItemGuid(), track_->parent->guid);
    EXPECT_TRUE(timeline->selectedClipGuids().contains(clip.clipGuid));
    ASSERT_TRUE(router->revealProjectObject(marker.runtimeId()));
    timeline = qobject_cast<VNX::TimelineEditorPanel*>(
        window_->activeStandaloneEditorPanel(QStringLiteral("TimelineEditor")));
    ASSERT_NE(timeline, nullptr);
    EXPECT_EQ(timeline->currentItemGuid(), piece_->guid);
    EXPECT_EQ(timeline->selectedMarkerGuid(), marker.markerGuid);
    auto* variable = manager.createItem(nullptr, QStringLiteral("Intensity"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), variable->guid);
    binding.guid = QUuid::createUuid();
    ASSERT_TRUE(sound->comp<VNX::VMixNode>()->bindings.replace({binding}));
    ASSERT_TRUE(router->revealProjectObject(binding.runtimeId()));
    auto* curves = qobject_cast<VNX::ExternalEditorPanel*>(
        window_->activeStandaloneEditorPanel(QStringLiteral("ExternalEditor")));
    ASSERT_NE(curves, nullptr);
    EXPECT_EQ(curves->boundItem(), sound);
    EXPECT_EQ(curves->selection().curve.bindingStableId, binding.runtimeId());
    ASSERT_TRUE(sound->comp<VNX::VMixNode>()->effects.replace({}));
    EXPECT_FALSE(router->revealProjectObject(effect.runtimeId()));
}

TEST_F(EditorDockLayoutTest, EventIdentityIndexSurvivesInvalidAuthoringAndObjectEdits)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* variable = manager.createItem(nullptr, QStringLiteral("Intensity"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    variable->components.remove<VNX::VVariable>();
    QString error;
    EXPECT_EQ(VNX::buildCurrentWorldSyncRequest(*manager.currentProject(), std::nullopt, &error), nullptr);
    EXPECT_EQ(error, QStringLiteral("Variable definition is invalid"));
    VNX::Monitor::CapturedEvent event;
    event.header.stableId = variable->runtimeId();
    event.objectGuid = variable->guid;
    event.projectGuid = QUuid(manager.currentProject()->projectId);
    auto* capture = window_->eventCaptureController();
    ASSERT_NE(capture, nullptr);
    EXPECT_EQ(capture->matchingProjectObject(event), variable->objectID);
    ASSERT_TRUE(manager.renameItem(variable, QStringLiteral("Intensity_Renamed")));
    EXPECT_EQ(capture->matchingProjectObject(event), variable->objectID);
    auto* copy = manager.duplicateItem(variable);
    ASSERT_NE(copy, nullptr);
    EXPECT_NE(copy->runtimeId(), event.header.stableId);
    auto copiedEvent = event;
    copiedEvent.header.stableId = copy->runtimeId();
    copiedEvent.objectGuid = copy->guid;
    EXPECT_EQ(capture->matchingProjectObject(copiedEvent), copy->objectID);
    ASSERT_TRUE(manager.deleteItem(variable));
    EXPECT_EQ(capture->matchingProjectObject(event), 0u);
    EXPECT_EQ(capture->matchingProjectObject(copiedEvent), copy->objectID);
    copiedEvent.projectGuid = QUuid::createUuid();
    EXPECT_EQ(capture->matchingProjectObject(copiedEvent), 0u);
}

TEST(StandaloneEditorIpcEventTest,
     AuditionSeekFinishedBroadcastsExactTerminalEventOnce)
{
    ensureApp();
    auto& projects = VNX::ProjectManager::instance();
    if (projects.isProjectOpen())
        projects.closeProject();
    VNX::ActionManager::instance().clearAllForTesting();

    QTcpSocket socket;
    auto window = std::make_unique<VNX::MainWindow>();
    const auto cleanup = qScopeGuard([&]() {
        socket.abort();
        VNX::UndoRedoManager::instance().clear();
        if (projects.isProjectOpen())
            projects.closeProject();
        QObject::disconnect(&VNX::UndoRedoManager::instance(), nullptr, window.get(), nullptr);
        VNX::ToastManager::instance().setParentWidget(nullptr);
        window.reset();
        VNX::ActionManager::instance().setTransactionCallbacks({});
        VNX::ActionManager::instance().clearAllForTesting();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });

    auto* server = window->ipcServer();
    ASSERT_NE(server, nullptr);
    server->stop();
    ASSERT_TRUE(server->start(0));
    ASSERT_GT(server->port(), 0);

    socket.connectToHost(QHostAddress::LocalHost,
                         static_cast<quint16>(server->port()));
    ASSERT_TRUE(VNX::TestSupport::spinUntil([&]() {
        return socket.state() == QAbstractSocket::ConnectedState
            && server->clientCount() == 1;
    }));

    auto* audition = window->auditionManager();
    ASSERT_NE(audition, nullptr);
    struct TerminalCase
    {
        quint64 requestId;
        int32_t engineResult;
        bool applied;
        QString state;
    };
    const std::array<TerminalCase, 2> cases{{
        {9007199254740993ULL, HarkenEditor::Bridge::kEngineOk, true,
         QStringLiteral("applied")},
        {9007199254740995ULL,
         static_cast<int32_t>(HarkenEditor::Bridge::kEngineStaleHandle), false,
         QStringLiteral("cancelled")},
    }};

    for (const auto& terminal : cases) {
        audition->auditionSeekFinished(
            42, terminal.requestId, terminal.engineResult, terminal.applied);
        ASSERT_TRUE(VNX::TestSupport::spinUntil(
            [&]() { return socket.canReadLine(); }));

        QJsonParseError error;
        const QJsonDocument message = QJsonDocument::fromJson(
            socket.readLine().trimmed(), &error);
        ASSERT_EQ(error.error, QJsonParseError::NoError);
        ASSERT_TRUE(message.isObject());
        const QJsonObject event = message.object();
        EXPECT_EQ(event.value(QStringLiteral("type")).toString(),
                  QStringLiteral("event"));
        EXPECT_EQ(event.value(QStringLiteral("name")).toString(),
                  QStringLiteral("audition_seek_finished"));
        const QJsonObject data = event.value(QStringLiteral("data")).toObject();
        EXPECT_EQ(data.value(QStringLiteral("auditionID")).toInt(), 42);
        EXPECT_EQ(data.value(QStringLiteral("audition_seek_request_id")).toString(),
                  QString::number(terminal.requestId));
        EXPECT_EQ(data.value(QStringLiteral("engine_result")).toInt(),
                  terminal.engineResult);
        EXPECT_EQ(data.value(QStringLiteral("state")).toString(),
                  terminal.state);
        EXPECT_FALSE(VNX::TestSupport::spinUntil(
            [&]() { return socket.canReadLine(); }, 50));
    }
}
