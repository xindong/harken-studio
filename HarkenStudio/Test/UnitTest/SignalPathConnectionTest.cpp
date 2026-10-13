#include "Engine.h"
#include "Core/Components/VSelection.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/Blueprint/SignalPathEditing.h"
#include "Widgets/Blueprint/ConnectionPath.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VTrack.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/ThemeManager.h"
#include "Core/ToastManager.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"

#include <QApplication>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QLabel>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolTip>

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>

namespace {

using namespace VNX;
using T = ProjectItemType;
using Gesture = SignalPathConnectGesture;
using Target = SignalPathConnectTarget;
using Feedback = NodeItem::DropFeedback;

void drainConnectionEvents()
{
    QApplication::processEvents();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

class SignalPathConnectionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        for (const auto* key : {"Blueprint/InteractionMode", "Blueprint/CanvasFlowDirection", "Blueprint/GridSnapEnabled"})
            preferences.insert(QString::fromLatin1(key), QSettings().value(QString::fromLatin1(key)));
        QSettings().setValue("Blueprint/InteractionMode", "Select");
        QSettings().setValue("Blueprint/CanvasFlowDirection", "LR");
        QSettings().setValue("Blueprint/GridSnapEnabled", false);
        ProjectConfig config;
        config.name = QStringLiteral("SignalPathConnection");
        config.location = directory.path();
        config.platforms = HarkenTest::platforms({"Windows", "Android"});
        ASSERT_TRUE(manager.createProject(config));
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, true);
        master = manager.getMasterBusItem();
        b1 = create("B1", T::Bus, master);
        b2 = create("B2", T::Bus, master);
        b3 = create("B3", T::Bus, master);
        b4 = create("B4", T::Bus, master);
        layer = create("Layer", T::Layer);
        c1 = create("C1", T::Container, layer);
        c2 = create("C2", T::Container, layer);
        s1 = create("S1", T::Sound, c1);
        s2 = create("S2", T::Sound, c1);
        setOutput(c1, b1, -7.0f);
        setOutput(c2, b3, -2.0f);
        c1->fieldGroups.send.shared = {FieldGroupMode::Local,
            SendFieldGroup{{{toFieldGroupGuid(b2->guid), -6.0f}}}};
        UndoRedoManager::instance().clear();
        drainConnectionEvents();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        drainConnectionEvents();
        for (auto it = preferences.cbegin(); it != preferences.cend(); ++it) {
            if (it.value().isValid()) QSettings().setValue(it.key(), it.value());
            else QSettings().remove(it.key());
        }
        PlatformContextService::instance().setStorageForTesting(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    ProjectItem* create(const char* name, T type, ProjectItem* parent = nullptr)
    {
        auto* item = ProjectManager::instance().createItem(parent, QString::fromLatin1(name), type);
        EXPECT_NE(item, nullptr);
        return item;
    }

    void setOutput(ProjectItem* item, const ProjectItem* bus, float volume = 0.0f)
    {
        item->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
            MainOutputFieldGroup{toFieldGroupGuid(bus->guid), volume}};
    }

    SignalPathGraph graph()
    {
        return buildProjectSignalPathGraph(*ProjectManager::instance().currentProject(),
            HarkenTest::FieldGroupPlatformIds::Windows);
    }

    SignalPathEndpoint endpoint(const SignalPathGraph& value, const ProjectItem* item)
    {
        for (size_t index = 0; index < value.nodes.size(); ++index)
            if (value.nodes[index].key.objectGuid == item->guid) return {index, 0};
        throw std::runtime_error("Expected blueprint object is missing");
    }

    SignalPathConnectPlan prepare(ProjectItem* source, ProjectItem* target,
        Gesture gesture = Gesture::Output, Target part = Target::Input, bool alt = false)
    {
        const auto value = graph();
        EXPECT_TRUE(value.ok());
        return prepareSignalPathConnect(value, {endpoint(value, source), endpoint(value, target), gesture, part, alt});
    }

    bool apply(SignalPathConnectPlan plan)
    {
        EXPECT_TRUE(plan.ok()) << plan.error.toStdString();
        return applySignalPathConnect(std::move(plan));
    }

    MainOutputFieldGroup output(const ProjectItem* item,
        FieldGroupScope scope = HarkenTest::FieldGroupPlatformIds::Windows)
    {
        const auto view = buildFieldGroupProjectView(*ProjectManager::instance().currentProject());
        const FieldGroupResolver resolver(view.definitions, toFieldGroupGuid(master->guid));
        const auto result = resolver.Resolve(toFieldGroupGuid(item->guid), scope, FieldGroupId::MainOutput);
        EXPECT_TRUE(result.ok());
        return std::get<MainOutputFieldGroup>(result.value->payload);
    }

    SendFieldGroup sends(const ProjectItem* item)
    {
        const auto view = buildFieldGroupProjectView(*ProjectManager::instance().currentProject());
        const FieldGroupResolver resolver(view.definitions, toFieldGroupGuid(master->guid));
        const auto result = resolver.Resolve(toFieldGroupGuid(item->guid),
            HarkenTest::FieldGroupPlatformIds::Windows, FieldGroupId::Send);
        EXPECT_TRUE(result.ok());
        return std::get<SendFieldGroup>(result.value->payload);
    }

    void mouse(BlueprintCanvasWidget& canvas, QEvent::Type type, QPoint position,
        Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = {})
    {
        QMouseEvent event(type, QPointF(position), QPointF(canvas.viewport()->mapToGlobal(position)),
            button, buttons, modifiers);
        QApplication::sendEvent(canvas.viewport(), &event);
    }

    NodeItem* node(BlueprintCanvasWidget& canvas, const ProjectItem* item)
    {
        return canvas.signalPathNode(endpoint(canvas.activeSignalPathSource()->graph(), item).occurrenceIndex);
    }

    void show(BlueprintCanvasWidget& canvas)
    {
        canvas.resize(1400, 900);
        canvas.show();
        canvas.zoomToFit();
        drainConnectionEvents();
    }

    QGraphicsPathItem* preview(BlueprintCanvasWidget& canvas)
    {
        for (auto* item : canvas.scene()->items())
            if (item->zValue() == 1000.0)
                return dynamic_cast<QGraphicsPathItem*>(item);
        return nullptr;
    }

    QLabel* hint(BlueprintCanvasWidget& canvas)
    {
        return canvas.viewport()->findChild<QLabel*>(QStringLiteral("blueprintConnectHint"));
    }

    QTemporaryDir directory;
    QMap<QString, QVariant> preferences;
    ProjectItem* master = nullptr;
    ProjectItem* b1 = nullptr;
    ProjectItem* b2 = nullptr;
    ProjectItem* b3 = nullptr;
    ProjectItem* b4 = nullptr;
    ProjectItem* layer = nullptr;
    ProjectItem* c1 = nullptr;
    ProjectItem* c2 = nullptr;
    ProjectItem* s1 = nullptr;
    ProjectItem* s2 = nullptr;
};

TEST_F(SignalPathConnectionTest, ParentGesturesAgreeForBodiesAndPinsAndPreserveAuthoredInheritance)
{
    const auto before = s1->fieldGroups;
    const auto occurrences = c1->comp<VNX::VSelection>()->selections.shared;
    for (const bool reverse : {false, true}) {
        for (const bool body : {false, true}) {
            SCOPED_TRACE(::testing::Message() << reverse << ", " << body);
            ASSERT_TRUE(apply(prepare(reverse ? c2 : s1, reverse ? s1 : c2,
                reverse ? Gesture::Input : Gesture::Output,
                body ? Target::Body : reverse ? Target::Output : Target::Input)));
            EXPECT_EQ(s1->parent, c2);
            EXPECT_EQ(s1->fieldGroups, before);
            EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b3->guid));
            EXPECT_TRUE(sends(s1).sends.empty());
            ASSERT_TRUE(UndoRedoManager::instance().undo());
            EXPECT_EQ(s1->parent, c1);
            EXPECT_EQ(c1->comp<VNX::VSelection>()->selections.shared, occurrences);
            ASSERT_TRUE(UndoRedoManager::instance().redo());
            EXPECT_EQ(s1->parent, c2);
            ASSERT_TRUE(UndoRedoManager::instance().undo());
            UndoRedoManager::instance().clear();
        }
    }
}

