#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ActionManager.h"
#include "BindingEditing.h"
#include "Components/VAttenuation.h"
#include "Components/VMixNode.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/Common/RangeField.h"
#include "Widgets/CurveEditor/CurveCanvas.h"
#include "Widgets/ExternalEditor/ExternalEditorPanel.h"
#include "Widgets/CurveEditor/CurveEditorWidget.h"
#include "Widgets/Inspector/EditorLaunchMap.h"
#include "Widgets/Inspector/BindingFieldEditor.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/SoundInspectorSchema.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QLabel>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QToolButton>
#include <vector>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class BindingCurveEditorHostTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerBindingActions(VNX::ActionManager::instance());
        VNX::registerBuiltinInspectorCustomizations();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        ASSERT_TRUE(temp_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("BindingCurveHost");
        config.location = temp_.path();
        ASSERT_TRUE(manager.createProject(config));
        sound_ = manager.createItem(nullptr, QStringLiteral("HostSound"),
                                    VNX::ProjectItem::Type::Sound);
        variable_ = manager.createItem(nullptr, QStringLiteral("Drive"),
                                       VNX::ProjectItem::Type::Variable);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(variable_, nullptr);
        auto* variable = variable_->compOrCreate<VNX::VVariable>();
        variable->kind = VNX::VariableKind::Smooth;
        variable->rangeMin = 0.0f;
        variable->rangeMax = 10.0f;
        variable->labels = {{0, QStringLiteral("Calm")}, {10, QStringLiteral("Storm")}};
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir temp_;
    VNX::ProjectItem* sound_ = nullptr;
    VNX::ProjectItem* variable_ = nullptr;
};

}

