#include "Widgets/TransitionEditor/TransitionEditorPanel.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/TableColumnLayout.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/CurveEditor/CurveCanvas.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/TransitionEditor/TransitionFadeButton.h"

#include "ActionManager.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "Components/VTrack.h"
#include "PlacementContext.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Timeline/TimelineCoordinates.h"
#include "Timeline/TimelineTypes.h"
#include "TransitionRehearsal.h"
#include "TransitionSemantics.h"
#include "UndoCommands.h"
#include "VariableRehearsal.h"
#include "Core/QtControlSkin.h"

#include <gtest/gtest.h>

#include <algorithm>

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QToolButton>

namespace {

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
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

QDialog* openFadeDuration(VNX::TransitionFadePopup* popup)
{
    auto* canvas = popup->findChild<VNX::CurveCanvas*>();
    if (!canvas)
        return nullptr;
    for (const auto& tick : canvas->renderData().textTicks) {
        if (tick.bound != VNX::CurveAxisBound::XMax)
            continue;
        const QPointF point = tick.rect.center();
        QMouseEvent click(QEvent::MouseButtonDblClick, point, canvas->mapToGlobal(point.toPoint()),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &click);
        for (auto* dialog : canvas->findChildren<QDialog*>(QStringLiteral("curveCanvasRangeEditor")))
            if (dialog->isVisible())
                return dialog;
    }
    return nullptr;
}

bool editFadeDuration(VNX::TransitionFadePopup* popup, double value)
{
    auto* dialog = openFadeDuration(popup);
    if (!dialog)
        return false;
    drainEvents();
    EXPECT_TRUE(popup->isVisible()) << "after opening input";
    auto* input = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("curveCanvasRangeInput"));
    if (!input)
        return false;
    input->setValue(value);
    dialog->accept();
    EXPECT_TRUE(popup->isVisible()) << "after accepting input";
    return true;
}

void setNominal(VNX::ProjectItem& piece, int64_t entry, int64_t exit)
{
    piece.comp<VNX::VPiece>()->markers.items = {
        {QUuid::createUuid(), VNX::MarkerKind::Entry, {}, entry},
        {QUuid::createUuid(), VNX::MarkerKind::Exit, {}, exit}
    };
}

int otherRuleIndex(VNX::TransitionEditorPanel& panel, int excludeIndex)
{
    for (int i = 0; i < panel.ruleCount(); ++i) {
        if (i != excludeIndex)
            return i;
    }
    return -1;
}

void expectRuleColumnWidths(VNX::TransitionEditorPanel& panel, const QList<double>& weights)
{
    auto* table = panel.findChild<VNX::TableWidget*>(QStringLiteral("listBox"));
    ASSERT_NE(table, nullptr);
    QList<QLabel*> headers;
    for (int i = 0; i < 6; ++i) headers.append(table->headerCell(i));
    const int span = headers[1]->width() + headers[2]->width()
        + headers[3]->width() + headers[4]->width();
    const double totalWeight = weights[0] + weights[1] + weights[2] + weights[3];
    for (int i = 0; i < weights.size(); ++i)
        EXPECT_NEAR(headers[i + 1]->width(), span * weights[i] / totalWeight, 2) << i;
    const auto rows = panel.findChildren<QWidget*>(QStringLiteral("transitionListRow"));
    ASSERT_FALSE(rows.isEmpty());
    for (auto* row : rows) {
        ASSERT_EQ(row->layout()->count(), headers.size());
        for (int i = 0; i < headers.size(); ++i) {
            auto* cell = row->layout()->itemAt(i)->widget();
            ASSERT_NE(cell, nullptr);
            EXPECT_EQ(cell->width(), headers[i]->width()) << i;
            EXPECT_EQ(cell->mapToGlobal(QPoint()).x(), headers[i]->mapToGlobal(QPoint()).x()) << i;
        }
    }
}

void clearTransitionColumnShares()
{
    VNX::columnShareStore().remove(QStringLiteral("Transition.Target"));
    VNX::columnShareStore().remove(QStringLiteral("Transition.Source"));
    VNX::columnShareStore().remove(QStringLiteral("Transition.Exit"));
    VNX::columnShareStore().remove(QStringLiteral("Transition.Entry"));
}

}

class TransitionEditorPanelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        clearTransitionColumnShares();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &manager, nullptr, {});

        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("TransitionPanelProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));

        container_ = manager.createItem(nullptr, QStringLiteral("Container"),
                                        VNX::ProjectItem::Type::Container);
        childA_ = manager.createItem(container_, QStringLiteral("ChildA"),
                                     VNX::ProjectItem::Type::Sound);
        childB_ = manager.createItem(container_, QStringLiteral("ChildB"),
                                     VNX::ProjectItem::Type::Sound);
        childC_ = manager.createItem(container_, QStringLiteral("ChildC"),
                                     VNX::ProjectItem::Type::Sound);
        nonContainer_ = manager.createItem(nullptr, QStringLiteral("Loose"),
                                           VNX::ProjectItem::Type::Sound);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(childA_, nullptr);
        ASSERT_NE(childB_, nullptr);
        ASSERT_NE(childC_, nullptr);
        ASSERT_NE(nonContainer_, nullptr);
        container_->compOrCreate<VNX::VSelection>()->mode = VNX::SelectionMode::Sequence;
        container_->compOrCreate<VNX::VSelection>()->playType = VNX::SelectionPlayType::Queue;
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        clearTransitionColumnShares();
    }

    QUndoStack* undoStack() const
    {
        return VNX::UndoRedoManager::instance().undoStack();
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* childA_ = nullptr;
    VNX::ProjectItem* childB_ = nullptr;
    VNX::ProjectItem* childC_ = nullptr;
    VNX::ProjectItem* nonContainer_ = nullptr;
};


TEST_F(TransitionEditorPanelTest, ExistingRulesStartUnselectedAndCancellationSurvivesRebinding)
{
    {
        VNX::TransitionEditorPanel setup;
        setup.bindItem(container_);
        ASSERT_TRUE(setup.addRule(childA_->guid, childB_->guid));
        EXPECT_GE(setup.selectedRuleIndex(), 0);
    }
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    panel.show();
    drainEvents();
    EXPECT_EQ(panel.selectedRuleIndex(), -1);
    ASSERT_TRUE(panel.selectRuleDisplayIndex(0));
    EXPECT_EQ(panel.selectedRuleIndex(), 0);
    auto* editor = panel.findChild<QWidget*>("TransitionEditorWidget");
    ASSERT_NE(editor, nullptr);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &escape);
    EXPECT_EQ(panel.selectedRuleIndex(), -1);
    panel.bindItem(container_);
    EXPECT_EQ(panel.selectedRuleIndex(), -1);
    ASSERT_TRUE(panel.selectRuleDisplayIndex(0));
    panel.findChild<VNX::TableWidget*>()->clearSelection();
    EXPECT_EQ(panel.selectedRuleIndex(), -1);
    ASSERT_TRUE(panel.addRule(childB_->guid, childC_->guid));
    EXPECT_GE(panel.selectedRuleIndex(), 0);
}

TEST_F(TransitionEditorPanelTest, EndpointPickerAndMutationRejectNonChildren)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    ASSERT_TRUE(panel.selectRuleDisplayIndex(0));

    auto* source = panel.findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("transitionSourceRef"));
    auto* target = panel.findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("transitionTargetRef"));
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    EXPECT_FALSE(source->canAcceptObject(nonContainer_->objectID));
    EXPECT_FALSE(target->canAcceptObject(nonContainer_->objectID));
    EXPECT_FALSE(panel.setRuleEndpoints(0, nonContainer_->guid, childB_->guid));
    EXPECT_EQ(panel.ruleSource(0), std::optional<QUuid>(childA_->guid));
}

