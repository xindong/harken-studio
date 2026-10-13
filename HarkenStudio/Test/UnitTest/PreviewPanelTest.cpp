#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ActionManager.h"
#include "AudioEngine.h"
#include "BindingEditing.h"
#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "UndoCommands.h"
#include "Widgets/PreviewPanel/PreviewPanel.h"
#include "Widgets/PreviewPanel/RehearsalTable.h"
#include "Widgets/PreviewPanel/VariableBindingTable.h"
#include "Widgets/PreviewPanel/VariableRehearsalRow.h"
#include "Widgets/Common/AxisRehearsalRow.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/ElidedLabel.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/RehearsalSlider.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QTemporaryDir>

namespace {

VNX::VariableRehearsalRow* rowFor(QWidget* host, const VNX::ProjectItem* variable)
{
    for (auto* row : host->findChildren<VNX::VariableRehearsalRow*>())
        if (row->variableId() == variable->guid)
            return row;
    return nullptr;
}

QList<QWidget*> bindingRows(QWidget* host)
{
    return host->findChildren<QWidget*>(QStringLiteral("auditionBindingRow"));
}

QWidget* bindingRowFor(QWidget* host, const VNX::ProjectItem* variable,
                      const VNX::ProjectItem* target, const QString& property,
                      harkenstudio::ObjectId stableId = {})
{
    for (auto* row : bindingRows(host))
        if (row->property("variableGuid").toString() == variable->guid.toString(QUuid::WithoutBraces)
            && row->property("targetGuid").toString() == target->guid.toString(QUuid::WithoutBraces)
            && row->findChild<QLabel*>(QStringLiteral("auditionBindingProperty"))->toolTip() == property
            && (harkenstudio::IsNull(stableId) || row->property("bindingStableId").value<harkenstudio::ObjectId>() == stableId))
            return row;
    return nullptr;
}

QString bindingValue(QWidget* row)
{
    return row->findChild<QLabel*>(QStringLiteral("auditionBindingValue"))->toolTip();
}

void drainEvents()
{
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents();
}

}

class PreviewPanelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char arg0[] = "harken_editor_tests";
            static char* argv[] = {arg0, nullptr};
            static QApplication app(argc, argv);
        }
        style_ = qApp->styleSheet();
        qApp->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
        VNX::columnShareStore().clear();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &manager,
                                    nullptr, {});
        ASSERT_TRUE(temp_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("AuditionRehearsal");
        config.location = temp_.path();
        ASSERT_TRUE(manager.createProject(config));
        sound_ = manager.createItem(nullptr, QStringLiteral("HostSound"),
                                    VNX::ProjectItem::Type::Sound);
        smooth_ = manager.createItem(nullptr, QStringLiteral("SmoothDrive"),
                                     VNX::ProjectItem::Type::Variable);
        step_ = manager.createItem(nullptr, QStringLiteral("StepDrive"),
                                   VNX::ProjectItem::Type::Variable);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(smooth_, nullptr);
        ASSERT_NE(step_, nullptr);
        auto* smoothVariable = smooth_->comp<VNX::VVariable>();
        smoothVariable->rangeMin = 0.0f;
        smoothVariable->rangeMax = 100.0f;
        smoothVariable->defaultValue = 10.0f;
        auto* stepVariable = step_->comp<VNX::VVariable>();
        stepVariable->kind = VNX::VariableKind::Step;
        stepVariable->rangeMin = 0.0f;
        stepVariable->rangeMax = 3.0f;
        stepVariable->defaultValue = 0.0f;
        stepVariable->labels = {{1, QStringLiteral("Low")}, {2, QStringLiteral("High")}};
        ASSERT_TRUE(bind(sound_, smooth_));
        VNX::UndoRedoManager::instance().clear();
        VNX::registerBuiltinInspectorCustomizations();
        VNX::AudioEngine::instance().bindVariableRehearsal(manager.variableRehearsal());
        VNX::AudioEngine::instance().clearRehearsalPushLog();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::AudioEngine::instance().clearRehearsalPushLog();
        qApp->setStyleSheet(style_);
        VNX::columnShareStore().clear();
        drainEvents();
    }

    static bool bind(VNX::ProjectItem* host, VNX::ProjectItem* variable)
    {
        return VNX::BindingEditing::upsert(host,
            VNX::BindingSemantics::defaultBinding(
                VNX::BindingSemantics::volumeAddress(host->type), variable->guid));
    }

    QTemporaryDir temp_;
    QString style_;
    VNX::ProjectItem* sound_ = nullptr;
    VNX::ProjectItem* smooth_ = nullptr;
    VNX::ProjectItem* step_ = nullptr;
};