TEST_F(BindingCurveEditorHostTest, SlotOpensTableAndCurveInTheSameExternalEditor)
{
    ASSERT_TRUE(VNX::BindingEditing::upsert(
        sound_,
        VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::volumeAddress(), variable_->guid)));
    auto* extra = VNX::ProjectManager::instance().createItem(
        nullptr, QStringLiteral("ExtraDrive"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(extra, nullptr);
    ASSERT_TRUE(VNX::BindingEditing::upsert(
        sound_, VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::volumeAddress(), extra->guid)));
    const auto bindings = sound_->comp<VNX::VMixNode>()->bindings.findAll(
        VNX::BindingSemantics::volumeAddress());
    ASSERT_EQ(bindings.size(), 2u);
    const harkenstudio::ObjectId selectedId = bindings.back()->runtimeId();

    VNX::InspectorPanel inspector;
    VNX::ExternalEditorPanel editor;
    editor.resize(420, 600);
    QObject::connect(&inspector, &VNX::InspectorPanel::externalEditorRequested, &editor,
        [&editor](const VNX::ExternalEditorSelection& selection, bool) {
            editor.bindSelection(selection);
            editor.show();
        });
    inspector.showItemInspector(sound_);
    std::vector<VNX::CurveSelection> requested;

    auto* hang = inspector.findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
    ASSERT_NE(hang, nullptr);
    hang->click();
    EXPECT_EQ(inspector.findChild<VNX::BindingFieldEditor*>(), nullptr);
    auto* popup = editor.findChild<VNX::BindingFieldEditor*>(QStringLiteral("bindingFieldEditor_volume"));
    ASSERT_NE(popup, nullptr);
    auto* curve = editor.findChild<VNX::CurveEditorWidget*>();
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(popup->isVisible());
    EXPECT_TRUE(curve->isVisible());
    EXPECT_EQ(popup->findChild<QToolButton*>(QStringLiteral("bindingOpenCurve")), nullptr);
    popup->selectRow(0);
    QObject::connect(popup, &VNX::BindingFieldEditor::curveSelectionChanged,
                     [&requested](const VNX::CurveSelection& selection) {
                         requested.push_back(selection);
                     });
    const auto rows = popup->findChildren<QWidget*>(QStringLiteral("listRow"));
    ASSERT_EQ(rows.size(), 2);
    auto* thumbnail = rows.back()->findChild<QWidget*>(QStringLiteral("bindingCurveThumb"));
    ASSERT_NE(thumbnail, nullptr);
    const QPointF local = thumbnail->rect().center();
    const QPointF global = thumbnail->mapToGlobal(local.toPoint());
    QMouseEvent rightPress(QEvent::MouseButtonPress, local, global,
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &rightPress);
    EXPECT_TRUE(requested.empty());
    QMouseEvent leftPress(QEvent::MouseButtonPress, local, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(thumbnail, &leftPress);
    ASSERT_EQ(requested.size(), 1u);
    EXPECT_TRUE(requested.front().hasBinding());
    EXPECT_EQ(requested.front().hostGuid, sound_->guid);
    EXPECT_EQ(requested.front().bindingStableId, selectedId);
    EXPECT_TRUE(popup->isVisible());
    EXPECT_EQ(curve->bindingStableId(), selectedId);
    EXPECT_EQ(editor.selection().ownerGuid, sound_->guid);
}

TEST_F(BindingCurveEditorHostTest, ExternalEditorRestoresAnEmptyFieldAndPlacementContext)
{
    const VNX::PlacementContext placement = VNX::PlacementContext::fromOccurrence(
        {QUuid::createUuid(), QUuid::createUuid(), sound_->guid});
    VNX::ExternalEditorPanel editor;
    editor.bindSelection(VNX::ExternalEditorSelection::binding(
        sound_->guid, VNX::BindingSemantics::pitchAddress(), placement));
    VNX::ExternalEditorPanel restored;
    restored.restoreState(editor.snapshotState());
    EXPECT_EQ(restored.selection().kind, VNX::ExternalEditorSelection::Kind::Binding);
    EXPECT_EQ(restored.boundItem(), sound_);
    EXPECT_EQ(restored.selection().bindingAddress, VNX::BindingSemantics::pitchAddress());
    ASSERT_TRUE(restored.selection().placement.has_value());
    EXPECT_EQ(*restored.selection().placement, placement);
    EXPECT_FALSE(restored.findChild<VNX::CurveEditorWidget*>()->hasBinding());
    EXPECT_TRUE(restored.findChild<QToolButton*>(QStringLiteral("bindingAdd"))->isEnabled());
    EXPECT_FALSE(restored.selection().sameContext(VNX::ExternalEditorSelection::binding(
        sound_->guid, VNX::BindingSemantics::volumeAddress(), placement)));
    EXPECT_FALSE(restored.selection().sameContext(VNX::ExternalEditorSelection::source(
        sound_->guid, QUuid::createUuid())));
}

TEST_F(BindingCurveEditorHostTest, BindingRoundTripDoesNotWriteDistanceKey)
{
    VNX::ParameterBindingDef binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), variable_->guid);
    binding.curve = {{0.0f, -12.0f}, {0.25f, -6.0f}, {1.0f, 0.0f}};
    binding.handles = {{0.1f, -10.0f, 0.2f, -8.0f}, {0.4f, -4.0f, 0.8f, -1.0f}};
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_, binding));
    const auto* stored = sound_->comp<VNX::VMixNode>()->bindings.findFirst(
        VNX::BindingSemantics::volumeAddress());
    ASSERT_NE(stored, nullptr);

    VNX::CurveEditorWidget editor;
    editor.bindSelection(VNX::CurveSelection::binding(
        sound_->guid, stored->address, stored->runtimeId()));
    ASSERT_TRUE(editor.hasBinding());
    auto* canvas = editor.canvas();
    ASSERT_NE(canvas, nullptr);
    EXPECT_TRUE(canvas->profile().discretePreview == false);
    EXPECT_EQ(canvas->profile().yAxisLabel, QStringLiteral("Volume (dB)"));
    EXPECT_FLOAT_EQ(canvas->xMin(), 0.0f);
    EXPECT_FLOAT_EQ(canvas->xMax(), 10.0f);
    ASSERT_EQ(canvas->curve().size(), 3);
    EXPECT_NEAR(canvas->curve()[1].x, 2.5f, 0.0001f);
    EXPECT_NEAR(canvas->curve()[1].value, -6.0f, 0.0001f);

    const harkenstudio::ObjectId stableId = stored->runtimeId();
    auto points = canvas->curve();
    points[1].value = -3.0f;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &editor, "onCanvasCurveEdited", Qt::DirectConnection,
        Q_ARG(QVector<VNX::CurvePointDef>, points),
        Q_ARG(QVector<VNX::CurveHandlePairDef>, canvas->curveHandles()),
        Q_ARG(QString, QStringLiteral("Edit Binding Curve"))));

    const auto* after = sound_->comp<VNX::VMixNode>()->bindings.find(stableId);
    ASSERT_NE(after, nullptr);
    ASSERT_EQ(after->curve.size(), 3);
    EXPECT_NEAR(after->curve[1].x, 0.25f, 0.0001f);
    EXPECT_NEAR(after->curve[1].value, -3.0f, 0.0001f);
    const QJsonObject json = after->toJson().toObject();
    const QJsonObject curve = json.value(QStringLiteral("curve")).toObject();
    EXPECT_FALSE(curve.contains(QStringLiteral("distance")));
}