TEST_F(TransitionEditorPanelTest, BridgePickerAcceptsProjectContentObjects)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* attenuation = manager.createItem(nullptr, QStringLiteral("Attenuation"),
                                                        VNX::ProjectItem::Type::Attenuation);
    ASSERT_NE(attenuation, nullptr);

    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    ASSERT_TRUE(panel.selectRuleDisplayIndex(0));

    auto* bridge = panel.findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("transitionBridgeRef"));
    ASSERT_NE(bridge, nullptr);
    EXPECT_TRUE(bridge->canAcceptObject(childC_->objectID));
    EXPECT_TRUE(bridge->canAcceptObject(nonContainer_->objectID));
    EXPECT_FALSE(bridge->canAcceptObject(attenuation->objectID));
    EXPECT_TRUE(panel.setRuleBridge(0, nonContainer_->guid));
}

TEST_F(TransitionEditorPanelTest, AcceptsContainerOnly)
{
    EXPECT_TRUE(VNX::TransitionEditorPanel::acceptsItem(container_));
    EXPECT_FALSE(VNX::TransitionEditorPanel::acceptsItem(nonContainer_));
    EXPECT_FALSE(VNX::TransitionEditorPanel::acceptsItem(nullptr));
}

TEST_F(TransitionEditorPanelTest, BindingNonContainerItemShowsUnavailableState)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(nonContainer_);
    EXPECT_EQ(panel.mountState(), VNX::TransitionMountState::None);
}

TEST_F(TransitionEditorPanelTest, MatchVariableResponseDecidesIdleOrEditable)
{
    auto* selection = container_->compOrCreate<VNX::VSelection>();
    selection->mode = VNX::SelectionMode::Match;
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    EXPECT_EQ(panel.mountState(), VNX::TransitionMountState::None);

    selection->selections.shared.matchAxes.push_back(QUuid::createUuid());
    emit VNX::ProjectManager::instance().itemPropertyChanged(
        container_, QStringLiteral("selections"), nullptr);
    drainEvents();
    EXPECT_EQ(panel.mountState(), VNX::TransitionMountState::Idle);
    selection->matchResponse = VNX::VariableResponse::Live;
    emit VNX::ProjectManager::instance().itemPropertyChanged(
        container_, QStringLiteral("variableResponse"), nullptr);
    drainEvents();
    EXPECT_EQ(panel.mountState(), VNX::TransitionMountState::Editable);
}

TEST_F(TransitionEditorPanelTest, TransitionsDataPersistsAcrossPlayTypeChanges)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    ASSERT_EQ(panel.ruleCount(), 2);

    container_->comp<VNX::VSelection>()->playType = VNX::SelectionPlayType::Single;
    emit VNX::ProjectManager::instance().itemPropertyChanged(
        container_, QStringLiteral("playType"), nullptr);
    drainEvents();
    EXPECT_EQ(panel.mountState(), VNX::TransitionMountState::None);
    EXPECT_EQ(panel.ruleCount(), 2);

    container_->comp<VNX::VSelection>()->playType = VNX::SelectionPlayType::Queue;
    emit VNX::ProjectManager::instance().itemPropertyChanged(
        container_, QStringLiteral("playType"), nullptr);
    drainEvents();
    EXPECT_EQ(panel.mountState(), VNX::TransitionMountState::Editable);
    EXPECT_EQ(panel.ruleCount(), 2);
}

TEST_F(TransitionEditorPanelTest, RuleTableOrdersBySpecificityDescendingWithDimmedAnyCells)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    ASSERT_TRUE(panel.addRule(childA_->guid, std::nullopt));
    ASSERT_TRUE(panel.addRule(std::nullopt, childB_->guid));
    ASSERT_EQ(panel.ruleCount(), 4);

    EXPECT_EQ(panel.ruleSpecificity(0), 3);
    EXPECT_EQ(panel.ruleSource(0), std::optional<QUuid>(childA_->guid));
    EXPECT_EQ(panel.ruleTarget(0), std::optional<QUuid>(childB_->guid));
    EXPECT_EQ(panel.ruleSpecificity(1), 2);
    EXPECT_EQ(panel.ruleSpecificity(2), 1);
    EXPECT_EQ(panel.ruleSpecificity(3), 0);
    EXPECT_TRUE(panel.ruleIsDefault(3));

    // The rule table renders one "transitionListRow" per display-order rule and
    // no group-header rows (specificity reads from Any-cell dimming instead).
    const auto rows = panel.findChildren<QWidget*>(QStringLiteral("transitionListRow"));
    EXPECT_EQ(rows.size(), 4);

    ASSERT_FALSE(rows.isEmpty());
    bool sawDimmedAny = false;
    for (QLabel* cell : rows.back()->findChildren<QLabel*>()) {
        if (cell->text() == QStringLiteral("Any")
            && cell->property("textRole").toString() == QStringLiteral("secondary")) {
            sawDimmedAny = true;
            break;
        }
    }
    EXPECT_TRUE(sawDimmedAny);
}

TEST_F(TransitionEditorPanelTest, DefaultRuleCannotBeRemoved)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_EQ(panel.ruleCount(), 1);
    EXPECT_TRUE(panel.ruleIsDefault(0));
    EXPECT_FALSE(panel.removeRule(0));
    EXPECT_TRUE(panel.lastInteractionError().contains(QStringLiteral("Default")));
    EXPECT_EQ(panel.ruleCount(), 1);
}

TEST_F(TransitionEditorPanelTest, DuplicateCombinationIsOccupiedAndRejected)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    EXPECT_TRUE(panel.isCombinationOccupied(childA_->guid, childB_->guid));
    EXPECT_TRUE(panel.isCombinationOccupied(std::nullopt, std::nullopt));
    EXPECT_FALSE(panel.isCombinationOccupied(childA_->guid, std::nullopt));

    const int undoCount = undoStack()->count();
    EXPECT_FALSE(panel.addRule(childA_->guid, childB_->guid));
    EXPECT_TRUE(panel.lastInteractionError().contains(QStringLiteral("duplicated")));
    EXPECT_EQ(undoStack()->count(), undoCount);
}

TEST_F(TransitionEditorPanelTest, AddAndRemoveRuleRoundtripUndoRedo)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    const int baseline = undoStack()->count();

    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    EXPECT_EQ(panel.ruleCount(), 2);
    EXPECT_EQ(undoStack()->count(), baseline + 1);

    ASSERT_TRUE(panel.removeRule(0));
    EXPECT_EQ(panel.ruleCount(), 1);
    EXPECT_EQ(undoStack()->count(), baseline + 2);

    undoStack()->undo();
    EXPECT_EQ(panel.ruleCount(), 2);
    undoStack()->undo();
    EXPECT_EQ(panel.ruleCount(), 1);
    undoStack()->redo();
    EXPECT_EQ(panel.ruleCount(), 2);
    undoStack()->redo();
    EXPECT_EQ(panel.ruleCount(), 1);
}

