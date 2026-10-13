#include "UnitTest/ObjectIdTestSupport.h"
#include "ActionManager.h"
#include "AuthoringPolicy.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "PlayablePropertyAuthoringV1.h"
#include "PlaybackReferenceValidation.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Timeline/TimelineEditing.h"
#include "TransitionSemantics.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace VNX;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QString guidString(const ProjectItem* item)
{
    return item->guid.toString(QUuid::WithoutBraces);
}

QUndoStack* undoStack()
{
    return UndoRedoManager::instance().undoStack();
}

Clip clipFor(ProjectItem* target, int64_t start, int64_t duration)
{
    return {QUuid::createUuid(), target->guid, start, duration};
}

uint32_t occurrenceCount(const ProjectItem* container, const QUuid& childGuid)
{
    return PlayablePropertyAuthoringV1::containerChildOccurrenceCount(
        *container, childGuid);
}

const SelectionOccurrence* occurrenceAt(
    const ProjectItem* container, uint32_t ordinal)
{
    return container ? container->comp<VSelection>()->selections.shared.find(ordinal) : nullptr;
}

} // namespace

class DirectChildReferenceCleanupTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        registerContainerActions(ActionManager::instance(), &manager);

        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("DirectChildReferenceCleanup");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));

        layerR_ = manager.createItem(nullptr, QStringLiteral("Layer_R"),
                                     ProjectItem::Type::Layer);
        ASSERT_NE(layerR_, nullptr);
        containerA_ = manager.createItem(layerR_, QStringLiteral("Container_A"),
                                         ProjectItem::Type::Container);
        containerB_ = manager.createItem(layerR_, QStringLiteral("Container_B"),
                                         ProjectItem::Type::Container);
        ASSERT_NE(containerA_, nullptr);
        ASSERT_NE(containerB_, nullptr);
        soundA_ = manager.createItem(containerA_, QStringLiteral("Sound_A"),
                                     ProjectItem::Type::Sound);
        soundB_ = manager.createItem(containerA_, QStringLiteral("Sound_B"),
                                     ProjectItem::Type::Sound);
        containerN_ = manager.createItem(containerA_, QStringLiteral("Container_N"),
                                         ProjectItem::Type::Container);
        ASSERT_NE(soundA_, nullptr);
        ASSERT_NE(soundB_, nullptr);
        ASSERT_NE(containerN_, nullptr);
        soundN_ = manager.createItem(containerN_, QStringLiteral("Sound_N"),
                                     ProjectItem::Type::Sound);
        ASSERT_NE(soundN_, nullptr);

        trackA_ = manager.createItem(layerR_, QStringLiteral("Track_A"),
                                     ProjectItem::Type::Track);
        trackB_ = manager.createItem(layerR_, QStringLiteral("Track_B"),
                                     ProjectItem::Type::Track);
        ASSERT_NE(trackA_, nullptr);
        ASSERT_NE(trackB_, nullptr);
        soundT_ = manager.createItem(trackA_, QStringLiteral("Sound_T"),
                                     ProjectItem::Type::Sound);
        containerT_ = manager.createItem(trackA_, QStringLiteral("Container_T"),
                                         ProjectItem::Type::Container);
        pieceT_ = manager.createItem(trackA_, QStringLiteral("Piece_T"),
                                     ProjectItem::Type::Piece);
        ASSERT_NE(soundT_, nullptr);
        ASSERT_NE(containerT_, nullptr);
        ASSERT_NE(pieceT_, nullptr);
        trackA_->comp<VTrack>()->clips = {};
        soundTN_ = manager.createItem(containerT_, QStringLiteral("Sound_TN"),
                                      ProjectItem::Type::Sound);
        ASSERT_NE(soundTN_, nullptr);

        variable_ = manager.createItem(nullptr, QStringLiteral("Variable"),
                                       ProjectItem::Type::Variable);
        ASSERT_NE(variable_, nullptr);

        errorConnection_ = QObject::connect(
            &manager, &ProjectManager::errorOccurred,
            [this](const QString& error) { errors_ << error; });
        UndoRedoManager::instance().clear();
        errors_.clear();
    }

    void TearDown() override
    {
        QObject::disconnect(errorConnection_);
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    ActionResult setProperty(ProjectItem* owner, const QString& name,
                             const QJsonValue& value)
    {
        if (name == QStringLiteral("matches") || name == QStringLiteral("blends")) {
            auto selections = owner->comp<VSelection>()->selections;
            if (name == QStringLiteral("matches")) {
                if (!selections.shared.setMatchGrid(DiscreteGridDef::fromJson(value.toObject()))) return {};
            } else {
                const auto mixing = MixingDef::fromJson(value.toArray());
                if (!mixing || !selections.shared.setMixing(*mixing)) return {};
            }
            return setProperty(owner, QStringLiteral("selections"), selections.toJson());
        }
        return ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            QJsonObject{
                {QStringLiteral("authoring_handle"),
                 static_cast<qint64>(owner->objectID)},
                {QStringLiteral("property"), name},
                {QStringLiteral("value"), value}},
            ActionSource::GUI);
    }

    QJsonObject discreteGrid(const std::vector<ProjectItem*>& targets) const
    {
        QJsonArray rows;
        int value = 1;
        for (ProjectItem* target : targets) {
            rows.append(QJsonObject{
                {QStringLiteral("target"), guidString(target)},
                {QStringLiteral("cells"), QJsonArray{value++}}});
        }
        return QJsonObject{
            {QStringLiteral("axes"), QJsonArray{guidString(variable_)}},
            {QStringLiteral("rows"), rows}};
    }

    QJsonArray blends(const std::vector<ProjectItem*>& targets) const
    {
        MixingDef mixing;
        mixing.rows.push_back({variable_->guid, {}});
        for (ProjectItem* target : targets)
            mixing.rows.front().children.push_back(MixingChildDef::unity(target->guid, 0.0f, 1.0f));
        return mixing.toJson();
    }

    TransitionTableDef authoredTransitions() const
    {
        TransitionTableDef table = AuthoringPolicy::defaultTransitionTable();
        table.rules[0].sourceFadeOut.durationSeconds = 1.25f;
        TransitionRuleDef aToB = AuthoringPolicy::defaultTransitionRule();
        aToB.source = soundA_->guid;
        aToB.target = soundB_->guid;
        EXPECT_TRUE(table.addRule(aToB));
        TransitionRuleDef bToAny = AuthoringPolicy::defaultTransitionRule();
        bToAny.source = soundB_->guid;
        EXPECT_TRUE(table.addRule(bToAny));
        return table;
    }

    const VSelection* selectionOf(const ProjectItem* owner) const
    {
        return owner ? owner->comp<VNX::VSelection>() : nullptr;
    }

    QTemporaryDir tempDir_;
    QMetaObject::Connection errorConnection_;
    QStringList errors_;
    ProjectItem* layerR_ = nullptr;
    ProjectItem* containerA_ = nullptr;
    ProjectItem* containerB_ = nullptr;
    ProjectItem* containerN_ = nullptr;
    ProjectItem* soundA_ = nullptr;
    ProjectItem* soundB_ = nullptr;
    ProjectItem* soundN_ = nullptr;
    ProjectItem* trackA_ = nullptr;
    ProjectItem* trackB_ = nullptr;
    ProjectItem* soundT_ = nullptr;
    ProjectItem* containerT_ = nullptr;
    ProjectItem* pieceT_ = nullptr;
    ProjectItem* soundTN_ = nullptr;
    ProjectItem* variable_ = nullptr;
};

