#include "Core/ActionManager.h"
#include "Core/AuditionManager.h"
#include "Core/IPC/IPCServer.h"
#include "Core/LayoutController.h"
#include "Core/SelectionRouter.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include "Components/VTrack.h"
#include "MainWindow.h"
#include "Monitor/EventCaptureController.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/Blueprint/NodeContentEditor.h"
#include "Widgets/GridEditor/GridEditorPanel.h"
#include "Widgets/GridEditor/DiscreteGridWidget.h"
#include "Widgets/GridEditor/SelectionListWidget.h"
#include "Widgets/ControlPanel/ControlPanel.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/ProjectExplorer/ProjectExplorer.h"
#include "Widgets/Timeline/TimelineCanvas.h"
#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <DockWidget.h>

#include <QApplication>
#include <QEventLoop>
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QAbstractSocket>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTcpSocket>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <memory>

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
        QEventLoop startup;
        bool presentationFinished = false;
        bool presentationShown = false;
        QObject::connect(window_.get(), &VNX::MainWindow::startupPresentationFinished,
                         &startup, [&](bool shown) {
            presentationFinished = true;
            presentationShown = shown;
            startup.quit();
        });
        window_->show();
        if (!presentationFinished) {
            QTimer::singleShot(3000, &startup, &QEventLoop::quit);
            startup.exec();
        }
        ASSERT_TRUE(presentationFinished);
        ASSERT_TRUE(presentationShown);
        window_->showNormal();
        window_->resize(1800, 1000);
        drainEvents();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        if (window_)
            QObject::disconnect(&VNX::UndoRedoManager::instance(), nullptr, window_.get(), nullptr);
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

    static void clickWidget(QWidget* widget, const QPoint& position,
                            Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QApplication::setActiveWindow(widget->window());
        QMouseEvent press(QEvent::MouseButtonPress, position, widget->mapToGlobal(position),
                          Qt::LeftButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(widget, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, position, widget->mapToGlobal(position),
                            Qt::LeftButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(widget, &release);
        drainEvents();
    }

    static void selectChildRow(VNX::GridEditorPanel* panel, int index)
    {
        auto* list = panel->findChild<VNX::SelectionListWidget*>();
        if (list && list->isVisible()) {
            for (auto* row : list->findChildren<QWidget*>(QStringLiteral("selectionChildRow"))) {
                if (row->property("rowIndex").toInt() != index) continue;
                auto* handle = row->findChild<QWidget*>(QStringLiteral("selectionChildDragHandle"));
                ASSERT_NE(handle, nullptr);
                clickWidget(handle, handle->rect().center());
                return;
            }
            FAIL() << "Selection row is missing";
        } else {
            auto* table = panel->discreteGrid()->table();
            ASSERT_NE(table->item(index, 0), nullptr);
            const QRect cell = table->visualItemRect(table->item(index, 0));
            ASSERT_TRUE(table->viewport()->rect().contains(cell.center()));
            clickWidget(table->viewport(), cell.center());
        }
    }

    QTemporaryDir directory_;
    std::unique_ptr<VNX::MainWindow> window_;
    VNX::LayoutController* controller_ = nullptr;
};

}

