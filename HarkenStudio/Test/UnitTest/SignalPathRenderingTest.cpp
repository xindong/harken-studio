#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Core/ContextInfo.h"
#include "Widgets/Blueprint/SignalPathSource.h"
#include "Widgets/Blueprint/ConnectionPath.h"
#include "Widgets/Blueprint/NodeContentEditor.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Common/ValueSegmentedControl.h"

#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/Components/VAudio.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSound.h"
#include "Core/Components/VTrack.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QDoubleSpinBox>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsProxyWidget>
#include <QHash>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QSlider>
#include <QStyleOptionSlider>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

using namespace VNX;
using T = ProjectItemType;
constexpr std::array kDirections{CanvasFlowDirection::LR, CanvasFlowDirection::RL,
    CanvasFlowDirection::TB, CanvasFlowDirection::BT};

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char name[] = "harken_editor_tests";
    static char* argv[] = {name, nullptr};
    static QApplication app(argc, argv);
}

class SignalPathRenderingTest : public ::testing::Test
{
protected:
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    uint32_t nextID = 1000;
    ProjectItem* master = create("Master", T::Bus);
    ProjectItem* b1 = create("B1", T::Bus, master);
    ProjectItem* b2 = create("B2", T::Bus, master);
    ProjectItem* b3 = create("B3", T::Bus, master);
    ProjectItem* l3 = create("L3", T::Layer);
    ProjectItem* l1 = create("L1", T::Layer, l3);
    ProjectItem* p1 = create("P1", T::Piece, l1);
    ProjectItem* t1 = create("T1", T::Track, p1);
    ProjectItem* s1 = create("S1", T::Sound, t1);
    ProjectItem* s2 = create("S2", T::Sound, t1);
    ProjectItem* t2 = create("T2", T::Track, p1);
    ProjectItem* s3 = create("S3", T::Sound, t2);
    ProjectItem* c1 = create("C1", T::Container, l1);
    ProjectItem* s4 = create("S4", T::Sound, c1);
    ProjectItem* s5 = create("S5", T::Sound, c1);
    ProjectItem* l2 = create("L2", T::Layer, l3);
    ProjectItem* t3 = create("T3", T::Track, l2);
    ProjectItem* s6 = create("S6", T::Sound, t3);
    QVariant flowSetting;
    QVariant contentSetting;
    QVariant interactionSetting;

    void SetUp() override
    {
        ensureApp();
        flowSetting = QSettings().value(QStringLiteral("Blueprint/CanvasFlowDirection"));
        contentSetting = QSettings().value(QStringLiteral("Blueprint/NodeContentFields"));
        interactionSetting = QSettings().value(QStringLiteral("Blueprint/InteractionMode"));
        output(p1, b1);
        output(c1, b1);
        output(s5, b3);
        output(l2, b1);
        c1->fieldGroups.send.shared = {FieldGroupMode::Local,
            SendFieldGroup{{{toFieldGroupGuid(b2->guid), -6.0f}}}};
    }

    void TearDown() override
    {
        QSettings settings;
        if (flowSetting.isValid())
            settings.setValue(QStringLiteral("Blueprint/CanvasFlowDirection"), flowSetting);
        else
            settings.remove(QStringLiteral("Blueprint/CanvasFlowDirection"));
        if (contentSetting.isValid()) settings.setValue(QStringLiteral("Blueprint/NodeContentFields"), contentSetting);
        else settings.remove(QStringLiteral("Blueprint/NodeContentFields"));
        if (interactionSetting.isValid()) settings.setValue(QStringLiteral("Blueprint/InteractionMode"), interactionSetting);
        else settings.remove(QStringLiteral("Blueprint/InteractionMode"));
    }

    ProjectItem* create(const char* name, T type, ProjectItem* parent = nullptr)
    {
        auto item = std::make_shared<ProjectItem>(QString::fromLatin1(name), type);
        item->guid = QUuid::createUuid();
        item->objectID = nextID++;
        item->guid = VNX::authoringUuid(harkentest::StudioObjectId(item->objectID));
        item->attachDefaultComponents();
        auto* result = item.get();
        if (parent) {
            item->parent = parent;
            item->parentRef = {type == T::Bus ? ParentRef::Kind::Bus : ParentRef::Kind::Item,
                               parent->guid};
            parent->children.push_back(std::move(item));
        } else {
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(type))
                item->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
            auto& bucket = type == T::Bus ? project.busses : project.layers;
            bucket.push_back({std::move(item), {}});
        }
        return result;
    }

    void output(ProjectItem* item, ProjectItem* bus)
    {
        item->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
            MainOutputFieldGroup{toFieldGroupGuid(bus->guid), 0.0f}};
    }

    SignalPathGraph build(FieldGroupScope scope = {}) const
    {
        std::vector<SignalPathIdentity> inputs;
        for (const auto* sound : {s1, s2, s3, s4, s5, s6}) inputs.push_back({sound->guid, {}});
        return buildSignalPathGraph(project, inputs, scope);
    }
};

std::vector<NodeItem*> nodesFor(const BlueprintCanvasWidget& canvas, const ProjectItem* item)
{
    std::vector<NodeItem*> result;
    for (auto* candidate : canvas.scene()->items()) {
        if (auto* node = dynamic_cast<NodeItem*>(candidate); node && node->objectID() == item->objectID)
            result.push_back(node);
    }
    return result;
}

std::vector<ConnectionPath*> connections(const BlueprintCanvasWidget& canvas)
{
    std::vector<ConnectionPath*> result;
    for (auto* item : canvas.scene()->items()) {
        if (auto* edge = dynamic_cast<ConnectionPath*>(item)) result.push_back(edge);
    }
    return result;
}

void canvasMouse(BlueprintCanvasWidget& canvas, QEvent::Type type, QPoint position,
    Qt::MouseButton button = Qt::NoButton, Qt::MouseButtons buttons = Qt::NoButton,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPoint global = canvas.viewport()->mapToGlobal(position);
    QMouseEvent event(type, QPointF(position), QPointF(global), button, buttons, modifiers);
    QApplication::sendEvent(canvas.viewport(), &event);
    QApplication::processEvents();
}

std::optional<QPoint> exposedConnectionPoint(BlueprintCanvasWidget& canvas, ConnectionPath* wire)
{
    canvas.centerOn(wire->path().pointAtPercent(0.5));
    for (int sample = 1; sample < 10; ++sample) {
        const auto point = canvas.mapFromScene(wire->path().pointAtPercent(sample / 10.0));
        if (canvas.viewport()->rect().contains(point) && canvas.itemAt(point) == wire) return point;
    }
    return {};
}

class ConnectionMenuBlueprint : public BlueprintCanvasWidget
{
public:
    void menuAt(QMenu& menu, const QPoint& position)
    {
        QContextMenuEvent event(QContextMenuEvent::Mouse, position, viewport()->mapToGlobal(position));
        populateContextMenu(menu, &event);
    }
};

class SignalPathConnectionInteractionTest : public ::testing::Test
{
protected:
    QTemporaryDir directory;
    QHash<QString, QVariant> preferences;
    ProjectItem* layer = nullptr;
    ProjectItem* sound = nullptr;
    ProjectItem* outputBus = nullptr;
    ProjectItem* sendBus = nullptr;

    void SetUp() override
    {
        ensureApp();
        for (const auto* key : {"Blueprint/InteractionMode", "Blueprint/CanvasFlowDirection",
                 "Blueprint/NodeContentFields", "Blueprint/GridSnapEnabled"})
            preferences.insert(QLatin1String(key), QSettings().value(QLatin1String(key)));
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        ProjectConfig config;
        config.name = QStringLiteral("SignalPathConnectionInteraction");
        config.location = directory.path();
        config.platforms = HarkenTest::platforms({"Windows"});
        ASSERT_TRUE(manager.createProject(config));
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
        outputBus = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Output"), T::Bus);
        sendBus = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Send"), T::Bus);
        layer = manager.createItem(nullptr, QStringLiteral("Layer"), T::Layer);
        ASSERT_NE(outputBus, nullptr);
        ASSERT_NE(sendBus, nullptr);
        ASSERT_NE(layer, nullptr);
        sound = manager.createItem(layer, QStringLiteral("Sound"), T::Sound);
        ASSERT_NE(sound, nullptr);
        sound->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
            MainOutputFieldGroup{toFieldGroupGuid(outputBus->guid), 0.0f}};
        sound->fieldGroups.send.shared = {FieldGroupMode::Local,
            SendFieldGroup{{{toFieldGroupGuid(sendBus->guid), -6.0f}}}};
        UndoRedoManager::instance().clear();
        drain();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        drain();
        PlatformContextService::instance().setStorageForTesting(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings, QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
        QSettings settings;
        for (auto it = preferences.cbegin(); it != preferences.cend(); ++it) {
            if (it.value().isValid()) settings.setValue(it.key(), it.value());
            else settings.remove(it.key());
        }
    }

    static void drain()
    {
        QApplication::processEvents();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    SignalPathGraph graph() const
    {
        return buildSignalPathGraph(*ProjectManager::instance().currentProject(),
            {{sound->guid, {}}}, HarkenTest::FieldGroupPlatformIds::Windows);
    }

    void showCanvas(ConnectionMenuBlueprint& canvas)
    {
        canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
        canvas.setNodeContentFields(kAllNodeContentFields);
        canvas.setFlowDirection(CanvasFlowDirection::LR);
        canvas.resize(1000, 650);
        canvas.show();
        ASSERT_TRUE(canvas.focusItem(sound));
        drain();
    }

    ConnectionPath* edgeOf(BlueprintCanvasWidget& canvas, BlueprintEdgeKind kind,
                           const ProjectItem* target = nullptr) const
    {
        const auto& graph = canvas.activeSignalPathSource()->graph();
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            auto* wire = canvas.signalPathEdge(index);
            if (wire->blueprintEdgeKind() == kind
                && (!target || graph.nodes[graph.edges[index].to.occurrenceIndex].key.objectGuid == target->guid))
                return wire;
        }
        return nullptr;
    }

    static void key(BlueprintCanvasWidget& canvas, int value)
    {
        QKeyEvent event(QEvent::KeyPress, value, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
        drain();
    }

    bool hasSend() const
    {
        const auto current = graph();
        EXPECT_TRUE(current.ok());
        return std::any_of(current.edges.begin(), current.edges.end(),
            [](const auto& edge) { return edge.kind == SignalPathEdgeKind::Send; });
    }
};

TEST_F(SignalPathConnectionInteractionTest, InheritedOutputVolumeHelpNamesTheOwningObject)
{
    auto& manager = ProjectManager::instance();
    auto* outer = manager.createItem(nullptr, QStringLiteral("Outer"), T::Layer);
    auto* middle = manager.createItem(outer, QStringLiteral("Middle"), T::Layer);
    auto* voice = manager.createItem(middle, QStringLiteral("Voice"), T::Sound);
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(middle, nullptr);
    ASSERT_NE(voice, nullptr);
    outer->fieldGroups.mainOutput.shared.mode = FieldGroupMode::Inherit;
    middle->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(outputBus->guid), -3.0f}};
    voice->fieldGroups.mainOutput.shared.mode = FieldGroupMode::Inherit;

    const auto built = buildSignalPathGraph(*manager.currentProject(),
        {{voice->guid, {}}}, HarkenTest::FieldGroupPlatformIds::Windows);
    ASSERT_TRUE(built.ok());
    const auto found = std::find_if(built.edges.begin(), built.edges.end(), [&](const auto& edge) {
        return edge.kind == SignalPathEdgeKind::Output && edge.routingSource
            && edge.routingSource->objectGuid == middle->guid;
    });
    ASSERT_NE(found, built.edges.end());
    const auto index = static_cast<size_t>(found - built.edges.begin());
    EXPECT_EQ(built.nodes[found->from.occurrenceIndex].objectID, outer->objectID);

    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(*manager.currentProject(), built);
    auto* gain = canvas.signalPathGainNode(index);
    ASSERT_NE(gain, nullptr);
    auto* editor = qobject_cast<NodeContentEditor*>(gain->contentWidget());
    ASSERT_NE(editor, nullptr);
    auto* slider = editor->findChild<ParameterControlHost*>(QStringLiteral("nodeSlider_outputBusVolume"));
    ASSERT_NE(slider, nullptr);
    const auto topic = ContextInfo::tagOf(slider);
    ASSERT_TRUE(topic);
    EXPECT_EQ(topic->objectGuid, middle->guid);
    EXPECT_EQ(topic->objectName, middle->name);
    EXPECT_EQ(topic->field, QStringLiteral("outputBusVolume"));
    EXPECT_NE(topic->objectGuid, outer->guid);
}