TEST_F(DirectChildReferenceCleanupTest,
       CrossTrackClipDragIsRejectedWithoutMovingTarget)
{
    auto& manager = ProjectManager::instance();
    const Clip first = clipFor(soundT_, 0, 10);
    const Clip second = clipFor(soundT_, 20, 10);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, trackA_, first).success);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, trackA_, second).success);
    const ClipLedger beforeA = trackA_->comp<VTrack>()->clips;
    const ClipLedger beforeB = trackB_->comp<VTrack>()->clips;
    const int undoCount = undoStack()->count();

    const ActionResult result = TimelineEditing::moveClipAcrossTracks(
        &manager, trackA_, trackB_, first.clipGuid, 40);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(trackA_->comp<VTrack>()->clips, beforeA);
    EXPECT_EQ(trackB_->comp<VTrack>()->clips, beforeB);
    EXPECT_EQ(soundT_->parent, trackA_);
    EXPECT_EQ(undoStack()->count(), undoCount);
}

TEST_F(DirectChildReferenceCleanupTest,
       CommandAndLedgerReplacementRejectCrossTrackClipOnlyMove)
{
    auto& manager = ProjectManager::instance();
    const Clip authored = clipFor(soundT_, 0, 10);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, trackA_, authored).success);
    const ClipLedger beforeA = trackA_->comp<VTrack>()->clips;
    const ClipLedger beforeB = trackB_->comp<VTrack>()->clips;
    const int undoCount = undoStack()->count();

    EXPECT_FALSE(TimelineEditing::moveClipAcrossTracks(
        &manager, trackA_, trackB_, authored.clipGuid, 30).success);

    Clip copied = beforeA.items.front();
    copied.clipGuid = QUuid::createUuid();
    copied.start = 40;
    copied.clipGuid = QUuid::createUuid();
    copied.stableOccurrenceOrdinal = 0;
    EXPECT_FALSE(TimelineEditing::addClip(&manager, trackB_, copied).success);

    ClipLedger replacement = beforeB;
    ASSERT_TRUE(replacement.mintOccurrenceOrdinal(&copied));
    replacement.items.push_back(copied);
    EXPECT_FALSE(TimelineEditing::replaceClips(&manager, trackB_, replacement).success);
    EXPECT_FALSE(setProperty(trackB_, QStringLiteral("clips"),
                             replacement.toJsonObject()).success);

    EXPECT_EQ(trackA_->comp<VTrack>()->clips, beforeA);
    EXPECT_EQ(trackB_->comp<VTrack>()->clips, beforeB);
    EXPECT_EQ(soundT_->parent, trackA_);
    EXPECT_EQ(undoStack()->count(), undoCount);
}

