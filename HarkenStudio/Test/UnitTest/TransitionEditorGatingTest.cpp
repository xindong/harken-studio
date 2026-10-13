#include "Widgets/TransitionEditor/TransitionEditorPanel.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include "ActionManager.h"
#include "AuthoringPolicy.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Timeline/TimelineTypes.h"
#include "TransitionSemantics.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QToolButton>

#include <tuple>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void setMusical(VNX::ProjectItem& piece)
{
    piece.comp<VNX::VPiece>()->pieceTime = VNX::PieceTimeSpec::musical();
}

void addMarker(VNX::ProjectItem& piece, VNX::MarkerKind kind, int64_t position = 0)
{
    piece.comp<VNX::VPiece>()->markers.items.push_back(
        {QUuid::createUuid(), kind, {}, position});
}

QStandardItem* exitItem(VNX::TransitionEditorPanel& panel, VNX::TransitionExitTiming timing)
{
    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("transitionExitTiming"));
    if (!combo) return nullptr;
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    if (!model) return nullptr;
    const int row = combo->findData(static_cast<int>(timing));
    return row >= 0 ? model->item(row) : nullptr;
}

QLabel* exitNote(VNX::TransitionEditorPanel& panel)
{
    return panel.findChild<QLabel*>(QStringLiteral("transitionExitTimingNote"));
}

}

class TransitionEditorGatingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &manager, nullptr, {});

        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("TransitionGatingProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));

        container_ = manager.createItem(nullptr, QStringLiteral("Container"),
                                        VNX::ProjectItem::Type::Container);
        // Children are direct Pieces (not Sound-wrapping-Piece) so their
        // VPiece component can be shaped per test without an extra
        // Track/Sound hop - the same fixture shape WP1's
        // TransitionFieldGatingTest.cpp already uses for the domain function.
        childClocked_ = manager.createItem(container_, QStringLiteral("Clocked"),
                                           VNX::ProjectItem::Type::Piece);
        childSilent_ = manager.createItem(container_, QStringLiteral("Silent"),
                                          VNX::ProjectItem::Type::Piece);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(childClocked_, nullptr);
        ASSERT_NE(childSilent_, nullptr);
        container_->compOrCreate<VNX::VSelection>()->mode = VNX::SelectionMode::Sequence;
        container_->compOrCreate<VNX::VSelection>()->playType = VNX::SelectionPlayType::Queue;
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* childClocked_ = nullptr;
    VNX::ProjectItem* childSilent_ = nullptr;
};

TEST_F(TransitionEditorGatingTest, AbsoluteMarkerSourceKeepsSupportedExitTimings)
{
    addMarker(*childClocked_, VNX::MarkerKind::Entry);
    addMarker(*childClocked_, VNX::MarkerKind::Exit, 1920);

    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childClocked_->guid, childSilent_->guid));

    for (auto timing : {VNX::TransitionExitTiming::Immediate, VNX::TransitionExitTiming::NaturalEnd,
                        VNX::TransitionExitTiming::NextMarker, VNX::TransitionExitTiming::ExitMarker}) {
        QStandardItem* item = exitItem(panel, timing);
        ASSERT_NE(item, nullptr);
        EXPECT_TRUE(item->isEnabled()) << static_cast<int>(timing);
    }
    QLabel* note = exitNote(panel);
    ASSERT_NE(note, nullptr);
    EXPECT_TRUE(note->isHidden());
}

TEST_F(TransitionEditorGatingTest, ExitTimingNextMarkerDisabledWhenSourceHasNoMarkers)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childClocked_->guid, childSilent_->guid));

    EXPECT_EQ(exitItem(panel, VNX::TransitionExitTiming::NextBeat), nullptr);
    EXPECT_EQ(exitItem(panel, VNX::TransitionExitTiming::NextBar), nullptr);
    EXPECT_FALSE(exitItem(panel, VNX::TransitionExitTiming::NextMarker)->isEnabled());
    for (auto timing : {VNX::TransitionExitTiming::Immediate, VNX::TransitionExitTiming::NaturalEnd,
                        VNX::TransitionExitTiming::ExitMarker}) {
        QStandardItem* item = exitItem(panel, timing);
        ASSERT_NE(item, nullptr);
        EXPECT_TRUE(item->isEnabled());
    }

    QLabel* note = exitNote(panel);
    ASSERT_NE(note, nullptr);
    EXPECT_FALSE(note->isHidden());
}