TEST_F(BindingCurveEditorHostTest, PitchAxisUsesCentAndOverlaysSiblings)
{
    auto* variable = variable_->comp<VNX::VVariable>();
    variable->kind = VNX::VariableKind::Step;
    variable->rangeMin = 0.0f;
    variable->rangeMax = 3.0f;
    VNX::ParameterBindingDef first = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), variable_->guid);
    first.outputMin = 0.0f;
    first.outputMax = 100.0f;
    VNX::ParameterBindingDef second = first;
    second.guid = VNX::authoringUuid(harkentest::StudioObjectId(0));
    second.curve = {{0.0f, 0.0f}, {1.0f, 50.0f}};
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_, first));
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_, second));
    const auto entries = sound_->comp<VNX::VMixNode>()->bindings.findAll(
        VNX::BindingSemantics::pitchAddress());
    ASSERT_EQ(entries.size(), 2u);

    VNX::CurveEditorWidget editor;
    editor.bindSelection(VNX::CurveSelection::binding(
        sound_->guid, entries.front()->address, entries.front()->runtimeId()));
    EXPECT_EQ(editor.canvas()->profile().valueUnit, QStringLiteral("cent"));
    EXPECT_TRUE(editor.canvas()->profile().discretePreview);
    EXPECT_TRUE(editor.findChildren<QDoubleSpinBox*>().isEmpty());
    EXPECT_TRUE(editor.canvas()->profile().yRangeEditable);
    EXPECT_FLOAT_EQ(editor.canvas()->profile().yRangeMinimum, -7200.0f);
    EXPECT_FLOAT_EQ(editor.canvas()->profile().yRangeMaximum, 7200.0f);
    ASSERT_EQ(editor.canvas()->overlays().size(), 1);
    ASSERT_EQ(editor.canvas()->overlays().front().points.size(), 2);
    EXPECT_NEAR(editor.canvas()->overlays().front().points.back().value, 50.0f, 0.0001f);
}

TEST_F(BindingCurveEditorHostTest, SwitchingHostsDoesNotLeakAttenuationCurve)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* attenuation = manager.createItem(
        nullptr, QStringLiteral("HostAttenuation"), VNX::ProjectItem::Type::Attenuation);
    ASSERT_NE(attenuation, nullptr);
    auto* component = attenuation->comp<VNX::VAttenuation>();
    ASSERT_NE(component, nullptr);
    component->dimensions.emplaceBack(VNX::AttenuationDimensionDef{VNX::AttenuationDimension::Volume});

    ASSERT_TRUE(VNX::BindingEditing::upsert(
        sound_,
        VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::volumeAddress(), variable_->guid)));
    const auto* stored = sound_->comp<VNX::VMixNode>()->bindings.findFirst(
        VNX::BindingSemantics::volumeAddress());
    ASSERT_NE(stored, nullptr);

    VNX::CurveEditorWidget editor;
    editor.bindSelection(VNX::CurveSelection::attenuation(
        component->hostItem()->guid, component->hostItem()->name, VNX::AttenuationDimension::Volume));
    EXPECT_TRUE(editor.hasAttenuation());
    EXPECT_FALSE(editor.hasBinding());
    EXPECT_EQ(editor.canvas()->profile().xAxisLabel, QStringLiteral("Distance"));
    EXPECT_TRUE(editor.findChildren<VNX::RangeField*>().isEmpty());
    EXPECT_FALSE(editor.canvas()->profile().yRangeEditable);
    EXPECT_FLOAT_EQ(editor.canvas()->profile().yMin, -96.0f);
    EXPECT_FLOAT_EQ(editor.canvas()->profile().yMax, 0.0f);
    EXPECT_EQ(editor.canvas()->profile().valueMaximum, std::optional<float>(0.0f));
    EXPECT_EQ(editor.canvas()->profile().axisScale, VNX::CurveAxisScale::AmplitudeDb);

    editor.bindSelection(VNX::CurveSelection::binding(
        sound_->guid, stored->address, stored->runtimeId()));
    EXPECT_TRUE(editor.hasBinding());
    EXPECT_FALSE(editor.hasAttenuation());
    EXPECT_EQ(editor.canvas()->profile().colorRole, VNX::CurveColorRole::ParameterBinding);
    EXPECT_TRUE(editor.findChildren<VNX::RangeField*>().isEmpty());
    EXPECT_FALSE(editor.canvas()->profile().yRangeEditable);
    EXPECT_FLOAT_EQ(editor.canvas()->profile().yMin, -96.0f);
    EXPECT_FLOAT_EQ(editor.canvas()->profile().yMax, 24.0f);
    EXPECT_FALSE(editor.canvas()->profile().valueMaximum.has_value());
    EXPECT_EQ(editor.canvas()->profile().axisScale,
              VNX::CurveAxisScale::MirroredAmplitudeDb);

    editor.bindSelection(VNX::CurveSelection::attenuation(
        component->hostItem()->guid, component->hostItem()->name, VNX::AttenuationDimension::Volume));
    EXPECT_TRUE(editor.hasAttenuation());
    EXPECT_EQ(editor.canvas()->profile().xAxisLabel, QStringLiteral("Distance"));
    EXPECT_TRUE(editor.findChildren<VNX::RangeField*>().isEmpty());
    EXPECT_FALSE(editor.canvas()->profile().yRangeEditable);
    EXPECT_FLOAT_EQ(editor.canvas()->profile().yMin, -96.0f);
    EXPECT_FLOAT_EQ(editor.canvas()->profile().yMax, 0.0f);
    EXPECT_EQ(editor.canvas()->profile().axisScale, VNX::CurveAxisScale::AmplitudeDb);
}