// D5 (WP5): None means "not consumed by the runtime", not "read-only" - an
// author can prepare the hand-over table before switching modes, so edits
// stay allowed in every mount state. This replaces the pre-D5 assertion that
// edits were rejected while mountState() == None.
TEST_F(TransitionEditorPanelTest, EditsAreAllowedWhenMountStateIsNone)
{
    container_->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Random;
    container_->comp<VNX::VSelection>()->playType = VNX::SelectionPlayType::Single;
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_EQ(panel.mountState(), VNX::TransitionMountState::None);

    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    EXPECT_EQ(panel.ruleCount(), 2);
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    EXPECT_TRUE(panel.removeRule(index));
    EXPECT_EQ(panel.ruleCount(), 1);
}

TEST_F(TransitionEditorPanelTest, CandidateChildrenAreDirectChildrenOnly)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* nested = manager.createItem(
        container_, QStringLiteral("Nested"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(nested, nullptr);
    VNX::ProjectItem* grandchild = manager.createItem(
        nested, QStringLiteral("Grandchild"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(grandchild, nullptr);

    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_EQ(panel.candidateCount(), 4);
    QSet<QUuid> guids;
    for (int i = 0; i < panel.candidateCount(); ++i)
        guids.insert(panel.candidateGuid(i));
    EXPECT_TRUE(guids.contains(childA_->guid));
    EXPECT_TRUE(guids.contains(childB_->guid));
    EXPECT_TRUE(guids.contains(childC_->guid));
    EXPECT_TRUE(guids.contains(nested->guid));
    EXPECT_FALSE(guids.contains(grandchild->guid));
}

TEST_F(TransitionEditorPanelTest, RuleSummaryTextKeepsEndpointNames)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));

    const QString summary = panel.ruleSummaryText(0);
    EXPECT_TRUE(summary.contains(QStringLiteral("ChildA")));
    EXPECT_TRUE(summary.contains(QStringLiteral("ChildB")));
}

TEST_F(TransitionEditorPanelTest, PinFollowToggleEmitsSignalAndUpdatesState)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    EXPECT_FALSE(panel.isPinned());

    int emitCount = 0;
    bool lastValue = false;
    QObject::connect(&panel, &VNX::TransitionEditorPanel::pinToggled,
                      [&emitCount, &lastValue](bool pinned) {
        ++emitCount;
        lastValue = pinned;
    });
    panel.setPinned(true);
    EXPECT_TRUE(panel.isPinned());
    EXPECT_EQ(emitCount, 1);
    EXPECT_TRUE(lastValue);
}

TEST_F(TransitionEditorPanelTest, SnapshotStateRoundTripsBoundItem)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    const QVariantMap state = panel.snapshotState();

    VNX::TransitionEditorPanel restored;
    restored.restoreState(state);
    EXPECT_EQ(restored.currentItemGuid(), container_->guid);
    EXPECT_EQ(restored.boundItem(), container_);
}

TEST_F(TransitionEditorPanelTest, ProjectReloadRestoresBindingByGuid)
{
    auto& manager = VNX::ProjectManager::instance();
    const QUuid guid = container_->guid;
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    VNX::ProjectItem* oldPointer = container_;
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    drainEvents();

    ASSERT_NE(panel.boundItem(), nullptr);
    EXPECT_NE(panel.boundItem(), oldPointer);
    EXPECT_EQ(panel.currentItemGuid(), guid);
    EXPECT_EQ(panel.boundItem()->guid, guid);
}

TEST_F(TransitionEditorPanelTest, DeletingBoundItemClearsBinding)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(manager.deleteItem(container_));
    EXPECT_EQ(panel.boundItem(), nullptr);
    EXPECT_TRUE(panel.currentItemGuid().isNull());
    EXPECT_EQ(panel.mountState(), VNX::TransitionMountState::None);
}

TEST_F(TransitionEditorPanelTest, DeletingAncestorOfBoundItemClearsBinding)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* nestedContainer = manager.createItem(
        container_, QStringLiteral("NestedContainer"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(nestedContainer, nullptr);
    VNX::TransitionEditorPanel panel;
    panel.bindItem(nestedContainer);
    ASSERT_TRUE(manager.deleteItem(container_));
    EXPECT_EQ(panel.boundItem(), nullptr);
    EXPECT_TRUE(panel.currentItemGuid().isNull());
}

TEST_F(TransitionEditorPanelTest, SetRuleEndpointsChangesSourceAndTargetWithUndo)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    const int undoCount = undoStack()->count();

    ASSERT_TRUE(panel.setRuleEndpoints(index, childA_->guid, childC_->guid));
    EXPECT_EQ(undoStack()->count(), undoCount + 1);
    index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    EXPECT_EQ(panel.ruleSource(index), std::optional<QUuid>(childA_->guid));
    EXPECT_EQ(panel.ruleTarget(index), std::optional<QUuid>(childC_->guid));

    undoStack()->undo();
    index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    EXPECT_EQ(panel.ruleTarget(index), std::optional<QUuid>(childB_->guid));
}

TEST_F(TransitionEditorPanelTest, SetRuleExitTimingUpdatesTiming)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);

    ASSERT_TRUE(panel.setRuleExitTiming(index, VNX::TransitionExitTiming::NextMarker));
    EXPECT_EQ(panel.ruleExitTiming(index), VNX::TransitionExitTiming::NextMarker);
}

TEST_F(TransitionEditorPanelTest, SetRuleFadeUpdatesOnlyTargetedSlot)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);

    VNX::TransitionFadeDef fade = panel.ruleFade(index, VNX::TransitionFadeSlot::SourceFadeOut);
    fade.durationSeconds = 1.5f;
    ASSERT_TRUE(panel.setRuleFade(index, VNX::TransitionFadeSlot::SourceFadeOut, fade));

    EXPECT_FLOAT_EQ(
        panel.ruleFade(index, VNX::TransitionFadeSlot::SourceFadeOut).durationSeconds, 1.5f);
    EXPECT_FLOAT_EQ(
        panel.ruleFade(index, VNX::TransitionFadeSlot::BridgeFadeIn).durationSeconds, 0.5f);
    EXPECT_FLOAT_EQ(
        panel.ruleFade(index, VNX::TransitionFadeSlot::BridgeFadeOut).durationSeconds, 0.5f);
    EXPECT_FLOAT_EQ(
        panel.ruleFade(index, VNX::TransitionFadeSlot::TargetFadeIn).durationSeconds, 0.5f);
}

TEST_F(TransitionEditorPanelTest, BridgeAndSamePositionAreMutuallyExclusiveBothOrders)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);

    ASSERT_TRUE(panel.setRuleBridge(index, childC_->guid));
    EXPECT_FALSE(panel.setRuleEntryMode(index, VNX::TransitionEntryMode::SamePosition));
    EXPECT_EQ(panel.ruleEntryMode(index), VNX::TransitionEntryMode::EntryMarker);

    ASSERT_TRUE(panel.setRuleBridge(index, std::nullopt));
    ASSERT_TRUE(panel.setRuleEntryMode(index, VNX::TransitionEntryMode::SamePosition));
    EXPECT_FALSE(panel.setRuleBridge(index, childC_->guid));
    EXPECT_FALSE(panel.ruleBridge(index).has_value());
}