TEST_F(PreviewPanelTest, SelectionShowsOnlyItsDependenciesAndClears)
{
    PreviewPanel bar(nullptr);
    EXPECT_TRUE(bar.findChildren<VNX::VariableRehearsalRow*>().isEmpty());
    bar.setItem(sound_);
    EXPECT_NE(rowFor(bar.variableTable(), smooth_), nullptr);
    EXPECT_EQ(rowFor(&bar, step_), nullptr);
    ASSERT_EQ(bindingRows(bar.bindingTable()).size(), 1);
    EXPECT_TRUE(bar.findChildren<VNX::ListFooter*>().isEmpty());
    EXPECT_EQ(bar.findChild<QLabel*>(QStringLiteral("nowPlayingLabel")), nullptr);
    EXPECT_EQ(bar.findChild<QPushButton*>(QStringLiteral("auditionRehearsalHandle")), nullptr);

    auto* row = rowFor(&bar, smooth_);
    ASSERT_NE(row, nullptr);
    auto* ref = row->findChild<VNX::ObjectRefWidget*>();
    ASSERT_NE(ref, nullptr);
    EXPECT_TRUE(ref->isReadOnly());
    EXPECT_EQ(ref->currentObjectID(), smooth_->objectID);

    auto* bindingRow = bindingRows(bar.bindingTable()).front();
    auto* variableRef = bindingRow->findChild<VNX::ObjectRefWidget*>(QStringLiteral("auditionBindingVariable"));
    auto* targetRef = bindingRow->findChild<VNX::ObjectRefWidget*>(QStringLiteral("auditionBindingTarget"));
    ASSERT_NE(variableRef, nullptr);
    ASSERT_NE(targetRef, nullptr);
    EXPECT_TRUE(variableRef->isReadOnly());
    EXPECT_TRUE(targetRef->isReadOnly());
    EXPECT_EQ(variableRef->currentObjectID(), smooth_->objectID);
    EXPECT_EQ(targetRef->currentObjectID(), sound_->objectID);
    EXPECT_TRUE(bar.bindingTable()->findChildren<QDoubleSpinBox*>().isEmpty());
    EXPECT_TRUE(bar.bindingTable()->findChildren<QSlider*>().isEmpty());

    bar.setItem(step_);
    EXPECT_TRUE(bar.findChildren<VNX::VariableRehearsalRow*>().isEmpty());
    EXPECT_TRUE(bindingRows(bar.bindingTable()).isEmpty());
    bar.setItem(sound_);
    ASSERT_EQ(bar.findChildren<VNX::VariableRehearsalRow*>().size(), 1);
    bar.setItem(nullptr);
    EXPECT_TRUE(bar.findChildren<VNX::VariableRehearsalRow*>().isEmpty());
    EXPECT_TRUE(bindingRows(bar.bindingTable()).isEmpty());
}

