#include <harken/Harken.h>
#include "Engine.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/ProjectExplorer/ProjectExplorer.h"
#include "Widgets/Blueprint/BlueprintPanel.h"
#include "Widgets/Blueprint/SignalPathPlayback.h"
#include "Widgets/Blueprint/SignalPathEditing.h"
#include "Widgets/Blueprint/NodeContentEditor.h"
#include "Widgets/Common/DecimalSpinBox.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ValueSegmentedControl.h"
#include "Widgets/Blueprint/ConnectionPath.h"
#include "Core/AudioEngine.h"
#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/Components/VSound.h"
#include "Core/RuntimeSyncCoordinator.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Core/ActionManager.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSelection.h"
#include "Core/Components/VTrack.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/PropertyEditSession.h"
#include "Core/QtControlSkin.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/SelectionRouter.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include "Core/HarkenStyle.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"

#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleOptionFrame>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QSlider>
#include <QTreeWidget>
#include <QWheelEvent>

#include <DockWidget.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace {

using namespace VNX;
using T = ProjectItemType;

void drainEvents()
{
    QApplication::processEvents();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void expectContentFitsProxy(NodeContentEditor* editor)
{
    const auto* proxy = editor->graphicsProxyWidget();
    ASSERT_NE(proxy, nullptr);
    EXPECT_EQ(proxy->size(), QSizeF(editor->size()));
    EXPECT_EQ(proxy->minimumSize(), QSizeF(editor->minimumSize()));
    EXPECT_EQ(proxy->maximumSize(), QSizeF(editor->maximumSize()));
    auto* grid = qobject_cast<QGridLayout*>(editor->layout());
    for (int row = 0; row < grid->rowCount(); ++row) {
        auto* stack = grid->itemAtPosition(row, 2)->widget();
        if (stack->isHidden()) continue;
        EXPECT_TRUE(proxy->boundingRect().contains(QRectF(stack->geometry())));
    }
}

class InteractiveBlueprint : public BlueprintCanvasWidget
{
public:
    void menuAt(QMenu& menu, const QPoint& position)
    {
        QContextMenuEvent event(QContextMenuEvent::Mouse, position, viewport()->mapToGlobal(position));
        populateContextMenu(menu, &event);
    }
};

void sendMouse(BlueprintCanvasWidget& canvas, QEvent::Type type, const QPoint& position,
               Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, position, canvas.viewport()->mapToGlobal(position), button, buttons, modifiers);
    QApplication::sendEvent(canvas.viewport(), &event);
}

void sendWheel(BlueprintCanvasWidget& canvas, const QPoint& position, const QPoint& delta,
               Qt::MouseButtons buttons = Qt::NoButton)
{
    QWheelEvent event(position, canvas.viewport()->mapToGlobal(position), {}, delta,
                      buttons, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas.viewport(), &event);
}

QStringList menuLabels(const QMenu& menu)
{
    QStringList labels;
    for (const auto* action : menu.actions()) {
        if (action->isVisible() && !action->isSeparator()) labels.push_back(action->text());
    }
    return labels;
}

QAction* actionNamed(const QMenu& menu, const QString& label)
{
    for (auto* action : menu.actions()) {
        if (action->text() == label) return action;
    }
    return nullptr;
}

void expectDisjointNodes(const BlueprintCanvasWidget& canvas)
{
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(graph.ok());
    std::vector<const NodeItem*> nodes;
    for (size_t index = 0; index < graph.nodes.size(); ++index)
        nodes.push_back(canvas.signalPathNode(index));
    for (size_t index = 0; index < graph.edges.size(); ++index)
        if (const auto* gain = canvas.signalPathGainNode(index)) nodes.push_back(gain);
    for (size_t index = 0; index < nodes.size(); ++index) {
        for (size_t other = index + 1; other < nodes.size(); ++other) {
            SCOPED_TRACE(std::to_string(index) + " / " + std::to_string(other));
            EXPECT_FALSE(nodes[index]->sceneBoundingRect().intersects(nodes[other]->sceneBoundingRect()));
        }
    }
}

class SignalPathObservationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        registerBuiltinInspectorCustomizations();
        for (const auto* key : {"Blueprint/InteractionMode", "Blueprint/GridSnapEnabled", "Blueprint/NodeContentFields"})
            preferences.insert(QLatin1String(key), QSettings().value(QLatin1String(key)));
        auto& pm = ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        ProjectConfig config;
        config.name = QStringLiteral("SignalPathObservation");
        config.location = directory.path();
        config.platforms = HarkenTest::platforms({"Windows", "Android"});
        ASSERT_TRUE(pm.createProject(config));
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
        master = pm.getMasterBusItem();
        b1 = create("B1", T::Bus, master);
        b2 = create("B2", T::Bus, master);
        b3 = create("B3", T::Bus, master);
        l3 = create("L3", T::Layer);
        l1 = create("L1", T::Layer, l3);
        p1 = create("P1", T::Piece, l1);
        t1 = create("T1", T::Track, p1);
        s1 = create("S1", T::Sound, t1);
        s2 = create("S2", T::Sound, t1);
        t2 = create("T2", T::Track, p1);
        s3 = create("S3", T::Sound, t2);
        c1 = create("C1", T::Container, l1);
        s4 = create("S4", T::Sound, c1);
        s5 = create("S5", T::Sound, c1);
        l2 = create("L2", T::Layer, l3);
        t3 = create("T3", T::Track, l2);
        s6 = create("S6", T::Sound, t3);
        output(p1, b1);
        output(c1, b1);
        output(s5, b3);
        output(l2, b1);
        c1->fieldGroups.send.shared = {FieldGroupMode::Local,
            SendFieldGroup{{{toFieldGroupGuid(b2->guid), -6.0f}}}};
        auto& actions = ActionManager::instance();
        actions.clearAllForTesting();
        registerProjectActions(actions, &pm, nullptr, {});
        registerMixerActions(actions, &AudioEngine::instance(), &pm, nullptr);
        registerFieldGroupActions(actions, &pm);
        UndoRedoManager::instance().clear();
        drainEvents();
    }

    void TearDown() override
    {
        QSettings settings;
        for (auto it = preferences.cbegin(); it != preferences.cend(); ++it) {
            if (it.value().isValid()) settings.setValue(it.key(), it.value());
            else settings.remove(it.key());
        }
        UndoRedoManager::instance().clear();
        ActionManager::instance().clearAllForTesting();
        ProjectManager::instance().closeProject();
        drainEvents();
        PlatformContextService::instance().setStorageForTesting(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings, QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    ProjectItem* create(const char* name, T type, ProjectItem* parent = nullptr)
    {
        auto* item = ProjectManager::instance().createItem(parent, QString::fromLatin1(name), type);
        EXPECT_NE(item, nullptr) << name;
        return item;
    }

    PlayablePropertyLocatorV1 place(ProjectItem* track, const ProjectItem* sound)
    {
        Clip clip{QUuid::createUuid(), sound->guid, 0, 1};
        clip.clipGuid = QUuid::createUuid();
        auto& ledger = track->comp<VTrack>()->clips;
        EXPECT_TRUE(ledger.mintOccurrenceOrdinal(&clip));
        ledger.items.push_back(clip);
        return *PlayablePropertyAuthoringV1::trackClipLocator(*track, clip.clipGuid);
    }

    void output(ProjectItem* item, const ProjectItem* bus)
    {
        item->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
            MainOutputFieldGroup{toFieldGroupGuid(bus->guid), 0.0f}};
    }

    SignalPathGraph build(ProjectItem* item, std::vector<PlayablePropertyLocatorV1> uses = {})
    {
        return buildSignalPathGraphForScope(*ProjectManager::instance().currentProject(),
            {item->guid, std::move(uses)}, HarkenTest::FieldGroupPlatformIds::Windows);
    }

    BlueprintCanvasWidget::ConnectResult connectPath(const SignalPathGraph& graph,
        SignalPathEndpoint from, SignalPathEndpoint to, bool addSend = false)
    {
        auto plan = prepareSignalPathConnect(graph, from, to, addSend);
        BlueprintCanvasWidget::ConnectResult result;
        result.error = plan.error;
        if (plan.ok()) result.success = applySignalPathConnect(std::move(plan));
        return result;
    }

    size_t indexOf(const SignalPathGraph& graph, ProjectItem* item)
    {
        for (size_t index = 0; index < graph.nodes.size(); ++index) {
            if (graph.nodes[index].key.objectGuid == item->guid) return index;
        }
        ADD_FAILURE() << "Missing node " << item->name.toStdString();
        return graph.nodes.size();
    }

    void doubleClickNode(MainWindow& window, ProjectItem* item)
    {
        auto* canvas = window.blueprintCanvas();
        ASSERT_NE(canvas, nullptr);
        ASSERT_NE(canvas->activeSignalPathSource(), nullptr);
        auto* node = canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), item));
        ASSERT_NE(node, nullptr);
        canvas->centerOn(node);
        const auto transform = canvas->transform();
        const auto center = canvas->mapToScene(canvas->viewport()->rect().center());
        const auto body = node->shape().boundingRect();
        const auto position = canvas->mapFromScene(node->mapToScene(
            QPointF(body.center().x(), body.bottom() - 2)));
        ASSERT_TRUE(canvas->viewport()->rect().contains(position));
        sendMouse(*canvas, QEvent::MouseButtonDblClick, position, Qt::LeftButton, Qt::LeftButton);
        drainEvents();
        EXPECT_EQ(canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), item)), node);
        EXPECT_EQ(canvas->transform(), transform);
        EXPECT_LT(QLineF(canvas->mapToScene(canvas->viewport()->rect().center()), center).length(), 2.0);
    }

    size_t count(const SignalPathGraph& graph, ProjectItem* item)
    {
        return static_cast<size_t>(std::count_if(graph.nodes.begin(), graph.nodes.end(),
            [&](const auto& node) { return node.key.objectGuid == item->guid; }));
    }

    size_t routingEdge(const SignalPathGraph& graph, SignalPathEdgeKind kind,
                       ProjectItem* provider, ProjectItem* target)
    {
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            const auto& edge = graph.edges[index];
            if (edge.kind == kind && edge.routingSource
                && edge.routingSource->objectGuid == provider->guid
                && graph.nodes[edge.to.occurrenceIndex].key.objectGuid == target->guid)
                return index;
        }
        throw std::runtime_error("Expected routing connection is missing");
    }

    size_t hierarchyEdge(const SignalPathGraph& graph, ProjectItem* child, ProjectItem* parent)
    {
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            const auto& edge = graph.edges[index];
            if (edge.kind == SignalPathEdgeKind::Through
                && graph.nodes[edge.from.occurrenceIndex].key.objectGuid == child->guid
                && graph.nodes[edge.to.occurrenceIndex].key.objectGuid == parent->guid)
                return index;
        }
        throw std::runtime_error("Expected Through connection is missing");
    }

    NodeContentEditor* editorFor(BlueprintCanvasWidget& canvas, ProjectItem* item)
    {
        auto* node = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), item));
        return qobject_cast<NodeContentEditor*>(node->contentWidget());
    }

    NodeContentEditor* gainEditorFor(BlueprintCanvasWidget& canvas, SignalPathEdgeKind kind,
                                    ProjectItem* provider, ProjectItem* target)
    {
        const auto index = routingEdge(canvas.activeSignalPathSource()->graph(), kind, provider, target);
        return qobject_cast<NodeContentEditor*>(canvas.signalPathGainNode(index)->contentWidget());
    }

    double value(ProjectItem* item, const char* field, PlatformScope scope = HarkenTest::PlatformIds::Windows)
    {
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
        const auto* descriptor = PlayablePropertyAuthoringV1::fieldByName(QLatin1String(field));
        return PlayablePropertyAuthoringV1::readProperty(*item, *locator, descriptor->id, scope)->value.toDouble();
    }

    const SignalPath& path(const SignalPathGraph& graph, ProjectItem* source)
    {
        const auto found = std::find_if(graph.paths.begin(), graph.paths.end(),
            [&](const auto& entry) { return entry.identity.sourceGuid == source->guid; });
        if (found == graph.paths.end()) {
            QStringList errors{QStringLiteral("Missing signal path: ") + source->name};
            for (const auto& diagnostic : graph.diagnostics) errors.push_back(diagnostic.message);
            throw std::runtime_error(errors.join(QStringLiteral("; ")).toStdString());
        }
        return *found;
    }

    void expectRoute(const SignalPathGraph& graph, ProjectItem* source, const QStringList& names,
                     ProjectItem* outputBus, const std::vector<ProjectItem*>& sends = {})
    {
        ASSERT_TRUE(graph.ok());
        const auto& route = path(graph, source);
        const auto routeIndex = static_cast<size_t>(&route - graph.paths.data());
        QStringList actualNames;
        for (const auto endpoint : route.computation) {
            const auto& node = graph.nodes[endpoint.occurrenceIndex];
            actualNames.push_back(node.name);
            ASSERT_EQ(node.portPairs.size(), 1u);
            const auto& paths = node.portPairs.front().pathIndices;
            EXPECT_NE(std::find(paths.begin(), paths.end(), routeIndex), paths.end());
        }
        EXPECT_EQ(actualNames, names);
        EXPECT_EQ(route.routing.outputBusID, outputBus->objectID);
        std::vector<uint32_t> outputTargets, sendTargets, expectedSends;
        for (const auto* send : sends) expectedSends.push_back(send->objectID);
        for (size_t index : route.edgeIndices) {
            const auto& edge = graph.edges[index];
            if (edge.from != route.computation.back()) continue;
            const auto target = graph.nodes[edge.to.occurrenceIndex].objectID;
            if (edge.kind == SignalPathEdgeKind::Output) outputTargets.push_back(target);
            if (edge.kind == SignalPathEdgeKind::Send) sendTargets.push_back(target);
        }
        EXPECT_EQ(outputTargets, (std::vector{outputBus->objectID}));
        EXPECT_EQ(sendTargets, expectedSends);
        for (size_t index = 1; index < route.computation.size(); ++index) {
            EXPECT_TRUE(std::any_of(route.edgeIndices.begin(), route.edgeIndices.end(), [&](size_t edgeIndex) {
                const auto& edge = graph.edges[edgeIndex];
                return edge.kind == SignalPathEdgeKind::Through && edge.from == route.computation[index - 1]
                    && edge.to == route.computation[index];
            }));
        }
    }

    ActionResult edit(ProjectItem* item, const char* group, const char* action, QJsonObject args)
    {
        args.insert("definition_guid", item->guid.toString(QUuid::WithoutBraces));
        args.insert("group", QLatin1String(group));
        args.insert("scope", "Shared");
        return ActionManager::instance().execute(QLatin1String(action), args);
    }

    QTemporaryDir directory;
    QVariantMap preferences;
    ProjectItem *master = nullptr, *b1 = nullptr, *b2 = nullptr, *b3 = nullptr;
    ProjectItem *l3 = nullptr, *l1 = nullptr, *l2 = nullptr, *p1 = nullptr, *c1 = nullptr;
    ProjectItem *t1 = nullptr, *t2 = nullptr, *t3 = nullptr;
    ProjectItem *s1 = nullptr, *s2 = nullptr, *s3 = nullptr, *s4 = nullptr, *s5 = nullptr, *s6 = nullptr;
};

TEST_F(SignalPathObservationTest, EditingSharedObjectNodeCommitsOneUndoForAllUses)
{
    place(t1, s1);
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(kAllNodeContentFields);
    auto* editor = editorFor(canvas, s1);
    ASSERT_NE(editor, nullptr);
    ASSERT_EQ(count(canvas.activeSignalPathSource()->graph(), s1), 1u);
    auto* binding = editor->binding(NodeContentField::Volume);
    binding->beginEdit();
    binding->previewEdit(-6.5);
    drainEvents();
    EXPECT_EQ(editorFor(canvas, s1), editor);
    EXPECT_DOUBLE_EQ(editor->binding(NodeContentField::Volume)->value(), -6.5);
    EXPECT_DOUBLE_EQ(value(s1, "volume"), -6.5);
    EXPECT_DOUBLE_EQ(value(s1, "volume", HarkenTest::PlatformIds::Android), 0.0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    binding->commitEdit();
    drainEvents();
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_DOUBLE_EQ(value(s1, "volume"), 0.0);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainEvents();
    binding = editorFor(canvas, s1)->binding(NodeContentField::Volume);
    binding->beginEdit();
    binding->previewEdit(-12.0);
    binding->cancelEdit();
    drainEvents();
    EXPECT_DOUBLE_EQ(value(s1, "volume"), -6.5);
    EXPECT_DOUBLE_EQ(editorFor(canvas, s1)->binding(NodeContentField::Volume)->value(), -6.5);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(SignalPathObservationTest, PitchIsIntegralAndOffsetPreviewsCommitAndUndo)
{
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(kAllNodeContentFields);
    auto* editor = editorFor(canvas, s1);
    editor->binding(NodeContentField::Pitch)->commitValue(123.4);
    drainEvents();
    EXPECT_DOUBLE_EQ(value(s1, "pitch"), 123.0);
    auto* offset = editorFor(canvas, s1)->binding(NodeContentField::OffsetOfStart);
    offset->beginEdit();
    offset->previewEdit(0.125);
    EXPECT_DOUBLE_EQ(s1->comp<VSound>()->scheduleOffsetSec, 0.125);
    offset->commitEdit();
    drainEvents();
    EXPECT_DOUBLE_EQ(s1->comp<VSound>()->scheduleOffsetSec, 0.125);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_DOUBLE_EQ(s1->comp<VSound>()->scheduleOffsetSec, 0.0);
    offset = editorFor(canvas, s1)->binding(NodeContentField::OffsetOfStart);
    offset->commitValue(0.375);
    drainEvents();
    EXPECT_DOUBLE_EQ(s1->comp<VSound>()->scheduleOffsetSec, 0.375);
}

TEST_F(SignalPathObservationTest, InspectorAndBlueprintSynchronizeDuringPreviewAndCancellation)
{
    place(t1, s1);
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(kAllNodeContentFields);
    InspectorPanel inspector;
    inspector.showItemInspector(s1);
    drainEvents();
    const std::array fields{
        std::tuple{NodeContentField::Volume, "volume", -6.5},
        std::tuple{NodeContentField::Pitch, "pitch", 123.0},
        std::tuple{NodeContentField::OffsetOfStart, "scheduleOffsetSec", 0.125}};
    for (const auto& [field, name, preview] : fields) {
        SCOPED_TRACE(name);
        auto* control = inspector.findChild<ParameterSlider*>(QStringLiteral("prop_") + QLatin1String(name));
        ASSERT_NE(control, nullptr);
        auto* inspectorBinding = control->binding();
        auto* editor = editorFor(canvas, s1);
        auto* nodeBinding = editor->binding(field);
        const auto original = nodeBinding->value();
        inspectorBinding->beginEdit();
        inspectorBinding->previewEdit(preview);
        drainEvents();
        EXPECT_TRUE(inspectorBinding->isEditing());
        EXPECT_EQ(editorFor(canvas, s1), editor);
        EXPECT_DOUBLE_EQ(nodeBinding->value(), preview);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
        inspectorBinding->cancelEdit();
        EXPECT_DOUBLE_EQ(nodeBinding->value(), original);

        nodeBinding->beginEdit();
        nodeBinding->previewEdit(preview);
        drainEvents();
        EXPECT_TRUE(nodeBinding->isEditing());
        EXPECT_DOUBLE_EQ(inspectorBinding->value(), preview);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
        nodeBinding->cancelEdit();
        EXPECT_DOUBLE_EQ(inspectorBinding->value(), original);
        drainEvents();
    }

    auto* binding = editorFor(canvas, s1)->binding(NodeContentField::Volume);
    binding->beginEdit();
    binding->previewEdit(-9.5);
    binding->commitEdit();
    drainEvents();
    auto* control = inspector.findChild<ParameterSlider*>(QStringLiteral("prop_volume"));
    EXPECT_DOUBLE_EQ(control->value(), -9.5);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_DOUBLE_EQ(control->value(), 0.0);
    EXPECT_DOUBLE_EQ(editorFor(canvas, s1)->binding(NodeContentField::Volume)->value(), 0.0);
}

TEST_F(SignalPathObservationTest, InspectorOutputVolumePreviewUpdatesGainWithoutAuthoringInheritedObjects)
{
    PlatformContextService::instance().setAll(true);
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(kAllNodeContentFields);
    InspectorPanel inspector;
    inspector.showItemInspector(c1);
    drainEvents();
    auto* control = inspector.findChild<ParameterSlider*>(QStringLiteral("prop_outputBusVolume"));
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(control->isEnabled());
    auto* binding = control->binding();
    const auto routeEditor = [&]() { return gainEditorFor(canvas, SignalPathEdgeKind::Output, c1, b1); };
    auto* editor = routeEditor();
    ASSERT_NE(editor->binding(NodeContentField::OutputVolume), nullptr);
    const auto before = c1->fieldGroups;
    const auto childBefore = s4->fieldGroups;
    binding->beginEdit();
    binding->previewEdit(-6.0);
    drainEvents();
    EXPECT_TRUE(binding->isEditing());
    EXPECT_EQ(routeEditor(), editor);
    EXPECT_DOUBLE_EQ(editor->binding(NodeContentField::OutputVolume)->value(), -6.0);
    EXPECT_DOUBLE_EQ(gainEditorFor(canvas, SignalPathEdgeKind::Output, s5, b3)
        ->binding(NodeContentField::OutputVolume)->value(), 0.0);
    EXPECT_EQ(c1->fieldGroups, before);
    EXPECT_EQ(s4->fieldGroups, childBefore);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    binding->cancelEdit();
    EXPECT_DOUBLE_EQ(editor->binding(NodeContentField::OutputVolume)->value(), 0.0);
    binding->beginEdit();
    binding->previewEdit(-6.0);
    binding->commitEdit();
    drainEvents();
    EXPECT_DOUBLE_EQ(routeEditor()->binding(NodeContentField::OutputVolume)->value(), -6.0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_EQ(s4->fieldGroups, childBefore);
    binding->beginEdit();
    binding->previewEdit(-12.0);
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    drainEvents();
    EXPECT_FALSE(binding->isEditing());
    EXPECT_DOUBLE_EQ(routeEditor()->binding(NodeContentField::OutputVolume)->value(), -12.0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 2);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_DOUBLE_EQ(routeEditor()->binding(NodeContentField::OutputVolume)->value(), -6.0);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_DOUBLE_EQ(routeEditor()->binding(NodeContentField::OutputVolume)->value(), 0.0);
}

TEST_F(SignalPathObservationTest, GainOutputVolumeEditsSynchronizeInspectorAndPreserveInheritance)
{
    PlatformContextService::instance().setAll(true);
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields({});
    InspectorPanel inspector;
    inspector.showItemInspector(c1);
    drainEvents();
    auto* control = inspector.findChild<ParameterSlider*>(QStringLiteral("prop_outputBusVolume"));
    ASSERT_NE(control, nullptr);
    const auto routeEditor = [&]() { return gainEditorFor(canvas, SignalPathEdgeKind::Output, c1, b1); };
    auto* editor = routeEditor();
    auto* binding = editor->binding(NodeContentField::OutputVolume);
    ASSERT_NE(binding, nullptr);
    const auto childBefore = s4->fieldGroups;
    const auto providerBefore = c1->fieldGroups;
    binding->beginEdit();
    binding->previewEdit(-9.5);
    drainEvents();
    EXPECT_EQ(routeEditor(), editor);
    EXPECT_TRUE(binding->isEditing());
    EXPECT_DOUBLE_EQ(control->value(), -9.5);
    EXPECT_EQ(s4->fieldGroups, childBefore);
    EXPECT_EQ(c1->fieldGroups, providerBefore);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    binding->cancelEdit();
    drainEvents();
    EXPECT_DOUBLE_EQ(control->value(), 0.0);
    binding = routeEditor()->binding(NodeContentField::OutputVolume);
    binding->beginEdit();
    binding->previewEdit(-6.5);
    binding->commitEdit();
    drainEvents();
    EXPECT_DOUBLE_EQ(control->value(), -6.5);
    EXPECT_FLOAT_EQ(c1->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -6.5f);
    EXPECT_EQ(s4->fieldGroups, childBefore);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_DOUBLE_EQ(control->value(), 0.0);
    EXPECT_DOUBLE_EQ(routeEditor()->binding(NodeContentField::OutputVolume)->value(), 0.0);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainEvents();
    EXPECT_DOUBLE_EQ(control->value(), -6.5);
    EXPECT_DOUBLE_EQ(routeEditor()->binding(NodeContentField::OutputVolume)->value(), -6.5);
    EXPECT_EQ(s4->fieldGroups, childBefore);
}

TEST_F(SignalPathObservationTest, NumericValueDoubleClickOpensEditorWithoutInspectingOrMovingNode)
{
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.resize(1200, 700);
    canvas.show();
    canvas.focusItem(s1);
    drainEvents();
    auto* node = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s1));
    auto* editor = editorFor(canvas, s1);
    auto* label = editor->findChild<QLabel*>("nodeValue_pitch");
    auto* input = editor->findChild<QDoubleSpinBox*>("nodeEditor_pitch");
    ASSERT_NE(label, nullptr);
    ASSERT_NE(input, nullptr);
    EXPECT_FALSE(input->isVisible());
    EXPECT_EQ(editor->binding(NodeContentField::OutputVolume), nullptr);
    EXPECT_EQ(editor->findChild<QDoubleSpinBox*>("nodeEditor_outputBusVolume"), nullptr);
    const auto position = node->pos();
    uint32_t inspected = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeDoubleClicked, &canvas,
        [&](uint32_t id) { inspected = id; });
    const auto point = canvas.mapFromScene(editor->graphicsProxyWidget()->mapToScene(
        label->mapTo(editor, label->rect().center())));
    sendMouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    sendMouse(canvas, QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    EXPECT_TRUE(input->isVisible());
    EXPECT_EQ(inspected, 0u);
    EXPECT_EQ(node->pos(), position);
    input->setValue(321);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input, &enter);
    drainEvents();
    EXPECT_DOUBLE_EQ(value(s1, "pitch"), 321.0);
    EXPECT_FALSE(editorFor(canvas, s1)->findChild<QDoubleSpinBox*>("nodeEditor_pitch")->isVisible());
    sendMouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    sendMouse(canvas, QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    input = editorFor(canvas, s1)->findChild<QDoubleSpinBox*>("nodeEditor_pitch");
    ASSERT_TRUE(input->isVisible());
    input->setValue(999);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(input, &escape);
    drainEvents();
    EXPECT_DOUBLE_EQ(value(s1, "pitch"), 321.0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(SignalPathObservationTest, GainSendVolumeEditsTheSelectedRouteAndTracksInspectorValues)
{
    PlatformContextService::instance().setAll(true);
    BlueprintCanvasWidget canvas;
    InspectorPanel inspector;
    inspector.showItemInspector(c1);
    drainEvents();
    auto* sends = inspector.findChild<QWidget*>(QStringLiteral("prop_sends"));
    ASSERT_NE(sends, nullptr);
    auto* control = sends->findChild<ParameterSlider*>();
    ASSERT_NE(control, nullptr);
    const auto inspectorControl = [&]() {
        return inspector.findChild<QWidget*>(QStringLiteral("prop_sends"))->findChild<ParameterSlider*>();
    };
    const auto routeEditor = [&]() { return gainEditorFor(canvas, SignalPathEdgeKind::Send, c1, b2); };
    auto* editor = routeEditor();
    auto* binding = editor->binding(NodeContentField::SendVolume);
    ASSERT_NE(binding, nullptr);
    EXPECT_NEAR(binding->value(), -6.0, 0.0001);
    const auto childBefore = s4->fieldGroups;
    const auto otherChildBefore = s5->fieldGroups;
    const auto outputBefore = c1->fieldGroups.mainOutput;
    control->binding()->beginEdit();
    control->binding()->previewEdit(-9.0);
    drainEvents();
    EXPECT_EQ(routeEditor(), editor);
    EXPECT_NEAR(binding->value(), -9.0, 0.0001);
    control->binding()->cancelEdit();
    drainEvents();
    EXPECT_NEAR(routeEditor()->binding(NodeContentField::SendVolume)->value(), -6.0, 0.0001);
    binding = routeEditor()->binding(NodeContentField::SendVolume);
    binding->beginEdit();
    binding->previewEdit(-12.0);
    drainEvents();
    EXPECT_NEAR(inspectorControl()->value(), -12.0, 0.0001);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    binding->commitEdit();
    drainEvents();
    ASSERT_EQ(c1->fieldGroups.send.shared.localHistory->sends.size(), 1u);
    EXPECT_NEAR(c1->fieldGroups.send.shared.localHistory->sends.front().sendLevel, -12.0f, 0.0001);
    EXPECT_EQ(c1->fieldGroups.mainOutput, outputBefore);
    EXPECT_EQ(s4->fieldGroups, childBefore);
    EXPECT_EQ(s5->fieldGroups, otherChildBefore);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_NEAR(routeEditor()->binding(NodeContentField::SendVolume)->value(), -6.0, 0.0001);
    EXPECT_NEAR(inspectorControl()->value(), -6.0, 0.0001);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainEvents();
    EXPECT_NEAR(routeEditor()->binding(NodeContentField::SendVolume)->value(), -12.0, 0.0001);
    EXPECT_NEAR(inspectorControl()->value(), -12.0, 0.0001);
}

TEST_F(SignalPathObservationTest, InspectorBusSendVolumePreviewsCancelAndCommitToGain)
{
    PlatformContextService::instance().setAll(true);
    b1->comp<VMixNode>()->sends = {{b2->objectID, VMixNode::dbToLinear(-6.0f)}};
    b1->playablePropertyPresence.Set(harkenplayableproperties::PlayablePropertyIdV1::Sends);
    const auto before = PlayablePropertyAuthoringV1::definitionToJson(*b1);
    BlueprintCanvasWidget canvas;
    InspectorPanel inspector;
    inspector.showItemInspector(b1);
    drainEvents();
    auto* sends = inspector.findChild<QWidget*>(QStringLiteral("prop_sends"));
    ASSERT_NE(sends, nullptr);
    auto* control = sends->findChild<ParameterSlider*>();
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(control->isEnabled());
    const auto routeEditor = [&]() { return gainEditorFor(canvas, SignalPathEdgeKind::Send, b1, b2); };
    auto* editor = routeEditor();
    ASSERT_NE(editor->binding(NodeContentField::SendVolume), nullptr);
    EXPECT_NEAR(editor->binding(NodeContentField::SendVolume)->value(), -6.0, 0.0001);
    control->binding()->beginEdit();
    control->binding()->previewEdit(-9.0);
    drainEvents();
    EXPECT_TRUE(control->binding()->isEditing());
    EXPECT_EQ(routeEditor(), editor);
    EXPECT_NEAR(editor->binding(NodeContentField::SendVolume)->value(), -9.0, 0.0001);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    control->binding()->cancelEdit();
    drainEvents();
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*b1), before);
    EXPECT_NEAR(routeEditor()->binding(NodeContentField::SendVolume)->value(), -6.0, 0.0001);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    control = inspector.findChild<QWidget*>(QStringLiteral("prop_sends"))->findChild<ParameterSlider*>();
    ASSERT_NE(control, nullptr);
    control->binding()->beginEdit();
    control->binding()->previewEdit(-12.0);
    drainEvents();
    EXPECT_NEAR(routeEditor()->binding(NodeContentField::SendVolume)->value(), -12.0, 0.0001);
    control->binding()->commitEdit();
    drainEvents();
    ASSERT_EQ(b1->comp<VMixNode>()->sends.size(), 1u);
    EXPECT_EQ(b1->comp<VMixNode>()->sends.front().targetBusID, b2->objectID);
    EXPECT_NEAR(VMixNode::linearToDb(b1->comp<VMixNode>()->sends.front().sendLevel), -12.0, 0.0001);
    EXPECT_NEAR(routeEditor()->binding(NodeContentField::SendVolume)->value(), -12.0, 0.0001);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*b1), before);
    EXPECT_NEAR(routeEditor()->binding(NodeContentField::SendVolume)->value(), -6.0, 0.0001);
    auto* restored = inspector.findChild<QWidget*>(QStringLiteral("prop_sends"))->findChild<ParameterSlider*>();
    ASSERT_NE(restored, nullptr);
    EXPECT_NEAR(restored->value(), -6.0, 0.0001);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainEvents();
    EXPECT_NEAR(VMixNode::linearToDb(b1->comp<VMixNode>()->sends.front().sendLevel), -12.0, 0.0001);
    EXPECT_NEAR(routeEditor()->binding(NodeContentField::SendVolume)->value(), -12.0, 0.0001);
    restored = inspector.findChild<QWidget*>(QStringLiteral("prop_sends"))->findChild<ParameterSlider*>();
    ASSERT_NE(restored, nullptr);
    EXPECT_NEAR(restored->value(), -12.0, 0.0001);
}