TEST_F(BindingCurveEditorHostTest, RehearsalPlayheadSurvivesRefresh)
{
    ASSERT_TRUE(VNX::BindingEditing::upsert(
        sound_,
        VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::volumeAddress(), variable_->guid)));
    const auto* stored = sound_->comp<VNX::VMixNode>()->bindings.findFirst(
        VNX::BindingSemantics::volumeAddress());
    ASSERT_NE(stored, nullptr);
    VNX::ProjectManager::instance().variableRehearsal().setOverride(variable_->guid, 4.0f);

    VNX::CurveEditorWidget editor;
    editor.bindSelection(VNX::CurveSelection::binding(
        sound_->guid, stored->address, stored->runtimeId()));
    ASSERT_TRUE(editor.canvas()->playhead().has_value());
    EXPECT_NEAR(*editor.canvas()->playhead(), 4.0f, 0.0001f);
    editor.refreshFromProject();
    ASSERT_TRUE(editor.canvas()->playhead().has_value());
    EXPECT_NEAR(*editor.canvas()->playhead(), 4.0f, 0.0001f);
}

TEST_F(BindingCurveEditorHostTest, VolumeEditingUsesFixedDomainAndPreservesUndo)
{
    const auto address = VNX::BindingSemantics::volumeAddress();
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_,
        VNX::BindingSemantics::defaultBinding(address, variable_->guid)));
    auto* mix = sound_->comp<VNX::VMixNode>();
    const auto original = *mix->bindings.findFirst(address);
    VNX::CurveEditorWidget editor;
    editor.bindSelection(VNX::CurveSelection::binding(sound_->guid, address, original.runtimeId()));
    EXPECT_FLOAT_EQ(mix->bindings.findFirst(address)->outputMin, original.outputMin);
    ASSERT_TRUE(editor.canvas()->setPointCoordinates(1, 10.0f, 24.0f));
    EXPECT_FLOAT_EQ(mix->bindings.findFirst(address)->curve.back().value, 24.0f);
    EXPECT_FLOAT_EQ(mix->bindings.findFirst(address)->curve.front().value, original.curve.front().value);
    EXPECT_FLOAT_EQ(mix->bindings.findFirst(address)->outputMin, -96.0f);
    EXPECT_FLOAT_EQ(mix->bindings.findFirst(address)->outputMax, 24.0f);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    EXPECT_EQ(undo->count(), 1);
    undo->undo();
    EXPECT_FLOAT_EQ(mix->bindings.findFirst(address)->outputMin, original.outputMin);
    EXPECT_FLOAT_EQ(mix->bindings.findFirst(address)->curve.back().value, original.curve.back().value);
    editor.refreshFromProject();
    EXPECT_FLOAT_EQ(editor.canvas()->profile().yMax, 24.0f);
    EXPECT_FALSE(editor.canvas()->profile().yRangeEditable);
}

