#include "Core/ActionManager.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSelection.h"
#include "Core/Components/VTrack.h"
#include "Core/IPC/IPCServer.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/FontManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/ReferenceSlotResolution.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/TransitionSemantics.h"
#include "Core/ToastManager.h"
#include "Core/UndoCommands.h"
#include "MainWindow.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/Blueprint/BlueprintConnectCommand.h"
#include "Widgets/Blueprint/BlueprintEdgeKind.h"
#include "Widgets/Blueprint/BlueprintPanel.h"
#include "Widgets/Blueprint/SignalChainSource.h"
#include "Widgets/Blueprint/ConnectionPath.h"
#include "Widgets/Selector/SelectorWindow.h"

#include <gtest/gtest.h>
#include <DockWidget.h>

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QGraphicsScene>
#include <QImage>
#include <QLabel>
#include <QJsonDocument>
#include <QLayout>
#include <QLineEdit>
#include <QMargins>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QKeyEvent>
#include <QScopeGuard>
#include <QScrollArea>
#include <QString>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVariantAnimation>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace VNX {
uint32_t itemShortID(const ProjectItem* item);
}

namespace Slots = VNX::ReferenceSlotResolution;
using VNX::ProjectItem;
using T = VNX::ProjectItem::Type;

namespace {

constexpr std::array<bool, 3> kAllLayers{true, true, true};

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void drainEvents()
{
    QApplication::processEvents();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

class BlueprintOutGraphTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::SelectorWindow::dismiss();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::UndoRedoManager::instance().clear();
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("BlueprintOutGraphProject");
        config.location = tempDir.path();
        ASSERT_TRUE(pm.createProject(config));
    }

    void TearDown() override
    {
        VNX::SelectorWindow::dismiss();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::UndoRedoManager::instance().clear();
    }

    QTemporaryDir tempDir;
};

class LocationPingCanvas : public VNX::BlueprintCanvasWidget
{
public:
    VNX::NodeItem* nodeFor(const ProjectItem* item) const
    {
        for (auto* graphicsItem : scene()->items()) {
            auto* node = dynamic_cast<VNX::NodeItem*>(graphicsItem);
            if (node && node->objectID() == item->objectID) return node;
        }
        return nullptr;
    }

    void advancePing(qreal progress = 0.5)
    {
        auto* animation = findChild<QVariantAnimation*>(QStringLiteral("blueprintLocationPing"));
        ASSERT_NE(animation, nullptr);
        animation->setCurrentTime(qRound(animation->duration() * progress));
    }

    QImage foreground()
    {
        QImage image(viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setWorldTransform(viewportTransform());
        drawForeground(&painter, mapToScene(viewport()->rect()).boundingRect());
        return image;
    }
};

QRect paintedBounds(const QImage& image)
{
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0)
                bounds = bounds.united(QRect(x, y, 1, 1));
        }
    }
    return bounds;
}

