#include "ActionManager.h"
#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/GridEditor/MixingEditorWidget.h"
#include "Widgets/CurveEditor/CurveCanvas.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Common/RangeField.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

}

class VariableRehearsalInspectorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& projectManager = VNX::ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &projectManager,
                                    nullptr);
        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("VariableRehearsalProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(projectManager.createProject(config));

        container_ = projectManager.createItem(nullptr, QStringLiteral("VR_Container"),
                                               VNX::ProjectItem::Type::Container);
        child_ = projectManager.createItem(container_, QStringLiteral("VR_Child"),
                                           VNX::ProjectItem::Type::Sound);
        smooth_ = projectManager.createItem(nullptr, QStringLiteral("VR_Smooth"),
                                            VNX::ProjectItem::Type::Variable);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(child_, nullptr);
        ASSERT_NE(smooth_, nullptr);

        auto* smoothVariable = smooth_->compOrCreate<VNX::VVariable>();
        smoothVariable->rangeMin = 0.0f;
        smoothVariable->rangeMax = 100.0f;
        smoothVariable->defaultValue = 0.0f;

        auto* selection = container_->compOrCreate<VNX::VSelection>();
        selection->mode = VNX::SelectionMode::Match;
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(smooth_->guid));
        ASSERT_TRUE(grid.addRow(child_->guid));
        ASSERT_TRUE(grid.setCell(0, 0, 0));
        ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));

        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& projectManager = VNX::ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* child_ = nullptr;
    VNX::ProjectItem* smooth_ = nullptr;
};

TEST(VariableDeclaration, EveryRangeEntryPointRejectsCollapsedDomainsAtomically)
{
    VNX::VVariable variable;
    for (const auto& edit : QList<QPair<QString, QVariant>>{
             {QStringLiteral("rangeMin"), 100.0f},
             {QStringLiteral("rangeMax"), 0.0f},
             {QStringLiteral("range"), QVariantList{10.0f, 10.0f}}}) {
        const auto before = variable.toJson();
        EXPECT_FALSE(variable.setField(edit.first, edit.second));
        EXPECT_EQ(variable.toJson(), before);
    }
    ASSERT_TRUE(variable.setField(QStringLiteral("range"), QVariantList{0.1f, 0.2f}));
    const auto beforeKind = variable.toJson();
    EXPECT_FALSE(variable.setField(QStringLiteral("kind"), static_cast<int>(VNX::VariableKind::Step)));
    EXPECT_EQ(variable.toJson(), beforeKind);
    ASSERT_TRUE(variable.setField(QStringLiteral("range"), QVariantList{0.0f, 3.0f}));
    ASSERT_TRUE(variable.setField(QStringLiteral("kind"), static_cast<int>(VNX::VariableKind::Step)));
    const auto beforeRange = variable.toJson();
    EXPECT_FALSE(variable.setField(QStringLiteral("range"), QVariantList{0.1f, 0.2f}));
    EXPECT_EQ(variable.toJson(), beforeRange);
}