TEST_F(TransitionEditorPanelTest, RuleBridgeDeletionClearsReferenceAndUndoRestoresIt)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    ASSERT_TRUE(panel.setRuleBridge(index, childC_->guid));

    const QUuid bridgeGuid = childC_->guid;
    ASSERT_TRUE(manager.deleteItem(childC_));
    EXPECT_FALSE(panel.ruleBridge(index).has_value());
    EXPECT_EQ(manager.findItemByGuid(bridgeGuid), nullptr);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_EQ(panel.ruleBridge(index), std::optional<QUuid>(bridgeGuid));
    EXPECT_NE(manager.findItemByGuid(bridgeGuid), nullptr);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    EXPECT_FALSE(panel.ruleBridge(index).has_value());
    EXPECT_EQ(manager.findItemByGuid(bridgeGuid), nullptr);
}

TEST_F(TransitionEditorPanelTest, EntryModeComboDisablesSamePositionWhenBridgeSet)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);

    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("transitionEntryMode"));
    ASSERT_NE(combo, nullptr);
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    ASSERT_NE(model, nullptr);
    auto* bridgeWidget = panel.findChild<QWidget*>(QStringLiteral("transitionBridgeRef"));
    ASSERT_NE(bridgeWidget, nullptr);
    auto* pickButton = bridgeWidget->findChild<QToolButton*>(QStringLiteral("refPick"));
    ASSERT_NE(pickButton, nullptr);
    const int samePositionRow = combo->findData(
        static_cast<int>(VNX::TransitionEntryMode::SamePosition));
    ASSERT_GE(samePositionRow, 0);

    EXPECT_TRUE(model->item(samePositionRow)->isEnabled());
    EXPECT_FALSE(pickButton->isHidden());

    ASSERT_TRUE(panel.setRuleBridge(index, childC_->guid));
    const int rowWithBridge = panel.selectedRuleIndex();
    const int otherIndex = otherRuleIndex(panel, rowWithBridge);
    ASSERT_GE(otherIndex, 0);
    ASSERT_TRUE(panel.selectRuleDisplayIndex(otherIndex));
    ASSERT_TRUE(panel.selectRuleDisplayIndex(rowWithBridge));
    EXPECT_FALSE(model->item(samePositionRow)->isEnabled());

    ASSERT_TRUE(panel.setRuleBridge(index, std::nullopt));
    const int rowNoBridge = panel.selectedRuleIndex();
    ASSERT_TRUE(panel.selectRuleDisplayIndex(otherRuleIndex(panel, rowNoBridge)));
    ASSERT_TRUE(panel.selectRuleDisplayIndex(rowNoBridge));
    combo->setCurrentIndex(samePositionRow);
    EXPECT_TRUE(pickButton->isHidden());
}

TEST_F(TransitionEditorPanelTest, SamePositionReadoutKeepsResolvedTargetName)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);

    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    VNX::ProjectItem* pieceSource = manager.createItem(
        container_, QStringLiteral("PieceSource"), VNX::ProjectItem::Type::Piece);
    VNX::ProjectItem* pieceTarget = manager.createItem(
        container_, QStringLiteral("PieceTarget"), VNX::ProjectItem::Type::Piece);
    ASSERT_NE(pieceSource, nullptr);
    ASSERT_NE(pieceTarget, nullptr);
    setNominal(*pieceSource, 0, 10 * VNX::TimelineCoordinates::kNanosecondsPerSecond);
    setNominal(*pieceTarget, 0, 4 * VNX::TimelineCoordinates::kNanosecondsPerSecond);

    ASSERT_TRUE(panel.addRule(pieceSource->guid, pieceTarget->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    EXPECT_TRUE(panel.samePositionReadoutText(index).contains(pieceTarget->name));
}

TEST_F(TransitionEditorPanelTest, HandoverReadoutKeepsWinnerEndpointNames)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));

    manager.transitionRehearsal().refreshWinner(container_->guid, childB_->guid);
    EXPECT_TRUE(panel.handoverReadoutText().contains(QStringLiteral("ChildA")));
    EXPECT_TRUE(panel.handoverReadoutText().contains(QStringLiteral("ChildB")));
}

TEST_F(TransitionEditorPanelTest, RehearsalRefreshesNeverTouchUndoStackOrDirtyState)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    const int undoCount = undoStack()->count();

    container_->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Sequence;
    container_->comp<VNX::VSelection>()->playType = VNX::SelectionPlayType::Queue;
    emit VNX::ProjectManager::instance().itemPropertyChanged(
        container_, QStringLiteral("mode"), nullptr);
    drainEvents();

    EXPECT_EQ(undoStack()->count(), undoCount);
}

TEST_F(TransitionEditorPanelTest, SetPlacementContextSwapsAtomicallyWithNoResidualState)
{
    const VNX::PlacementContext placementA = VNX::PlacementContext::fromOccurrence(
        {QUuid::createUuid(), QUuid::createUuid(), QUuid::createUuid()});
    const VNX::PlacementContext placementB = VNX::PlacementContext::fromOccurrence(
        {QUuid::createUuid(), QUuid::createUuid(), QUuid::createUuid()});

    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);

    panel.setPlacementContext(placementA);
    ASSERT_NE(panel.placementContext(), nullptr);
    EXPECT_EQ(*panel.placementContext(), placementA);

    panel.setPlacementContext(placementB);
    ASSERT_NE(panel.placementContext(), nullptr);
    EXPECT_EQ(*panel.placementContext(), placementB);
    EXPECT_FALSE(*panel.placementContext() == placementA);

    panel.setPlacementContext(std::nullopt);
    EXPECT_EQ(panel.placementContext(), nullptr);
}

TEST_F(TransitionEditorPanelTest, RebindingItemClearsStalePlacementContext)
{
    const VNX::PlacementContext placement = VNX::PlacementContext::fromOccurrence(
        {QUuid::createUuid(), QUuid::createUuid(), QUuid::createUuid()});

    VNX::TransitionEditorPanel panel;

    panel.bindItem(container_);
    panel.setPlacementContext(placement);
    ASSERT_NE(panel.placementContext(), nullptr);
    panel.bindItem(childA_);
    EXPECT_EQ(panel.placementContext(), nullptr);

    panel.bindItem(container_);
    panel.setPlacementContext(placement);
    ASSERT_NE(panel.placementContext(), nullptr);
    panel.bindGuid(childA_->guid);
    EXPECT_EQ(panel.placementContext(), nullptr);

    panel.bindItem(container_);
    panel.setPlacementContext(placement);
    ASSERT_NE(panel.placementContext(), nullptr);
    panel.clearEditor();
    EXPECT_EQ(panel.placementContext(), nullptr);
}