TEST_F(BlueprintOutGraphTest, CanvasHidesNativeScrollbars)
{
    VNX::BlueprintCanvasWidget canvas;

    EXPECT_EQ(canvas.horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    EXPECT_EQ(canvas.verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
}

TEST_F(BlueprintOutGraphTest, MainWindowVisibilityMenuPreservesCanvasAndDoesNotScrollWholePanel)
{
    VNX::ActionManager::instance().clearAllForTesting();
    auto window = std::make_unique<VNX::MainWindow>();
    const auto cleanup = qScopeGuard([&]() {
        VNX::UndoRedoManager::instance().clear();
        VNX::ProjectManager::instance().closeProject();
        QObject::disconnect(&VNX::UndoRedoManager::instance(), nullptr, window.get(), nullptr);
        VNX::ToastManager::instance().setParentWidget(nullptr);
        window.reset();
        VNX::ActionManager::instance().setTransactionCallbacks({});
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::UndoRedoManager::instance().clear();
        drainEvents();
    });
    auto* dock = window->findChild<ads::CDockWidget*>(QStringLiteral("BlueprintCanvasDock"));
    ASSERT_NE(dock, nullptr);
    auto* panel = qobject_cast<VNX::BlueprintPanel*>(dock->widget());
    ASSERT_NE(panel, nullptr);
    for (auto* ancestor = panel->parentWidget(); ancestor && ancestor != dock; ancestor = ancestor->parentWidget())
        EXPECT_EQ(qobject_cast<QScrollArea*>(ancestor), nullptr);
    window->resize(1200, 600);
    window->show();
    window->revealDock(dock);
    drainEvents();
    auto* canvas = panel->canvas();
    auto* filter = panel->findChild<QToolButton*>(QStringLiteral("BlueprintViewportVisibilityButton"));
    auto* drawer = panel->findChild<QWidget*>(QStringLiteral("BlueprintViewportVisibilityMenu"));
    auto* status = panel->findChild<QWidget*>(QStringLiteral("BlueprintStatusBar"));
    auto* toolbar = panel->findChild<QWidget*>(QStringLiteral("BlueprintToolBar"));
    auto* meter = panel->findChild<QWidget*>(QStringLiteral("BlueprintPeakMeter"));
    auto* expand = panel->findChild<QToolButton*>(QStringLiteral("meterExpandButton"));
    auto* waveform = panel->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Waveform"));
    ASSERT_NE(filter, nullptr);
    ASSERT_NE(drawer, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(meter, nullptr);
    ASSERT_NE(expand, nullptr);
    ASSERT_NE(waveform, nullptr);
    ASSERT_TRUE(panel->isVisible());
    const auto canvasGeometry = canvas->geometry();
    const auto statusGeometry = status->geometry();
    const auto sceneOrigin = canvas->mapToScene(QPoint());
    const auto transform = canvas->transform();
    for (auto* button : {filter, expand, filter, filter, expand, filter}) {
        button->click();
        drainEvents();
        EXPECT_EQ(canvas->geometry(), canvasGeometry);
        EXPECT_EQ(canvas->mapToScene(QPoint()), sceneOrigin);
        EXPECT_EQ(canvas->transform(), transform);
        EXPECT_EQ(status->geometry(), statusGeometry);
        EXPECT_TRUE(status->isVisible());
        EXPECT_TRUE(panel->rect().contains(status->geometry()));
        const QRect canvasBounds(canvas->mapTo(panel, QPoint()), canvas->size());
        const QRect meterBounds(meter->mapTo(panel, QPoint()), meter->size());
        const QRect toolbarBounds(toolbar->mapTo(panel, QPoint()), toolbar->size());
        EXPECT_EQ(meterBounds.top(), toolbarBounds.top());
        EXPECT_EQ(meterBounds.right() + 1, toolbarBounds.left());
        EXPECT_EQ(meterBounds.bottom(), canvasBounds.bottom());
        EXPECT_EQ(canvasBounds.left(), 0);
        EXPECT_EQ(canvasBounds.right(), panel->rect().right());

    }
    EXPECT_FALSE(drawer->isVisible());
}

TEST_F(BlueprintOutGraphTest, OverlayControlsRemainAccessibleAcrossFontAndWidthChanges)
{
    auto& fonts = VNX::FontManager::instance();
    const float oldScale = fonts.scaleFactor();
    const QString oldStyle = qApp->styleSheet();
    const auto restoreStyle = qScopeGuard([&] {
        fonts.setScaleFactor(oldScale);
        qApp->setStyleSheet(oldStyle);
    });
    VNX::BlueprintPanel panel;
    panel.resize(1000, 600);
    panel.show();
    auto* meter = panel.findChild<QWidget*>(QStringLiteral("BlueprintPeakMeter"));
    ASSERT_NE(meter, nullptr);
    auto* plot = meter->findChild<QWidget*>(QStringLiteral("meterCanvas"));
    auto* toolbar = panel.findChild<QWidget*>(QStringLiteral("BlueprintToolBar"));
    auto* expand = meter->findChild<QToolButton*>(QStringLiteral("meterExpandButton"));
    auto* resizeHandle = panel.findChild<QWidget*>(QStringLiteral("BlueprintMeterResizeHandle"));
    ASSERT_NE(plot, nullptr);
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(expand, nullptr);
    ASSERT_NE(resizeHandle, nullptr);
    for (float scale : {1.0f, 1.5f, 1.0f}) {
        fonts.setScaleFactor(scale);
        qApp->setStyleSheet(VNX::QtControlSkin(VNX::ThemeManager::instance().snapshot()).applicationStyleSheet());
        panel.resize(1000, 600);
        drainEvents();
        const QRect canvasBounds = panel.canvas()->geometry();
        for (bool expanded : {false, true, false}) {
            expand->setChecked(expanded);
            drainEvents();
            const int plotTop = plot->mapTo(&panel, QPoint()).y();
            EXPECT_EQ(plotTop, toolbar->geometry().bottom() + 1);
            EXPECT_EQ(panel.canvas()->geometry(), canvasBounds);
        }
        expand->setChecked(true);
        drainEvents();
        const int preferredWidth = meter->width();
        panel.resize(panel.minimumWidth(), panel.height());
        drainEvents();
        EXPECT_TRUE(expand->isChecked());
        EXPECT_TRUE(resizeHandle->isVisible());
        EXPECT_EQ(meter->width(), meter->minimumWidth());
        EXPECT_TRUE(panel.rect().contains(toolbar->geometry()));
        for (auto* button : toolbar->findChildren<QToolButton*>()) {
            EXPECT_TRUE(panel.rect().contains(QRect(button->mapTo(&panel, QPoint()), button->size())));
            EXPECT_EQ(panel.childAt(button->mapTo(&panel, button->rect().center())), button);
        }
        EXPECT_EQ(panel.canvas()->geometry().left(), 0);
        EXPECT_EQ(panel.canvas()->geometry().width(), panel.width());
        panel.resize(1000, 600);
        drainEvents();
        EXPECT_EQ(meter->width(), preferredWidth);
        expand->click();
        drainEvents();
        EXPECT_FALSE(expand->isChecked());
        panel.resize(panel.minimumWidth(), panel.height());
        drainEvents();
        expand->click();
        drainEvents();
        EXPECT_TRUE(expand->isChecked());
        EXPECT_TRUE(resizeHandle->isVisible());
        EXPECT_GE(meter->width(), meter->minimumWidth());
        EXPECT_TRUE(panel.rect().contains(toolbar->geometry()));
    }
}

TEST_F(BlueprintOutGraphTest, ZoomButtonKeepsItsWidthAcrossDigitCounts)
{
    VNX::BlueprintPanel panel;
    auto* zoomButton = panel.findChild<QToolButton*>(
        QStringLiteral("BlueprintZoomButton"));
    ASSERT_NE(zoomButton, nullptr);
    const int width = zoomButton->width();

    panel.canvas()->setZoomFactor(0.5);
    EXPECT_EQ(zoomButton->width(), width);

    panel.canvas()->setZoomFactor(4.0);
    EXPECT_EQ(zoomButton->width(), width);
}

TEST_F(BlueprintOutGraphTest, UnifiedViewHasNoPropertyModeOrResolutionDetail)
{
    VNX::BlueprintPanel panel;
    auto* properties = panel.findChild<QToolButton*>(QStringLiteral("BlueprintViewportVisibilityButton"));
    auto* chain = panel.findChild<QToolButton*>(QStringLiteral("BlueprintViewportFilterButton"));
    auto* toolbar = panel.findChild<QWidget*>(QStringLiteral("BlueprintToolBar"));
    ASSERT_NE(properties, nullptr);
    ASSERT_NE(chain, nullptr);
    ASSERT_NE(toolbar, nullptr);
    EXPECT_TRUE(toolbar->findChildren<QComboBox*>().empty());
    for (auto* button : {chain, properties}) {
        EXPECT_EQ(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
        EXPECT_FALSE(button->icon().isNull());
        EXPECT_EQ(button->size(), QSize(22, 22));
        EXPECT_TRUE(button->property("flatIconButton").toBool());
    }
    const auto fields = panel.canvas()->nodeContentFields();
    EXPECT_FALSE(chain->isChecked());
    EXPECT_EQ(properties->isChecked(), fields != VNX::kAllNodeContentFields);
    EXPECT_EQ(panel.findChild<QToolButton*>(QStringLiteral("BlueprintLayersFilterButton")), nullptr);
    EXPECT_EQ(panel.findChild<QToolButton*>(QStringLiteral("BlueprintBusFilterButton")), nullptr);
    EXPECT_EQ(panel.findChild<QLabel*>(QStringLiteral("BlueprintResolutionDetail")), nullptr);
    auto* bar = panel.findChild<QWidget*>(QStringLiteral("BlueprintStatusBar"));
    ASSERT_NE(bar, nullptr);
    EXPECT_TRUE(bar->findChildren<QComboBox*>().empty());
    EXPECT_TRUE(bar->findChildren<QLabel*>().empty());
    EXPECT_EQ(bar->findChildren<QToolButton*>().size(), 2);
    EXPECT_EQ(bar->findChild<QToolButton*>(QStringLiteral("BlueprintBackButton")), nullptr);
    EXPECT_EQ(bar->findChild<QToolButton*>(QStringLiteral("BlueprintUpButton")), nullptr);
}

const VNX::BlueprintEdgeDesc*
firstItemToBusEdge(const std::vector<VNX::BlueprintEdgeDesc>& edges)
{
    for (auto const& e : edges) {
        if (e.fromKind == VNX::BlueprintNodeKind::ProjectItem &&
            e.toKind == VNX::BlueprintNodeKind::Bus) {
            return &e;
        }
    }
    return nullptr;
}

const VNX::BlueprintEdgeDesc*
findEdge(const std::vector<VNX::BlueprintEdgeDesc>& edges,
         VNX::BlueprintNodeKind fromKind, uint32_t fromId,
         VNX::BlueprintNodeKind toKind, uint32_t toId)
{
    for (auto const& e : edges) {
        if (e.fromKind == fromKind && e.fromId == fromId &&
            e.toKind == toKind && e.toId == toId) {
            return &e;
        }
    }
    return nullptr;
}

bool chainHas(const std::vector<VNX::SignalChainNode>& nodes, uint32_t objectID)
{
    for (auto const& n : nodes) {
        if (n.objectID == objectID) return true;
    }
    return false;
}

uint8_t kindOf(VNX::BlueprintEdgeKind kind)
{
    return static_cast<uint8_t>(kind);
}

bool setMainOutputLocal(ProjectItem* item, const ProjectItem* bus)
{
    auto groups = item->fieldGroups;
    groups.mainOutput.shared = {
        VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(bus->guid), 0.0f}};
    return VNX::ProjectManager::instance().restoreFieldGroupsInternal(item, groups);
}

bool setSendLocal(ProjectItem* item, const ProjectItem* bus)
{
    if (item->type == T::Bus) {
        return VNX::ProjectManager::instance().applyBusPropertyLive(
            item, QStringLiteral("sends"),
            QVariantList{QVariantMap{{QStringLiteral("targetBusID"), bus->objectID},
                                    {QStringLiteral("sendLevel"), -6.0f}}});
    }
    auto groups = item->fieldGroups;
    groups.send.shared = {VNX::FieldGroupMode::Local,
        VNX::SendFieldGroup{{{VNX::toFieldGroupGuid(bus->guid), -6.0f}}}};
    return VNX::ProjectManager::instance().restoreFieldGroupsInternal(item, groups);
}

QSet<QUuid> graphObjects(const VNX::BlueprintCanvasWidget& canvas)
{
    QSet<QUuid> objects;
    for (const auto& node : canvas.activeSignalPathSource()->graph().nodes)
        objects.insert(node.key.objectGuid);
    return objects;
}

QSet<QUuid> visibleGraphObjects(const VNX::BlueprintCanvasWidget& canvas)
{
    QSet<QUuid> objects;
    const auto& nodes = canvas.activeSignalPathSource()->graph().nodes;
    for (size_t index = 0; index < nodes.size(); ++index) {
        if (canvas.signalPathNode(index)->isVisible()) objects.insert(nodes[index].key.objectGuid);
    }
    return objects;
}


}

TEST_F(BlueprintOutGraphTest, ViewportMenusDismissEachOtherAndKeepIndependentStates)
{
    VNX::BlueprintPanel panel;
    panel.resize(900, 600);
    panel.show();
    drainEvents();
    auto* filter = panel.findChild<QToolButton*>(QStringLiteral("BlueprintViewportFilterButton"));
    auto* visibility = panel.findChild<QToolButton*>(QStringLiteral("BlueprintViewportVisibilityButton"));
    auto* filterMenu = panel.findChild<QMenu*>(QStringLiteral("BlueprintViewportFilterMenu"));
    auto* visibilityMenu = panel.findChild<QMenu*>(QStringLiteral("BlueprintViewportVisibilityMenu"));
    ASSERT_NE(filter, nullptr);
    ASSERT_NE(visibility, nullptr);
    ASSERT_NE(filterMenu, nullptr);
    ASSERT_NE(visibilityMenu, nullptr);
    const auto original = panel.canvas()->nodeContentFields();
    const auto restore = qScopeGuard([&] { panel.canvas()->setNodeContentFields(original); });
    panel.canvas()->setNodeContentFields(VNX::kAllNodeContentFields);
    visibility->click();
    drainEvents();
    EXPECT_TRUE(visibilityMenu->isVisible());
    filter->click();
    drainEvents();
    EXPECT_FALSE(visibilityMenu->isVisible());
    EXPECT_TRUE(filterMenu->isVisible());
    ASSERT_EQ(filterMenu->actions().size(), 1);
    filterMenu->actions().front()->trigger();
    EXPECT_TRUE(panel.canvas()->soundsFiltered());
    visibility->click();
    drainEvents();
    EXPECT_FALSE(filterMenu->isVisible());
    EXPECT_TRUE(visibilityMenu->isVisible());
    visibility->click();
    drainEvents();
    EXPECT_FALSE(visibilityMenu->isVisible());
    EXPECT_FALSE(visibility->isChecked());
    EXPECT_TRUE(filter->isChecked());
}



TEST_F(BlueprintOutGraphTest, CollapseCapabilityFollowsInputsAcrossMovesAndDeletion)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* master = pm.getMasterBusItem();
    VNX::BlueprintPanel panel;
    auto* canvas = panel.canvas();
    EXPECT_FALSE(canvas->canCollapseChain(master->objectID));
    for (auto type : {T::Bus, T::Layer, T::Container, T::Track, T::Piece}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto* target = pm.createItem(type == T::Bus ? master : nullptr, QStringLiteral("Target"), type);
        ASSERT_NE(target, nullptr);
        drainEvents();
        const auto expectState = [&](bool enabled, Qt::CheckState state) {
            EXPECT_EQ(canvas->canCollapseChain(target->objectID), enabled);
            const auto& graph = canvas->activeSignalPathSource()->graph();
            for (size_t index = 0; index < graph.nodes.size(); ++index) {
                if (graph.nodes[index].objectID != target->objectID) continue;
                auto* pin = canvas->signalPathNode(index)->inputPin();
                EXPECT_EQ(pin->chainVisibility(), enabled ? std::optional(state) : std::nullopt);
                return;
            }
            FAIL() << "Expected graph node is missing";
        };
        expectState(false, Qt::Unchecked);
        EXPECT_TRUE(canvas->canCollapseChain(master->objectID));
        canvas->setChainVisible(target->objectID, false);
        expectState(false, Qt::Unchecked);
        EXPECT_TRUE(canvas->hiddenHierarchyRoots().isEmpty());
        EXPECT_TRUE(canvas->hiddenBuses().isEmpty());
        const auto childType = type == T::Bus ? T::Bus : type == T::Piece ? T::Track : T::Sound;
        auto* input = pm.createItem(target, QStringLiteral("Input"), childType);
        ASSERT_NE(input, nullptr);
        drainEvents();
        expectState(true, Qt::Checked);
        canvas->setChainVisible(target->objectID, false);
        drainEvents();
        expectState(true, Qt::Unchecked);
        EXPECT_FALSE(visibleGraphObjects(*canvas).contains(input->guid));
        ASSERT_TRUE(type == T::Bus ? pm.moveItem(input, master)
            : pm.moveItemToBucketRoot(input, QString::fromLatin1(VNX::ProjectManager::kLayers)));
        drainEvents();
        expectState(false, Qt::Unchecked);
        EXPECT_TRUE(canvas->hiddenHierarchyRoots().isEmpty());
        EXPECT_TRUE(canvas->hiddenBuses().isEmpty());
        input = pm.createItem(target, QStringLiteral("ReplacementInput"), childType);
        ASSERT_NE(input, nullptr);
        drainEvents();
        expectState(true, Qt::Checked);
        EXPECT_TRUE(visibleGraphObjects(*canvas).contains(input->guid));
        canvas->setChainVisible(target->objectID, false);
        ASSERT_TRUE(pm.deleteItem(input));
        drainEvents();
        expectState(false, Qt::Unchecked);
        EXPECT_TRUE(canvas->hiddenHierarchyRoots().isEmpty());
        EXPECT_TRUE(canvas->hiddenBuses().isEmpty());
    }
}





TEST_F(BlueprintOutGraphTest, BusFilterKeepsItsOriginAndDownstreamRoutingWhileCollapsingInputs)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* master = pm.getMasterBusItem();
    auto* target = pm.createItem(master, QStringLiteral("Target"), T::Bus);
    auto* childBus = pm.createItem(target, QStringLiteral("ChildBus"), T::Bus);
    auto* receiveBus = pm.createItem(master, QStringLiteral("ReceiveBus"), T::Bus);
    auto* upstreamBus = pm.createItem(master, QStringLiteral("UpstreamBus"), T::Bus);
    auto* unrelatedBus = pm.createItem(master, QStringLiteral("UnrelatedBus"), T::Bus);
    auto* isolatedBus = pm.createItem(master, QStringLiteral("IsolatedBus"), T::Bus);
    auto* direct = pm.createItem(nullptr, QStringLiteral("Direct"), T::Layer);
    auto* directSound = pm.createItem(direct, QStringLiteral("DirectSound"), T::Sound);
    auto* received = pm.createItem(nullptr, QStringLiteral("Received"), T::Layer);
    auto* receivedSound = pm.createItem(received, QStringLiteral("ReceivedSound"), T::Sound);
    auto* upstream = pm.createItem(nullptr, QStringLiteral("Upstream"), T::Layer);
    auto* upstreamSound = pm.createItem(upstream, QStringLiteral("UpstreamSound"), T::Sound);
    auto* unrelated = pm.createItem(nullptr, QStringLiteral("Unrelated"), T::Layer);
    auto* unrelatedSound = pm.createItem(unrelated, QStringLiteral("UnrelatedSound"), T::Sound);
    auto* sender = pm.createItem(nullptr, QStringLiteral("Sender"), T::Sound);
    for (auto* item : {master, target, childBus, receiveBus, upstreamBus, unrelatedBus, isolatedBus,
                      direct, directSound, received, receivedSound, upstream, upstreamSound,
                      unrelated, unrelatedSound, sender}) ASSERT_NE(item, nullptr);
    ASSERT_TRUE(setMainOutputLocal(direct, target));
    ASSERT_TRUE(setMainOutputLocal(received, receiveBus));
    ASSERT_TRUE(setMainOutputLocal(upstream, upstreamBus));
    ASSERT_TRUE(setMainOutputLocal(unrelated, unrelatedBus));
    ASSERT_TRUE(setMainOutputLocal(sender, unrelatedBus));
    ASSERT_TRUE(setSendLocal(sender, childBus));
    ASSERT_TRUE(setSendLocal(receiveBus, childBus));
    ASSERT_TRUE(setSendLocal(upstreamBus, receiveBus));
    ASSERT_TRUE(setSendLocal(target, unrelatedBus));
    auto* isolatedSound = pm.createItem(nullptr, QStringLiteral("IsolatedSound"), T::Sound);
    ASSERT_NE(isolatedSound, nullptr);
    ASSERT_TRUE(setMainOutputLocal(isolatedSound, isolatedBus));
    VNX::BlueprintCanvasWidget canvas;
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(graph.ok());
    const auto original = graphObjects(canvas);
    const auto edges = graph.edges.size();
    const QSet<QUuid> hidden{childBus->guid, receiveBus->guid, upstreamBus->guid,
        direct->guid, directSound->guid, received->guid, receivedSound->guid,
        upstream->guid, upstreamSound->guid, sender->guid};
    canvas.setBusVisible(target->objectID, false);
    EXPECT_EQ(graphObjects(canvas), original);
    EXPECT_EQ(graph.edges.size(), edges);
    EXPECT_EQ(visibleGraphObjects(canvas), original - hidden);
    EXPECT_EQ(canvas.busVisibility(target->objectID), Qt::Unchecked);
    EXPECT_EQ(canvas.busVisibility(upstreamBus->objectID), Qt::Unchecked);
    EXPECT_EQ(canvas.busVisibility(master->objectID), Qt::PartiallyChecked);
    EXPECT_EQ(canvas.busVisibility(unrelatedBus->objectID), Qt::PartiallyChecked);
    size_t retainedOutputEdges = 0;
    size_t retainedSendEdges = 0;
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        const auto& from = graph.nodes[edge.from.occurrenceIndex];
        const auto& to = graph.nodes[edge.to.occurrenceIndex];
        if (from.key.objectGuid == target->guid) {
            EXPECT_TRUE(canvas.signalPathEdge(index)->isVisible());
            if (auto* gain = canvas.signalPathGainNode(index)) EXPECT_TRUE(gain->isVisible());
            if (auto* output = canvas.signalPathGainOutputEdge(index)) EXPECT_TRUE(output->isVisible());
            if (edge.kind == VNX::SignalPathEdgeKind::Output) ++retainedOutputEdges;
            if (edge.kind == VNX::SignalPathEdgeKind::Send) ++retainedSendEdges;
        }
        if (!hidden.contains(from.key.objectGuid) && !hidden.contains(to.key.objectGuid)) continue;
        EXPECT_FALSE(canvas.signalPathEdge(index)->isVisible());
        if (auto* gain = canvas.signalPathGainNode(index)) EXPECT_FALSE(gain->isVisible());
        if (auto* output = canvas.signalPathGainOutputEdge(index)) EXPECT_FALSE(output->isVisible());
    }
    EXPECT_GT(retainedOutputEdges, 0u);
    EXPECT_GT(retainedSendEdges, 0u);
    canvas.setHierarchyVisible(unrelated->objectID, false);
    const QSet<QUuid> hiddenLayer{unrelatedSound->guid};
    EXPECT_EQ(visibleGraphObjects(canvas), original - hidden - hiddenLayer);
    canvas.setBusVisible(target->objectID, true);
    EXPECT_FALSE(canvas.hasCollapsedBuses());
    EXPECT_TRUE(canvas.hasCollapsedHierarchy());
    EXPECT_EQ(visibleGraphObjects(canvas), original - hiddenLayer);
    canvas.setBusVisible(target->objectID, false);
    canvas.setBusVisible(isolatedBus->objectID, false);
    canvas.setChainVisible(childBus->objectID, true);
    EXPECT_EQ(canvas.chainVisibility(childBus->objectID), Qt::Checked);
    EXPECT_EQ(canvas.chainVisibility(target->objectID), Qt::PartiallyChecked);
    EXPECT_EQ(visibleGraphObjects(canvas), original - hiddenLayer
        - QSet<QUuid>({isolatedSound->guid, direct->guid, directSound->guid}));
    EXPECT_EQ(canvas.busVisibility(isolatedBus->objectID), Qt::Unchecked);
    size_t isolatedOutputs = 0;
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        if (graph.nodes[graph.edges[index].from.occurrenceIndex].key.objectGuid != isolatedBus->guid) continue;
        ++isolatedOutputs;
        EXPECT_TRUE(canvas.signalPathEdge(index)->isVisible());
        if (auto* gain = canvas.signalPathGainNode(index)) EXPECT_TRUE(gain->isVisible());
    }
    EXPECT_GT(isolatedOutputs, 0u);
    const auto beforeMasterCollapse = visibleGraphObjects(canvas);
    canvas.setBusVisible(master->objectID, false);
    EXPECT_EQ(visibleGraphObjects(canvas), QSet<QUuid>{master->guid});
    EXPECT_EQ(graphObjects(canvas), original);
    EXPECT_EQ(graph.edges.size(), edges);
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        EXPECT_FALSE(canvas.signalPathEdge(index)->isVisible());
        if (auto* gain = canvas.signalPathGainNode(index)) EXPECT_FALSE(gain->isVisible());
        if (auto* output = canvas.signalPathGainOutputEdge(index)) EXPECT_FALSE(output->isVisible());
    }
    canvas.setBusVisible(master->objectID, true);
    EXPECT_TRUE(canvas.hasCollapsedBuses());
    EXPECT_EQ(visibleGraphObjects(canvas), beforeMasterCollapse);
}