TEST_F(SignalPathObservationTest, SwitchingValueEditorsCommitsBothFieldsWithSeparateUndoSteps)
{
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.resize(1200, 750);
    canvas.show();
    canvas.focusItem(s1);
    drainEvents();
    const auto open = [&](const QString& field) {
        auto* editor = editorFor(canvas, s1);
        auto* label = editor->findChild<QLabel*>(QStringLiteral("nodeValue_") + field);
        const auto point = canvas.mapFromScene(editor->graphicsProxyWidget()->mapToScene(
            label->mapTo(editor, label->rect().center())));
        sendMouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        sendMouse(canvas, QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        return editor->findChild<QDoubleSpinBox*>(QStringLiteral("nodeEditor_") + field);
    };
    auto* volume = open(QStringLiteral("volume"));
    ASSERT_TRUE(volume->isVisible());
    volume->setValue(-6.5);
    auto* pitch = open(QStringLiteral("pitch"));
    ASSERT_TRUE(pitch->isVisible());
    EXPECT_FALSE(volume->isVisible());
    pitch->setValue(200);
    editorFor(canvas, s1)->finishEditing(true);
    drainEvents();
    EXPECT_DOUBLE_EQ(value(s1, "volume"), -6.5);
    EXPECT_DOUBLE_EQ(value(s1, "pitch"), 200.0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 2);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_DOUBLE_EQ(value(s1, "pitch"), 0.0);
    EXPECT_DOUBLE_EQ(value(s1, "volume"), -6.5);
}

TEST_F(SignalPathObservationTest, LoopExpandsOnDoubleClickAndCollapsesAfterCommitOrEscape)
{
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.show();
    const auto open = [&]() {
        auto* editor = editorFor(canvas, s1);
        auto* label = editor->findChild<QLabel*>("nodeValue_loopCount");
        QMouseEvent event(QEvent::MouseButtonDblClick, label->rect().center(),
            label->mapToGlobal(label->rect().center()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(label, &event);
        return editor->findChild<ValueSegmentedControl*>("nodeEditor_loopCount");
    };
    EXPECT_FALSE(editorFor(canvas, s1)->findChild<ValueSegmentedControl*>()->isVisible());
    auto* loop = open();
    ASSERT_TRUE(loop->isVisible());
    const auto buttons = loop->findChildren<QToolButton*>();
    ASSERT_EQ(buttons.size(), 2);
    buttons.back()->click();
    drainEvents();
    EXPECT_DOUBLE_EQ(value(s1, "loopCount"), 0.0);
    EXPECT_FALSE(editorFor(canvas, s1)->findChild<ValueSegmentedControl*>()->isVisible());
    loop = open();
    auto* count = loop->findChildren<QToolButton*>().front();
    QMouseEvent edit(QEvent::MouseButtonDblClick, count->rect().center(),
        count->mapToGlobal(count->rect().center()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(count, &edit);
    auto* input = loop->findChild<QLineEdit*>();
    ASSERT_TRUE(input->isVisible());
    input->setText(QStringLiteral("7"));
    editorFor(canvas, s1)->finishEditing(true);
    drainEvents();
    EXPECT_DOUBLE_EQ(value(s1, "loopCount"), 7.0);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_DOUBLE_EQ(value(s1, "loopCount"), 0.0);
    loop = open();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(loop, &escape);
    EXPECT_FALSE(loop->isVisible());
    EXPECT_DOUBLE_EQ(value(s1, "loopCount"), 0.0);
}

TEST_F(SignalPathObservationTest, LabelAndSliderColumnsStayFixedAndSliderDragDoesNotMoveNodes)
{
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.resize(1200, 750);
    canvas.show();
    canvas.focusItem(s1);
    drainEvents();
    auto* editor = editorFor(canvas, s1);
    auto* node = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s1));
    auto* grid = qobject_cast<QGridLayout*>(editor->layout());
    ASSERT_NE(grid, nullptr);
    const auto labelWidth = NodeContentEditor::labelWidth(NodeContentField::Volume
        | NodeContentField::Pitch | NodeContentField::OffsetOfStart | NodeContentField::Loop);
    for (auto* item : {s1, c1, l1, b1}) {
        auto* content = editorFor(canvas, item);
        auto* layout = qobject_cast<QGridLayout*>(content->layout());
        EXPECT_EQ(layout->itemAtPosition(0, 0)->widget()->width(), labelWidth);
        for (auto* slider : content->findChildren<ParameterControlHost*>())
            EXPECT_EQ(slider->width(), NodeContentEditor::kSliderWidth);
        for (auto* label : content->findChildren<QLabel*>())
            if (label->objectName().startsWith("nodeValue_"))
                EXPECT_EQ(label->width(), NodeContentEditor::kValueWidth);
    }
    const auto position = node->pos();
    const auto bounds = node->boundingRect();
    auto* host = editor->findChild<ParameterControlHost*>("nodeSlider_volume");
    auto* slider = host->findChild<QSlider*>();
    ASSERT_NE(slider, nullptr);
    const auto point = [&](int x) {
        return canvas.mapFromScene(editor->graphicsProxyWidget()->mapToScene(
            slider->mapTo(editor, QPoint(x, slider->height() / 2))));
    };
    sendMouse(canvas, QEvent::MouseButtonPress, point(slider->width() / 2), Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseMove, point(slider->width() / 3), Qt::NoButton, Qt::LeftButton);
    drainEvents();
    EXPECT_EQ(editorFor(canvas, s1), editor);
    EXPECT_EQ(node->pos(), position);
    EXPECT_EQ(node->boundingRect().height(), bounds.height());
    EXPECT_GE(node->boundingRect().width(), bounds.width());
    EXPECT_EQ(grid->itemAtPosition(0, 0)->widget()->width(), labelWidth);
    EXPECT_EQ(host->width(), NodeContentEditor::kSliderWidth);
    sendMouse(canvas, QEvent::MouseButtonRelease, point(slider->width() / 3), Qt::LeftButton, Qt::NoButton);
    drainEvents();
    EXPECT_NE(value(s1, "volume"), 0.0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(SignalPathObservationTest, NonSliderNodeSurfacesDragWithoutEditingParameters)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1200, 800);
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
    canvas.setGridSnapEnabled(false);
    canvas.setZoomFactor(1.0);
    canvas.show();
    drainEvents();
    for (auto* object : std::array<ProjectItem*, 4>{s1, c1, b1, nullptr}) {
        SCOPED_TRACE(object ? object->name.toStdString() : "Send gain");
        const auto nodeFor = [&]() {
            const auto& graph = canvas.activeSignalPathSource()->graph();
            return object ? canvas.signalPathNode(indexOf(graph, object))
                : canvas.signalPathGainNode(routingEdge(graph, SignalPathEdgeKind::Send, c1, b2));
        };
        for (const auto* surface : {"label", "value", "slider padding", "background", "header"}) {
            if (!object && QLatin1String(surface) == QLatin1String("header")) continue;
            SCOPED_TRACE(surface);
            canvas.scene()->clearSelection();
            auto* node = nodeFor();
            for (auto* item : canvas.scene()->items()) {
                auto* other = dynamic_cast<NodeItem*>(item);
                if (other && other != node) other->setPos(other->pos() + QPointF(5000, 5000));
            }
            auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget());
            ASSERT_NE(editor, nullptr);
            auto* grid = qobject_cast<QGridLayout*>(editor->layout());
            ASSERT_NE(grid, nullptr);
            auto* label = grid->itemAtPosition(0, 0)->widget();
            auto* value = editor->findChild<QLabel*>(object ? "nodeValue_volume" : "nodeValue_sendLevel");
            auto* host = editor->findChild<ParameterControlHost*>(object ? "nodeSlider_volume" : "nodeSlider_sendLevel");
            ASSERT_NE(value, nullptr);
            ASSERT_NE(host, nullptr);
            const auto field = object ? NodeContentField::Volume : NodeContentField::SendVolume;
            const auto originalValue = editor->binding(field)->value();
            const auto before = PlayablePropertyAuthoringV1::definitionToJson(*(object ? object : c1));
            const auto original = node->pos();
            canvas.centerOn(node);
            const auto widgetPoint = [&](QWidget* widget, QPoint position) {
                return canvas.mapFromScene(editor->graphicsProxyWidget()->mapToScene(widget->mapTo(editor, position)));
            };
            QPoint start;
            if (QLatin1String(surface) == QLatin1String("label"))
                start = widgetPoint(label, label->rect().center());
            else if (QLatin1String(surface) == QLatin1String("value"))
                start = widgetPoint(value, value->rect().center());
            else if (QLatin1String(surface) == QLatin1String("slider padding"))
                start = widgetPoint(host, QPoint(host->width() / 2, 1));
            else {
                const auto body = node->shape().boundingRect();
                const auto point = QLatin1String(surface) == QLatin1String("header")
                    ? QPointF(body.center().x(), 12) : QPointF(body.center().x(), body.bottom() - 2);
                start = canvas.mapFromScene(node->mapToScene(point));
            }
            const auto target = start + QPoint(64, 48);
            sendMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            sendMouse(canvas, QEvent::MouseMove, start + QPoint(24, 18), Qt::NoButton, Qt::LeftButton);
            sendMouse(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
            sendMouse(canvas, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
            drainEvents();
            EXPECT_EQ(nodeFor()->pos(), original + QPointF(64, 48));
            EXPECT_EQ(PlayablePropertyAuthoringV1::definitionToJson(*(object ? object : c1)), before);
            auto* currentEditor = qobject_cast<NodeContentEditor*>(nodeFor()->contentWidget());
            EXPECT_FALSE(currentEditor->isEditing());
            EXPECT_DOUBLE_EQ(currentEditor->binding(field)->value(), originalValue);
            ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
            ASSERT_TRUE(UndoRedoManager::instance().undo());
            drainEvents();
            EXPECT_EQ(nodeFor()->pos(), original);
            UndoRedoManager::instance().clear();
        }
    }
}

TEST_F(SignalPathObservationTest, SliderSensitiveAreaAdjustsOrdinaryAndGainValuesWithoutDraggingNodes)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1200, 800);
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
    canvas.setGridSnapEnabled(false);
    canvas.setZoomFactor(1.0);
    canvas.show();
    drainEvents();
    for (auto* object : std::array<ProjectItem*, 4>{s1, c1, b1, nullptr}) {
        SCOPED_TRACE(object ? object->name.toStdString() : "Send gain");
        const auto nodeFor = [&]() {
            const auto& graph = canvas.activeSignalPathSource()->graph();
            return object ? canvas.signalPathNode(indexOf(graph, object))
                : canvas.signalPathGainNode(routingEdge(graph, SignalPathEdgeKind::Send, c1, b2));
        };
        auto* node = nodeFor();
        auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget());
        auto* host = editor->findChild<ParameterControlHost*>(object ? "nodeSlider_volume" : "nodeSlider_sendLevel");
        auto* slider = host->findChild<QSlider*>();
        ASSERT_NE(slider, nullptr);
        const auto field = object ? NodeContentField::Volume : NodeContentField::SendVolume;
        const auto originalValue = editor->binding(field)->value();
        const auto original = node->pos();
        canvas.centerOn(node);
        const auto point = [&](int x) {
            return canvas.mapFromScene(editor->graphicsProxyWidget()->mapToScene(
                slider->mapTo(editor, QPoint(x, slider->height() / 2))));
        };
        sendMouse(canvas, QEvent::MouseButtonPress, point(slider->width() / 2), Qt::LeftButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseMove, point(slider->width() / 3), Qt::NoButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, point(slider->width() / 3), Qt::LeftButton, Qt::NoButton);
        drainEvents();
        EXPECT_EQ(nodeFor()->pos(), original);
        EXPECT_NE(qobject_cast<NodeContentEditor*>(nodeFor()->contentWidget())->binding(field)->value(), originalValue);
        ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        drainEvents();
        EXPECT_DOUBLE_EQ(qobject_cast<NodeContentEditor*>(nodeFor()->contentWidget())->binding(field)->value(), originalValue);
        UndoRedoManager::instance().clear();
    }
}

TEST_F(SignalPathObservationTest, EditingNumericTextKeepsMouseSelectionAndDoesNotDragNodes)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1200, 800);
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
    canvas.setZoomFactor(1.0);
    canvas.show();
    drainEvents();
    for (const bool gain : {false, true}) {
        const auto& graph = canvas.activeSignalPathSource()->graph();
        auto* node = gain ? canvas.signalPathGainNode(routingEdge(graph, SignalPathEdgeKind::Send, c1, b2))
            : canvas.signalPathNode(indexOf(graph, s1));
        auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget());
        auto* label = editor->findChild<QLabel*>(gain ? "nodeValue_sendLevel" : "nodeValue_volume");
        auto* input = editor->findChild<QDoubleSpinBox*>(gain ? "nodeEditor_sendLevel" : "nodeEditor_volume");
        ASSERT_NE(label, nullptr);
        ASSERT_NE(input, nullptr);
        const auto original = node->pos();
        canvas.centerOn(node);
        const auto point = [&](QWidget* widget, QPoint position) {
            return canvas.mapFromScene(editor->graphicsProxyWidget()->mapToScene(widget->mapTo(editor, position)));
        };
        const auto valuePoint = point(label, label->rect().center());
        sendMouse(canvas, QEvent::MouseButtonPress, valuePoint, Qt::LeftButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, valuePoint, Qt::LeftButton, Qt::NoButton);
        sendMouse(canvas, QEvent::MouseButtonDblClick, valuePoint, Qt::LeftButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, valuePoint, Qt::LeftButton, Qt::NoButton);
        ASSERT_TRUE(input->isVisible());
        auto* text = input->findChild<QLineEdit*>();
        ASSERT_NE(text, nullptr);
        const auto start = point(text, QPoint(2, text->height() / 2));
        const auto end = point(text, QPoint(text->width() - 2, text->height() / 2));
        sendMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        EXPECT_FALSE(text->selectedText().isEmpty());
        EXPECT_EQ(node->pos(), original);
        EXPECT_TRUE(editor->isEditing());
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(input, &escape);
        drainEvents();
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    }
}

TEST_F(SignalPathObservationTest, LongValuesExpandOnlyTheirNodeAndKeepConnectionsAttached)
{
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.setFlowDirection(CanvasFlowDirection::LR);
    canvas.resize(1200, 750);
    canvas.show();
    canvas.focusItem(s1);
    drainEvents();
    const auto nodeFor = [&](ProjectItem* item) {
        return canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), item));
    };
    const auto compactBounds = nodeFor(s1)->boundingRect();
    const auto originalPosition = nodeFor(s1)->pos();
    const auto originalPin = nodeFor(s1)->outputPin()->scenePos();
    const auto otherBounds = nodeFor(s2)->boundingRect();
    const auto labelWidth = qobject_cast<QGridLayout*>(editorFor(canvas, s1)->layout())
        ->itemAtPosition(0, 0)->widget()->width();
    const auto expectConnectedEndpoints = [&]() {
        const auto& edges = canvas.activeSignalPathSource()->graph().edges;
        for (size_t index = 0; index < edges.size(); ++index) {
            const auto* connection = canvas.signalPathEdge(index);
            EXPECT_EQ(connection->path().pointAtPercent(0), connection->startPin()->scenePos());
            EXPECT_EQ(connection->path().pointAtPercent(1), connection->endPin()->scenePos());
        }
    };
    PropertyEditSession session(s1, QStringLiteral("loopCount"), 1);
    session.preview(1000000000);
    drainEvents();
    auto* editor = editorFor(canvas, s1);
    auto* valueLabel = editor->findChild<QLabel*>("nodeValue_loopCount");
    ASSERT_NE(valueLabel, nullptr);
    const auto expandedWidth = valueLabel->parentWidget()->width();
    EXPECT_GT(expandedWidth, NodeContentEditor::kValueWidth);
    EXPECT_GE(expandedWidth, valueLabel->sizeHint().width());
    auto* grid = qobject_cast<QGridLayout*>(editor->layout());
    for (int row = 0; row < grid->rowCount(); ++row) {
        EXPECT_EQ(grid->itemAtPosition(row, 0)->widget()->width(), labelWidth);
        EXPECT_EQ(grid->itemAtPosition(row, 2)->widget()->width(), expandedWidth);
    }
    for (auto* slider : editor->findChildren<ParameterControlHost*>())
        EXPECT_EQ(slider->width(), NodeContentEditor::kSliderWidth);
    EXPECT_EQ(nodeFor(s1)->boundingRect().width() - compactBounds.width(), expandedWidth - NodeContentEditor::kValueWidth);
    EXPECT_EQ(nodeFor(s1)->boundingRect().height(), compactBounds.height());
    EXPECT_EQ(nodeFor(s1)->pos(), originalPosition);
    EXPECT_NE(nodeFor(s1)->outputPin()->scenePos(), originalPin);
    EXPECT_EQ(nodeFor(s2)->boundingRect(), otherBounds);
    EXPECT_EQ(editorFor(canvas, s2)->findChild<QLabel*>("nodeValue_scheduleOffsetSec")->width(), NodeContentEditor::kValueWidth);
    expectContentFitsProxy(editor);
    expectConnectedEndpoints();
    session.commit();
    drainEvents();
    EXPECT_DOUBLE_EQ(value(s1, "loopCount"), 1000000000.0);
    EXPECT_GT(nodeFor(s1)->boundingRect().width(), compactBounds.width());
    EXPECT_EQ(nodeFor(s2)->boundingRect(), otherBounds);
    expectConnectedEndpoints();
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_EQ(nodeFor(s1)->boundingRect(), compactBounds);
    EXPECT_EQ(nodeFor(s2)->boundingRect(), otherBounds);
    EXPECT_EQ(nodeFor(s1)->outputPin()->scenePos(), originalPin);
    expectContentFitsProxy(editorFor(canvas, s1));
    expectConnectedEndpoints();
}