TEST_F(SignalPathConnectionTest, ParentChangePreservesLocalRoutesAndDoesNotMatchSiblingPaths)
{
    setOutput(s1, b4, -11.0f);
    const auto before = s1->fieldGroups;
    ASSERT_TRUE(apply(prepare(s1, c2)));
    EXPECT_EQ(s1->fieldGroups, before);
    EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b4->guid));
    EXPECT_FLOAT_EQ(output(s1).outputBusVolume, -11.0f);
    EXPECT_TRUE(sends(s1).sends.empty());
}

TEST_F(SignalPathConnectionTest, OutputGesturesChangeOnlyObjectRoutingAndPreserveItsParent)
{
    for (const bool reverse : {false, true}) {
        for (const bool body : {false, true}) {
            const auto before = s1->fieldGroups;
            const auto originalSends = sends(s1);
            ASSERT_TRUE(apply(prepare(reverse ? b3 : s1, reverse ? s1 : b3,
                reverse ? Gesture::Input : Gesture::Output,
                body ? Target::Body : reverse ? Target::Output : Target::Input)));
            EXPECT_EQ(s1->parent, c1);
            EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b3->guid));
            EXPECT_FLOAT_EQ(output(s1).outputBusVolume, -7.0f);
            EXPECT_EQ(sends(s1), originalSends);
            EXPECT_EQ(output(s2).mainOutputBusRef, toFieldGroupGuid(b1->guid));
            ASSERT_TRUE(UndoRedoManager::instance().undo());
            EXPECT_EQ(s1->fieldGroups, before);
            UndoRedoManager::instance().clear();
        }
    }
}

TEST_F(SignalPathConnectionTest, ContainerOutputChangesOwnerAndOnlyInheritingChildrenFollow)
{
    setOutput(s1, b1);
    const auto first = s1->fieldGroups;
    const auto second = s2->fieldGroups;
    ASSERT_TRUE(apply(prepare(c1, b3)));
    EXPECT_EQ(c1->parent, layer);
    EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b1->guid));
    EXPECT_EQ(output(s2).mainOutputBusRef, toFieldGroupGuid(b3->guid));
    EXPECT_EQ(s1->fieldGroups, first);
    EXPECT_EQ(s2->fieldGroups, second);
}

TEST_F(SignalPathConnectionTest, SendAndReceiveGesturesPreserveInheritedSendsAndOtherFields)
{
    for (const bool reverse : {false, true}) {
        for (const bool body : {false, true}) {
            const auto before = s1->fieldGroups;
            auto plan = prepare(reverse ? b3 : s1, reverse ? s1 : b3,
                reverse ? Gesture::Input : Gesture::Output,
                body ? Target::Body : reverse ? Target::Output : Target::Input, true);
            EXPECT_EQ(plan.description, reverse ? QStringLiteral("Add Receive") : QStringLiteral("Add Send"));
            ASSERT_TRUE(apply(std::move(plan)));
            EXPECT_EQ(s1->parent, c1);
            EXPECT_EQ(s1->fieldGroups.mainOutput, before.mainOutput);
            const auto value = sends(s1);
            ASSERT_EQ(value.sends.size(), 2u);
            EXPECT_FLOAT_EQ(value.sends.front().sendLevel, -6.0f);
            EXPECT_EQ(value.sends.back().targetBusID, toFieldGroupGuid(b3->guid));
            EXPECT_FLOAT_EQ(value.sends.back().sendLevel, 0.0f);
            EXPECT_EQ(sends(s2).sends.size(), 1u);
            EXPECT_FALSE(prepare(s1, b3, Gesture::Output, Target::Body, true).ok());
            ASSERT_TRUE(UndoRedoManager::instance().undo());
            EXPECT_EQ(s1->fieldGroups, before);
            UndoRedoManager::instance().clear();
        }
    }
}

TEST_F(SignalPathConnectionTest, BusGesturesReparentOrAddOneDirectionalSendAndUndo)
{
    for (const bool alt : {false, true}) {
        for (const bool reverse : {false, true}) {
            for (const bool body : {false, true}) {
                ASSERT_TRUE(apply(prepare(reverse ? b2 : b1, reverse ? b1 : b2,
                    reverse ? Gesture::Input : Gesture::Output,
                    body ? Target::Body : reverse ? Target::Output : Target::Input, alt)));
                EXPECT_EQ(b1->parent, alt ? master : b2);
                EXPECT_EQ(b1->comp<VMixNode>()->sends.size(), alt ? 1u : 0u);
                if (alt) {
                    EXPECT_EQ(b1->comp<VMixNode>()->sends.front().targetBusID, b2->objectID);
                    EXPECT_FLOAT_EQ(b1->comp<VMixNode>()->sends.front().sendLevel, 1.0f);
                }
                EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
                ASSERT_TRUE(UndoRedoManager::instance().undo());
                EXPECT_EQ(b1->parent, master);
                EXPECT_TRUE(b1->comp<VMixNode>()->sends.empty());
                ASSERT_TRUE(UndoRedoManager::instance().redo());
                EXPECT_EQ(b1->parent, alt ? master : b2);
                ASSERT_TRUE(UndoRedoManager::instance().undo());
                UndoRedoManager::instance().clear();
            }
        }
    }
}

TEST_F(SignalPathConnectionTest, DirectDropToBusMovesToRootAndPreservesOtherEffectiveGroupsAndReferencesOnUndo)
{
    const auto before = s1->fieldGroups;
    const auto occurrences = c1->comp<VNX::VSelection>()->selections.shared;
    const auto originalSends = sends(s1);
    ASSERT_TRUE(apply(prepare(s1, b3, Gesture::Node, Target::Body)));
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_NE(ProjectManager::findRootBinding(ProjectManager::instance().currentProject(), s1), nullptr);
    EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b3->guid));
    EXPECT_FLOAT_EQ(output(s1).outputBusVolume, -7.0f);
    EXPECT_EQ(sends(s1), originalSends);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(s1->fieldGroups, before);
    EXPECT_EQ(c1->comp<VNX::VSelection>()->selections.shared, occurrences);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b3->guid));
}

TEST_F(SignalPathConnectionTest, MultiDropPreservesOrderAndRestoresAllSelectionReferencesWithOneUndo)
{
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*c1, s1->guid));
    const auto rows = c1->comp<VNX::VSelection>()->selections.shared;
    const auto value = graph();
    ASSERT_TRUE(apply(prepareSignalPathNodeDrop(value,
        {endpoint(value, s1), endpoint(value, s2)}, endpoint(value, c2))));
    EXPECT_EQ(s1->parent, c2);
    EXPECT_EQ(s2->parent, c2);
    EXPECT_EQ(c2->children[0].get(), s1);
    EXPECT_EQ(c2->children[1].get(), s2);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(c1->children[0].get(), s1);
    EXPECT_EQ(c1->children[1].get(), s2);
    EXPECT_EQ(c1->comp<VNX::VSelection>()->selections.shared, rows);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(s2->parent, c2);
}

TEST_F(SignalPathConnectionTest, MultiDropToBusMovesOnlySelectedAncestorsAndKeepsLocalDescendants)
{
    setOutput(s1, b4);
    const auto first = s1->fieldGroups;
    const auto value = graph();
    auto plan = prepareSignalPathNodeDrop(value,
        {endpoint(value, s1), endpoint(value, c1), endpoint(value, s2)}, endpoint(value, b3));
    ASSERT_TRUE(plan.ok()) << plan.error.toStdString();
    ASSERT_EQ(plan.moves.size(), 1u);
    ASSERT_TRUE(apply(std::move(plan)));
    EXPECT_EQ(c1->parent, nullptr);
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(s2->parent, c1);
    EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b4->guid));
    EXPECT_EQ(output(s2).mainOutputBusRef, toFieldGroupGuid(b3->guid));
    EXPECT_EQ(s1->fieldGroups, first);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(c1->parent, layer);
}