TEST_F(BlueprintOutGraphTest, BranchExpansionPreservesOtherInputsAcrossRefreshAndRepeatedToggles)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* master = pm.getMasterBusItem();
    auto* grandparent = pm.createItem(master, QStringLiteral("Grandparent"), T::Bus);
    auto* parent = pm.createItem(grandparent, QStringLiteral("Parent"), T::Bus);
    auto* first = pm.createItem(parent, QStringLiteral("First"), T::Bus);
    auto* second = pm.createItem(parent, QStringLiteral("Second"), T::Bus);
    auto* sibling = pm.createItem(grandparent, QStringLiteral("Sibling"), T::Bus);
    auto* layer = pm.createItem(nullptr, QStringLiteral("SharedLayer"), T::Layer);
    auto* firstSound = pm.createItem(layer, QStringLiteral("FirstSound"), T::Sound);
    auto* secondSound = pm.createItem(layer, QStringLiteral("SecondSound"), T::Sound);
    auto* siblingSound = pm.createItem(nullptr, QStringLiteral("SiblingSound"), T::Sound);
    for (auto* item : {master, grandparent, parent, first, second, sibling,
                      layer, firstSound, secondSound, siblingSound}) ASSERT_NE(item, nullptr);
    ASSERT_TRUE(setMainOutputLocal(firstSound, first));
    ASSERT_TRUE(setMainOutputLocal(secondSound, second));
    ASSERT_TRUE(setMainOutputLocal(siblingSound, sibling));
    VNX::BlueprintCanvasWidget canvas;
    const auto original = graphObjects(canvas);
    const QSet<QUuid> otherInputs{second->guid, secondSound->guid, sibling->guid, siblingSound->guid};
    for (int iteration = 0; iteration < 2; ++iteration) {
        canvas.setChainVisible(grandparent->objectID, false);
        canvas.setChainVisible(first->objectID, true);
        EXPECT_EQ(visibleGraphObjects(canvas), original - otherInputs);
        EXPECT_EQ(canvas.chainVisibility(first->objectID), Qt::Checked);
        EXPECT_EQ(canvas.busVisibility(parent->objectID), Qt::PartiallyChecked);
        EXPECT_EQ(canvas.chainVisibility(parent->objectID), Qt::PartiallyChecked);
        EXPECT_EQ(canvas.chainVisibility(grandparent->objectID), Qt::PartiallyChecked);
        EXPECT_EQ(canvas.hierarchyVisibility(layer->objectID), Qt::PartiallyChecked);
        canvas.setFlowDirection(iteration == 0 ? VNX::CanvasFlowDirection::TB : VNX::CanvasFlowDirection::LR);
        canvas.rebuildFromProject();
        drainEvents();
        EXPECT_EQ(visibleGraphObjects(canvas), original - otherInputs);
        canvas.setChainVisible(first->objectID, false);
        EXPECT_EQ(canvas.chainVisibility(first->objectID), Qt::Unchecked);
        EXPECT_FALSE(visibleGraphObjects(canvas).contains(firstSound->guid));
        canvas.setChainVisible(first->objectID, true);
        EXPECT_EQ(visibleGraphObjects(canvas), original - otherInputs);
        canvas.setBusVisible(parent->objectID, true);
        EXPECT_EQ(canvas.busVisibility(parent->objectID), Qt::Checked);
        EXPECT_EQ(canvas.chainVisibility(parent->objectID), Qt::PartiallyChecked);
        EXPECT_EQ(canvas.chainVisibility(second->objectID), Qt::Unchecked);
        EXPECT_EQ(visibleGraphObjects(canvas), original - otherInputs + QSet<QUuid>{second->guid});
        canvas.setChainVisible(parent->objectID, true);
        EXPECT_EQ(canvas.chainVisibility(parent->objectID), Qt::Checked);
        EXPECT_EQ(canvas.chainVisibility(grandparent->objectID), Qt::PartiallyChecked);
        EXPECT_EQ(visibleGraphObjects(canvas), original - QSet<QUuid>({sibling->guid, siblingSound->guid}));
    }
    canvas.setChainVisible(sibling->objectID, true);
    EXPECT_EQ(visibleGraphObjects(canvas), original);
    EXPECT_EQ(canvas.chainVisibility(grandparent->objectID), Qt::Checked);
    EXPECT_FALSE(canvas.hasCollapsedBuses());
    ASSERT_TRUE(setSendLocal(firstSound, sibling));
    drainEvents();
    canvas.setChainVisible(grandparent->objectID, false);
    canvas.setChainVisible(first->objectID, true);
    EXPECT_EQ(visibleGraphObjects(canvas), original - otherInputs + QSet<QUuid>{sibling->guid});
    EXPECT_EQ(canvas.busVisibility(sibling->objectID), Qt::PartiallyChecked);
    EXPECT_EQ(canvas.chainVisibility(sibling->objectID), Qt::PartiallyChecked);
    EXPECT_EQ(canvas.busVisibility(grandparent->objectID), Qt::Checked);
    EXPECT_EQ(canvas.chainVisibility(grandparent->objectID), Qt::PartiallyChecked);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const bool visible = canvas.signalPathEdge(index)->isVisible();
        if (auto* gain = canvas.signalPathGainNode(index)) EXPECT_EQ(gain->isVisible(), visible);
        if (auto* output = canvas.signalPathGainOutputEdge(index)) EXPECT_EQ(output->isVisible(), visible);
        if (visible) {
            EXPECT_TRUE(canvas.signalPathNode(graph.edges[index].from.occurrenceIndex)->isVisible());
            EXPECT_TRUE(canvas.signalPathNode(graph.edges[index].to.occurrenceIndex)->isVisible());
        }
    }
}

TEST_F(BlueprintOutGraphTest, ChainIndicatorsDistinguishDirectInputsFromCompleteUpstreamRouting)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* master = pm.getMasterBusItem();
    auto* output = pm.createItem(master, QStringLiteral("Output"), T::Bus);
    auto* receive = pm.createItem(master, QStringLiteral("Receive"), T::Bus);
    auto* independent = pm.createItem(master, QStringLiteral("Independent"), T::Bus);
    auto* sender = pm.createItem(nullptr, QStringLiteral("Sender"), T::Sound);
    for (auto* item : {master, output, receive, independent, sender}) ASSERT_NE(item, nullptr);
    ASSERT_TRUE(setMainOutputLocal(sender, output));
    ASSERT_TRUE(setSendLocal(sender, receive));
    auto* independentSound = pm.createItem(nullptr, QStringLiteral("IndependentSound"), T::Sound);
    ASSERT_NE(independentSound, nullptr);
    ASSERT_TRUE(setMainOutputLocal(independentSound, independent));
    VNX::BlueprintPanel panel;
    auto* canvas = panel.canvas();
    const auto expectState = [&](const ProjectItem* item, Qt::CheckState inputs, Qt::CheckState upstream) {
        SCOPED_TRACE(item->name.toStdString());
        EXPECT_EQ(canvas->busVisibility(item->objectID), inputs);
        EXPECT_EQ(canvas->chainVisibility(item->objectID), upstream);
        const auto& graph = canvas->activeSignalPathSource()->graph();
        for (size_t index = 0; index < graph.nodes.size(); ++index) {
            if (graph.nodes[index].objectID != item->objectID) continue;
            EXPECT_EQ(canvas->signalPathNode(index)->inputPin()->chainVisibility(), inputs);
            return;
        }
        FAIL() << "Expected graph node is missing";
    };
    canvas->setBusVisible(independent->objectID, false);
    canvas->setBusVisible(output->objectID, false);
    drainEvents();
    EXPECT_FALSE(visibleGraphObjects(*canvas).contains(sender->guid));
    expectState(output, Qt::Unchecked, Qt::Unchecked);
    expectState(receive, Qt::Unchecked, Qt::Unchecked);
    expectState(master, Qt::Checked, Qt::PartiallyChecked);
    for (const auto* name : {"DirectOne", "DirectTwo"}) {
        auto* direct = pm.createItem(nullptr, QString::fromLatin1(name), T::Sound);
        ASSERT_NE(direct, nullptr);
        ASSERT_TRUE(setMainOutputLocal(direct, receive));
    }
    drainEvents();
    expectState(receive, Qt::PartiallyChecked, Qt::PartiallyChecked);
    expectState(master, Qt::Checked, Qt::PartiallyChecked);
    canvas->setChainVisible(receive->objectID, true);
    drainEvents();
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(sender->guid));
    expectState(output, Qt::Checked, Qt::Checked);
    expectState(receive, Qt::Checked, Qt::Checked);
    expectState(master, Qt::Checked, Qt::PartiallyChecked);
    expectState(independent, Qt::Unchecked, Qt::Unchecked);
    canvas->setBusVisible(output->objectID, false);
    canvas->setBusVisible(master->objectID, true);
    drainEvents();
    EXPECT_FALSE(visibleGraphObjects(*canvas).contains(sender->guid));
    expectState(master, Qt::Checked, Qt::PartiallyChecked);
    canvas->setChainVisible(master->objectID, true);
    drainEvents();
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(sender->guid));
    expectState(output, Qt::Checked, Qt::Checked);
    expectState(receive, Qt::Checked, Qt::Checked);
    expectState(master, Qt::Checked, Qt::Checked);
    expectState(independent, Qt::Checked, Qt::Checked);
}

TEST_F(BlueprintOutGraphTest, ExpandingHierarchyInputsReleasesCombinedBusFiltersAndKeepsIndependentFilters)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* master = pm.getMasterBusItem();
    auto* firstBus = pm.createItem(master, QStringLiteral("FirstBus"), T::Bus);
    auto* secondBus = pm.createItem(master, QStringLiteral("SecondBus"), T::Bus);
    auto* independent = pm.createItem(nullptr, QStringLiteral("Independent"), T::Layer);
    auto* parent = pm.createItem(nullptr, QStringLiteral("Parent"), T::Layer);
    auto* child = pm.createItem(parent, QStringLiteral("Child"), T::Container);
    auto* first = pm.createItem(child, QStringLiteral("First"), T::Sound);
    auto* second = pm.createItem(child, QStringLiteral("Second"), T::Sound);
    auto* sibling = pm.createItem(parent, QStringLiteral("Sibling"), T::Sound);
    for (auto* item : {firstBus, secondBus, independent, parent, child, first, second, sibling})
        ASSERT_NE(item, nullptr);
    ASSERT_TRUE(setMainOutputLocal(first, firstBus));
    ASSERT_TRUE(setMainOutputLocal(second, secondBus));
    ASSERT_NE(pm.createItem(independent, QStringLiteral("IndependentSound"), T::Sound), nullptr);
    VNX::BlueprintCanvasWidget canvas;
    canvas.setHierarchyVisible(independent->objectID, false);
    canvas.setBusVisible(firstBus->objectID, false);
    EXPECT_EQ(canvas.hierarchyVisibility(child->objectID), Qt::PartiallyChecked);
    EXPECT_EQ(canvas.hierarchyVisibility(parent->objectID), Qt::Checked);
    EXPECT_EQ(canvas.chainVisibility(parent->objectID), Qt::PartiallyChecked);
    canvas.setHierarchyVisible(parent->objectID, true);
    EXPECT_FALSE(visibleGraphObjects(canvas).contains(first->guid));
    canvas.setChainVisible(parent->objectID, true);
    EXPECT_TRUE(visibleGraphObjects(canvas).contains(first->guid));
    EXPECT_EQ(canvas.chainVisibility(parent->objectID), Qt::Checked);
    canvas.setBusVisible(firstBus->objectID, false);
    canvas.setBusVisible(secondBus->objectID, false);
    EXPECT_FALSE(visibleGraphObjects(canvas).contains(child->guid));
    EXPECT_EQ(canvas.hierarchyVisibility(child->objectID), Qt::Unchecked);
    EXPECT_EQ(canvas.hierarchyVisibility(parent->objectID), Qt::PartiallyChecked);
    canvas.setHierarchyVisible(parent->objectID, true);
    EXPECT_TRUE(visibleGraphObjects(canvas).contains(first->guid));
    EXPECT_TRUE(visibleGraphObjects(canvas).contains(second->guid));
    EXPECT_EQ(canvas.hierarchyVisibility(child->objectID), Qt::Checked);
    EXPECT_EQ(canvas.hierarchyVisibility(parent->objectID), Qt::Checked);
    EXPECT_EQ(canvas.hierarchyVisibility(independent->objectID), Qt::Unchecked);
    EXPECT_FALSE(canvas.hasCollapsedBuses());
}