TEST_F(SignalPathObservationTest, OrdinaryNumbersEnterEditWithoutResizingOrClippingTheValueColumn)
{
    const QString previousStyle = qApp->styleSheet();
    const auto restore = qScopeGuard([&]() { qApp->setStyleSheet(previousStyle); });
    qApp->setStyleSheet(QtControlSkin::current().applicationStyleSheet());
    master->comp<VMixNode>()->volume = -15.1f;
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.resize(1200, 750);
    canvas.show();
    canvas.focusItem(master);
    drainEvents();
    auto* node = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), master));
    auto* editor = editorFor(canvas, master);
    auto* label = editor->findChild<QLabel*>("nodeValue_volume");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->width(), NodeContentEditor::kValueWidth);
    const auto size = editor->size();
    const auto nodeBounds = node->boundingRect();
    const auto point = canvas.mapFromScene(editor->graphicsProxyWidget()->mapToScene(
        label->mapTo(editor, label->rect().center())));
    sendMouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    sendMouse(canvas, QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    drainEvents();
    auto* input = editor->findChild<QDoubleSpinBox*>("nodeEditor_volume");
    ASSERT_TRUE(input->isVisible());
    EXPECT_EQ(editor->size(), size);
    EXPECT_EQ(node->boundingRect(), nodeBounds);
    expectContentFitsProxy(editor);
    auto* text = input->findChild<QLineEdit*>();
    QStyleOptionFrame option;
    option.initFrom(text);
    const auto contents = text->style()->subElementRect(QStyle::SE_LineEditContents, &option, text);
    EXPECT_GE(contents.width(), text->fontMetrics().horizontalAdvance(text->text()) + 2);
    EXPECT_GE(contents.height(), text->fontMetrics().height());
    editor->finishEditing(false);
    drainEvents();
    EXPECT_EQ(editorFor(canvas, master)->size(), size);
    expectContentFitsProxy(editorFor(canvas, master));
}

TEST_F(SignalPathObservationTest, NumericEditTextExpandsCurrentNodeBeforeCommitAndCancelRestoresWidth)
{
    const QString previousStyle = qApp->styleSheet();
    const auto restore = qScopeGuard([&]() { qApp->setStyleSheet(previousStyle); });
    qApp->setStyleSheet(QtControlSkin::current().applicationStyleSheet());
    BlueprintCanvasWidget canvas;
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.setFlowDirection(CanvasFlowDirection::LR);
    canvas.resize(1200, 750);
    canvas.show();
    canvas.focusItem(s1);
    drainEvents();
    auto* node = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s1));
    auto* otherNode = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s2));
    const auto compactBounds = node->boundingRect();
    const auto otherBounds = otherNode->boundingRect();
    auto* editor = editorFor(canvas, s1);
    auto* label = editor->findChild<QLabel*>("nodeValue_scheduleOffsetSec");
    auto* input = editor->findChild<QDoubleSpinBox*>("nodeEditor_scheduleOffsetSec");
    ASSERT_NE(input, nullptr);
    const auto point = canvas.mapFromScene(editor->graphicsProxyWidget()->mapToScene(
        label->mapTo(editor, label->rect().center())));
    sendMouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    sendMouse(canvas, QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    ASSERT_TRUE(input->isVisible());
    auto* text = input->findChild<QLineEdit*>();
    ASSERT_NE(text, nullptr);
    text->setText(QStringLiteral("00099.125"));
    drainEvents();
    EXPECT_DOUBLE_EQ(editor->binding(NodeContentField::OffsetOfStart)->value(), 0.0);
    EXPECT_DOUBLE_EQ(s1->comp<VSound>()->scheduleOffsetSec, 0.0);
    EXPECT_GT(input->parentWidget()->width(), NodeContentEditor::kValueWidth);
    expectContentFitsProxy(editor);
    QStyleOptionFrame textOption;
    textOption.initFrom(text);
    textOption.rect = text->rect();
    const auto textBounds = text->style()->subElementRect(QStyle::SE_LineEditContents, &textOption, text);
    EXPECT_GE(textBounds.width(), text->fontMetrics().horizontalAdvance(text->text()));
    EXPECT_GE(textBounds.height(), text->fontMetrics().height());
    EXPECT_GT(node->boundingRect().width(), compactBounds.width());
    EXPECT_EQ(node->boundingRect().height(), compactBounds.height());
    EXPECT_EQ(otherNode->boundingRect(), otherBounds);
    auto* grid = qobject_cast<QGridLayout*>(editor->layout());
    for (int row = 0; row < grid->rowCount(); ++row)
        EXPECT_EQ(grid->itemAtPosition(row, 2)->widget()->width(), input->parentWidget()->width());
    const auto& edges = canvas.activeSignalPathSource()->graph().edges;
    for (size_t index = 0; index < edges.size(); ++index) {
        const auto* connection = canvas.signalPathEdge(index);
        EXPECT_EQ(connection->path().pointAtPercent(0), connection->startPin()->scenePos());
        EXPECT_EQ(connection->path().pointAtPercent(1), connection->endPin()->scenePos());
    }
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(input, &escape);
    drainEvents();
    EXPECT_FALSE(editorFor(canvas, s1)->isEditing());
    expectContentFitsProxy(editorFor(canvas, s1));
    EXPECT_EQ(canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s1))->boundingRect(), compactBounds);
    EXPECT_EQ(canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s2))->boundingRect(), otherBounds);
    EXPECT_DOUBLE_EQ(s1->comp<VSound>()->scheduleOffsetSec, 0.0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathObservationTest, PinDraggingAndCanvasConnectionApiEditObjectRoutes)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1400, 900);
    canvas.show();
    canvas.zoomToFit();
    drainEvents();
    const auto graph = canvas.activeSignalPathSource()->graph();
    const auto original = s1->fieldGroups;
    const auto points = [&](ProjectItem* bus) {
        const auto from = path(canvas.activeSignalPathSource()->graph(), s1).computation.front();
        auto* source = canvas.signalPathNode(from.occurrenceIndex)->outputPin(static_cast<int>(from.portPairIndex));
        auto* target = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), bus))->inputPin();
        return std::pair{canvas.mapFromScene(source->scenePos()), canvas.mapFromScene(target->scenePos())};
    };
    for (const auto modifiers : {Qt::NoModifier, Qt::AltModifier}) {
        const auto [start, end] = points(b3);
        sendMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, modifiers);
        sendMouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton, modifiers);
        EXPECT_EQ(canvas.signalPathNode(indexOf(graph, b3))->inputPin()->connectState(), NodePin::ConnectState::Ok);
        sendMouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton, modifiers);
        drainEvents();
        EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s1).routing.outputBusID,
            modifiers == Qt::AltModifier ? b1->objectID : b3->objectID);
        EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s1).routing.sends.size(),
            modifiers == Qt::AltModifier ? 1u : 0u);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        drainEvents();
        EXPECT_EQ(s1->fieldGroups, original);
        UndoRedoManager::instance().clear();
    }
    EXPECT_TRUE(canvas.connectSignalPathNodes({indexOf(graph, b1), 0}, {indexOf(graph, b2), 0}).success);
    EXPECT_EQ(b1->parent, b2);
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s1).routing.outputBusID, b1->objectID);
}

TEST_F(SignalPathObservationTest, DeleteSendEditsItsProviderAndPreservesParallelLocalConfiguration)
{
    PlatformContextService::instance().setAll(true);
    auto* another = create("AnotherInheritedSound", T::Sound, c1);
    s5->fieldGroups.send.shared = {FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(b2->guid), -6.0f}}}};
    const auto providerBefore = c1->fieldGroups;
    const auto firstBefore = s4->fieldGroups;
    const auto anotherBefore = another->fieldGroups;
    const auto localBefore = s5->fieldGroups;
    UndoRedoManager::instance().clear();
    BlueprintCanvasWidget canvas;
    const auto graph = canvas.activeSignalPathSource()->graph();
    const auto inheritedEdge = routingEdge(graph, SignalPathEdgeKind::Send, c1, b2);
    const auto localEdge = routingEdge(graph, SignalPathEdgeKind::Send, s5, b2);
    ASSERT_NE(inheritedEdge, localEdge);
    EXPECT_EQ(graph.edges[inheritedEdge].from, graph.edges[localEdge].from);
    EXPECT_EQ(graph.edges[inheritedEdge].to, graph.edges[localEdge].to);
    EXPECT_FLOAT_EQ(graph.edges[inheritedEdge].sendLevel, graph.edges[localEdge].sendLevel);
    canvas.signalPathEdge(inheritedEdge)->setSelected(true);
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &remove);
    drainEvents();
    const auto updated = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(updated.ok());
    EXPECT_TRUE(path(updated, s4).routing.sends.empty());
    EXPECT_TRUE(path(updated, another).routing.sends.empty());
    ASSERT_EQ(path(updated, s5).routing.sends.size(), 1u);
    EXPECT_EQ(path(updated, s5).routing.sends.front().targetBusID, b2->objectID);
    EXPECT_EQ(s4->fieldGroups, firstBefore);
    EXPECT_EQ(another->fieldGroups, anotherBefore);
    EXPECT_EQ(s5->fieldGroups, localBefore);
    EXPECT_TRUE(c1->fieldGroups.send.shared.localHistory->sends.empty());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_EQ(c1->fieldGroups, providerBefore);
    EXPECT_EQ(s4->fieldGroups, firstBefore);
    EXPECT_EQ(another->fieldGroups, anotherBefore);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainEvents();
    EXPECT_TRUE(path(canvas.activeSignalPathSource()->graph(), s4).routing.sends.empty());
    EXPECT_EQ(s5->fieldGroups, localBefore);
}

TEST_F(SignalPathObservationTest, DeleteOutputReturnsOnlyItsProviderToMasterAndProtectsTheResult)
{
    PlatformContextService::instance().setAll(true);
    p1->fieldGroups.mainOutput.shared.localHistory->outputBusVolume = -9.0f;
    const auto providerBefore = p1->fieldGroups;
    const auto firstBefore = s1->fieldGroups;
    const auto containerBefore = c1->fieldGroups;
    const auto layerBefore = l2->fieldGroups;
    BlueprintCanvasWidget canvas;
    auto graph = canvas.activeSignalPathSource()->graph();
    const auto result = canvas.disconnectSignalPathEdges(
        {routingEdge(graph, SignalPathEdgeKind::Output, p1, b1)});
    ASSERT_TRUE(result.success) << result.error.toStdString();
    drainEvents();
    graph = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(graph.ok());
    for (auto* sound : {s1, s2, s3}) {
        EXPECT_EQ(path(graph, sound).routing.outputBusID, master->objectID);
        EXPECT_FALSE(canvas.signalPathNode(indexOf(graph, sound))->contentInfo().outputVolume.has_value());
    }
    EXPECT_DOUBLE_EQ(gainEditorFor(canvas, SignalPathEdgeKind::Output, p1, master)
        ->binding(NodeContentField::OutputVolume)->value(), -9.0);
    EXPECT_EQ(path(graph, s4).routing.outputBusID, b1->objectID);
    EXPECT_EQ(path(graph, s6).routing.outputBusID, b1->objectID);
    EXPECT_EQ(p1->parent, l1);
    EXPECT_EQ(s1->fieldGroups, firstBefore);
    EXPECT_EQ(c1->fieldGroups, containerBefore);
    EXPECT_EQ(l2->fieldGroups, layerBefore);
    EXPECT_EQ(p1->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef,
              toFieldGroupGuid(master->guid));
    EXPECT_FLOAT_EQ(p1->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -9.0f);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    const auto masterEdge = routingEdge(graph, SignalPathEdgeKind::Output, p1, master);
    canvas.signalPathEdge(masterEdge)->setSelected(true);
    EXPECT_TRUE(canvas.signalPathEdge(masterEdge)->isSelected());
    EXPECT_FALSE(canvas.disconnectSignalPathEdges({masterEdge}).success);
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &remove);
    drainEvents();
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s1).routing.outputBusID, master->objectID);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_EQ(p1->fieldGroups, providerBefore);
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s1).routing.outputBusID, b1->objectID);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainEvents();
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s1).routing.outputBusID, master->objectID);
}

TEST_F(SignalPathObservationTest, DeleteSharedThroughMovesToRootAndRemovesAllClipsInOneUndo)
{
    place(t1, s1);
    const auto beforeClips = t1->comp<VTrack>()->clips;
    const auto beforeGroups = s1->fieldGroups;
    BlueprintCanvasWidget canvas;
    const auto& graph = canvas.activeSignalPathSource()->graph();
    const auto through = hierarchyEdge(graph, s1, t1);
    ASSERT_EQ(graph.edges[through].pathIndices.size(), 2u);
    canvas.signalPathEdge(through)->setSelected(true);
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &remove);
    drainEvents();
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_NE(ProjectManager::findRootBinding(ProjectManager::instance().currentProject(), s1), nullptr);
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s1).routing.outputBusID, b1->objectID);
    auto remainingClips = beforeClips;
    std::erase_if(remainingClips.items, [&](const auto& clip) { return clip.targetGuid == s1->guid; });
    EXPECT_EQ(t1->comp<VTrack>()->clips, remainingClips);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_TRUE(canvas.scene()->selectedItems().empty());
    QKeyEvent unselectedRemove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &unselectedRemove);
    drainEvents();
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_EQ(s1->parent, t1);
    EXPECT_EQ(s1->fieldGroups, beforeGroups);
    EXPECT_EQ(t1->comp<VTrack>()->clips, beforeClips);
    const auto& restored = canvas.activeSignalPathSource()->graph();
    EXPECT_EQ(restored.edges[hierarchyEdge(restored, s1, t1)].pathIndices.size(), 2u);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainEvents();
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(t1->comp<VTrack>()->clips, remainingClips);
}

TEST_F(SignalPathObservationTest, BusOutputConnectionReparentsAndSupportsUndoRedo)
{
    BlueprintCanvasWidget canvas;
    const auto graph = canvas.activeSignalPathSource()->graph();
    const auto result = connectPath(graph, {indexOf(graph, b1), 0}, {indexOf(graph, b2), 0});
    ASSERT_TRUE(result.success) << result.error.toStdString();
    drainEvents();
    EXPECT_EQ(b1->parent, b2);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_EQ(b1->parent, master);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainEvents();
    EXPECT_EQ(b1->parent, b2);
    EXPECT_TRUE(canvas.activeSignalPathSource()->graph().ok());
}

TEST_F(SignalPathObservationTest, ObjectOutputChangesInheritingChildrenAndPreservesVolume)
{
    p1->fieldGroups.mainOutput.shared.localHistory->outputBusVolume = -9.0f;
    BlueprintCanvasWidget canvas;
    const auto layerGroups = l3->fieldGroups;
    const auto soundGroups = s1->fieldGroups;
    const auto fullGraph = canvas.activeSignalPathSource()->graph();
    const auto result = connectPath(fullGraph, {indexOf(fullGraph, p1), 0}, {indexOf(fullGraph, b3), 0});
    ASSERT_TRUE(result.success) << result.error.toStdString();
    drainEvents();
    const auto next = canvas.activeSignalPathSource()->graph();
    for (auto* sound : {s1, s2, s3}) {
        const auto& route = path(next, sound);
        EXPECT_EQ(route.routing.outputBusID, b3->objectID);
        EXPECT_FALSE(canvas.signalPathNode(indexOf(next, sound))->contentInfo().outputVolume.has_value());
        const auto output = std::find_if(route.edgeIndices.begin(), route.edgeIndices.end(), [&](size_t index) {
            return next.edges[index].kind == SignalPathEdgeKind::Output
                && next.edges[index].from == route.computation.back();
        });
        ASSERT_NE(output, route.edgeIndices.end());
        EXPECT_EQ(canvas.signalPathGainNode(*output)->contentInfo().outputVolume, -9.0f);
    }
    EXPECT_EQ(path(next, s4).routing.outputBusID, b1->objectID);
    EXPECT_EQ(l3->fieldGroups, layerGroups);
    EXPECT_EQ(path(buildProjectSignalPathGraph(*ProjectManager::instance().currentProject(),
        HarkenTest::FieldGroupPlatformIds::Android), s1).routing.outputBusID, b1->objectID);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_EQ(s1->fieldGroups, soundGroups);
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s1).routing.outputBusID, b1->objectID);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainEvents();
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s2).routing.outputBusID, b3->objectID);
}

TEST_F(SignalPathObservationTest, PathSendsPreserveInheritedSendsAndRejectConflictingOutputs)
{
    BlueprintCanvasWidget canvas;
    auto graph = canvas.activeSignalPathSource()->graph();
    const auto original = s4->fieldGroups;
    const auto result = connectPath(graph, path(graph, s4).computation.front(),
        {indexOf(graph, b3), 0}, true);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    drainEvents();
    graph = canvas.activeSignalPathSource()->graph();
    const auto& sends = path(graph, s4).routing.sends;
    ASSERT_EQ(sends.size(), 2u);
    EXPECT_NEAR(sends.front().sendLevel, VMixNode::dbToLinear(-6.0f), 0.0001f);
    EXPECT_FLOAT_EQ(sends.back().sendLevel, 1.0f);
    const auto endpoint = path(graph, s4).computation.front();
    EXPECT_FALSE(connectPath(graph, endpoint, {indexOf(graph, b3), 0}, true).success);
    EXPECT_FALSE(connectPath(graph, endpoint, {indexOf(graph, b3), 0}).success);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_EQ(s4->fieldGroups, original);
}

TEST_F(SignalPathObservationTest, ContainerConnectionPreservesInheritanceAndRestoresTrackPlacementOnUndo)
{
    output(s5, b1);
    BlueprintCanvasWidget canvas;
    const auto graph = canvas.activeSignalPathSource()->graph();
    const auto groups = s1->fieldGroups;
    const auto clip = t1->comp<VTrack>()->clips.items.front().clipGuid;
    const auto locator = PlayablePropertyAuthoringV1::trackClipLocator(*t1, clip);
    const auto result = connectPath(graph, path(graph, s1).computation.front(),
        path(graph, s4).computation[1]);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    drainEvents();
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(s1->fieldGroups, groups);
    EXPECT_FALSE(PlayablePropertyAuthoringV1::trackClipLocator(*t1, clip));
    EXPECT_TRUE(PlayablePropertyAuthoringV1::containerChildLocator(*c1, s1->guid));
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s1).routing,
              path(graph, s4).routing);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_EQ(s1->parent, t1);
    EXPECT_EQ(s1->fieldGroups, groups);
    EXPECT_EQ(PlayablePropertyAuthoringV1::trackClipLocator(*t1, clip), locator);
    EXPECT_FALSE(PlayablePropertyAuthoringV1::containerChildLocator(*c1, s1->guid));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainEvents();
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s1).routing, path(graph, s4).routing);
}

TEST_F(SignalPathObservationTest, MixedContainerRoutesDoNotPreventParentChanges)
{
    BlueprintCanvasWidget canvas;
    const auto graph = canvas.activeSignalPathSource()->graph();
    const auto other = s4->fieldGroups;
    const auto original = s1->fieldGroups;
    const auto result = connectPath(graph, path(graph, s1).computation.front(),
        path(graph, s5).computation[1]);
    EXPECT_TRUE(result.success) << result.error.toStdString();
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(s1->fieldGroups, original);
    EXPECT_EQ(s4->fieldGroups, other);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(SignalPathObservationTest, ConnectionsRejectInvalidTypesCyclesAndStalePathsWithoutUndoEntries)
{
    BlueprintCanvasWidget canvas;
    auto graph = canvas.activeSignalPathSource()->graph();
    const auto sound = path(graph, s1).computation.front();
    const auto container = path(graph, s4).computation[1];
    EXPECT_FALSE(connectPath(graph, container, sound).success);
    EXPECT_FALSE(connectPath(graph, sound, container, true).success);
    EXPECT_FALSE(connectPath(graph, path(graph, s4).computation.back(), container).success);
    EXPECT_FALSE(connectPath(graph, {indexOf(graph, master), 0}, {indexOf(graph, b1), 0}).success);
    output(s1, b3);
    EXPECT_FALSE(prepareSignalPathConnect(graph, sound, container, false).ok());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    canvas.rebuildFromProject();
    graph = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(connectPath(graph, {indexOf(graph, b1), 0}, {indexOf(graph, b2), 0}, true).success);
    drainEvents();
    graph = canvas.activeSignalPathSource()->graph();
    EXPECT_FALSE(connectPath(graph, {indexOf(graph, b2), 0}, {indexOf(graph, b1), 0}).success);
    EXPECT_FALSE(connectPath(graph, {indexOf(graph, b2), 0}, {indexOf(graph, b1), 0}, true).success);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(SignalPathObservationTest, SharedRouteValidationRejectsAnyPlatformConflictAtomically)
{
    s2->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, SendFieldGroup{{{toFieldGroupGuid(b3->guid), -3.0f}}}};
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android},
        HarkenTest::PlatformIds::Windows, true);
    BlueprintCanvasWidget canvas;
    const auto graph = canvas.activeSignalPathSource()->graph();
    const auto first = s1->fieldGroups;
    const auto second = s2->fieldGroups;
    EXPECT_FALSE(connectPath(graph, {indexOf(graph, p1), 0},
        {indexOf(graph, b3), 0}).success);
    EXPECT_EQ(s1->fieldGroups, first);
    EXPECT_EQ(s2->fieldGroups, second);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathObservationTest, ProjectViewBuildsTheCompletePrototypeWithoutSelection)
{
    BlueprintCanvasWidget canvas;
    ASSERT_NE(canvas.activeSignalPathSource(), nullptr);
    EXPECT_EQ(canvas.activeSignalChainSource(), nullptr);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(graph.paths.size(), 6u);
    EXPECT_EQ(graph.nodes.size(), 18u);
    EXPECT_EQ(count(graph, l1), 1u);
    EXPECT_EQ(count(graph, l3), 1u);
    ASSERT_EQ(count(graph, c1), 1u);
    EXPECT_EQ(graph.nodes[indexOf(graph, c1)].portPairs.size(), 1u);
    const std::array<QStringList, 6> expected{{
        {"S1", "T1", "P1", "L1", "L3"}, {"S2", "T1", "P1", "L1", "L3"},
        {"S3", "T2", "P1", "L1", "L3"}, {"S4", "C1", "L1", "L3"},
        {"S5", "C1", "L1", "L3"}, {"S6", "T3", "L2", "L3"}
    }};
    for (size_t index = 0; index < graph.paths.size(); ++index) {
        QStringList names;
        for (const auto endpoint : graph.paths[index].computation)
            names.push_back(graph.nodes[endpoint.occurrenceIndex].name);
        EXPECT_EQ(names, expected[index]);
        EXPECT_FALSE(graph.paths[index].identity.occurrences.empty());
    }
    EXPECT_EQ(path(graph, s5).routing.outputBusID, b3->objectID);
    EXPECT_EQ(path(graph, s5).routing.sends.front().targetBusID, b2->objectID);
    EXPECT_TRUE(canvas.transitionWatch().containerGuid.isNull());
    EXPECT_FALSE(canvas.connectNodes(s1->objectID, b2->objectID).success);
}

TEST_F(SignalPathObservationTest, TrackScopeRoutesUnplacedChildrenWithoutInventingClipOccurrences)
{
    auto* unplaced = create("Unplaced", T::Sound, t1);
    t1->comp<VTrack>()->clips.items.pop_back();
    const auto repeated = place(t1, s1);
    const auto graph = build(t1);
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(graph.paths.size(), 4u);
    EXPECT_EQ(count(graph, s1), 1u);
    EXPECT_EQ(count(graph, unplaced), 1u);
    EXPECT_TRUE(path(graph, unplaced).identity.occurrences.empty());
    expectRoute(graph, unplaced, {"Unplaced", "T1", "P1", "L1", "L3"}, b1);
    EXPECT_EQ(count(graph, t1), 1u);
    EXPECT_NE(graph.paths.front().identity, graph.paths.back().identity);
    const auto focused = build(s1, {repeated});
    ASSERT_EQ(focused.paths.size(), 1u);
    EXPECT_EQ(focused.paths.front().identity.occurrences, (std::vector{repeated}));
    const auto allUses = build(s1);
    EXPECT_EQ(allUses.paths.size(), 2u);
    EXPECT_EQ(build(unplaced).paths.size(), 1u);
}

TEST_F(SignalPathObservationTest, ProjectViewRoutesUnplacedChildrenThroughTheirHierarchy)
{
    auto* unplaced = create("Unplaced", T::Sound, t1);
    t1->comp<VTrack>()->clips.items.pop_back();
    auto* unplacedContainer = create("UnplacedContainer", T::Container, t1);
    t1->comp<VTrack>()->clips.items.pop_back();
    auto* nested = create("Nested", T::Sound, unplacedContainer);
    BlueprintCanvasWidget canvas;
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(graph.ok());
    EXPECT_EQ(graph.paths.size(), 8u);
    for (auto* item : {unplaced, unplacedContainer, nested}) {
        EXPECT_EQ(count(graph, item), 1u);
        EXPECT_FALSE(graph.nodes[indexOf(graph, item)].portPairs.front().pathIndices.empty());
    }
    EXPECT_TRUE(path(graph, unplaced).identity.occurrences.empty());
    const auto& uses = path(graph, nested).identity.occurrences;
    ASSERT_EQ(uses.size(), 1u);
    EXPECT_EQ(uses.front().ownerGuid, unplacedContainer->guid);
    expectRoute(graph, unplaced, {"Unplaced", "T1", "P1", "L1", "L3"}, b1);
    expectRoute(graph, nested, {"Nested", "UnplacedContainer", "T1", "P1", "L1", "L3"}, b1);
}

TEST_F(SignalPathObservationTest, EmptyContentRoutesThroughLayersUsingItsOwnEffectiveOutputAndSends)
{
    auto* layer = create("Root", T::Layer);
    BlueprintCanvasWidget canvas;
    expectRoute(canvas.activeSignalPathSource()->graph(), layer, {"Root"}, master);
    output(layer, b1);
    layer->fieldGroups.send.shared = {FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(b2->guid), -6.0f}}}};
    const auto layerConfig = layer->fieldGroups;
    const std::vector<std::pair<const char*, T>> types{
        {"Sound", T::Sound}, {"Container", T::Container}, {"Track", T::Track},
        {"Piece", T::Piece}, {"Layer", T::Layer}};
    for (const auto& [name, type] : types) {
        auto* child = create(name, type, layer);
        const auto config = child->fieldGroups;
        drainEvents();
        expectRoute(canvas.activeSignalPathSource()->graph(), child,
                    {QString::fromLatin1(name), "Root"}, b1, {b2});
        EXPECT_EQ(child->fieldGroups, config);
        EXPECT_EQ(layer->fieldGroups, layerConfig);
        if (const auto* track = child->comp<VTrack>()) EXPECT_TRUE(track->clips.items.empty());
        output(child, b3);
        emit ProjectManager::instance().itemPropertyChanged(child, QStringLiteral("mainOutput"));
        drainEvents();
        const auto& graph = canvas.activeSignalPathSource()->graph();
        expectRoute(graph, child, {QString::fromLatin1(name), "Root"}, b3, {b2});
        EXPECT_TRUE(std::none_of(graph.paths.begin(), graph.paths.end(), [&](const auto& route) {
            return route.identity.sourceGuid == layer->guid;
        }));
    }
}