TEST_F(SignalPathConnectionInteractionTest, MovingNodesKeepsEditorsAndConnectionsAliveAcrossUndoAndReset)
{
    ConnectionMenuBlueprint canvas;
    showCanvas(canvas);
    canvas.setGridSnapEnabled(false);
    auto* node = nodesFor(canvas, sound).front();
    const auto original = node->pos();
    const auto wires = connections(canvas);
    std::vector<QPointer<QWidget>> editors;
    for (auto* item : canvas.scene()->items()) {
        if (auto* candidate = dynamic_cast<NodeItem*>(item); candidate && candidate->contentWidget())
            editors.emplace_back(candidate->contentWidget());
    }
    int rebuilds = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::signalChainViewChanged,
        &canvas, [&] { ++rebuilds; });
    const auto expectRetained = [&] {
        EXPECT_EQ(rebuilds, 0);
        for (const auto& editor : editors) EXPECT_FALSE(editor.isNull());
        const auto current = connections(canvas);
        for (auto* wire : wires) EXPECT_NE(std::find(current.begin(), current.end(), wire), current.end());
    };
    for (int iteration = 0; iteration < 6; ++iteration) {
        canvas.centerOn(node);
        const auto start = canvas.mapFromScene(node->mapToScene(node->titleRect().center()));
        const QPoint offset(0, iteration % 2 == 0 ? 35 : -35);
        const auto before = node->pos();
        canvasMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        canvasMouse(canvas, QEvent::MouseMove, start + offset / 2, Qt::NoButton, Qt::LeftButton);
        canvasMouse(canvas, QEvent::MouseMove, start + offset, Qt::NoButton, Qt::LeftButton);
        canvasMouse(canvas, QEvent::MouseButtonRelease, start + offset, Qt::LeftButton);
        drain();
        expectRetained();
        ASSERT_EQ(nodesFor(canvas, sound).front(), node);
        const auto moved = node->pos();
        EXPECT_NE(moved, before);
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        drain();
        expectRetained();
        EXPECT_EQ(node->pos(), before);
        ASSERT_TRUE(UndoRedoManager::instance().redo());
        drain();
        expectRetained();
        EXPECT_EQ(node->pos(), moved);
    }
    canvas.resetNodeLayout();
    drain();
    expectRetained();
    EXPECT_EQ(node->pos(), original);
}

TEST_F(SignalPathConnectionInteractionTest, DragSnapFollowsVisibleGridAcrossZoomAndUndo)
{
    ConnectionMenuBlueprint canvas;
    showCanvas(canvas);
    auto* node = nodesFor(canvas, sound).front();
    const auto& edges = canvas.activeSignalPathSource()->graph().edges;
    const auto send = std::find_if(edges.begin(), edges.end(),
        [](const auto& edge) { return edge.kind == SignalPathEdgeKind::Send; });
    ASSERT_NE(send, edges.end());
    auto* gain = canvas.signalPathGainNode(static_cast<size_t>(send - edges.begin()));
    ASSERT_NE(gain, nullptr);
    const auto drag = [&](NodeItem* moving, QPoint offset) {
        canvas.scene()->clearSelection();
        canvas.centerOn(moving);
        const QPointF grab = moving->routeGainMode()
            ? QPointF(moving->shape().boundingRect().center().x(), 2.0)
            : moving->titleRect().center();
        const auto start = canvas.mapFromScene(moving->mapToScene(grab));
        canvasMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        canvasMouse(canvas, QEvent::MouseMove, start + offset / 2, Qt::NoButton, Qt::LeftButton);
        canvasMouse(canvas, QEvent::MouseMove, start + offset, Qt::NoButton, Qt::LeftButton);
        const auto raw = moving->pos();
        canvasMouse(canvas, QEvent::MouseButtonRelease, start + offset, Qt::LeftButton);
        drain();
        return raw;
    };
    for (auto* moving : {node, gain}) {
        for (const double zoom : {0.3, 0.6, 1.0, 2.0, 4.0}) {
            SCOPED_TRACE(zoom);
            canvas.setGridSnapEnabled(true);
            const auto original = moving->pos();
            canvas.setZoomFactor(zoom);
            EXPECT_EQ(moving->pos(), original);
            const double spacing = canvas.gridSpacing();
            const auto raw = drag(moving, QPoint(-83, 107));
            ASSERT_NE(raw, original);
            const QPointF snapped(std::round(raw.x() / spacing) * spacing,
                                  std::round(raw.y() / spacing) * spacing);
            EXPECT_EQ(moving->pos(), snapped);
            ASSERT_TRUE(UndoRedoManager::instance().undo());
            drain();
            EXPECT_EQ(moving->pos(), original);
            ASSERT_TRUE(UndoRedoManager::instance().redo());
            drain();
            EXPECT_EQ(moving->pos(), snapped);
            canvas.setGridSnapEnabled(false);
            const auto unsnapped = drag(moving, QPoint(43, -61));
            EXPECT_EQ(moving->pos(), unsnapped);
        }
    }
}

TEST_F(SignalPathConnectionInteractionTest, RoutingAndPropertyEditsCoalesceRefreshRequests)
{
    ConnectionMenuBlueprint canvas;
    showCanvas(canvas);
    int rebuilds = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::signalChainViewChanged,
        &canvas, [&] { ++rebuilds; });
    auto* content = qobject_cast<NodeContentEditor*>(nodesFor(canvas, sound).front()->contentWidget());
    ASSERT_NE(content, nullptr);
    auto* binding = content->binding(NodeContentField::Volume);
    ASSERT_NE(binding, nullptr);
    binding->beginEdit();
    binding->previewEdit(-12.0);
    binding->commitEdit();
    drain();
    EXPECT_EQ(rebuilds, 1);
    const auto& edges = canvas.activeSignalPathSource()->graph().edges;
    const auto send = std::find_if(edges.begin(), edges.end(),
        [](const auto& edge) { return edge.kind == SignalPathEdgeKind::Send; });
    ASSERT_NE(send, edges.end());
    ASSERT_TRUE(canvas.disconnectSignalPathEdges({static_cast<size_t>(send - edges.begin())}).success);
    drain();
    EXPECT_FALSE(hasSend());
    EXPECT_EQ(rebuilds, 2);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drain();
    EXPECT_TRUE(hasSend());
    EXPECT_EQ(rebuilds, 3);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drain();
    EXPECT_FALSE(hasSend());
    EXPECT_EQ(rebuilds, 4);
}

TEST_F(SignalPathConnectionInteractionTest, SendDeletionWorksFromContextMenuAndKeyboardWithUndo)
{
    ConnectionMenuBlueprint canvas;
    showCanvas(canvas);
    auto* wire = edgeOf(canvas, BlueprintEdgeKind::Send);
    ASSERT_NE(wire, nullptr);
    const auto point = exposedConnectionPoint(canvas, wire);
    ASSERT_TRUE(point.has_value());
    QMenu menu;
    canvas.menuAt(menu, *point);
    ASSERT_EQ(menu.actions().size(), 1);
    auto* action = menu.actions().front();
    EXPECT_EQ(action->text(), QStringLiteral("Delete"));
    ASSERT_TRUE(action->isEnabled()) << action->toolTip().toStdString();
    EXPECT_TRUE(wire->isSelected());
    const auto groups = sound->fieldGroups;
    action->trigger();
    drain();
    EXPECT_FALSE(hasSend());
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drain();
    EXPECT_EQ(sound->fieldGroups, groups);
    for (const auto pressed : {Qt::Key_Delete, Qt::Key_Backspace}) {
        auto* selected = edgeOf(canvas, BlueprintEdgeKind::Send);
        ASSERT_NE(selected, nullptr);
        selected->setSelected(true);
        key(canvas, pressed);
        EXPECT_FALSE(hasSend());
        EXPECT_EQ(sound->parent, layer);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        drain();
        EXPECT_TRUE(hasSend());
        EXPECT_EQ(sound->fieldGroups, groups);
    }
}

TEST_F(SignalPathConnectionInteractionTest, MixedDeletionKeepsObjectsAndProtectsMasterOutput)
{
    ConnectionMenuBlueprint canvas;
    showCanvas(canvas);
    auto* through = edgeOf(canvas, BlueprintEdgeKind::Through);
    auto* output = edgeOf(canvas, BlueprintEdgeKind::BusRoute, outputBus);
    auto* masterOutput = edgeOf(canvas, BlueprintEdgeKind::BusRoute,
        ProjectManager::instance().getMasterBusItem());
    ASSERT_NE(through, nullptr);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(masterOutput, nullptr);
    const auto layers = nodesFor(canvas, layer);
    ASSERT_FALSE(layers.empty());
    layers.front()->setSelected(true);
    through->setSelected(true);
    output->setSelected(true);
    masterOutput->setSelected(true);
    edgeOf(canvas, BlueprintEdgeKind::Send)->setSelected(true);
    const auto soundGuid = sound->guid;
    const auto layerGuid = layer->guid;
    key(canvas, Qt::Key_Delete);
    EXPECT_EQ(ProjectManager::instance().findItemByGuid(soundGuid), sound);
    EXPECT_EQ(ProjectManager::instance().findItemByGuid(layerGuid), layer);
    EXPECT_EQ(sound->parent, nullptr);
    EXPECT_FALSE(hasSend());
    EXPECT_EQ(graph().paths.front().routing.outputBusID, ProjectManager::instance().getMasterBusItem()->objectID);
    EXPECT_NE(edgeOf(canvas, BlueprintEdgeKind::BusRoute), nullptr);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drain();
    EXPECT_EQ(sound->parent, layer);
    EXPECT_TRUE(hasSend());
    EXPECT_EQ(graph().paths.front().routing.outputBusID, outputBus->objectID);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drain();
    EXPECT_EQ(sound->parent, nullptr);
    EXPECT_FALSE(hasSend());
}