TEST_F(PreviewPanelTest, GridAxesAndBindingsShareVariablesAndRefreshOnKindChange)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* container = manager.createItem(nullptr, QStringLiteral("Container"),
                                         VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = VNX::SelectionMode::Match;
    selection->selections.shared.matchAxes.push_back(step_->guid);
    ASSERT_TRUE(bind(container, smooth_));
    PreviewPanel bar(nullptr);
    bar.setItem(container);
    auto* stepRow = rowFor(bar.variableTable(), step_);
    ASSERT_NE(stepRow, nullptr);
    ASSERT_NE(rowFor(bar.variableTable(), smooth_), nullptr);
    EXPECT_EQ(bar.findChildren<VNX::VariableRehearsalRow*>().size(), 2);
    EXPECT_EQ(bindingRows(bar.bindingTable()).size(), 2);
    auto* discrete = bindingRowFor(&bar, step_, container, QStringLiteral("Discrete Grid Axis 1"));
    ASSERT_NE(discrete, nullptr);
    EXPECT_EQ(bindingValue(discrete), QStringLiteral("0"));
    auto* combo = rowFor(&bar, step_)->findChild<QComboBox*>(QStringLiteral("parameterValueChoice"));
    ASSERT_NE(combo, nullptr);
    EXPECT_FALSE(combo->isHidden());
    combo->setCurrentIndex(combo->findData(2));
    EXPECT_EQ(manager.variableRehearsal().overrideFor(step_->guid), 2.0f);
    EXPECT_EQ(bindingValue(discrete), QStringLiteral("2"));

    step_->comp<VNX::VVariable>()->kind = VNX::VariableKind::Smooth;
    emit manager.itemPropertyChanged(step_, QStringLiteral("kind"), nullptr);
    drainEvents();
    EXPECT_EQ(rowFor(bar.variableTable(), step_), stepRow);
    auto* spin = stepRow->findChild<QDoubleSpinBox*>();
    ASSERT_NE(spin, nullptr);
    EXPECT_FALSE(spin->isHidden());
    EXPECT_TRUE(combo->isHidden());
    EXPECT_EQ(spin->decimals(), 2);
    EXPECT_EQ(manager.variableRehearsal().overrideFor(step_->guid), 2.0f);
    manager.variableRehearsal().setOverride(step_->guid, 1.6f);
    EXPECT_EQ(bindingValue(discrete), QStringLiteral("2"));

    ASSERT_TRUE(selection->setField(QStringLiteral("mode"), static_cast<int>(VNX::SelectionMode::Blend)));
    selection->selections.shared.mixingRows.push_back({QUuid::createUuid(), smooth_->guid});
    emit manager.itemPropertyChanged(container, QStringLiteral("mode"), nullptr);
    drainEvents();
    EXPECT_EQ(rowFor(&bar, step_), nullptr);
    EXPECT_EQ(bindingRows(bar.bindingTable()).size(), 2);
    auto* continuous = bindingRowFor(&bar, smooth_, container, QStringLiteral("Continuous Grid Axis 1"));
    ASSERT_NE(continuous, nullptr);
    EXPECT_EQ(bindingValue(continuous), QStringLiteral("10"));
    manager.variableRehearsal().setOverride(smooth_->guid, 42.5f);
    EXPECT_EQ(bindingValue(continuous), QStringLiteral("42.5"));

    ASSERT_TRUE(selection->setField(QStringLiteral("mode"), static_cast<int>(VNX::SelectionMode::Random)));
    emit manager.itemPropertyChanged(container, QStringLiteral("mode"), nullptr);
    drainEvents();
    EXPECT_EQ(rowFor(&bar, step_), nullptr);
    EXPECT_NE(rowFor(&bar, smooth_), nullptr);
    EXPECT_EQ(bindingRows(bar.bindingTable()).size(), 1);
}