TEST_F(SignalPathObservationTest, NewProjectShowsMasterBusCenteredAtFullScale)
{
    auto& pm = ProjectManager::instance();
    pm.closeProject();
    QTemporaryDir freshDirectory;
    BlueprintCanvasWidget canvas;
    canvas.resize(900, 650);
    canvas.show();
    drainEvents();
    ProjectConfig config;
    config.name = QStringLiteral("FreshBlueprint");
    config.location = freshDirectory.path();
    ASSERT_TRUE(pm.createProject(config));
    drainEvents();
    auto* freshMaster = pm.getMasterBusItem();
    ASSERT_NE(freshMaster, nullptr);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(graph.nodes.size(), 1u);
    EXPECT_EQ(graph.nodes.front().key.objectGuid, freshMaster->guid);
    EXPECT_TRUE(graph.paths.empty());
    auto* node = canvas.signalPathNode(0);
    ASSERT_NE(node, nullptr);
    EXPECT_FALSE(node->isSelected());
    for (const QSize size : {QSize(900, 650), QSize(1500, 900)}) {
        canvas.resize(size);
        drainEvents();
        EXPECT_DOUBLE_EQ(canvas.transform().m11(), 1.0);
        EXPECT_LT(QLineF(canvas.mapToScene(canvas.viewport()->rect().center()),
                        node->sceneBoundingRect().center()).length(), 2.0);
    }
    BlueprintCanvasWidget reopened;
    reopened.resize(1100, 750);
    reopened.show();
    drainEvents();
    ASSERT_EQ(reopened.activeSignalPathSource()->graph().nodes.size(), 1u);
    EXPECT_DOUBLE_EQ(reopened.transform().m11(), 1.0);
    EXPECT_LT(QLineF(reopened.mapToScene(reopened.viewport()->rect().center()),
                    reopened.signalPathNode(0)->sceneBoundingRect().center()).length(), 2.0);
    pm.closeProject();
}

TEST_F(SignalPathObservationTest, NestedContainerAndClipOccurrencesRetainTheirIdentity)
{
    auto* outer = create("Outer", T::Container, l2);
    auto* track = create("NestedTrack", T::Track, outer);
    auto* inner = create("Inner", T::Container, track);
    auto* sound = create("NestedSound", T::Sound, inner);
    track->comp<VTrack>()->clips = {};
    const auto clip1 = place(track, inner);
    place(track, inner);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*outer, track->guid));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*inner, sound->guid));
    const auto graph = build(outer);
    ASSERT_TRUE(graph.ok());
    ASSERT_EQ(graph.paths.size(), 8u);
    for (const auto& entry : graph.paths) {
        ASSERT_EQ(entry.identity.occurrences.size(), 3u);
        EXPECT_EQ(entry.identity.occurrences[0].ownerGuid, inner->guid);
        EXPECT_EQ(entry.identity.occurrences[1].ownerGuid, track->guid);
        EXPECT_EQ(entry.identity.occurrences[2].ownerGuid, outer->guid);
    }
    const auto focused = build(inner, {clip1});
    EXPECT_EQ(focused.paths.size(), 4u);
}

TEST_F(SignalPathObservationTest, InvalidClipsCannotConnectUnrelatedObjectsOrInventOccurrences)
{
    auto* unrelated = create("Unrelated", T::Sound, l2);
    auto& clips = t1->comp<VTrack>()->clips.items;
    clips.front().targetGuid = unrelated->guid;
    clips.back().targetGuid = QUuid::createUuid();
    const auto graph = build(t1);
    EXPECT_TRUE(graph.ok());
    EXPECT_EQ(graph.paths.size(), 2u);
    EXPECT_TRUE(path(graph, s1).identity.occurrences.empty());
    EXPECT_TRUE(path(graph, s2).identity.occurrences.empty());
    expectRoute(graph, s1, {"S1", "T1", "P1", "L1", "L3"}, b1);
    expectRoute(graph, s2, {"S2", "T1", "P1", "L1", "L3"}, b1);
    EXPECT_EQ(count(graph, unrelated), 0u);
    EXPECT_EQ(count(graph, t1), 1u);
}

TEST_F(SignalPathObservationTest, FocusingObjectsAndLayersKeepsTheWholeGraph)
{
    BlueprintPanel panel;
    auto* canvas = panel.canvas();
    panel.resize(1200, 800);
    panel.show();
    drainEvents();
    auto* originalNode = canvas->signalPathNode(0);
    for (auto* item : {c1, s5, b2, l3}) {
        canvas->setZoomFactor(0.6);
        ASSERT_TRUE(canvas->focusItem(item));
        EXPECT_EQ(canvas->activeSignalPathSource()->graph().paths.size(), 6u);
        EXPECT_EQ(canvas->signalPathNode(0), originalNode);
        EXPECT_DOUBLE_EQ(canvas->transform().m11(), 1.0);
        const auto* node = canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), item));
        EXPECT_TRUE(node->isSelected());
        EXPECT_LT(QLineF(canvas->mapToScene(canvas->viewport()->rect().center()),
                        node->sceneBoundingRect().center()).length(), 2.0);
    }
    EXPECT_EQ(canvas->signalPathNode(0), originalNode);
}

TEST_F(SignalPathObservationTest, BlankMenuCreatesAllLayerAndBusTypesAtBucketRoots)
{
    InteractiveBlueprint canvas;
    canvas.resize(1200, 800);
    canvas.focusItem(s5);
    const auto originalTransform = canvas.transform();
    const auto originalPosition = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s5))->pos();
    uint32_t selected = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeSelected, &canvas,
        [&](uint32_t id) { selected = id; });
    auto* stack = UndoRedoManager::instance().undoStack();
    const std::vector<std::pair<QString, T>> entries{
        {"Sound", T::Sound}, {"Random", T::Container}, {"Sequence", T::Container},
        {"Match", T::Container}, {"Blend", T::Container}, {"Track", T::Track}, {"Piece", T::Piece},
        {"Layer", T::Layer}, {"Bus", T::Bus}};
    for (const auto& [label, type] : entries) {
        QMenu menu;
        const auto blank = canvas.mapFromScene(canvas.scene()->itemsBoundingRect().bottomRight() + QPointF(200, 200));
        canvas.menuAt(menu, blank);
        ASSERT_EQ(menuLabels(menu), (QStringList{"Create"}));
        auto* createMenu = menu.actions().front()->menu();
        ASSERT_NE(createMenu, nullptr);
        EXPECT_EQ(menuLabels(*createMenu), (QStringList{"Sound", "Container", "Track", "Piece", "Layer", "Bus"}));
        QAction* action = nullptr;
        for (auto* entry : createMenu->actions()) {
            if (entry->text() == label) action = entry;
            if (auto* sub = entry->menu()) {
                EXPECT_EQ(menuLabels(*sub), (QStringList{"Random", "Sequence", "Match", "Blend"}));
                for (auto* mode : sub->actions()) {
                    if (mode->text() == label) action = mode;
                }
            }
        }
        ASSERT_NE(action, nullptr);
        const int previousIndex = stack->index();
        const auto position = canvas.mapToScene(blank);
        action->trigger();
        drainEvents();
        auto* created = ProjectManager::instance().findItemByObjectID(selected);
        ASSERT_NE(created, nullptr);
        ASSERT_EQ(created->type, type);
        EXPECT_EQ(created->parent, type == T::Bus ? master : nullptr);
        EXPECT_EQ(stack->index(), previousIndex + 1);
        EXPECT_EQ(canvas.transform(), originalTransform);
        const auto& graph = canvas.activeSignalPathSource()->graph();
        ASSERT_TRUE(graph.ok());
        EXPECT_EQ(canvas.signalPathNode(indexOf(graph, s5))->pos(), originalPosition);
        EXPECT_EQ(count(graph, created), 1u);
        EXPECT_EQ(canvas.signalPathNode(indexOf(graph, created))->pos(), position);
        if (type == T::Container) {
            const auto modes = std::vector{SelectionMode::Random, SelectionMode::Sequence, SelectionMode::Match, SelectionMode::Blend};
            const auto names = QStringList{"Random", "Sequence", "Match", "Blend"};
            EXPECT_EQ(created->comp<VNX::VSelection>()->mode, modes[names.indexOf(label)]);
        }
        const auto guid = created->guid;
        stack->undo();
        drainEvents();
        EXPECT_EQ(ProjectManager::instance().findItemByGuid(guid), nullptr);
        EXPECT_EQ(count(canvas.activeSignalPathSource()->graph(), created), 0u);
        stack->redo();
        drainEvents();
        ASSERT_NE(ProjectManager::instance().findItemByGuid(guid), nullptr);
        const auto& restored = canvas.activeSignalPathSource()->graph();
        EXPECT_EQ(canvas.signalPathNode(indexOf(restored, created))->pos(), position);
    }
}

TEST_F(SignalPathObservationTest, CanvasCreationUsesDefaultNameAndUndoRestoresProjectAndLayout)
{
    InteractiveBlueprint canvas;
    const auto originalNodes = canvas.activeSignalPathSource()->graph().nodes.size();
    const auto originalPaths = canvas.activeSignalPathSource()->graph().paths.size();
    QMenu menu;
    const auto blank = canvas.mapFromScene(canvas.scene()->itemsBoundingRect().bottomRight() + QPointF(200, 200));
    canvas.menuAt(menu, blank);
    ASSERT_EQ(menuLabels(menu), (QStringList{"Create"}));
    auto* createMenu = menu.actions().front()->menu();
    ASSERT_NE(createMenu, nullptr);
    auto* createSound = actionNamed(*createMenu, QStringLiteral("Sound"));
    ASSERT_NE(createSound, nullptr);
    auto* stack = UndoRedoManager::instance().undoStack();
    const int previousIndex = stack->index();
    createSound->trigger();
    drainEvents();
    EXPECT_EQ(stack->index(), previousIndex + 1);
    EXPECT_EQ(canvas.activeSignalPathSource()->graph().paths.size(), originalPaths + 1);
    stack->undo();
    drainEvents();
    EXPECT_EQ(stack->index(), previousIndex);
    EXPECT_EQ(canvas.activeSignalPathSource()->graph().nodes.size(), originalNodes);
    EXPECT_EQ(canvas.activeSignalPathSource()->graph().paths.size(), originalPaths);
    createSound->trigger();
    drainEvents();
    EXPECT_FALSE(canvas.scene()->items().empty());
    EXPECT_EQ(canvas.activeSignalPathSource()->graph().paths.size(), originalPaths + 1);
    canvas.rebuildFromProject();
    EXPECT_FALSE(canvas.scene()->items().empty());
}

TEST_F(SignalPathObservationTest, NodeMenuRenamesAndDeletesWithUndoAndProtectsMasterBus)
{
    InteractiveBlueprint canvas;
    canvas.resize(1000, 700);
    canvas.focusItem(l3);
    const auto nodePoint = [&](ProjectItem* item) {
        auto* node = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), item));
        canvas.centerOn(node);
        return canvas.mapFromScene(node->sceneBoundingRect().center());
    };
    QMenu menu;
    canvas.menuAt(menu, nodePoint(l1));
    EXPECT_EQ(menuLabels(menu), (QStringList{"New Child", "Rename", "Delete"}));
    actionNamed(menu, QStringLiteral("Rename"))->trigger();
    auto* node = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), l1));
    ASSERT_TRUE(node->isTitleEditing());
    node->titleEditor()->setText(QStringLiteral("RenamedLayer"));
    node->finishTitleEditing(true);
    drainEvents();
    EXPECT_EQ(l1->name, QStringLiteral("RenamedLayer"));
    const auto& graph = canvas.activeSignalPathSource()->graph();
    EXPECT_EQ(count(graph, l1), 1u);
    EXPECT_EQ(canvas.signalPathNode(indexOf(graph, l1))->title(), l1->name);
    UndoRedoManager::instance().undoStack()->undo();
    drainEvents();
    EXPECT_EQ(l1->name, QStringLiteral("L1"));
    QMenu soundMenu;
    canvas.menuAt(soundMenu, nodePoint(s5));
    EXPECT_EQ(menuLabels(soundMenu), (QStringList{"Rename", "Delete"}));
    soundMenu.actions().back()->trigger();
    drainEvents();
    EXPECT_EQ(count(canvas.activeSignalPathSource()->graph(), s5), 0u);
    UndoRedoManager::instance().undoStack()->undo();
    drainEvents();
    EXPECT_EQ(count(canvas.activeSignalPathSource()->graph(), s5), 1u);
    QMenu masterMenu;
    canvas.menuAt(masterMenu, nodePoint(master));
    EXPECT_EQ(menuLabels(masterMenu), (QStringList{"New Child", "Rename", "Delete"}));
    EXPECT_FALSE(masterMenu.actions().back()->isEnabled());
}

TEST_F(SignalPathObservationTest, NodeNewChildUsesHierarchyRulesAndCreatesUnderItsTarget)
{
    InteractiveBlueprint canvas;
    canvas.resize(1200, 800);
    canvas.show();
    drainEvents();
    const std::vector<std::pair<ProjectItem*, QStringList>> rows{
        {l1, {"Sound", "Container", "Track", "Piece", "Layer"}},
        {c1, {"Sound", "Container", "Track", "Piece"}},
        {t1, {"Sound", "Container", "Piece"}},
        {p1, {"Track"}}, {b1, {"Bus"}}, {master, {"Bus"}}, {s5, {}}};
    uint32_t selected = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeSelected, &canvas,
        [&](uint32_t id) { selected = id; });
    auto* stack = UndoRedoManager::instance().undoStack();
    for (const auto& [parent, labels] : rows) {
        auto* node = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), parent));
        canvas.centerOn(node);
        QMenu menu;
        canvas.menuAt(menu, canvas.mapFromScene(node->sceneBoundingRect().center()));
        auto* childAction = actionNamed(menu, QStringLiteral("New Child"));
        ASSERT_NE(childAction, nullptr);
        EXPECT_EQ(childAction->isVisible(), !labels.empty());
        ASSERT_NE(childAction->menu(), nullptr);
        EXPECT_EQ(menuLabels(*childAction->menu()), labels);
        if (labels.empty()) continue;
        const auto transform = canvas.transform();
        const auto center = canvas.mapToScene(canvas.viewport()->rect().center());
        actionNamed(*childAction->menu(), labels.front())->trigger();
        drainEvents();
        auto* created = ProjectManager::instance().findItemByObjectID(selected);
        ASSERT_NE(created, nullptr);
        EXPECT_EQ(created->parent, parent);
        const auto& graph = canvas.activeSignalPathSource()->graph();
        ASSERT_TRUE(graph.ok());
        ASSERT_EQ(count(graph, created), 1u);
        if (created->type != T::Bus) {
            const auto& route = path(graph, created);
            ASSERT_GE(route.computation.size(), 2u);
            EXPECT_EQ(graph.nodes[route.computation[1].occurrenceIndex].key.objectGuid, parent->guid);
        }
        auto* childNode = canvas.signalPathNode(indexOf(graph, created));
        EXPECT_TRUE(childNode->isSelected());
        expectDisjointNodes(canvas);
        EXPECT_EQ(canvas.transform(), transform);
        EXPECT_EQ(canvas.mapToScene(canvas.viewport()->rect().center()), center);
        const auto position = childNode->pos();
        const auto guid = created->guid;
        stack->undo();
        drainEvents();
        EXPECT_EQ(ProjectManager::instance().findItemByGuid(guid), nullptr);
        expectDisjointNodes(canvas);
        stack->redo();
        drainEvents();
        ASSERT_EQ(ProjectManager::instance().findItemByGuid(guid), created);
        EXPECT_EQ(created->parent, parent);
        EXPECT_EQ(canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), created))->pos(), position);
        expectDisjointNodes(canvas);
    }
}

TEST_F(SignalPathObservationTest, HierarchyCreationReflowsEmptyLayersAndNestedContainersInEveryDirection)
{
    auto& pm = ProjectManager::instance();
    pm.closeProject();
    ProjectConfig config;
    config.name = QStringLiteral("DynamicBlueprintLayout");
    config.location = directory.path();
    config.platforms = HarkenTest::platforms({"Windows"});
    ASSERT_TRUE(pm.createProject(config));
    master = pm.getMasterBusItem();
    Hierarchy hierarchy;
    BlueprintCanvasWidget canvas;
    canvas.resize(1200, 800);
    canvas.show();
    for (auto direction : {CanvasFlowDirection::LR, CanvasFlowDirection::RL,
                           CanvasFlowDirection::TB, CanvasFlowDirection::BT}) {
        SCOPED_TRACE(static_cast<int>(direction));
        canvas.setFlowDirection(direction);
        canvas.setZoomFactor(0.75);
        canvas.centerOn(QPointF(200, 200));
        const auto center = canvas.mapToScene(canvas.viewport()->rect().center());
        auto* layer = create("NewLayer", T::Layer);
        ASSERT_TRUE(hierarchy.selectProjectItem(layer));
        drainEvents();
        expectRoute(canvas.activeSignalPathSource()->graph(), layer, {"NewLayer"}, master);
        expectDisjointNodes(canvas);
        auto* parent = layer;
        QStringList names{layer->name};
        for (const auto& [name, type] : std::array<std::pair<const char*, T>, 3>{{
                {"NewContainer", T::Container}, {"NestedContainer", T::Container}, {"NewSound", T::Sound}}}) {
            parent = create(name, type, parent);
            ASSERT_TRUE(hierarchy.selectProjectItem(parent));
            names.prepend(parent->name);
            drainEvents();
            expectRoute(canvas.activeSignalPathSource()->graph(), parent, names, master);
            expectDisjointNodes(canvas);
            EXPECT_DOUBLE_EQ(canvas.transform().m11(), 0.75);
            EXPECT_EQ(canvas.mapToScene(canvas.viewport()->rect().center()), center);
        }
        for (int branch = 0; branch < 3; ++branch) {
            ASSERT_NE(pm.createItem(layer, QStringLiteral("SiblingContainer%1").arg(branch), T::Container), nullptr);
            drainEvents();
            expectDisjointNodes(canvas);
        }
        ASSERT_TRUE(pm.deleteItem(layer));
        drainEvents();
        expectDisjointNodes(canvas);
    }
}

TEST_F(SignalPathObservationTest, CreatingLayerThenTrackThenSoundConnectsAndKeepsLayerPositionAcrossUndo)
{
    InteractiveBlueprint canvas;
    canvas.resize(1200, 800);
    canvas.show();
    drainEvents();
    uint32_t selected = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeSelected, &canvas,
        [&](uint32_t id) { selected = id; });
    QMenu blankMenu;
    const auto blank = canvas.mapFromScene(canvas.scene()->itemsBoundingRect().bottomRight() + QPointF(200, 200));
    const auto layerPosition = canvas.mapToScene(blank);
    canvas.menuAt(blankMenu, blank);
    auto* createAction = actionNamed(blankMenu, QStringLiteral("Create"));
    ASSERT_NE(createAction, nullptr);
    ASSERT_NE(createAction->menu(), nullptr);
    auto* layerAction = actionNamed(*createAction->menu(), QStringLiteral("Layer"));
    ASSERT_NE(layerAction, nullptr);
    layerAction->trigger();
    drainEvents();
    auto* layer = ProjectManager::instance().findItemByObjectID(selected);
    ASSERT_NE(layer, nullptr);
    ASSERT_EQ(layer->type, T::Layer);
    EXPECT_EQ(canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), layer))->pos(), layerPosition);
    EXPECT_EQ(layer->name, QStringLiteral("New_Layer"));
    expectRoute(canvas.activeSignalPathSource()->graph(), layer, {"New_Layer"}, master);
    expectDisjointNodes(canvas);
    const auto config = layer->fieldGroups;
    auto* parent = layer;
    QStringList names{layer->name};
    for (const auto& type : {QStringLiteral("Track"), QStringLiteral("Sound")}) {
        auto* parentNode = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), parent));
        canvas.centerOn(parentNode);
        QMenu menu;
        canvas.menuAt(menu, canvas.mapFromScene(parentNode->sceneBoundingRect().center()));
        auto* childAction = actionNamed(menu, QStringLiteral("New Child"));
        ASSERT_NE(childAction, nullptr);
        ASSERT_NE(childAction->menu(), nullptr);
        auto* action = actionNamed(*childAction->menu(), type);
        ASSERT_NE(action, nullptr);
        action->trigger();
        drainEvents();
        auto* child = ProjectManager::instance().findItemByObjectID(selected);
        ASSERT_NE(child, nullptr);
        EXPECT_EQ(child->parent, parent);
        names.prepend(child->name);
        const auto& graph = canvas.activeSignalPathSource()->graph();
        expectRoute(graph, child, names, master);
        EXPECT_EQ(count(graph, layer), 1u);
        EXPECT_EQ(canvas.signalPathNode(indexOf(graph, layer))->pos(), layerPosition);
        EXPECT_EQ(layer->fieldGroups, config);
        expectDisjointNodes(canvas);
        parent = child;
    }
    auto* track = parent->parent;
    auto* timeline = track->comp<VTrack>();
    ASSERT_NE(timeline, nullptr);
    const auto defaultClips = timeline->clips;
    ASSERT_EQ(defaultClips.items.size(), 1u);
    EXPECT_EQ(defaultClips.items.front().targetGuid, parent->guid);
    EXPECT_EQ(defaultClips.items.front().start, 0);
    const auto occurrence = PlayablePropertyAuthoringV1::trackClipLocator(
        *track, defaultClips.items.front().clipGuid);
    ASSERT_TRUE(occurrence.has_value());
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), parent).identity.occurrences,
              (std::vector{*occurrence}));
    auto* stack = UndoRedoManager::instance().undoStack();
    stack->undo();
    drainEvents();
    EXPECT_TRUE(timeline->clips.items.empty());
    expectRoute(canvas.activeSignalPathSource()->graph(), track, {"New_Track", "New_Layer"}, master);
    stack->undo();
    drainEvents();
    expectRoute(canvas.activeSignalPathSource()->graph(), layer, {"New_Layer"}, master);
    EXPECT_EQ(canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), layer))->pos(), layerPosition);
    stack->redo();
    stack->redo();
    drainEvents();
    EXPECT_EQ(timeline->clips, defaultClips);
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), parent).identity.occurrences,
              (std::vector{*occurrence}));
    expectRoute(canvas.activeSignalPathSource()->graph(), parent, names, master);
    EXPECT_EQ(canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), layer))->pos(), layerPosition);
    expectDisjointNodes(canvas);
}