TEST_F(VariableRehearsalInspectorTest, RangeChangesPreserveConsumerShapeAndUndoAcrossPlatforms)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* selection = container_->comp<VNX::VSelection>();
    selection->mode = VNX::SelectionMode::Blend;
    selection->selections = {};
    VNX::MixingChildDef curve{child_->guid,
        {{0.0f, 0.0f}, {40.0f, 0.8f}, {100.0f, 0.2f}},
        {{10.0f, 0.1f, 30.0f, 0.7f}, {55.0f, 0.9f, 80.0f, 0.4f}}};
    VNX::MixingDef mixing{{{smooth_->guid, {curve}}}};
    ASSERT_TRUE(selection->selections.shared.setMixing(mixing));
    const auto platform = manager.currentProject()->platforms.front().id;
    auto platformMixing = mixing;
    platformMixing.rows.front().children.front().points[1].value = 0.5f;
    ASSERT_TRUE(selection->selections.editTable(platform).setMixing(platformMixing));
    const auto before = selection->selections;
    auto* otherContainer = manager.createItem(nullptr, QStringLiteral("Other_Container"),
        VNX::ProjectItem::Type::Container);
    ASSERT_NE(otherContainer, nullptr);
    auto* otherChild = manager.createItem(otherContainer, QStringLiteral("Other_Child"),
        VNX::ProjectItem::Type::Sound);
    ASSERT_NE(otherChild, nullptr);
    auto* otherSelection = otherContainer->comp<VNX::VSelection>();
    auto otherMixing = mixing;
    otherMixing.rows.front().children.front().target = otherChild->guid;
    ASSERT_TRUE(otherSelection->selections.shared.setMixing(otherMixing));
    const auto otherBefore = otherSelection->selections.toJson();

    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    editor.selectChild(0, 0);
    VNX::UndoRedoManager::instance().clear();
    const auto result = VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
        QJsonObject{{"authoring_handle", static_cast<qint64>(smooth_->objectID)},
                    {"property", "range"}, {"value", QJsonArray{10.0, 10.01}},
                    {"range_policy", "stretch"}});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    const auto* variable = smooth_->comp<VNX::VVariable>();
    const float span = variable->rangeMax - variable->rangeMin;
    const auto oldTables = before.tables();
    const auto newTables = selection->selections.tables();
    ASSERT_EQ(newTables.size(), oldTables.size());
    for (int table = 0; table < oldTables.size(); ++table) {
        const auto oldCurve = oldTables[table]->mixing().rows.front().children.front();
        const auto newCurve = newTables[table]->mixing().rows.front().children.front();
        for (int sample = 0; sample <= 20; ++sample) {
            const float fraction = static_cast<float>(sample) / 20.0f;
            EXPECT_NEAR(newCurve.gainAt(variable->rangeMin + fraction * span),
                        oldCurve.gainAt(fraction * 100.0f), 0.0003f);
        }
    }
    const auto after = selection->selections.toJson();
    const auto otherAfter = otherSelection->selections.toJson();
    EXPECT_NE(otherAfter, otherBefore);
    QApplication::processEvents();
    ASSERT_NE(editor.canvas(0), nullptr);
    const auto midpoint = editor.canvas(0)->curve()[1];
    EXPECT_GT(midpoint.x, variable->rangeMin);
    EXPECT_LT(midpoint.x, variable->rangeMax);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_EQ(selection->selections.toJson(), before.toJson());
    EXPECT_EQ(otherSelection->selections.toJson(), otherBefore);
    EXPECT_FLOAT_EQ(variable->rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(variable->rangeMax, 100.0f);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    EXPECT_EQ(selection->selections.toJson(), after);
    EXPECT_EQ(otherSelection->selections.toJson(), otherAfter);
    EXPECT_FLOAT_EQ(variable->rangeMin, 10.0f);
    EXPECT_FLOAT_EQ(variable->rangeMax, 10.01f);
}

TEST_F(VariableRehearsalInspectorTest, UnrepresentableRangeChangesLeaveNoPartialEdits)
{
    auto* selection = container_->comp<VNX::VSelection>();
    selection->selections = {};
    VNX::MixingChildDef curve{child_->guid,
        {{0.0f, 0.0f}, {50.0f, 1.0f}, {100.0f, 0.0f}}, {}};
    curve.normalize();
    ASSERT_TRUE(selection->selections.shared.setMixing({{{smooth_->guid, {curve}}}}));
    const auto before = selection->selections.toJson();
    const auto declaration = smooth_->comp<VNX::VVariable>()->toJson();
    VNX::UndoRedoManager::instance().clear();
    const auto result = VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
        QJsonObject{{"authoring_handle", static_cast<qint64>(smooth_->objectID)},
                    {"property", "range"}, {"value", QJsonArray{16777216.0, 16777218.0}},
                    {"range_policy", "stretch"}});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(selection->selections.toJson(), before);
    EXPECT_EQ(smooth_->comp<VNX::VVariable>()->toJson(), declaration);
    EXPECT_FALSE(VNX::UndoRedoManager::instance().canUndo());
}