TEST_F(SignalPathConnectionInteractionTest, DeletingGainOrDownstreamWireDeletesTheBusinessSendOnce)
{
    ConnectionMenuBlueprint canvas;
    showCanvas(canvas);
    canvas.scene()->clearSelection();
    const auto before = sound->fieldGroups;
    for (const bool selectGain : {true, false}) {
        const auto& current = canvas.activeSignalPathSource()->graph();
        const auto found = std::find_if(current.edges.begin(), current.edges.end(),
            [](const auto& edge) { return edge.kind == SignalPathEdgeKind::Send; });
        ASSERT_NE(found, current.edges.end());
        const auto index = static_cast<size_t>(found - current.edges.begin());
        QGraphicsItem* selected = selectGain
            ? static_cast<QGraphicsItem*>(canvas.signalPathGainNode(index))
            : static_cast<QGraphicsItem*>(canvas.signalPathGainOutputEdge(index));
        ASSERT_NE(selected, nullptr);
        selected->setSelected(true);
        EXPECT_EQ(canvas.scene()->selectedItems().size(), 3);
        key(canvas, Qt::Key_Delete);
        EXPECT_FALSE(hasSend());
        EXPECT_EQ(sound->parent, layer);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        drain();
        EXPECT_TRUE(hasSend());
        EXPECT_EQ(sound->fieldGroups, before);
    }
}

TEST_F(SignalPathConnectionInteractionTest, MasterOutputDeletionIsProtectedInKeyboardAndContextMenu)
{
    ConnectionMenuBlueprint canvas;
    showCanvas(canvas);
    auto* output = edgeOf(canvas, BlueprintEdgeKind::BusRoute,
        ProjectManager::instance().getMasterBusItem());
    ASSERT_NE(output, nullptr);
    output->setSelected(true);
    for (const auto pressed : {Qt::Key_Delete, Qt::Key_Backspace}) {
        key(canvas, pressed);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
        EXPECT_TRUE(output->isSelected());
        EXPECT_EQ(sound->parent, layer);
        EXPECT_TRUE(hasSend());
    }
    const auto point = exposedConnectionPoint(canvas, output);
    ASSERT_TRUE(point.has_value());
    QMenu menu;
    canvas.menuAt(menu, *point);
    ASSERT_EQ(menu.actions().size(), 1);
    EXPECT_EQ(menu.actions().front()->text(), QStringLiteral("Delete"));
    EXPECT_FALSE(menu.actions().front()->isEnabled());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathConnectionInteractionTest, NumericEditingConsumesDeleteWithoutDeletingSelectedConnections)
{
    ConnectionMenuBlueprint canvas;
    showCanvas(canvas);
    const auto sounds = nodesFor(canvas, sound);
    ASSERT_EQ(sounds.size(), 1);
    auto* editor = qobject_cast<NodeContentEditor*>(sounds.front()->contentWidget());
    ASSERT_NE(editor, nullptr);
    auto* label = editor->findChild<QLabel*>("nodeValue_pitch");
    auto* input = editor->findChild<QDoubleSpinBox*>("nodeEditor_pitch");
    ASSERT_NE(label, nullptr);
    ASSERT_NE(input, nullptr);
    canvas.centerOn(sounds.front());
    const auto point = canvas.mapFromScene(editor->graphicsProxyWidget()->mapToScene(
        label->mapTo(editor, label->rect().center())));
    canvasMouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    canvasMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton);
    canvasMouse(canvas, QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
    canvasMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton);
    ASSERT_TRUE(input->isVisible());
    ASSERT_TRUE(editor->isEditing());
    auto* wire = edgeOf(canvas, BlueprintEdgeKind::Through);
    ASSERT_NE(wire, nullptr);
    wire->setSelected(true);
    for (const auto pressed : {Qt::Key_Delete, Qt::Key_Backspace}) {
        key(canvas, pressed);
        EXPECT_TRUE(hasSend());
        EXPECT_EQ(sound->parent, layer);
        EXPECT_TRUE(wire->isSelected());
        EXPECT_TRUE(editor->isEditing());
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    }
    editor->finishEditing(false);
    drain();
}

void expectConnections(const BlueprintCanvasWidget& canvas, const SignalPathGraph& graph)
{
    const auto edges = connections(canvas);
    const auto gainCount = std::count_if(graph.edges.begin(), graph.edges.end(),
        [](const auto& edge) { return edge.kind != SignalPathEdgeKind::Through; });
    ASSERT_EQ(edges.size(), graph.edges.size() + gainCount);
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        auto* from = canvas.signalPathNode(edge.from.occurrenceIndex);
        auto* to = canvas.signalPathNode(edge.to.occurrenceIndex);
        ASSERT_NE(from, nullptr);
        ASSERT_NE(to, nullptr);
        const auto kind = edge.kind == SignalPathEdgeKind::Through ? BlueprintEdgeKind::Through
            : edge.kind == SignalPathEdgeKind::Output ? BlueprintEdgeKind::BusRoute : BlueprintEdgeKind::Send;
        const auto* wire = canvas.signalPathEdge(index);
        ASSERT_NE(wire, nullptr);
        EXPECT_EQ(wire->startPin(), from->outputPin());
        EXPECT_EQ(wire->blueprintEdgeKind(), kind);
        const auto* gain = canvas.signalPathGainNode(index);
        const auto* downstream = canvas.signalPathGainOutputEdge(index);
        if (edge.kind == SignalPathEdgeKind::Through) {
            EXPECT_EQ(gain, nullptr);
            EXPECT_EQ(downstream, nullptr);
            EXPECT_EQ(wire->endPin(), to->inputPin());
        } else {
            ASSERT_NE(gain, nullptr);
            ASSERT_NE(downstream, nullptr);
            EXPECT_TRUE(gain->routeGainMode());
            EXPECT_EQ(gain->inputCount(), 1);
            EXPECT_EQ(gain->outputCount(), 1);
            EXPECT_EQ(wire->endPin(), gain->inputPin());
            EXPECT_EQ(downstream->startPin(), gain->outputPin());
            EXPECT_EQ(downstream->endPin(), to->inputPin());
            EXPECT_EQ(downstream->blueprintEdgeKind(), kind);
        }
    }
}

void expectLayout(const BlueprintCanvasWidget& canvas, const SignalPathGraph& graph)
{
    std::vector<const NodeItem*> nodes;
    for (size_t index = 0; index < graph.nodes.size(); ++index)
        nodes.push_back(canvas.signalPathNode(index));
    for (size_t index = 0; index < graph.edges.size(); ++index)
        if (const auto* gain = canvas.signalPathGainNode(index)) nodes.push_back(gain);
    for (size_t index = 0; index < nodes.size(); ++index) {
        const auto* node = nodes[index];
        ASSERT_NE(node, nullptr);
        EXPECT_TRUE(std::isfinite(node->pos().x()));
        EXPECT_TRUE(std::isfinite(node->pos().y()));
        for (size_t other = index + 1; other < nodes.size(); ++other) {
            SCOPED_TRACE(std::to_string(index) + " / " + std::to_string(other));
            EXPECT_FALSE(node->sceneBoundingRect().intersects(nodes[other]->sceneBoundingRect()));
        }
    }
    for (const auto* wire : connections(canvas)) {
        SCOPED_TRACE(static_cast<const NodeItem*>(wire->startPin()->parentItem())->title().toStdString()
            + " -> " + static_cast<const NodeItem*>(wire->endPin()->parentItem())->title().toStdString());
        const auto from = wire->startPin()->scenePos();
        const auto to = wire->endPin()->scenePos();
        switch (canvas.flowDirection()) {
        case CanvasFlowDirection::LR: EXPECT_LT(from.x(), to.x()); break;
        case CanvasFlowDirection::RL: EXPECT_GT(from.x(), to.x()); break;
        case CanvasFlowDirection::TB: EXPECT_LT(from.y(), to.y()); break;
        case CanvasFlowDirection::BT: EXPECT_GT(from.y(), to.y()); break;
        }
        EXPECT_EQ(wire->path().pointAtPercent(0.0), from);
        EXPECT_EQ(wire->path().pointAtPercent(1.0), to);
        ASSERT_EQ(wire->path().elementCount(), 4);
        EXPECT_EQ(wire->path().elementAt(1).type, QPainterPath::CurveToElement);
    }
}

TEST_F(SignalPathRenderingTest, PrototypeMaterializesOneNodeAndOneInterfacePairPerObject)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    ASSERT_NE(canvas.activeSignalPathSource(), nullptr);
    EXPECT_EQ(canvas.activeSignalChainSource(), nullptr);
    ASSERT_EQ(graph.nodes.size(), 18u);
    size_t count = 0, gainCount = 0;
    for (auto* item : canvas.scene()->items()) {
        if (const auto* node = dynamic_cast<NodeItem*>(item)) {
            if (node->routeGainMode()) ++gainCount;
            else ++count;
        }
    }
    EXPECT_EQ(count, graph.nodes.size());
    EXPECT_EQ(gainCount, std::count_if(graph.edges.begin(), graph.edges.end(),
        [](const auto& edge) { return edge.kind != SignalPathEdgeKind::Through; }));
    for (auto* item : {master, b1, b2, b3, l3, l1, p1, t1, s1, s2, t2, s3, c1, s4, s5, l2, t3, s6}) {
        const auto nodes = nodesFor(canvas, item);
        ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(nodes.front()->inputCount(), 1);
        EXPECT_EQ(nodes.front()->outputCount(), 1);
    }
    expectConnections(canvas, graph);
}

TEST_F(SignalPathRenderingTest, PrototypeKeepsDefaultCurvesAndNonoverlappingNodesInEveryDirection)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    for (const auto direction : kDirections) {
        SCOPED_TRACE(static_cast<int>(direction));
        canvas.setFlowDirection(direction);
        expectConnections(canvas, graph);
        expectLayout(canvas, graph);
        for (const auto* node : nodesFor(canvas, c1)) {
            const auto aIn = node->inputPin(0)->scenePos();
            const auto aOut = node->outputPin(0)->scenePos();
            if (direction == CanvasFlowDirection::LR || direction == CanvasFlowDirection::RL) {
                EXPECT_EQ(aIn.y(), aOut.y());
            } else {
                EXPECT_EQ(aIn.x(), aOut.x());
            }
        }
    }
}

TEST_F(SignalPathRenderingTest, AutomaticLayoutAvoidsManualPositionsInEveryDirection)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    SignalPathSource source;
    source.loadGraph(project, graph);
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    for (auto direction : kDirections) {
        SCOPED_TRACE(static_cast<int>(direction));
        canvas.setFlowDirection(direction);
        std::vector<QSizeF> sizes;
        for (size_t index = 0; index < graph.nodes.size(); ++index)
            sizes.push_back(canvas.signalPathNode(index)->shape().boundingRect().size());
        const auto automatic = source.layout(sizes, direction);
        std::vector<std::optional<QPointF>> fixed(graph.nodes.size());
        const auto anchor = static_cast<size_t>(std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [&](const auto& node) { return node.key.objectGuid == c1->guid; }) - graph.nodes.begin());
        ASSERT_LT(anchor, graph.nodes.size());
        fixed[anchor] = automatic.nodePositions.back();
        const auto layout = source.layout(sizes, direction, fixed);
        EXPECT_EQ(layout.nodePositions[anchor], *fixed[anchor]);
        for (size_t index = 0; index < graph.nodes.size(); ++index) {
            const QRectF bounds(layout.nodePositions[index], sizes[index]);
            for (size_t other = index + 1; other < graph.nodes.size(); ++other) {
                SCOPED_TRACE(graph.nodes[index].name.toStdString() + " / " + graph.nodes[other].name.toStdString());
                EXPECT_FALSE(bounds.adjusted(-20, -20, 20, 20)
                    .intersects(QRectF(layout.nodePositions[other], sizes[other])));
            }
        }
        for (const auto& edge : graph.edges) {
            const auto from = edge.from.occurrenceIndex;
            const auto to = edge.to.occurrenceIndex;
            const bool vertical = direction == CanvasFlowDirection::TB || direction == CanvasFlowDirection::BT;
            const bool reverse = direction == CanvasFlowDirection::RL || direction == CanvasFlowDirection::BT;
            const auto leading = [&](size_t index) {
                const auto pos = layout.nodePositions[index];
                const qreal major = vertical ? pos.y() : pos.x();
                const qreal length = vertical ? sizes[index].height() : sizes[index].width();
                return reverse ? -major - length : major;
            };
            EXPECT_LT(leading(from) + (vertical ? sizes[from].height() : sizes[from].width()), leading(to));
        }
        EXPECT_EQ(source.layout(sizes, direction, fixed).nodePositions, layout.nodePositions);
    }
}