TEST_F(SignalPathObservationTest, VisibilityMenuTogglesFieldsWithoutDismissing)
{
    const auto sendKey = [](QWidget* target, int key) {
        for (auto type : {QEvent::KeyPress, QEvent::KeyRelease}) {
            QKeyEvent event(type, key, Qt::NoModifier);
            QApplication::sendEvent(target, &event);
        }
        drainEvents();
    };
    const auto click = [](QWidget* target, const QPoint& position) {
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, position, target->mapToGlobal(position), Qt::LeftButton,
                type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(target, &event);
        }
        drainEvents();
    };
    BlueprintPanel panel;
    panel.resize(900, 600);
    panel.show();
    drainEvents();
    auto* canvas = panel.canvas();
    canvas->setNodeContentFields(kAllNodeContentFields);
    canvas->setZoomFactor(0.75);
    auto* filter = panel.findChild<QToolButton*>(QStringLiteral("BlueprintViewportVisibilityButton"));
    auto* menu = panel.findChild<QMenu*>(QStringLiteral("BlueprintViewportVisibilityMenu"));
    ASSERT_NE(filter, nullptr);
    ASSERT_NE(menu, nullptr);
    EXPECT_EQ(panel.findChild<QWidget*>(QStringLiteral("BlueprintPropertiesFilterDrawer")), nullptr);
    EXPECT_EQ(menu->findChildren<QCheckBox*>().size(), 7);
    EXPECT_TRUE(menu->findChildren<QToolButton*>().isEmpty());
    ASSERT_NE(menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Waveform")), nullptr);
    auto* title = menu->findChild<QLabel*>(QStringLiteral("BlueprintVisibilityLabel_Playback"));
    auto* playback = menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Playback"));
    auto* scroll = menu->findChild<QScrollArea*>(QStringLiteral("BlueprintVisibilityScrollArea"));
    ASSERT_NE(title, nullptr);
    ASSERT_NE(playback, nullptr);
    ASSERT_NE(scroll, nullptr);
    EXPECT_EQ(title->parentWidget(), playback->parentWidget());
    EXPECT_EQ(playback->checkState(), Qt::Checked);
    const auto canvasGeometry = canvas->geometry();
    const auto sceneOrigin = canvas->mapToScene(QPoint());
    const auto nodeHeight = canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), s1))->contentHeight();
    filter->click();
    drainEvents();
    ASSERT_TRUE(menu->isVisible());
    EXPECT_TRUE(filter->isChecked());
    EXPECT_LE(scroll->height(), 240);
    EXPECT_EQ(title->x(), 8);
    auto* volume = menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Volume"));
    auto* pitch = menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Pitch"));
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(pitch, nullptr);
    auto* assist = menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Assist"));
    EXPECT_EQ(QApplication::focusWidget(), assist);
    playback->setFocus();
    sendKey(playback, Qt::Key_Tab);
    EXPECT_EQ(QApplication::focusWidget(), volume);
    sendKey(volume, Qt::Key_Tab);
    EXPECT_EQ(QApplication::focusWidget(), pitch);
    sendKey(pitch, Qt::Key_Up);
    EXPECT_EQ(QApplication::focusWidget(), volume);
    sendKey(volume, Qt::Key_Backtab);
    EXPECT_EQ(QApplication::focusWidget(), playback);
    sendKey(assist, Qt::Key_Up);
    auto* loop = menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Loop"));
    ASSERT_NE(loop, nullptr);
    EXPECT_EQ(QApplication::focusWidget(), loop);
    sendKey(loop, Qt::Key_Down);
    EXPECT_EQ(QApplication::focusWidget(), assist);
    EXPECT_TRUE(menu->isVisible());
    click(title, title->rect().center());
    EXPECT_TRUE(menu->isVisible());
    EXPECT_EQ(canvas->nodeContentFields(), kAllNodeContentFields);
    const auto propertyFields = NodeContentField::Volume | NodeContentField::Pitch
        | NodeContentField::OffsetOfStart | NodeContentField::Loop;
    for (const auto* name : {"Volume", "Pitch", "OffsetOfStart", "Loop"}) {
        auto* field = menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_") + QLatin1String(name));
        auto* label = menu->findChild<QLabel*>(QStringLiteral("BlueprintVisibilityLabel_") + QLatin1String(name));
        ASSERT_NE(field, nullptr);
        ASSERT_NE(label, nullptr);
        EXPECT_EQ(field->checkState(), Qt::Checked);
        EXPECT_EQ(label->x(), 24);
        EXPECT_EQ(label->x() - title->x(), 16);
        EXPECT_GT(title->font().weight(), label->font().weight());
        EXPECT_EQ(field->mapTo(menu, QPoint()).x(), playback->mapTo(menu, QPoint()).x());
        const auto before = canvas->nodeContentFields();
        click(label, label->rect().center());
        EXPECT_EQ(canvas->nodeContentFields(), before);
        EXPECT_TRUE(menu->isVisible());
        click(field, field->rect().center());
        EXPECT_EQ(field->checkState(), Qt::Unchecked);
        EXPECT_TRUE(menu->isVisible());
        EXPECT_TRUE(canvas->nodeContentFields().testFlag(NodeContentField::Waveform));
        EXPECT_EQ(canvas->geometry(), canvasGeometry);
        EXPECT_EQ(canvas->mapToScene(QPoint()), sceneOrigin);
        EXPECT_DOUBLE_EQ(canvas->transform().m11(), 0.75);
    }
    EXPECT_EQ(canvas->nodeContentFields() & propertyFields, NodeContentFields{});
    EXPECT_LT(canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), s1))->contentHeight(), nodeHeight);
    EXPECT_NE(gainEditorFor(*canvas, SignalPathEdgeKind::Output, p1, b1)->binding(NodeContentField::OutputVolume), nullptr);
    expectDisjointNodes(*canvas);
    sendKey(menu, Qt::Key_Escape);
    EXPECT_FALSE(menu->isVisible());
    EXPECT_TRUE(filter->isChecked());
    BlueprintPanel reopened;
    EXPECT_EQ(reopened.canvas()->nodeContentFields(), kAllNodeContentFields & ~propertyFields);
    filter->click();
    drainEvents();
    ASSERT_TRUE(menu->isVisible());
    for (const auto* name : {"Volume", "Pitch", "OffsetOfStart", "Loop"}) {
        auto* field = menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_") + QLatin1String(name));
        ASSERT_NE(field, nullptr);
        field->setFocus();
        sendKey(field, Qt::Key_Space);
        EXPECT_TRUE(menu->isVisible());
        EXPECT_EQ(field->checkState(), Qt::Checked);
    }
    EXPECT_EQ(canvas->nodeContentFields(), kAllNodeContentFields);
    menu->hide();
    drainEvents();
    EXPECT_FALSE(filter->isChecked());
    canvas->setNodeContentFields(propertyFields);
    EXPECT_TRUE(filter->isChecked());
    EXPECT_EQ(canvas->geometry(), canvasGeometry);
    EXPECT_EQ(canvas->mapToScene(QPoint()), sceneOrigin);
    QFont largeFont = menu->font();
    largeFont.setPixelSize(64);
    menu->setFont(largeFont);
    filter->click();
    drainEvents();
    ASSERT_TRUE(menu->isVisible());
    EXPECT_EQ(scroll->height(), 240);
    EXPECT_GT(scroll->verticalScrollBar()->maximum(), 0);
    EXPECT_EQ(scroll->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    scroll->ensureWidgetVisible(loop);
    drainEvents();
    EXPECT_TRUE(scroll->viewport()->rect().contains(loop->mapTo(scroll->viewport(), loop->rect().center())));
    menu->hide();
}

TEST_F(SignalPathObservationTest, VisibilityGroupCheckboxTracksPartialStateAndPreservesOtherFields)
{
    const auto click = [](QWidget* target, const QPoint& position) {
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, position, target->mapToGlobal(position), Qt::LeftButton,
                type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(target, &event);
        }
        drainEvents();
    };
    const auto sendSpace = [](QWidget* target) {
        for (auto type : {QEvent::KeyPress, QEvent::KeyRelease}) {
            QKeyEvent event(type, Qt::Key_Space, Qt::NoModifier);
            QApplication::sendEvent(target, &event);
        }
        drainEvents();
    };
    BlueprintPanel panel;
    panel.resize(900, 600);
    panel.show();
    drainEvents();
    auto* canvas = panel.canvas();
    auto* filter = panel.findChild<QToolButton*>(QStringLiteral("BlueprintViewportVisibilityButton"));
    auto* menu = panel.findChild<QMenu*>(QStringLiteral("BlueprintViewportVisibilityMenu"));
    ASSERT_NE(filter, nullptr);
    ASSERT_NE(menu, nullptr);
    auto* playback = menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Playback"));
    auto* title = menu->findChild<QLabel*>(QStringLiteral("BlueprintVisibilityLabel_Playback"));
    auto* volume = menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Volume"));
    auto* pitch = menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_Pitch"));
    ASSERT_NE(playback, nullptr);
    ASSERT_NE(title, nullptr);
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(pitch, nullptr);
    constexpr auto playbackFields = NodeContentField::Volume | NodeContentField::Pitch
        | NodeContentField::OffsetOfStart | NodeContentField::Loop;
    for (const bool waveformVisible : {false, true}) {
        SCOPED_TRACE(waveformVisible);
        const auto unaffected = waveformVisible ? kAllNodeContentFields & ~playbackFields
            : kAllNodeContentFields & ~playbackFields & ~NodeContentFields(NodeContentField::Waveform);
        const auto partial = unaffected | NodeContentField::Volume | NodeContentField::Loop;
        canvas->setNodeContentFields(partial);
        filter->click();
        drainEvents();
        ASSERT_TRUE(menu->isVisible());
        EXPECT_EQ(playback->checkState(), Qt::PartiallyChecked);
        EXPECT_EQ(volume->checkState(), Qt::Checked);
        EXPECT_EQ(pitch->checkState(), Qt::Unchecked);
        click(title, title->rect().center());
        EXPECT_EQ(canvas->nodeContentFields(), partial);
        EXPECT_TRUE(menu->isVisible());
        const auto expectState = [&](Qt::CheckState state, NodeContentFields expected) {
            EXPECT_EQ(playback->checkState(), state);
            EXPECT_EQ(canvas->nodeContentFields(), expected);
            EXPECT_EQ(canvas->nodeContentFields() & ~playbackFields, unaffected);
            EXPECT_TRUE(menu->isVisible());
            for (const auto* name : {"Volume", "Pitch", "OffsetOfStart", "Loop"}) {
                auto* field = menu->findChild<QCheckBox*>(QStringLiteral("BlueprintVisibilityField_") + QLatin1String(name));
                ASSERT_NE(field, nullptr);
                EXPECT_EQ(field->checkState(), state);
            }
        };
        click(playback, playback->rect().center());
        expectState(Qt::Checked, unaffected | playbackFields);
        playback->setFocus();
        sendSpace(playback);
        expectState(Qt::Unchecked, unaffected);
        sendSpace(playback);
        expectState(Qt::Checked, unaffected | playbackFields);
        pitch->click();
        EXPECT_EQ(playback->checkState(), Qt::PartiallyChecked);
        EXPECT_EQ(canvas->nodeContentFields(), (unaffected | playbackFields) & ~NodeContentFields(NodeContentField::Pitch));
        click(playback, playback->rect().center());
        expectState(Qt::Checked, unaffected | playbackFields);
        menu->hide();
        drainEvents();
    }
}



TEST_F(SignalPathObservationTest, PanToolMovesCanvasOverNodesAndSelectToolRestoresRubberBand)
{
    BlueprintPanel panel;
    panel.resize(1000, 750);
    panel.show();
    drainEvents();
    auto& canvas = *panel.canvas();
    canvas.setZoomFactor(1.0);
    auto* node = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s1));
    const auto original = node->pos();
    node->setSelected(true);
    canvas.centerOn(node);
    auto* pan = panel.findChild<QToolButton*>(QStringLiteral("BlueprintPanButton"));
    auto* select = panel.findChild<QToolButton*>(QStringLiteral("BlueprintSelectButton"));
    ASSERT_NE(pan, nullptr);
    ASSERT_NE(select, nullptr);
    EXPECT_FALSE(pan->icon().isNull());
    EXPECT_FALSE(select->icon().isNull());
    canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
    const auto panOutline = pan->icon().pixmap(20, 20).toImage();
    const auto selectOutline = select->icon().pixmap(20, 20).toImage();
    const auto panGeometry = pan->geometry();
    const auto selectGeometry = select->geometry();
    for (auto* button : {pan, select}) {
        EXPECT_EQ(button->iconSize(), QSize(20, 20));
        EXPECT_TRUE(button->property("flatIconButton").toBool());
    }
    pan->click();
    EXPECT_NE(pan->icon().pixmap(20, 20).toImage(), panOutline);
    EXPECT_NE(select->icon().pixmap(20, 20).toImage(), selectOutline);
    EXPECT_EQ(pan->geometry(), panGeometry);
    EXPECT_EQ(select->geometry(), selectGeometry);
    EXPECT_EQ(canvas.interactionMode(), BlueprintCanvasView::InteractionMode::Pan);
    EXPECT_TRUE(pan->isChecked());
    EXPECT_FALSE(select->isChecked());
    int doubleClicks = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeDoubleClicked, &panel, [&](uint32_t) { ++doubleClicks; });
    const auto center = canvas.mapToScene(canvas.viewport()->rect().center());
    const auto start = canvas.mapFromScene(node->sceneBoundingRect().center());
    sendMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseMove, start + QPoint(80, 60), Qt::NoButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, start + QPoint(80, 60), Qt::LeftButton, Qt::NoButton);
    sendMouse(canvas, QEvent::MouseButtonDblClick, start + QPoint(80, 60), Qt::LeftButton, Qt::LeftButton);
    EXPECT_EQ(node->pos(), original);
    EXPECT_TRUE(node->isSelected());
    EXPECT_NE(canvas.mapToScene(canvas.viewport()->rect().center()), center);
    EXPECT_EQ(doubleClicks, 0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    BlueprintPanel reopened;
    EXPECT_EQ(reopened.canvas()->interactionMode(), BlueprintCanvasView::InteractionMode::Pan);
    select->click();
    EXPECT_EQ(pan->icon().pixmap(20, 20).toImage(), panOutline);
    EXPECT_EQ(select->icon().pixmap(20, 20).toImage(), selectOutline);
    EXPECT_EQ(canvas.dragMode(), QGraphicsView::RubberBandDrag);
    EXPECT_TRUE(canvas.isInteractive());
    canvas.scene()->clearSelection();
    const auto bounds = node->sceneBoundingRect().adjusted(-10, -10, 10, 10);
    const auto corner = canvas.mapFromScene(bounds.topLeft());
    const auto opposite = canvas.mapFromScene(bounds.bottomRight());
    sendMouse(canvas, QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseMove, opposite, Qt::NoButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, opposite, Qt::LeftButton, Qt::NoButton);
    EXPECT_TRUE(node->isSelected());
    EXPECT_EQ(node->pos(), original);
}

TEST_F(SignalPathObservationTest, PanOverridesFieldDraggingAndTitleOrValueEditing)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1200, 800);
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Pan);
    canvas.setZoomFactor(1.0);
    canvas.show();
    drainEvents();
    for (const bool gain : {false, true}) {
        const auto& graph = canvas.activeSignalPathSource()->graph();
        auto* node = gain ? canvas.signalPathGainNode(routingEdge(graph, SignalPathEdgeKind::Send, c1, b2))
            : canvas.signalPathNode(indexOf(graph, s1));
        auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget());
        auto* label = qobject_cast<QGridLayout*>(editor->layout())->itemAtPosition(0, 0)->widget();
        auto* value = editor->findChild<QLabel*>(gain ? "nodeValue_sendLevel" : "nodeValue_volume");
        auto* host = editor->findChild<ParameterControlHost*>(gain ? "nodeSlider_sendLevel" : "nodeSlider_volume");
        auto* slider = host->findChild<QSlider*>();
        ASSERT_NE(value, nullptr);
        ASSERT_NE(slider, nullptr);
        const auto field = gain ? NodeContentField::SendVolume : NodeContentField::Volume;
        const auto originalValue = editor->binding(field)->value();
        const auto original = node->pos();
        for (auto* widget : std::array<QWidget*, 4>{label, value, slider, nullptr}) {
            if (gain && !widget) continue;
            canvas.centerOn(node);
            const auto point = [&]() {
                return canvas.mapFromScene(widget
                    ? editor->graphicsProxyWidget()->mapToScene(widget->mapTo(editor, widget->rect().center()))
                    : node->mapToScene(node->titleRect().center()));
            };
            const auto start = point();
            const auto previousTransform = canvas.viewportTransform();
            sendMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            sendMouse(canvas, QEvent::MouseMove, start + QPoint(40, 30), Qt::NoButton, Qt::LeftButton);
            sendMouse(canvas, QEvent::MouseButtonRelease, start + QPoint(40, 30), Qt::LeftButton, Qt::NoButton);
            EXPECT_NE(canvas.viewportTransform(), previousTransform);
            const auto doubleClick = point();
            sendMouse(canvas, QEvent::MouseButtonDblClick, doubleClick, Qt::LeftButton, Qt::LeftButton);
            sendMouse(canvas, QEvent::MouseButtonRelease, doubleClick, Qt::LeftButton, Qt::NoButton);
            EXPECT_EQ(node->pos(), original);
            EXPECT_FALSE(editor->isEditing());
            EXPECT_FALSE(node->isTitleEditing());
            EXPECT_DOUBLE_EQ(editor->binding(field)->value(), originalValue);
        }
    }
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathObservationTest, WheelZoomKeepsCursorAnchorDuringAndImmediatelyAfterPan)
{
    for (const auto button : {Qt::RightButton, Qt::LeftButton}) {
        for (const bool releaseBeforeWheel : {false, true}) {
            for (const double initialZoom : {0.5, 2.0}) {
                SCOPED_TRACE(testing::Message() << "button=" << button
                    << " released=" << releaseBeforeWheel << " zoom=" << initialZoom);
                BlueprintCanvasWidget canvas;
                canvas.resize(1000, 750);
                canvas.setInteractionMode(button == Qt::LeftButton
                    ? BlueprintCanvasView::InteractionMode::Pan
                    : BlueprintCanvasView::InteractionMode::Select);
                canvas.show();
                drainEvents();
                canvas.setZoomFactor(initialZoom);
                canvas.centerOn(1000, -800);
                const QPoint start(180, 160);
                const QPoint cursor(780, 520);
                sendWheel(canvas, start, QPoint(0, 120));
                sendMouse(canvas, QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
                sendMouse(canvas, QEvent::MouseButtonPress, start, button, button);
                sendMouse(canvas, QEvent::MouseMove, cursor, Qt::NoButton, button);
                if (releaseBeforeWheel)
                    sendMouse(canvas, QEvent::MouseButtonRelease, cursor, button, Qt::NoButton);

                const auto buttons = releaseBeforeWheel ? Qt::NoButton : button;
                for (const int delta : {120, -120}) {
                    const auto anchor = canvas.mapToScene(cursor);
                    const double zoom = canvas.transform().m11();
                    sendWheel(canvas, cursor, QPoint(0, delta), buttons);
                    EXPECT_NEAR(canvas.transform().m11(), zoom * (delta > 0 ? 1.15 : 1.0 / 1.15), 1e-9);
                    const auto projected = canvas.mapFromScene(anchor);
                    EXPECT_NEAR(projected.x(), cursor.x(), 1);
                    EXPECT_NEAR(projected.y(), cursor.y(), 1);
                }

                if (!releaseBeforeWheel) {
                    const auto anchor = canvas.mapToScene(cursor);
                    const QPoint next = cursor + QPoint(24, -18);
                    sendMouse(canvas, QEvent::MouseMove, next, Qt::NoButton, button);
                    EXPECT_EQ(canvas.mapFromScene(anchor), next);
                    sendMouse(canvas, QEvent::MouseButtonRelease, next, button, Qt::NoButton);
                }
                const auto releasedTransform = canvas.viewportTransform();
                sendMouse(canvas, QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
                EXPECT_EQ(canvas.viewportTransform(), releasedTransform);
            }
        }
    }
}

TEST_F(SignalPathObservationTest, WheelWithoutVerticalDeltaDoesNotZoomOrMoveCanvas)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1000, 750);
    canvas.show();
    drainEvents();
    canvas.setZoomFactor(1.0);
    const QPoint cursor(780, 520);
    sendMouse(canvas, QEvent::MouseButtonPress, cursor, Qt::RightButton, Qt::RightButton);
    const auto before = canvas.viewportTransform();
    int zoomChanges = 0;
    QObject::connect(&canvas, &BlueprintCanvasView::zoomChanged, &canvas,
        [&](double) { ++zoomChanges; });
    for (const auto delta : {QPoint(), QPoint(120, 0)}) {
        sendWheel(canvas, cursor, delta, Qt::RightButton);
        EXPECT_EQ(canvas.viewportTransform(), before);
    }
    EXPECT_EQ(zoomChanges, 0);
    sendMouse(canvas, QEvent::MouseButtonRelease, cursor, Qt::RightButton, Qt::NoButton);
}

TEST_F(SignalPathObservationTest, WheelZoomRespectsLimitsAndAbsoluteZoomKeepsViewportCenter)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1000, 750);
    canvas.show();
    drainEvents();
    const QPoint cursor(780, 520);
    for (const auto limit : {BlueprintCanvasView::kMinZoom, BlueprintCanvasView::kMaxZoom}) {
        canvas.setZoomFactor(limit);
        canvas.centerOn(1000, -800);
        const auto before = canvas.viewportTransform();
        sendWheel(canvas, cursor, QPoint(0, limit < 1.0 ? -120 : 120));
        EXPECT_EQ(canvas.viewportTransform(), before);
    }
    canvas.setZoomFactor(1.0);
    sendWheel(canvas, cursor, QPoint(0, 120));
    const auto center = canvas.viewport()->rect().center();
    const auto sceneCenter = canvas.mapToScene(center);
    canvas.setZoomFactor(2.0);
    const auto projected = canvas.mapFromScene(sceneCenter);
    EXPECT_NEAR(projected.x(), center.x(), 1);
    EXPECT_NEAR(projected.y(), center.y(), 1);
}

TEST_F(SignalPathObservationTest, SnapToggleControlsDraggedPositionsAndKeepsUndoAndRedo)
{
    BlueprintPanel panel;
    panel.resize(1100, 750);
    panel.show();
    drainEvents();
    auto& canvas = *panel.canvas();
    canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
    canvas.setZoomFactor(1.0);
    auto* snap = panel.findChild<QToolButton*>(QStringLiteral("BlueprintSnapButton"));
    ASSERT_NE(snap, nullptr);
    canvas.setGridSnapEnabled(true);
    snap->click();
    EXPECT_FALSE(canvas.gridSnapEnabled());
    const auto index = indexOf(canvas.activeSignalPathSource()->graph(), s1);
    const auto original = canvas.signalPathNode(index)->pos();
    const auto drag = [&](const QPoint& offset) {
        auto* node = canvas.signalPathNode(index);
        canvas.centerOn(node);
        const auto start = canvas.mapFromScene(node->mapToScene(QPointF(node->boundingRect().width() / 2, 12)));
        sendMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseMove, start + QPoint(30, 30), Qt::NoButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseMove, start + offset, Qt::NoButton, Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, start + offset, Qt::LeftButton, Qt::NoButton);
        drainEvents();
    };
    drag(QPoint(83, 67));
    const auto unsnapped = canvas.signalPathNode(index)->pos();
    EXPECT_EQ(unsnapped, original + QPointF(83, 67));
    auto* stack = UndoRedoManager::instance().undoStack();
    stack->undo();
    drainEvents();
    EXPECT_EQ(canvas.signalPathNode(index)->pos(), original);
    stack->redo();
    drainEvents();
    EXPECT_EQ(canvas.signalPathNode(index)->pos(), unsnapped);
    BlueprintPanel reopened;
    EXPECT_FALSE(reopened.canvas()->gridSnapEnabled());
    snap->click();
    drag(QPoint(73, 57));
    const auto snapped = canvas.signalPathNode(index)->pos();
    EXPECT_DOUBLE_EQ(std::fmod(snapped.x(), BlueprintCanvasView::kGridSpacing), 0.0);
    EXPECT_DOUBLE_EQ(std::fmod(snapped.y(), BlueprintCanvasView::kGridSpacing), 0.0);
}

TEST_F(SignalPathObservationTest, MouseDragPersistsObjectPositionsAndSupportsUndoAndReset)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1100, 750);
    canvas.show();
    canvas.setFlowDirection(CanvasFlowDirection::LR);
    canvas.focusItem(l3);
    canvas.setZoomFactor(1.0);
    drainEvents();
    const size_t first = indexOf(canvas.activeSignalPathSource()->graph(), l3);
    const size_t second = indexOf(canvas.activeSignalPathSource()->graph(), l2);
    auto* node = canvas.signalPathNode(first);
    const auto original = node->pos();
    const auto otherPosition = canvas.signalPathNode(second)->pos();
    canvas.centerOn(node);
    const auto start = canvas.mapFromScene(node->mapToScene(QPointF(node->boundingRect().width() / 2, 12)));
    const auto target = start + QPoint(83, 67);
    sendMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseMove, start + QPoint(30, 30), Qt::NoButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
    EXPECT_NE(node->pos(), original);
    emit ProjectManager::instance().itemPropertyChanged(l3, QStringLiteral("volume"));
    drainEvents();
    EXPECT_EQ(canvas.signalPathNode(first), node);
    const auto& edges = canvas.activeSignalPathSource()->graph().edges;
    for (size_t index = 0; index < edges.size(); ++index) {
        const auto& edge = edges[index];
        EXPECT_EQ(canvas.signalPathEdge(index)->path().pointAtPercent(0),
            canvas.signalPathNode(edge.from.occurrenceIndex)->outputPin(static_cast<int>(edge.from.portPairIndex))->scenePos());
        auto* gain = canvas.signalPathGainNode(index);
        if (gain) {
            EXPECT_EQ(canvas.signalPathEdge(index)->path().pointAtPercent(1), gain->inputPin()->scenePos());
            EXPECT_EQ(canvas.signalPathGainOutputEdge(index)->path().pointAtPercent(0), gain->outputPin()->scenePos());
        }
        EXPECT_EQ((gain ? canvas.signalPathGainOutputEdge(index) : canvas.signalPathEdge(index))
                ->path().pointAtPercent(1),
            canvas.signalPathNode(edge.to.occurrenceIndex)->inputPin(static_cast<int>(edge.to.portPairIndex))->scenePos());
    }
    sendMouse(canvas, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
    drainEvents();
    const auto moved = canvas.signalPathNode(first)->pos();
    EXPECT_NE(moved, original);
    EXPECT_EQ(std::fmod(moved.x(), 20.0), 0.0);
    EXPECT_EQ(std::fmod(moved.y(), 20.0), 0.0);
    EXPECT_EQ(canvas.signalPathNode(second)->pos(), otherPosition);
    EXPECT_EQ(l3->canvasPos, QPointF());
    auto* stack = UndoRedoManager::instance().undoStack();
    ASSERT_EQ(stack->count(), 1);
    canvas.rebuildFromProject();
    EXPECT_EQ(canvas.signalPathNode(first)->pos(), moved);
    stack->undo();
    drainEvents();
    EXPECT_EQ(canvas.signalPathNode(first)->pos(), original);
    stack->redo();
    drainEvents();
    EXPECT_EQ(canvas.signalPathNode(first)->pos(), moved);
    canvas.resetNodeLayout();
    EXPECT_EQ(canvas.signalPathNode(first)->pos(), original);
    stack->undo();
    drainEvents();
    EXPECT_EQ(canvas.signalPathNode(first)->pos(), moved);
    BlueprintCanvasWidget reopened;
    reopened.setFlowDirection(CanvasFlowDirection::LR);
    reopened.focusItem(l3);
    EXPECT_EQ(reopened.signalPathNode(first)->pos(), moved);
    EXPECT_EQ(reopened.signalPathNode(second)->pos(), otherPosition);
}