TEST_F(BlueprintOutGraphTest, BusFilterKeepsSharedLayerNodesAndUnrelatedSoundPaths)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* master = pm.getMasterBusItem();
    auto* filteredBus = pm.createItem(master, QStringLiteral("FilteredBus"), T::Bus);
    auto* otherBus = pm.createItem(master, QStringLiteral("OtherBus"), T::Bus);
    auto* layer = pm.createItem(nullptr, QStringLiteral("SharedLayer"), T::Layer);
    auto* hiddenSound = pm.createItem(layer, QStringLiteral("HiddenSound"), T::Sound);
    auto* visibleSound = pm.createItem(layer, QStringLiteral("VisibleSound"), T::Sound);
    for (auto* item : {master, filteredBus, otherBus, layer, hiddenSound, visibleSound})
        ASSERT_NE(item, nullptr);
    ASSERT_TRUE(setMainOutputLocal(layer, otherBus));
    ASSERT_TRUE(setMainOutputLocal(hiddenSound, filteredBus));
    VNX::BlueprintCanvasWidget canvas;
    const auto original = graphObjects(canvas);
    canvas.setBusVisible(filteredBus->objectID, false);
    EXPECT_EQ(visibleGraphObjects(canvas), original - QSet<QUuid>{hiddenSound->guid});
    EXPECT_EQ(canvas.hierarchyVisibility(layer->objectID), Qt::PartiallyChecked);
    EXPECT_EQ(canvas.busVisibility(otherBus->objectID), Qt::Checked);
    EXPECT_EQ(canvas.chainVisibility(layer->objectID), Qt::PartiallyChecked);
    EXPECT_EQ(canvas.chainVisibility(filteredBus->objectID), Qt::Unchecked);
    EXPECT_EQ(canvas.chainVisibility(otherBus->objectID), Qt::Checked);
    canvas.setChainVisible(otherBus->objectID, true);
    EXPECT_EQ(visibleGraphObjects(canvas), original - QSet<QUuid>{hiddenSound->guid});
    const auto& graph = canvas.activeSignalPathSource()->graph();
    size_t survivingSoundEdges = 0;
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        if (graph.nodes[edge.from.occurrenceIndex].key.objectGuid != visibleSound->guid) continue;
        ++survivingSoundEdges;
        EXPECT_TRUE(canvas.signalPathEdge(index)->isVisible());
    }
    EXPECT_GT(survivingSoundEdges, 0u);
    ASSERT_TRUE(setMainOutputLocal(hiddenSound, otherBus));
    drainEvents();
    EXPECT_EQ(visibleGraphObjects(canvas), original);
    EXPECT_EQ(canvas.busVisibility(filteredBus->objectID), Qt::Unchecked);
    EXPECT_FALSE(canvas.canCollapseChain(filteredBus->objectID));
    EXPECT_TRUE(canvas.hiddenBuses().isEmpty());
    ASSERT_TRUE(setMainOutputLocal(hiddenSound, filteredBus));
    drainEvents();
    EXPECT_TRUE(visibleGraphObjects(canvas).contains(hiddenSound->guid));
    EXPECT_TRUE(canvas.canCollapseChain(filteredBus->objectID));
    EXPECT_EQ(canvas.busVisibility(filteredBus->objectID), Qt::Checked);
    canvas.setHierarchyVisible(layer->objectID, false);
    canvas.setBusVisible(filteredBus->objectID, true);
    EXPECT_FALSE(visibleGraphObjects(canvas).contains(hiddenSound->guid));
    EXPECT_FALSE(visibleGraphObjects(canvas).contains(visibleSound->guid));
    EXPECT_EQ(canvas.busVisibility(filteredBus->objectID), Qt::Checked);
    EXPECT_EQ(canvas.chainVisibility(filteredBus->objectID), Qt::PartiallyChecked);
    canvas.setChainVisible(filteredBus->objectID, true);
    EXPECT_EQ(visibleGraphObjects(canvas), original - QSet<QUuid>{visibleSound->guid});
    EXPECT_EQ(canvas.chainVisibility(layer->objectID), Qt::PartiallyChecked);
    EXPECT_EQ(canvas.chainVisibility(filteredBus->objectID), Qt::Checked);
    canvas.setBusVisible(layer->objectID, false);
    EXPECT_FALSE(canvas.hasCollapsedBuses());
}

TEST_F(BlueprintOutGraphTest, CollapsedBusOriginCanBecomeUpstreamOfAnotherCollapsedBus)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* master = pm.getMasterBusItem();
    auto* downstream = pm.createItem(master, QStringLiteral("Downstream"), T::Bus);
    auto* upstream = pm.createItem(master, QStringLiteral("Upstream"), T::Bus);
    auto* source = pm.createItem(nullptr, QStringLiteral("Source"), T::Layer);
    auto* sound = pm.createItem(source, QStringLiteral("Sound"), T::Sound);
    for (auto* item : {master, downstream, upstream, source, sound}) ASSERT_NE(item, nullptr);
    ASSERT_TRUE(setMainOutputLocal(source, upstream));
    auto* downstreamSound = pm.createItem(nullptr, QStringLiteral("DownstreamSound"), T::Sound);
    ASSERT_NE(downstreamSound, nullptr);
    ASSERT_TRUE(setMainOutputLocal(downstreamSound, downstream));
    VNX::BlueprintCanvasWidget canvas;
    const auto original = graphObjects(canvas);
    const QSet<QUuid> hiddenInputs{source->guid, sound->guid, downstreamSound->guid};
    canvas.setBusVisible(downstream->objectID, false);
    canvas.setBusVisible(upstream->objectID, false);
    EXPECT_EQ(visibleGraphObjects(canvas), original - hiddenInputs);
    VNX::BlueprintSendConnectCommand connect(upstream->objectID, downstream->objectID);
    connect.redo();
    drainEvents();
    EXPECT_EQ(canvas.hiddenBuses(), QSet<QUuid>({downstream->guid, upstream->guid}));
    EXPECT_EQ(visibleGraphObjects(canvas), original - hiddenInputs - QSet<QUuid>{upstream->guid});
    connect.undo();
    drainEvents();
    EXPECT_EQ(visibleGraphObjects(canvas), original - hiddenInputs);
    EXPECT_EQ(canvas.busVisibility(upstream->objectID), Qt::Unchecked);
    canvas.setBusVisible(downstream->objectID, true);
    EXPECT_EQ(canvas.hiddenBuses(), QSet<QUuid>{upstream->guid});
    canvas.setBusVisible(upstream->objectID, true);
    EXPECT_EQ(visibleGraphObjects(canvas), original);
}

TEST_F(BlueprintOutGraphTest, BusFilterTracksRenamesNewInputsDeletionAndProjectReset)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* master = pm.getMasterBusItem();
    auto* bus = pm.createItem(master, QStringLiteral("Filtered"), T::Bus);
    ASSERT_NE(bus, nullptr);
    const QUuid guid = bus->guid;
    const uint32_t id = bus->objectID;
    ASSERT_NE(pm.createItem(bus, QStringLiteral("InitialInput"), T::Bus), nullptr);
    VNX::BlueprintPanel panel;
    auto* canvas = panel.canvas();
    canvas->setBusVisible(id, false);
    QApplication::processEvents();
    ASSERT_TRUE(pm.renameItem(bus, QStringLiteral("RenamedBus")));
    auto* child = pm.createItem(bus, QStringLiteral("NewInputBus"), T::Bus);
    auto* source = pm.createItem(nullptr, QStringLiteral("NewInput"), T::Layer);
    auto* unrelated = pm.createItem(master, QStringLiteral("NewUnrelatedBus"), T::Bus);
    for (auto* item : {child, source, unrelated}) ASSERT_NE(item, nullptr);
    ASSERT_NE(pm.createItem(source, QStringLiteral("SourceSound"), T::Sound), nullptr);
    ASSERT_NE(pm.createItem(unrelated, QStringLiteral("UnrelatedInput"), T::Bus), nullptr);
    ASSERT_TRUE(setMainOutputLocal(source, child));
    QApplication::processEvents();
    QApplication::processEvents();
    EXPECT_EQ(canvas->hiddenBuses(), QSet<QUuid>{guid});
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(guid));
    EXPECT_FALSE(visibleGraphObjects(*canvas).contains(child->guid));
    EXPECT_FALSE(visibleGraphObjects(*canvas).contains(source->guid));
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(unrelated->guid));
    ASSERT_TRUE(setMainOutputLocal(source, master));
    drainEvents();
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(source->guid));
    ASSERT_TRUE(pm.deleteItem(bus));
    QApplication::processEvents();
    QApplication::processEvents();
    EXPECT_FALSE(canvas->hasCollapsedBuses());
    EXPECT_EQ(visibleGraphObjects(*canvas), graphObjects(*canvas));
    canvas->setBusVisible(unrelated->objectID, false);
    canvas->setHierarchyVisible(source->objectID, false);
    pm.closeProject();
    EXPECT_FALSE(canvas->hasCollapsedBuses());
    EXPECT_TRUE(canvas->hiddenBuses().isEmpty());
    EXPECT_FALSE(canvas->hasCollapsedHierarchy());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("BusReplacementProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    QApplication::processEvents();
    EXPECT_FALSE(canvas->hasCollapsedBuses());
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(pm.getMasterBusItem()->guid));
}

TEST_F(BlueprintOutGraphTest, LayerFilterKeepsOriginWhileHidingDescendantsAndPreservingBuses)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* master = pm.getMasterBusItem();
    ASSERT_NE(master, nullptr);
    auto* outputParent = pm.createItem(master, QStringLiteral("OutputParent"), T::Bus);
    auto* output = pm.createItem(outputParent, QStringLiteral("Output"), T::Bus);
    auto* sendParent = pm.createItem(master, QStringLiteral("SendParent"), T::Bus);
    auto* send = pm.createItem(sendParent, QStringLiteral("Send"), T::Bus);
    auto* unrelatedBus = pm.createItem(master, QStringLiteral("UnrelatedBus"), T::Bus);
    auto* ancestor = pm.createItem(nullptr, QStringLiteral("Ancestor"), T::Layer);
    auto* selected = pm.createItem(ancestor, QStringLiteral("Selected"), T::Layer);
    auto* sibling = pm.createItem(ancestor, QStringLiteral("Sibling"), T::Sound);
    auto* container = pm.createItem(selected, QStringLiteral("Container"), T::Container);
    auto* track = pm.createItem(container, QStringLiteral("Track"), T::Track);
    auto* sound = pm.createItem(track, QStringLiteral("Sound"), T::Sound);
    auto* piece = pm.createItem(selected, QStringLiteral("Piece"), T::Piece);
    auto* empty = pm.createItem(selected, QStringLiteral("Empty"), T::Container);
    auto* otherLayer = pm.createItem(nullptr, QStringLiteral("OtherLayer"), T::Layer);
    auto* otherSound = pm.createItem(otherLayer, QStringLiteral("OtherSound"), T::Sound);
    for (auto* item : {outputParent, output, sendParent, send, unrelatedBus, ancestor,
                      selected, sibling, container, track, sound, piece, empty, otherLayer, otherSound})
        ASSERT_NE(item, nullptr);
    ASSERT_TRUE(setMainOutputLocal(ancestor, output));
    ASSERT_TRUE(setMainOutputLocal(otherLayer, unrelatedBus));
    auto groups = sound->fieldGroups;
    groups.send.shared = {VNX::FieldGroupMode::Local,
        VNX::SendFieldGroup{{{VNX::toFieldGroupGuid(send->guid), -6.0f}}}};
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(sound, groups));

    VNX::BlueprintCanvasWidget canvas;
    ASSERT_NE(canvas.activeSignalPathSource(), nullptr);
    ASSERT_TRUE(canvas.activeSignalPathSource()->graph().ok());
    const auto original = graphObjects(canvas);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    const size_t edgeCount = graph.edges.size();
    canvas.setHierarchyVisible(selected->objectID, false);
    EXPECT_EQ(graphObjects(canvas), original);
    EXPECT_EQ(graph.edges.size(), edgeCount);
    const QSet<QUuid> hidden{container->guid, track->guid,
        sound->guid, piece->guid, empty->guid};
    EXPECT_EQ(visibleGraphObjects(canvas), original - hidden);
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        const auto& from = graph.nodes[edge.from.occurrenceIndex];
        const auto& to = graph.nodes[edge.to.occurrenceIndex];
        if (from.type == T::Bus && to.type == T::Bus) {
            EXPECT_TRUE(canvas.signalPathEdge(index)->isVisible());
            if (auto* gain = canvas.signalPathGainNode(index)) EXPECT_TRUE(gain->isVisible());
        }
        if (hidden.contains(from.key.objectGuid) || hidden.contains(to.key.objectGuid)) {
            EXPECT_FALSE(canvas.signalPathEdge(index)->isVisible());
            if (auto* gain = canvas.signalPathGainNode(index)) EXPECT_FALSE(gain->isVisible());
            if (auto* outputEdge = canvas.signalPathGainOutputEdge(index)) EXPECT_FALSE(outputEdge->isVisible());
        }
    }
    canvas.setHierarchyVisible(otherLayer->objectID, false);
    EXPECT_EQ(visibleGraphObjects(canvas), original - hidden - QSet<QUuid>{otherSound->guid});
    canvas.setHierarchyVisible(selected->objectID, true);
    EXPECT_TRUE(visibleGraphObjects(canvas).contains(sound->guid));
    EXPECT_FALSE(visibleGraphObjects(canvas).contains(otherSound->guid));
    EXPECT_TRUE(visibleGraphObjects(canvas).contains(otherLayer->guid));
    canvas.setHierarchyVisible(sound->objectID, false);
    canvas.setHierarchyVisible(master->objectID, false);
    EXPECT_EQ(canvas.hiddenHierarchyRoots(), QSet<QUuid>{otherLayer->guid});
}