TEST_F(VariableRehearsalInspectorTest, RehearsalDoesNotTouchDirtyUndoOrProjectBytes)
{
    auto& projectManager = VNX::ProjectManager::instance();
    ASSERT_TRUE(projectManager.saveProject());
    const QString projectPath = projectManager.currentProject()->projectPath;
    const QByteArray before = readFile(projectPath);
    ASSERT_FALSE(before.isEmpty());
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->setClean();
    const int count = undo->count();
    const int index = undo->index();
    const bool containerDirty = container_->isDirtyObject();
    const bool variableDirty = smooth_->isDirtyObject();

    ASSERT_TRUE(projectManager.variableRehearsal().setOverride(smooth_->guid, 62.5f));

    EXPECT_EQ(undo->count(), count);
    EXPECT_EQ(undo->index(), index);
    EXPECT_TRUE(undo->isClean());
    EXPECT_EQ(container_->isDirtyObject(), containerDirty);
    EXPECT_EQ(smooth_->isDirtyObject(), variableDirty);
    ASSERT_TRUE(projectManager.saveProject());
    EXPECT_EQ(readFile(projectPath), before);
}

TEST_F(VariableRehearsalInspectorTest, RangePoliciesPreserveConsumerFunctionsAndAtomicHistory)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* selection = container_->comp<VNX::VSelection>();
    auto* variable = smooth_->comp<VNX::VVariable>();
    variable->defaultValue = 75.0f;
    selection->mode = VNX::SelectionMode::Blend;
    selection->selections = {};
    VNX::MixingChildDef curve{child_->guid,
        {{0.0f, 0.1f}, {40.0f, 0.8f}, {100.0f, 0.3f}},
        {{10.0f, 0.2f, 30.0f, 0.7f}, {55.0f, 0.9f, 80.0f, 0.4f}}};
    ASSERT_TRUE(selection->selections.shared.setMixing({{{smooth_->guid, {curve}}}}));
    const auto platform = manager.currentProject()->platforms.front().id;
    auto outside = curve;
    outside.points = {{70.0f, 1.0f}, {100.0f, 0.5f}};
    outside.handles.clear();
    outside.normalize();
    ASSERT_TRUE(selection->selections.editTable(platform).setMixing({{{smooth_->guid, {outside}}}}));
    const auto oldSelection = selection->selections;

    auto* mix = child_->comp<VNX::VMixNode>();
    auto binding = VNX::BindingSemantics::defaultBinding(VNX::BindingSemantics::volumeAddress(), smooth_->guid);
    binding.guid = QUuid::createUuid();
    binding.curve = {{0.0f, -48.0f}, {0.4f, -6.0f}, {1.0f, -24.0f}};
    binding.handles = {{0.1f, -72.0f, 0.3f, 0.0f}, {0.5f, -3.0f, 0.8f, -48.0f}};
    ASSERT_TRUE(mix->bindings.upsert(binding));
    const auto oldBinding = *mix->bindings.find(binding.runtimeId());
    auto unrelated = VNX::BindingSemantics::defaultBinding(VNX::BindingSemantics::pitchAddress(), QUuid::createUuid());
    unrelated.guid = QUuid::createUuid();
    ASSERT_TRUE(mix->bindings.upsert(unrelated));

    auto* actionItem = manager.createItem(nullptr, QStringLiteral("Range_Action"), VNX::ProjectItem::Type::Action);
    ASSERT_NE(actionItem, nullptr);
    auto* action = actionItem->comp<VNX::VAction>();
    for (float value : {80.0f, 50.0f, 10.0f}) {
        VNX::VActionStep step;
        step.type = VNX::VActionStep::Type::SetVariable;
        step.targetObjectID = smooth_->objectID;
        step.value = value;
        action->steps.push_back(step);
    }
    const auto snapshot = [&] {
        return QJsonObject{{"variable", variable->toJson()}, {"selection", selection->selections.toJson()},
                           {"bindings", mix->bindings.toJson()}, {"action", action->toJson()}};
    };
    const auto before = snapshot();
    VNX::UndoRedoManager::instance().clear();
    std::vector<QJsonObject> observations;
    const auto connection = QObject::connect(&manager, &VNX::ProjectManager::itemPropertyChanged,
        &manager, [&](VNX::ProjectItem*, const QString&, const QObject*) { observations.push_back(snapshot()); });
    const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
    QJsonObject args{{"authoring_handle", static_cast<qint64>(smooth_->objectID)},
                     {"property", "range"}, {"value", QJsonArray{20.0, 60.0}}};
    const auto pending = VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"), args);
    EXPECT_FALSE(pending.success);
    EXPECT_EQ(pending.errorCode, QStringLiteral("VARIABLE_RANGE_POLICY_REQUIRED"));
    EXPECT_EQ(snapshot(), before);
    EXPECT_TRUE(observations.empty());
    EXPECT_FALSE(VNX::UndoRedoManager::instance().canUndo());

    for (const auto& policy : {QStringLiteral("stretch"), QStringLiteral("fix")}) {
        const bool stretch = policy == QLatin1String("stretch");
        args[QStringLiteral("range_policy")] = policy;
        const auto result = VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"), args);
        ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
        const auto after = snapshot();
        ASSERT_FALSE(observations.empty());
        for (const auto& state : observations) EXPECT_EQ(state, after);
        observations.clear();
        EXPECT_FLOAT_EQ(variable->defaultValue, stretch ? 50.0f : 60.0f);
        EXPECT_FLOAT_EQ(action->steps[0].value, stretch ? 52.0f : 60.0f);
        EXPECT_FLOAT_EQ(action->steps[1].value, stretch ? 40.0f : 50.0f);
        EXPECT_FLOAT_EQ(action->steps[2].value, stretch ? 24.0f : 20.0f);
        EXPECT_EQ(mix->bindings.find(unrelated.runtimeId())->toJson(), unrelated.toJson());
        const auto* updatedBinding = mix->bindings.find(binding.runtimeId());
        ASSERT_NE(updatedBinding, nullptr);
        const auto oldTables = oldSelection.tables();
        const auto newTables = selection->selections.tables();
        for (int sample = 0; sample <= 40; ++sample) {
            const float x = 20.0f + sample;
            const float oldX = stretch ? (x - 20.0f) * 2.5f : x;
            EXPECT_NEAR(VNX::BindingSemantics::contribution(*updatedBinding, variable->kind, x, 20.0f, 60.0f),
                        VNX::BindingSemantics::contribution(oldBinding, variable->kind, oldX, 0.0f, 100.0f), 0.001f);
            for (int table = 0; table < oldTables.size(); ++table) {
                const auto original = oldTables[table]->mixing().rows.front().children.front();
                const auto updated = newTables[table]->mixing().rows.front().children.front();
                EXPECT_EQ(updated.target, original.target);
                EXPECT_NEAR(updated.gainAt(x), original.gainAt(oldX), 0.0001f);
            }
        }
        ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
        EXPECT_EQ(snapshot(), before);
        for (const auto& state : observations) EXPECT_EQ(state, before);
        observations.clear();
        ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
        EXPECT_EQ(snapshot(), after);
        for (const auto& state : observations) EXPECT_EQ(state, after);
        observations.clear();
        ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
        observations.clear();
        VNX::UndoRedoManager::instance().clear();
    }
}