TEST_F(SignalPathRenderingTest, FieldFilteringKeepsRoutesContinuousInEveryDirection)
{
    const auto graph = build();
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    for (const auto fields : {NodeContentFields{}, NodeContentFields(NodeContentField::Volume),
            NodeContentFields(NodeContentField::Pitch), NodeContentFields(NodeContentField::OutputVolume),
            NodeContentFields(NodeContentField::OffsetOfStart), NodeContentFields(NodeContentField::Loop),
            NodeContentFields(NodeContentField::Waveform), kAllNodeContentFields}) {
        SCOPED_TRACE(fields.toInt());
        canvas.setNodeContentFields(fields);
        for (const auto direction : kDirections) {
            SCOPED_TRACE(static_cast<int>(direction));
            canvas.setFlowDirection(direction);
            expectConnections(canvas, graph);
            expectLayout(canvas, graph);
            EXPECT_EQ(canvas.signalPathNode(0)->contentFields(), fields);
            for (size_t index = 0; index < graph.edges.size(); ++index) {
                const auto* gain = canvas.signalPathGainNode(index);
                if (!gain) continue;
                const auto field = graph.edges[index].kind == SignalPathEdgeKind::Output
                    ? NodeContentField::OutputVolume : NodeContentField::SendVolume;
                EXPECT_TRUE(gain->contentFields().testFlag(field));
                EXPECT_GT(gain->contentHeight(), 0);
            }
        }
    }
}

TEST_F(SignalPathRenderingTest, ObjectSelectionDimsOnlyUnrelatedPathsAndMultiselectionUsesTheirUnion)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    const auto expectFocus = [&](const std::vector<ProjectItem*>& selected) {
        std::vector<bool> visibleNodes(graph.nodes.size(), selected.empty());
        std::vector<bool> visibleEdges(graph.edges.size(), selected.empty());
        for (const auto& path : graph.paths) {
            if (std::none_of(selected.begin(), selected.end(),
                    [&](const auto* source) { return source->guid == path.identity.sourceGuid; })) continue;
            for (const auto endpoint : path.computation) visibleNodes[endpoint.occurrenceIndex] = true;
            for (const auto index : path.edgeIndices) {
                visibleEdges[index] = true;
                visibleNodes[graph.edges[index].from.occurrenceIndex] = true;
                visibleNodes[graph.edges[index].to.occurrenceIndex] = true;
            }
        }
        for (size_t index = 0; index < graph.nodes.size(); ++index) {
            const auto* node = canvas.signalPathNode(index);
            EXPECT_TRUE(node->isVisible());
            if (visibleNodes[index]) EXPECT_DOUBLE_EQ(node->opacity(), 1.0);
            else EXPECT_LT(node->opacity(), 1.0);
        }
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            const auto* edge = canvas.signalPathEdge(index);
            EXPECT_TRUE(edge->isVisible());
            if (visibleEdges[index]) EXPECT_DOUBLE_EQ(edge->opacity(), 1.0);
            else EXPECT_LT(edge->opacity(), 1.0);
            if (const auto* gain = canvas.signalPathGainNode(index)) {
                EXPECT_DOUBLE_EQ(gain->opacity(), edge->opacity());
                EXPECT_DOUBLE_EQ(canvas.signalPathGainOutputEdge(index)->opacity(), edge->opacity());
            }
        }
    };
    expectFocus({});
    nodesFor(canvas, s4).front()->setSelected(true);
    expectFocus({s4});
    EXPECT_DOUBLE_EQ(nodesFor(canvas, l3).front()->opacity(), 1.0);
    EXPECT_LT(nodesFor(canvas, s5).front()->opacity(), 1.0);
    nodesFor(canvas, s1).front()->setSelected(true);
    expectFocus({s4, s1});
    canvas.setNodeContentFields(NodeContentField::Pitch);
    expectFocus({s4, s1});
    canvas.scene()->clearSelection();
    expectFocus({});
}

TEST_F(SignalPathRenderingTest, UnrelatedPlaybackStaysDimmedUntilItsConnectionIsSelected)
{
    const auto graph = build();
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    nodesFor(canvas, s4).front()->setSelected(true);
    auto* unrelated = nodesFor(canvas, s5).front();
    unrelated->setPlaybackActive(true);
    EXPECT_TRUE(unrelated->playbackActive());
    EXPECT_LT(unrelated->opacity(), 1.0);
    const auto edge = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const auto& entry) {
        return entry.kind == SignalPathEdgeKind::Output
            && graph.nodes[entry.to.occurrenceIndex].objectID == b3->objectID;
    });
    ASSERT_NE(edge, graph.edges.end());
    auto* wire = canvas.signalPathEdge(static_cast<size_t>(edge - graph.edges.begin()));
    EXPECT_LT(wire->opacity(), 1.0);
    wire->setSelected(true);
    EXPECT_DOUBLE_EQ(wire->opacity(), 1.0);
    EXPECT_DOUBLE_EQ(unrelated->opacity(), 1.0);
    EXPECT_EQ(wire->displayColor(), ThemeManager::instance().colors().accent);
    wire->setPlaybackActive(true);
    EXPECT_DOUBLE_EQ(wire->opacity(), 1.0);
    EXPECT_EQ(wire->displayColor(), ThemeManager::instance().colors().success);
}

TEST_F(SignalPathRenderingTest, ParallelRouteSelectionFocusesItsOwnPathsAndSurvivesViewChanges)
{
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
    canvas.resize(1000, 700);
    canvas.show();
    QApplication::processEvents();
    const auto findRoute = [&](const ProjectItem* provider, SignalPathEdgeKind kind) {
        const auto found = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const auto& edge) {
            return edge.kind == kind && edge.routingSource && edge.routingSource->objectGuid == provider->guid;
        });
        EXPECT_NE(found, graph.edges.end());
        return static_cast<size_t>(found - graph.edges.begin());
    };
    const auto first = findRoute(p1, SignalPathEdgeKind::Output);
    const auto second = findRoute(c1, SignalPathEdgeKind::Send);
    const auto unrelated = findRoute(l2, SignalPathEdgeKind::Output);
    ASSERT_LT(first, graph.edges.size());
    ASSERT_LT(second, graph.edges.size());
    ASSERT_LT(unrelated, graph.edges.size());
    const auto click = [&](size_t index, Qt::KeyboardModifiers modifiers) {
        const auto point = exposedConnectionPoint(canvas, canvas.signalPathEdge(index));
        ASSERT_TRUE(point.has_value());
        canvasMouse(canvas, QEvent::MouseButtonPress, *point, Qt::LeftButton, Qt::LeftButton, modifiers);
        canvasMouse(canvas, QEvent::MouseButtonRelease, *point, Qt::LeftButton, Qt::NoButton, modifiers);
    };
    click(first, Qt::NoModifier);
    EXPECT_TRUE(canvas.signalPathEdge(first)->isSelected());
    EXPECT_TRUE(canvas.selectedProjectItems().empty());
    EXPECT_DOUBLE_EQ(nodesFor(canvas, s1).front()->opacity(), 1.0);
    EXPECT_LT(nodesFor(canvas, s4).front()->opacity(), 1.0);
    EXPECT_LT(canvas.signalPathEdge(unrelated)->opacity(), 1.0);
    EXPECT_NE(canvas.signalPathGainNode(first)->pos(), canvas.signalPathGainNode(unrelated)->pos());
    click(second, Qt::ControlModifier);
    const auto expectSelection = [&]() {
        EXPECT_TRUE(canvas.signalPathEdge(first)->isSelected());
        EXPECT_TRUE(canvas.signalPathEdge(second)->isSelected());
        EXPECT_TRUE(canvas.signalPathGainNode(first)->isSelected());
        EXPECT_TRUE(canvas.signalPathGainOutputEdge(first)->isSelected());
        EXPECT_TRUE(canvas.signalPathGainNode(second)->isSelected());
        EXPECT_TRUE(canvas.signalPathGainOutputEdge(second)->isSelected());
        EXPECT_EQ(canvas.scene()->selectedItems().size(), 6);
        for (const auto* item : {s1, s2, s3, s4, s5})
            EXPECT_DOUBLE_EQ(nodesFor(canvas, item).front()->opacity(), 1.0);
        EXPECT_LT(nodesFor(canvas, s6).front()->opacity(), 1.0);
        EXPECT_LT(canvas.signalPathEdge(unrelated)->opacity(), 1.0);
    };
    expectSelection();
    canvas.setNodeContentFields(NodeContentField::Pitch);
    expectSelection();
    canvas.setFlowDirection(CanvasFlowDirection::TB);
    expectSelection();
    canvas.scene()->clearSelection();
    for (const auto* item : {s1, s2, s3, s4, s5, s6})
        EXPECT_DOUBLE_EQ(nodesFor(canvas, item).front()->opacity(), 1.0);
    for (size_t index = 0; index < graph.edges.size(); ++index)
        EXPECT_DOUBLE_EQ(canvas.signalPathEdge(index)->opacity(), 1.0);
}

TEST_F(SignalPathRenderingTest, ConnectionSelectionFollowsItsConfigurationWhenGraphIndicesAndLevelChange)
{
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, build());
    const auto selectedIndex = [&]() {
        const auto& graph = canvas.activeSignalPathSource()->graph();
        const auto found = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const auto& edge) {
            return edge.kind == SignalPathEdgeKind::Send && edge.routingSource
                && edge.routingSource->objectGuid == c1->guid;
        });
        EXPECT_NE(found, graph.edges.end());
        return static_cast<size_t>(found - graph.edges.begin());
    };
    const auto before = selectedIndex();
    canvas.signalPathEdge(before)->setSelected(true);
    s1->fieldGroups.send.shared = {FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(b3->guid), -3.0f}}}};
    c1->fieldGroups.send.shared.localHistory->sends.front().sendLevel = -12.0f;
    canvas.activeSignalPathSource()->loadGraph(project, build());
    const auto after = selectedIndex();
    EXPECT_NE(before, after);
    EXPECT_TRUE(canvas.signalPathEdge(after)->isSelected());
    EXPECT_TRUE(canvas.signalPathGainNode(after)->isSelected());
    EXPECT_TRUE(canvas.signalPathGainOutputEdge(after)->isSelected());
    EXPECT_EQ(canvas.scene()->selectedItems().size(), 3);
    EXPECT_LT(nodesFor(canvas, s1).front()->opacity(), 1.0);
    EXPECT_DOUBLE_EQ(nodesFor(canvas, s4).front()->opacity(), 1.0);
    EXPECT_DOUBLE_EQ(nodesFor(canvas, s5).front()->opacity(), 1.0);
    c1->fieldGroups.send.shared.localHistory->sends.clear();
    canvas.activeSignalPathSource()->loadGraph(project, build());
    EXPECT_TRUE(canvas.scene()->selectedItems().empty());
    for (const auto* item : {s1, s4, s5})
        EXPECT_DOUBLE_EQ(nodesFor(canvas, item).front()->opacity(), 1.0);
}