TEST_F(BindingCurveEditorHostTest, NewAndResetVolumeCurvesAreStraightFromZeroToFloor)
{
    const auto address = VNX::BindingSemantics::volumeAddress();
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_,
        VNX::BindingSemantics::defaultBinding(address, variable_->guid)));
    auto* mix = sound_->comp<VNX::VMixNode>();
    const auto id = mix->bindings.findFirst(address)->runtimeId();
    VNX::CurveEditorWidget editor;
    editor.resize(640, 360);
    editor.bindSelection(VNX::CurveSelection::binding(sound_->guid, address, id));
    auto* canvas = editor.canvas();
    EXPECT_FLOAT_EQ(canvas->curve().front().value, 0.0f);
    EXPECT_FLOAT_EQ(canvas->curve().back().value, -96.0f);
    const auto data = canvas->renderData();
    const QPointF start = data.continuousPath.elementAt(0);
    const QPointF end = data.continuousPath.elementAt(data.continuousPath.elementCount() - 1);
    for (int i = 1; i < data.continuousPath.elementCount() - 1; ++i) {
        const QPointF point = data.continuousPath.elementAt(i);
        const double t = (point.x() - start.x()) / (end.x() - start.x());
        EXPECT_NEAR(point.y(), start.y() + t * (end.y() - start.y()), 0.001);
    }
    EXPECT_NEAR(VNX::BindingSemantics::contribution(*mix->bindings.find(id),
        VNX::VariableKind::Smooth, 5.0f, 0.0f, 10.0f), -6.0205f, 0.0001f);
    ASSERT_TRUE(canvas->setPointCoordinates(1, 10.0f, 12.0f));
    ASSERT_TRUE(canvas->resetCurveToDefault());
    EXPECT_FLOAT_EQ(mix->bindings.find(id)->curve.front().value, 0.0f);
    EXPECT_FLOAT_EQ(mix->bindings.find(id)->curve.back().value, -96.0f);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->undo();
    EXPECT_FLOAT_EQ(mix->bindings.find(id)->curve.back().value, 12.0f);
}

TEST_F(BindingCurveEditorHostTest, PitchRangeDoubleClickRemapsCurveWithOneUndo)
{
    const auto address = VNX::BindingSemantics::pitchAddress();
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_,
        VNX::BindingSemantics::defaultBinding(address, variable_->guid)));
    auto* mix = sound_->comp<VNX::VMixNode>();
    const auto original = *mix->bindings.findFirst(address);
    VNX::CurveEditorWidget editor;
    editor.bindSelection(VNX::CurveSelection::binding(sound_->guid, address, original.runtimeId()));
    editor.resize(360, 240);
    editor.show();
    QApplication::processEvents();
    auto* canvas = editor.canvas();
    QRectF label;
    for (const auto& tick : canvas->renderData().textTicks)
        if (tick.bound == VNX::CurveAxisBound::YMin && tick.editable)
            label = tick.rect;
    ASSERT_TRUE(label.isValid());
    QMouseEvent click(QEvent::MouseButtonDblClick, label.center(),
        canvas->mapToGlobal(label.center().toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &click);
    auto* dialog = canvas->findChild<QDialog*>(QStringLiteral("curveCanvasRangeEditor"));
    ASSERT_NE(dialog, nullptr);
    auto* input = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("curveCanvasRangeInput"));
    ASSERT_NE(input, nullptr);
    input->setValue(-1200.0);
    dialog->accept();
    EXPECT_FLOAT_EQ(mix->bindings.findFirst(address)->outputMin, -1200.0f);
    EXPECT_FLOAT_EQ(mix->bindings.findFirst(address)->curve.front().value, -1200.0f);
    EXPECT_FLOAT_EQ(mix->bindings.findFirst(address)->curve.back().value, 1200.0f);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    EXPECT_EQ(undo->count(), 1);
    undo->undo();
    EXPECT_FLOAT_EQ(mix->bindings.findFirst(address)->outputMin, original.outputMin);
}