TEST_F(EditorDockLayoutTest, ColorRefreshPreservesBlueprintEditorsAndStructureRefreshReplacesThem)
{
    auto& project = VNX::ProjectManager::instance();
    ASSERT_NE(project.createItem(nullptr, QStringLiteral("ColorPreviewNode"), VNX::ProjectItem::Type::Sound), nullptr);
    auto* blueprint = window_->findChild<VNX::BlueprintCanvasWidget*>();
    ASSERT_NE(blueprint, nullptr);
    blueprint->setSoundsFiltered(false);
    blueprint->rebuildFromProject();
    drainEvents();
    drainEvents();

    QList<QPointer<VNX::NodeContentEditor>> editors;
    for (auto* widget : QApplication::allWidgets()) {
        auto* editor = qobject_cast<VNX::NodeContentEditor*>(widget);
        if (editor && editor->isHidden() && editor->parentWidget()
            && !editor->parentWidget()->parentWidget()) editors.append(editor);
    }
    ASSERT_FALSE(editors.isEmpty());

    VNX::NodeContentRenderer renderer;
    VNX::NodeContentInfo content;
    content.volume = -3.0f;
    const auto initialDrawing = renderer.presentation(content, content.fields(), std::nullopt, true);
    const auto image = [](const VNX::NodeContentPresentation& presentation) {
        QImage result(presentation.size, QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        painter.drawPicture(0, 0, presentation.drawing);
        return result;
    };
    const auto initialImage = image(initialDrawing);

    const auto originalPalette = blueprint->viewport()->palette();
    auto palette = originalPalette;
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#326475")));
    blueprint->viewport()->setPalette(palette);
    drainEvents();
    for (const auto& editor : editors) EXPECT_FALSE(editor.isNull());

    auto& theme = VNX::ThemeManager::instance();
    const auto restore = qScopeGuard([&] {
        theme.cancelAppearancePreview();
        blueprint->viewport()->setPalette(originalPalette);
        drainEvents();
    });
    auto draft = theme.appearance();
    draft.colors[draft.themeId][draft.paletteId].insert(QStringLiteral("background.primary"), QColor(QStringLiteral("#293746")));
    draft.colors[draft.themeId][draft.paletteId].insert(QStringLiteral("text.primary"), QColor(QStringLiteral("#ff3366")));
    ASSERT_TRUE(theme.previewAppearance(draft).ok());
    drainEvents();
    for (const auto& editor : editors) EXPECT_FALSE(editor.isNull());
    renderer.clearDrawings();
    const auto colorDrawing = renderer.presentation(content, content.fields(), std::nullopt, true);
    EXPECT_EQ(colorDrawing.size, initialDrawing.size);
    EXPECT_EQ(colorDrawing.fields, initialDrawing.fields);
    EXPECT_NE(image(colorDrawing), initialImage);
    content.volume = -12.0f;
    const auto valueDrawing = renderer.presentation(content, content.fields(), std::nullopt, true);
    EXPECT_NE(image(valueDrawing), image(colorDrawing));

    QEvent styleChange(QEvent::StyleChange);
    QApplication::sendEvent(blueprint->viewport(), &styleChange);
    drainEvents();
    for (const auto& editor : editors) EXPECT_TRUE(editor.isNull());
}

TEST_F(EditorDockLayoutTest, LocalBlueprintLayoutPreservesProjectSavePointsAcrossUndoAndReopen)
{
    auto& manager = VNX::ProjectManager::instance();
    auto& undo = VNX::UndoRedoManager::instance();
    auto* sound = manager.createItem(nullptr, QStringLiteral("LayoutNode"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    const auto guid = sound->guid;
    const auto id = sound->objectID;
    ASSERT_TRUE(manager.saveProject());
    undo.clearHistory();
    const QString projectPath = manager.currentProject()->projectPath;
    const QString projectId = manager.currentProject()->projectId;
    const auto gridSnap = QSettings().value(QStringLiteral("Blueprint/GridSnapEnabled"));
    const auto cleanup = qScopeGuard([&] {
        QSettings settings;
        settings.remove(QStringLiteral("Blueprint/SignalChainLayout/Nodes/") + projectId);
        if (gridSnap.isValid()) settings.setValue(QStringLiteral("Blueprint/GridSnapEnabled"), gridSnap);
        else settings.remove(QStringLiteral("Blueprint/GridSnapEnabled"));
    });
    const auto documents = [&] {
        QMap<QString, QPair<QByteArray, QDateTime>> result;
        QDirIterator files(QFileInfo(projectPath).absolutePath(),
                           {QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                           QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile file(files.next());
            EXPECT_TRUE(file.open(QIODevice::ReadOnly));
            result.insert(file.fileName(), {file.readAll(), QFileInfo(file).lastModified()});
        }
        return result;
    };
    const auto saved = documents();
    auto canvas = std::make_unique<VNX::BlueprintCanvasWidget>();
    canvas->setGridSnapEnabled(false);
    canvas->useSignalChain(sound);
    VNX::NodeItem* node = nullptr;
    for (auto* item : canvas->scene()->items()) {
        auto* candidate = dynamic_cast<VNX::NodeItem*>(item);
        if (candidate && candidate->objectID() == id) node = candidate;
    }
    ASSERT_NE(node, nullptr);
    const auto original = node->pos();
    const auto moved = original + QPointF(96, 64);
    QGraphicsSceneMouseEvent press(QEvent::GraphicsSceneMousePress);
    press.setButton(Qt::LeftButton);
    press.setButtons(Qt::LeftButton);
    canvas->scene()->sendEvent(node, &press);
    node->setPos(moved);
    QGraphicsSceneMouseEvent release(QEvent::GraphicsSceneMouseRelease);
    release.setButton(Qt::LeftButton);
    canvas->scene()->sendEvent(node, &release);
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), moved);
    EXPECT_TRUE(undo.isClean());
    EXPECT_FALSE(sound->isDirtyObject());
    ASSERT_TRUE(undo.undo());
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), original);
    EXPECT_TRUE(undo.isClean());
    ASSERT_TRUE(undo.redo());
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), moved);
    EXPECT_TRUE(undo.isClean());

    ASSERT_TRUE(manager.renameItem(sound, QStringLiteral("RenamedNode")));
    EXPECT_FALSE(undo.isClean());
    EXPECT_TRUE(sound->isDirtyObject());
    canvas->resetNodeLayout();
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), original);
    EXPECT_FALSE(undo.isClean());
    ASSERT_TRUE(undo.undo());
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), moved);
    EXPECT_FALSE(undo.isClean());
    ASSERT_TRUE(undo.undo());
    drainEvents();
    EXPECT_TRUE(undo.isClean());
    EXPECT_FALSE(sound->isDirtyObject());
    canvas->resetNodeLayout();
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), original);
    EXPECT_TRUE(undo.isClean());
    ASSERT_TRUE(undo.undo());
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), moved);
    EXPECT_TRUE(undo.isClean());
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(documents(), saved);

    undo.clearHistory();
    canvas = std::make_unique<VNX::BlueprintCanvasWidget>();
    canvas->useSignalChain(sound);
    EXPECT_EQ(canvas->nodePosition(id), moved);
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    ASSERT_TRUE(VNX::TestSupport::spinUntil([&] { return manager.findItemByGuid(guid) != nullptr; }));
    sound = manager.findItemByGuid(guid);
    ASSERT_NE(sound, nullptr);
    canvas->useSignalChain(sound);
    EXPECT_EQ(canvas->nodePosition(sound->objectID), moved);
    EXPECT_TRUE(undo.isClean());
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(documents(), saved);
}