TEST_F(SignalPathConnectionTest, ConsecutiveMovesUndoAndRedoAcrossReferenceAllocatorChanges)
{
    const auto originalRows = c1->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
    ASSERT_TRUE(apply(prepare(s1, c2)));
    ASSERT_TRUE(apply(prepare(s2, c2)));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 2);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(s2->parent, c1);
    EXPECT_EQ(c1->comp<VNX::VSelection>()->selections.shared.occurrenceRows(), originalRows);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(s1->parent, c2);
    EXPECT_EQ(s2->parent, c2);
}

TEST_F(SignalPathConnectionTest, DeleteThenReconnectCanUndoAndRedoTheWholeHistory)
{
    const auto original = s1->fieldGroups;
    const auto originalRows = c1->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
    const auto value = graph();
    const auto from = endpoint(value, s1);
    const auto edge = std::find_if(value.edges.begin(), value.edges.end(), [&](const auto& entry) {
        return entry.kind == SignalPathEdgeKind::Through && entry.from == from;
    });
    ASSERT_NE(edge, value.edges.end());
    ASSERT_TRUE(apply(prepareSignalPathDisconnect(value, {static_cast<size_t>(edge - value.edges.begin())})));
    EXPECT_EQ(s1->parent, nullptr);
    ASSERT_TRUE(apply(prepare(s1, c1)));
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, nullptr);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(s1->fieldGroups, original);
    EXPECT_EQ(c1->comp<VNX::VSelection>()->selections.shared.occurrenceRows(), originalRows);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(s1->parent, c1);
}

TEST_F(SignalPathConnectionTest, InvalidMultiDropAndInheritedOutputSendConflictsDoNotPartiallyApply)
{
    const auto value = graph();
    auto plan = prepareSignalPathNodeDrop(value,
        {endpoint(value, s1), endpoint(value, b1)}, endpoint(value, c2));
    EXPECT_FALSE(plan.ok());
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(b1->parent, master);
    EXPECT_FALSE(prepare(s1, b2).ok());
    EXPECT_FALSE(prepare(s1, b1, Gesture::Output, Target::Input, true).ok());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathConnectionTest, InvalidDirectionsSelfLoopsMasterAndBusCyclesAreRejected)
{
    EXPECT_FALSE(prepare(s1, s1).ok());
    EXPECT_FALSE(prepare(s1, b3, Gesture::Input, Target::Input, true).ok());
    EXPECT_FALSE(prepare(s1, b3, Gesture::Input, Target::Body, true).ok());
    EXPECT_FALSE(prepare(s1, b3, Gesture::Input, Target::Output).ok());
    EXPECT_FALSE(prepare(b3, s1, Gesture::Output, Target::Body).ok());
    EXPECT_FALSE(prepare(s1, b3, Gesture::Output, Target::Output).ok());
    EXPECT_FALSE(prepare(master, b1).ok());
    EXPECT_FALSE(prepare(layer, c1).ok());
    ASSERT_TRUE(apply(prepare(b1, b2, Gesture::Output, Target::Input, true)));
    EXPECT_FALSE(prepare(b2, b1).ok());
    EXPECT_FALSE(prepare(b2, b1, Gesture::Output, Target::Input, true).ok());
    EXPECT_FALSE(prepare(b1, b2).ok());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(SignalPathConnectionTest, PlatformEditPreservesOtherPlatformsAndVolumePrecision)
{
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    c1->fieldGroups.mainOutput.shared.localHistory->outputBusVolume = -7.125f;
    ASSERT_TRUE(apply(prepare(s1, b3)));
    EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b3->guid));
    EXPECT_FLOAT_EQ(output(s1).outputBusVolume, -7.125f);
    EXPECT_EQ(output(s1, HarkenTest::FieldGroupPlatformIds::Android).mainOutputBusRef, toFieldGroupGuid(b1->guid));
    EXPECT_EQ(s1->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Inherit);
}

TEST_F(SignalPathConnectionTest, StaleProviderAndReferencePlansFailWithoutUndoEntries)
{
    auto plan = prepare(s1, b3);
    c1->fieldGroups.mainOutput.shared.localHistory->outputBusVolume = -9.0f;
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b1->guid));
    plan = prepare(s1, c2);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*c1, s1->guid));
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathConnectionTest, CanvasAcceptsBodyTargetsAndAltChangesPreviewBeforeRelease)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    const auto start = canvas.mapFromScene(node(canvas, s1)->outputPin()->scenePos());
    const auto end = canvas.mapFromScene(node(canvas, b3)->mapToScene(QPointF(70, 10)));
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    ASSERT_NE(preview(canvas), nullptr);
    ASSERT_NE(hint(canvas), nullptr);
    EXPECT_TRUE(hint(canvas)->isVisible());
    EXPECT_EQ(preview(canvas)->pen().style(), Qt::SolidLine);
    EXPECT_EQ(preview(canvas)->pen().color(), ConnectionPath::defaultColor());
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::Valid);
    EXPECT_TRUE(hint(canvas)->isVisible());
    QKeyEvent alt(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
    QApplication::sendEvent(&canvas, &alt);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::Valid);
    EXPECT_EQ(preview(canvas)->pen().dashPattern(), (QList<qreal>{6, 3}));
    QKeyEvent altRelease(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &altRelease);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::Valid);
    EXPECT_EQ(preview(canvas)->pen().style(), Qt::SolidLine);
    QApplication::sendEvent(&canvas, &alt);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::Valid);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    drainConnectionEvents();
    EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b1->guid));
    EXPECT_EQ(sends(s1).sends.size(), 2u);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::None);
    EXPECT_FALSE(hint(canvas)->isVisible());
    EXPECT_EQ(preview(canvas), nullptr);
}

TEST_F(SignalPathConnectionTest, CanvasReverseInputConnectsToObjectBodyAndEscapeCancels)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    EXPECT_FALSE(canvas.canCollapseChain(b4->objectID));
    EXPECT_FALSE(node(canvas, b4)->inputPin()->chainVisibility().has_value());
    const auto start = canvas.mapFromScene(node(canvas, b4)->inputPin()->scenePos());
    const auto end = canvas.mapFromScene(node(canvas, s1)->mapToScene(QPointF(70, 10)));
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    EXPECT_TRUE(hint(canvas)->isVisible());
    EXPECT_EQ(node(canvas, s1)->dropFeedback(), Feedback::Valid);
    QKeyEvent alt(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
    QApplication::sendEvent(&canvas, &alt);
    EXPECT_EQ(node(canvas, s1)->dropFeedback(), Feedback::Valid);
    EXPECT_EQ(preview(canvas)->pen().dashPattern(), (QList<qreal>{6, 3}));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    EXPECT_EQ(node(canvas, s1)->dropFeedback(), Feedback::None);
    EXPECT_FALSE(hint(canvas)->isVisible());
    EXPECT_EQ(preview(canvas), nullptr);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    drainConnectionEvents();
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b4->guid));
    EXPECT_TRUE(canvas.canCollapseChain(b4->objectID));
    EXPECT_EQ(node(canvas, b4)->inputPin()->chainVisibility(), Qt::Checked);
    canvas.setBusVisible(b4->objectID, false);
    EXPECT_FALSE(node(canvas, s1)->isVisible());
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainConnectionEvents();
    EXPECT_FALSE(canvas.canCollapseChain(b4->objectID));
    EXPECT_FALSE(node(canvas, b4)->inputPin()->chainVisibility().has_value());
    EXPECT_TRUE(canvas.hiddenBuses().isEmpty());
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainConnectionEvents();
    EXPECT_TRUE(node(canvas, s1)->isVisible());
    EXPECT_EQ(node(canvas, b4)->inputPin()->chainVisibility(), Qt::Checked);
}