TEST_F(SignalPathObservationTest, GainDragPersistsIndependentlyAndSupportsUndoAndReset)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1100, 750);
    canvas.show();
    canvas.setFlowDirection(CanvasFlowDirection::LR);
    canvas.setGridSnapEnabled(false);
    canvas.setZoomFactor(1.0);
    canvas.scene()->clearSelection();
    drainEvents();
    const auto edgeIndex = routingEdge(canvas.activeSignalPathSource()->graph(), SignalPathEdgeKind::Send, c1, b2);
    auto* gain = canvas.signalPathGainNode(edgeIndex);
    ASSERT_NE(gain, nullptr);
    const auto original = gain->pos();
    std::vector<QPointF> objectPositions;
    for (size_t index = 0; index < canvas.activeSignalPathSource()->graph().nodes.size(); ++index)
        objectPositions.push_back(canvas.signalPathNode(index)->pos());
    canvas.centerOn(gain);
    auto* editor = qobject_cast<NodeContentEditor*>(gain->contentWidget());
    ASSERT_NE(editor, nullptr);
    auto* layout = qobject_cast<QGridLayout*>(editor->layout());
    ASSERT_NE(layout, nullptr);
    auto* label = layout->itemAtPosition(0, 0)->widget();
    const auto start = canvas.mapFromScene(editor->graphicsProxyWidget()->mapToScene(
        label->mapTo(editor, label->rect().center())));
    const auto target = start + QPoint(80, -160);
    sendMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseMove, start + QPoint(0, -30), Qt::NoButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
    EXPECT_NE(gain->pos(), original);
    emit ProjectManager::instance().itemPropertyChanged(c1, QStringLiteral("volume"));
    drainEvents();
    EXPECT_EQ(canvas.signalPathGainNode(edgeIndex), gain);
    EXPECT_EQ(canvas.signalPathEdge(edgeIndex)->path().pointAtPercent(1), gain->inputPin()->scenePos());
    EXPECT_EQ(canvas.signalPathGainOutputEdge(edgeIndex)->path().pointAtPercent(0), gain->outputPin()->scenePos());
    sendMouse(canvas, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
    drainEvents();
    const auto moved = canvas.signalPathGainNode(edgeIndex)->pos();
    EXPECT_EQ(moved, original + QPointF(80, -160));
    EXPECT_EQ(canvas.scene()->selectedItems().size(), 3);
    EXPECT_TRUE(canvas.selectedProjectItems().empty());
    for (size_t index = 0; index < objectPositions.size(); ++index)
        EXPECT_EQ(canvas.signalPathNode(index)->pos(), objectPositions[index]);
    auto* stack = UndoRedoManager::instance().undoStack();
    ASSERT_EQ(stack->count(), 1);
    canvas.rebuildFromProject();
    EXPECT_EQ(canvas.signalPathGainNode(edgeIndex)->pos(), moved);
    stack->undo();
    drainEvents();
    EXPECT_EQ(canvas.signalPathGainNode(edgeIndex)->pos(), original);
    stack->redo();
    drainEvents();
    EXPECT_EQ(canvas.signalPathGainNode(edgeIndex)->pos(), moved);
    canvas.resetNodeLayout();
    EXPECT_EQ(canvas.signalPathGainNode(edgeIndex)->pos(), original);
    stack->undo();
    drainEvents();
    EXPECT_EQ(canvas.signalPathGainNode(edgeIndex)->pos(), moved);
    BlueprintCanvasWidget reopened;
    reopened.setFlowDirection(CanvasFlowDirection::LR);
    EXPECT_EQ(reopened.signalPathGainNode(edgeIndex)->pos(), moved);
    for (size_t index = 0; index < objectPositions.size(); ++index)
        EXPECT_EQ(reopened.signalPathNode(index)->pos(), objectPositions[index]);
}

TEST_F(SignalPathObservationTest, GainAndWirePartsNeverAddHierarchyObjectsToMixedSelection)
{
    BlueprintCanvasWidget canvas;
    const auto& graph = canvas.activeSignalPathSource()->graph();
    const auto index = routingEdge(graph, SignalPathEdgeKind::Output, c1, b1);
    const std::array<QGraphicsItem*, 3> parts{canvas.signalPathGainNode(index),
        canvas.signalPathEdge(index), canvas.signalPathGainOutputEdge(index)};
    auto* sound = canvas.signalPathNode(indexOf(graph, s1));
    for (auto* part : parts) {
        canvas.scene()->clearSelection();
        part->setSelected(true);
        EXPECT_TRUE(canvas.selectedProjectItems().empty());
        sound->setSelected(true);
        EXPECT_EQ(canvas.scene()->selectedItems().size(), 4);
        EXPECT_EQ(canvas.selectedProjectItems(), (QList<ProjectItem*>{s1}));
    }
}

TEST_F(SignalPathObservationTest, GroupDragUsesOneUndoStep)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1100, 750);
    canvas.show();
    canvas.focusItem(l3);
    canvas.scene()->clearSelection();
    canvas.setZoomFactor(1.0);
    drainEvents();
    auto* first = canvas.signalPathNode(0);
    auto* second = canvas.signalPathNode(1);
    const auto firstPosition = first->pos();
    const auto secondPosition = second->pos();
    first->setSelected(true);
    second->setSelected(true);
    canvas.centerOn(first);
    const auto start = canvas.mapFromScene(first->mapToScene(QPointF(first->boundingRect().width() / 2, 12)));
    const auto end = start + QPoint(100, 80);
    sendMouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseMove, start + QPoint(30, 30), Qt::NoButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    drainEvents();
    EXPECT_NE(canvas.signalPathNode(0)->pos(), firstPosition);
    EXPECT_NE(canvas.signalPathNode(1)->pos(), secondPosition);
    auto* stack = UndoRedoManager::instance().undoStack();
    ASSERT_EQ(stack->count(), 1);
    stack->undo();
    drainEvents();
    EXPECT_EQ(canvas.signalPathNode(0)->pos(), firstPosition);
    EXPECT_EQ(canvas.signalPathNode(1)->pos(), secondPosition);
}

TEST_F(SignalPathObservationTest, SelectingSharedNodesReportsObjectsWithoutClipContext)
{
    place(t1, s1);
    BlueprintCanvasWidget canvas;
    canvas.focusItem(l3);
    canvas.scene()->clearSelection();
    std::vector<uint32_t> selected;
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeSelected, &canvas,
                     [&](uint32_t id) { selected.push_back(id); });
    const auto& graph = canvas.activeSignalPathSource()->graph();
    auto* repeated = canvas.signalPathNode(indexOf(graph, s1));
    ASSERT_NE(repeated, nullptr);
    repeated->setSelected(true);
    ASSERT_EQ(selected.size(), 1u);
    EXPECT_EQ(selected.back(), s1->objectID);
    EXPECT_FALSE(canvas.selectedPlacementContext());
    InspectorPanel inspector;
    inspector.showItemInspector(s1);
    EXPECT_EQ(inspector.placementContext(), nullptr);
    canvas.scene()->clearSelection();
    canvas.signalPathNode(indexOf(graph, l3))->setSelected(true);
    EXPECT_EQ(selected.back(), l3->objectID);
    EXPECT_FALSE(canvas.selectedPlacementContext());
    EXPECT_EQ(canvas.activeSignalPathSource()->graph().paths.size(), 7u);
}

TEST_F(SignalPathObservationTest, InheritanceEditUndoAndRedoUpdateRoutesThroughSharedPorts)
{
    BlueprintCanvasWidget canvas;
    canvas.focusItem(l3);
    canvas.scene()->clearSelection();
    canvas.setZoomFactor(0.75);
    canvas.centerOn(QPointF(450.0, 300.0));
    const QPointF center = canvas.mapToScene(canvas.viewport()->rect().center());
    const auto& original = canvas.activeSignalPathSource()->graph();
    canvas.signalPathNode(indexOf(original, s5))->setSelected(true);
    const auto result = edit(s5, "mainOutput", "set_field_group_mode", {{"mode", "Inherit"}});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    drainEvents();
    const auto& inherited = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(inherited.ok());
    EXPECT_EQ(path(inherited, s5).routing.outputBusID, b1->objectID);
    EXPECT_EQ(path(inherited, s5).routing.sends.front().targetBusID, b2->objectID);
    EXPECT_EQ(inherited.nodes[indexOf(inherited, c1)].portPairs.size(), 1u);
    EXPECT_TRUE(canvas.signalPathNode(indexOf(inherited, s5))->isSelected());
    EXPECT_DOUBLE_EQ(canvas.transform().m11(), 0.75);
    EXPECT_EQ(canvas.mapToScene(canvas.viewport()->rect().center()), center);
    UndoRedoManager::instance().undoStack()->undo();
    drainEvents();
    const auto& undone = canvas.activeSignalPathSource()->graph();
    EXPECT_EQ(path(undone, s5).routing.outputBusID, b3->objectID);
    EXPECT_EQ(undone.nodes[indexOf(undone, c1)].portPairs.size(), 1u);
    UndoRedoManager::instance().undoStack()->redo();
    drainEvents();
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s5).routing.outputBusID, b1->objectID);
}

TEST_F(SignalPathObservationTest, OwnEmptySendsRemoveOnlyTheSelectedSoundsBranch)
{
    BlueprintCanvasWidget canvas;
    canvas.focusItem(l3);
    ASSERT_TRUE(edit(s5, "send", "set_field_group_mode", {{"mode", "Local"}}).success);
    ASSERT_TRUE(edit(s5, "send", "replace_field_group_sends", {{"sends", QJsonArray{}}}).success);
    drainEvents();
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(graph.ok());
    EXPECT_TRUE(path(graph, s5).routing.sends.empty());
    EXPECT_EQ(path(graph, s5).routing.outputBusID, b3->objectID);
    EXPECT_EQ(path(graph, s4).routing.sends.front().targetBusID, b2->objectID);
    UndoRedoManager::instance().undoStack()->undo();
    drainEvents();
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s5).routing.sends.size(), 1u);
}

TEST_F(SignalPathObservationTest, PrimaryPlatformSwitchRebuildsWithoutWritingSharedRouting)
{
    const auto shared = s5->fieldGroups.mainOutput.shared;
    s5->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(b1->guid), 0.0f}};
    BlueprintCanvasWidget canvas;
    canvas.focusItem(l3);
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s5).routing.outputBusID, b3->objectID);
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    drainEvents();
    ASSERT_TRUE(canvas.activeSignalPathSource()->graph().ok());
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s5).routing.outputBusID, b1->objectID);
    EXPECT_EQ(s5->fieldGroups.mainOutput.shared, shared);
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    drainEvents();
    EXPECT_EQ(path(canvas.activeSignalPathSource()->graph(), s5).routing.outputBusID, b3->objectID);
}

TEST_F(SignalPathObservationTest, PlacementRemovalPreservesObjectSelectionAndHierarchyConnection)
{
    const auto use = place(t1, s1);
    auto& ledger = t1->comp<VTrack>()->clips;
    const auto before = ledger;
    const auto placement = signalPathPlacementContext({use});
    BlueprintCanvasWidget canvas;
    canvas.focusItem(s1, &placement);
    ASSERT_EQ(canvas.activeSignalPathSource()->graph().paths.size(), 7u);
    const auto beforeEdgeCount = canvas.activeSignalPathSource()->graph().edges.size();
    EXPECT_FALSE(canvas.selectedPlacementContext());
    ledger.items.pop_back();
    emit ProjectManager::instance().itemPropertyChanged(t1, QStringLiteral("clips"));
    drainEvents();
    EXPECT_FALSE(canvas.scene()->items().empty());
    EXPECT_TRUE(canvas.activeSignalPathSource()->graph().ok());
    EXPECT_EQ(canvas.activeSignalPathSource()->graph().paths.size(), 6u);
    EXPECT_EQ(canvas.activeSignalPathSource()->graph().edges.size(), beforeEdgeCount);
    EXPECT_TRUE(canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s1))->isSelected());
    EXPECT_FALSE(canvas.selectedPlacementContext());
    ledger = before;
    emit ProjectManager::instance().itemPropertyChanged(t1, QStringLiteral("clips"));
    drainEvents();
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_EQ(graph.paths.size(), 7u);
    EXPECT_EQ(count(graph, s1), 1u);
    EXPECT_EQ(graph.edges.size(), beforeEdgeCount);
    EXPECT_EQ(graph.edges[hierarchyEdge(graph, s1, t1)].pathIndices.size(), 2u);
    EXPECT_TRUE(std::any_of(graph.paths.begin(), graph.paths.end(), [&](const auto& path) {
        return std::find(path.identity.occurrences.begin(), path.identity.occurrences.end(), use)
            != path.identity.occurrences.end();
    }));
}

TEST_F(SignalPathObservationTest, CreationRenameDeletionAndUndoRefreshTheProjectView)
{
    auto& pm = ProjectManager::instance();
    BlueprintCanvasWidget canvas;
    auto* added = create("Added", T::Sound, c1);
    const QUuid addedGuid = added->guid;
    drainEvents();
    EXPECT_EQ(canvas.activeSignalPathSource()->graph().paths.size(), 7u);
    ASSERT_TRUE(pm.renameItem(added, QStringLiteral("Renamed")));
    drainEvents();
    const auto& renamed = canvas.activeSignalPathSource()->graph();
    EXPECT_EQ(renamed.nodes[indexOf(renamed, added)].name, QStringLiteral("Renamed"));
    canvas.signalPathNode(indexOf(renamed, added))->setSelected(true);
    int cleared = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::selectionCleared, &canvas, [&]() { ++cleared; });
    ASSERT_TRUE(pm.deleteItem(added));
    drainEvents();
    EXPECT_EQ(canvas.activeSignalPathSource()->graph().paths.size(), 6u);
    EXPECT_EQ(cleared, 1);
    UndoRedoManager::instance().undoStack()->undo();
    drainEvents();
    EXPECT_NE(pm.findItemByGuid(addedGuid), nullptr);
    EXPECT_EQ(canvas.activeSignalPathSource()->graph().paths.size(), 7u);
}

TEST_F(SignalPathObservationTest, BusScopeIncludesOnlyPathsThatReachItAndTheirOtherOutputs)
{
    const auto graph = build(b2);
    ASSERT_TRUE(graph.ok());
    EXPECT_EQ(graph.paths.size(), 2u);
    EXPECT_EQ(count(graph, s4), 1u);
    EXPECT_EQ(count(graph, s5), 1u);
    EXPECT_EQ(count(graph, b1), 1u);
    EXPECT_EQ(count(graph, b3), 1u);
    EXPECT_EQ(count(graph, master), 1u);
    EXPECT_EQ(build(master).paths.size(), 6u);
}

TEST_F(SignalPathObservationTest, UnusedBusStillShowsItsOutputAndSendChain)
{
    auto* unused = create("Unused", T::Bus, master);
    unused->comp<VMixNode>()->sends.push_back({b2->objectID, 0.5f});
    BlueprintCanvasWidget canvas;
    canvas.focusItem(unused);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(graph.ok());
    EXPECT_EQ(graph.paths.size(), 6u);
    const auto unusedIndex = indexOf(graph, unused);
    EXPECT_EQ(std::count_if(graph.edges.begin(), graph.edges.end(), [&](const auto& edge) {
        return edge.from.occurrenceIndex == unusedIndex && edge.kind == SignalPathEdgeKind::Output
            && graph.nodes[edge.to.occurrenceIndex].key.objectGuid == master->guid;
    }), 1);
    EXPECT_EQ(std::count_if(graph.edges.begin(), graph.edges.end(), [&](const auto& edge) {
        return edge.from.occurrenceIndex == unusedIndex && edge.kind == SignalPathEdgeKind::Send
            && graph.nodes[edge.to.occurrenceIndex].key.objectGuid == b2->guid;
    }), 1);
    for (size_t index = 0; index < graph.nodes.size(); ++index) {
        const auto* node = canvas.signalPathNode(index);
        ASSERT_NE(node, nullptr);
        EXPECT_TRUE(std::isfinite(node->pos().x()));
        EXPECT_TRUE(std::isfinite(node->pos().y()));
    }
}

TEST_F(SignalPathObservationTest, ClosingProjectCancelsPendingRefresh)
{
    BlueprintCanvasWidget canvas;
    canvas.focusItem(l3);
    canvas.focusItem(c1);
    emit ProjectManager::instance().itemPropertyChanged(c1, QStringLiteral("volume"));
    ProjectManager::instance().closeProject();
    drainEvents();
    EXPECT_TRUE(canvas.scene()->items().empty());
}

TEST_F(SignalPathObservationTest, DoubleClickSelectsSharedSoundWithoutChangingGraphOrZoom)
{
    place(t1, s1);
    BlueprintCanvasWidget canvas;
    canvas.resize(1000, 650);
    canvas.focusItem(t1);
    canvas.setZoomFactor(0.8);
    const auto transform = canvas.transform();
    uint32_t inspected = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeDoubleClicked, &canvas,
        [&](uint32_t id) { inspected = id; });
    const auto* node = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s1));
    const auto body = node->shape().boundingRect();
    const QPointF position = canvas.mapFromScene(node->mapToScene(QPointF(body.center().x(), body.bottom() - 2)));
    QMouseEvent event(QEvent::MouseButtonDblClick, position,
        canvas.viewport()->mapToGlobal(position.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas.viewport(), &event);
    EXPECT_EQ(canvas.activeSignalPathSource()->graph().paths.size(), 7u);
    EXPECT_EQ(canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s1)), node);
    EXPECT_EQ(canvas.transform(), transform);
    EXPECT_EQ(inspected, s1->objectID);
    EXPECT_FALSE(canvas.selectedPlacementContext());
}

TEST_F(SignalPathObservationTest, MovingContentRebuildsItsInheritedRoutingAndLayerPath)
{
    BlueprintCanvasWidget canvas;
    canvas.focusItem(l3);
    ASSERT_TRUE(ProjectManager::instance().moveItem(s4, l2));
    drainEvents();
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(graph.ok());
    const auto& moved = path(graph, s4);
    QStringList names;
    for (const auto endpoint : moved.computation) names.push_back(graph.nodes[endpoint.occurrenceIndex].name);
    EXPECT_EQ(names, (QStringList{"S4", "L2", "L3"}));
    EXPECT_TRUE(moved.routing.sends.empty());
    EXPECT_EQ(moved.routing.outputBusID, b1->objectID);
    UndoRedoManager::instance().undoStack()->undo();
    drainEvents();
    const auto& restored = canvas.activeSignalPathSource()->graph();
    EXPECT_EQ(path(restored, s4).routing.sends.size(), 1u);
    EXPECT_EQ(path(restored, s4).computation.size(), 4u);
}

TEST_F(SignalPathObservationTest, MainWindowSelectionKeepsTheWholeGraphAndInspectorPlacement)
{
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    window.openOrFocusInspectorForItem(l3);
    auto* router = window.findChild<SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    router->routeSelection(l3, false);
    ASSERT_NE(canvas->activeSignalPathSource(), nullptr);
    ASSERT_EQ(canvas->activeSignalPathSource()->graph().paths.size(), 6u);
    const auto use = place(t1, s1);
    emit ProjectManager::instance().itemPropertyChanged(t1, QStringLiteral("clips"));
    drainEvents();
    canvas->setZoomFactor(0.7);
    const auto transform = canvas->transform();
    const auto center = canvas->mapToScene(canvas->viewport()->rect().center());
    auto* inspector = window.activeInspectorPanel();
    ASSERT_NE(inspector, nullptr);
    auto* node = canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), s1));
    ASSERT_NE(node, nullptr);
    canvas->scene()->clearSelection();
    node->setSelected(true);
    EXPECT_EQ(inspector->placementContext(), nullptr);
    EXPECT_EQ(canvas->activeSignalPathSource()->graph().paths.size(), 7u);
    router->routeSelection(b2, false);
    ASSERT_NE(canvas->activeSignalPathSource(), nullptr);
    EXPECT_EQ(canvas->activeSignalPathSource()->graph().paths.size(), 7u);
    const auto placement = signalPathPlacementContext({use});
    router->selectItem(s1, false, placement);
    EXPECT_EQ(canvas->activeSignalPathSource()->graph().paths.size(), 7u);
    ASSERT_NE(inspector->placementContext(), nullptr);
    EXPECT_EQ(*inspector->placementContext(), placement);
    router->routeSelection(nullptr, false);
    EXPECT_EQ(canvas->activeSignalPathSource()->graph().paths.size(), 7u);
    EXPECT_EQ(canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), s1)), node);
    EXPECT_EQ(canvas->transform(), transform);
    EXPECT_EQ(canvas->mapToScene(canvas->viewport()->rect().center()), center);
}

TEST_F(SignalPathObservationTest, DoubleClickReopensInspectorForSharedSoundWithoutNavigating)
{
    place(t1, s1);
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    window.resize(1500, 900);
    window.show();
    for (auto* panel : window.allInspectorPanels()) window.closeInspectorPanel(panel->panelId());
    drainEvents();
    EXPECT_TRUE(window.allInspectorPanels().empty());
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    auto* blueprintDock = window.findChild<ads::CDockWidget*>(QStringLiteral("BlueprintCanvasDock"));
    ASSERT_NE(blueprintDock, nullptr);
    window.revealDock(blueprintDock);
    drainEvents();
    canvas->setZoomFactor(0.75);
    const auto transform = canvas->transform();
    const auto doubleClick = [&]() {
        auto* node = canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), s1));
        canvas->centerOn(node);
        const auto center = canvas->mapToScene(canvas->viewport()->rect().center());
        const auto body = node->shape().boundingRect();
        const auto position = canvas->mapFromScene(node->mapToScene(QPointF(body.center().x(), body.bottom() - 2)));
        sendMouse(*canvas, QEvent::MouseButtonDblClick, position, Qt::LeftButton, Qt::LeftButton);
        drainEvents();
        EXPECT_EQ(canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), s1)), node);
        EXPECT_LT(QLineF(canvas->mapToScene(canvas->viewport()->rect().center()), center).length(), 2.0);
    };
    doubleClick();
    ASSERT_EQ(window.allInspectorPanels().size(), 1);
    auto* inspector = window.activeInspectorPanel();
    ASSERT_NE(inspector, nullptr);
    EXPECT_EQ(inspector->currentContextItem(), s1);
    EXPECT_EQ(inspector->placementContext(), nullptr);
    EXPECT_EQ(canvas->transform(), transform);
    auto* dock = window.dockOfPanel(inspector);
    ASSERT_NE(dock, nullptr);
    dock->toggleView(false);
    EXPECT_TRUE(dock->isClosed());
    doubleClick();
    EXPECT_FALSE(dock->isClosed());
    EXPECT_EQ(window.allInspectorPanels().size(), 1);
    EXPECT_EQ(canvas->transform(), transform);
}

TEST_F(SignalPathObservationTest, BlueprintDoubleClickOpensTimelineThenInspectorAndReusesClosedPanels)
{
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    window.resize(1800, 1000);
    window.show();
    drainEvents();
    ASSERT_TRUE(window.allInspectorPanels().isEmpty());
    ASSERT_TRUE(window.allTimelineEditorPanels().isEmpty());
    window.blueprintCanvas()->setZoomFactor(0.75);
    auto* manager = window.findChild<ads::CDockManager*>();
    ASSERT_NE(manager, nullptr);
    QStringList opened;
    QObject::connect(manager, &ads::CDockManager::dockWidgetAdded, &window,
                     [&](ads::CDockWidget* dock) { opened.append(dock->objectName()); });
    TimelineEditorPanel* firstTimeline = nullptr;
    for (auto* item : {t1, p1, t1}) {
        SCOPED_TRACE(item->name.toStdString());
        doubleClickNode(window, item);
        ASSERT_EQ(window.allTimelineEditorPanels().size(), 1);
        auto* timeline = window.allTimelineEditorPanels().front();
        if (!firstTimeline) firstTimeline = timeline;
        EXPECT_EQ(timeline, firstTimeline);
        EXPECT_EQ(timeline->boundItem(), p1);
        EXPECT_EQ(timeline->viewKind(), TimelineEditorPanel::ViewKind::Piece);
        if (item == t1)
            EXPECT_EQ(timeline->laneGuid(timeline->headerSelectedLane()), t1->guid);
        auto* timelineDock = window.dockOfPanel(timeline);
        ASSERT_NE(timelineDock, nullptr);
        EXPECT_FALSE(timelineDock->isClosed());
        EXPECT_FALSE(timelineDock->isInFloatingContainer());
        auto* inspector = window.activeInspectorPanel();
        ASSERT_NE(inspector, nullptr);
        EXPECT_EQ(inspector->currentContextItem(), item);
        auto* inspectorDock = window.dockOfPanel(inspector);
        ASSERT_NE(inspectorDock, nullptr);
        EXPECT_FALSE(inspectorDock->isClosed());
        EXPECT_FALSE(inspectorDock->isInFloatingContainer());
        EXPECT_EQ(opened.mid(0, 2), (QStringList{
            QStringLiteral("TimelineEditorDock_1"), QStringLiteral("InspectorDock_1")}));
        timelineDock->toggleView(false);
        inspectorDock->toggleView(false);
        drainEvents();
    }
    EXPECT_EQ(window.allInspectorPanels().size(), 2);
}

TEST_F(SignalPathObservationTest, BlueprintDoubleClickKeepsPinnedEditorsOnTheirObjects)
{
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    window.resize(1800, 1000);
    window.show();
    drainEvents();
    window.blueprintCanvas()->setZoomFactor(0.75);
    doubleClickNode(window, t1);
    ASSERT_EQ(window.allTimelineEditorPanels().size(), 1);
    auto* pinnedTimeline = window.allTimelineEditorPanels().front();
    auto* pinnedInspector = window.activeInspectorPanel();
    ASSERT_NE(pinnedInspector, nullptr);
    pinnedTimeline->setPinned(true);
    pinnedInspector->setPinned(true);

    doubleClickNode(window, p1);
    EXPECT_EQ(pinnedTimeline->boundItem(), p1);
    EXPECT_EQ(pinnedInspector->currentContextItem(), t1);
    EXPECT_TRUE(pinnedTimeline->isPinned());
    EXPECT_TRUE(pinnedInspector->isPinned());
    ASSERT_EQ(window.allTimelineEditorPanels().size(), 1);
    ASSERT_EQ(window.allInspectorPanels().size(), 2);
    EXPECT_EQ(window.allTimelineEditorPanels().front(), pinnedTimeline);
    auto* inspector = window.activeInspectorPanel();
    ASSERT_NE(inspector, nullptr);
    EXPECT_NE(inspector, pinnedInspector);
    EXPECT_EQ(inspector->currentContextItem(), p1);
}