TEST_F(BlueprintOutGraphTest, NestedHierarchyVisibilityRestoresAncestorsAndKeepsIndependentExclusions)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* parent = pm.createItem(nullptr, QStringLiteral("Parent"), T::Layer);
    auto* container = pm.createItem(parent, QStringLiteral("Container"), T::Container);
    auto* track = pm.createItem(container, QStringLiteral("Track"), T::Track);
    auto* piece = pm.createItem(track, QStringLiteral("Piece"), T::Piece);
    auto* nested = pm.createItem(piece, QStringLiteral("Nested"), T::Track);
    auto* child = pm.createItem(nested, QStringLiteral("Child"), T::Sound);
    auto* sibling = pm.createItem(parent, QStringLiteral("Sibling"), T::Sound);
    auto* independent = pm.createItem(nullptr, QStringLiteral("Independent"), T::Layer);
    auto* independentSound = pm.createItem(independent, QStringLiteral("IndependentSound"), T::Sound);
    for (auto* item : {parent, container, track, piece, nested, child, sibling, independent, independentSound})
        ASSERT_NE(item, nullptr);
    VNX::BlueprintPanel panel;
    auto* canvas = panel.canvas();
    canvas->setHierarchyVisible(independent->objectID, false);
    const std::array chain{parent, container, track, piece, nested};
    for (int index = static_cast<int>(chain.size()) - 1; index >= 0; --index) {
        SCOPED_TRACE(chain[index]->name.toStdString());
        canvas->setHierarchyVisible(chain[index]->objectID, false);
        const auto visible = visibleGraphObjects(*canvas);
        for (int depth = 0; depth < static_cast<int>(chain.size()); ++depth) {
            EXPECT_EQ(visible.contains(chain[depth]->guid), depth <= index);
            EXPECT_EQ(canvas->hierarchyVisibility(chain[depth]->objectID),
                depth < index ? Qt::Checked : Qt::Unchecked);
            EXPECT_EQ(canvas->chainVisibility(chain[depth]->objectID),
                depth < index ? Qt::PartiallyChecked : Qt::Unchecked);
        }
        EXPECT_EQ(visible.contains(sibling->guid), index > 0);
        EXPECT_FALSE(visible.contains(child->guid));
        EXPECT_FALSE(visible.contains(independentSound->guid));
    }
    canvas->setHierarchyVisible(nested->objectID, true);
    for (auto* item : chain)
        EXPECT_EQ(canvas->hierarchyVisibility(item->objectID), item == parent ? Qt::PartiallyChecked : Qt::Checked);
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(child->guid));
    EXPECT_FALSE(visibleGraphObjects(*canvas).contains(sibling->guid));
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(independent->guid));
    EXPECT_FALSE(visibleGraphObjects(*canvas).contains(independentSound->guid));
    canvas->setChainVisible(parent->objectID, true);
    EXPECT_EQ(canvas->chainVisibility(parent->objectID), Qt::Checked);
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(sibling->guid));
    for (size_t index = 1; index < chain.size(); ++index) {
        const auto* target = chain[index - 1];
        SCOPED_TRACE(target->name.toStdString());
        canvas->setHierarchyVisible(chain[index]->objectID, false);
        EXPECT_EQ(canvas->hierarchyVisibility(target->objectID), Qt::Checked);
        EXPECT_EQ(canvas->chainVisibility(target->objectID), Qt::PartiallyChecked);
        canvas->setHierarchyVisible(target->objectID, true);
        EXPECT_FALSE(visibleGraphObjects(*canvas).contains(child->guid));
        canvas->setChainVisible(target->objectID, true);
        EXPECT_TRUE(visibleGraphObjects(*canvas).contains(child->guid));
        EXPECT_EQ(canvas->chainVisibility(target->objectID), Qt::Checked);
        EXPECT_FALSE(visibleGraphObjects(*canvas).contains(independentSound->guid));
    }
}

TEST_F(BlueprintOutGraphTest, CollapsedHierarchyRetainsDownstreamAncestryRoutingAndGainNodes)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* parent = pm.createItem(nullptr, QStringLiteral("Parent"), T::Layer);
    auto* master = pm.getMasterBusItem();
    auto* output = pm.createItem(master, QStringLiteral("Output"), T::Bus);
    for (auto* item : {parent, master, output}) ASSERT_NE(item, nullptr);
    ASSERT_TRUE(setMainOutputLocal(parent, output));
    for (const auto type : {T::Layer, T::Container, T::Track, T::Piece}) {
        const auto name = VNX::ItemTypeCatalog::displayName(type);
        SCOPED_TRACE(name.toStdString());
        auto* nested = pm.createItem(parent, name, type);
        auto* input = pm.createItem(nested, name + QStringLiteral("Input"),
            type == T::Track ? T::Container : T::Track);
        auto* sound = pm.createItem(input, name + QStringLiteral("Sound"), T::Sound);
        for (auto* item : {nested, input, sound}) ASSERT_NE(item, nullptr);
        VNX::BlueprintCanvasWidget canvas;
        const auto& graph = canvas.activeSignalPathSource()->graph();
        canvas.setHierarchyVisible(nested->objectID, false);
        const auto visible = visibleGraphObjects(canvas);
        for (auto* item : {nested, parent, output}) EXPECT_TRUE(visible.contains(item->guid));
        const QSet<QUuid> hidden{input->guid, sound->guid};
        EXPECT_FALSE(visible.intersects(hidden));
        size_t retainedAncestryEdges = 0;
        size_t retainedOutputEdges = 0;
        size_t hiddenInputEdges = 0;
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            const auto& edge = graph.edges[index];
            const auto from = graph.nodes[edge.from.occurrenceIndex].key.objectGuid;
            const auto to = graph.nodes[edge.to.occurrenceIndex].key.objectGuid;
            if (hidden.contains(from) || hidden.contains(to)) {
                ++hiddenInputEdges;
                EXPECT_FALSE(canvas.signalPathEdge(index)->isVisible());
                if (auto* gain = canvas.signalPathGainNode(index)) EXPECT_FALSE(gain->isVisible());
                if (auto* outputEdge = canvas.signalPathGainOutputEdge(index)) EXPECT_FALSE(outputEdge->isVisible());
            }
            if (from == nested->guid && to == parent->guid) {
                ++retainedAncestryEdges;
                EXPECT_TRUE(canvas.signalPathEdge(index)->isVisible());
            }
            if (from != parent->guid || to != output->guid) continue;
            ++retainedOutputEdges;
            EXPECT_TRUE(canvas.signalPathEdge(index)->isVisible());
            ASSERT_NE(canvas.signalPathGainNode(index), nullptr);
            EXPECT_TRUE(canvas.signalPathGainNode(index)->isVisible());
            ASSERT_NE(canvas.signalPathGainOutputEdge(index), nullptr);
            EXPECT_TRUE(canvas.signalPathGainOutputEdge(index)->isVisible());
        }
        EXPECT_GT(retainedAncestryEdges, 0u);
        EXPECT_GT(retainedOutputEdges, 0u);
        EXPECT_GT(hiddenInputEdges, 0u);
        canvas.setHierarchyVisible(nested->objectID, true);
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            EXPECT_TRUE(canvas.signalPathEdge(index)->isVisible());
            if (auto* gain = canvas.signalPathGainNode(index)) EXPECT_TRUE(gain->isVisible());
        }
    }
}

TEST_F(BlueprintOutGraphTest, HiddenLayersTrackRenamesCreationDeletionAndProjectReset)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* selected = pm.createItem(nullptr, QStringLiteral("Selected"), T::Layer);
    auto* other = pm.createItem(nullptr, QStringLiteral("Other"), T::Layer);
    ASSERT_NE(selected, nullptr);
    ASSERT_NE(other, nullptr);
    ASSERT_NE(pm.createItem(selected, QStringLiteral("InitialSound"), T::Sound), nullptr);
    ASSERT_NE(pm.createItem(other, QStringLiteral("OtherSound"), T::Sound), nullptr);
    const QUuid selectedGuid = selected->guid;
    const uint32_t selectedId = selected->objectID;
    VNX::BlueprintPanel panel;
    auto* canvas = panel.canvas();
    canvas->setHierarchyVisible(selectedId, false);
    QApplication::processEvents();
    ASSERT_TRUE(pm.renameItem(selected, QStringLiteral("Renamed")));
    auto* child = pm.createItem(selected, QStringLiteral("NewChild"), T::Sound);
    auto* newLayer = pm.createItem(nullptr, QStringLiteral("NewLayer"), T::Layer);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(newLayer, nullptr);
    QApplication::processEvents();
    QApplication::processEvents();
    EXPECT_EQ(canvas->hiddenHierarchyRoots(), QSet<QUuid>{selectedGuid});
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(selectedGuid));
    EXPECT_FALSE(visibleGraphObjects(*canvas).contains(child->guid));
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(newLayer->guid));

    ASSERT_TRUE(pm.deleteItem(selected));
    QApplication::processEvents();
    QApplication::processEvents();
    EXPECT_FALSE(canvas->hasCollapsedHierarchy());
    EXPECT_EQ(visibleGraphObjects(*canvas), graphObjects(*canvas));

    canvas->setHierarchyVisible(other->objectID, false);
    pm.closeProject();
    EXPECT_FALSE(canvas->hasCollapsedHierarchy());
    EXPECT_TRUE(canvas->hiddenHierarchyRoots().isEmpty());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("ReplacementProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    auto* replacement = pm.createItem(nullptr, QStringLiteral("ReplacementLayer"), T::Layer);
    ASSERT_NE(replacement, nullptr);
    QApplication::processEvents();
    EXPECT_FALSE(canvas->hasCollapsedHierarchy());
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(replacement->guid));
    QApplication::processEvents();
}