TEST_F(TransitionEditorPanelTest, SnapshotStateRoundTripsValidPlacementContext)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* placementPiece = manager.createItem(
        nullptr, QStringLiteral("PlacementPiece"), VNX::ProjectItem::Type::Piece);
    VNX::ProjectItem* placementTrack = manager.createItem(
        placementPiece, QStringLiteral("PlacementTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(placementPiece, nullptr);
    ASSERT_NE(placementTrack, nullptr);
    const VNX::Clip clip{QUuid::createUuid(), container_->guid, 0, 1};
    placementTrack->compOrCreate<VNX::VTrack>()->clips.items.push_back(clip);
    const VNX::PlacementContext placement = VNX::PlacementContext::fromOccurrence(
        {placementTrack->guid, clip.clipGuid, container_->guid});

    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    panel.setPlacementContext(placement);

    const QVariantMap state = panel.snapshotState();
    const QVariantList placementList = state.value(QStringLiteral("placement")).toList();
    ASSERT_EQ(placementList.size(), 1);
    const QVariantMap entry = placementList.front().toMap();
    EXPECT_FALSE(QUuid::fromString(entry.value(QStringLiteral("track")).toString()).isNull());
    EXPECT_FALSE(QUuid::fromString(entry.value(QStringLiteral("clip")).toString()).isNull());
    EXPECT_FALSE(QUuid::fromString(entry.value(QStringLiteral("target")).toString()).isNull());

    VNX::TransitionEditorPanel restored;
    restored.restoreState(state);
    ASSERT_EQ(restored.boundItem(), container_);
    ASSERT_NE(restored.placementContext(), nullptr);
    EXPECT_EQ(*restored.placementContext(), placement);
}

TEST_F(TransitionEditorPanelTest, RestoreStateFallsBackWhenSnapshottedPlacementBecomesInvalid)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* placementPiece = manager.createItem(
        nullptr, QStringLiteral("StalePiece"), VNX::ProjectItem::Type::Piece);
    VNX::ProjectItem* placementTrack = manager.createItem(
        placementPiece, QStringLiteral("StaleTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(placementPiece, nullptr);
    ASSERT_NE(placementTrack, nullptr);
    const VNX::Clip clip{QUuid::createUuid(), container_->guid, 0, 1};
    placementTrack->compOrCreate<VNX::VTrack>()->clips.items.push_back(clip);
    const VNX::PlacementContext placement = VNX::PlacementContext::fromOccurrence(
        {placementTrack->guid, clip.clipGuid, container_->guid});

    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    panel.setPlacementContext(placement);
    const QVariantMap state = panel.snapshotState();

    placementTrack->comp<VNX::VTrack>()->clips.items.clear();

    VNX::TransitionEditorPanel restored;
    restored.restoreState(state);
    ASSERT_EQ(restored.boundItem(), container_);
    EXPECT_EQ(restored.placementContext(), nullptr);
}

TEST_F(TransitionEditorPanelTest, WatchViewRefreshesSynchronouslyOnVariableRehearsalChange)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* variable = manager.createItem(
        nullptr, QStringLiteral("WatchVariable"), VNX::ProjectItem::Type::Variable);
    VNX::ProjectItem* watchContainer = manager.createItem(
        nullptr, QStringLiteral("WatchContainer"), VNX::ProjectItem::Type::Container);
    VNX::ProjectItem* watchA = manager.createItem(
        watchContainer, QStringLiteral("WatchA"), VNX::ProjectItem::Type::Piece);
    VNX::ProjectItem* watchB = manager.createItem(
        watchContainer, QStringLiteral("WatchB"), VNX::ProjectItem::Type::Piece);
    ASSERT_NE(variable, nullptr);
    ASSERT_NE(watchContainer, nullptr);
    ASSERT_NE(watchA, nullptr);
    ASSERT_NE(watchB, nullptr);
    auto* declaration = variable->comp<VNX::VVariable>();
    declaration->kind = VNX::VariableKind::Step;
    declaration->rangeMin = 0.0f;
    declaration->rangeMax = 10.0f;

    auto* selection = watchContainer->comp<VNX::VSelection>();
    selection->mode = VNX::SelectionMode::Match;
    selection->matchResponse = VNX::VariableResponse::Live;
    auto grid = selection->matchGrid();
    ASSERT_TRUE(grid.addAxis(variable->guid));
    ASSERT_TRUE(grid.addRow(watchA->guid));
    ASSERT_TRUE(grid.addRow(watchB->guid));
    ASSERT_TRUE(grid.setCell(0, 0, 0));
    ASSERT_TRUE(grid.setCell(1, 0, 5));
    ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));

    VNX::TransitionEditorPanel panel;
    panel.bindItem(watchContainer);
    EXPECT_TRUE(panel.watchView().hasSource);
    ASSERT_TRUE(panel.watchView().found());
    EXPECT_EQ(*panel.watchView().referencePiece, watchA->guid);

    int emitCount = 0;
    VNX::TransitionSemantics::TransitionWatchView lastView;
    QObject::connect(&panel, &VNX::TransitionEditorPanel::watchViewChanged,
                     [&emitCount, &lastView](
                         const VNX::TransitionSemantics::TransitionWatchView& view) {
        ++emitCount;
        lastView = view;
    });

    ASSERT_TRUE(manager.variableRehearsal().setOverride(variable->guid, 5.0f));
    EXPECT_EQ(emitCount, 1);
    ASSERT_TRUE(lastView.hasSource);
    ASSERT_TRUE(lastView.found());
    EXPECT_EQ(*lastView.referencePiece, watchB->guid);
    auto* watchLabel = panel.findChild<QLabel*>(QStringLiteral("transitionWatch"));
    ASSERT_NE(watchLabel, nullptr);
    // WP13: the Watch row no longer displays the domain-layer caption verbatim;
    // it is built from the structured direction/path fields instead. Rewrite,
    // not delete - assert on the resolved reference name that must survive.
    EXPECT_TRUE(watchLabel->text().contains(watchB->name));
    EXPECT_EQ(panel.watchView().containerGuid, watchContainer->guid);
}

TEST_F(TransitionEditorPanelTest, SettingPlacementContextNeverWritesRehearsalState)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));

    const std::optional<QUuid> winnerBefore =
        manager.transitionRehearsal().currentWinner(container_->guid);

    const VNX::PlacementContext placementA = VNX::PlacementContext::fromOccurrence(
        {QUuid::createUuid(), QUuid::createUuid(), QUuid::createUuid()});
    const VNX::PlacementContext placementB = VNX::PlacementContext::fromOccurrence(
        {QUuid::createUuid(), QUuid::createUuid(), QUuid::createUuid()});
    panel.setPlacementContext(placementA);
    panel.setPlacementContext(placementB);
    panel.setPlacementContext(std::nullopt);

    EXPECT_EQ(manager.transitionRehearsal().currentWinner(container_->guid), winnerBefore);
}

// WP6 refresh granularity: commitTable() now forwards `this` as the action
// originator, so the panel's own edit is fully applied synchronously and the
// later queued itemPropertyChanged echo is skipped instead of forcing a full
// rebuild() - which previously reset every CollapsibleSection back to its
// width-based default on every single edit, discarding manual expand/collapse.
TEST_F(TransitionEditorPanelTest, CommittingRuleEditDoesNotForceSectionsBackToDefaultCollapse)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    ASSERT_TRUE(panel.selectRuleDisplayIndex(0));

    auto* resolution = panel.findChild<VNX::CollapsibleSection*>(
        QStringLiteral("transitionResolutionSection"));
    ASSERT_NE(resolution, nullptr);
    const bool flipped = !resolution->isExpanded();
    resolution->setExpanded(flipped);
    ASSERT_EQ(resolution->isExpanded(), flipped);

    ASSERT_TRUE(panel.setRuleExitTiming(0, VNX::TransitionExitTiming::NaturalEnd));
    drainEvents();

    EXPECT_EQ(resolution->isExpanded(), flipped);
}

// WP6 refresh granularity: itemCreated now filters for relevance (only a direct
// child of the bound Container matters); an unrelated item created anywhere
// else in the project must not trigger a full rebuild() of the rule table.
TEST_F(TransitionEditorPanelTest, UnrelatedItemCreationElsewhereDoesNotRebuildRuleRows)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));

    auto* row = panel.findChild<QWidget*>(QStringLiteral("transitionListRow"));
    ASSERT_NE(row, nullptr);
    QPointer<QWidget> guard(row);

    VNX::ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Unrelated"), VNX::ProjectItem::Type::Sound);
    drainEvents();

    EXPECT_TRUE(guard) << "an unrelated item's creation must not trigger a full rebuild";
}