TEST_F(SignalPathConnectionTest, FilterPinClicksAndCancelledDragsPreserveProjectAndSelection)
{
    auto* track = create("Track", T::Track);
    auto* trackSound = create("TrackSound", T::Sound, track);
    auto* piece = create("Piece", T::Piece);
    auto* pieceTrack = create("PieceTrack", T::Track, piece);
    auto* pieceSound = create("PieceSound", T::Sound, pieceTrack);
    create("IndependentBusInput", T::Bus, b4);
    BlueprintCanvasWidget canvas;
    show(canvas);
    UndoRedoManager::instance().clear();
    const auto originalGroups = s1->fieldGroups;
    int activations = 0;
    QObject::connect(&canvas, &BlueprintCanvasWidget::nodeDoubleClicked, &canvas,
        [&activations] { ++activations; });
    for (const auto direction : {CanvasFlowDirection::LR, CanvasFlowDirection::RL,
                                CanvasFlowDirection::TB, CanvasFlowDirection::BT}) {
        canvas.setFlowDirection(direction);
        canvas.setZoomFactor(1.0);
        for (const auto& [target, child] : {std::pair{layer, s1}, std::pair{c1, s1},
                 std::pair{track, trackSound}, std::pair{piece, pieceSound}, std::pair{b1, s1}}) {
            SCOPED_TRACE(::testing::Message() << static_cast<int>(direction) << ", " << target->name.toStdString());
            auto* pin = node(canvas, target)->inputPin();
            canvas.centerOn(pin->scenePos());
            canvas.scene()->clearSelection();
            node(canvas, b4)->setSelected(true);
            const auto point = canvas.mapFromScene(pin->scenePos());
            for (const bool expanded : {false, true}) {
                mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
                mouse(canvas, QEvent::MouseMove, point + QPoint(1, 0), Qt::NoButton, Qt::LeftButton);
                mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
                EXPECT_EQ(node(canvas, child)->isVisible(), expanded);
                EXPECT_TRUE(node(canvas, target)->isVisible());
                EXPECT_TRUE(node(canvas, b4)->isSelected());
                EXPECT_EQ(target->type == T::Bus ? canvas.busVisibility(target->objectID)
                                                : canvas.hierarchyVisibility(target->objectID),
                    expanded ? Qt::Checked : Qt::Unchecked);
            }
            mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&canvas, &escape);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
            EXPECT_TRUE(node(canvas, child)->isVisible());
            mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, point + QPoint(QApplication::startDragDistance() + 2, 0),
                Qt::NoButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
            EXPECT_TRUE(node(canvas, child)->isVisible());
            mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
            EXPECT_FALSE(node(canvas, child)->isVisible());
            mouse(canvas, QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
            EXPECT_TRUE(node(canvas, child)->isVisible());
            EXPECT_TRUE(node(canvas, b4)->isSelected());
            EXPECT_EQ(activations, 0);
            mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
            mouse(canvas, QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, point + QPoint(QApplication::startDragDistance() + 2, 0),
                Qt::NoButton, Qt::LeftButton);
            EXPECT_NE(preview(canvas), nullptr);
            QApplication::sendEvent(&canvas, &escape);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
            EXPECT_FALSE(node(canvas, child)->isVisible());
            mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
            EXPECT_TRUE(node(canvas, child)->isVisible());
            canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Pan);
            mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
            EXPECT_TRUE(node(canvas, child)->isVisible());
            canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
        }
        NodeItem* gain = nullptr;
        for (size_t index = 0; index < canvas.activeSignalPathSource()->graph().edges.size(); ++index)
            if ((gain = canvas.signalPathGainNode(index))) break;
        ASSERT_NE(gain, nullptr);
        for (auto* pin : {node(canvas, c2)->inputPin(), node(canvas, s1)->inputPin(), node(canvas, s1)->outputPin(),
                         node(canvas, b1)->outputPin(), gain->inputPin(), gain->outputPin()}) {
            canvas.centerOn(pin->scenePos());
            const auto point = canvas.mapFromScene(pin->scenePos());
            for (auto press : {QEvent::MouseButtonPress, QEvent::MouseButtonDblClick}) {
                mouse(canvas, press, point, Qt::LeftButton, Qt::LeftButton);
                EXPECT_EQ(preview(canvas) != nullptr, pin->parentItem() != gain);
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(&canvas, &escape);
                mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
                EXPECT_EQ(activations, 0);
                EXPECT_TRUE(node(canvas, b4)->isSelected());
            }
        }
        canvas.setBusVisible(b4->objectID, false);
        canvas.setBusVisible(b2->objectID, false);
        auto* partialPin = node(canvas, layer)->inputPin();
        EXPECT_EQ(partialPin->chainVisibility(), Qt::PartiallyChecked);
        canvas.centerOn(partialPin->scenePos());
        const auto partialPoint = canvas.mapFromScene(partialPin->scenePos());
        canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Pan);
        mouse(canvas, QEvent::MouseButtonPress, partialPoint, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, partialPoint, Qt::LeftButton, Qt::NoButton);
        EXPECT_FALSE(node(canvas, c1)->isVisible());
        EXPECT_EQ(partialPin->chainVisibility(), Qt::PartiallyChecked);
        canvas.setInteractionMode(BlueprintCanvasView::InteractionMode::Select);
        mouse(canvas, QEvent::MouseButtonPress, partialPoint, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, partialPoint, Qt::LeftButton, Qt::NoButton);
        EXPECT_TRUE(node(canvas, c1)->isVisible());
        EXPECT_EQ(partialPin->chainVisibility(), Qt::Checked);
        EXPECT_EQ(canvas.busVisibility(b2->objectID), Qt::Checked);
        EXPECT_EQ(canvas.busVisibility(b4->objectID), Qt::Unchecked);
    }
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(s1->fieldGroups, originalGroups);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathConnectionTest, RecursivePinGesturesPreserveLocalStatesAndRespectObjectFiltering)
{
    auto* secondSound = create("SecondSound", T::Sound, c2);
    auto* firstTrack = create("FirstTrack", T::Track, c1);
    auto* secondTrack = create("SecondTrack", T::Track, c2);
    create("FirstTrackSound", T::Sound, firstTrack);
    create("SecondTrackSound", T::Sound, secondTrack);
    const auto originalGroups = c1->fieldGroups;
    BlueprintCanvasWidget canvas;
    show(canvas);
    UndoRedoManager::instance().clear();
    const auto click = [&](ProjectItem* target, bool recursive) {
        auto* pin = node(canvas, target)->inputPin();
        canvas.centerOn(pin->scenePos());
        const auto point = canvas.mapFromScene(pin->scenePos());
        const auto modifiers = recursive ? Qt::ControlModifier : Qt::NoModifier;
        mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, modifiers);
        mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton, modifiers);
    };
    for (const auto direction : {CanvasFlowDirection::LR, CanvasFlowDirection::RL,
                                CanvasFlowDirection::TB, CanvasFlowDirection::BT}) {
        canvas.setFlowDirection(direction);
        canvas.setZoomFactor(1.0);
        for (const bool filtered : {false, true}) {
            SCOPED_TRACE(::testing::Message() << static_cast<int>(direction) << ", " << filtered);
            canvas.setSoundsFiltered(false);
            canvas.setChainVisible(master->objectID, true);
            canvas.setSoundsFiltered(filtered);
            click(c1, false);
            EXPECT_EQ(node(canvas, layer)->inputPin()->chainVisibility(), Qt::Checked);
            click(layer, false);
            click(layer, false);
            EXPECT_EQ(node(canvas, c1)->inputPin()->chainVisibility(), Qt::Unchecked);
            EXPECT_EQ(node(canvas, c2)->inputPin()->chainVisibility(), Qt::Checked);
            click(layer, true);
            EXPECT_FALSE(node(canvas, c1)->isVisible());
            EXPECT_FALSE(node(canvas, c2)->isVisible());
            click(layer, false);
            EXPECT_TRUE(node(canvas, c1)->isVisible());
            EXPECT_TRUE(node(canvas, c2)->isVisible());
            EXPECT_EQ(node(canvas, c1)->inputPin()->chainVisibility(), Qt::Unchecked);
            EXPECT_EQ(node(canvas, c2)->inputPin()->chainVisibility(), Qt::Unchecked);
            click(c1, false);
            EXPECT_TRUE(node(canvas, firstTrack)->isVisible());
            EXPECT_FALSE(node(canvas, secondTrack)->isVisible());
            EXPECT_EQ(node(canvas, c2)->inputPin()->chainVisibility(), Qt::Unchecked);
            click(layer, false);
            click(layer, true);
            EXPECT_TRUE(node(canvas, firstTrack)->isVisible());
            EXPECT_TRUE(node(canvas, secondTrack)->isVisible());
            EXPECT_EQ(node(canvas, s1)->isVisible(), !filtered);
            EXPECT_EQ(node(canvas, secondSound)->isVisible(), !filtered);
            const auto pinState = node(canvas, layer)->inputPin()->chainVisibility();
            QKeyEvent control(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
            QApplication::sendEvent(&canvas, &control);
            EXPECT_EQ(node(canvas, layer)->inputPin()->chainVisibility(), pinState);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
            QApplication::sendEvent(&canvas, &release);
            click(master, true);
            EXPECT_FALSE(node(canvas, layer)->isVisible());
            click(master, false);
            EXPECT_TRUE(node(canvas, b1)->isVisible());
            EXPECT_EQ(node(canvas, b1)->inputPin()->chainVisibility(), Qt::Unchecked);
            click(master, false);
            click(master, true);
            EXPECT_TRUE(node(canvas, firstTrack)->isVisible());
            EXPECT_TRUE(node(canvas, secondTrack)->isVisible());
            const auto hierarchy = canvas.hiddenHierarchyRoots();
            const auto buses = canvas.hiddenBuses();
            auto* pin = node(canvas, layer)->inputPin();
            canvas.centerOn(pin->scenePos());
            const auto point = canvas.mapFromScene(pin->scenePos());
            mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
            mouse(canvas, QEvent::MouseMove, point + QPoint(QApplication::startDragDistance() + 2, 0),
                Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
            EXPECT_NE(preview(canvas), nullptr);
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::ControlModifier);
            QApplication::sendEvent(&canvas, &escape);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
            EXPECT_EQ(canvas.hiddenHierarchyRoots(), hierarchy);
            EXPECT_EQ(canvas.hiddenBuses(), buses);
        }
    }
    EXPECT_EQ(c1->fieldGroups, originalGroups);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathConnectionTest, SuccessfulConnectionsRevealTheirInputsWithoutResettingIndependentFilters)
{
    auto* receivingLayer = create("ReceivingLayer", T::Layer);
    auto* receivingContainer = create("ReceivingContainer", T::Container);
    auto* receivingTrack = create("ReceivingTrack", T::Track);
    auto* receivingPiece = create("ReceivingPiece", T::Piece);
    auto* inputTrack = create("InputTrack", T::Track, c1);
    auto* independentLayer = create("IndependentLayer", T::Layer);
    create("IndependentSound", T::Sound, independentLayer);
    create("IndependentBusInput", T::Bus, b4);
    for (auto* target : {receivingLayer, receivingContainer, receivingTrack, receivingPiece})
        create("ExistingInput", target == receivingPiece ? T::Track : T::Sound, target);
    for (auto* target : {receivingLayer, receivingContainer, receivingTrack, receivingPiece, b3}) {
        auto* source = target == receivingPiece ? inputTrack : s1;
        for (const bool reverse : {false, true}) {
            for (const bool body : {false, true}) {
                for (const bool addSend : {false, true}) {
                    if (addSend && target->type != T::Bus) continue;
                    SCOPED_TRACE(::testing::Message() << target->name.toStdString() << ", "
                        << reverse << ", " << body << ", " << addSend);
                    BlueprintCanvasWidget canvas;
                    show(canvas);
                    canvas.setHierarchyVisible(independentLayer->objectID, false);
                    canvas.setBusVisible(b4->objectID, false);
                    if (target->type == T::Bus) canvas.setBusVisible(target->objectID, false);
                    else canvas.setHierarchyVisible(target->objectID, false);
                    const auto state = [&] {
                        return target->type == T::Bus ? canvas.busVisibility(target->objectID)
                                                     : canvas.hierarchyVisibility(target->objectID);
                    };
                    const auto* sourceNode = node(canvas, reverse ? target : source);
                    const auto* targetNode = node(canvas, reverse ? source : target);
                    const auto start = canvas.mapFromScene((reverse ? sourceNode->inputPin()
                                                                    : sourceNode->outputPin())->scenePos());
                    const auto end = canvas.mapFromScene(body ? targetNode->mapToScene(QPointF(70, 10))
                        : (reverse ? targetNode->outputPin() : targetNode->inputPin())->scenePos());
                    const auto modifiers = addSend ? Qt::AltModifier : Qt::NoModifier;
                    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, modifiers);
                    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton, modifiers);
                    EXPECT_EQ(state(), Qt::Unchecked);
                    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                    QApplication::sendEvent(&canvas, &escape);
                    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton, modifiers);
                    EXPECT_EQ(state(), Qt::Unchecked);
                    UndoRedoManager::instance().clear();
                    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, modifiers);
                    if (!reverse || !body)
                        mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton, modifiers);
                    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton, modifiers);
                    drainConnectionEvents();
                    EXPECT_EQ(state(), Qt::Checked);
                    EXPECT_TRUE(node(canvas, source)->isVisible());
                    EXPECT_TRUE(node(canvas, target)->isVisible());
                    const auto& graph = canvas.activeSignalPathSource()->graph();
                    bool connected = false;
                    for (size_t index = 0; index < graph.edges.size(); ++index) {
                        const auto& edge = graph.edges[index];
                        if (graph.nodes[edge.to.occurrenceIndex].key.objectGuid != target->guid
                            || std::none_of(edge.pathIndices.begin(), edge.pathIndices.end(),
                                [&](size_t pathIndex) { return graph.paths[pathIndex].identity.sourceGuid == source->guid; }))
                            continue;
                        connected = true;
                        EXPECT_TRUE(canvas.signalPathEdge(index)->isVisible());
                        if (auto* gain = canvas.signalPathGainNode(index)) EXPECT_TRUE(gain->isVisible());
                        if (auto* output = canvas.signalPathGainOutputEdge(index)) EXPECT_TRUE(output->isVisible());
                    }
                    EXPECT_TRUE(connected);
                    EXPECT_EQ(canvas.hierarchyVisibility(independentLayer->objectID), Qt::Unchecked);
                    EXPECT_EQ(canvas.busVisibility(b4->objectID), Qt::Unchecked);
                    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
                    ASSERT_TRUE(UndoRedoManager::instance().undo());
                    drainConnectionEvents();
                    EXPECT_EQ(state(), Qt::Checked);
                    EXPECT_EQ(source->parent, c1);
                    EXPECT_EQ(output(source).mainOutputBusRef, toFieldGroupGuid(b1->guid));
                }
            }
        }
    }
}