TEST_F(DirectChildReferenceCleanupTest,
       SameTrackClipCopySharesDirectChildWithoutCouplingEdits)
{
    auto& manager = ProjectManager::instance();
    const Clip authored = clipFor(soundT_, 10, 20);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, trackA_, authored).success);
    Clip copy = trackA_->comp<VTrack>()->clips.items.front();
    copy.clipGuid = QUuid::createUuid();
    copy.start = 40;
    copy.clipGuid = QUuid::createUuid();
    copy.stableOccurrenceOrdinal = 0;
    ASSERT_TRUE(TimelineEditing::addClip(&manager, trackA_, copy).success);

    auto* timeline = trackA_->comp<VTrack>();
    ASSERT_EQ(timeline->clips.items.size(), 2u);
    EXPECT_EQ(timeline->clips.items[0].targetGuid, soundT_->guid);
    EXPECT_EQ(timeline->clips.items[1].targetGuid, soundT_->guid);
    EXPECT_NE(timeline->clips.items[0].clipGuid, timeline->clips.items[1].clipGuid);

    ASSERT_TRUE(TimelineEditing::moveClip(
        &manager, trackA_, timeline->clips.items[1].clipGuid, 80).success);
    EXPECT_EQ(timeline->clips.items[0].start, 10);
    EXPECT_EQ(timeline->clips.items[1].start, 80);

    const QUuid kept = timeline->clips.items[0].clipGuid;
    ASSERT_TRUE(TimelineEditing::removeClip(
        &manager, trackA_, timeline->clips.items[1].clipGuid).success);
    ASSERT_EQ(timeline->clips.items.size(), 1u);
    EXPECT_EQ(timeline->clips.items[0].clipGuid, kept);
    EXPECT_EQ(soundT_->parent, trackA_);
}