TEST_F(SignalPathRenderingTest, SelectingAnyGainRoutePartSelectsAndFocusesTheWholeRoute)
{
    const auto graph = build();
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        if (graph.edges[index].kind == SignalPathEdgeKind::Through) continue;
        SCOPED_TRACE(index);
        auto* gain = canvas.signalPathGainNode(index);
        auto* upstream = canvas.signalPathEdge(index);
        auto* downstream = canvas.signalPathGainOutputEdge(index);
        ASSERT_NE(gain, nullptr);
        ASSERT_NE(downstream, nullptr);
        const std::array<QGraphicsItem*, 3> parts{gain, upstream, downstream};
        std::vector<qreal> routeFocus;
        for (auto* part : parts) {
            canvas.scene()->clearSelection();
            part->setSelected(true);
            EXPECT_EQ(canvas.scene()->selectedItems().size(), 3);
            EXPECT_TRUE(canvas.selectedProjectItems().empty());
            for (const auto* selected : parts) {
                EXPECT_TRUE(selected->isSelected());
                EXPECT_DOUBLE_EQ(selected->opacity(), 1.0);
            }
            std::vector<qreal> focus;
            for (size_t node = 0; node < graph.nodes.size(); ++node)
                focus.push_back(canvas.signalPathNode(node)->opacity());
            if (routeFocus.empty()) routeFocus = focus;
            else EXPECT_EQ(focus, routeFocus);
            part->setSelected(false);
            EXPECT_TRUE(canvas.scene()->selectedItems().empty());
            for (size_t node = 0; node < graph.nodes.size(); ++node)
                EXPECT_DOUBLE_EQ(canvas.signalPathNode(node)->opacity(), 1.0);
        }
    }
}

TEST_F(SignalPathRenderingTest, OutputVolumePlatformOverrideKeepsTheSameOutputConnectionSelected)
{
    const auto platform = HarkenTest::FieldGroupPlatformIds::Windows;
    const auto graph = build(platform);
    ASSERT_TRUE(graph.ok());
    const auto found = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const auto& edge) {
        return edge.kind == SignalPathEdgeKind::Output && edge.routingSource
            && edge.routingSource->objectGuid == p1->guid;
    });
    ASSERT_NE(found, graph.edges.end());
    EXPECT_FALSE(found->routingSource->scope);
    const auto index = static_cast<size_t>(found - graph.edges.begin());
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    canvas.signalPathEdge(index)->setSelected(true);
    p1->fieldGroups.mainOutput.platforms[platform] = {
        FieldGroupMode::UseShared, MainOutputFieldGroup{{}, -9.0f},
        FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume)};
    auto updated = build(platform);
    ASSERT_TRUE(updated.ok());
    EXPECT_EQ(updated.edges[index].routingSource->scope, platform);
    canvas.activeSignalPathSource()->loadGraph(project, std::move(updated));
    EXPECT_TRUE(canvas.signalPathEdge(index)->isSelected());
    EXPECT_TRUE(canvas.signalPathGainNode(index)->isSelected());
    EXPECT_TRUE(canvas.signalPathGainOutputEdge(index)->isSelected());
    EXPECT_EQ(canvas.scene()->selectedItems().size(), 3);
    EXPECT_DOUBLE_EQ(nodesFor(canvas, s1).front()->opacity(), 1.0);
    EXPECT_LT(nodesFor(canvas, s4).front()->opacity(), 1.0);
    canvas.activeSignalPathSource()->loadGraph(project, build(HarkenTest::FieldGroupPlatformIds::Android));
    EXPECT_TRUE(canvas.signalPathEdge(index)->isSelected());
    EXPECT_TRUE(canvas.signalPathGainNode(index)->isSelected());
    EXPECT_TRUE(canvas.signalPathGainOutputEdge(index)->isSelected());
    EXPECT_EQ(canvas.scene()->selectedItems().size(), 3);
}

TEST_F(SignalPathRenderingTest, FilteringWaveformAndFieldsPreservesAllPairedPins)
{
    NodeItem node(QStringLiteral("Sound"));
    node.setContentInfo({.volume = 0, .pitch = 0});
    node.setBlueprintWaveformMode(true);
    node.setWaveformPeaks({0.25f, 0.5f, 0.25f});
    for (int pair = 0; pair < 12; ++pair) {
        node.addInputPin();
        node.addOutputPin();
    }
    const auto fullHeight = node.contentHeight();
    node.setContentFields(kAllNodeContentFields & ~NodeContentFields(NodeContentField::Waveform));
    EXPECT_LT(node.contentHeight(), fullHeight);
    node.setContentFields({});
    for (const auto direction : kDirections) {
        node.setCanvasFlowDirection(direction);
        const auto body = node.shape().boundingRect();
        for (int pair = 0; pair < 12; ++pair) {
            EXPECT_TRUE(body.contains(node.inputPin(pair)->scenePos()));
            EXPECT_TRUE(body.contains(node.outputPin(pair)->scenePos()));
        }
    }
}

TEST_F(SignalPathRenderingTest, ContentFieldsUseSupportedPropertiesAndEffectiveOutputVolumes)
{
    for (auto* item : {s1, c1, t1, p1, l1, b1}) {
        item->comp<VMixNode>()->volume = -6.0f;
        item->comp<VMixNode>()->pitch = 3.0f;
        if (auto* audio = item->comp<VAudio>()) audio->loopCount = 4;
    }
    s1->comp<VSound>()->scheduleOffsetSec = 0.375;
    s1->comp<VMixNode>()->outputBusVolume = -80.0f;
    p1->fieldGroups.mainOutput.shared.localHistory->outputBusVolume = -9.0f;
    b1->comp<VMixNode>()->outputBusVolume = -4.0f;
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, build());
    canvas.setNodeContentFields(NodeContentField::Pitch);
    for (auto* item : {s1, c1, t1, p1, l1, b1}) {
        SCOPED_TRACE(item->name.toStdString());
        const auto nodes = nodesFor(canvas, item);
        ASSERT_FALSE(nodes.empty());
        const auto& info = nodes.front()->contentInfo();
        EXPECT_EQ(info.volume, -6.0f);
        EXPECT_EQ(info.pitch, 3.0f);
        EXPECT_EQ(info.loopCount, item == l1 || item == b1
            ? std::nullopt : std::optional<uint32_t>{4});
        EXPECT_EQ(info.offsetOfStart, item == s1 ? std::optional<double>{0.375} : std::nullopt);
        EXPECT_FALSE(info.outputVolume.has_value());
        EXPECT_FALSE(info.sendVolume.has_value());
        auto* node = nodes.front();
        const auto render = [&]() {
            QImage image(node->shape().boundingRect().size().toSize(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            canvas.scene()->render(&painter, image.rect(), node->sceneBoundingRect());
            return image;
        };
        const auto before = render();
        auto changed = info;
        changed.pitch = -12.0f;
        node->setContentInfo(changed);
        EXPECT_NE(render(), before);
        const auto pitchHeight = node->contentHeight();
        node->setContentFields({});
        EXPECT_LT(node->contentHeight(), pitchHeight);
    }
    const auto& graph = canvas.activeSignalPathSource()->graph();
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        const auto* gain = canvas.signalPathGainNode(index);
        if (edge.kind == SignalPathEdgeKind::Through) continue;
        ASSERT_NE(gain, nullptr);
        if (edge.kind == SignalPathEdgeKind::Send) {
            EXPECT_EQ(gain->contentInfo().sendVolume, edge.sendLevel);
        } else if (edge.routingSource && edge.routingSource->objectGuid == p1->guid) {
            EXPECT_EQ(gain->contentInfo().outputVolume, -9.0f);
        } else if (graph.nodes[edge.from.occurrenceIndex].objectID == b1->objectID) {
            EXPECT_EQ(gain->contentInfo().outputVolume, -4.0f);
        }
    }
}

TEST_F(SignalPathRenderingTest, ContentValuesFollowTheGraphPlatformScope)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    for (auto* item : {s1, c1, t1, p1, l1, b1}) {
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
        ASSERT_TRUE(locator);
        ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(*item, *locator,
            static_cast<uint8_t>(Property::Pitch), 5.0, HarkenTest::PlatformIds::Android));
    }
    p1->fieldGroups.mainOutput.platforms[toFieldGroupGuid(HarkenTest::PlatformIds::Android)] = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(b1->guid), -12.0f}};
    for (const auto platform : {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}) {
        const auto graph = buildSignalPathGraph(project, {{s1->guid, {}}, {s4->guid, {}}},
            toFieldGroupScope(platform));
        ASSERT_TRUE(graph.ok());
        SignalPathSource source;
        source.loadGraph(project, graph);
        for (const auto& desc : source.buildNodes()) {
            if (desc.id == l3->objectID || desc.id == master->objectID || desc.id == b2->objectID
                || desc.id == s4->objectID) continue;
            EXPECT_EQ(desc.content.pitch, platform == HarkenTest::PlatformIds::Android ? 5.0f : 0.0f);
            EXPECT_FALSE(desc.content.outputVolume.has_value());
        }
        const auto edges = source.buildEdges();
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            const auto& edge = graph.edges[index];
            if (edge.kind == SignalPathEdgeKind::Output && edge.routingSource
                && edge.routingSource->objectGuid == p1->guid)
                EXPECT_EQ(edges[index].gainContent.outputVolume,
                    platform == HarkenTest::PlatformIds::Android ? -12.0f : 0.0f);
        }
    }
}