TEST_F(TransitionEditorGatingTest, ExitTimingShowsBarrenReferenceNoteWhenReferenceHasNoMarkers)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childSilent_->guid, childClocked_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    // Pin the stored value to an always-available option first, so the
    // "current value itself is unavailable" warning (a separate, higher-
    // priority note - see the Stale* test below) does not mask the note
    // under test here.
    ASSERT_TRUE(panel.setRuleExitTiming(index, VNX::TransitionExitTiming::Immediate));

    EXPECT_EQ(exitItem(panel, VNX::TransitionExitTiming::NextBeat), nullptr);
    EXPECT_EQ(exitItem(panel, VNX::TransitionExitTiming::NextBar), nullptr);
    EXPECT_FALSE(exitItem(panel, VNX::TransitionExitTiming::NextMarker)->isEnabled());
    EXPECT_TRUE(exitItem(panel, VNX::TransitionExitTiming::ExitMarker)->isEnabled());

    QLabel* note = exitNote(panel);
    ASSERT_NE(note, nullptr);
    EXPECT_FALSE(note->isHidden());
}

TEST_F(TransitionEditorGatingTest, ExitTimingKeepsNextMarkerWhenSourceHasMarkerButNoClock)
{
    addMarker(*childSilent_, VNX::MarkerKind::Entry);

    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childSilent_->guid, childClocked_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    ASSERT_TRUE(panel.setRuleExitTiming(index, VNX::TransitionExitTiming::Immediate));

    EXPECT_EQ(exitItem(panel, VNX::TransitionExitTiming::NextBeat), nullptr);
    EXPECT_EQ(exitItem(panel, VNX::TransitionExitTiming::NextBar), nullptr);
    EXPECT_TRUE(exitItem(panel, VNX::TransitionExitTiming::NextMarker)->isEnabled());
    EXPECT_TRUE(exitItem(panel, VNX::TransitionExitTiming::ExitMarker)->isEnabled());
}

TEST_F(TransitionEditorGatingTest, QueueAndMatchModesShareTheSameGatingSnapshot)
{
    auto& manager = VNX::ProjectManager::instance();
    auto snapshot = [](VNX::TransitionEditorPanel& panel) {
        return std::tuple{
            panel.mountState(),
            panel.ruleCount(),
            panel.ruleExitTiming(0),
            exitItem(panel, VNX::TransitionExitTiming::NextBar) == nullptr,
            exitItem(panel, VNX::TransitionExitTiming::NextMarker)->isEnabled(),
        };
    };

    addMarker(*childClocked_, VNX::MarkerKind::Entry);
    addMarker(*childClocked_, VNX::MarkerKind::Exit, 1920);

    VNX::TransitionEditorPanel sequenceQueue;
    sequenceQueue.bindItem(container_);
    ASSERT_TRUE(sequenceQueue.addRule(childClocked_->guid, childSilent_->guid));
    const auto sequenceSnap = snapshot(sequenceQueue);

    container_->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Random;
    container_->comp<VNX::VSelection>()->playType = VNX::SelectionPlayType::Queue;
    emit manager.itemPropertyChanged(container_, QStringLiteral("mode"), nullptr);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    EXPECT_EQ(snapshot(sequenceQueue), sequenceSnap);

    container_->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Sequence;
    container_->comp<VNX::VSelection>()->playType = VNX::SelectionPlayType::Single;
    emit manager.itemPropertyChanged(container_, QStringLiteral("mode"), nullptr);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    EXPECT_EQ(sequenceQueue.mountState(), VNX::TransitionMountState::None);
    EXPECT_EQ(exitItem(sequenceQueue, VNX::TransitionExitTiming::NextBar), nullptr);
    EXPECT_TRUE(exitItem(sequenceQueue, VNX::TransitionExitTiming::NextMarker)->isEnabled());

    auto* selection = container_->comp<VNX::VSelection>();
    selection->mode = VNX::SelectionMode::Match;
    selection->matchResponse = VNX::VariableResponse::Live;
    selection->selections.shared.matchAxes.push_back(QUuid::createUuid());
    emit manager.itemPropertyChanged(container_, QStringLiteral("mode"), nullptr);
    emit manager.itemPropertyChanged(container_, QStringLiteral("selections"), nullptr);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    EXPECT_EQ(snapshot(sequenceQueue), sequenceSnap);

    selection->matchResponse = VNX::VariableResponse::AtStart;
    emit manager.itemPropertyChanged(container_, QStringLiteral("selections"), nullptr);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    EXPECT_EQ(sequenceQueue.mountState(), VNX::TransitionMountState::Idle);
    EXPECT_EQ(exitItem(sequenceQueue, VNX::TransitionExitTiming::NextBar), nullptr);
    EXPECT_TRUE(exitItem(sequenceQueue, VNX::TransitionExitTiming::NextMarker)->isEnabled());
}