TEST_F(SignalPathConnectionTest, NewSharedSendRevealsInputsUsingTheUpdatedGraph)
{
    auto* sender = create("Sender", T::Sound);
    create("IndependentBusInput", T::Bus, b4);
    setOutput(sender, b1);
    BlueprintCanvasWidget canvas;
    canvas.setBusVisible(b4->objectID, false);
    canvas.setBusVisible(b1->objectID, false);
    EXPECT_FALSE(node(canvas, sender)->isVisible());
    EXPECT_EQ(canvas.busVisibility(b3->objectID), Qt::Checked);
    const auto& current = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(canvas.connectSignalPathNodes(endpoint(current, sender), endpoint(current, b3), true).success);
    drainConnectionEvents();
    EXPECT_TRUE(node(canvas, sender)->isVisible());
    EXPECT_FALSE(node(canvas, s1)->isVisible());
    EXPECT_FALSE(node(canvas, s2)->isVisible());
    EXPECT_EQ(canvas.busVisibility(b1->objectID), Qt::PartiallyChecked);
    EXPECT_EQ(canvas.busVisibility(b3->objectID), Qt::Checked);
    EXPECT_EQ(canvas.busVisibility(b4->objectID), Qt::Unchecked);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainConnectionEvents();
    EXPECT_TRUE(node(canvas, sender)->isVisible());
    EXPECT_EQ(canvas.busVisibility(b4->objectID), Qt::Unchecked);
}