TEST_F(SignalPathRenderingTest, WaveformUsesActiveSourceAndItsEffectiveTrimWindow)
{
    QTemporaryDir directory(QDir::current().filePath(QStringLiteral(".temp/blueprint-waveform-XXXXXX")));
    ASSERT_TRUE(directory.isValid());
    QDir root(directory.path());
    ASSERT_TRUE(root.mkpath(QStringLiteral("AudioSources")));
    project.projectPath = root.filePath(QStringLiteral("waveform.hsp"));
    auto& registry = AssetRegistry::instance();
    const auto previousAssets = registry.snapshot();
    const auto previousRoot = registry.projectRoot();
    const auto restore = qScopeGuard([&]() { registry.replaceSnapshot(previousRoot, previousAssets); });
    auto* sound = s1->comp<VSound>();
    for (int index = 0; index < 2; ++index) {
        AssetInfo asset;
        asset.sourceID = registry.allocateStableID();
        asset.relativePath = QStringLiteral("source%1.wav").arg(index);
        asset.absolutePath = root.filePath(QStringLiteral("AudioSources/") + asset.relativePath);
        asset.metadata.sampleRate = 48000;
        asset.metadata.channels = 1;
        asset.metadata.totalSamples = 16384;
        asset.metadata.durationSec = 16384.0 / 48000.0;
        std::vector<float> samples(16384, 0.1f);
        std::fill(samples.begin() + 4096, samples.begin() + 8192, index == 0 ? 0.3f : 0.8f);
        ASSERT_TRUE(AssetConverter::writeFloatWav(asset.absolutePath, samples, 1, 48000));
        ASSERT_TRUE(WaveformCache::generate(asset.absolutePath, 64));
        ASSERT_TRUE(registry.registerAsset(asset));
        SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = asset.sourceID;
        source.trimStartFrame = 4096;
        source.trimEndFrame = 8192;
        sound->sources.plain.entries.push_back(source);
    }
    sound->sources.plain.active = sound->sources.plain.entries[1].entryId;
    sound->sources.plain.platforms[HarkenTest::platformIdString(QStringLiteral("Windows"))].active = sound->sources.plain.entries[0].entryId;
    s1->sourceID = sound->sources.plain.entries[0].sourceId;
    s1->sourcePath = QStringLiteral("source0.wav");
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(NodeContentField::Waveform);
    const auto render = [](NodeItem* node) {
        QImage image(node->shape().boundingRect().size().toSize(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        node->paint(&painter, nullptr, nullptr);
        return image;
    };
    const auto expectFreshPresentation = [&](const SignalPathGraph& graph) {
        BlueprintCanvasWidget fresh;
        fresh.setNodeContentFields(NodeContentField::Waveform);
        fresh.useSignalPaths(project, graph);
        const auto currentNodes = nodesFor(canvas, s1);
        const auto freshNodes = nodesFor(fresh, s1);
        ASSERT_EQ(currentNodes.size(), 1u);
        ASSERT_EQ(freshNodes.size(), 1u);
        const auto* current = currentNodes.front();
        const auto* expected = freshNodes.front();
        EXPECT_EQ(current->blueprintWaveformMode(), expected->blueprintWaveformMode());
        EXPECT_EQ(current->contentHeight(), expected->contentHeight());
        EXPECT_EQ(current->shape().boundingRect(), expected->shape().boundingRect());
        EXPECT_EQ(render(currentNodes.front()), render(freshNodes.front()));
    };
    for (const auto platform : {HarkenTest::PlatformIds::Android, HarkenTest::PlatformIds::Windows}) {
        const auto graph = buildSignalPathGraph(project, {{s1->guid, {}}}, toFieldGroupScope(platform));
        ASSERT_TRUE(graph.ok());
        if (platform == HarkenTest::PlatformIds::Android)
            canvas.useSignalPaths(project, graph);
        else
            canvas.activeSignalPathSource()->loadGraph(project, graph);
        expectFreshPresentation(graph);
        auto* node = nodesFor(canvas, s1).front();
        const auto descriptors = canvas.activeSignalPathSource()->buildNodes();
        const auto desc = std::find_if(descriptors.begin(), descriptors.end(),
            [&](const auto& value) { return value.id == s1->objectID; });
        ASSERT_NE(desc, descriptors.end());
        const auto path = root.filePath(platform == HarkenTest::PlatformIds::Android
            ? QStringLiteral("AudioSources/source1.wav") : QStringLiteral("AudioSources/source0.wav"));
        EXPECT_EQ(desc->mediaPath, path);
        EXPECT_EQ(desc->waveformRange, (WaveformFrameRange{4096, 8192}));
        WaveformPeakData peaks;
        ASSERT_TRUE(WaveformCache::load(path, peaks));
        const auto actual = render(node);
        node->setWaveformPeakData(peaks, WaveformFrameRange{4096, 8192});
        EXPECT_EQ(render(node), actual);
        node->setWaveformPeakData(peaks);
        EXPECT_NE(render(node), actual);
        node->setWaveformPeakData(peaks, WaveformFrameRange{4096, 8192});
    }
    const auto androidGraph = buildSignalPathGraph(project, {{s1->guid, {}}},
        toFieldGroupScope(HarkenTest::PlatformIds::Android));
    ASSERT_TRUE(androidGraph.ok());
    canvas.activeSignalPathSource()->loadGraph(project, androidGraph);
    expectFreshPresentation(androidGraph);
    const auto originalSource = render(nodesFor(canvas, s1).front());
    sound->sources.plain.active = sound->sources.plain.entries[0].entryId;
    canvas.activeSignalPathSource()->loadGraph(project, androidGraph);
    expectFreshPresentation(androidGraph);
    const auto changedSource = render(nodesFor(canvas, s1).front());
    EXPECT_NE(changedSource, originalSource);
    sound->sources.plain.entries[0].trimStartFrame = 0;
    sound->sources.plain.entries[0].trimEndFrame = 4096;
    canvas.activeSignalPathSource()->loadGraph(project, androidGraph);
    expectFreshPresentation(androidGraph);
    EXPECT_NE(render(nodesFor(canvas, s1).front()), changedSource);
    sound->sources.plain.active = sound->sources.plain.entries[1].entryId;
    sound->sources.plain.entries[1].trimEndFrame = 32768;
    canvas.activeSignalPathSource()->loadGraph(project, androidGraph);
    expectFreshPresentation(androidGraph);
    const auto waveformHeight = nodesFor(canvas, s1).front()->contentHeight();
    for (const auto& desc : canvas.activeSignalPathSource()->buildNodes()) {
        if (desc.id == s1->objectID) EXPECT_EQ(desc.waveformRange, (WaveformFrameRange{4096, 16384}));
    }
    sound->sources.plain.entries[1].trimStartFrame = 16384;
    canvas.activeSignalPathSource()->loadGraph(project, androidGraph);
    expectFreshPresentation(androidGraph);
    EXPECT_FALSE(nodesFor(canvas, s1).front()->blueprintWaveformMode());
    EXPECT_LT(nodesFor(canvas, s1).front()->contentHeight(), waveformHeight);
    for (const auto& desc : canvas.activeSignalPathSource()->buildNodes()) {
        if (desc.id != s1->objectID) continue;
        EXPECT_FALSE(desc.waveformMode);
        EXPECT_TRUE(desc.mediaPath.isEmpty());
    }
    sound->sources.plain.active = QUuid{};
    sound->sources.plain.platforms.clear();
    canvas.activeSignalPathSource()->loadGraph(project, androidGraph);
    expectFreshPresentation(androidGraph);
    EXPECT_FALSE(nodesFor(canvas, s1).front()->blueprintWaveformMode());
    for (const auto& desc : canvas.activeSignalPathSource()->buildNodes()) {
        if (desc.id == s1->objectID) EXPECT_FALSE(desc.waveformMode);
    }
    sound->sources.plain.active = sound->sources.plain.entries[0].entryId;
    canvas.activeSignalPathSource()->loadGraph(project, androidGraph);
    expectFreshPresentation(androidGraph);
    EXPECT_TRUE(nodesFor(canvas, s1).front()->blueprintWaveformMode());
    EXPECT_EQ(nodesFor(canvas, s1).front()->contentHeight(), waveformHeight);
}

TEST_F(SignalPathRenderingTest, SquareSocketsFaceOutwardAndKeepAGapAroundConnectedPlugs)
{
    NodeItem node(QStringLiteral("Socket"));
    node.addInputPin();
    node.addOutputPin();
    const auto& domain = ThemeManager::instance().snapshot()->domains.pinDomain;
    for (const auto direction : kDirections) {
        node.setCanvasFlowDirection(direction);
        for (auto* pin : {node.inputPin(), node.outputPin()}) {
            const bool vertical = direction == CanvasFlowDirection::TB || direction == CanvasFlowDirection::BT;
            const bool reverse = direction == CanvasFlowDirection::RL || direction == CanvasFlowDirection::BT;
            const qreal sign = (pin->pinType() == NodePin::Input) != reverse ? -1.0 : 1.0;
            const QPointF outward = vertical ? QPointF(0, sign) : QPointF(sign, 0);
            const QPointF side(-outward.y(), outward.x());
            const auto render = [&]() {
                QImage image(224, 224, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                painter.scale(8, 8);
                painter.translate(8, 8);
                pin->paint(&painter, nullptr, nullptr);
                return image;
            };
            const auto pixel = [&](const QImage& image, const QPointF& offset) {
                return image.pixelColor(((QPointF(14, 14) - outward * 2 + offset) * 8).toPoint());
            };
            const auto unplugged = render();
            EXPECT_EQ(pixel(unplugged, -outward * 3.25), domain.audio);
            EXPECT_EQ(pixel(unplugged, side * 3.25), domain.audio);
            EXPECT_EQ(pixel(unplugged, outward * 3.25), domain.ring);
            EXPECT_EQ(pixel(unplugged, {}), domain.ring);
            {
                ConnectionPath wire(pin, nullptr);
                wire.setFlag(QGraphicsItem::ItemIsSelectable);
                const auto plugged = render();
                EXPECT_EQ(pixel(plugged, {}), ConnectionPath::defaultColor());
                EXPECT_EQ(pixel(plugged, outward * 3.25), ConnectionPath::defaultColor());
                EXPECT_EQ(pixel(plugged, -outward * 2), domain.ring);
                EXPECT_EQ(pixel(plugged, side * 2), domain.ring);
                EXPECT_EQ(pin->boundingRect(), QRectF(-8, -8, 28, 28));
                wire.setSelected(true);
                EXPECT_EQ(pixel(render(), {}), ThemeManager::instance().colors().accent);
                wire.setPlaybackActive(true);
                EXPECT_EQ(pixel(render(), {}), ThemeManager::instance().colors().success);
                pin->setConnectState(NodePin::ConnectState::Bad);
                EXPECT_EQ(pixel(render(), {}), ThemeManager::instance().colors().success);
                EXPECT_EQ(pixel(render(), side * 3.25), domain.targetBad);
                pin->setConnectState(NodePin::ConnectState::None);
                wire.setPlaybackActive(false);
                EXPECT_EQ(pixel(render(), {}), ThemeManager::instance().colors().accent);
            }
            EXPECT_EQ(render(), unplugged);
        }
    }
}

TEST_F(SignalPathRenderingTest, ConnectionLinesShareNeutralColorAndHaveNoArrowheads)
{
    NodePin from(NodePin::Output);
    NodePin to(NodePin::Input);
    from.setPos(0, 20);
    to.setPos(180, 20);
    ConnectionPath wire(&from, &to);
    QImage throughImage;
    for (const auto kind : {BlueprintEdgeKind::Through, BlueprintEdgeKind::Containment,
            BlueprintEdgeKind::BusRoute, BlueprintEdgeKind::Send, BlueprintEdgeKind::Override,
            BlueprintEdgeKind::Placement, BlueprintEdgeKind::Organization}) {
        wire.setBlueprintEdgeKind(kind);
        QImage image(200, 50, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            wire.paint(&painter, nullptr, nullptr);
        }
        if (kind == BlueprintEdgeKind::Through) throughImage = image;
        if (kind == BlueprintEdgeKind::BusRoute) EXPECT_EQ(image, throughImage);
        bool visible = false;
        for (int x = 0; x < image.width(); ++x) {
            for (int y = 0; y < image.height(); ++y) {
                if (image.pixelColor(x, y).alpha() == 0) continue;
                visible = true;
                EXPECT_LE(std::abs(y - 26), 1);
                EXPECT_EQ(image.pixelColor(x, y), ConnectionPath::defaultColor());
            }
        }
        EXPECT_TRUE(visible);
    }
}

TEST_F(SignalPathRenderingTest, SharedSocketUsesThePlayingBranchColor)
{
    NodePin pin(NodePin::Output);
    ConnectionPath first(&pin, nullptr);
    ConnectionPath second(&pin, nullptr);
    second.setFlag(QGraphicsItem::ItemIsSelectable);
    second.setSelected(true);
    const auto coreColor = [&]() {
        QImage image(28, 28, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.translate(8, 8);
            pin.paint(&painter, nullptr, nullptr);
        }
        return image.pixelColor(12, 14);
    };
    EXPECT_EQ(coreColor(), ThemeManager::instance().colors().accent);
    first.setPlaybackActive(true);
    EXPECT_EQ(coreColor(), ThemeManager::instance().colors().success);
    first.setPlaybackActive(false);
    EXPECT_EQ(coreColor(), ThemeManager::instance().colors().accent);
    second.setSelected(false);
    EXPECT_EQ(coreColor(), ConnectionPath::defaultColor());
}

TEST_F(SignalPathRenderingTest, SelectedConnectionsUseSemanticColorsAndRemainDistinctDuringPlayback)
{
    NodePin from(NodePin::Output);
    NodePin to(NodePin::Input);
    from.setPos(0, 20);
    to.setPos(180, 20);
    ConnectionPath wire(&from, &to);
    const auto midpoint = wire.path().pointAtPercent(0.5);
    EXPECT_TRUE(wire.shape().contains(midpoint + QPointF(0, 4)));
    EXPECT_FALSE(wire.shape().contains(midpoint + QPointF(0, 8)));
    EXPECT_FALSE(wire.shape().contains(midpoint + QPointF(30, -8)));
    wire.setLabel(QStringLiteral("Send (-12.0 dB)"));
    EXPECT_TRUE(wire.shape().contains(midpoint + QPointF(30, -8)));
    wire.setLabel({});
    for (const auto kind : {BlueprintEdgeKind::Through, BlueprintEdgeKind::BusRoute, BlueprintEdgeKind::Send}) {
        wire.setBlueprintEdgeKind(kind);
        EXPECT_TRUE(wire.flags().testFlag(QGraphicsItem::ItemIsSelectable));
        for (const bool playing : {false, true}) {
            wire.setPlaybackActive(playing);
            for (const bool selected : {false, true}) {
                wire.setSelected(selected);
                QImage image(200, 50, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                {
                    QPainter painter(&image);
                    wire.paint(&painter, nullptr, nullptr);
                }
                const auto color = playing ? ThemeManager::instance().colors().success
                    : selected ? ThemeManager::instance().colors().accent : ConnectionPath::defaultColor();
                int thickness = 0;
                for (int y = 0; y < image.height(); ++y) {
                    if (image.pixelColor(10, y).alpha() == 0) continue;
                    ++thickness;
                    EXPECT_EQ(image.pixelColor(10, y), color);
                }
                EXPECT_EQ(thickness, 2 + int(playing) + int(selected));
            }
        }
    }
}

TEST_F(SignalPathRenderingTest, ConnectionClicksSupportControlSelectionWithoutOpeningNodeViews)
{
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, build());
    canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
    canvas.resize(900, 600);
    canvas.show();
    QApplication::processEvents();
    int nodeSelections = 0;
    int nodeOpenRequests = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeSelected, &canvas,
        [&](uint32_t) { ++nodeSelections; });
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeDoubleClicked, &canvas,
        [&](uint32_t) { ++nodeOpenRequests; });
    const auto wires = connections(canvas);
    std::vector<ConnectionPath*> chosen;
    size_t selectedPartCount = 0;
    for (const auto kind : {BlueprintEdgeKind::Through, BlueprintEdgeKind::BusRoute, BlueprintEdgeKind::Send}) {
        const auto found = std::find_if(wires.begin(), wires.end(), [&](auto* wire) {
            return wire->blueprintEdgeKind() == kind && exposedConnectionPoint(canvas, wire).has_value();
        });
        ASSERT_NE(found, wires.end());
        auto* wire = *found;
        const auto point = exposedConnectionPoint(canvas, wire);
        ASSERT_TRUE(point.has_value());
        const auto modifiers = chosen.empty() ? Qt::NoModifier : Qt::ControlModifier;
        canvasMouse(canvas, QEvent::MouseButtonPress, *point, Qt::LeftButton, Qt::LeftButton, modifiers);
        canvasMouse(canvas, QEvent::MouseButtonRelease, *point, Qt::LeftButton, Qt::NoButton, modifiers);
        chosen.push_back(wire);
        selectedPartCount += kind == BlueprintEdgeKind::Through ? 1 : 3;
        EXPECT_EQ(canvas.scene()->selectedItems().size(), selectedPartCount);
        for (auto* selected : chosen) EXPECT_TRUE(selected->isSelected());
    }
    auto* first = chosen.front();
    const auto point = exposedConnectionPoint(canvas, first);
    ASSERT_TRUE(point.has_value());
    canvasMouse(canvas, QEvent::MouseButtonPress, *point, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    canvasMouse(canvas, QEvent::MouseButtonRelease, *point, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
    EXPECT_FALSE(first->isSelected());
    EXPECT_EQ(canvas.scene()->selectedItems().size(), 6);
    canvasMouse(canvas, QEvent::MouseButtonPress, *point, Qt::LeftButton, Qt::LeftButton);
    canvasMouse(canvas, QEvent::MouseButtonRelease, *point, Qt::LeftButton);
    canvasMouse(canvas, QEvent::MouseButtonDblClick, *point, Qt::LeftButton, Qt::LeftButton);
    canvasMouse(canvas, QEvent::MouseButtonRelease, *point, Qt::LeftButton);
    EXPECT_TRUE(first->isSelected());
    EXPECT_EQ(canvas.scene()->selectedItems().size(), 1);
    EXPECT_EQ(nodeSelections, 0);
    EXPECT_EQ(nodeOpenRequests, 0);
}

TEST_F(SignalPathRenderingTest, ConnectionDraggingKeepsNodesStillAndPanOverridesSelection)
{
    BlueprintCanvasWidget canvas;
    const auto graph = build();
    canvas.useSignalPaths(project, graph);
    canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
    canvas.resize(900, 600);
    canvas.show();
    QApplication::processEvents();
    const auto wires = connections(canvas);
    const auto found = std::find_if(wires.begin(), wires.end(), [&](auto* wire) {
        return exposedConnectionPoint(canvas, wire).has_value();
    });
    ASSERT_NE(found, wires.end());
    auto* wire = *found;
    const auto point = exposedConnectionPoint(canvas, wire);
    ASSERT_TRUE(point.has_value());
    std::vector<QPointF> positions;
    for (size_t index = 0; index < graph.nodes.size(); ++index)
        positions.push_back(canvas.signalPathNode(index)->pos());
    canvasMouse(canvas, QEvent::MouseButtonPress, *point, Qt::LeftButton, Qt::LeftButton);
    EXPECT_EQ(canvas.scene()->mouseGrabberItem(), wire);
    canvasMouse(canvas, QEvent::MouseMove, *point + QPoint(30, 20), Qt::NoButton, Qt::LeftButton);
    canvasMouse(canvas, QEvent::MouseButtonRelease, *point + QPoint(30, 20), Qt::LeftButton);
    EXPECT_TRUE(wire->isSelected());
    for (size_t index = 0; index < positions.size(); ++index)
        EXPECT_EQ(canvas.signalPathNode(index)->pos(), positions[index]);
    canvas.scene()->clearSelection();
    canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Pan);
    const auto panPoint = exposedConnectionPoint(canvas, wire);
    ASSERT_TRUE(panPoint.has_value());
    const auto initialTransform = canvas.viewportTransform();
    canvasMouse(canvas, QEvent::MouseButtonPress, *panPoint, Qt::LeftButton, Qt::LeftButton);
    EXPECT_EQ(canvas.scene()->mouseGrabberItem(), nullptr);
    canvasMouse(canvas, QEvent::MouseMove, *panPoint + QPoint(40, 30), Qt::NoButton, Qt::LeftButton);
    EXPECT_NE(canvas.viewportTransform(), initialTransform);
    canvasMouse(canvas, QEvent::MouseButtonRelease, *panPoint + QPoint(40, 30), Qt::LeftButton);
    EXPECT_TRUE(canvas.scene()->selectedItems().isEmpty());
    for (size_t index = 0; index < positions.size(); ++index)
        EXPECT_EQ(canvas.signalPathNode(index)->pos(), positions[index]);
    wire->setSelected(true);
    canvas.setSelectedProjectItems({s1});
    EXPECT_FALSE(wire->isSelected());
    for (auto* node : nodesFor(canvas, s1)) EXPECT_TRUE(node->isSelected());
    canvas.setSelectedProjectItems({});
    EXPECT_TRUE(canvas.scene()->selectedItems().isEmpty());
}

TEST_F(SignalPathRenderingTest, SelectionUsesTheSameHardBorderForEveryNodeTypeAndCoexistsWithPlayback)
{
    const auto theme = ThemeManager::instance().snapshot();
    for (const auto type : {T::Sound, T::Layer, T::Bus, T::Container, T::Track, T::Piece,
            T::Attenuation, T::Action}) {
        NodeItem node(QStringLiteral("Node"));
        node.setItemType(type);
        node.setContentInfo({.volume = 0, .pitch = 0});
        const auto bounds = node.shape().boundingRect();
        const auto render = [&]() {
            QImage image((bounds.size().toSize() + QSize(32, 32)) * 4,
                QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.scale(4, 4);
            painter.translate(16, 16);
            node.paint(&painter, nullptr, nullptr);
            return image;
        };
        const auto idle = render();
        node.setSelected(true);
        const auto selected = render();
        EXPECT_EQ(selected.pixelColor(64, 64 + 45 * 4), theme->ui.colors.accent);
        const QRectF borderBounds = bounds.adjusted(-1, -1, 1, 1);
        for (int y = 0; y < selected.height(); ++y) {
            for (int x = 0; x < selected.width(); ++x) {
                if (borderBounds.contains(QPointF(x / 4.0 - 16, y / 4.0 - 16))) continue;
                EXPECT_EQ(selected.pixelColor(x, y), idle.pixelColor(x, y));
            }
        }
        node.setPlaybackActive(true);
        const auto playingSelected = render();
        EXPECT_EQ(playingSelected.pixelColor(64, 64 + 45 * 4), theme->ui.colors.accent);
        EXPECT_EQ(playingSelected.pixelColor(64 + 3 * 4, 64 + 45 * 4), theme->ui.colors.success);
        node.setSelected(false);
        const auto playing = render();
        EXPECT_EQ(playing.pixelColor(64 + 3 * 4, 64 + 45 * 4), theme->ui.colors.success);
    }
}

TEST_F(SignalPathRenderingTest, ManyPortPairsStayDistinctAndInsideNodeBounds)
{
    NodeItem node(QStringLiteral("Container"));
    node.setContentInfo({.volume = 0, .pitch = 0});
    const float originalHeight = node.contentHeight();
    for (int pair = 0; pair < 12; ++pair) {
        node.addInputPin();
        node.addOutputPin();
    }
    EXPECT_GT(node.contentHeight(), originalHeight);
    EXPECT_FLOAT_EQ(node.contentHeight(), NodeItem::heightForContent(node.contentInfo(), false, false, false, 12));
    for (auto direction : kDirections) {
        node.setCanvasFlowDirection(direction);
        const auto body = node.shape().boundingRect();
        for (int pair = 0; pair < 12; ++pair) {
            EXPECT_TRUE(body.contains(node.inputPin(pair)->scenePos()));
            EXPECT_TRUE(body.contains(node.outputPin(pair)->scenePos()));
            if (pair == 0) continue;
            EXPECT_FALSE(node.inputPin(pair - 1)->sceneBoundingRect().intersects(node.inputPin(pair)->sceneBoundingRect()));
            EXPECT_FALSE(node.outputPin(pair - 1)->sceneBoundingRect().intersects(node.outputPin(pair)->sceneBoundingRect()));
        }
    }
}

TEST_F(SignalPathRenderingTest, PinConnectionsFollowContentResizing)
{
    QGraphicsScene scene;
    auto* from = new NodeItem(QStringLiteral("Sound"));
    auto* to = new NodeItem(QStringLiteral("Layer"));
    scene.addItem(from);
    scene.addItem(to);
    from->addInputPin();
    from->addOutputPin();
    to->addInputPin();
    from->setCanvasFlowDirection(CanvasFlowDirection::TB);
    to->setCanvasFlowDirection(CanvasFlowDirection::TB);
    to->setPos(0, 400);
    auto* wire = new ConnectionPath(from->outputPin(), to->inputPin());
    scene.addItem(wire);
    wire->setBlueprintFlowDirection(CanvasFlowDirection::TB);
    from->setContentInfo({.volume = 0, .pitch = 0});
    from->setSubtitle(QStringLiteral("Subtitle"));
    from->setWaveformPeaks({0.25f, 0.5f, 0.25f});
    const auto oldHeight = from->contentHeight();
    from->setBlueprintWaveformMode(true);
    EXPECT_GT(from->contentHeight(), oldHeight);
    EXPECT_FLOAT_EQ(from->contentHeight(), NodeItem::heightForContent(from->contentInfo(), true, true, true));
    EXPECT_EQ(wire->path().pointAtPercent(0), from->outputPin()->scenePos());
    EXPECT_EQ(wire->path().pointAtPercent(1), to->inputPin()->scenePos());
}

TEST_F(SignalPathRenderingTest, SavedLegacyPositionAndSelectionDoNotMoveObjectNodes)
{
    l1->canvasPos = {4321, 5678};
    l3->canvasPos = l1->canvasPos;
    const auto beforeOutput = s5->fieldGroups.mainOutput;
    const auto beforeSend = s5->fieldGroups.send;
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    const auto repeated = nodesFor(canvas, l3);
    ASSERT_EQ(repeated.size(), 1u);
    std::vector<QPointF> positions;
    for (auto* node : repeated) {
        EXPECT_EQ(node->objectID(), l3->objectID);
        EXPECT_NE(node->pos(), l3->canvasPos);
        EXPECT_EQ(std::count(positions.begin(), positions.end(), node->pos()), 0);
        positions.push_back(node->pos());
        QGraphicsSceneMouseEvent press(QEvent::GraphicsSceneMousePress);
        press.setButton(Qt::LeftButton);
        canvas.scene()->sendEvent(node, &press);
        QGraphicsSceneMouseEvent release(QEvent::GraphicsSceneMouseRelease);
        release.setButton(Qt::LeftButton);
        canvas.scene()->sendEvent(node, &release);
        EXPECT_EQ(node->pos(), positions.back());
    }
    canvas.resetNodeLayout();
    for (const auto* node : nodesFor(canvas, l3))
        EXPECT_EQ(std::count(positions.begin(), positions.end(), node->pos()), 1);
    EXPECT_EQ(l1->canvasPos, QPointF(4321, 5678));
    EXPECT_EQ(l3->canvasPos, QPointF(4321, 5678));
    EXPECT_EQ(s5->fieldGroups.mainOutput, beforeOutput);
    EXPECT_EQ(s5->fieldGroups.send, beforeSend);
}

TEST_F(SignalPathRenderingTest, ReloadUpdatesRoutesWithoutDuplicatingNodesOrLeavingStaleConnections)
{
    auto graph = build();
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    output(s5, b1);
    graph = build();
    ASSERT_TRUE(graph.ok());
    canvas.useSignalPaths(project, graph);
    ASSERT_EQ(nodesFor(canvas, c1).size(), 1u);
    EXPECT_EQ(nodesFor(canvas, c1)[0]->inputCount(), 1);
    EXPECT_EQ(nodesFor(canvas, l1).size(), 1u);
    EXPECT_EQ(nodesFor(canvas, l3).size(), 1u);
    EXPECT_TRUE(nodesFor(canvas, b3).empty());
    expectConnections(canvas, graph);
    expectLayout(canvas, graph);

    s5->fieldGroups.send.shared = {FieldGroupMode::Local, SendFieldGroup{}};
    graph = build();
    ASSERT_TRUE(graph.ok());
    canvas.useSignalPaths(project, graph);
    EXPECT_EQ(nodesFor(canvas, c1)[0]->inputCount(), 1);
    expectConnections(canvas, graph);
    expectLayout(canvas, graph);
}

TEST_F(SignalPathRenderingTest, RepeatedUsesShareOneThroughAndRetainSelectionWhenReferencesChange)
{
    auto& clips = t1->comp<VTrack>()->clips;
    std::vector<SignalPathIdentity> inputs;
    for (int index = 0; index < 2; ++index) {
        Clip clip{QUuid::createUuid(), s1->guid, index, 1};
        clip.stableOccurrenceOrdinal = static_cast<uint32_t>(clips.occurrenceOrdinalHighWater++);
        clips.items.push_back(clip);
        inputs.push_back({s1->guid, {*PlayablePropertyAuthoringV1::trackClipLocator(*t1, clip.clipGuid)}});
    }
    auto graph = buildSignalPathGraph(project, inputs);
    ASSERT_TRUE(graph.ok());
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    const auto sounds = nodesFor(canvas, s1);
    ASSERT_EQ(sounds.size(), 1u);
    EXPECT_EQ(nodesFor(canvas, t1).size(), 1u);
    std::vector<size_t> throughEdges;
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        if (graph.nodes[edge.from.occurrenceIndex].key.objectGuid == s1->guid
            && graph.nodes[edge.to.occurrenceIndex].key.objectGuid == t1->guid)
            throughEdges.push_back(index);
    }
    ASSERT_EQ(throughEdges.size(), 1u);
    const auto through = throughEdges.front();
    EXPECT_EQ(graph.edges[through].kind, SignalPathEdgeKind::Through);
    EXPECT_EQ(graph.edges[through].pathIndices, (std::vector<size_t>{0, 1}));
    EXPECT_DOUBLE_EQ(canvas.signalPathEdge(through)->parallelOffset(), 0.0);
    canvas.signalPathEdge(through)->setSelected(true);
    expectConnections(canvas, graph);
    expectLayout(canvas, graph);

    std::reverse(inputs.begin(), inputs.end());
    graph = buildSignalPathGraph(project, inputs);
    canvas.activeSignalPathSource()->loadGraph(project, graph);
    expectConnections(canvas, graph);
    EXPECT_TRUE(canvas.signalPathEdge(through)->isSelected());
    clips.items.clear();
    graph = buildSignalPathGraph(project, {{s1->guid, {}}});
    canvas.activeSignalPathSource()->loadGraph(project, graph);
    expectConnections(canvas, graph);
    EXPECT_TRUE(canvas.signalPathEdge(through)->isSelected());
    EXPECT_DOUBLE_EQ(canvas.signalPathEdge(through)->parallelOffset(), 0.0);
    EXPECT_EQ(graph.edges[through].pathIndices, (std::vector<size_t>{0}));
}

TEST_F(SignalPathRenderingTest, BusSendBranchContinuesThroughSharedBuses)
{
    b1->comp<VMixNode>()->sends.push_back({b3->objectID, 0.25f});
    const auto graph = build();
    ASSERT_TRUE(graph.ok());
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, graph);
    EXPECT_EQ(nodesFor(canvas, b3).size(), 1u);
    EXPECT_EQ(nodesFor(canvas, master).size(), 1u);
    expectConnections(canvas, graph);
    for (auto direction : kDirections) {
        canvas.setFlowDirection(direction);
        expectLayout(canvas, graph);
    }
}

TEST_F(SignalPathRenderingTest, PathViewEnablesValidatedEditingAndRejectsLegacyRoutingApis)
{
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, build());
    ASSERT_NE(canvas.activeSignalPathSource(), nullptr);
    EXPECT_TRUE(canvas.activeSignalPathSource()->supportsEditing());
    EXPECT_FALSE(canvas.connectNodes(s5->objectID, c1->objectID).success);
    EXPECT_FALSE(canvas.disconnectNodes(c1->objectID, l1->objectID));
    EXPECT_FALSE(canvas.setNodeOverride(s5->objectID, false));
    for (auto* item : canvas.scene()->items()) {
        if (auto* node = dynamic_cast<NodeItem*>(item))
            EXPECT_TRUE(node->flags().testFlag(QGraphicsItem::ItemIsMovable));
    }
}