TEST_F(TransitionEditorPanelTest, RejectionReasonDoesNotSurviveRebinding)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    EXPECT_FALSE(panel.addRule(std::nullopt, std::nullopt));
    auto* editor = panel.findChild<VNX::TransitionEditorWidget*>();
    ASSERT_NE(editor, nullptr);
    EXPECT_FALSE(panel.lastInteractionError().isEmpty());

    panel.bindItem(container_);
    EXPECT_TRUE(panel.lastInteractionError().isEmpty());

    EXPECT_FALSE(panel.addRule(std::nullopt, std::nullopt));
    panel.clearEditor();
    EXPECT_TRUE(panel.lastInteractionError().isEmpty());
}

// Gap round: only the panel itself overrode focusInEvent, but the panel is a
// dense form - clicks land on a combo, an ObjectRefWidget or a rule row, never
// on the panel - so lastFocusedPanelId never moved and "follow focus" followed
// whichever instance happened to be first in the registry.
TEST_F(TransitionEditorPanelTest, FocusOnAnyChildWidgetReportsPanelFocus)
{
    VNX::TransitionEditorPanel panel;
    panel.setPanelId(7);
    panel.bindItem(container_);

    int focused = 0;
    QObject::connect(&panel, &VNX::TransitionEditorPanel::panelFocused,
                     [&](int id) { if (id == 7) ++focused; });

    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("transitionExitTiming"));
    ASSERT_NE(combo, nullptr);
    QFocusEvent focusIn(QEvent::FocusIn, Qt::MouseFocusReason);
    QCoreApplication::sendEvent(combo, &focusIn);
    EXPECT_GE(focused, 1);
}

// Gap round: an unresolved endpoint rendered in the primary text role with the
// literal "(missing)", indistinguishable from a resolved name. 03-design 2.2
// asks for the error role plus the GUID it still points at.
TEST_F(TransitionEditorPanelTest, UnresolvedEndpointCellUsesErrorRoleAndShowsGuid)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    const QUuid lost = QUuid::createUuid();
    auto& table = container_->comp<VNX::VSelection>()->transitions;
    table.rules[VNX::TransitionSemantics::rulesForDisplay(table)[panel.selectedRuleIndex()]].source = lost;
    emit manager.itemPropertyChanged(container_, QStringLiteral("transitions"));
    QCoreApplication::processEvents();

    const auto rows = panel.findChildren<QWidget*>(QStringLiteral("transitionListRow"));
    ASSERT_FALSE(rows.isEmpty());
    bool sawError = false;
    for (QWidget* row : rows) {
        for (QLabel* cell : row->findChildren<QLabel*>()) {
            if (cell->property("textRole").toString() != QStringLiteral("error"))
                continue;
            sawError = true;
            EXPECT_TRUE(lost.toString(QUuid::WithoutBraces).startsWith(cell->text()));
            EXPECT_TRUE(cell->toolTip().contains(lost.toString(QUuid::WithoutBraces)));
        }
    }
    EXPECT_TRUE(sawError);
}

TEST_F(TransitionEditorPanelTest, AdaptiveLayoutDoesNotImposeMinimumWidth)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.selectRuleDisplayIndex(0));
    auto* bridge = panel.findChild<VNX::CollapsibleSection*>(
        QStringLiteral("transitionBridgeSection"));
    ASSERT_NE(bridge, nullptr);
    auto* longChild = VNX::ProjectManager::instance().createItem(
        container_,
        QStringLiteral("VeryLongChildNameThatWouldForceAMinimumWidthIfObjectRefLeaked"),
        VNX::ProjectItem::Type::Sound);
    ASSERT_NE(longChild, nullptr);
    ASSERT_TRUE(panel.addRule(longChild->guid, childB_->guid));
    ASSERT_TRUE(panel.selectRuleDisplayIndex(0));
    bridge->expand();
    drainEvents();
    ASSERT_TRUE(bridge->isExpanded());
    bridge->collapse();
    drainEvents();
    ASSERT_FALSE(bridge->isExpanded());
}

TEST_F(TransitionEditorPanelTest, RuleOrdinalColumnMatchesDisplayOrderAndHasNoReorderHandle)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    ASSERT_TRUE(panel.addRule(childA_->guid, std::nullopt));
    ASSERT_EQ(panel.ruleCount(), 3);
    EXPECT_EQ(panel.ruleSpecificity(0), 3);
    EXPECT_EQ(panel.ruleSpecificity(1), 2);
    EXPECT_TRUE(panel.ruleIsDefault(2));
}

TEST_F(TransitionEditorPanelTest, RuleTableColumnSharesPersistAcrossReopen)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    panel.resize(900, 600);
    panel.show();
    drainEvents();
    VNX::columnShareStore().insert(QStringLiteral("Transition.Source"), 360);
    VNX::columnShareStore().insert(QStringLiteral("Transition.Target"), 300);
    VNX::columnShareStore().insert(QStringLiteral("Transition.Exit"), 180);
    VNX::columnShareStore().insert(QStringLiteral("Transition.Entry"), 160);

    VNX::TransitionEditorPanel second;
    second.bindItem(container_);
    second.resize(900, 600);
    second.show();
    drainEvents();
    expectRuleColumnWidths(second, {360, 300, 180, 160});

    clearTransitionColumnShares();
}