TEST_F(DirectChildReferenceCleanupTest,
       MovingClipTargetClearsOldClipsAndRestoresTheDefaultDestinationClip)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(TimelineEditing::addClip(
        &manager, trackA_, clipFor(soundT_, 0, 10)).success);
    ASSERT_TRUE(TimelineEditing::addClip(
        &manager, trackA_, clipFor(soundT_, 20, 10)).success);
    const ClipLedger original = trackA_->comp<VTrack>()->clips;
    UndoRedoManager::instance().clear();

    ASSERT_TRUE(manager.moveItem(soundT_, trackB_));
    EXPECT_EQ(soundT_->parent, trackB_);
    EXPECT_TRUE(trackA_->comp<VTrack>()->clips.items.empty());
    const ClipLedger destination = trackB_->comp<VTrack>()->clips;
    ASSERT_EQ(destination.items.size(), 1u);
    EXPECT_EQ(destination.items.front().targetGuid, soundT_->guid);
    EXPECT_EQ(destination.items.front().start, 0);

    undoStack()->undo();
    EXPECT_EQ(soundT_->parent, trackA_);
    EXPECT_EQ(trackA_->comp<VTrack>()->clips, original);
    EXPECT_TRUE(trackB_->comp<VTrack>()->clips.items.empty());

    undoStack()->redo();
    EXPECT_EQ(soundT_->parent, trackB_);
    EXPECT_TRUE(trackA_->comp<VTrack>()->clips.items.empty());
    EXPECT_EQ(trackB_->comp<VTrack>()->clips, destination);
}

TEST_F(DirectChildReferenceCleanupTest,
       MovingGridAndTransitionTargetDropsOnlyIllegalRowsAndRules)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("mode"),
                            static_cast<int>(SelectionMode::Match)).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                            discreteGrid({soundA_, soundB_})).success);
    const TransitionTableDef transitions = authoredTransitions();
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("transitions"),
                            transitions.toJson()).success);
    UndoRedoManager::instance().clear();

    ASSERT_TRUE(manager.moveItem(soundA_, layerR_));
    EXPECT_EQ(soundA_->parent, layerR_);
    const auto* selection = selectionOf(containerA_);
    ASSERT_EQ(selection->matchGrid().rows.size(), 1u);
    EXPECT_EQ(selection->matchGrid().rows[0].target, soundB_->guid);
    EXPECT_EQ(selection->matchGrid().axes[0], variable_->guid);
    EXPECT_TRUE(selection->mixing().empty());
    ASSERT_EQ(selection->transitions.rules.size(), 2u);
    EXPECT_TRUE(selection->transitions.rules[0].isDefault());
    EXPECT_FLOAT_EQ(selection->transitions.rules[0].sourceFadeOut.durationSeconds, 1.25f);
    EXPECT_EQ(selection->transitions.rules[1].source, soundB_->guid);
    EXPECT_FALSE(selection->transitions.rules[1].target.has_value());

    undoStack()->undo();
    EXPECT_EQ(soundA_->parent, containerA_);
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows.size(), 2u);
    EXPECT_TRUE(selectionOf(containerA_)->mixing().empty());
    EXPECT_EQ(selectionOf(containerA_)->transitions.rules.size(), 3u);
    EXPECT_EQ(selectionOf(containerA_)->transitions.rules[1].source, soundA_->guid);
}