TEST_F(SignalPathRenderingTest, InvalidOrEmptySnapshotClearsThePreviousGraph)
{
    BlueprintCanvasWidget canvas;
    canvas.useSignalPaths(project, build());
    ASSERT_FALSE(canvas.scene()->items().empty());
    b1->comp<VMixNode>()->sends.push_back({9999, 0.25f});
    const auto invalid = build();
    ASSERT_FALSE(invalid.ok());
    canvas.useSignalPaths(project, invalid);
    EXPECT_TRUE(canvas.scene()->items().empty());
    EXPECT_FALSE(canvas.activeSignalPathSource()->graph().ok());
    EXPECT_EQ(canvas.signalPathNode(0), nullptr);
    canvas.useSignalPaths(project, {});
    EXPECT_TRUE(canvas.scene()->items().empty());
    EXPECT_TRUE(canvas.activeSignalPathSource()->graph().ok());
    canvas.clear();
    EXPECT_TRUE(canvas.scene()->items().empty());
    canvas.useSignalChain(nullptr);
    EXPECT_NE(canvas.activeSignalChainSource(), nullptr);
    EXPECT_EQ(canvas.activeSignalPathSource(), nullptr);
}

TEST_F(SignalPathRenderingTest, PressingAndReleasingCompactSliderPreservesValueAndTogglesEditingState)
{
    const QString previousStyle = qApp->styleSheet();
    const auto restore = qScopeGuard([&]() { qApp->setStyleSheet(previousStyle); });
    qApp->setStyleSheet(QtControlSkin::current().applicationStyleSheet());
    NodeContentInfo info;
    info.volume = -48.0f;
    info.pitch = 0.0f;
    NodeContentEditor editor(info, kAllNodeContentFields,
        NodeContentEditor::labelWidth(kAllNodeContentFields));
    editor.show();
    QApplication::processEvents();
    auto* host = editor.findChild<ParameterControlHost*>("nodeSlider_volume");
    ASSERT_NE(host, nullptr);
    auto* slider = host->findChild<QSlider*>();
    ASSERT_NE(slider, nullptr);
    QStyleOptionSlider option;
    option.initFrom(slider);
    option.state |= QStyle::State_Horizontal;
    option.orientation = Qt::Horizontal;
    option.minimum = slider->minimum();
    option.maximum = slider->maximum();
    option.sliderPosition = slider->sliderPosition();
    option.sliderValue = slider->value();
    const auto sliderHandle = slider->style()->subControlRect(QStyle::CC_Slider, &option,
        QStyle::SC_SliderHandle, slider);
    const auto value = host->binding()->value();
    QMouseEvent press(QEvent::MouseButtonPress, sliderHandle.center(),
        slider->mapToGlobal(sliderHandle.center()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(slider, &press);
    ASSERT_TRUE(slider->isSliderDown());
    ASSERT_TRUE(host->binding()->isEditing());
    EXPECT_DOUBLE_EQ(host->binding()->value(), value);
    QMouseEvent release(QEvent::MouseButtonRelease, sliderHandle.center(),
        slider->mapToGlobal(sliderHandle.center()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(slider, &release);
    EXPECT_FALSE(slider->isSliderDown());
    EXPECT_FALSE(host->binding()->isEditing());
    EXPECT_DOUBLE_EQ(host->binding()->value(), value);
}

}