TEST_F(PreviewPanelTest, ActionClosureIncludesOutputParentAndSendBusesWithoutUnrelatedBuses)
{
    auto& manager = VNX::ProjectManager::instance();
    const auto busType = VNX::ProjectItem::Type::Bus;
    auto* parent = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Parent"), busType);
    auto* output = manager.createItem(parent, QStringLiteral("Output"), busType);
    auto* send = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Send"), busType);
    auto* unrelated = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Unrelated"), busType);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(send, nullptr);
    ASSERT_NE(unrelated, nullptr);
    QList<VNX::ProjectItem*> busVariables;
    for (auto* bus : {parent, output, send, unrelated}) {
        auto* variable = manager.createItem(nullptr, bus->name + QStringLiteral("Variable"),
                                             VNX::ProjectItem::Type::Variable);
        ASSERT_NE(variable, nullptr);
        ASSERT_TRUE(bind(bus, variable));
        busVariables.append(variable);
    }
    ASSERT_TRUE(bind(unrelated, smooth_));
    sound_->fieldGroups.mainOutput.shared = {VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(output->guid), 0.0f}};
    output->comp<VNX::VMixNode>()->sends.push_back({send->objectID, 1.0f});
    auto* childAction = manager.createItem(nullptr, QStringLiteral("ChildAction"),
                                           VNX::ProjectItem::Type::Action);
    auto* action = manager.createItem(nullptr, QStringLiteral("Action"), VNX::ProjectItem::Type::Action);
    ASSERT_NE(childAction, nullptr);
    ASSERT_NE(action, nullptr);
    VNX::VActionStep play;
    play.targetObjectID = sound_->objectID;
    childAction->comp<VNX::VAction>()->steps = {play};
    VNX::VActionStep post;
    post.type = VNX::VActionStep::Type::PostAction;
    post.targetObjectID = childAction->objectID;
    VNX::VActionStep set;
    set.type = VNX::VActionStep::Type::SetVariable;
    set.targetObjectID = step_->objectID;
    action->comp<VNX::VAction>()->steps = {post, set};

    PreviewPanel bar(nullptr);
    bar.setItem(action);
    EXPECT_NE(rowFor(&bar, smooth_), nullptr);
    EXPECT_NE(rowFor(&bar, step_), nullptr);
    for (int i = 0; i < 3; ++i)
        EXPECT_NE(rowFor(&bar, busVariables[i]), nullptr) << i;
    EXPECT_EQ(rowFor(&bar, busVariables[3]), nullptr);
    EXPECT_EQ(bar.findChildren<VNX::VariableRehearsalRow*>().size(), 5);
    EXPECT_EQ(bindingRows(bar.bindingTable()).size(), 4);
    EXPECT_EQ(bindingRowFor(&bar, smooth_, unrelated, QStringLiteral("Volume")), nullptr);

    post.targetObjectID = action->objectID;
    childAction->comp<VNX::VAction>()->steps.push_back(post);
    emit manager.itemPropertyChanged(childAction, QStringLiteral("steps"), nullptr);
    drainEvents();
    EXPECT_EQ(bar.findChildren<VNX::VariableRehearsalRow*>().size(), 5);
    EXPECT_EQ(bindingRows(bar.bindingTable()).size(), 4);
    bar.setItem(unrelated);
    EXPECT_EQ(bar.findChildren<VNX::VariableRehearsalRow*>().size(), 2);
    EXPECT_NE(rowFor(&bar, busVariables[3]), nullptr);
    EXPECT_EQ(bindingRows(bar.bindingTable()).size(), 2);
}

TEST_F(PreviewPanelTest, TrackClipsAndOwnedChildrenReachTheirVariables)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* piece = manager.createItem(nullptr, QStringLiteral("Piece"), VNX::ProjectItem::Type::Piece);
    ASSERT_NE(piece, nullptr);
    auto* track = manager.createItem(piece, QStringLiteral("Track"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = sound_->guid;
    track->comp<VNX::VTrack>()->clips.items.push_back(clip);
    ASSERT_TRUE(bind(track, step_));
    PreviewPanel bar(nullptr);
    bar.setItem(piece);
    EXPECT_NE(rowFor(&bar, smooth_), nullptr);
    EXPECT_NE(rowFor(&bar, step_), nullptr);
    EXPECT_EQ(bar.findChildren<VNX::VariableRehearsalRow*>().size(), 2);
}

TEST_F(PreviewPanelTest, RehearsalGestureStaysActiveAndDoesNotDirtyTheProject)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->setClean();
    smooth_->clearDirty();
    const int count = undo->count();
    PreviewPanel bar(nullptr);
    bar.setItem(sound_);
    auto* row = rowFor(&bar, smooth_);
    ASSERT_NE(row, nullptr);
    auto* slider = row->findChild<VNX::RehearsalSlider*>();
    ASSERT_NE(slider, nullptr);
    auto* binding = slider->binding();
    binding->beginEdit();
    binding->previewEdit(42);
    EXPECT_TRUE(binding->isEditing());
    EXPECT_EQ(manager.variableRehearsal().overrideFor(smooth_->guid), 42.0f);
    binding->previewEdit(75);
    EXPECT_TRUE(binding->isEditing());
    binding->commitEdit();
    EXPECT_EQ(manager.variableRehearsal().overrideFor(smooth_->guid), 75.0f);
    EXPECT_EQ(undo->count(), count);
    EXPECT_TRUE(undo->isClean());
    EXPECT_FALSE(smooth_->isDirtyObject());
    EXPECT_FLOAT_EQ(smooth_->comp<VNX::VVariable>()->defaultValue, 10.0f);
    manager.variableRehearsal().setOverride(smooth_->guid, 10.0f);
    EXPECT_FLOAT_EQ(slider->rawValue(), 10.0f);
}