TEST_F(TransitionEditorPanelTest, RuleTableColumnSharesApplyAndResetThroughResizer)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    panel.resize(800, 480);
    panel.show();
    drainEvents();

    auto* head = panel.findChild<QWidget*>(QStringLiteral("listHead"));
    auto* headSource = panel.findChild<QLabel*>(QStringLiteral("transitionHeadSource"));
    auto* headTarget = panel.findChild<QLabel*>(QStringLiteral("transitionHeadTarget"));
    auto* headExit = panel.findChild<QLabel*>(QStringLiteral("transitionHeadExit"));
    auto* headEntry = panel.findChild<QLabel*>(QStringLiteral("transitionHeadEntry"));
    ASSERT_NE(head, nullptr);
    ASSERT_NE(headSource, nullptr);
    ASSERT_NE(headTarget, nullptr);
    ASSERT_NE(headExit, nullptr);
    ASSERT_NE(headEntry, nullptr);
    const QList<int> before{headSource->width(), headTarget->width(), headExit->width(), headEntry->width()};
    ASSERT_GT(before[0], 70);
    ASSERT_GT(before[1], 100);

    const QPoint edge(headSource->geometry().right() + 1, headSource->geometry().center().y());
    const QPoint dragged = edge + QPoint(40, 0);
    QMouseEvent press(QEvent::MouseButtonPress, edge, head->mapToGlobal(edge),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    ASSERT_TRUE(QCoreApplication::sendEvent(head, &press));
    QMouseEvent move(QEvent::MouseMove, dragged, head->mapToGlobal(dragged),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    ASSERT_TRUE(QCoreApplication::sendEvent(head, &move));
    QMouseEvent release(QEvent::MouseButtonRelease, dragged, head->mapToGlobal(dragged),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    ASSERT_TRUE(QCoreApplication::sendEvent(head, &release));
    drainEvents();

    EXPECT_NEAR(headSource->width(), before[0] + 40, 2);
    EXPECT_NEAR(headTarget->width(), before[1] - 40, 2);
    EXPECT_NEAR(headExit->width(), before[2], 2);
    EXPECT_NEAR(headEntry->width(), before[3], 2);
    QList<double> saved;
    for (const QString& name : {"Source", "Target", "Exit", "Entry"}) {
        const auto key = QStringLiteral("Transition.") + name;
        ASSERT_TRUE(VNX::columnShareStore().contains(key));
        saved.append(VNX::columnShareStore().value(key));
    }
    expectRuleColumnWidths(panel, saved);
    VNX::TransitionEditorPanel reopened;
    reopened.bindItem(container_);
    reopened.resize(800, 480);
    reopened.show();
    drainEvents();
    expectRuleColumnWidths(reopened, saved);

    const QPoint resetEdge(headSource->geometry().right() + 1, headSource->geometry().center().y());
    QMouseEvent dbl(QEvent::MouseButtonDblClick, resetEdge, head->mapToGlobal(resetEdge),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    ASSERT_TRUE(QCoreApplication::sendEvent(head, &dbl));
    drainEvents();
    for (const QString& name : {"Source", "Target", "Exit", "Entry"})
        EXPECT_FALSE(VNX::columnShareStore().contains(QStringLiteral("Transition.") + name));
}

TEST_F(TransitionEditorPanelTest, RuleTableNormalizesLargeSavedWeightsWithoutLosingTarget)
{
    VNX::columnShareStore().insert(QStringLiteral("Transition.Source"), 500);
    VNX::columnShareStore().insert(QStringLiteral("Transition.Exit"), 400);
    VNX::columnShareStore().insert(QStringLiteral("Transition.Entry"), 400);
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    panel.resize(1000, 600);
    panel.show();
    drainEvents();
    expectRuleColumnWidths(panel, {500, 280, 400, 400});
}

TEST_F(TransitionEditorPanelTest, RuleTableHeadLetsClicksThrough)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    panel.show();
    panel.resize(640, 480);
    drainEvents();

    auto* head = panel.findChild<QWidget*>(QStringLiteral("listHead"));
    auto* headNum = panel.findChild<QLabel*>(QStringLiteral("transitionHeadNum"));
    auto* headSource = panel.findChild<QLabel*>(QStringLiteral("transitionHeadSource"));
    auto* headTarget = panel.findChild<QLabel*>(QStringLiteral("transitionHeadTarget"));
    ASSERT_NE(head, nullptr);
    ASSERT_NE(headNum, nullptr);
    ASSERT_NE(headSource, nullptr);
    ASSERT_NE(headTarget, nullptr);
    EXPECT_TRUE(headNum->testAttribute(Qt::WA_TransparentForMouseEvents));
    EXPECT_TRUE(headSource->testAttribute(Qt::WA_TransparentForMouseEvents));

}

TEST_F(TransitionEditorPanelTest, FadePopupShowsObjectContextAndDomainReadout)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    ASSERT_TRUE(panel.setRuleBridge(index, childC_->guid));
    panel.show();
    drainEvents();
    if (auto* bridge = panel.findChild<VNX::CollapsibleSection*>(
            QStringLiteral("transitionBridgeSection")))
        bridge->expand();
    drainEvents();

    struct SlotCase {
        const char* buttonName;
        const char* objectName;
    };
    const SlotCase cases[] = {
        {"transitionFadeSourceOut", "ChildA"},
        {"transitionFadeBridgeIn", "ChildC"},
        {"transitionFadeBridgeOut", "ChildC"},
        {"transitionFadeTargetIn", "ChildB"},
    };
    for (const SlotCase& slot : cases) {
        auto* button = panel.findChild<QToolButton*>(QString::fromLatin1(slot.buttonName));
        ASSERT_NE(button, nullptr) << slot.buttonName;
        button->click();
        drainEvents();
        auto* popup = panel.findChild<VNX::TransitionFadePopup*>(
            QStringLiteral("transitionFadePopup"));
        ASSERT_NE(popup, nullptr) << slot.buttonName;
        auto* objectName = popup->findChild<QLabel*>(QStringLiteral("transitionFadeObjectName"));
        auto* canvas = popup->findChild<VNX::CurveCanvas*>();
        ASSERT_NE(objectName, nullptr) << slot.buttonName;
        ASSERT_NE(canvas, nullptr) << slot.buttonName;
        EXPECT_EQ(objectName->text(), QString::fromLatin1(slot.objectName)) << slot.buttonName;
        auto* reset = popup->findChild<QToolButton*>(QStringLiteral("transitionFadeReset"));
        ASSERT_NE(reset, nullptr) << slot.buttonName;
        EXPECT_FALSE(reset->isEnabled()) << slot.buttonName;
        popup->close();
        drainEvents();
    }
}

TEST_F(TransitionEditorPanelTest, FadePopupShowsAnyWhenEndpointIsOpen)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, std::nullopt));
    panel.show();
    drainEvents();

    auto* button = panel.findChild<QToolButton*>(QStringLiteral("transitionFadeTargetIn"));
    ASSERT_NE(button, nullptr);
    button->click();
    drainEvents();
    auto* popup = panel.findChild<VNX::TransitionFadePopup*>(
        QStringLiteral("transitionFadePopup"));
    ASSERT_NE(popup, nullptr);
    popup->close();
}

TEST_F(TransitionEditorPanelTest, FadePopupDurationEditUpdatesAxisLabel)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    panel.show();
    drainEvents();

    auto* button = panel.findChild<QToolButton*>(QStringLiteral("transitionFadeSourceOut"));
    ASSERT_NE(button, nullptr);
    button->click();
    drainEvents();
    auto* popup = panel.findChild<VNX::TransitionFadePopup*>(
        QStringLiteral("transitionFadePopup"));
    ASSERT_NE(popup, nullptr);
    auto* canvas = popup->findChild<VNX::CurveCanvas*>();
    EXPECT_TRUE(popup->findChildren<QDoubleSpinBox*>().isEmpty());
    ASSERT_NE(canvas, nullptr);

    QPointer<VNX::TransitionFadePopup> popupGuard(popup);
    ASSERT_TRUE(editFadeDuration(popup, 1.25));
    drainEvents();
    ASSERT_TRUE(popupGuard);
    ASSERT_TRUE(popup->isVisible());
    EXPECT_FLOAT_EQ(canvas->xMax(), 1.25f);
    EXPECT_FLOAT_EQ(
        panel.ruleFade(panel.selectedRuleIndex(), VNX::TransitionFadeSlot::SourceFadeOut)
            .durationSeconds,
        1.25f);

    auto* dialog = openFadeDuration(popup);
    ASSERT_NE(dialog, nullptr);
    auto* duration = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("curveCanvasRangeInput"));
    ASSERT_NE(duration, nullptr);
    duration->setValue(2.5);
    dialog->reject();
    drainEvents();
    EXPECT_FLOAT_EQ(canvas->xMax(), 1.25f);
    EXPECT_FLOAT_EQ(
        panel.ruleFade(panel.selectedRuleIndex(), VNX::TransitionFadeSlot::SourceFadeOut)
            .durationSeconds,
        1.25f);
    ASSERT_TRUE(editFadeDuration(popup, 0.0));
    drainEvents();
    ASSERT_TRUE(popupGuard);
    EXPECT_EQ(canvas->profile().xMaxEditValue, std::optional<float>(0.0f));
    dialog = openFadeDuration(popup);
    ASSERT_NE(dialog, nullptr);
    EXPECT_DOUBLE_EQ(dialog->findChild<QDoubleSpinBox*>(QStringLiteral("curveCanvasRangeInput"))->value(), 0.0);
    dialog->reject();
    ASSERT_TRUE(editFadeDuration(popup, 0.5));
    drainEvents();
    EXPECT_FLOAT_EQ(canvas->xMax(), 0.5f);
    popup->close();
}