TEST_F(TransitionEditorGatingTest, LegacyClockNeverRestoresRemovedExitOptions)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childClocked_->guid, childSilent_->guid));
    const int index = panel.selectedRuleIndex();
    setMusical(*childClocked_);
    ASSERT_TRUE(panel.selectRuleDisplayIndex(index));
    EXPECT_EQ(exitItem(panel, VNX::TransitionExitTiming::NextBeat), nullptr);
    EXPECT_EQ(exitItem(panel, VNX::TransitionExitTiming::NextBar), nullptr);
    EXPECT_FALSE(panel.setRuleExitTiming(index, VNX::TransitionExitTiming::NextBar));
    EXPECT_EQ(panel.ruleExitTiming(index), VNX::TransitionExitTiming::Immediate);
}

TEST_F(TransitionEditorGatingTest, LegacyMusicalRuleKeepsValueAndCanBeReplaced)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childClocked_->guid, childSilent_->guid));
    const int index = panel.selectedRuleIndex();
    auto& rules = container_->comp<VNX::VSelection>()->transitions.rules;
    for (auto& rule : rules)
        if (rule.source == childClocked_->guid && rule.target == childSilent_->guid)
            rule.exitTiming = VNX::TransitionExitTiming::NextBar;
    ASSERT_TRUE(panel.selectRuleDisplayIndex(index));
    EXPECT_EQ(panel.ruleExitTiming(index), VNX::TransitionExitTiming::NextBar);
    auto* combo = panel.findChild<QComboBox*>(QStringLiteral("transitionExitTiming"));
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentIndex(), -1);
    QLabel* note = exitNote(panel);
    ASSERT_NE(note, nullptr);
    EXPECT_FALSE(note->isHidden());
    EXPECT_EQ(note->property("textRole").toString(), QStringLiteral("warning"));
    VNX::TransitionFadeDef fade = panel.ruleFade(index, VNX::TransitionFadeSlot::SourceFadeOut);
    fade.durationSeconds = 2.0f;
    EXPECT_FALSE(panel.setRuleFade(index, VNX::TransitionFadeSlot::SourceFadeOut, fade));
    EXPECT_TRUE(panel.setRuleExitTiming(index, VNX::TransitionExitTiming::Immediate));
    EXPECT_TRUE(panel.setRuleFade(index, VNX::TransitionFadeSlot::SourceFadeOut, fade));
}