TEST_F(PreviewPanelTest, DeclarationEditClampsDisplayWithoutRewritingOverride)
{
    auto& manager = VNX::ProjectManager::instance();
    PreviewPanel bar(nullptr);
    bar.setItem(sound_);
    manager.variableRehearsal().setOverride(smooth_->guid, 150.0f);
    smooth_->comp<VNX::VVariable>()->rangeMax = 50.0f;
    emit manager.itemPropertyChanged(smooth_, QStringLiteral("rangeMax"), nullptr);
    drainEvents();
    EXPECT_EQ(manager.variableRehearsal().overrideFor(smooth_->guid), 150.0f);
    auto* row = rowFor(&bar, smooth_);
    ASSERT_NE(row, nullptr);
    EXPECT_FLOAT_EQ(row->findChild<VNX::RehearsalSlider*>()->rawValue(), 50.0f);
}

TEST_F(PreviewPanelTest, BindingContributionsStaySeparateAndFollowLiveEdits)
{
    auto& manager = VNX::ProjectManager::instance();
    auto& rehearsal = manager.variableRehearsal();
    auto* mix = sound_->comp<VNX::VMixNode>();
    ASSERT_TRUE(VNX::BindingEditing::removeAll(sound_, VNX::BindingSemantics::volumeAddress()));
    auto first = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), smooth_->guid);
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_, first));
    const auto firstId = mix->bindings.entries().back().runtimeId();
    auto second = first;
    VNX::BindingSemantics::remapOutputDomain(second, -300.0f, 300.0f);
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_, second));
    const auto secondId = mix->bindings.entries().back().runtimeId();
    auto stepped = first;
    stepped.variable = step_->guid;
    VNX::BindingSemantics::remapOutputDomain(stepped, 0.0f, 600.0f);
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_, stepped));
    const auto stepId = mix->bindings.entries().back().runtimeId();
    sound_->clearDirty();
    smooth_->clearDirty();
    step_->clearDirty();
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->setClean();
    const int undoCount = undo->count();

    PreviewPanel bar(nullptr);
    bar.setItem(sound_);
    ASSERT_EQ(bindingRows(bar.bindingTable()).size(), 3);
    EXPECT_EQ(bar.variableTable()->findChildren<VNX::VariableRehearsalRow*>().size(), 2);
    const auto pitch = QStringLiteral("Pitch");
    auto* firstRow = bindingRowFor(&bar, smooth_, sound_, pitch, firstId);
    auto* secondRow = bindingRowFor(&bar, smooth_, sound_, pitch, secondId);
    auto* stepRow = bindingRowFor(&bar, step_, sound_, pitch, stepId);
    ASSERT_NE(firstRow, nullptr);
    ASSERT_NE(secondRow, nullptr);
    ASSERT_NE(stepRow, nullptr);
    EXPECT_EQ(bindingValue(firstRow), QStringLiteral("120 cent"));
    EXPECT_EQ(bindingValue(secondRow), QStringLiteral("-240 cent"));
    EXPECT_EQ(bindingValue(stepRow), QStringLiteral("0 cent"));

    auto* control = rowFor(&bar, smooth_)->findChild<VNX::RehearsalSlider*>()->binding();
    control->beginEdit();
    control->previewEdit(50.0);
    EXPECT_TRUE(control->isEditing());
    EXPECT_EQ(bindingValue(firstRow), QStringLiteral("600 cent"));
    EXPECT_EQ(bindingValue(secondRow), QStringLiteral("0 cent"));
    rehearsal.setOverride(step_->guid, 1.6f);
    EXPECT_EQ(bindingValue(stepRow), QStringLiteral("400 cent"));
    control->previewEdit(75.0);
    EXPECT_EQ(bindingValue(firstRow), QStringLiteral("900 cent"));
    EXPECT_EQ(bindingValue(secondRow), QStringLiteral("150 cent"));
    control->commitEdit();
    EXPECT_EQ(undo->count(), undoCount);
    EXPECT_TRUE(undo->isClean());
    EXPECT_FALSE(sound_->isDirtyObject());
    EXPECT_FALSE(smooth_->isDirtyObject());
    EXPECT_FALSE(step_->isDirtyObject());

    first = *mix->bindings.find(firstId);
    VNX::BindingSemantics::remapOutputDomain(first, 0.0f, 600.0f);
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_, first));
    drainEvents();
    EXPECT_EQ(bindingRowFor(&bar, smooth_, sound_, pitch, firstId), firstRow);
    EXPECT_EQ(bindingValue(firstRow), QStringLiteral("450 cent"));
    smooth_->comp<VNX::VVariable>()->rangeMax = 50.0f;
    emit manager.itemPropertyChanged(smooth_, QStringLiteral("rangeMax"), nullptr);
    drainEvents();
    EXPECT_EQ(rehearsal.overrideFor(smooth_->guid), 75.0f);
    EXPECT_EQ(bindingValue(firstRow), QStringLiteral("600 cent"));
    EXPECT_EQ(bindingValue(secondRow), QStringLiteral("300 cent"));
    rehearsal.clearAll();
    EXPECT_EQ(bindingValue(firstRow), QStringLiteral("120 cent"));
    EXPECT_EQ(bindingValue(secondRow), QStringLiteral("-180 cent"));
    EXPECT_EQ(bindingValue(stepRow), QStringLiteral("0 cent"));

    ASSERT_TRUE(VNX::BindingEditing::remove(sound_, secondId));
    drainEvents();
    EXPECT_EQ(bindingRows(bar.bindingTable()).size(), 2);
    EXPECT_EQ(bindingRowFor(&bar, smooth_, sound_, pitch, secondId), nullptr);
    ASSERT_TRUE(VNX::BindingEditing::removeAll(sound_, VNX::BindingSemantics::pitchAddress()));
    drainEvents();
    EXPECT_TRUE(bindingRows(bar.bindingTable()).isEmpty());
    EXPECT_TRUE(bar.variableTable()->findChildren<VNX::VariableRehearsalRow*>().isEmpty());
}