TEST_F(DirectChildReferenceCleanupTest,
       MovingAndDeletingBlendMembersRestoreTheirRowsOnUndo)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("mode"),
                            static_cast<int>(SelectionMode::Blend)).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("blends"),
                            blends({soundA_, soundB_})).success);
    const QUuid soundGuid = soundA_->guid;
    const auto originalGrid = selectionOf(containerA_)->mixing();
    UndoRedoManager::instance().clear();

    ASSERT_TRUE(manager.moveItem(soundA_, layerR_));
    ASSERT_EQ(selectionOf(containerA_)->mixing().rows.front().children.size(), 1u);
    EXPECT_EQ(selectionOf(containerA_)->mixing().rows.front().children[0].target, soundB_->guid);
    EXPECT_EQ(selectionOf(containerA_)->mixing().rows[0].variable, originalGrid.rows[0].variable);
    EXPECT_TRUE(selectionOf(containerA_)->matchGrid().empty());

    undoStack()->undo();
    EXPECT_EQ(soundA_->parent, containerA_);
    EXPECT_EQ(selectionOf(containerA_)->mixing().toJson(), originalGrid.toJson());

    ASSERT_TRUE(manager.deleteItem(soundA_));
    EXPECT_EQ(manager.findItemByGuid(soundGuid), nullptr);
    ASSERT_EQ(selectionOf(containerA_)->mixing().rows.front().children.size(), 1u);
    EXPECT_EQ(selectionOf(containerA_)->mixing().rows.front().children[0].target, soundB_->guid);

    undoStack()->undo();
    const auto* restored = manager.findItemByGuid(soundGuid);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->parent, containerA_);
    EXPECT_EQ(selectionOf(containerA_)->mixing().toJson(), originalGrid.toJson());
    EXPECT_TRUE(validatePlaybackReferences(*manager.currentProject()));
}

TEST_F(DirectChildReferenceCleanupTest,
       MovingMultipleOccurrencesRestoresOriginalIdentitiesOnUndo)
{
    SelectionEntry second;
    SelectionEntry third;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *containerA_, soundA_->guid, std::nullopt, &second));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *containerA_, soundA_->guid, std::nullopt, &third));
    auto* first = const_cast<SelectionOccurrence*>(occurrenceAt(containerA_, 0u));
    auto* row2 = const_cast<SelectionOccurrence*>(
        occurrenceAt(containerA_, second.stableOccurrenceOrdinal));
    auto* row3 = const_cast<SelectionOccurrence*>(
        occurrenceAt(containerA_, third.stableOccurrenceOrdinal));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(row2, nullptr);
    ASSERT_NE(row3, nullptr);
    first->weight = 1;
    row2->weight = 2;
    row3->weight = 3;
    auto reorderedSelections = containerA_->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
    std::reverse(reorderedSelections.begin(), reorderedSelections.end());
    EXPECT_TRUE(containerA_->comp<VNX::VSelection>()->selections.shared.setOccurrenceRows(reorderedSelections));
    auto* selection = containerA_->comp<VNX::VSelection>();
    selection->selections.shared.find(second.stableOccurrenceOrdinal)->weight = 23;
    selection->selections.shared.find(third.stableOccurrenceOrdinal)->weight = 71;
    const auto original = selection->selections.shared;
    const auto originalBehavior = selection->behavior;
    UndoRedoManager::instance().clear();

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.moveItem(soundA_, containerB_));
    EXPECT_EQ(soundA_->parent, containerB_);
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 0u);
    ASSERT_EQ(occurrenceCount(containerB_, soundA_->guid), 1u);

    undoStack()->undo();
    EXPECT_EQ(soundA_->parent, containerA_);
    EXPECT_EQ(containerA_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), original.occurrenceRows().size());
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 3u);
    EXPECT_EQ(occurrenceCount(containerB_, soundA_->guid), 0u);
    ASSERT_EQ(containerA_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), original.occurrenceRows().size());
    for (size_t i = 0; i < original.occurrenceRows().size(); ++i) {
        EXPECT_EQ(containerA_->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[i].stableOccurrenceOrdinal,
                  original.occurrenceRows()[i].stableOccurrenceOrdinal);
        EXPECT_EQ(containerA_->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[i].weight,
                  original.occurrenceRows()[i].weight);
    }
    EXPECT_EQ(selection->behavior, originalBehavior);
    ASSERT_TRUE(manager.moveItemToBucketRoot(soundA_, QStringLiteral("Layers")));
    EXPECT_EQ(soundA_->parent, nullptr);
    EXPECT_FALSE(selection->selections.shared.find(second.stableOccurrenceOrdinal));
    EXPECT_FALSE(selection->selections.shared.find(third.stableOccurrenceOrdinal));
    undoStack()->undo();
    EXPECT_EQ(soundA_->parent, containerA_);
    EXPECT_EQ(containerA_->comp<VNX::VSelection>()->selections.shared, original);
    EXPECT_EQ(selection->behavior, originalBehavior);
}