TEST_F(SignalPathConnectionTest, ConnectionsInReopenedBranchesPreserveSiblingFiltersThroughUndoRedo)
{
    auto* parent = create("Parent", T::Bus, master);
    auto* receiving = create("Receiving", T::Bus, parent);
    auto* sibling = create("Sibling", T::Bus, parent);
    auto* receivingInput = create("ReceivingInput", T::Sound);
    auto* siblingInput = create("SiblingInput", T::Sound);
    auto* sender = create("Sender", T::Sound);
    setOutput(receivingInput, receiving);
    setOutput(siblingInput, sibling);
    setOutput(sender, b4);
    BlueprintCanvasWidget canvas;
    canvas.setChainVisible(parent->objectID, false);
    canvas.setChainVisible(receiving->objectID, true);
    canvas.setBusVisible(receiving->objectID, false);
    ASSERT_TRUE(node(canvas, sender)->isVisible());
    ASSERT_TRUE(node(canvas, receiving)->isVisible());
    ASSERT_EQ(canvas.busVisibility(receiving->objectID), Qt::Unchecked);
    const auto& current = canvas.activeSignalPathSource()->graph();
    ASSERT_TRUE(canvas.connectSignalPathNodes(endpoint(current, sender), endpoint(current, receiving), true).success);
    const auto expectBranchVisibility = [&] {
        drainConnectionEvents();
        EXPECT_TRUE(node(canvas, sender)->isVisible());
        EXPECT_TRUE(node(canvas, receivingInput)->isVisible());
        EXPECT_FALSE(node(canvas, sibling)->isVisible());
        EXPECT_FALSE(node(canvas, siblingInput)->isVisible());
        EXPECT_EQ(canvas.busVisibility(receiving->objectID), Qt::Checked);
        EXPECT_EQ(canvas.chainVisibility(parent->objectID), Qt::PartiallyChecked);
    };
    expectBranchVisibility();
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    expectBranchVisibility();
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    expectBranchVisibility();
    ASSERT_TRUE(ProjectManager::instance().deleteItem(siblingInput));
    drainConnectionEvents();
    EXPECT_FALSE(canvas.canCollapseChain(sibling->objectID));
    EXPECT_FALSE(node(canvas, sibling)->isVisible());
    ASSERT_TRUE(ProjectManager::instance().deleteItem(sibling));
    drainConnectionEvents();
    EXPECT_EQ(canvas.chainVisibility(parent->objectID), Qt::Checked);
    EXPECT_FALSE(canvas.hasCollapsedBuses());
}

TEST_F(SignalPathConnectionTest, CanvasHintsDescribeEffectiveMovesAndOutputsForBodiesAndPins)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    struct Case {
        ProjectItem* source;
        ProjectItem* target;
        bool reverse;
    };
    const Case cases[] = {
        {s1, c2, false},
        {c2, s1, true},
        {c1, c2, false},
        {c2, c1, true},
        {s1, b3, false},
        {b3, s1, true},
        {b1, b3, false},
        {b3, b1, true}
    };
    for (const auto& entry : cases) {
        for (const bool body : {false, true}) {
            auto* origin = entry.reverse ? node(canvas, entry.source)->inputPin()
                                         : node(canvas, entry.source)->outputPin();
            auto* destination = entry.reverse ? node(canvas, entry.target)->outputPin()
                                              : node(canvas, entry.target)->inputPin();
            const auto start = canvas.mapFromScene(origin->scenePos());
            const auto end = canvas.mapFromScene(body ? node(canvas, entry.target)->mapToScene(QPointF(70, 10))
                                                      : destination->scenePos());
            mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
            EXPECT_EQ(node(canvas, entry.target)->dropFeedback(), body ? Feedback::Valid : Feedback::None);
            EXPECT_EQ(destination->connectState(), body ? NodePin::ConnectState::None : NodePin::ConnectState::Ok);
            EXPECT_EQ(preview(canvas)->pen().style(), Qt::SolidLine);
            EXPECT_EQ(s1->parent, c1);
            EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b1->guid));
            mouse(canvas, QEvent::MouseButtonPress, end, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
            EXPECT_FALSE(hint(canvas)->isVisible());
            EXPECT_EQ(destination->connectState(), NodePin::ConnectState::None);
            mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        }
    }
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathConnectionTest, CanvasBasicMoveHintsOnlyDescribeHierarchyAcrossInheritanceModes)
{
    s1->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(b1->guid), 0.0f}};
    for (const bool android : {false, true}) {
        const auto scope = android ? HarkenTest::PlatformIds::Android : HarkenTest::PlatformIds::Windows;
        PlatformContextService::instance().setSelected({scope}, scope, true);
        BlueprintCanvasWidget canvas;
        show(canvas);
        const auto start = canvas.mapFromScene(node(canvas, s1)->outputPin()->scenePos());
        const auto end = canvas.mapFromScene(node(canvas, c2)->inputPin()->scenePos());
        mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        EXPECT_EQ(node(canvas, c2)->dropFeedback(), Feedback::None);
    }
    s1->fieldGroups.mainOutput.platforms.clear();
    setOutput(c2, b1);
    BlueprintCanvasWidget canvas;
    show(canvas);
    const auto start = canvas.mapFromScene(node(canvas, s1)->outputPin()->scenePos());
    const auto end = canvas.mapFromScene(node(canvas, c2)->inputPin()->scenePos());
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, c2)->dropFeedback(), Feedback::None);
}