TEST_F(EditorDockLayoutTest, AuditionTargetInvalidatesWithDeletedSubtreesAndProjectClosure)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* parent = manager.createItem(nullptr, QStringLiteral("AuditionRoot"), VNX::ProjectItem::Type::Container);
    auto* nested = manager.createItem(parent, QStringLiteral("NestedAudition"), VNX::ProjectItem::Type::Container);
    auto* child = manager.createItem(nested, QStringLiteral("NestedChild"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(child, nullptr);
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    router->onProjectItemDoubleClicked(parent);
    drainEvents();
    auto* panel = qobject_cast<VNX::GridEditorPanel*>(
        window_->activeStandaloneEditorPanel(QStringLiteral("GridEditor")));
    ASSERT_NE(panel, nullptr);
    panel->drillInto(nested->guid);
    drainEvents();
    selectChildRow(panel, 0);
    ASSERT_EQ(window_->auditionTarget(), child);
    ASSERT_TRUE(manager.deleteItem(nested));
    drainEvents();
    EXPECT_EQ(window_->auditionTarget(), nullptr);

    router->selectItem(parent, false);
    ASSERT_EQ(window_->auditionTarget(), parent);
    ASSERT_TRUE(manager.closeProject());
    drainEvents();
    EXPECT_EQ(window_->auditionTarget(), nullptr);
    EXPECT_EQ(window_->findChild<ControlPanel*>()->boundItem(), nullptr);
}

TEST_F(EditorDockLayoutTest, NavigationViewsKeepLocalSelectionsAcrossActiveObjectChanges)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* first = window_->createHierarchyPanel();
    auto* second = window_->createHierarchyPanel();
    auto* explorer = window_->findChild<VNX::ProjectExplorer*>();
    auto* inspector = window_->createInspectorPanel();
    auto* blueprint = window_->findChild<VNX::BlueprintCanvasWidget*>();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(inspector, nullptr);
    ASSERT_NE(blueprint, nullptr);
    blueprint->setFixedSize(640, 480);
    window_->setLastFocusedInspectorPanelId(inspector->panelId());
    auto* firstItem = manager.createItem(nullptr, QStringLiteral("FirstSelection"), VNX::ProjectItem::Type::Sound);
    auto* secondItem = manager.createItem(nullptr, QStringLiteral("SecondSelection"), VNX::ProjectItem::Type::Sound);
    auto* explorerItem = manager.createItem(nullptr, QStringLiteral("ExplorerSelection"), VNX::ProjectItem::Type::Sound);
    auto* bus = manager.createItem(manager.getMasterBusItem(), QStringLiteral("NavigationBus"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(firstItem, nullptr);
    ASSERT_NE(secondItem, nullptr);
    ASSERT_NE(explorerItem, nullptr);
    ASSERT_NE(bus, nullptr);
    blueprint->setSoundsFiltered(false);
    blueprint->rebuildFromProject();
    blueprint->setBusVisible(bus->objectID, true);
    drainEvents();

    ASSERT_TRUE(second->selectProjectItem(secondItem));
    ASSERT_TRUE(explorer->selectProjectItem(explorerItem));
    blueprint->setZoomFactor(0.75);
    blueprint->centerOn(QPointF(1200, 800));
    drainEvents();
    const QTransform initialViewport = blueprint->viewportTransform();
    ASSERT_TRUE(first->selectProjectItem(firstItem));
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<VNX::ProjectItem*>{firstItem});
    EXPECT_EQ(second->selectedItems(), QList<VNX::ProjectItem*>{secondItem});
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->viewportTransform(), initialViewport);
    EXPECT_EQ(inspector->currentContextItem(), firstItem);
    EXPECT_EQ(window_->auditionTarget(), firstItem);

    ASSERT_TRUE(first->selectProjectItem(bus));
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<VNX::ProjectItem*>{bus});
    EXPECT_EQ(second->selectedItems(), QList<VNX::ProjectItem*>{secondItem});
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->viewportTransform(), initialViewport);
    EXPECT_EQ(inspector->currentContextItem(), bus);
    EXPECT_EQ(window_->auditionTarget(), bus);
    ASSERT_TRUE(first->selectProjectItem(firstItem));

    ASSERT_TRUE(explorer->selectProjectItem(secondItem));
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<VNX::ProjectItem*>{firstItem});
    EXPECT_EQ(second->selectedItems(), QList<VNX::ProjectItem*>{secondItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{secondItem});
    EXPECT_EQ(blueprint->viewportTransform(), initialViewport);
    EXPECT_EQ(inspector->currentContextItem(), secondItem);
    EXPECT_EQ(window_->auditionTarget(), secondItem);

    ASSERT_TRUE(blueprint->focusItem(explorerItem));
    ASSERT_FALSE(blueprint->scene()->selectedItems().isEmpty());
    auto* selectedNode = blueprint->scene()->selectedItems().front();
    const QTransform selectedViewport = blueprint->viewportTransform();
    blueprint->scene()->clearSelection();
    selectedNode->setSelected(true);
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<VNX::ProjectItem*>{firstItem});
    EXPECT_EQ(second->selectedItems(), QList<VNX::ProjectItem*>{secondItem});
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->scene()->selectedItems(), QList<QGraphicsItem*>{selectedNode});
    EXPECT_EQ(blueprint->viewportTransform(), selectedViewport);
    EXPECT_EQ(inspector->currentContextItem(), explorerItem);
    EXPECT_EQ(window_->auditionTarget(), explorerItem);

    for (auto* panel : window_->allInspectorPanels())
        window_->closeInspectorPanel(panel->panelId());
    drainEvents();
    ASSERT_EQ(window_->activeInspectorPanel(), nullptr);
    ASSERT_EQ(window_->inspectorShowingItem(firstItem, true), nullptr);
    auto* tree = first->treeWidget();
    window_->dockOfPanel(first)->setAsCurrentTab();
    tree->scrollToItem(tree->currentItem());
    drainEvents();
    blueprint->setZoomFactor(0.75);
    blueprint->centerOn(QPointF(1200, 800));
    const QTransform hierarchyViewport = blueprint->viewportTransform();
    const QPoint position = tree->visualItemRect(tree->currentItem()).center();
    ASSERT_TRUE(tree->viewport()->rect().contains(position));
    clickWidget(tree->viewport(), position);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, position,
                            tree->viewport()->mapToGlobal(position),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &doubleClick);
    QMouseEvent release(QEvent::MouseButtonRelease, position,
                        tree->viewport()->mapToGlobal(position),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &release);
    drainEvents();
    ASSERT_NE(window_->inspectorShowingItem(firstItem), nullptr);
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{explorerItem});
    EXPECT_EQ(second->selectedItems(), QList<VNX::ProjectItem*>{secondItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->viewportTransform(), hierarchyViewport);
    EXPECT_EQ(window_->auditionTarget(), firstItem);

    const auto inspectorsBefore = window_->allInspectorPanels();
    first->openInNewWindowRequested(firstItem);
    drainEvents();
    EXPECT_GT(window_->allInspectorPanels().size(), inspectorsBefore.size());
    ASSERT_NE(window_->activeInspectorPanel(), nullptr);
    EXPECT_EQ(window_->activeInspectorPanel()->currentContextItem(), firstItem);
    EXPECT_EQ(first->selectedItems(), QList<VNX::ProjectItem*>{firstItem});
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{explorerItem});
    EXPECT_EQ(second->selectedItems(), QList<VNX::ProjectItem*>{secondItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->viewportTransform(), hierarchyViewport);
    EXPECT_EQ(window_->auditionTarget(), firstItem);

    auto* explorerTree = explorer->treeWidget();
    window_->dockOfPanel(explorer)->setAsCurrentTab();
    explorerTree->scrollToItem(explorerTree->currentItem());
    drainEvents();
    const QPoint explorerPosition = explorerTree->visualItemRect(explorerTree->currentItem()).center();
    ASSERT_TRUE(explorerTree->viewport()->rect().contains(explorerPosition));
    clickWidget(explorerTree->viewport(), explorerPosition);
    QMouseEvent explorerDoubleClick(QEvent::MouseButtonDblClick, explorerPosition,
                                    explorerTree->viewport()->mapToGlobal(explorerPosition),
                                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(explorerTree->viewport(), &explorerDoubleClick);
    QMouseEvent explorerRelease(QEvent::MouseButtonRelease, explorerPosition,
                                explorerTree->viewport()->mapToGlobal(explorerPosition),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(explorerTree->viewport(), &explorerRelease);
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<VNX::ProjectItem*>{firstItem});
    EXPECT_EQ(second->selectedItems(), QList<VNX::ProjectItem*>{secondItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{explorerItem});
    EXPECT_DOUBLE_EQ(blueprint->transform().m11(), 1.0);
    EXPECT_NE(blueprint->viewportTransform(), hierarchyViewport);

    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    QObject activationObserver;
    int activationCount = 0;
    QObject::connect(router, &VNX::SelectionRouter::selectionChanged, &activationObserver,
                     [&](VNX::ProjectItem*) { ++activationCount; });
    for (auto* target : {explorerItem, bus}) {
        ASSERT_TRUE(explorer->selectProjectItem(target));
        window_->dockOfPanel(blueprint)->setAsCurrentTab();
        ASSERT_TRUE(blueprint->focusItem(target));
        ASSERT_TRUE(first->selectProjectItem(firstItem));
        drainEvents();
        ASSERT_EQ(router->activeItem(), firstItem);
        auto* currentInspector = window_->activeInspectorPanel();
        ASSERT_NE(currentInspector, nullptr);
        ASSERT_EQ(currentInspector->currentContextItem(), firstItem);
        ASSERT_EQ(blueprint->scene()->selectedItems().size(), 1);
        auto* node = dynamic_cast<VNX::NodeItem*>(blueprint->scene()->selectedItems().front());
        ASSERT_NE(node, nullptr);
        ASSERT_EQ(node->objectID(), target->objectID);
        const QPoint nodePosition = blueprint->mapFromScene(node->mapToScene(node->titleRect().center()));
        ASSERT_TRUE(blueprint->viewport()->rect().contains(nodePosition));
        ASSERT_EQ(blueprint->itemAt(nodePosition), node);
        activationCount = 0;

        clickWidget(blueprint->viewport(), nodePosition);

        EXPECT_EQ(activationCount, 1);
        EXPECT_EQ(router->activeItem(), target);
        EXPECT_EQ(currentInspector->currentContextItem(), target);
        EXPECT_EQ(window_->auditionTarget(), target);
        EXPECT_EQ(first->selectedItems(), QList<VNX::ProjectItem*>{firstItem});
        EXPECT_EQ(second->selectedItems(), QList<VNX::ProjectItem*>{secondItem});
        EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{target});
        EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{target});
    }
}