TEST_F(PreviewPanelTest, DeletingTheSelectionClearsItsVariables)
{
    PreviewPanel bar(nullptr);
    bar.setItem(sound_);
    auto& manager = VNX::ProjectManager::instance();
    ASSERT_TRUE(manager.deleteItem(sound_));
    drainEvents();
    EXPECT_TRUE(bar.findChildren<VNX::VariableRehearsalRow*>().isEmpty());
    EXPECT_TRUE(bindingRows(bar.bindingTable()).isEmpty());
}

TEST_F(PreviewPanelTest, ValueCellSharesSpaceAcrossSliderEditorAndScope)
{
    VNX::RehearsalTable table;
    table.setVariables({smooth_});
    table.show();
    const auto& metrics = VNX::ThemeManager::instance().metrics().table;
    for (const auto kind : {VNX::VariableKind::Smooth, VNX::VariableKind::Step,
                            VNX::VariableKind::Smooth}) {
        smooth_->comp<VNX::VVariable>()->kind = kind;
        table.setVariables({smooth_});
        for (const int width : {641, 360}) {
            table.resize(width, 220);
            drainEvents();
            table.refreshLayout();
            drainEvents();
            auto* row = rowFor(&table, smooth_);
            ASSERT_NE(row, nullptr);
            auto* cell = row->cells().at(1);
            auto* slider = cell->findChild<QSlider*>();
            auto* scope = cell->findChild<QComboBox*>(QStringLiteral("auditionRehearsalScope"));
            QWidget* editor = kind == VNX::VariableKind::Step
                ? static_cast<QWidget*>(cell->findChild<QComboBox*>(QStringLiteral("parameterValueChoice")))
                : static_cast<QWidget*>(cell->findChild<QDoubleSpinBox*>());
            ASSERT_NE(slider, nullptr);
            ASSERT_NE(scope, nullptr);
            ASSERT_NE(editor, nullptr);
            ASSERT_TRUE(editor->isVisible());
            const QRect sliderRect(slider->mapTo(cell, QPoint()), slider->size());
            const QRect editorRect(editor->mapTo(cell, QPoint()), editor->size());
            const QRect scopeRect(scope->mapTo(cell, QPoint()), scope->size());
            EXPECT_NEAR(sliderRect.width(), editorRect.width(), 1);
            EXPECT_NEAR(editorRect.width(), scopeRect.width(), 1);
            EXPECT_EQ(sliderRect.left(), metrics.cellPaddingX);
            EXPECT_EQ(editorRect.left() - sliderRect.right() - 1, metrics.cellPaddingX * 2);
            EXPECT_EQ(scopeRect.left() - editorRect.right() - 1, metrics.cellPaddingX * 2);
            EXPECT_EQ(cell->width() - scopeRect.right() - 1, metrics.cellPaddingX);
            EXPECT_GE(editorRect.top(), metrics.cellPaddingY);
            EXPECT_GE(cell->height() - editorRect.bottom() - 1, metrics.cellPaddingY);
            EXPECT_EQ(editorRect.top(), scopeRect.top());
            EXPECT_EQ(editorRect.height(), scopeRect.height());
        }
    }
}