TEST_F(SignalPathConnectionTest, CanvasAltHintsNeverFallBackToMovesForInvalidTargets)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    for (const bool reverse : {false, true}) {
        const auto start = canvas.mapFromScene((reverse ? node(canvas, c2)->inputPin()
                                                        : node(canvas, s1)->outputPin())->scenePos());
        const auto target = canvas.mapFromScene((reverse ? node(canvas, s1)->outputPin()
                                                         : node(canvas, c2)->inputPin())->scenePos());
        mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
        mouse(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
        EXPECT_FALSE(hint(canvas)->isVisible());
        EXPECT_TRUE(hint(canvas)->text().isEmpty());
        EXPECT_EQ(preview(canvas)->pen().dashPattern(), (QList<qreal>{6, 3}));
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &release);
        EXPECT_EQ(preview(canvas)->pen().style(), Qt::SolidLine);
        EXPECT_TRUE(hint(canvas)->isVisible());
        QKeyEvent alt(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
        QApplication::sendEvent(&canvas, &alt);
        EXPECT_FALSE(hint(canvas)->isVisible());
        EXPECT_TRUE(hint(canvas)->text().isEmpty());
        mouse(canvas, QEvent::MouseMove, QPoint(2, 2), Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
        EXPECT_TRUE(hint(canvas)->isVisible());
        QApplication::sendEvent(&canvas, &release);
        EXPECT_EQ(preview(canvas)->pen().style(), Qt::SolidLine);
        QApplication::sendEvent(&canvas, &alt);
        EXPECT_EQ(preview(canvas)->pen().dashPattern(), (QList<qreal>{6, 3}));
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &escape);
        mouse(canvas, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
    }
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathConnectionTest, CanvasHintUsesThemePanelAboveThePointerAndHidesOnlyForInvalidTargets)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    const auto start = canvas.mapFromScene(node(canvas, s1)->outputPin()->scenePos());
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    EXPECT_TRUE(hint(canvas)->isVisible());
    auto* pin = node(canvas, b3)->inputPin();
    auto target = canvas.mapFromScene(pin->scenePos());
    mouse(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(pin->connectState(), NodePin::ConnectState::Ok);
    EXPECT_TRUE(hint(canvas)->isVisible());
    QHelpEvent help(QEvent::ToolTip, target, canvas.viewport()->mapToGlobal(target));
    QApplication::sendEvent(canvas.viewport(), &help);
    EXPECT_FALSE(QToolTip::isVisible());
    const auto invalid = canvas.mapFromScene(node(canvas, b3)->outputPin()->scenePos());
    mouse(canvas, QEvent::MouseMove, invalid, Qt::NoButton, Qt::LeftButton);
    EXPECT_FALSE(hint(canvas)->isVisible());
    EXPECT_EQ(node(canvas, b3)->outputPin()->connectState(), NodePin::ConnectState::Bad);
    mouse(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
    EXPECT_TRUE(hint(canvas)->isVisible());
    canvas.clear();
    EXPECT_FALSE(hint(canvas)->isVisible());
    EXPECT_EQ(preview(canvas), nullptr);
}

TEST_F(SignalPathConnectionTest, ManualNodeMovesKeepEveryUneditedObjectAndGainInPlaceAcrossRefreshAndUndo)
{
    for (const auto direction : {CanvasFlowDirection::LR, CanvasFlowDirection::RL,
                                CanvasFlowDirection::TB, CanvasFlowDirection::BT}) {
        for (const bool multiple : {false, true}) {
            SCOPED_TRACE(::testing::Message() << static_cast<int>(direction) << ", " << multiple);
            BlueprintCanvasWidget canvas;
            canvas.setFlowDirection(direction);
            canvas.resetNodeLayout();
            UndoRedoManager::instance().clear();
            show(canvas);
            canvas.setZoomFactor(1.0);
            canvas.centerOn(node(canvas, b1));
            canvas.scene()->clearSelection();
            node(canvas, b1)->setSelected(true);
            if (multiple) node(canvas, b2)->setSelected(true);
            const auto graph = canvas.activeSignalPathSource()->graph();
            std::vector<QPointF> objects;
            std::vector<std::optional<QPointF>> gains(graph.edges.size());
            for (size_t index = 0; index < graph.nodes.size(); ++index)
                objects.push_back(canvas.signalPathNode(index)->pos());
            for (size_t index = 0; index < gains.size(); ++index)
                if (auto* gain = canvas.signalPathGainNode(index)) gains[index] = gain->pos();
            const bool vertical = direction == CanvasFlowDirection::TB || direction == CanvasFlowDirection::BT;
            const int distance = direction == CanvasFlowDirection::RL || direction == CanvasFlowDirection::BT ? -48 : 48;
            const QPoint delta = vertical ? QPoint(0, distance) : QPoint(distance, 0);
            const auto start = canvas.mapFromScene(node(canvas, b1)->mapToScene(QPointF(70, 10)));
            const auto end = start + delta;
            mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, start + delta / 2, Qt::NoButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
            drainConnectionEvents();
            const auto checkPositions = [&](BlueprintCanvasWidget& view, bool moved) {
                for (size_t index = 0; index < objects.size(); ++index) {
                    const auto guid = graph.nodes[index].key.objectGuid;
                    const bool edited = guid == b1->guid || (multiple && guid == b2->guid);
                    EXPECT_EQ(view.signalPathNode(index)->pos(),
                        objects[index] + (moved && edited ? QPointF(delta) : QPointF()));
                }
                for (size_t index = 0; index < gains.size(); ++index)
                    if (gains[index]) EXPECT_EQ(view.signalPathGainNode(index)->pos(), *gains[index]);
            };
            checkPositions(canvas, true);
            ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
            canvas.rebuildFromProject();
            checkPositions(canvas, true);
            ASSERT_TRUE(UndoRedoManager::instance().undo());
            drainConnectionEvents();
            checkPositions(canvas, false);
            ASSERT_TRUE(UndoRedoManager::instance().redo());
            drainConnectionEvents();
            checkPositions(canvas, true);
            BlueprintCanvasWidget reopened;
            reopened.setFlowDirection(direction);
            checkPositions(reopened, true);
        }
    }
}

TEST_F(SignalPathConnectionTest, GainNodesRejectDropsWithoutMovingOrAuthoring)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    ToastManager::instance().setParentWidget(&canvas);
    size_t gainIndex = 0;
    const auto edgeCount = canvas.activeSignalPathSource()->graph().edges.size();
    while (gainIndex < edgeCount && !canvas.signalPathGainNode(gainIndex)) ++gainIndex;
    ASSERT_LT(gainIndex, edgeCount);
    for (const bool moveNode : {false, true}) {
        for (int part = 0; part < 3; ++part) {
            canvas.scene()->clearSelection();
            auto* gain = canvas.signalPathGainNode(gainIndex);
            const auto original = node(canvas, s1)->pos();
            const auto gainPosition = gain->pos();
            const auto gainRect = gain->shape().boundingRect();
            const auto target = canvas.mapFromScene(part == 0
                ? gain->mapToScene(QPointF(gainRect.center().x(), gainRect.bottom() - 3))
                : part == 1 ? gain->inputPin()->scenePos() : gain->outputPin()->scenePos());
            const auto start = canvas.mapFromScene(moveNode
                ? node(canvas, s1)->mapToScene(QPointF(70, 10)) : node(canvas, s1)->outputPin()->scenePos());
            const auto valid = canvas.mapFromScene(moveNode
                ? node(canvas, c2)->mapToScene(QPointF(70, 10)) : node(canvas, b3)->inputPin()->scenePos());
            mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, valid, Qt::NoButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
            EXPECT_EQ(gain->dropFeedback(), Feedback::None);
            EXPECT_EQ(gain->inputPin()->connectState(), NodePin::ConnectState::None);
            EXPECT_EQ(gain->outputPin()->connectState(), NodePin::ConnectState::None);
            EXPECT_EQ(node(canvas, c2)->dropFeedback(), Feedback::None);
            EXPECT_EQ(node(canvas, b3)->inputPin()->connectState(), NodePin::ConnectState::None);
            EXPECT_FALSE(hint(canvas)->isVisible());
            mouse(canvas, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
            drainConnectionEvents();
            EXPECT_EQ(node(canvas, s1)->pos(), original);
            EXPECT_EQ(canvas.signalPathGainNode(gainIndex)->pos(), gainPosition);
            EXPECT_TRUE(canvas.findChildren<QToastWidget*>().empty());
            EXPECT_EQ(s1->parent, c1);
            EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
        }
    }
    ToastManager::instance().setParentWidget(nullptr);
}

TEST_F(SignalPathConnectionTest, CanvasNodeDropChangesHierarchyWithoutLeavingOverlappingNodes)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    const auto start = canvas.mapFromScene(node(canvas, s1)->mapToScene(QPointF(70, 10)));
    const auto end = canvas.mapFromScene(node(canvas, c2)->mapToScene(QPointF(70, 10)));
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, c2)->dropFeedback(), Feedback::Valid);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    drainConnectionEvents();
    EXPECT_EQ(s1->parent, c2);
    EXPECT_EQ(node(canvas, c2)->dropFeedback(), Feedback::None);
    EXPECT_FALSE(node(canvas, s1)->sceneBoundingRect().intersects(node(canvas, c2)->sceneBoundingRect()));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainConnectionEvents();
    EXPECT_EQ(s1->parent, c1);
}

TEST_F(SignalPathConnectionTest, CanvasInvalidNodeDropRestoresPositionsAndDoesNotCreateUndo)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    const auto original = node(canvas, s1)->pos();
    const auto start = canvas.mapFromScene(node(canvas, s1)->mapToScene(QPointF(70, 10)));
    const auto end = canvas.mapFromScene(node(canvas, s2)->mapToScene(QPointF(70, 10)));
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, s2)->dropFeedback(), Feedback::Invalid);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    drainConnectionEvents();
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(node(canvas, s1)->pos(), original);
    EXPECT_EQ(node(canvas, s2)->dropFeedback(), Feedback::None);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathConnectionTest, CanvasMultiDropCommitsAllSelectedObjectsAsOneUndo)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    node(canvas, s1)->setSelected(true);
    node(canvas, s2)->setSelected(true);
    const auto rows = c1->comp<VNX::VSelection>()->selections.shared;
    const auto start = canvas.mapFromScene(node(canvas, s1)->mapToScene(QPointF(70, 10)));
    const auto end = canvas.mapFromScene(node(canvas, c2)->mapToScene(QPointF(70, 10)));
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    drainConnectionEvents();
    EXPECT_EQ(s1->parent, c2);
    EXPECT_EQ(s2->parent, c2);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(c1->comp<VNX::VSelection>()->selections.shared, rows);
}