TEST_F(EditorDockLayoutTest, BackgroundProjectChangesPreserveActiveContextUntilItsObjectIsDeleted)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* first = window_->createHierarchyPanel();
    auto* second = window_->createHierarchyPanel();
    auto* explorer = window_->findChild<VNX::ProjectExplorer*>();
    auto* inspector = window_->createInspectorPanel();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(inspector, nullptr);
    window_->setLastFocusedInspectorPanelId(inspector->panelId());
    auto* parent = manager.createItem(nullptr, QStringLiteral("InactiveBranch"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(parent, nullptr);
    auto* inactive = manager.createItem(parent, QStringLiteral("InactiveSelection"), VNX::ProjectItem::Type::Sound);
    auto* active = manager.createItem(nullptr, QStringLiteral("ActiveSelection"), VNX::ProjectItem::Type::Sound);
    auto* parked = manager.createItem(nullptr, QStringLiteral("ParkedExplorerSelection"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(inactive, nullptr);
    ASSERT_NE(active, nullptr);
    ASSERT_NE(parked, nullptr);
    ASSERT_TRUE(explorer->selectProjectItem(parked));
    ASSERT_TRUE(first->selectProjectItem(inactive));
    ASSERT_TRUE(second->selectProjectItem(active));

    ASSERT_NE(manager.createItem(nullptr, QStringLiteral("BackgroundCreation"), VNX::ProjectItem::Type::Sound), nullptr);
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<VNX::ProjectItem*>{inactive});
    EXPECT_EQ(second->selectedItems(), QList<VNX::ProjectItem*>{active});
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(inspector->currentContextItem(), active);
    EXPECT_EQ(window_->auditionTarget(), active);

    ASSERT_TRUE(manager.deleteItem(parent));
    drainEvents();
    EXPECT_TRUE(first->selectedItems().isEmpty());
    EXPECT_EQ(second->selectedItems(), QList<VNX::ProjectItem*>{active});
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(inspector->currentContextItem(), active);
    EXPECT_EQ(window_->auditionTarget(), active);

    ASSERT_TRUE(manager.deleteItem(active));
    drainEvents();
    EXPECT_TRUE(second->selectedItems().isEmpty());
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(inspector->currentContextItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);
    EXPECT_EQ(window_->findChild<ControlPanel*>()->boundItem(), nullptr);
}

TEST_F(EditorDockLayoutTest, ActiveObjectChangesFollowExplicitNavigationInput)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* hierarchy = window_->createHierarchyPanel();
    auto* otherHierarchy = window_->createHierarchyPanel();
    auto* explorer = window_->findChild<VNX::ProjectExplorer*>();
    auto* blueprint = window_->findChild<VNX::BlueprintCanvasWidget*>();
    auto* inspector = window_->createInspectorPanel();
    ASSERT_NE(hierarchy, nullptr);
    ASSERT_NE(otherHierarchy, nullptr);
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(blueprint, nullptr);
    ASSERT_NE(inspector, nullptr);
    blueprint->setFixedSize(640, 480);
    window_->setLastFocusedInspectorPanelId(inspector->panelId());
    auto* parent = manager.createItem(nullptr, QStringLiteral("InputBranch"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(parent, nullptr);
    auto* first = manager.createItem(parent, QStringLiteral("FirstInput"), VNX::ProjectItem::Type::Sound);
    auto* second = manager.createItem(parent, QStringLiteral("SecondInput"), VNX::ProjectItem::Type::Sound);
    auto* parked = manager.createItem(nullptr, QStringLiteral("ParkedInput"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(parked, nullptr);
    blueprint->setSoundsFiltered(false);
    blueprint->rebuildFromProject();
    drainEvents();
    auto* tree = hierarchy->treeWidget();
    ASSERT_TRUE(hierarchy->selectProjectItem(first));
    auto* firstRow = tree->currentItem();
    ASSERT_TRUE(hierarchy->selectProjectItem(second));
    ASSERT_TRUE(explorer->selectProjectItem(parked));
    window_->dockOfPanel(hierarchy)->setAsCurrentTab();
    drainEvents();
    tree->scrollToItem(firstRow);
    tree->setFocus(Qt::OtherFocusReason);
    drainEvents();
    const QPoint firstPosition = tree->visualItemRect(firstRow).center();
    ASSERT_TRUE(tree->viewport()->rect().contains(firstPosition));
    blueprint->setZoomFactor(0.75);
    blueprint->centerOn(QPointF(1200, 800));
    const QTransform blueprintViewport = blueprint->viewportTransform();

    clickWidget(tree->viewport(), firstPosition);
    EXPECT_EQ(inspector->currentContextItem(), first);
    EXPECT_EQ(window_->auditionTarget(), first);
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);

    QKeyEvent downPress(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(tree, &downPress);
    QKeyEvent downRelease(QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(tree, &downRelease);
    drainEvents();
    ASSERT_EQ(hierarchy->selectedItem(), second);
    EXPECT_EQ(inspector->currentContextItem(), second);
    EXPECT_EQ(window_->auditionTarget(), second);
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);

    const QPoint rightPosition = tree->visualItemRect(firstRow).center();
    QMouseEvent rightPress(QEvent::MouseButtonPress, rightPosition,
                           tree->viewport()->mapToGlobal(rightPosition),
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &rightPress);
    QMouseEvent rightRelease(QEvent::MouseButtonRelease, rightPosition,
                             tree->viewport()->mapToGlobal(rightPosition),
                             Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &rightRelease);
    drainEvents();
    ASSERT_EQ(hierarchy->selectedItem(), first);
    EXPECT_EQ(inspector->currentContextItem(), second);
    EXPECT_EQ(window_->auditionTarget(), second);
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);

    clickWidget(tree->viewport(), tree->visualItemRect(firstRow).center());
    EXPECT_EQ(inspector->currentContextItem(), first);
    EXPECT_EQ(window_->auditionTarget(), first);
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);

    ASSERT_TRUE(otherHierarchy->selectProjectItem(second));
    ASSERT_EQ(hierarchy->selectedItem(), first);
    clickWidget(tree->viewport(), tree->visualItemRect(firstRow).center());
    EXPECT_EQ(inspector->currentContextItem(), first);
    EXPECT_EQ(window_->auditionTarget(), first);
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(tree, &escape);
    drainEvents();
    EXPECT_TRUE(hierarchy->selectedItems().isEmpty());
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(otherHierarchy->selectedItems(), QList<VNX::ProjectItem*>{second});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);
    EXPECT_EQ(inspector->currentContextItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);
}

TEST_F(EditorDockLayoutTest, DeletedActiveObjectDoesNotClearIndependentEditorAndAuditionContexts)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* active = manager.createItem(nullptr, QStringLiteral("ActiveObject"), VNX::ProjectItem::Type::Sound);
    auto* inspected = manager.createItem(nullptr, QStringLiteral("IndependentInspector"), VNX::ProjectItem::Type::Sound);
    auto* container = manager.createItem(nullptr, QStringLiteral("IndependentAudition"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(active, nullptr);
    ASSERT_NE(inspected, nullptr);
    ASSERT_NE(container, nullptr);
    auto* auditioned = manager.createItem(container, QStringLiteral("AuditionChild"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(auditioned, nullptr);
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    router->selectItem(active, false);
    window_->openItemInNewInspector(inspected, VNX::MainWindow::PreferredOpenArea::CentralTab, false);
    auto* inspector = window_->inspectorShowingItem(inspected);
    ASSERT_NE(inspector, nullptr);
    auto* editor = qobject_cast<VNX::GridEditorPanel*>(
        window_->openStandaloneEditor(QStringLiteral("GridEditor"), container));
    ASSERT_NE(editor, nullptr);
    drainEvents();
    selectChildRow(editor, 0);
    ASSERT_EQ(router->activeItem(), active);
    ASSERT_EQ(inspector->currentContextItem(), inspected);
    ASSERT_EQ(window_->auditionTarget(), auditioned);

    ASSERT_TRUE(manager.deleteItem(active));
    drainEvents();
    EXPECT_EQ(router->activeItem(), nullptr);
    EXPECT_EQ(inspector->currentContextItem(), inspected);
    EXPECT_EQ(editor->boundItem(), container);
    EXPECT_EQ(window_->auditionTarget(), auditioned);
    EXPECT_EQ(window_->findChild<ControlPanel*>()->boundItem(), auditioned);
}

TEST_F(EditorDockLayoutTest, UndoRedoRestoresObjectsWithoutRestoringActiveContexts)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* inspected = manager.createItem(nullptr, QStringLiteral("IndependentInspector"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(inspected, nullptr);
    window_->openItemInNewInspector(inspected, VNX::MainWindow::PreferredOpenArea::CentralTab, false);
    auto* inspector = window_->inspectorShowingItem(inspected);
    ASSERT_NE(inspector, nullptr);
    inspector->setPinned(true);
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    auto& undo = VNX::UndoRedoManager::instance();
    undo.clear();
    const auto created = VNX::ActionManager::instance().execute(
        QStringLiteral("create_attenuation"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("ActiveAttenuation")}});
    ASSERT_TRUE(created.success) << created.errorMessage.toStdString();
    const QUuid guid(created.data.value(QStringLiteral("guid")).toString());
    ASSERT_FALSE(guid.isNull());
    auto* active = manager.findItemByGuid(guid);
    ASSERT_NE(active, nullptr);
    router->selectItem(active, false);
    drainEvents();
    ASSERT_EQ(router->activeItem(), active);
    ASSERT_EQ(window_->auditionTarget(), active);
    ASSERT_EQ(inspector->currentContextItem(), inspected);

    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(manager.findItemByGuid(guid), nullptr);
    EXPECT_EQ(router->activeItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);
    EXPECT_EQ(inspector->currentContextItem(), inspected);

    ASSERT_TRUE(undo.redo());
    ASSERT_NE(manager.findItemByGuid(guid), nullptr);
    EXPECT_EQ(router->activeItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);
    EXPECT_EQ(inspector->currentContextItem(), inspected);
    drainEvents();
    EXPECT_EQ(router->activeItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);

    inspector->setPinned(false);
    drainEvents();
    EXPECT_EQ(router->activeItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);
    EXPECT_EQ(inspector->currentContextItem(), inspected);
    EXPECT_EQ(window_->findChild<ControlPanel*>()->boundItem(), nullptr);
}

TEST_F(EditorDockLayoutTest, LocalTimelineSelectionPreservesNavigationUntilExplicitInspection)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* segment = manager.createItem(nullptr, QStringLiteral("ClipSegment"), VNX::ProjectItem::Type::Segment);
    ASSERT_NE(segment, nullptr);
    auto* track = manager.createItem(segment, QStringLiteral("ClipTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    auto* sound = manager.createItem(track, QStringLiteral("ClipTarget"), VNX::ProjectItem::Type::Sound);
    auto* active = manager.createItem(nullptr, QStringLiteral("ActiveOutsideTimeline"), VNX::ProjectItem::Type::Sound);
    auto* parked = manager.createItem(nullptr, QStringLiteral("ParkedOutsideTimeline"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(active, nullptr);
    ASSERT_NE(parked, nullptr);
    track->comp<VNX::VTrack>()->clips = {};
    auto* hierarchy = window_->createHierarchyPanel();
    auto* explorer = window_->findChild<VNX::ProjectExplorer*>();
    auto* inspector = window_->createInspectorPanel();
    auto* router = window_->findChild<VNX::SelectionRouter*>();
    ASSERT_NE(hierarchy, nullptr);
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(inspector, nullptr);
    ASSERT_NE(router, nullptr);
    window_->setLastFocusedInspectorPanelId(inspector->panelId());
    auto* timeline = qobject_cast<VNX::TimelineEditorPanel*>(
        window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), track));
    ASSERT_NE(timeline, nullptr);
    ASSERT_TRUE(timeline->placeItem(sound, 0, 0, true));
    ASSERT_EQ(timeline->clipCount(0), 1);
    const QUuid clip = timeline->clipGuid(0, 0);
    ASSERT_TRUE(timeline->selectLane(track->guid));
    timeline->setEditCursorVisible(false);
    timeline->fitClips();
    ASSERT_TRUE(explorer->selectProjectItem(parked));
    ASSERT_TRUE(hierarchy->selectProjectItem(active));
    window_->dockOfPanel(timeline)->setAsCurrentTab();
    drainEvents();
    auto* canvas = timeline->findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    canvas->repaint();
    ASSERT_EQ(canvas->lastRenderData().clipItems.size(), 1);
    const QRect visibleClip = canvas->lastRenderData().clipItems.front().rect.intersected(canvas->rect());
    ASSERT_GT(visibleClip.width(), 10);
    ASSERT_GT(visibleClip.height(), 0);
    ASSERT_TRUE(timeline->selectedClipGuids().isEmpty());

    clickWidget(canvas, visibleClip.center());
    EXPECT_EQ(timeline->selectedClipGuids(), QSet<QUuid>{clip});
    EXPECT_EQ(router->activeItem(), active);
    EXPECT_EQ(inspector->currentContextItem(), active);
    EXPECT_EQ(window_->auditionTarget(), active);
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{parked});
    EXPECT_EQ(hierarchy->selectedItems(), QList<VNX::ProjectItem*>{active});

    ASSERT_TRUE(timeline->inspectClip(0, clip));
    drainEvents();
    EXPECT_EQ(router->activeItem(), sound);
    EXPECT_EQ(inspector->currentContextItem(), sound);
    EXPECT_EQ(window_->auditionTarget(), sound);
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{sound});
    EXPECT_EQ(hierarchy->selectedItems(), QList<VNX::ProjectItem*>{active});
    ASSERT_NE(inspector->placementContext(), nullptr);
    ASSERT_FALSE(inspector->placementContext()->empty());
    EXPECT_EQ(inspector->placementContext()->occurrences().front().clipGuid, clip);

    ASSERT_TRUE(hierarchy->selectProjectItem(active));
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, visibleClip.center(),
                            canvas->mapToGlobal(visibleClip.center()),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &doubleClick);
    drainEvents();
    EXPECT_EQ(router->activeItem(), sound);
    EXPECT_EQ(inspector->currentContextItem(), sound);
    EXPECT_EQ(window_->auditionTarget(), sound);
    EXPECT_EQ(explorer->selectedItems(), QList<VNX::ProjectItem*>{sound});
    EXPECT_EQ(hierarchy->selectedItems(), QList<VNX::ProjectItem*>{active});
    ASSERT_NE(inspector->placementContext(), nullptr);
    ASSERT_FALSE(inspector->placementContext()->empty());
    EXPECT_EQ(inspector->placementContext()->occurrences().front().clipGuid, clip);
}

TEST_F(EditorDockLayoutTest, EventIdentityIndexSurvivesObjectEdits)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* variable = manager.createItem(nullptr, QStringLiteral("Intensity"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
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
     AuditionSeekRequestIdentitySurvivesIpc)
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
    for (const quint64 requestId : {9007199254740993ULL, 9007199254740995ULL}) {
        audition->auditionSeekFinished(
            42, requestId, HarkenEditor::Bridge::kEngineOk, true);
        ASSERT_TRUE(VNX::TestSupport::spinUntil(
            [&]() { return socket.canReadLine(); }));

        QJsonParseError error;
        const QJsonDocument message = QJsonDocument::fromJson(
            socket.readLine().trimmed(), &error);
        ASSERT_EQ(error.error, QJsonParseError::NoError);
        ASSERT_TRUE(message.isObject());
        const QJsonObject event = message.object();
        const QJsonObject data = event.value(QStringLiteral("data")).toObject();
        EXPECT_EQ(data.value(QStringLiteral("audition_seek_request_id")).toString(),
                  QString::number(requestId));
    }
}