TEST_F(BlueprintOutGraphTest, ViewportFieldsFilteringAndCollapseRemainIndependent)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* layer = pm.createItem(nullptr, QStringLiteral("Selected"), T::Layer);
    auto* other = pm.createItem(nullptr, QStringLiteral("Other"), T::Layer);
    auto* bus = pm.createItem(pm.getMasterBusItem(), QStringLiteral("OtherBus"), T::Bus);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(other, nullptr);
    ASSERT_NE(bus, nullptr);
    ASSERT_TRUE(setMainOutputLocal(other, bus));
    ASSERT_NE(pm.createItem(layer, QStringLiteral("Sound"), T::Sound), nullptr);
    VNX::BlueprintPanel panel;
    auto* canvas = panel.canvas();
    const auto originalFields = canvas->nodeContentFields();
    const auto restore = qScopeGuard([&]() { canvas->setNodeContentFields(originalFields); });
    canvas->setNodeContentFields(VNX::kAllNodeContentFields);
    canvas->setHierarchyVisible(layer->objectID, false);
    canvas->setBusVisible(bus->objectID, false);
    EXPECT_EQ(canvas->nodeContentFields(), VNX::kAllNodeContentFields);
    const auto selectedObjects = visibleGraphObjects(*canvas);
    auto* waveform = panel.findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Waveform"));
    auto* volume = panel.findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Volume"));
    auto* chain = panel.findChild<QToolButton*>(QStringLiteral("BlueprintViewportFilterButton"));
    auto* properties = panel.findChild<QToolButton*>(QStringLiteral("BlueprintViewportVisibilityButton"));
    ASSERT_NE(waveform, nullptr);
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(chain, nullptr);
    ASSERT_NE(properties, nullptr);
    waveform->click();
    EXPECT_FALSE(canvas->nodeContentFields().testFlag(VNX::NodeContentField::Waveform));
    EXPECT_TRUE(properties->isChecked());
    volume->click();
    EXPECT_FALSE(canvas->nodeContentFields().testFlag(VNX::NodeContentField::Volume));
    EXPECT_FALSE(canvas->nodeContentFields().testFlag(VNX::NodeContentField::Waveform));
    EXPECT_TRUE(properties->isChecked());
    waveform->click();
    EXPECT_TRUE(canvas->nodeContentFields().testFlag(VNX::NodeContentField::Waveform));
    EXPECT_TRUE(properties->isChecked());
    EXPECT_FALSE(chain->isChecked());
    canvas->setSoundsFiltered(true);
    EXPECT_TRUE(chain->isChecked());
    canvas->setSoundsFiltered(false);
    EXPECT_TRUE(canvas->hasCollapsedHierarchy());
    EXPECT_EQ(canvas->hiddenHierarchyRoots(), QSet<QUuid>{layer->guid});
    EXPECT_EQ(canvas->hiddenBuses(), QSet<QUuid>{bus->guid});
    EXPECT_EQ(visibleGraphObjects(*canvas), selectedObjects);
    canvas->setHierarchyVisible(layer->objectID, true);
    EXPECT_TRUE(canvas->nodeContentFields().testFlag(VNX::NodeContentField::Waveform));
    EXPECT_FALSE(canvas->nodeContentFields().testFlag(VNX::NodeContentField::Volume));
    EXPECT_FALSE(visibleGraphObjects(*canvas).contains(other->guid));
    canvas->setBusVisible(bus->objectID, true);
    EXPECT_TRUE(visibleGraphObjects(*canvas).contains(other->guid));
    EXPECT_FALSE(chain->isChecked());
    EXPECT_TRUE(properties->isChecked());
}

TEST_F(BlueprintOutGraphTest, ObjectFilteringPreservesRoutingAndDormantCollapseState)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* parent = pm.createItem(nullptr, QStringLiteral("Parent"), T::Layer);
    auto* child = pm.createItem(parent, QStringLiteral("Child"), T::Container);
    auto* sound = pm.createItem(child, QStringLiteral("Sound"), T::Sound);
    auto* direct = pm.createItem(nullptr, QStringLiteral("Direct"), T::Sound);
    auto* bus = pm.createItem(pm.getMasterBusItem(), QStringLiteral("Bus"), T::Bus);
    auto* send = pm.createItem(pm.getMasterBusItem(), QStringLiteral("Send"), T::Bus);
    for (auto* item : {parent, child, sound, direct, bus, send}) ASSERT_NE(item, nullptr);
    ASSERT_TRUE(setMainOutputLocal(direct, bus));
    ASSERT_TRUE(setSendLocal(sound, send));
    VNX::BlueprintCanvasWidget canvas;
    const auto& graph = canvas.activeSignalPathSource()->graph();
    const auto original = graphObjects(canvas);
    const auto edges = graph.edges.size();
    const auto soundGroups = sound->fieldGroups;
    canvas.setSoundsFiltered(true);
    EXPECT_EQ(visibleGraphObjects(canvas), original - QSet<QUuid>({sound->guid, direct->guid}));
    EXPECT_EQ(graph.edges.size(), edges);
    EXPECT_TRUE(canvas.canCollapseChain(parent->objectID));
    EXPECT_FALSE(canvas.canCollapseChain(child->objectID));
    EXPECT_FALSE(canvas.canCollapseChain(bus->objectID));
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        const bool visible = graph.nodes[edge.from.occurrenceIndex].type != T::Sound
            && graph.nodes[edge.to.occurrenceIndex].type != T::Sound;
        EXPECT_EQ(canvas.signalPathEdge(index)->isVisible(), visible);
        if (auto* gain = canvas.signalPathGainNode(index)) EXPECT_EQ(gain->isVisible(), visible);
        if (auto* output = canvas.signalPathGainOutputEdge(index)) EXPECT_EQ(output->isVisible(), visible);
    }
    canvas.setSoundsFiltered(false);
    canvas.setHierarchyVisible(child->objectID, false);
    const auto collapsed = visibleGraphObjects(canvas);
    canvas.setSoundsFiltered(true);
    EXPECT_FALSE(canvas.canCollapseChain(child->objectID));
    EXPECT_EQ(canvas.hiddenHierarchyRoots(), QSet<QUuid>{child->guid});
    canvas.setChainVisible(parent->objectID, false);
    canvas.setHierarchyVisible(parent->objectID, true);
    EXPECT_TRUE(visibleGraphObjects(canvas).contains(child->guid));
    EXPECT_EQ(canvas.hiddenHierarchyRoots(), QSet<QUuid>{child->guid});
    canvas.setSoundsFiltered(false);
    EXPECT_EQ(visibleGraphObjects(canvas), collapsed);
    EXPECT_EQ(canvas.hierarchyVisibility(child->objectID), Qt::Unchecked);
    canvas.setBusVisible(bus->objectID, false);
    canvas.setSoundsFiltered(true);
    canvas.setChainVisible(pm.getMasterBusItem()->objectID, false);
    canvas.setChainVisible(pm.getMasterBusItem()->objectID, true);
    EXPECT_TRUE(canvas.hiddenBuses().contains(bus->guid));
    EXPECT_TRUE(visibleGraphObjects(canvas).contains(bus->guid));
    EXPECT_FALSE(canvas.canCollapseChain(bus->objectID));
    canvas.setSoundsFiltered(false);
    EXPECT_FALSE(visibleGraphObjects(canvas).contains(direct->guid));
    EXPECT_EQ(sound->fieldGroups, soundGroups);
    EXPECT_EQ(graphObjects(canvas), original);
}

TEST_F(BlueprintOutGraphTest, NavigationRevealsCollapsedTargetsWithoutOverridingFiltersOrOtherBranches)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* parent = pm.createItem(nullptr, QStringLiteral("Parent"), T::Layer);
    auto* child = pm.createItem(parent, QStringLiteral("Child"), T::Container);
    auto* sibling = pm.createItem(parent, QStringLiteral("Sibling"), T::Container);
    auto* sound = pm.createItem(child, QStringLiteral("Sound"), T::Sound);
    auto* otherSound = pm.createItem(sibling, QStringLiteral("OtherSound"), T::Sound);
    auto* bus = pm.createItem(pm.getMasterBusItem(), QStringLiteral("Bus"), T::Bus);
    for (auto* item : {parent, child, sibling, sound, otherSound, bus}) ASSERT_NE(item, nullptr);
    ASSERT_TRUE(setMainOutputLocal(parent, bus));
    VNX::BlueprintCanvasWidget canvas;
    canvas.setChainVisible(bus->objectID, false);
    ASSERT_TRUE(canvas.focusItem(child));
    EXPECT_TRUE(visibleGraphObjects(canvas).contains(child->guid));
    EXPECT_FALSE(visibleGraphObjects(canvas).contains(sound->guid));
    EXPECT_FALSE(visibleGraphObjects(canvas).contains(sibling->guid));
    EXPECT_EQ(canvas.hierarchyVisibility(child->objectID), Qt::Unchecked);
    canvas.setSoundsFiltered(true);
    const auto hidden = canvas.hiddenHierarchyRoots();
    const auto buses = canvas.hiddenBuses();
    const auto visible = visibleGraphObjects(canvas);
    EXPECT_FALSE(canvas.focusItem(sound));
    EXPECT_EQ(canvas.hiddenHierarchyRoots(), hidden);
    EXPECT_EQ(canvas.hiddenBuses(), buses);
    EXPECT_EQ(visibleGraphObjects(canvas), visible);
    canvas.setSoundsFiltered(false);
    EXPECT_TRUE(canvas.focusItem(sound));
    EXPECT_TRUE(visibleGraphObjects(canvas).contains(sound->guid));
    EXPECT_FALSE(visibleGraphObjects(canvas).contains(otherSound->guid));
    EXPECT_EQ(canvas.selectedProjectItems(), QList<ProjectItem*>{sound});
}

TEST_F(BlueprintOutGraphTest, EveryTypeBuildsAChainViewNamedByTheCatalog)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);

    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("MatrixLayer"), T::Layer);
    ASSERT_NE(layer, nullptr);

    ProjectItem* variable =
        pm.createItem(nullptr, QStringLiteral("MatrixVariable"), T::Variable);
    const std::vector<ProjectItem*> items{
        layer,
        pm.createItem(layer, QStringLiteral("MatrixSound"), T::Sound),
        pm.createItem(layer, QStringLiteral("MatrixContainer"), T::Container),
        pm.createItem(layer, QStringLiteral("MatrixTrack"), T::Track),
        pm.createItem(layer, QStringLiteral("MatrixPiece"), T::Piece),
        pm.createItem(master, QStringLiteral("MatrixBus"), T::Bus),
        pm.createItem(nullptr, QStringLiteral("MatrixAction"), T::Action),
        variable,
        pm.createItem(nullptr, QStringLiteral("MatrixAttenuation"), T::Attenuation),
    };

    ASSERT_EQ(items.size(), VNX::ItemTypeCatalog::allTypes().size());
    for (ProjectItem* item : items) {
        ASSERT_NE(item, nullptr);
        const VNX::SignalChainView view = pm.buildSignalChainView(item);
        EXPECT_EQ(view.self.name, item->name);
        EXPECT_EQ(view.self.itemType, item->type);
        EXPECT_EQ(view.self.type,
                  VNX::ItemTypeCatalog::blueprintChainName(item->type));
    }

    ASSERT_NE(variable, nullptr);
    EXPECT_EQ(pm.buildSignalChainView(variable).self.type,
              QStringLiteral("Unknown"));
}

TEST_F(BlueprintOutGraphTest, BusEdgeLeavesTheResolvedSlotOwner)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);
    ProjectItem* targetBus = pm.createItem(master, QStringLiteral("OwnerTargetBus"), T::Bus);
    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("OwnerLayer"), T::Layer);
    ASSERT_NE(targetBus, nullptr);
    ASSERT_NE(layer, nullptr);
    ProjectItem* container = pm.createItem(layer, QStringLiteral("OwnerContainer"), T::Container);
    ASSERT_NE(container, nullptr);
    ProjectItem* sound = pm.createItem(container, QStringLiteral("OwnerSound"), T::Sound);
    ASSERT_NE(sound, nullptr);

    ASSERT_TRUE(setMainOutputLocal(container, targetBus));

    VNX::SignalChainView view = pm.buildSignalChainView(sound);
    const ProjectItem* owner = Slots::resolveOutputBusSlotOwner(sound);
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(owner, container);
    EXPECT_EQ(view.busSlotOwnerObjectID, owner->objectID);
    EXPECT_EQ(view.busSlotOwnerObjectID, container->objectID);

    auto edges = VNX::SignalChainSource::edgesFromView(view, kAllLayers);
    const VNX::BlueprintEdgeDesc* busEdge = firstItemToBusEdge(edges);
    ASSERT_NE(busEdge, nullptr);
    EXPECT_EQ(busEdge->fromId, container->objectID);
    EXPECT_EQ(busEdge->toId, Slots::resolveEffectiveOutputBusId(sound));
    EXPECT_EQ(busEdge->edgeKind, kindOf(VNX::BlueprintEdgeKind::Override));

    ASSERT_TRUE(setMainOutputLocal(sound, targetBus));
    view = pm.buildSignalChainView(sound);
    EXPECT_EQ(Slots::resolveOutputBusSlotOwner(sound), sound);
    EXPECT_EQ(view.busSlotOwnerObjectID, sound->objectID);
    edges = VNX::SignalChainSource::edgesFromView(view, kAllLayers);
    busEdge = firstItemToBusEdge(edges);
    ASSERT_NE(busEdge, nullptr);
    EXPECT_EQ(busEdge->fromId, sound->objectID);
    EXPECT_EQ(busEdge->toId, Slots::resolveEffectiveOutputBusId(sound));
    EXPECT_EQ(busEdge->edgeKind, kindOf(VNX::BlueprintEdgeKind::Override));

    auto soundGroups = sound->fieldGroups;
    soundGroups.mainOutput.shared.mode = VNX::FieldGroupMode::Inherit;
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(sound, soundGroups));
    auto containerGroups = container->fieldGroups;
    containerGroups.mainOutput.shared.mode = VNX::FieldGroupMode::Inherit;
    ASSERT_TRUE(pm.restoreFieldGroupsInternal(container, containerGroups));
    view = pm.buildSignalChainView(sound);
    EXPECT_EQ(Slots::resolveOutputBusSlotOwner(sound), layer);
    EXPECT_EQ(view.busSlotOwnerObjectID, layer->objectID);
    EXPECT_EQ(Slots::resolveEffectiveOutputBusId(sound), master->objectID);
    edges = VNX::SignalChainSource::edgesFromView(view, kAllLayers);
    busEdge = firstItemToBusEdge(edges);
    ASSERT_NE(busEdge, nullptr);
    EXPECT_EQ(busEdge->fromId, layer->objectID);
    EXPECT_EQ(busEdge->toId, master->objectID);
    EXPECT_EQ(busEdge->edgeKind, kindOf(VNX::BlueprintEdgeKind::Override));
    EXPECT_EQ(layer->fieldGroups.mainOutput.shared.mode, VNX::FieldGroupMode::Inherit);
    const auto rootView = pm.buildSignalChainView(layer);
    EXPECT_EQ(rootView.busSlotOwnerObjectID, layer->objectID);
    EXPECT_EQ(Slots::resolveOutputBusSlotOwner(layer), layer);
}