TEST_F(VariableRehearsalInspectorTest, InspectorCancelIsReadOnlyAndEachChoiceCommitsOnce)
{
    VNX::registerBuiltinInspectorCustomizations();
    auto& manager = VNX::ProjectManager::instance();
    auto* selection = container_->comp<VNX::VSelection>();
    selection->selections = {};
    selection->mode = VNX::SelectionMode::Blend;
    auto curve = VNX::MixingChildDef::unity(child_->guid, 0.0f, 100.0f);
    curve.points.back().value = 0.0f;
    curve.handles = {{0.0f, 1.0f, 100.0f, 0.0f}};
    ASSERT_TRUE(selection->selections.shared.setMixing({{{smooth_->guid, {curve}}}}));
    ASSERT_TRUE(manager.saveProject());
    VNX::UndoRedoManager::instance().clear();
    const auto before = selection->selections.toJson();
    const bool wasDirty = smooth_->isDirtyObject();
    const bool containerWasDirty = container_->isDirtyObject();
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(smooth_);
    auto* range = inspector.findChild<VNX::RangeField*>(QStringLiteral("prop_range"));
    ASSERT_NE(range, nullptr);
    int commits = 0;
    QObject::connect(&inspector, &VNX::InspectorPanel::propertyChanged, &inspector, [&] { ++commits; });
    for (const auto& choice : {QStringLiteral("cancel"), QStringLiteral("Stretch"), QStringLiteral("Fix")}) {
        bool sawDialog = false;
        QTimer::singleShot(0, &inspector, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) { ADD_FAILURE() << "Range edit did not open a dialog"; return; }
            sawDialog = true;
            EXPECT_EQ(dialog->objectName(), QStringLiteral("variableRangePolicyDialog"));
            EXPECT_FLOAT_EQ(smooth_->comp<VNX::VVariable>()->rangeMax, 100.0f);
            EXPECT_EQ(selection->selections.toJson(), before);
            if (choice == QLatin1String("cancel")) dialog->reject();
            else {
                auto* button = dialog->findChild<QPushButton*>(QStringLiteral("variableRange") + choice);
                if (!button) { ADD_FAILURE() << "Missing range policy button"; dialog->reject(); return; }
                EXPECT_TRUE(button->isEnabled());
                button->click();
            }
        });
        auto* high = range->findChild<QDoubleSpinBox*>(QStringLiteral("rangeFieldHigh"));
        ASSERT_NE(high, nullptr);
        high->setValue(50.0);
        QApplication::processEvents();
        EXPECT_TRUE(sawDialog);
        if (choice == QLatin1String("cancel")) {
            EXPECT_EQ(commits, 0);
            EXPECT_EQ(range->highValue(), 100.0);
            EXPECT_EQ(smooth_->isDirtyObject(), wasDirty);
            EXPECT_EQ(container_->isDirtyObject(), containerWasDirty);
            EXPECT_FALSE(VNX::UndoRedoManager::instance().canUndo());
        } else {
            EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
            EXPECT_FLOAT_EQ(smooth_->comp<VNX::VVariable>()->rangeMax, 50.0f);
            const auto updated = selection->mixing().rows.front().children.front();
            EXPECT_NEAR(updated.gainAt(25.0f), choice == QLatin1String("Stretch") ? 0.5f : 0.75f, 0.0001f);
            ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
            EXPECT_EQ(selection->selections.toJson(), before);
            VNX::UndoRedoManager::instance().clear();
        }
    }
    EXPECT_EQ(commits, 2);
}