TEST_F(SignalPathObservationTest, SelectedTitleSlowClickRenamesInPlaceAndSynchronizesHierarchyUndo)
{
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    window.resize(1500, 900);
    window.show();
    auto* hierarchy = window.createHierarchyPanel();
    ASSERT_NE(hierarchy, nullptr);
    for (auto* panel : window.allInspectorPanels()) window.closeInspectorPanel(panel->panelId());
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    auto* dock = window.findChild<ads::CDockWidget*>(QStringLiteral("BlueprintCanvasDock"));
    ASSERT_NE(dock, nullptr);
    window.revealDock(dock);
    canvas->setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
    canvas->setZoomFactor(1.0);
    window.activateWindow();
    canvas->setFocus(Qt::OtherFocusReason);
    canvas->scene()->setFocus(Qt::OtherFocusReason);
    drainEvents();
    for (auto* object : {s1, c1, b1}) {
        SCOPED_TRACE(object->name.toStdString());
        const auto oldName = object->name;
        const auto hierarchyLabel = [&](const QString& name) {
            return object->isDirtyObject() ? name + QStringLiteral(" *") : name;
        };
        const auto nodeFor = [&]() {
            return canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), object));
        };
        ASSERT_TRUE(hierarchy->selectProjectItem(object));
        const auto original = nodeFor()->pos();
        const auto open = [&]() {
            auto* node = nodeFor();
            node->setSelected(true);
            canvas->centerOn(node);
            const auto icon = canvas->mapFromScene(node->mapToScene(QPointF(20, 12)));
            sendMouse(*canvas, QEvent::MouseButtonPress, icon, Qt::LeftButton, Qt::LeftButton);
            sendMouse(*canvas, QEvent::MouseButtonRelease, icon, Qt::LeftButton, Qt::NoButton);
            const auto title = canvas->mapFromScene(node->mapToScene(node->titleRect().center()));
            sendMouse(*canvas, QEvent::MouseButtonPress, title, Qt::LeftButton, Qt::LeftButton);
            sendMouse(*canvas, QEvent::MouseButtonRelease, title, Qt::LeftButton, Qt::NoButton);
            QEventLoop loop;
            QTimer::singleShot(QApplication::doubleClickInterval() + 100, &loop, &QEventLoop::quit);
            loop.exec();
            return nodeFor()->titleEditor();
        };
        auto* input = open();
        ASSERT_NE(input, nullptr);
        ASSERT_TRUE(nodeFor()->isTitleEditing());
        ASSERT_TRUE(input->isVisible());
        EXPECT_EQ(input->text(), oldName);
        EXPECT_TRUE(window.allInspectorPanels().empty());
        const auto renamed = QStringLiteral("Edited ") + oldName;
        input->setText(renamed);
        const auto point = [&](QPointF position) {
            return canvas->mapFromScene(input->graphicsProxyWidget()->mapToScene(position));
        };
        const auto start = point(QPointF(3, input->height() / 2));
        const auto end = point(QPointF(input->width() - 3, input->height() / 2));
        sendMouse(*canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        sendMouse(*canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        sendMouse(*canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        EXPECT_FALSE(input->selectedText().isEmpty());
        EXPECT_EQ(nodeFor()->pos(), original);
        EXPECT_EQ(object->name, oldName);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(input, &enter);
        drainEvents();
        EXPECT_EQ(object->name, renamed);
        EXPECT_EQ(nodeFor()->title(), renamed);
        EXPECT_FALSE(nodeFor()->isTitleEditing());
        ASSERT_NE(hierarchy->treeWidget()->currentItem(), nullptr);
        EXPECT_EQ(hierarchy->treeWidget()->currentItem()->text(0), hierarchyLabel(renamed));
        EXPECT_TRUE(window.allInspectorPanels().empty());
        ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        drainEvents();
        EXPECT_EQ(object->name, oldName);
        EXPECT_EQ(nodeFor()->title(), oldName);
        EXPECT_EQ(hierarchy->treeWidget()->currentItem()->text(0), hierarchyLabel(oldName));
        ASSERT_TRUE(UndoRedoManager::instance().redo());
        drainEvents();
        EXPECT_EQ(object->name, renamed);
        EXPECT_EQ(hierarchy->treeWidget()->currentItem()->text(0), hierarchyLabel(renamed));
        input = open();
        ASSERT_NE(input, nullptr);
        input->setText(QStringLiteral("Discarded Name"));
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(input, &escape);
        drainEvents();
        EXPECT_EQ(object->name, renamed);
        EXPECT_FALSE(nodeFor()->isTitleEditing());
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
        input = open();
        ASSERT_NE(input, nullptr);
        const auto focusName = QStringLiteral("Focus ") + oldName;
        input->setText(focusName);
        EXPECT_EQ(canvas->scene()->focusItem(), input->graphicsProxyWidget());
        canvas->scene()->setFocusItem(nullptr, Qt::OtherFocusReason);
        drainEvents();
        EXPECT_EQ(object->name, focusName);
        EXPECT_EQ(nodeFor()->title(), focusName);
        EXPECT_EQ(hierarchy->treeWidget()->currentItem()->text(0), hierarchyLabel(focusName));
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 2);
        EXPECT_TRUE(window.allInspectorPanels().empty());
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        drainEvents();
        EXPECT_EQ(object->name, oldName);
        UndoRedoManager::instance().clear();
    }
}

TEST_F(SignalPathObservationTest, BackgroundHeaderWhitespaceAndFieldLabelsKeepInspectorDoubleClick)
{
    BlueprintCanvasWidget canvas;
    canvas.resize(1200, 800);
    canvas.setNodeContentFields(kAllNodeContentFields);
    canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
    canvas.setZoomFactor(1.0);
    canvas.show();
    drainEvents();
    uint32_t inspected = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeDoubleClicked, &canvas,
        [&](uint32_t id) { inspected = id; });
    for (auto* object : {s1, c1, b1}) {
        auto* node = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), object));
        auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget());
        auto* label = qobject_cast<QGridLayout*>(editor->layout())->itemAtPosition(0, 0)->widget();
        canvas.centerOn(node);
        const auto body = node->shape().boundingRect();
        const std::array points{
            node->mapToScene(QPointF(body.center().x(), body.bottom() - 2)),
            node->mapToScene(QPointF(body.right() - 24, 12)),
            editor->graphicsProxyWidget()->mapToScene(label->mapTo(editor, label->rect().center()))};
        const auto original = node->pos();
        for (const auto& scenePoint : points) {
            inspected = 0;
            const auto point = canvas.mapFromScene(scenePoint);
            sendMouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
            sendMouse(canvas, QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
            sendMouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
            EXPECT_EQ(inspected, object->objectID);
            EXPECT_FALSE(node->isTitleEditing());
            EXPECT_FALSE(editor->isEditing());
            EXPECT_EQ(node->pos(), original);
        }
    }
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathObservationTest, HierarchySelectionMirrorsUniqueObjectsWithoutMovingTheCanvas)
{
    place(t1, s1);
    auto* variable = create("SelectionVariable", T::Variable);
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    auto* hierarchy = window.createHierarchyPanel();
    auto* otherHierarchy = window.createHierarchyPanel();
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->setZoomFactor(0.65);
    const auto transform = canvas->transform();
    const auto center = canvas->mapToScene(canvas->viewport()->rect().center());
    const auto& graph = canvas->activeSignalPathSource()->graph();
    const auto nodeCount = graph.nodes.size();
    ASSERT_TRUE(hierarchy->selectProjectItem(s1));
    ASSERT_EQ(count(graph, s1), 1u);
    EXPECT_EQ(canvas->scene()->selectedItems().size(), count(graph, s1));
    EXPECT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{s1}));
    EXPECT_EQ(otherHierarchy->selectedItems(), (QList<ProjectItem*>{s1}));
    for (size_t index = 0; index < graph.nodes.size(); ++index)
        EXPECT_EQ(canvas->signalPathNode(index)->isSelected(), graph.nodes[index].objectID == s1->objectID);

    ASSERT_TRUE(hierarchy->selectProjectItem(l1));
    EXPECT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{l1}));
    EXPECT_EQ(canvas->scene()->selectedItems().size(), count(graph, l1));
    ASSERT_TRUE(hierarchy->selectProjectItem(b2));
    EXPECT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{b2}));
    EXPECT_EQ(otherHierarchy->selectedItems(), (QList<ProjectItem*>{b2}));

    ASSERT_TRUE(hierarchy->selectProjectItem(variable));
    EXPECT_TRUE(canvas->scene()->selectedItems().empty());
    EXPECT_EQ(otherHierarchy->selectedItems(), (QList<ProjectItem*>{variable}));
    EXPECT_EQ(canvas->activeSignalPathSource()->graph().nodes.size(), nodeCount);
    EXPECT_EQ(canvas->transform(), transform);
    EXPECT_EQ(canvas->mapToScene(canvas->viewport()->rect().center()), center);
}

TEST_F(SignalPathObservationTest, HierarchySelectionFocusesExactPathsAndClearingRestoresTheGraph)
{
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    auto* hierarchy = window.createHierarchyPanel();
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    const auto& graph = canvas->activeSignalPathSource()->graph();
    const auto node = [&](ProjectItem* item) { return canvas->signalPathNode(indexOf(graph, item)); };
    const auto outputToB3 = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const auto& edge) {
        return edge.kind == SignalPathEdgeKind::Output
            && graph.nodes[edge.from.occurrenceIndex].key.objectGuid == l3->guid
            && graph.nodes[edge.to.occurrenceIndex].key.objectGuid == b3->guid;
    });
    ASSERT_NE(outputToB3, graph.edges.end());
    auto* unrelatedOutput = canvas->signalPathEdge(static_cast<size_t>(outputToB3 - graph.edges.begin()));
    const auto sendToB2 = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const auto& edge) {
        return edge.kind == SignalPathEdgeKind::Send
            && graph.nodes[edge.from.occurrenceIndex].key.objectGuid == l3->guid
            && graph.nodes[edge.to.occurrenceIndex].key.objectGuid == b2->guid;
    });
    ASSERT_NE(sendToB2, graph.edges.end());
    auto* relatedSend = canvas->signalPathEdge(static_cast<size_t>(sendToB2 - graph.edges.begin()));
    ASSERT_TRUE(hierarchy->selectProjectItem(s1));
    auto* soundRow = hierarchy->treeWidget()->currentItem();
    ASSERT_NE(soundRow, nullptr);
    ASSERT_TRUE(hierarchy->selectProjectItem(s4));
    for (auto* item : {s4, c1, l1, l3, b1, b2, master})
        EXPECT_DOUBLE_EQ(node(item)->opacity(), 1.0);
    for (auto* item : {s1, s2, s3, s5, s6, b3}) EXPECT_LT(node(item)->opacity(), 1.0);
    EXPECT_DOUBLE_EQ(relatedSend->opacity(), 1.0);
    EXPECT_LT(unrelatedOutput->opacity(), 1.0);

    hierarchy->treeWidget()->setCurrentItem(soundRow, 0, QItemSelectionModel::NoUpdate);
    soundRow->setSelected(true);
    EXPECT_EQ(canvas->selectedProjectItems().size(), 2);
    for (auto* item : {s1, t1, p1, s4}) EXPECT_DOUBLE_EQ(node(item)->opacity(), 1.0);
    for (auto* item : {s2, s3, s5, s6}) EXPECT_LT(node(item)->opacity(), 1.0);
    EXPECT_LT(unrelatedOutput->opacity(), 1.0);

    hierarchy->selectNone();
    EXPECT_TRUE(canvas->selectedProjectItems().empty());
    for (size_t index = 0; index < graph.nodes.size(); ++index)
        EXPECT_DOUBLE_EQ(canvas->signalPathNode(index)->opacity(), 1.0);
    for (size_t index = 0; index < graph.edges.size(); ++index)
        EXPECT_DOUBLE_EQ(canvas->signalPathEdge(index)->opacity(), 1.0);
}

TEST_F(SignalPathObservationTest, MainWindowConnectionSelectionFocusesPathsWithoutSelectingHierarchyObjects)
{
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    window.resize(1500, 900);
    window.show();
    auto* hierarchy = window.createHierarchyPanel();
    auto* otherHierarchy = window.createHierarchyPanel();
    window.openOrFocusInspectorForItem(s5);
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    auto* dock = window.findChild<ads::CDockWidget*>(QStringLiteral("BlueprintCanvasDock"));
    ASSERT_NE(dock, nullptr);
    window.revealDock(dock);
    canvas->setFlowDirection(CanvasFlowDirection::LR);
    canvas->setZoomFactor(1.0);
    drainEvents();
    ASSERT_TRUE(hierarchy->selectProjectItem(s5));
    const auto& graph = canvas->activeSignalPathSource()->graph();
    auto* connection = canvas->signalPathEdge(routingEdge(graph, SignalPathEdgeKind::Output, c1, b1));
    ASSERT_NE(connection, nullptr);
    std::optional<QPoint> position;
    for (int sample = 1; sample < 20; ++sample) {
        const auto scenePoint = connection->mapToScene(connection->path().pointAtPercent(sample / 20.0));
        canvas->centerOn(scenePoint);
        const auto candidate = canvas->mapFromScene(scenePoint);
        if (canvas->viewport()->rect().contains(candidate) && canvas->itemAt(candidate) == connection) {
            position = candidate;
            break;
        }
    }
    ASSERT_TRUE(position);
    sendMouse(*canvas, QEvent::MouseButtonPress, *position, Qt::LeftButton, Qt::LeftButton);
    sendMouse(*canvas, QEvent::MouseButtonRelease, *position, Qt::LeftButton, Qt::NoButton);
    drainEvents();
    EXPECT_TRUE(connection->isSelected());
    EXPECT_EQ(canvas->scene()->selectedItems().size(), 3);
    EXPECT_TRUE(canvas->selectedProjectItems().empty());
    EXPECT_TRUE(hierarchy->selectedItems().empty());
    EXPECT_TRUE(otherHierarchy->selectedItems().empty());
    EXPECT_EQ(window.activeInspectorPanel()->currentContextItem(), nullptr);
    const auto node = [&](ProjectItem* item) { return canvas->signalPathNode(indexOf(graph, item)); };
    for (auto* item : {s4, c1, l1, l3, b1, b2, master})
        EXPECT_DOUBLE_EQ(node(item)->opacity(), 1.0);
    for (auto* item : {s1, s2, s3, s5, s6, b3}) EXPECT_LT(node(item)->opacity(), 1.0);

    auto* selectedNode = node(s1);
    canvas->centerOn(selectedNode);
    const auto header = canvas->mapFromScene(selectedNode->mapToScene(
        QPointF(selectedNode->boundingRect().width() / 2, 12)));
    sendMouse(*canvas, QEvent::MouseButtonPress, header, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    sendMouse(*canvas, QEvent::MouseButtonRelease, header, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
    drainEvents();
    EXPECT_TRUE(connection->isSelected());
    EXPECT_TRUE(selectedNode->isSelected());
    EXPECT_EQ(canvas->scene()->selectedItems().size(), 4);
    EXPECT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{s1}));
    EXPECT_EQ(hierarchy->selectedItems(), (QList<ProjectItem*>{s1}));
    EXPECT_EQ(otherHierarchy->selectedItems(), (QList<ProjectItem*>{s1}));
    EXPECT_EQ(window.activeInspectorPanel()->currentContextItem(), s1);
    for (auto* item : {s1, t1, p1, s4, c1, l1, l3, b1, b2, master})
        EXPECT_DOUBLE_EQ(node(item)->opacity(), 1.0);
    for (auto* item : {s2, s3, s5, s6, b3}) EXPECT_LT(node(item)->opacity(), 1.0);
    canvas->scene()->clearSelection();
    EXPECT_TRUE(hierarchy->selectedItems().empty());
    EXPECT_TRUE(otherHierarchy->selectedItems().empty());
    for (size_t index = 0; index < graph.nodes.size(); ++index)
        EXPECT_DOUBLE_EQ(canvas->signalPathNode(index)->opacity(), 1.0);
}

TEST_F(SignalPathObservationTest, BlueprintMultiSelectionMirrorsUniqueObjectsAndClearsBothViews)
{
    place(t1, s1);
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    auto* hierarchy = window.createHierarchyPanel();
    auto* otherHierarchy = window.createHierarchyPanel();
    window.openOrFocusInspectorForItem(c1);
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->scene()->clearSelection();
    const auto& graph = canvas->activeSignalPathSource()->graph();
    auto* first = canvas->signalPathNode(indexOf(graph, s1));
    auto* container = canvas->signalPathNode(indexOf(graph, c1));
    first->setSelected(true);
    EXPECT_EQ(count(graph, s1), 1u);
    EXPECT_EQ(hierarchy->selectedItems(), (QList<ProjectItem*>{s1}));
    EXPECT_EQ(otherHierarchy->selectedItems(), (QList<ProjectItem*>{s1}));
    EXPECT_EQ(window.activeInspectorPanel()->placementContext(), nullptr);
    container->setSelected(true);
    EXPECT_EQ(canvas->scene()->selectedItems().size(), 2);
    EXPECT_EQ(hierarchy->selectedItems().size(), 2);
    EXPECT_TRUE(hierarchy->selectedItems().contains(s1));
    EXPECT_TRUE(hierarchy->selectedItems().contains(c1));
    EXPECT_EQ(otherHierarchy->selectedItems().size(), 2);

    first->setSelected(false);
    EXPECT_TRUE(container->isSelected());
    EXPECT_EQ(hierarchy->selectedItems(), (QList<ProjectItem*>{c1}));
    canvas->scene()->clearSelection();
    EXPECT_TRUE(hierarchy->selectedItems().empty());
    EXPECT_EQ(hierarchy->selectedItem(), nullptr);
    EXPECT_TRUE(otherHierarchy->selectedItems().empty());
    EXPECT_EQ(window.activeInspectorPanel()->currentContextItem(), nullptr);
}

TEST_F(SignalPathObservationTest, HierarchyKeyboardAndControlSelectionKeepTheBlueprintInSync)
{
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    auto* hierarchy = window.createHierarchyPanel();
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    ASSERT_TRUE(hierarchy->selectProjectItem(c1));
    auto* containerRow = hierarchy->treeWidget()->currentItem();
    ASSERT_NE(containerRow, nullptr);
    ASSERT_TRUE(hierarchy->selectProjectItem(s1));
    auto* tree = hierarchy->treeWidget();
    tree->setCurrentItem(containerRow, 0, QItemSelectionModel::NoUpdate);
    containerRow->setSelected(true);
    EXPECT_EQ(hierarchy->selectedItems().size(), 2);
    EXPECT_EQ(canvas->selectedProjectItems().size(), 2);
    EXPECT_TRUE(canvas->selectedProjectItems().contains(s1));
    EXPECT_TRUE(canvas->selectedProjectItems().contains(c1));

    containerRow->setSelected(false);
    ASSERT_TRUE(QMetaObject::invokeMethod(hierarchy, "onItemClicked", Qt::DirectConnection,
        Q_ARG(QTreeWidgetItem*, containerRow), Q_ARG(int, 0)));
    EXPECT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{s1}));
    tree->setCurrentItem(containerRow);
    EXPECT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{c1}));
    hierarchy->selectNone();
    EXPECT_TRUE(canvas->selectedProjectItems().empty());
}

TEST_F(SignalPathObservationTest, PassiveSelectionSyncDoesNotOpenInspectorOrTransitionPanels)
{
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    auto* hierarchy = window.createHierarchyPanel();
    for (auto* inspector : window.allInspectorPanels()) window.closeInspectorPanel(inspector->panelId());
    drainEvents();
    ASSERT_TRUE(window.allInspectorPanels().empty());
    const auto transitionPanels = window.allTransitionEditorPanels();
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    ASSERT_TRUE(hierarchy->selectProjectItem(c1));
    EXPECT_TRUE(window.allInspectorPanels().empty());
    EXPECT_EQ(window.allTransitionEditorPanels(), transitionPanels);
    canvas->scene()->clearSelection();
    canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), c1))->setSelected(true);
    EXPECT_EQ(hierarchy->selectedItems(), (QList<ProjectItem*>{c1}));
    EXPECT_TRUE(window.allInspectorPanels().empty());
    EXPECT_EQ(window.allTransitionEditorPanels(), transitionPanels);
}

TEST_F(SignalPathObservationTest, CreatedHierarchyObjectsRemainSelectedAfterQueuedMaterialization)
{
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    auto* hierarchy = window.createHierarchyPanel();
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    for (const auto& [type, name] : {std::pair{T::Sound, "SelectionCreatedSound"},
                                   std::pair{T::Container, "SelectionCreatedContainer"},
                                   std::pair{T::Layer, "SelectionCreatedLayer"}}) {
        auto* created = create(name, type);
        ASSERT_NE(created, nullptr);
        ASSERT_TRUE(hierarchy->selectProjectItem(created));
        EXPECT_EQ(hierarchy->selectedItems(), (QList<ProjectItem*>{created}));
        drainEvents();
        EXPECT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{created}));
        EXPECT_EQ(canvas->scene()->selectedItems().size(),
                  count(canvas->activeSignalPathSource()->graph(), created));
    }
}

TEST_F(SignalPathObservationTest, NewOccurrencesKeepOneSelectedObjectAndPreserveEveryPath)
{
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    auto* hierarchy = window.createHierarchyPanel();
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    ASSERT_TRUE(hierarchy->selectProjectItem(s1));
    place(t1, s1);
    emit ProjectManager::instance().itemPropertyChanged(t1, QStringLiteral("clips"));
    drainEvents();
    const auto selectedCount = count(canvas->activeSignalPathSource()->graph(), s1);
    ASSERT_EQ(selectedCount, 1u);
    EXPECT_EQ(canvas->scene()->selectedItems().size(), selectedCount);

    canvas->scene()->clearSelection();
    auto* selectedNode = canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), s1));
    selectedNode->setSelected(true);
    EXPECT_FALSE(canvas->selectedPlacementContext());
    place(t1, s1);
    emit ProjectManager::instance().itemPropertyChanged(t1, QStringLiteral("clips"));
    drainEvents();
    const auto& graph = canvas->activeSignalPathSource()->graph();
    EXPECT_EQ(count(graph, s1), 1u);
    EXPECT_EQ(std::count_if(graph.paths.begin(), graph.paths.end(),
        [&](const auto& path) { return path.identity.sourceGuid == s1->guid; }), 3);
    EXPECT_EQ(canvas->scene()->selectedItems().size(), 1);
    EXPECT_FALSE(canvas->selectedPlacementContext());
    EXPECT_EQ(hierarchy->selectedItems(), (QList<ProjectItem*>{s1}));
}

TEST_F(SignalPathObservationTest, TreeMaintenancePreservesSelectionAndDeletedObjectsClearIt)
{
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    auto* hierarchy = window.createHierarchyPanel();
    auto* otherHierarchy = window.createHierarchyPanel();
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    auto* explorer = window.findChild<ProjectExplorer*>();
    ASSERT_NE(explorer, nullptr);
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(hierarchy->selectProjectItem(b2));
    ASSERT_TRUE(manager.renameItem(b2, QStringLiteral("SelectionRenamedBus")));
    drainEvents();
    EXPECT_EQ(hierarchy->selectedItems(), (QList<ProjectItem*>{b2}));
    EXPECT_EQ(otherHierarchy->selectedItems(), (QList<ProjectItem*>{b2}));
    EXPECT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{b2}));
    hierarchy->setFilter(ItemTypeCatalog::Bucket::Layers);
    hierarchy->setFilter(ItemTypeCatalog::Bucket::None);
    canvas->setNodeContentFields(NodeContentField::Pitch);
    EXPECT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{b2}));
    EXPECT_EQ(hierarchy->selectedItems(), (QList<ProjectItem*>{b2}));

    auto* created = create("SelectionDeleted", T::Sound);
    ASSERT_TRUE(hierarchy->selectProjectItem(created));
    drainEvents();
    ASSERT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{created}));
    canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), b2))->setSelected(true);
    ASSERT_EQ(hierarchy->selectedItems().size(), 2);
    ASSERT_EQ(explorer->selectedItems().size(), 2);
    ASSERT_TRUE(manager.deleteItem(created));
    drainEvents();
    EXPECT_EQ(hierarchy->selectedItems(), (QList<ProjectItem*>{b2}));
    EXPECT_EQ(otherHierarchy->selectedItems(), (QList<ProjectItem*>{b2}));
    EXPECT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{b2}));
    EXPECT_EQ(explorer->selectedItems(), (QList<ProjectItem*>{b2}));
    auto* onlySelected = create("SelectionDeletedOnly", T::Sound);
    ASSERT_TRUE(hierarchy->selectProjectItem(onlySelected));
    drainEvents();
    ASSERT_TRUE(manager.deleteItem(onlySelected));
    drainEvents();
    EXPECT_TRUE(hierarchy->selectedItems().empty());
    EXPECT_TRUE(otherHierarchy->selectedItems().empty());
    EXPECT_TRUE(canvas->selectedProjectItems().empty());
}