TEST_F(BlueprintOutGraphTest, MainOutputLocalColorsTheBusEdge)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);
    ProjectItem* targetBus = pm.createItem(master, QStringLiteral("OverrideBus"), T::Bus);
    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("OverrideLayer"), T::Layer);
    ASSERT_NE(targetBus, nullptr);
    ASSERT_NE(layer, nullptr);
    ProjectItem* sound = pm.createItem(layer, QStringLiteral("OverrideSound"), T::Sound);
    ASSERT_NE(sound, nullptr);

    ASSERT_TRUE(setMainOutputLocal(sound, targetBus));

    const VNX::SignalChainView view = pm.buildSignalChainView(sound);
    EXPECT_TRUE(view.self.outputBusOverride);
    EXPECT_EQ(view.busSlotOwnerObjectID, sound->objectID);

    const auto edges = VNX::SignalChainSource::edgesFromView(view, kAllLayers);
    const VNX::BlueprintEdgeDesc* busEdge = firstItemToBusEdge(edges);
    ASSERT_NE(busEdge, nullptr);
    EXPECT_EQ(busEdge->fromId, sound->objectID);
    EXPECT_EQ(busEdge->edgeKind, kindOf(VNX::BlueprintEdgeKind::Override));
}

TEST_F(BlueprintOutGraphTest, MixingCapableAncestorsEnterTheParentChain)
{
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_NE(pm.currentProject(), nullptr);

    ProjectItem layer{QStringLiteral("ChainLayer"), T::Layer};
    ProjectItem sentinel{QStringLiteral("SyntheticRoot"), T::None};
    ProjectItem container{QStringLiteral("ChainContainer"), T::Container};
    ProjectItem track{QStringLiteral("ChainTrack"), T::Track};
    ProjectItem piece{QStringLiteral("ChainPiece"), T::Piece};
    ProjectItem leaf{QStringLiteral("ChainSound"), T::Sound};
    layer.objectID     = 9001;
    sentinel.objectID    = 9002;
    container.objectID = 9003;
    track.objectID     = 9004;
    piece.objectID     = 9005;
    leaf.objectID      = 9006;
    sentinel.parent    = &layer;
    container.parent = &sentinel;
    track.parent     = &container;
    piece.parent     = &track;
    leaf.parent      = &piece;

    const VNX::SignalChainView view =
        VNX::ProjectManager::buildSignalChainView(pm.currentProject(), &leaf);

    ASSERT_EQ(view.organizationChain.size(), 4u);
    EXPECT_EQ(view.organizationChain[0].objectID, piece.objectID);
    EXPECT_EQ(view.organizationChain[1].objectID, track.objectID);
    EXPECT_EQ(view.organizationChain[2].objectID, container.objectID);
    EXPECT_EQ(view.organizationChain[3].objectID, layer.objectID);
    EXPECT_FALSE(chainHas(view.organizationChain, sentinel.objectID));
}

TEST(BlueprintOutGraphEdgeKindTest, ContainmentBusRouteAndSendGoldTable)
{
    VNX::SignalChainView view;
    view.self.objectID = 1;
    view.self.name     = QStringLiteral("Self");
    view.self.itemType = T::Container;

    VNX::SignalChainNode child;
    child.objectID = 2;
    child.name     = QStringLiteral("Child");
    child.itemType = T::Sound;
    view.children.push_back(child);

    VNX::BusChainNode routed;
    routed.busID = 10;
    routed.name  = QStringLiteral("Routed");
    routed.sends.push_back(VNX::SendViewInfo{90, QStringLiteral("Reverb"), -6.0f});
    VNX::BusChainNode master;
    master.busID = 11;
    master.name  = QStringLiteral("Master");
    view.busChain.push_back(routed);
    view.busChain.push_back(master);
    view.busSlotOwnerObjectID = 1;

    const auto edges = VNX::SignalChainSource::edgesFromView(view, kAllLayers);

    const VNX::BlueprintEdgeDesc* containment =
        findEdge(edges, VNX::BlueprintNodeKind::ProjectItem, 2,
                 VNX::BlueprintNodeKind::ProjectItem, 1);
    ASSERT_NE(containment, nullptr);
    EXPECT_EQ(containment->edgeKind, kindOf(VNX::BlueprintEdgeKind::Containment));

    const VNX::BlueprintEdgeDesc* busRoute =
        findEdge(edges, VNX::BlueprintNodeKind::ProjectItem, 1,
                 VNX::BlueprintNodeKind::Bus, 10);
    ASSERT_NE(busRoute, nullptr);
    EXPECT_EQ(busRoute->edgeKind, kindOf(VNX::BlueprintEdgeKind::BusRoute));

    const VNX::BlueprintEdgeDesc* busHop =
        findEdge(edges, VNX::BlueprintNodeKind::Bus, 10,
                 VNX::BlueprintNodeKind::Bus, 11);
    ASSERT_NE(busHop, nullptr);
    EXPECT_EQ(busHop->edgeKind, kindOf(VNX::BlueprintEdgeKind::BusRoute));

    const VNX::BlueprintEdgeDesc* send =
        findEdge(edges, VNX::BlueprintNodeKind::Bus, 10,
                 VNX::BlueprintNodeKind::Bus, 90);
    ASSERT_NE(send, nullptr);
    EXPECT_EQ(send->edgeKind, kindOf(VNX::BlueprintEdgeKind::Send));
    EXPECT_EQ(send->label, QStringLiteral("Send (-6.0 dB)"));
}

TEST(BlueprintOutGraphEdgeKindTest, MissingSlotOwnerFallsBackToTheLastContentCard)
{
    VNX::SignalChainView view;
    view.self.objectID = 1;
    view.self.name     = QStringLiteral("Self");
    view.self.itemType = T::Sound;

    VNX::SignalChainNode hiddenParent;
    hiddenParent.objectID = 7;
    hiddenParent.name     = QStringLiteral("HiddenParent");
    hiddenParent.itemType = T::Layer;
    view.organizationChain.push_back(hiddenParent);

    VNX::BusChainNode routed;
    routed.busID = 10;
    routed.name  = QStringLiteral("Routed");
    view.busChain.push_back(routed);
    view.busSlotOwnerObjectID = 7;

    const std::array<bool, 3> parentsHidden{true, false, true};
    const auto edges = VNX::SignalChainSource::edgesFromView(view, parentsHidden);

    const VNX::BlueprintEdgeDesc* busEdge = firstItemToBusEdge(edges);
    ASSERT_NE(busEdge, nullptr);
    EXPECT_EQ(busEdge->fromId, view.self.objectID);
    EXPECT_EQ(busEdge->edgeKind, kindOf(VNX::BlueprintEdgeKind::BusRoute));
}

TEST(BlueprintOutGraphEdgeKindTest, PlacementAndOrganizationAreIndependentBranches)
{
    VNX::SignalChainView view;
    view.self.objectID = 1;
    view.self.name = QStringLiteral("Self");
    view.self.itemType = T::Sound;

    VNX::SignalChainNode placement;
    placement.objectID = 2;
    placement.name = QStringLiteral("Placement");
    placement.itemType = T::Track;
    view.placementChain.push_back(placement);

    VNX::SignalChainNode organization;
    organization.objectID = 3;
    organization.name = QStringLiteral("Organization");
    organization.itemType = T::Layer;
    view.organizationChain.push_back(organization);

    const auto edges = VNX::SignalChainSource::edgesFromView(view, kAllLayers);
    const auto* placementEdge = findEdge(
        edges, VNX::BlueprintNodeKind::ProjectItem, 1,
        VNX::BlueprintNodeKind::ProjectItem, 2);
    const auto* organizationEdge = findEdge(
        edges, VNX::BlueprintNodeKind::ProjectItem, 1,
        VNX::BlueprintNodeKind::ProjectItem, 3);
    ASSERT_NE(placementEdge, nullptr);
    ASSERT_NE(organizationEdge, nullptr);
    EXPECT_EQ(placementEdge->edgeKind,
              kindOf(VNX::BlueprintEdgeKind::Placement));
    EXPECT_EQ(organizationEdge->edgeKind,
              kindOf(VNX::BlueprintEdgeKind::Organization));
    EXPECT_EQ(findEdge(edges, VNX::BlueprintNodeKind::ProjectItem, 2,
                       VNX::BlueprintNodeKind::ProjectItem, 3), nullptr);
}

TEST_F(BlueprintOutGraphTest, SingletonSourceReplacesPlacementContextAtomically)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);
    ProjectItem* busA = pm.createItem(master, QStringLiteral("BusA"), T::Bus);
    ProjectItem* busB = pm.createItem(master, QStringLiteral("BusB"), T::Bus);
    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("Layer"), T::Layer);
    ProjectItem* sound = pm.createItem(layer, QStringLiteral("Sound"), T::Sound);
    ProjectItem* pieceA = pm.createItem(nullptr, QStringLiteral("PieceA"), T::Piece);
    ProjectItem* pieceB = pm.createItem(nullptr, QStringLiteral("PieceB"), T::Piece);
    ProjectItem* trackA = pm.createItem(pieceA, QStringLiteral("TrackA"), T::Track);
    ProjectItem* trackB = pm.createItem(pieceB, QStringLiteral("TrackB"), T::Track);
    ASSERT_NE(busA, nullptr);
    ASSERT_NE(busB, nullptr);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(trackA, nullptr);
    ASSERT_NE(trackB, nullptr);

    VNX::Clip clipA{QUuid::createUuid(), sound->guid, 0, 1};
    VNX::Clip clipB{QUuid::createUuid(), sound->guid, 0, 1};
    trackA->compOrCreate<VNX::VTrack>()->clips.items.push_back(clipA);
    trackB->compOrCreate<VNX::VTrack>()->clips.items.push_back(clipB);
    ASSERT_TRUE(setMainOutputLocal(layer, busA));
    ASSERT_TRUE(setMainOutputLocal(trackA, busA));
    ASSERT_TRUE(setMainOutputLocal(trackB, busB));

    VNX::SignalChainSource source;
    source.loadItem(sound, VNX::PlacementContext::fromOccurrence(
                               {trackA->guid, clipA.clipGuid, sound->guid}));
    ASSERT_EQ(source.cachedView().placementChain.size(), 2u);
    EXPECT_EQ(source.cachedView().placementChain.front().objectID, trackA->objectID);
    EXPECT_EQ(source.cachedView().busSlotOwnerObjectID, layer->objectID);
    ASSERT_FALSE(source.cachedView().busChain.empty());
    EXPECT_EQ(source.cachedView().busChain.front().busID, busA->objectID);

    source.loadItem(sound, VNX::PlacementContext::fromOccurrence(
                               {trackB->guid, clipB.clipGuid, sound->guid}));
    ASSERT_EQ(source.cachedView().placementChain.size(), 2u);
    EXPECT_EQ(source.cachedView().placementChain.front().objectID, trackB->objectID);
    EXPECT_FALSE(chainHas(source.cachedView().placementChain, trackA->objectID));
    EXPECT_EQ(source.cachedView().busSlotOwnerObjectID, layer->objectID);
    ASSERT_FALSE(source.cachedView().busChain.empty());
    EXPECT_EQ(source.cachedView().busChain.front().busID, busA->objectID);
    ASSERT_FALSE(source.cachedView().slotSources.empty());
    EXPECT_EQ(source.cachedView().slotSources.front().via,
              QStringLiteral("via organization Layer"));

    source.loadItem(sound);
    EXPECT_TRUE(source.cachedView().placementChain.empty());
    ASSERT_EQ(source.cachedView().organizationChain.size(), 1u);
    EXPECT_EQ(source.cachedView().organizationChain.front().objectID, layer->objectID);
    EXPECT_EQ(source.cachedView().busSlotOwnerObjectID, layer->objectID);
}

TEST_F(BlueprintOutGraphTest, InvalidPlacementFallsBackThroughTheSameView)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ProjectItem* bus = pm.createItem(master, QStringLiteral("FallbackBus"), T::Bus);
    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("FallbackLayer"), T::Layer);
    ProjectItem* sound = pm.createItem(layer, QStringLiteral("FallbackSound"), T::Sound);
    ProjectItem* piece = pm.createItem(nullptr, QStringLiteral("HostPiece"), T::Piece);
    ProjectItem* track = pm.createItem(piece, QStringLiteral("HostTrack"), T::Track);
    ASSERT_NE(bus, nullptr);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(track, nullptr);

    VNX::Clip clip{QUuid::createUuid(), sound->guid, 0, 1};
    track->compOrCreate<VNX::VTrack>()->clips.items.push_back(clip);
    ASSERT_TRUE(setMainOutputLocal(track, bus));
    ASSERT_TRUE(setMainOutputLocal(layer, bus));

    VNX::SignalChainSource source;
    source.loadItem(sound, VNX::PlacementContext::fromOccurrence(
                               {track->guid, clip.clipGuid, sound->guid}));
    ASSERT_TRUE(source.cachedView().placementContextValid);
    EXPECT_EQ(source.cachedView().busSlotOwnerObjectID, layer->objectID);
    track->comp<VNX::VTrack>()->clips.items.clear();
    source.refreshView();

    const auto& view = source.cachedView();
    EXPECT_FALSE(view.placementContextValid);
    EXPECT_FALSE(view.placementContextError.isEmpty());
    EXPECT_TRUE(view.placementChain.empty());
    EXPECT_EQ(view.busSlotOwnerObjectID, layer->objectID);
    ASSERT_FALSE(view.slotSources.empty());
    EXPECT_EQ(view.slotSources.front().via,
              QStringLiteral("via organization FallbackLayer"));

}