// Acceptance 4 (bridge/SamePosition mutual exclusion, insert-affordance
// side): selecting Same Position must also gate the "+ Insert a bridge"
// action with a reason, not just the picker on an already-present bridge.
TEST_F(TransitionEditorGatingTest, BridgeInsertActionDisabledWithReasonWhenSamePositionSelected)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childClocked_->guid, childSilent_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);

    auto* insertButton = panel.findChild<QToolButton*>(QStringLiteral("transitionInsertBridge"));
    ASSERT_NE(insertButton, nullptr);
    EXPECT_TRUE(insertButton->isEnabled());
    ASSERT_TRUE(panel.setRuleEntryMode(index, VNX::TransitionEntryMode::SamePosition));
    EXPECT_FALSE(insertButton->isEnabled());
}

// Acceptance 5: the default rule's endpoints are presented as dimmed static
// Any text, not an editable picker, and the domain layer backstops the UI
// gate against illegal edits.
TEST_F(TransitionEditorGatingTest, DefaultRuleEndpointsAreDimmedAndDomainRejectsEdits)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_EQ(panel.ruleCount(), 1);
    ASSERT_TRUE(panel.ruleIsDefault(0));
    ASSERT_TRUE(panel.selectRuleDisplayIndex(0));

    auto* sourceRef = panel.findChild<VNX::ObjectRefWidget*>(QStringLiteral("transitionSourceRef"));
    auto* sourceAny = panel.findChild<QLabel*>(QStringLiteral("transitionSourceAny"));
    ASSERT_NE(sourceRef, nullptr);
    ASSERT_NE(sourceAny, nullptr);
    EXPECT_TRUE(sourceRef->isHidden());
    EXPECT_FALSE(sourceAny->isHidden());

    EXPECT_FALSE(panel.setRuleEndpoints(0, childClocked_->guid, childSilent_->guid));
    EXPECT_FALSE(panel.lastInteractionError().isEmpty());
}

// Acceptance 6 / D5: None keeps the table visible and editable (with a mount
// note explaining why), and switching back to Editable leaves the data as-is.
TEST_F(TransitionEditorGatingTest, NoneMountStateKeepsTableVisibleEditableWithMountNote)
{
    container_->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Random;
    container_->comp<VNX::VSelection>()->playType = VNX::SelectionPlayType::Single;
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_EQ(panel.mountState(), VNX::TransitionMountState::None);

    const auto rows = panel.findChildren<QWidget*>(QStringLiteral("transitionListRow"));
    EXPECT_GE(rows.size(), 1);

    auto* note = panel.findChild<QLabel*>(QStringLiteral("transitionMountNote"));
    ASSERT_NE(note, nullptr);
    EXPECT_FALSE(note->isHidden());

    ASSERT_TRUE(panel.addRule(childClocked_->guid, childSilent_->guid));
    EXPECT_EQ(panel.ruleCount(), 2);

    container_->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Sequence;
    container_->comp<VNX::VSelection>()->playType = VNX::SelectionPlayType::Queue;
    emit VNX::ProjectManager::instance().itemPropertyChanged(
        container_, QStringLiteral("mode"), nullptr);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    EXPECT_EQ(panel.mountState(), VNX::TransitionMountState::Editable);
    EXPECT_EQ(panel.ruleCount(), 2);
    EXPECT_TRUE(note->isHidden());
}

// 03-design SS3.4: the footer's add/remove buttons gate on table-level state
// (no children left to combine, or the default rule selected) with a reason,
// rather than staying unconditionally enabled.
TEST_F(TransitionEditorGatingTest, FooterButtonsGateWithReasonsForOccupiedCombosAndDefaultSelection)
{
    // Gap-round rewrite: a single-child container has exactly TWO non-default
    // combinations (child/Any, Any/child). The self-pair child/child is not one of
    // them - a same-object hand-over is decided before any rule is matched, so such
    // a rule could never fire, and 03-design 2.2 asked for it to be excluded.
    auto& manager = VNX::ProjectManager::instance();
    manager.deleteItem(childSilent_);

    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    auto* addButton = panel.findChild<QToolButton*>(QStringLiteral("transitionAddRule"));
    auto* removeButton = panel.findChild<QToolButton*>(QStringLiteral("transitionRemoveRule"));
    ASSERT_NE(addButton, nullptr);
    ASSERT_NE(removeButton, nullptr);

    EXPECT_TRUE(addButton->isEnabled());
    addButton->click();
    addButton->click();
    EXPECT_EQ(panel.ruleCount(), 3);
    for (int i = 0; i < panel.ruleCount(); ++i) {
        const auto source = panel.ruleSource(i);
        const auto target = panel.ruleTarget(i);
        EXPECT_FALSE(source && target && *source == *target) << i;
    }

    EXPECT_FALSE(addButton->isEnabled());

    ASSERT_TRUE(panel.selectRuleDisplayIndex(panel.ruleCount() - 1));
    ASSERT_TRUE(panel.ruleIsDefault(panel.ruleCount() - 1));
    EXPECT_FALSE(removeButton->isEnabled());

    ASSERT_TRUE(panel.selectRuleDisplayIndex(0));
    ASSERT_FALSE(panel.ruleIsDefault(0));
    EXPECT_TRUE(removeButton->isEnabled());
}