TEST_F(TransitionEditorPanelTest, FadePopupResetRestoresCanonicalFadeOnce)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    VNX::TransitionFadeDef custom = panel.ruleFade(index, VNX::TransitionFadeSlot::SourceFadeOut);
    custom.durationSeconds = 1.5f;
    ASSERT_TRUE(panel.setRuleFade(index, VNX::TransitionFadeSlot::SourceFadeOut, custom));
    const int undoCount = undoStack()->count();
    panel.show();
    drainEvents();

    auto* button = panel.findChild<QToolButton*>(QStringLiteral("transitionFadeSourceOut"));
    ASSERT_NE(button, nullptr);
    button->click();
    drainEvents();
    auto* popup = panel.findChild<VNX::TransitionFadePopup*>(
        QStringLiteral("transitionFadePopup"));
    ASSERT_NE(popup, nullptr);
    auto* reset = popup->findChild<QToolButton*>(QStringLiteral("transitionFadeReset"));
    auto* canvas = popup->findChild<VNX::CurveCanvas*>();
    ASSERT_NE(reset, nullptr);
    ASSERT_NE(canvas, nullptr);
    EXPECT_TRUE(reset->isEnabled());

    reset->click();
    drainEvents();
    EXPECT_EQ(undoStack()->count(), undoCount + 1);
    EXPECT_EQ(panel.ruleFade(panel.selectedRuleIndex(), VNX::TransitionFadeSlot::SourceFadeOut),
              VNX::canonicalTransitionFade());
    EXPECT_FLOAT_EQ(canvas->xMax(), 0.5f);
    EXPECT_FALSE(reset->isEnabled());

    reset->click();
    drainEvents();
    EXPECT_EQ(undoStack()->count(), undoCount + 1);
    popup->close();
}

TEST_F(TransitionEditorPanelTest, FadePopupResetAfterCurveEditDisablesReset)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    panel.show();
    drainEvents();

    auto* button = panel.findChild<QToolButton*>(QStringLiteral("transitionFadeSourceOut"));
    ASSERT_NE(button, nullptr);
    button->click();
    drainEvents();
    auto* popup = panel.findChild<VNX::TransitionFadePopup*>(
        QStringLiteral("transitionFadePopup"));
    ASSERT_NE(popup, nullptr);
    auto* canvas = popup->findChild<VNX::CurveCanvas*>();
    auto* reset = popup->findChild<QToolButton*>(QStringLiteral("transitionFadeReset"));
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(reset, nullptr);
    EXPECT_FALSE(reset->isEnabled());
    ASSERT_GE(canvas->curve().size(), 2);
    ASSERT_TRUE(canvas->setPointCoordinates(1, canvas->curve().at(1).x, 0.4f));
    drainEvents();
    EXPECT_TRUE(reset->isEnabled());

    reset->click();
    drainEvents();
    EXPECT_FALSE(reset->isEnabled());
    EXPECT_EQ(panel.ruleFade(panel.selectedRuleIndex(), VNX::TransitionFadeSlot::SourceFadeOut),
              VNX::canonicalTransitionFade());
    popup->close();
}

TEST_F(TransitionEditorPanelTest, FadePopupShowAtKeepsFrameOnScreen)
{
    const QRect available(0, 0, 1280, 720);
    const QSize popupSize(400, 280);
    const QRect fitted = VNX::TransitionFadePopup::fittedFrame(
        QPoint(available.right() - 40, available.bottom() - 30), popupSize, available);
    EXPECT_TRUE(available.contains(fitted));
    EXPECT_EQ(fitted.size(), popupSize);

    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    panel.show();
    drainEvents();
    auto* button = panel.findChild<QToolButton*>(QStringLiteral("transitionFadeSourceOut"));
    ASSERT_NE(button, nullptr);
    button->click();
    drainEvents();
    auto* popup = panel.findChild<VNX::TransitionFadePopup*>(
        QStringLiteral("transitionFadePopup"));
    ASSERT_NE(popup, nullptr);
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen) {
        popup->showAt(screen->availableGeometry().bottomRight() - QPoint(20, 20));
        drainEvents();
        EXPECT_TRUE(screen->availableGeometry().contains(popup->geometry()));
    }
    popup->close();
}

TEST_F(TransitionEditorPanelTest, FadePopupReloadsWhenModelChanges)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    panel.show();
    drainEvents();

    auto* button = panel.findChild<QToolButton*>(QStringLiteral("transitionFadeSourceOut"));
    ASSERT_NE(button, nullptr);
    button->click();
    drainEvents();
    auto* popup = panel.findChild<VNX::TransitionFadePopup*>(
        QStringLiteral("transitionFadePopup"));
    ASSERT_NE(popup, nullptr);
    auto* canvas = popup->findChild<VNX::CurveCanvas*>();
    ASSERT_NE(canvas, nullptr);

    VNX::TransitionFadeDef fade = panel.ruleFade(index, VNX::TransitionFadeSlot::SourceFadeOut);
    fade.durationSeconds = 2.0f;
    ASSERT_TRUE(panel.setRuleFade(index, VNX::TransitionFadeSlot::SourceFadeOut, fade));
    drainEvents();
    EXPECT_FLOAT_EQ(canvas->xMax(), 2.0f);
    popup->close();
}

TEST_F(TransitionEditorPanelTest, FadePopupCommitFollowsRuleIdentityAfterReorder)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childA_->guid, childB_->guid));
    ASSERT_TRUE(panel.addRule(childA_->guid, std::nullopt));
    int specific = -1;
    int anyTarget = -1;
    for (int i = 0; i < panel.ruleCount(); ++i) {
        if (panel.ruleSource(i) == std::optional<QUuid>(childA_->guid)
            && panel.ruleTarget(i) == std::optional<QUuid>(childB_->guid))
            specific = i;
        if (panel.ruleSource(i) == std::optional<QUuid>(childA_->guid)
            && !panel.ruleTarget(i).has_value())
            anyTarget = i;
    }
    ASSERT_GE(specific, 0);
    ASSERT_GE(anyTarget, 0);
    ASSERT_TRUE(panel.selectRuleDisplayIndex(specific));
    panel.show();
    drainEvents();

    auto* button = panel.findChild<QToolButton*>(QStringLiteral("transitionFadeSourceOut"));
    ASSERT_NE(button, nullptr);
    button->click();
    drainEvents();
    auto* popup = panel.findChild<VNX::TransitionFadePopup*>(
        QStringLiteral("transitionFadePopup"));
    ASSERT_NE(popup, nullptr);

    ASSERT_TRUE(panel.addRule(childA_->guid, childC_->guid));
    drainEvents();

    ASSERT_TRUE(editFadeDuration(popup, 1.75));
    drainEvents();

    int updated = -1;
    for (int i = 0; i < panel.ruleCount(); ++i) {
        if (panel.ruleSource(i) == std::optional<QUuid>(childA_->guid)
            && panel.ruleTarget(i) == std::optional<QUuid>(childB_->guid))
            updated = i;
    }
    ASSERT_GE(updated, 0);
    EXPECT_FLOAT_EQ(
        panel.ruleFade(updated, VNX::TransitionFadeSlot::SourceFadeOut).durationSeconds, 1.75f);
    popup->close();
}