TEST_F(DirectChildReferenceCleanupTest,
       MovingIntoNestedOwnerStopsDirectReferencesFromTheOldParent)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("mode"),
                            static_cast<int>(SelectionMode::Match)).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                            discreteGrid({soundA_, soundB_})).success);
    ASSERT_TRUE(TimelineEditing::addClip(
        &manager, trackA_, clipFor(soundT_, 0, 10)).success);

    ASSERT_TRUE(manager.moveItem(soundA_, containerN_));
    EXPECT_EQ(soundA_->parent, containerN_);
    ASSERT_EQ(selectionOf(containerA_)->matchGrid().rows.size(), 1u);
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows[0].target, soundB_->guid);

    ASSERT_TRUE(manager.moveItem(soundT_, containerT_));
    EXPECT_EQ(soundT_->parent, containerT_);
    EXPECT_TRUE(trackA_->comp<VTrack>()->clips.items.empty());
}

TEST_F(DirectChildReferenceCleanupTest,
       MovingWholeOwnerKeepsInternalDirectChildReferences)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("mode"),
                            static_cast<int>(SelectionMode::Match)).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                            discreteGrid({soundA_, soundB_})).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("transitions"),
                            authoredTransitions().toJson()).success);
    ASSERT_TRUE(TimelineEditing::addClip(
        &manager, trackA_, clipFor(soundT_, 0, 10)).success);
    const auto originalGrid = selectionOf(containerA_)->matchGrid();
    const auto originalTransitions = selectionOf(containerA_)->transitions;
    const auto originalClips = trackA_->comp<VTrack>()->clips;

    ASSERT_TRUE(manager.moveItem(containerA_, containerB_));
    EXPECT_EQ(containerA_->parent, containerB_);
    EXPECT_EQ(soundA_->parent, containerA_);
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows[0].target, soundA_->guid);
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows.size(), originalGrid.rows.size());
    EXPECT_EQ(selectionOf(containerA_)->transitions.rules.size(),
              originalTransitions.rules.size());

    ASSERT_TRUE(manager.moveItem(trackA_, containerB_));
    EXPECT_EQ(trackA_->parent, containerB_);
    EXPECT_EQ(soundT_->parent, trackA_);
    EXPECT_EQ(trackA_->comp<VTrack>()->clips.items.size(),
              originalClips.items.size());
    EXPECT_EQ(trackA_->comp<VTrack>()->clips.items[0].targetGuid, soundT_->guid);
}

TEST_F(DirectChildReferenceCleanupTest,
       GroupingChildrenDoesNotRewriteOldReferencesToTheNewContainer)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("mode"),
                            static_cast<int>(SelectionMode::Match)).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                            discreteGrid({soundA_, soundB_, containerN_})).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("transitions"),
                            authoredTransitions().toJson()).success);

    ProjectItem* grouped = manager.createParentContainer(
        {soundA_, soundB_}, ProjectItem::Type::Container, QStringLiteral("Grouped"));
    ASSERT_NE(grouped, nullptr);
    EXPECT_EQ(grouped->parent, containerA_);
    EXPECT_EQ(soundA_->parent, grouped);
    EXPECT_EQ(soundB_->parent, grouped);

    const auto* selection = selectionOf(containerA_);
    ASSERT_EQ(selection->matchGrid().rows.size(), 1u);
    EXPECT_EQ(selection->matchGrid().rows[0].target, containerN_->guid);
    for (const auto& row : selection->matchGrid().rows)
        EXPECT_NE(row.target, grouped->guid);
    ASSERT_EQ(selection->transitions.rules.size(), 1u);
    EXPECT_TRUE(selection->transitions.rules[0].isDefault());
}