// Gap round: the domain answered "are these two fade slots live" but nothing read
// it, so revealing the BRIDGE form without picking a bridge left both bridge fade
// slots fully editable - an edit there wrote a duration into a slot the rule has no
// bridge for. 03-design 3.3 asks for gray-out plus the reason in place.
TEST_F(TransitionEditorGatingTest, BridgeFadeSlotsAreGatedUntilABridgeIsPicked)
{
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childClocked_->guid, childSilent_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);

    auto* insert = panel.findChild<QToolButton*>(QStringLiteral("transitionInsertBridge"));
    ASSERT_NE(insert, nullptr);
    insert->click();

    auto* form = panel.findChild<QWidget*>(QStringLiteral("transitionBridgeForm"));
    ASSERT_NE(form, nullptr);
    EXPECT_FALSE(form->isHidden());

    const QStringList fadeSlots{QStringLiteral("transitionFadeBridgeIn"),
                                QStringLiteral("transitionFadeBridgeOut")};
    for (const QString& name : fadeSlots) {
        auto* button = panel.findChild<QToolButton*>(name);
        ASSERT_NE(button, nullptr) << name.toStdString();
        EXPECT_FALSE(button->isEnabled()) << name.toStdString();
    }

    ASSERT_TRUE(panel.setRuleBridge(index, childSilent_->guid));
    for (const QString& name : fadeSlots) {
        auto* button = panel.findChild<QToolButton*>(name);
        ASSERT_NE(button, nullptr) << name.toStdString();
        EXPECT_TRUE(button->isEnabled()) << name.toStdString();
    }
}

// Gap round: a rule whose source object has been deleted resolved its watch the
// same way source=Any does - upward through the organization chain - so a deleted
// source could still light up Next Beat / Next Bar off an ancestor's clock.
TEST_F(TransitionEditorGatingTest, UnresolvedSourceLeavesNoClockOrMarkerInReach)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::TransitionEditorPanel panel;
    panel.bindItem(container_);
    ASSERT_TRUE(panel.addRule(childClocked_->guid, childSilent_->guid));
    const int index = panel.selectedRuleIndex();
    ASSERT_GE(index, 0);
    ASSERT_TRUE(panel.setRuleExitTiming(index, VNX::TransitionExitTiming::Immediate));
    EXPECT_EQ(exitItem(panel, VNX::TransitionExitTiming::NextBar), nullptr);

    auto& table = container_->comp<VNX::VSelection>()->transitions;
    table.rules[VNX::TransitionSemantics::rulesForDisplay(table)[index]].source = QUuid::createUuid();
    emit manager.itemPropertyChanged(container_, QStringLiteral("transitions"));
    QCoreApplication::processEvents();
    ASSERT_TRUE(panel.selectRuleDisplayIndex(index));

    EXPECT_EQ(exitItem(panel, VNX::TransitionExitTiming::NextBeat), nullptr);
    EXPECT_EQ(exitItem(panel, VNX::TransitionExitTiming::NextBar), nullptr);
    EXPECT_FALSE(exitItem(panel, VNX::TransitionExitTiming::NextMarker)->isEnabled());
}