TEST_F(VariableRehearsalInspectorTest, FixedBindingDomainsPreserveEndpointExtensionAndRejectPointOverflow)
{
    auto* variable = smooth_->comp<VNX::VVariable>();
    variable->rangeMin = 100.0f;
    variable->rangeMax = 200.0f;
    variable->defaultValue = 150.0f;
    container_->comp<VNX::VSelection>()->selections = {};
    auto* mix = child_->comp<VNX::VMixNode>();
    auto binding = VNX::BindingSemantics::defaultBinding(VNX::BindingSemantics::pitchAddress(), smooth_->guid);
    binding.guid = QUuid::createUuid();
    binding.curve = {{0.0f, -100.0f}, {0.5f, 50.0f}, {1.0f, 100.0f}};
    binding.handles = {{0.0f, -100.0f, 0.3f, 50.0f}, {0.7f, -50.0f, 1.0f, 100.0f}};
    ASSERT_TRUE(mix->bindings.upsert(binding));
    const auto original = *mix->bindings.find(binding.runtimeId());
    for (const auto bounds : {QJsonArray{0.0, 300.0}, QJsonArray{10.0, 20.0}, QJsonArray{250.0, 300.0}}) {
        const auto result = VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
            QJsonObject{{"authoring_handle", static_cast<qint64>(smooth_->objectID)},
                         {"property", "range"}, {"value", bounds}, {"range_policy", "fix"}});
        ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
        const auto* updated = mix->bindings.find(binding.runtimeId());
        ASSERT_NE(updated, nullptr);
        EXPECT_FLOAT_EQ(updated->curve.front().x, 0.0f);
        EXPECT_FLOAT_EQ(updated->curve.back().x, 1.0f);
        for (int sample = 0; sample <= 40; ++sample) {
            const float x = variable->rangeMin + (variable->rangeMax - variable->rangeMin) * sample / 40.0f;
            EXPECT_NEAR(VNX::BindingSemantics::contribution(*updated, variable->kind, x,
                            variable->rangeMin, variable->rangeMax),
                        VNX::BindingSemantics::contribution(original, variable->kind, x, 100.0f, 200.0f), 0.001f);
        }
        ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
        EXPECT_EQ(mix->bindings.find(binding.runtimeId())->toJson(), original.toJson());
        VNX::UndoRedoManager::instance().clear();
    }
    binding.curve.clear();
    binding.handles.clear();
    for (int point = 0; point < VNX::kCurveMaxPoints; ++point)
        binding.curve.append({point / static_cast<float>(VNX::kCurveMaxPoints - 1), point * 25.0f});
    ASSERT_TRUE(mix->bindings.upsert(binding));
    const auto before = mix->bindings.toJson();
    const auto declaration = variable->toJson();
    QJsonObject args{{"authoring_handle", static_cast<qint64>(smooth_->objectID)},
                     {"property", "range"}, {"value", QJsonArray{0.0, 300.0}}};
    const auto pending = VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"), args);
    EXPECT_EQ(pending.errorCode, QStringLiteral("VARIABLE_RANGE_POLICY_REQUIRED"));
    args[QStringLiteral("range_policy")] = QStringLiteral("fix");
    const auto rejected = VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"), args);
    EXPECT_EQ(rejected.errorCode, QStringLiteral("VARIABLE_RANGE_CURVE_CONFLICT"));
    EXPECT_EQ(variable->toJson(), declaration);
    EXPECT_EQ(mix->bindings.toJson(), before);
    EXPECT_FALSE(VNX::UndoRedoManager::instance().canUndo());
}