TEST_F(SignalPathObservationTest, TreeDoubleClickRevealsCollapsedObjectsAndRespectsViewportFiltering)
{
    place(t1, s1);
    ActionManager::instance().clearAllForTesting();
    MainWindow window;
    window.resize(1500, 900);
    window.show();
    auto* hierarchy = window.createHierarchyPanel();
    drainEvents();
    ASSERT_NE(hierarchy, nullptr);
    auto* canvas = window.blueprintCanvas();
    ASSERT_NE(canvas, nullptr);
    auto* explorer = window.findChild<ProjectExplorer*>();
    ASSERT_NE(explorer, nullptr);
    for (auto* tree : std::array<ProjectTreePanel*, 2>{hierarchy, explorer}) {
        for (auto* item : {s1, t1, c1, p1, b2, l1}) {
            canvas->setChainVisible(ProjectManager::instance().getMasterBusItem()->objectID, false);
            canvas->setZoomFactor(0.6);
            emit tree->itemDoubleClicked(item);
            drainEvents();
            auto* inspector = window.activeInspectorPanel();
            ASSERT_NE(inspector, nullptr);
            EXPECT_EQ(inspector->currentContextItem(), item);
            EXPECT_EQ(inspector->placementContext(), nullptr);
            EXPECT_DOUBLE_EQ(canvas->transform().m11(), 1.0);
            ASSERT_EQ(canvas->scene()->selectedItems().size(), 1);
            auto* node = qgraphicsitem_cast<NodeItem*>(canvas->scene()->selectedItems().front());
            ASSERT_NE(node, nullptr);
            EXPECT_EQ(node->objectID(), item->objectID);
            EXPECT_LT(QLineF(canvas->mapToScene(canvas->viewport()->rect().center()),
                            node->sceneBoundingRect().center()).length(), 2.0);
        }
    }
    EXPECT_EQ(canvas->selectedProjectItems(), (QList<ProjectItem*>{l1}));
    EXPECT_EQ(canvas->activeSignalPathSource()->graph().paths.size(), 7u);
    canvas->setSoundsFiltered(true);
    canvas->setZoomFactor(0.6);
    const auto hierarchyState = canvas->hiddenHierarchyRoots();
    const auto busState = canvas->hiddenBuses();
    emit explorer->itemDoubleClicked(s1);
    drainEvents();
    EXPECT_FALSE(canvas->signalPathNode(indexOf(canvas->activeSignalPathSource()->graph(), s1))->isVisible());
    EXPECT_TRUE(canvas->soundsFiltered());
    EXPECT_EQ(canvas->hiddenHierarchyRoots(), hierarchyState);
    EXPECT_EQ(canvas->hiddenBuses(), busState);
    EXPECT_DOUBLE_EQ(canvas->transform().m11(), 0.6);
}

class SignalPathPlaybackTest : public SignalPathObservationTest
{
protected:
    void TearDown() override
    {
        AudioEngine::instance().shutdown();
        AssetRegistry::instance().clear();
        SignalPathObservationTest::TearDown();
    }

    void startRuntime()
    {
        auto& engine = AudioEngine::instance();
        engine.shutdown();
        HarkenEditor::Bridge::EngineSessionConfig config;
        config.sampleRate = 48000;
        config.bufferSize = 64;
        config.maxVoices = 16;
        config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
        ASSERT_TRUE(engine.initialize(config));
        ASSERT_TRUE(engine.startOutput());
        auto& pm = ProjectManager::instance();
        const auto root = QFileInfo(pm.currentProject()->projectPath).absolutePath();
        const auto audioPath = QDir(root).filePath(QStringLiteral("AudioSources/path.wav"));
        ASSERT_TRUE(AssetConverter::writeFloatWav(audioPath, std::vector<float>(9600, 0.25f), 1, 48000));
        AssetInfo asset;
        asset.sourceID = 1001;
        asset.absolutePath = audioPath;
        asset.relativePath = QStringLiteral("path.wav");
        asset.metadata.sampleRate = 48000;
        asset.metadata.channels = 1;
        asset.metadata.totalSamples = 9600;
        asset.metadata.durationSec = 0.2;
        ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));
        for (auto* sound : {s1, s2, s3, s4, s5, s6}) {
            HarkenTest::setSharedSoundSource(*sound, asset.sourceID);
            sound->sourcePath = asset.relativePath;
        }
        for (auto* track : {t1, t2, t3})
            for (auto& clip : track->comp<VTrack>()->clips.items) clip.duration = 0;
        pm.syncProjectToRuntime(pm.currentProject());
        for (int attempt = 0; attempt < 32; ++attempt) {
            QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
            drainEvents();
            const auto& coordinator = engine.runtimeSyncCoordinator();
            if (coordinator.barrier(coordinator.currentAuthoringRevision()).state
                == RuntimeSyncCoordinator::RevisionBarrierState::Ready) return;
        }
        FAIL() << "Runtime world did not become ready";
    }

    quint64 play(ProjectItem* item)
    {
        const auto kind = item->type == T::Sound ? 0u : item->type == T::Container ? 1u
            : item->type == T::Track ? 2u : 3u;
        const auto started = AudioEngine::instance().playPreviewPlayable(kind, item->runtimeId());
        EXPECT_EQ(started.status, 0);
        EXPECT_NE(started.handle, 0u);
        return started.handle;
    }

    void pump()
    {
        ASSERT_EQ(Vn_Update(), VN_OK);
        auto* runtime = harken::detail::ActiveEngine();
        ASSERT_NE(runtime, nullptr);
        runtime->nullBackend.Pump(1);
        QMetaObject::invokeMethod(&AudioEngine::instance(), "onMonitorTimer", Qt::DirectConnection);
    }

    bool waitForVoices(size_t expected, int limit = 400)
    {
        for (int block = 0; block < limit; ++block) {
            pump();
            if (AudioEngine::instance().playbackPaths().size() == expected) return true;
        }
        return false;
    }

    void expectPaths(BlueprintCanvasWidget& canvas, const std::vector<size_t>& indices)
    {
        const auto& graph = canvas.activeSignalPathSource()->graph();
        std::vector<bool> expectedEdges(graph.edges.size());
        std::vector<std::vector<bool>> expectedPairs;
        for (const auto& node : graph.nodes) expectedPairs.emplace_back(node.portPairs.size(), false);
        for (size_t index : indices) {
            for (auto endpoint : graph.paths[index].computation)
                expectedPairs[endpoint.occurrenceIndex][endpoint.portPairIndex] = true;
            for (size_t edgeIndex : graph.paths[index].edgeIndices) {
                expectedEdges[edgeIndex] = true;
                for (auto endpoint : {graph.edges[edgeIndex].from, graph.edges[edgeIndex].to})
                    expectedPairs[endpoint.occurrenceIndex][endpoint.portPairIndex] = true;
            }
        }
        for (size_t index = 0; index < expectedEdges.size(); ++index)
            EXPECT_EQ(canvas.signalPathEdge(index)->playbackActive(), expectedEdges[index]) << "Edge " << index;
        for (size_t index = 0; index < expectedPairs.size(); ++index) {
            auto* node = canvas.signalPathNode(index);
            EXPECT_EQ(node->playbackActive(), std::any_of(expectedPairs[index].begin(), expectedPairs[index].end(),
                [](bool active) { return active; })) << node->title().toStdString();
            for (size_t pair = 0; pair < expectedPairs[index].size(); ++pair) {
                EXPECT_EQ(node->inputPin(static_cast<int>(pair))->playbackActive(), expectedPairs[index][pair]);
                EXPECT_EQ(node->outputPin(static_cast<int>(pair))->playbackActive(), expectedPairs[index][pair]);
            }
        }
    }

    size_t pathIndex(const SignalPathGraph& graph, ProjectItem* sound, size_t occurrence = 0)
    {
        for (size_t index = 0; index < graph.paths.size(); ++index)
            if (graph.paths[index].identity.sourceGuid == sound->guid && occurrence-- == 0) return index;
        ADD_FAILURE() << "Missing path";
        return graph.paths.size();
    }
};

TEST_F(SignalPathPlaybackTest, ActualContainerSelectionsHighlightTheirPairedPortsOutputsAndSends)
{
    c1->comp<VNX::VSelection>()->mode = SelectionMode::Sequence;
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    BlueprintCanvasWidget canvas;
    canvas.focusItem(l3);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_EQ(graph.paths.size(), 6u);
    const auto first = pathIndex(graph, s4);
    const auto second = pathIndex(graph, s5);
    canvas.signalPathNode(indexOf(graph, s5))->setSelected(true);
    expectPaths(canvas, {});
    const auto firstHandle = play(c1);
    expectPaths(canvas, {});
    ASSERT_TRUE(waitForVoices(1));
    ASSERT_EQ(AudioEngine::instance().playbackPaths().front().soundId, s4->runtimeId());
    expectPaths(canvas, {first});
    EXPECT_TRUE(canvas.signalPathNode(indexOf(graph, s5))->isSelected());
    EXPECT_EQ(AudioEngine::instance().stopPreviewPlayable(firstHandle, 20), 0);
    ASSERT_TRUE(waitForVoices(0));
    expectPaths(canvas, {});
    const auto secondHandle = play(c1);
    ASSERT_TRUE(waitForVoices(1));
    ASSERT_EQ(AudioEngine::instance().playbackPaths().front().soundId, s5->runtimeId());
    expectPaths(canvas, {second});
    EXPECT_EQ(graph.nodes.size(), 18u);
    EXPECT_EQ(AudioEngine::instance().stopPreviewPlayable(secondHandle), 0);
    ASSERT_TRUE(waitForVoices(0));
    expectPaths(canvas, {});
}

TEST_F(SignalPathPlaybackTest, ConcurrentSoundsRetainSharedEdgesUntilBothHaveStopped)
{
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    BlueprintCanvasWidget canvas;
    canvas.focusItem(l3);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    const auto first = pathIndex(graph, s4);
    const auto second = pathIndex(graph, s5);
    const auto firstHandle = play(s4);
    const auto secondHandle = play(s5);
    ASSERT_TRUE(waitForVoices(2));
    expectPaths(canvas, {first, second});
    ASSERT_EQ(AudioEngine::instance().stopPreviewPlayable(firstHandle), 0);
    ASSERT_TRUE(waitForVoices(1));
    expectPaths(canvas, {second});
    ASSERT_EQ(AudioEngine::instance().stopPreviewPlayable(secondHandle), 0);
    ASSERT_TRUE(waitForVoices(0));
    expectPaths(canvas, {});
}

TEST_F(SignalPathPlaybackTest, RepeatedClipsReactivateTheSameNodeAndHierarchyConnection)
{
    auto& clips = t1->comp<VTrack>()->clips.items;
    clips.erase(clips.begin() + 1);
    const auto secondUse = place(t1, s1);
    clips.back().start = 300000000;
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    BlueprintCanvasWidget canvas;
    canvas.focusItem(t1);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_EQ(count(graph, s1), 1u);
    const auto first = pathIndex(graph, s1);
    const auto second = pathIndex(graph, s1, 1);
    ASSERT_NE(first, second);
    const auto through = hierarchyEdge(graph, s1, t1);
    ASSERT_EQ(graph.edges[through].pathIndices, (std::vector<size_t>{first, second}));
    auto* soundNode = canvas.signalPathNode(indexOf(graph, s1));
    auto* connection = canvas.signalPathEdge(through);
    const auto handle = play(t1);
    ASSERT_TRUE(waitForVoices(1));
    expectPaths(canvas, {first});
    EXPECT_TRUE(soundNode->playbackActive());
    EXPECT_TRUE(connection->playbackActive());
    ASSERT_TRUE(waitForVoices(0));
    expectPaths(canvas, {});
    EXPECT_FALSE(soundNode->playbackActive());
    EXPECT_FALSE(connection->playbackActive());
    ASSERT_TRUE(waitForVoices(1));
    const auto& observed = AudioEngine::instance().playbackPaths().front();
    ASSERT_NE(std::find_if(observed.rows.begin(), observed.rows.end(), [&](const auto& row) {
        return row.stableId == t1->runtimeId() && row.occurrenceOrdinal == secondUse.stableOccurrenceOrdinal;
    }), observed.rows.end());
    expectPaths(canvas, {second});
    EXPECT_EQ(canvas.signalPathNode(indexOf(graph, s1)), soundNode);
    EXPECT_EQ(canvas.signalPathEdge(through), connection);
    EXPECT_TRUE(soundNode->playbackActive());
    EXPECT_TRUE(connection->playbackActive());
    ASSERT_TRUE(waitForVoices(0));
    expectPaths(canvas, {});
    EXPECT_NE(handle, 0u);
}

TEST_F(SignalPathPlaybackTest, RepeatedSelectionEntriesReactivateTheSameNodeAndHierarchyConnection)
{
    PlayablePropertyAuthoringV1::removeContainerChildOccurrences(*c1, s5->guid);
    SelectionEntry repeated;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*c1, s4->guid, {}, &repeated));
    c1->comp<VNX::VSelection>()->mode = SelectionMode::Sequence;
    const auto firstOrdinal = c1->comp<VNX::VSelection>()->selections.shared.occurrenceRows().front().stableOccurrenceOrdinal;
    ASSERT_NE(firstOrdinal, repeated.stableOccurrenceOrdinal);
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    BlueprintCanvasWidget canvas;
    canvas.focusItem(c1);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_EQ(count(graph, s4), 1u);
    const auto first = pathIndex(graph, s4);
    const auto second = pathIndex(graph, s4, 1);
    const auto through = hierarchyEdge(graph, s4, c1);
    ASSERT_EQ(graph.edges[through].pathIndices, (std::vector<size_t>{first, second}));
    auto* soundNode = canvas.signalPathNode(indexOf(graph, s4));
    auto* connection = canvas.signalPathEdge(through);
    for (const auto& [pathIndex, ordinal] : {std::pair{first, firstOrdinal},
             std::pair{second, repeated.stableOccurrenceOrdinal}}) {
        const auto handle = play(c1);
        ASSERT_TRUE(waitForVoices(1));
        const auto& observed = AudioEngine::instance().playbackPaths().front();
        EXPECT_EQ(observed.soundId, s4->runtimeId());
        ASSERT_NE(std::find_if(observed.rows.begin(), observed.rows.end(), [&](const auto& row) {
            return row.stableId == c1->runtimeId() && row.occurrenceOrdinal == ordinal;
        }), observed.rows.end());
        expectPaths(canvas, {pathIndex});
        EXPECT_EQ(canvas.signalPathNode(indexOf(graph, s4)), soundNode);
        EXPECT_EQ(canvas.signalPathEdge(through), connection);
        EXPECT_TRUE(soundNode->playbackActive());
        EXPECT_TRUE(connection->playbackActive());
        ASSERT_EQ(AudioEngine::instance().stopPreviewPlayable(handle), 0);
        ASSERT_TRUE(waitForVoices(0));
        expectPaths(canvas, {});
        EXPECT_FALSE(soundNode->playbackActive());
        EXPECT_FALSE(connection->playbackActive());
    }
}

TEST_F(SignalPathPlaybackTest, DirectSoundHighlightsItsSharedClipHierarchyWithoutOccurrenceIdentity)
{
    place(t1, s1);
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    BlueprintCanvasWidget canvas;
    canvas.focusItem(t1);
    play(s1);
    ASSERT_TRUE(waitForVoices(1));
    const auto& graph = canvas.activeSignalPathSource()->graph();
    expectPaths(canvas, {pathIndex(graph, s1)});
    const auto& observed = AudioEngine::instance().playbackPaths().front();
    EXPECT_TRUE(std::all_of(observed.rows.begin(), observed.rows.end(), [](const auto& row) {
        return row.occurrenceOrdinal == UINT32_MAX;
    }));
    const auto& uses = path(canvas.activeSignalPathSource()->graph(), s1).identity.occurrences;
    const auto placement = signalPathPlacementContext(uses);
    canvas.focusItem(s1, &placement);
    expectPaths(canvas, {pathIndex(canvas.activeSignalPathSource()->graph(), s1)});
}

TEST_F(SignalPathPlaybackTest, DirectSoundHighlightsItsSharedSelectionHierarchyWithoutOccurrenceIdentity)
{
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*c1, s4->guid));
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    BlueprintCanvasWidget canvas;
    canvas.focusItem(c1);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_EQ(count(graph, s4), 1u);
    const auto through = hierarchyEdge(graph, s4, c1);
    ASSERT_EQ(graph.edges[through].pathIndices.size(), 2u);
    const auto handle = play(s4);
    ASSERT_TRUE(waitForVoices(1));
    expectPaths(canvas, {pathIndex(graph, s4)});
    EXPECT_TRUE(canvas.signalPathEdge(through)->playbackActive());
    ASSERT_EQ(AudioEngine::instance().stopPreviewPlayable(handle), 0);
    ASSERT_TRUE(waitForVoices(0));
    expectPaths(canvas, {});
}

TEST_F(SignalPathPlaybackTest, DirectTrackAndContainerIgnoreMissingOuterUsesButRejectUnknownObservedOrdinals)
{
    auto* outer = create("Outer", T::Container, l1);
    for (auto* child : {t1, c1}) {
        ASSERT_TRUE(ProjectManager::instance().moveItem(child, outer));
        ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*outer, child->guid));
    }
    t1->comp<VTrack>()->clips.items.pop_back();
    c1->comp<VNX::VSelection>()->mode = SelectionMode::Sequence;
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    BlueprintCanvasWidget canvas;
    canvas.focusItem(outer);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    for (const auto& [parent, sound] : {std::pair{t1, s1}, std::pair{c1, s4}}) {
        SCOPED_TRACE(parent->name.toStdString());
        const auto first = pathIndex(graph, sound);
        const auto second = pathIndex(graph, sound, 1);
        const auto through = hierarchyEdge(graph, sound, parent);
        ASSERT_EQ(graph.edges[through].pathIndices, (std::vector<size_t>{first, second}));
        const auto handle = play(parent);
        ASSERT_TRUE(waitForVoices(1));
        expectPaths(canvas, {first});
        EXPECT_TRUE(canvas.signalPathEdge(through)->playbackActive());
        auto observations = AudioEngine::instance().playbackPaths();
        const auto& observed = observations.front();
        EXPECT_EQ(observed.soundId, sound->runtimeId());
        EXPECT_TRUE(std::none_of(observed.rows.begin(), observed.rows.end(), [&](const auto& row) {
            return row.stableId == outer->runtimeId() && row.occurrenceOrdinal != UINT32_MAX;
        }));
        auto actualUse = std::find_if(observations.front().rows.begin(), observations.front().rows.end(),
            [&](const auto& row) {
                return row.stableId == parent->runtimeId() && row.occurrenceOrdinal != UINT32_MAX;
            });
        ASSERT_NE(actualUse, observations.front().rows.end());
        actualUse->occurrenceOrdinal = UINT32_MAX - 1;
        const auto rejected = projectSignalPathActivity(*ProjectManager::instance().currentProject(), graph, observations);
        EXPECT_TRUE(std::none_of(rejected.edges.begin(), rejected.edges.end(), [](bool active) { return active; }));
        for (const auto& ports : rejected.portPairs)
            EXPECT_TRUE(std::none_of(ports.begin(), ports.end(), [](bool active) { return active; }));
        ASSERT_EQ(AudioEngine::instance().stopPreviewPlayable(handle), 0);
        ASSERT_TRUE(waitForVoices(0));
        expectPaths(canvas, {});
    }
}

TEST_F(SignalPathPlaybackTest, StaticUnplacedSoundRouteActivatesOnlyWhenThatSoundActuallyPlays)
{
    t1->comp<VTrack>()->clips.items.pop_back();
    auto* emptyLayer = create("EmptyLayer", T::Layer);
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    BlueprintCanvasWidget canvas;
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(graph.ok());
    EXPECT_TRUE(path(graph, s2).identity.occurrences.empty());
    expectRoute(graph, emptyLayer, {"EmptyLayer"}, master);
    expectPaths(canvas, {});
    const auto trackHandle = play(t1);
    ASSERT_TRUE(waitForVoices(1));
    expectPaths(canvas, {pathIndex(graph, s1)});
    EXPECT_FALSE(canvas.signalPathNode(indexOf(graph, s2))->playbackActive());
    EXPECT_FALSE(canvas.signalPathNode(indexOf(graph, emptyLayer))->playbackActive());
    ASSERT_EQ(AudioEngine::instance().stopPreviewPlayable(trackHandle), 0);
    ASSERT_TRUE(waitForVoices(0));
    play(s2);
    ASSERT_TRUE(waitForVoices(1));
    expectPaths(canvas, {pathIndex(graph, s2)});
    EXPECT_TRUE(t1->comp<VTrack>()->clips.items.size() == 1u);
}

TEST_F(SignalPathPlaybackTest, FocusAndRebuildPreserveActivityAndOutputShutdownClearsIt)
{
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    BlueprintCanvasWidget canvas;
    canvas.focusItem(l3);
    play(s5);
    ASSERT_TRUE(waitForVoices(1));
    canvas.focusItem(c1);
    ASSERT_EQ(canvas.activeSignalPathSource()->graph().paths.size(), 6u);
    expectPaths(canvas, {pathIndex(canvas.activeSignalPathSource()->graph(), s5)});
    canvas.scale(1.3, 1.3);
    const auto transform = canvas.transform();
    const auto* before = canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s5));
    pump();
    EXPECT_EQ(canvas.signalPathNode(indexOf(canvas.activeSignalPathSource()->graph(), s5)), before);
    canvas.rebuildFromProject();
    EXPECT_EQ(canvas.transform(), transform);
    expectPaths(canvas, {pathIndex(canvas.activeSignalPathSource()->graph(), s5)});
    canvas.focusItem(l3);
    expectPaths(canvas, {pathIndex(canvas.activeSignalPathSource()->graph(), s5)});
    AudioEngine::instance().stopOutput();
    expectPaths(canvas, {});
    EXPECT_TRUE(AudioEngine::instance().playbackPaths().empty());
}

TEST_F(SignalPathPlaybackTest, NestedContainerAndRepeatedTrackUsesKeepBothOccurrenceIdentities)
{
    auto* outer = create("Outer", T::Container, l1);
    ASSERT_TRUE(ProjectManager::instance().moveItem(t1, outer));
    const auto firstUse = *PlayablePropertyAuthoringV1::containerChildLocator(
        *outer, outer->comp<VNX::VSelection>()->selections.shared.occurrenceRows().front().stableOccurrenceOrdinal);
    auto repeated = outer->comp<VNX::VSelection>()->selections.shared.occurrenceRows().front();
    repeated.stableOccurrenceOrdinal = static_cast<uint32_t>(outer->comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater++);
    auto repeatedRows = outer->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
    repeatedRows.push_back(repeated);
    ASSERT_TRUE(outer->comp<VNX::VSelection>()->selections.shared.setOccurrenceRows(repeatedRows));
    auto& clips = t1->comp<VTrack>()->clips.items;
    clips.erase(clips.begin() + 1);
    place(t1, s1);
    clips.back().start = 300000000;
    outer->comp<VNX::VSelection>()->mode = SelectionMode::Sequence;
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    BlueprintCanvasWidget canvas;
    canvas.focusItem(outer);
    const auto& graph = canvas.activeSignalPathSource()->graph();
    ASSERT_EQ(count(graph, s1), 1u);
    play(outer);
    ASSERT_TRUE(waitForVoices(1));
    const auto& observed = AudioEngine::instance().playbackPaths().front();
    EXPECT_EQ(std::count_if(observed.rows.begin(), observed.rows.end(), [](const auto& row) {
        return row.occurrenceOrdinal != UINT32_MAX;
    }), 2);
    const auto active = std::find_if(graph.paths.begin(), graph.paths.end(), [&](const auto& path) {
        return path.identity.sourceGuid == s1->guid && path.identity.occurrences.back() == firstUse
            && path.identity.occurrences.front().occurrenceGuid == clips.front().clipGuid;
    });
    ASSERT_NE(active, graph.paths.end());
    expectPaths(canvas, {static_cast<size_t>(active - graph.paths.begin())});
}

TEST_F(SignalPathPlaybackTest, ChangedStaticRoutingDoesNotHighlightAnUnplayedOutput)
{
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    play(s5);
    ASSERT_TRUE(waitForVoices(1));
    const auto observations = AudioEngine::instance().playbackPaths();
    output(s5, b1);
    const auto graph = build(l3);
    ASSERT_TRUE(graph.ok());
    const auto activity = projectSignalPathActivity(*ProjectManager::instance().currentProject(), graph, observations);
    const auto terminal = path(graph, s5).computation.back();
    bool checkedOutput = false;
    bool checkedSend = false;
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        if (edge.from != terminal) continue;
        if (edge.kind == SignalPathEdgeKind::Output) {
            EXPECT_FALSE(activity.edges[index]);
            checkedOutput = true;
        } else if (edge.kind == SignalPathEdgeKind::Send) {
            EXPECT_TRUE(activity.edges[index]);
            checkedSend = true;
        }
    }
    EXPECT_TRUE(checkedOutput);
    EXPECT_TRUE(checkedSend);
}

TEST_F(SignalPathPlaybackTest, UnavailableGraphDuringPlaybackClearsItsSceneAndRecovers)
{
    ASSERT_NO_FATAL_FAILURE(startRuntime());
    BlueprintCanvasWidget canvas;
    canvas.focusItem(l3);
    play(s4);
    ASSERT_TRUE(waitForVoices(1));
    const auto saved = s5->fieldGroups.mainOutput;
    s5->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(QUuid::createUuid()), 0.0f}};
    canvas.rebuildFromProject();
    EXPECT_FALSE(canvas.activeSignalPathSource()->graph().ok());
    EXPECT_TRUE(canvas.scene()->items().empty());
    s5->fieldGroups.mainOutput = saved;
    canvas.rebuildFromProject();
    ASSERT_TRUE(canvas.activeSignalPathSource()->graph().ok());
    expectPaths(canvas, {pathIndex(canvas.activeSignalPathSource()->graph(), s4)});
}

}