TEST_F(PreviewPanelTest, PushPathFollowsScope)
{
    auto& rehearsal = VNX::ProjectManager::instance().variableRehearsal();
    auto& engine = VNX::AudioEngine::instance();
    engine.clearRehearsalPushLog();
    rehearsal.setScope(smooth_->guid, VNX::VariableScope::Global);
    rehearsal.setOverride(smooth_->guid, 20.0f);
    rehearsal.setOverride(step_->guid, 2.0f);
    bool sawGlobal = false;
    bool sawEmitter = false;
    for (const auto& record : engine.rehearsalPushLog()) {
        if (record.variableId == smooth_->guid) {
            EXPECT_EQ(record.path, VNX::AudioEngine::RehearsalPushPath::Global);
            sawGlobal = true;
        }
        if (record.variableId == step_->guid) {
            EXPECT_EQ(record.path, VNX::AudioEngine::RehearsalPushPath::Emitter);
            sawEmitter = true;
        }
    }
    EXPECT_TRUE(sawGlobal);
    EXPECT_TRUE(sawEmitter);
}

TEST_F(PreviewPanelTest, ClearingRehearsalResetsBothScopeSelectors)
{
    auto& rehearsal = VNX::ProjectManager::instance().variableRehearsal();
    VNX::RehearsalTable table;
    VNX::VariableRehearsalRow audition(smooth_, &table);
    VNX::AxisRehearsalRow axis(smooth_, QStringLiteral("Continuous"), 1);
    auto* auditionScope = audition.findChild<QComboBox*>(QStringLiteral("auditionRehearsalScope"));
    auto* axisScope = axis.findChild<QComboBox*>(QStringLiteral("axisRehearsalScope"));
    ASSERT_NE(auditionScope, nullptr);
    ASSERT_NE(axisScope, nullptr);
    const int global = static_cast<int>(VNX::VariableScope::Global);
    const int emitter = static_cast<int>(VNX::VariableScope::Emitter);
    for (const bool withValue : {false, true}) {
        auditionScope->setCurrentIndex(auditionScope->findData(global));
        ASSERT_EQ(axisScope->currentData().toInt(), global);
        if (withValue)
            ASSERT_TRUE(rehearsal.setOverride(smooth_->guid, 20.0f));
        rehearsal.clearAll();
        EXPECT_EQ(rehearsal.scopeFor(smooth_->guid), VNX::VariableScope::Emitter);
        EXPECT_EQ(rehearsal.overrideFor(smooth_->guid), std::nullopt);
        EXPECT_EQ(auditionScope->currentData().toInt(), emitter);
        EXPECT_EQ(axisScope->currentData().toInt(), emitter);
    }
}