TEST_F(SignalPathConnectionTest, CanvasInvalidMultiDropRestoresEveryPositionAndEveryParent)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    node(canvas, s1)->setSelected(true);
    node(canvas, b1)->setSelected(true);
    const auto first = node(canvas, s1)->pos();
    const auto bus = node(canvas, b1)->pos();
    const auto start = canvas.mapFromScene(node(canvas, s1)->mapToScene(QPointF(70, 10)));
    const auto end = canvas.mapFromScene(node(canvas, c2)->mapToScene(QPointF(70, 10)));
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, c2)->dropFeedback(), Feedback::Invalid);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    drainConnectionEvents();
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(b1->parent, master);
    EXPECT_EQ(node(canvas, s1)->pos(), first);
    EXPECT_EQ(node(canvas, b1)->pos(), bus);
    EXPECT_EQ(node(canvas, c2)->dropFeedback(), Feedback::None);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathConnectionTest, CanvasEscapeCancelsNodeDropAndRestoresOriginalPosition)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    const auto before = node(canvas, s1)->pos();
    const auto start = canvas.mapFromScene(node(canvas, s1)->mapToScene(QPointF(70, 10)));
    const auto end = canvas.mapFromScene(node(canvas, b3)->mapToScene(QPointF(70, 10)));
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::Valid);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::None);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    drainConnectionEvents();
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b1->guid));
    EXPECT_EQ(node(canvas, s1)->pos(), before);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathConnectionTest, CanvasRejectsSameSidePinsAndKeepsGainPinsOutOfConnectionEditing)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    const auto start = canvas.mapFromScene(node(canvas, s1)->outputPin()->scenePos());
    const auto wrong = canvas.mapFromScene(node(canvas, b3)->outputPin()->scenePos());
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, wrong, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, b3)->outputPin()->connectState(), NodePin::ConnectState::Bad);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::None);
    mouse(canvas, QEvent::MouseMove, wrong, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::None);
    mouse(canvas, QEvent::MouseButtonRelease, wrong, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::None);
    EXPECT_EQ(node(canvas, b3)->outputPin()->connectState(), NodePin::ConnectState::None);
    NodeItem* gain = nullptr;
    for (size_t index = 0; index < canvas.activeSignalPathSource()->graph().edges.size(); ++index)
        if ((gain = canvas.signalPathGainNode(index))) break;
    ASSERT_NE(gain, nullptr);
    const auto gainPosition = gain->pos();
    const auto gainStart = canvas.mapFromScene(gain->outputPin()->scenePos());
    const auto target = canvas.mapFromScene(node(canvas, b3)->inputPin()->scenePos());
    mouse(canvas, QEvent::MouseButtonPress, gainStart, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(gain->pos(), gainPosition);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_EQ(output(s1).mainOutputBusRef, toFieldGroupGuid(b1->guid));
}

TEST_F(SignalPathConnectionTest, CanvasConnectionKeepsPinAndBodyFeedbackSeparateDuringTargetChanges)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    const auto start = canvas.mapFromScene(node(canvas, s1)->outputPin()->scenePos());
    const auto body = canvas.mapFromScene(node(canvas, b3)->mapToScene(QPointF(70, 10)));
    const auto input = canvas.mapFromScene(node(canvas, b3)->inputPin()->scenePos());
    const auto output = canvas.mapFromScene(node(canvas, b3)->outputPin()->scenePos());
    const auto invalid = canvas.mapFromScene(node(canvas, s2)->mapToScene(QPointF(70, 10)));
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, body, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::Valid);
    mouse(canvas, QEvent::MouseMove, input, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::None);
    EXPECT_EQ(node(canvas, b3)->inputPin()->connectState(), NodePin::ConnectState::Ok);
    mouse(canvas, QEvent::MouseMove, output, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::None);
    EXPECT_EQ(node(canvas, b3)->inputPin()->connectState(), NodePin::ConnectState::None);
    EXPECT_EQ(node(canvas, b3)->outputPin()->connectState(), NodePin::ConnectState::Bad);
    mouse(canvas, QEvent::MouseMove, invalid, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, b3)->outputPin()->connectState(), NodePin::ConnectState::None);
    EXPECT_EQ(node(canvas, s2)->dropFeedback(), Feedback::Invalid);
    mouse(canvas, QEvent::MouseMove, body, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(node(canvas, s2)->dropFeedback(), Feedback::None);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::Valid);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    mouse(canvas, QEvent::MouseButtonRelease, body, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(node(canvas, b3)->dropFeedback(), Feedback::None);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathConnectionTest, CanvasNodeDropRejectsPinsAndRevalidatesReleasePosition)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    for (const bool input : {true, false}) {
        for (const bool preview : {true, false}) {
            const auto original = node(canvas, s1)->pos();
            const auto start = canvas.mapFromScene(node(canvas, s1)->mapToScene(QPointF(70, 10)));
            const auto body = canvas.mapFromScene(node(canvas, c2)->mapToScene(QPointF(70, 10)));
            auto* pin = input ? node(canvas, c2)->inputPin() : node(canvas, c2)->outputPin();
            const auto target = canvas.mapFromScene(pin->scenePos());
            mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, body, Qt::NoButton, Qt::LeftButton);
            EXPECT_EQ(node(canvas, c2)->dropFeedback(), Feedback::Valid);
            if (preview) {
                mouse(canvas, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
                EXPECT_EQ(node(canvas, c2)->dropFeedback(), Feedback::None);
                EXPECT_EQ(pin->connectState(), NodePin::ConnectState::None);
            }
            mouse(canvas, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
            drainConnectionEvents();
            EXPECT_EQ(node(canvas, c2)->dropFeedback(), Feedback::None);
            EXPECT_EQ(node(canvas, s1)->pos(), original);
            EXPECT_EQ(s1->parent, c1);
            EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
        }
    }
}

TEST_F(SignalPathConnectionTest, PreviewTangentsFollowPinDirectionInAllFourCanvasOrientations)
{
    BlueprintCanvasWidget canvas;
    show(canvas);
    for (const auto direction : {CanvasFlowDirection::LR, CanvasFlowDirection::RL,
                                CanvasFlowDirection::TB, CanvasFlowDirection::BT}) {
        canvas.setFlowDirection(direction);
        canvas.zoomToFit();
        drainConnectionEvents();
        for (const bool reverse : {false, true}) {
            auto* source = reverse ? node(canvas, b3)->inputPin() : node(canvas, s1)->outputPin();
            const auto start = canvas.mapFromScene(source->scenePos());
            mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, start + QPoint(40, 40), Qt::NoButton, Qt::LeftButton);
            QGraphicsPathItem* preview = nullptr;
            for (auto* item : canvas.scene()->items())
                if (item->zValue() == 1000.0) preview = dynamic_cast<QGraphicsPathItem*>(item);
            ASSERT_NE(preview, nullptr);
            ASSERT_EQ(preview->path().elementCount(), 4);
            const auto tangent = preview->path().elementAt(1);
            const auto origin = preview->path().elementAt(0);
            const bool vertical = direction == CanvasFlowDirection::TB || direction == CanvasFlowDirection::BT;
            const bool positive = direction == CanvasFlowDirection::LR || direction == CanvasFlowDirection::TB;
            EXPECT_EQ(vertical ? tangent.x : tangent.y, vertical ? origin.x : origin.y);
            const auto delta = vertical ? tangent.y - origin.y : tangent.x - origin.x;
            EXPECT_EQ(delta > 0.0, positive != reverse);
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&canvas, &escape);
            mouse(canvas, QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::NoButton);
        }
    }
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

}