TEST_F(DirectChildReferenceCleanupTest,
       BatchMoveRestoresMiddleSiblingsInOriginalOrder)
{
    auto& manager = ProjectManager::instance();
    auto* source = manager.createItem(layerR_, QStringLiteral("OrderedSource"),
                                     ProjectItem::Type::Container);
    ASSERT_NE(source, nullptr);
    for (int index = 0; index < 5; ++index)
        ASSERT_NE(manager.createItem(source, QStringLiteral("OrderedSound%1").arg(index),
                                     ProjectItem::Type::Sound), nullptr);
    const auto sourceOrder = source->children;
    const auto destinationOrder = containerB_->children;
    auto& history = UndoRedoManager::instance();
    history.clear();

    ASSERT_TRUE(manager.moveItems({sourceOrder[2].get(), sourceOrder[3].get()}, containerB_));
    EXPECT_EQ(history.undoStack()->count(), 1);
    const auto movedSourceOrder = source->children;
    const auto movedDestinationOrder = containerB_->children;
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(source->children, sourceOrder);
    EXPECT_EQ(containerB_->children, destinationOrder);
    ASSERT_TRUE(history.redo());
    EXPECT_EQ(source->children, movedSourceOrder);
    EXPECT_EQ(containerB_->children, movedDestinationOrder);
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(source->children, sourceOrder);
}

TEST_F(DirectChildReferenceCleanupTest,
       ReorderingTreeChildrenDoesNotRewriteReferenceTables)
{
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("mode"),
                            static_cast<int>(SelectionMode::Match)).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                            discreteGrid({soundA_, soundB_})).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("transitions"),
                            authoredTransitions().toJson()).success);
    const auto originalOccurrences = containerA_->comp<VNX::VSelection>()->selections.shared;
    const auto originalGrid = selectionOf(containerA_)->matchGrid();
    const auto originalTransitions = selectionOf(containerA_)->transitions;

    const ActionResult result = ActionManager::instance().execute(
        QStringLiteral("reorder_container_children"),
        QJsonObject{
            {QStringLiteral("container_path"), QStringLiteral("Container_A")},
            {QStringLiteral("child_order"),
             QJsonArray{
                 QStringLiteral("Sound_B"),
                 QStringLiteral("Sound_A"),
                 QStringLiteral("Container_N")}}},
        ActionSource::GUI);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_EQ(containerA_->children.size(), 3u);
    EXPECT_EQ(containerA_->children[0].get(), soundB_);
    EXPECT_EQ(containerA_->children[1].get(), soundA_);
    EXPECT_EQ(containerA_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(),
              originalOccurrences.occurrenceRows().size());
    EXPECT_EQ(containerA_->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[0].childGuid,
              originalOccurrences.occurrenceRows()[0].childGuid);
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows[0].target,
              originalGrid.rows[0].target);
    EXPECT_EQ(selectionOf(containerA_)->transitions.rules[1].source,
              originalTransitions.rules[1].source);
}

TEST_F(DirectChildReferenceCleanupTest,
       DeleteAndUndoRestoreReferencedObjectAndLocalRecordsTogether)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("mode"),
                            static_cast<int>(SelectionMode::Match)).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("matches"),
                            discreteGrid({soundA_, soundB_})).success);
    ASSERT_TRUE(setProperty(containerA_, QStringLiteral("transitions"),
                            authoredTransitions().toJson()).success);
    const QUuid soundGuid = soundA_->guid;
    const auto originalGrid = selectionOf(containerA_)->matchGrid();
    const auto originalTransitions = selectionOf(containerA_)->transitions;
    auto reorderedSelections = containerA_->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
    std::reverse(reorderedSelections.begin(), reorderedSelections.end());
    EXPECT_TRUE(containerA_->comp<VNX::VSelection>()->selections.shared.setOccurrenceRows(reorderedSelections));
    const auto originalOccurrences = containerA_->comp<VNX::VSelection>()->selections.shared;
    UndoRedoManager::instance().clear();

    ASSERT_TRUE(manager.deleteItem(soundA_));
    EXPECT_EQ(manager.findItemByGuid(soundGuid), nullptr);
    ASSERT_EQ(selectionOf(containerA_)->matchGrid().rows.size(), 1u);
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows[0].target, soundB_->guid);
    EXPECT_TRUE(selectionOf(containerA_)->mixing().empty());
    ASSERT_EQ(selectionOf(containerA_)->transitions.rules.size(), 2u);
    EXPECT_TRUE(selectionOf(containerA_)->transitions.rules[0].isDefault());
    EXPECT_EQ(selectionOf(containerA_)->transitions.rules[1].source, soundB_->guid);

    undoStack()->undo();
    EXPECT_NE(manager.findItemByGuid(soundGuid), nullptr);
    EXPECT_EQ(containerA_->comp<VNX::VSelection>()->selections.shared, originalOccurrences);
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows.size(), originalGrid.rows.size());
    EXPECT_EQ(selectionOf(containerA_)->transitions.rules.size(),
              originalTransitions.rules.size());
    EXPECT_EQ(selectionOf(containerA_)->transitions.rules[1].source, soundGuid);

    undoStack()->redo();
    EXPECT_EQ(manager.findItemByGuid(soundGuid), nullptr);
    EXPECT_EQ(selectionOf(containerA_)->matchGrid().rows[0].target, soundB_->guid);
}