TEST_F(VariableRehearsalInspectorTest, RangePoliciesKeepDiscreteWritesLegalWithoutMovingMatchLabels)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* variable = smooth_->comp<VNX::VVariable>();
    variable->kind = VNX::VariableKind::Step;
    variable->defaultValue = 50.0f;
    variable->labels = {{0, QStringLiteral("Zero")}, {50, QStringLiteral("Middle")}};
    const auto oldLabels = variable->getField(QStringLiteral("labels"));
    auto* selection = container_->comp<VNX::VSelection>();
    const auto before = selection->selections.toJson();
    auto* actionItem = manager.createItem(nullptr, QStringLiteral("Step_Action"), VNX::ProjectItem::Type::Action);
    ASSERT_NE(actionItem, nullptr);
    auto* action = actionItem->comp<VNX::VAction>();
    VNX::VActionStep step;
    step.type = VNX::VActionStep::Type::SetVariable;
    step.targetObjectID = smooth_->objectID;
    step.value = 50.0f;
    action->steps = {step};
    VNX::UndoRedoManager::instance().clear();
    for (const auto& policy : {QStringLiteral("stretch"), QStringLiteral("fix")}) {
        const auto declaration = variable->toJson();
        QJsonObject args{{"authoring_handle", static_cast<qint64>(smooth_->objectID)},
                         {"property", "range"}, {"value", QJsonArray{1.0, 8.0}}, {"range_policy", policy}};
        const auto blocked = VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"), args);
        EXPECT_EQ(blocked.errorCode, QStringLiteral("VARIABLE_RANGE_CONFLICT"));
        EXPECT_EQ(variable->toJson(), declaration);
        EXPECT_FLOAT_EQ(action->steps.front().value, 50.0f);
        EXPECT_FALSE(VNX::UndoRedoManager::instance().canUndo());
        args[QStringLiteral("allow_invalid_references")] = true;
        const auto accepted = VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"), args);
        ASSERT_TRUE(accepted.success) << accepted.errorMessage.toStdString();
        EXPECT_FLOAT_EQ(variable->defaultValue, policy == QLatin1String("stretch") ? 5.0f : 8.0f);
        EXPECT_FLOAT_EQ(action->steps.front().value, variable->defaultValue);
        EXPECT_EQ(variable->getField(QStringLiteral("labels")), oldLabels);
        EXPECT_EQ(selection->selections.toJson(), before);
        ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
        EXPECT_EQ(variable->toJson(), declaration);
        EXPECT_FLOAT_EQ(action->steps.front().value, 50.0f);
        VNX::UndoRedoManager::instance().clear();
    }
}