TEST_F(BlueprintOutGraphTest, LocationPingHighlightsVisibleBodiesWithoutChangingViewOrSelection)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* layer = pm.createItem(nullptr, QStringLiteral("LocationLayer"), T::Layer);
    auto* sound = pm.createItem(layer, QStringLiteral("LocationSound"), T::Sound);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(sound, nullptr);
    LocationPingCanvas canvas;
    canvas.resize(800, 600);
    canvas.show();
    drainEvents();
    canvas.setSceneRect(-5000, -5000, 10000, 10000);

    for (bool chain : {false, true}) {
        SCOPED_TRACE(chain);
        if (chain) canvas.useSignalChain(sound);
        else canvas.rebuildFromProject();
        auto* node = canvas.nodeFor(sound);
        ASSERT_NE(node, nullptr);
        node->setContentFields(VNX::kAllNodeContentFields);
        canvas.setSelectedProjectItems({layer});
        ASSERT_EQ(canvas.selectedProjectItems(), QList<ProjectItem*>({layer}));
        const double zoom = chain ? 0.75 : 1.5;
        canvas.setZoomFactor(zoom);
        canvas.centerOn(0, 0);
        const auto body = node->shape().boundingRect();
        for (bool partial : {false, true}) {
            SCOPED_TRACE(partial);
            const QPoint center(canvas.viewport()->width() / 2,
                partial ? qRound(12.0 - body.height() * zoom * 0.5)
                        : canvas.viewport()->height() / 2);
            node->setPos(canvas.mapToScene(center) - body.center());
            const auto projectedBody = canvas.viewportTransform().mapRect(node->mapRectToScene(body));
            if (partial) {
                ASSERT_LT(projectedBody.top(), 0.0);
                ASSERT_GT(projectedBody.bottom(), 0.0);
            }
            const auto transform = canvas.transform();
            const auto origin = canvas.mapToScene(QPoint());
            const auto selection = canvas.selectedProjectItems();
            ASSERT_TRUE(canvas.pingProjectItem(sound));
            canvas.advancePing();
            const auto painted = paintedBounds(canvas.foreground());
            ASSERT_FALSE(painted.isEmpty());
            EXPECT_TRUE(projectedBody.intersects(QRectF(painted)));
            EXPECT_EQ(canvas.transform(), transform);
            EXPECT_EQ(canvas.mapToScene(QPoint()), origin);
            EXPECT_EQ(canvas.selectedProjectItems(), selection);
        }
    }
}

TEST_F(BlueprintOutGraphTest, LocationPingPointsTowardOffscreenBodiesOutsideObscuredMargins)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* sound = pm.createItem(nullptr, QStringLiteral("LocationSound"), T::Sound);
    ASSERT_NE(sound, nullptr);
    LocationPingCanvas canvas;
    canvas.resize(800, 600);
    canvas.show();
    drainEvents();
    canvas.setSceneRect(-5000, -5000, 10000, 10000);
    canvas.centerOn(0, 0);
    auto* node = canvas.nodeFor(sound);
    ASSERT_NE(node, nullptr);
    const auto body = node->shape().boundingRect();

    for (const auto margins : {QMargins(), QMargins(220, 80, 0, 0)}) {
        SCOPED_TRACE(margins.left());
        canvas.setNavigationViewportMargins(margins);
        const auto available = canvas.viewport()->rect().marginsRemoved(margins);
        for (const auto direction : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1),
                                     QPoint(-1, -1), QPoint(1, -1), QPoint(-1, 1), QPoint(1, 1)}) {
            SCOPED_TRACE(direction.x());
            SCOPED_TRACE(direction.y());
            QPointF center = available.center();
            center.rx() += direction.x() * (available.width() * 0.5 + body.width() * 0.5 + 12);
            center.ry() += direction.y() * (available.height() * 0.5 + body.height() * 0.5 + 12);
            node->setPos(canvas.mapToScene(center.toPoint()) - body.center());
            const auto origin = canvas.mapToScene(QPoint());
            const auto transform = canvas.transform();
            ASSERT_TRUE(canvas.pingProjectItem(sound));
            canvas.advancePing();
            const auto painted = paintedBounds(canvas.foreground());
            ASSERT_FALSE(painted.isEmpty());
            EXPECT_TRUE(available.contains(painted));
            QRect edge = available;
            if (direction.x() < 0) edge.setRight(available.center().x());
            if (direction.x() > 0) edge.setLeft(available.center().x());
            if (direction.y() < 0) edge.setBottom(available.center().y());
            if (direction.y() > 0) edge.setTop(available.center().y());
            EXPECT_TRUE(edge.contains(painted));
            EXPECT_EQ(canvas.mapToScene(QPoint()), origin);
            EXPECT_EQ(canvas.transform(), transform);
        }
    }
}

TEST_F(BlueprintOutGraphTest, LocationPingClearsForUnavailableTargetsAndCanvasLifecycleChanges)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* layer = pm.createItem(nullptr, QStringLiteral("LocationLayer"), T::Layer);
    auto* sound = pm.createItem(layer, QStringLiteral("LocationSound"), T::Sound);
    auto* absent = pm.createItem(nullptr, QStringLiteral("LocationAttenuation"), T::Attenuation);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(absent, nullptr);
    LocationPingCanvas canvas;
    canvas.resize(800, 600);
    canvas.show();
    drainEvents();
    const auto startPing = [&] {
        ASSERT_TRUE(canvas.pingProjectItem(sound));
        canvas.advancePing();
        EXPECT_FALSE(paintedBounds(canvas.foreground()).isEmpty());
    };

    startPing();
    EXPECT_FALSE(canvas.pingProjectItem(absent));
    EXPECT_TRUE(paintedBounds(canvas.foreground()).isEmpty());
    startPing();
    canvas.setHierarchyVisible(layer->objectID, false);
    EXPECT_FALSE(canvas.pingProjectItem(sound));
    EXPECT_TRUE(paintedBounds(canvas.foreground()).isEmpty());
    canvas.setHierarchyVisible(layer->objectID, true);
    startPing();
    canvas.advancePing(1.0);
    EXPECT_TRUE(paintedBounds(canvas.foreground()).isEmpty());
    startPing();
    canvas.hide();
    canvas.show();
    EXPECT_TRUE(paintedBounds(canvas.foreground()).isEmpty());
    startPing();
    canvas.rebuildFromProject();
    EXPECT_TRUE(paintedBounds(canvas.foreground()).isEmpty());
    startPing();
    canvas.clear();
    EXPECT_TRUE(paintedBounds(canvas.foreground()).isEmpty());
    EXPECT_FALSE(canvas.pingProjectItem(sound));
}

TEST_F(BlueprintOutGraphTest, TransitionWatchStateDoesNotPaintOrSynthesizeSceneItems)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);
    ProjectItem* bus = pm.createItem(master, QStringLiteral("WatchBus"), T::Bus);
    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("WatchLayer"), T::Layer);
    ProjectItem* sound = pm.createItem(layer, QStringLiteral("WatchSound"), T::Sound);
    ASSERT_NE(bus, nullptr);
    ASSERT_NE(sound, nullptr);
    layer->compOrCreate<VNX::VMixNode>()->outputBusId = bus->objectID;

    class WatchCanvas : public VNX::BlueprintCanvasWidget {
    public:
        using BlueprintCanvasWidget::drawForeground;
    };
    WatchCanvas canvas;
    canvas.resize(640, 480);
    canvas.useSignalChain(sound);
    ASSERT_NE(canvas.scene(), nullptr);
    const auto baselineItemCount = canvas.scene()->items().size();
    ASSERT_GT(baselineItemCount, 0);

    const auto paintForeground = [&canvas]() {
        QImage image(canvas.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        canvas.drawForeground(&painter, canvas.sceneRect());
        return image;
    };
    const QImage baseline = paintForeground();
    ASSERT_FALSE(baseline.isNull());

    VNX::TransitionSemantics::TransitionWatchView view;
    view.containerGuid = sound->guid;
    view.hasSource = true;
    view.direction = VNX::TransitionSemantics::TransitionWatchDirection::Down;
    const QUuid offCanvasGuid = QUuid::createUuid();
    view.path = {sound->guid, offCanvasGuid};
    view.referencePiece = offCanvasGuid;
    view.caption = QStringLiteral("WatchSound -> Offstage");

    canvas.setTransitionWatch(view);
    EXPECT_EQ(canvas.transitionWatch(), view);
    EXPECT_EQ(canvas.scene()->items().size(), baselineItemCount);
    EXPECT_EQ(paintForeground(), baseline);

    canvas.setTransitionWatch(VNX::TransitionSemantics::TransitionWatchView{});
    EXPECT_EQ(canvas.transitionWatch(), VNX::TransitionSemantics::TransitionWatchView{});
    EXPECT_EQ(canvas.scene()->items().size(), baselineItemCount);
    EXPECT_EQ(paintForeground(), baseline);
}

TEST_F(BlueprintOutGraphTest, TransitionWatchRemainsHiddenAcrossPlacementChanges)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* container = pm.createItem(nullptr, QStringLiteral("WatchContainer"), T::Container);
    ProjectItem* child = pm.createItem(container, QStringLiteral("WatchChild"), T::Sound);
    ProjectItem* pieceA = pm.createItem(nullptr, QStringLiteral("WatchPieceA"), T::Piece);
    ProjectItem* pieceB = pm.createItem(nullptr, QStringLiteral("WatchPieceB"), T::Piece);
    ProjectItem* trackA = pm.createItem(pieceA, QStringLiteral("WatchTrackA"), T::Track);
    ProjectItem* trackB = pm.createItem(pieceB, QStringLiteral("WatchTrackB"), T::Track);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(trackA, nullptr);
    ASSERT_NE(trackB, nullptr);

    container->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Sequence;
    container->comp<VNX::VSelection>()->playType = VNX::SelectionPlayType::Queue;
    const VNX::Clip clipA{QUuid::createUuid(), container->guid, 0, 1};
    const VNX::Clip clipB{QUuid::createUuid(), container->guid, 0, 1};
    trackA->comp<VNX::VTrack>()->clips.items = {clipA};
    trackB->comp<VNX::VTrack>()->clips.items = {clipB};

    VNX::BlueprintPanel panel;
    panel.canvas()->useSignalChain(
        container, VNX::PlacementContext::fromOccurrence(
                       {trackA->guid, clipA.clipGuid, container->guid}));
    EXPECT_EQ(panel.canvas()->transitionWatch(),
              VNX::TransitionSemantics::TransitionWatchView{});

    VNX::TransitionSemantics::TransitionWatchView injected;
    injected.containerGuid = container->guid;
    injected.hasSource = true;
    injected.referencePiece = pieceA->guid;
    injected.caption = QStringLiteral("WatchPieceA");
    panel.canvas()->setTransitionWatch(injected);

    panel.canvas()->useSignalChain(
        container, VNX::PlacementContext::fromOccurrence(
                       {trackB->guid, clipB.clipGuid, container->guid}));
    EXPECT_EQ(panel.canvas()->transitionWatch(),
              VNX::TransitionSemantics::TransitionWatchView{});
}

TEST_F(BlueprintOutGraphTest, GetSignalChainResponseNeverExposesWatchState)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);
    ProjectItem* bus = pm.createItem(master, QStringLiteral("IpcWatchBus"), T::Bus);
    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("IpcWatchLayer"), T::Layer);
    ProjectItem* sound = pm.createItem(layer, QStringLiteral("IpcWatchSound"), T::Sound);
    ASSERT_NE(bus, nullptr);
    ASSERT_NE(sound, nullptr);
    layer->compOrCreate<VNX::VMixNode>()->outputBusId = bus->objectID;

    VNX::BlueprintPanel panel;
    panel.canvas()->useSignalChain(sound);

    VNX::TransitionSemantics::TransitionWatchView view;
    view.containerGuid = sound->guid;
    view.hasSource = true;
    view.direction = VNX::TransitionSemantics::TransitionWatchDirection::Down;
    view.path = {sound->guid};
    view.referencePiece = sound->guid;
    view.caption = QStringLiteral("watch-leak-probe-caption");
    panel.canvas()->setTransitionWatch(view);

    VNX::IPCServer server{nullptr};
    server.setProjectManager(&pm);
    const QJsonObject response = server.dispatchCommand(
        QStringLiteral("get_signal_chain"),
        {{QStringLiteral("object_id"), static_cast<qint64>(VNX::itemShortID(sound))}});

    ASSERT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    EXPECT_FALSE(result.contains(QStringLiteral("watch")));
    const QString serialized = QString::fromUtf8(
        QJsonDocument(result).toJson(QJsonDocument::Compact));
    EXPECT_FALSE(serialized.contains(view.caption));
}