TEST_F(DirectChildReferenceCleanupTest,
       UndoMoveCleansReferencesAddedThroughCoreEditing)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(TimelineEditing::addClip(
        &manager, trackA_, clipFor(soundT_, 0, 10)).success);
    ASSERT_TRUE(TimelineEditing::addClip(
        &manager, trackA_, clipFor(soundT_, 20, 10)).success);
    const auto oldClips = trackA_->comp<VTrack>()->clips;
    ASSERT_TRUE(manager.moveItem(soundT_, trackB_));
    ASSERT_TRUE(TimelineEditing::addClip(
        &manager, trackB_, clipFor(soundT_, 40, 10)).success);
    undoStack()->undo();
    EXPECT_EQ(soundT_->parent, trackA_);
    EXPECT_EQ(trackA_->comp<VTrack>()->clips, oldClips);
    EXPECT_TRUE(trackB_->comp<VTrack>()->clips.items.empty());

    ASSERT_TRUE(manager.moveItem(soundA_, containerB_));
    auto* selection = containerB_->comp<VNX::VSelection>();
    selection->mode = SelectionMode::Match;
    auto grid = selection->matchGrid();
    ASSERT_TRUE(grid.addRow(soundA_->guid));
    ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    TransitionRuleDef rule = selection->transitions.rules.front();
    rule.source = soundA_->guid;
    ASSERT_TRUE(selection->transitions.addRule(rule));
    const auto defaultRule = selection->transitions.rules.front();
    undoStack()->undo();
    EXPECT_TRUE(selection->matchGrid().rows.empty());
    EXPECT_FALSE(selection->mixing().hasChildren());
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 1u);
    EXPECT_EQ(occurrenceCount(containerB_, soundA_->guid), 0u);
    ASSERT_EQ(selection->transitions.rules.size(), 1u);
    EXPECT_EQ(selection->transitions.rules.front(), defaultRule);
    EXPECT_TRUE(validatePlaybackReferences(*manager.currentProject()));
}

TEST_F(DirectChildReferenceCleanupTest,
       RejectedTreeMoveLeavesClipsAndUndoHistoryUntouched)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(TimelineEditing::addClip(
        &manager, trackA_, clipFor(soundT_, 0, 10)).success);
    ASSERT_TRUE(TimelineEditing::addClip(
        &manager, trackA_, clipFor(soundT_, 20, 10)).success);
    const ClipLedger original = trackA_->comp<VTrack>()->clips;
    UndoRedoManager::instance().clear();
    errors_.clear();

    EXPECT_FALSE(manager.moveItem(soundT_, pieceT_));
    EXPECT_EQ(soundT_->parent, trackA_);
    EXPECT_EQ(trackA_->comp<VTrack>()->clips, original);
    EXPECT_EQ(undoStack()->count(), 0);
    EXPECT_FALSE(errors_.isEmpty());

    errors_.clear();
    EXPECT_FALSE(manager.moveItem(containerA_, containerN_));
    EXPECT_EQ(containerA_->parent, layerR_);
    EXPECT_EQ(soundA_->parent, containerA_);
    EXPECT_EQ(undoStack()->count(), 0);
    EXPECT_FALSE(errors_.isEmpty());
}
