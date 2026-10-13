#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ActionManager.h"
#include "AuthoringPolicy.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "ProjectManager.h"
#include "Timeline/TimelineEditing.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>

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

Clip clipFor(ProjectItem* target, int64_t start, int64_t duration)
{
    return {QUuid::createUuid(), target->guid, start, duration};
}

ClipLedger makeClipLedger(std::initializer_list<Clip> clips)
{
    ClipLedger ledger;
    for (Clip clip : clips) {
        EXPECT_TRUE(ledger.mintOccurrenceOrdinal(&clip));
        ledger.items.push_back(std::move(clip));
    }
    return ledger;
}

ProjectItem* directChildOfType(ProjectItem* parent, ProjectItem::Type type)
{
    const auto it = std::find_if(
        parent->children.begin(), parent->children.end(),
        [type](const std::shared_ptr<ProjectItem>& child) {
            return child && child->type == type;
        });
    return it == parent->children.end() ? nullptr : it->get();
}

class TimelineStructuralIntegrityTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("TimelineStructureProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        root_ = manager.createItem(nullptr, QStringLiteral("StructureRoot"),
                                   ProjectItem::Type::Layer);
        ASSERT_NE(root_, nullptr);
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
    ProjectItem* root_ = nullptr;
};

}

TEST_F(TimelineStructuralIntegrityTest, NewTrackChildrenCreateClipsAtZeroAndUndoRestoresTheirIdentities)
{
    auto& manager = ProjectManager::instance();
    auto* track = manager.createItem(root_, QStringLiteral("AutoClipTrack"), ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    auto* timeline = track->comp<VTrack>();
    auto* stack = UndoRedoManager::instance().undoStack();
    int index = 0;
    for (const auto type : {ProjectItem::Type::Sound, ProjectItem::Type::Container, ProjectItem::Type::Piece}) {
        const auto before = timeline->clips.items;
        const int undoCount = stack->count();
        auto* child = manager.createItem(track, QStringLiteral("Child%1").arg(++index), type);
        ASSERT_NE(child, nullptr);
        ASSERT_EQ(timeline->clips.items.size(), before.size() + 1);
        const Clip clip = timeline->clips.items.back();
        EXPECT_EQ(clip.targetGuid, child->guid);
        EXPECT_EQ(clip.start, 0);
        EXPECT_NE(clip.runtimeId(), harkentest::StudioObjectId(0u));
        EXPECT_EQ(stack->count(), undoCount + 1);
        const ClipLedger installed = timeline->clips;
        for (int repeat = 0; repeat < 3; ++repeat) {
            stack->undo();
            EXPECT_EQ(timeline->clips.items, before);
            EXPECT_EQ(manager.findItemByGuid(child->guid), nullptr);
            stack->redo();
            EXPECT_EQ(timeline->clips, installed);
            EXPECT_EQ(manager.findItemByGuid(child->guid), child);
        }
        ASSERT_TRUE(manager.moveItem(child, track));
        EXPECT_EQ(timeline->clips, installed);
    }
    const ClipLedger installed = timeline->clips;
    auto* removed = track->children.front().get();
    ASSERT_TRUE(manager.deleteItem(removed));
    stack->undo();
    EXPECT_EQ(timeline->clips, installed);
    ASSERT_TRUE(TimelineEditing::removeClip(&manager, track, installed.items.front().clipGuid).success);
    const ClipLedger unplaced = timeline->clips;
    ASSERT_TRUE(manager.deleteItem(removed));
    stack->undo();
    EXPECT_EQ(timeline->clips, unplaced);
}

TEST_F(TimelineStructuralIntegrityTest, TrackChildCopyAndPasteCreateOneClipPerNewObject)
{
    auto& manager = ProjectManager::instance();
    auto* track = manager.createItem(root_, QStringLiteral("CopyClipTrack"), ProjectItem::Type::Track);
    auto* child = manager.createItem(track, QStringLiteral("CopyClipSound"), ProjectItem::Type::Sound);
    ASSERT_NE(child, nullptr);
    auto* timeline = track->comp<VTrack>();
    ASSERT_EQ(timeline->clips.items.size(), 1u);
    ASSERT_TRUE(TimelineEditing::moveClip(&manager, track, timeline->clips.items.front().clipGuid, 3000000000LL).success);
    const Clip original = timeline->clips.items.front();
    auto* duplicate = manager.duplicateItem(child);
    ASSERT_NE(duplicate, nullptr);
    ASSERT_EQ(timeline->clips.items.size(), 2u);
    EXPECT_EQ(timeline->clips.items.back().targetGuid, duplicate->guid);
    EXPECT_EQ(timeline->clips.items.back().start, 0);
    EXPECT_NE(timeline->clips.items.back().runtimeId(), original.runtimeId());
    EXPECT_EQ(timeline->clips.items.front(), original);
    ASSERT_EQ(manager.setClipboard({child, duplicate}, ProjectManager::ClipboardOp::Copy), 2);
    const auto copied = manager.pasteClipboard(track);
    ASSERT_EQ(copied.size(), 2u);
    ASSERT_EQ(timeline->clips.items.size(), 4u);
    for (size_t i = 0; i < copied.size(); ++i) {
        EXPECT_EQ(timeline->clips.items[i + 2].targetGuid, copied[i]->guid);
        EXPECT_EQ(timeline->clips.items[i + 2].start, 0);
    }
    const ClipLedger pasted = timeline->clips;
    auto* stack = UndoRedoManager::instance().undoStack();
    stack->undo();
    EXPECT_EQ(timeline->clips.items.size(), 2u);
    stack->redo();
    EXPECT_EQ(timeline->clips, pasted);
}

TEST_F(TimelineStructuralIntegrityTest, MoveIntoTrackCreatesOneClipAndUndoRestoresBothParents)
{
    auto& manager = ProjectManager::instance();
    auto* first = manager.createItem(root_, QStringLiteral("FirstAutoTrack"), ProjectItem::Type::Track);
    auto* second = manager.createItem(root_, QStringLiteral("SecondAutoTrack"), ProjectItem::Type::Track);
    auto* child = manager.createItem(first, QStringLiteral("MovedAutoSound"), ProjectItem::Type::Sound);
    ASSERT_NE(child, nullptr);
    auto* source = first->comp<VTrack>();
    auto* destination = second->comp<VTrack>();
    ASSERT_EQ(source->clips.items.size(), 1u);
    ASSERT_TRUE(TimelineEditing::moveClip(&manager, first, source->clips.items.front().clipGuid, 3000000000LL).success);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, first, clipFor(child, 5000000000LL, 1000000000LL)).success);
    const ClipLedger before = source->clips;
    ASSERT_EQ(manager.setClipboard({child}, ProjectManager::ClipboardOp::Cut), 1);
    ASSERT_EQ(manager.pasteClipboard(second).size(), 1u);
    EXPECT_TRUE(source->clips.empty());
    ASSERT_EQ(destination->clips.items.size(), 1u);
    EXPECT_EQ(destination->clips.items.front().start, 0);
    EXPECT_EQ(destination->clips.items.front().targetGuid, child->guid);
    const ClipLedger moved = destination->clips;
    auto* stack = UndoRedoManager::instance().undoStack();
    for (int repeat = 0; repeat < 3; ++repeat) {
        stack->undo();
        EXPECT_EQ(child->parent, first);
        EXPECT_EQ(source->clips, before);
        EXPECT_TRUE(destination->clips.empty());
        stack->redo();
        EXPECT_EQ(child->parent, second);
        EXPECT_TRUE(source->clips.empty());
        EXPECT_EQ(destination->clips, moved);
    }
    ASSERT_TRUE(TimelineEditing::removeClip(&manager, second, moved.items.front().clipGuid).success);
    ASSERT_TRUE(manager.moveItem(child, root_));
    stack->undo();
    EXPECT_TRUE(destination->clips.empty());
    EXPECT_EQ(child->parent, second);
}

TEST_F(TimelineStructuralIntegrityTest, RootObjectMovedIntoTrackRetainsItsDefaultClipAcrossUndoRedo)
{
    auto& manager = ProjectManager::instance();
    auto* track = manager.createItem(root_, QStringLiteral("RootAutoTrack"), ProjectItem::Type::Track);
    auto* child = manager.createItem(nullptr, QStringLiteral("RootAutoSound"), ProjectItem::Type::Sound);
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(manager.moveItem(child, track));
    auto* timeline = track->comp<VTrack>();
    ASSERT_EQ(timeline->clips.items.size(), 1u);
    const ClipLedger moved = timeline->clips;
    auto* stack = UndoRedoManager::instance().undoStack();
    for (int repeat = 0; repeat < 3; ++repeat) {
        stack->undo();
        EXPECT_EQ(child->parent, nullptr);
        EXPECT_TRUE(timeline->clips.empty());
        stack->redo();
        EXPECT_EQ(child->parent, track);
        EXPECT_EQ(timeline->clips, moved);
    }
}

TEST_F(TimelineStructuralIntegrityTest, DeleteCascadeUndoRestoresEveryClipAtItsExactIndex)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* track = manager.createItem(
        root_, QStringLiteral("CascadeTrack"), ProjectItem::Type::Track);
    ProjectItem* deletedGroup = manager.createItem(
        track, QStringLiteral("DeletedGroup"), ProjectItem::Type::Container);
    ProjectItem* deletedA = manager.createItem(
        deletedGroup, QStringLiteral("DeletedA"), ProjectItem::Type::Sound);
    ProjectItem* deletedB = manager.createItem(
        deletedGroup, QStringLiteral("DeletedB"), ProjectItem::Type::Sound);
    ProjectItem* survivorA = manager.createItem(
        track, QStringLiteral("SurvivorA"), ProjectItem::Type::Sound);
    ProjectItem* survivorB = manager.createItem(
        track, QStringLiteral("SurvivorB"), ProjectItem::Type::Sound);
    ASSERT_NE(deletedGroup, nullptr);
    ASSERT_NE(deletedA, nullptr);
    ASSERT_NE(deletedB, nullptr);
    ASSERT_NE(survivorA, nullptr);
    ASSERT_NE(survivorB, nullptr);
    ASSERT_NE(track, nullptr);

    const Clip keep0 = clipFor(survivorA, 0, 10);
    const Clip remove1 = clipFor(deletedGroup, 10, 10);
    const Clip keep2 = clipFor(survivorB, 20, 10);
    const Clip remove3 = clipFor(deletedGroup, 30, 10);
    const Clip keep4 = clipFor(survivorA, 40, 10);
    ClipLedger original = makeClipLedger(
        {keep0, remove1, keep2, remove3, keep4});
    ClipLedger survivors = original;
    survivors.items = {original.items[0], original.items[2], original.items[4]};
    ASSERT_TRUE(TimelineEditing::replaceClips(&manager, track, original).success);
    original = track->comp<VTrack>()->clips;
    survivors = original;
    survivors.items = {original.items[0], original.items[2], original.items[4]};
    UndoRedoManager::instance().clear();

    ASSERT_TRUE(manager.deleteItem(deletedGroup));
    EXPECT_EQ(track->comp<VTrack>()->clips, survivors);
    EXPECT_EQ(manager.findItemByGuid(deletedA->guid), nullptr);
    EXPECT_EQ(manager.findItemByGuid(deletedB->guid), nullptr);

    QUndoStack* undo = UndoRedoManager::instance().undoStack();
    ASSERT_TRUE(undo->canUndo());
    undo->undo();
    EXPECT_EQ(track->comp<VTrack>()->clips, original);
    EXPECT_NE(manager.findItemByGuid(deletedA->guid), nullptr);
    EXPECT_NE(manager.findItemByGuid(deletedB->guid), nullptr);

    ASSERT_TRUE(undo->canRedo());
    undo->redo();
    EXPECT_EQ(track->comp<VTrack>()->clips, survivors);
    undo->undo();
    EXPECT_EQ(track->comp<VTrack>()->clips, original);
}

TEST_F(TimelineStructuralIntegrityTest, DuplicateRemapsInternalTargetsAndRemintsAllIdentities)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* external = manager.createItem(
        root_, QStringLiteral("ExternalTarget"), ProjectItem::Type::Sound);
    ProjectItem* source = manager.createItem(
        root_, QStringLiteral("DuplicatedContainer"), ProjectItem::Type::Container);
    ProjectItem* internal = manager.createItem(
        source, QStringLiteral("InternalTarget"), ProjectItem::Type::Sound);
    ProjectItem* track = manager.createItem(
        source, QStringLiteral("InternalTrack"), ProjectItem::Type::Track);
    ProjectItem* piece = manager.createItem(
        track, QStringLiteral("InternalPiece"), ProjectItem::Type::Piece);
    ASSERT_NE(external, nullptr);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(internal, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(piece, nullptr);

    const Clip internalSoundClip = clipFor(internal, 0, 10);
    const Clip internalPieceClip = clipFor(piece, 10, 10);
    const ClipLedger sourceClips = makeClipLedger(
        {internalPieceClip});
    ASSERT_TRUE(TimelineEditing::replaceClips(&manager, track, sourceClips).success);
    const Marker sourceMarker{QUuid::createUuid(), MarkerKind::Custom,
                        QStringLiteral("Inside"), 5};
    ASSERT_TRUE(TimelineEditing::addMarker(&manager, piece, sourceMarker).success);
    UndoRedoManager::instance().clear();

    ProjectItem* clone = manager.duplicateItem(source);
    ASSERT_NE(clone, nullptr);
    ProjectItem* clonedInternal = directChildOfType(clone, ProjectItem::Type::Sound);
    ProjectItem* clonedTrack = directChildOfType(clone, ProjectItem::Type::Track);
    ASSERT_NE(clonedInternal, nullptr);
    ASSERT_NE(clonedTrack, nullptr);
    ProjectItem* clonedPiece = directChildOfType(clonedTrack, ProjectItem::Type::Piece);
    ASSERT_NE(clonedPiece, nullptr);

    EXPECT_NE(clone->guid, source->guid);
    EXPECT_NE(clonedInternal->guid, internal->guid);
    EXPECT_NE(clonedTrack->guid, track->guid);
    EXPECT_NE(clonedPiece->guid, piece->guid);

    const ClipLedger& clonedClips = clonedTrack->comp<VTrack>()->clips;
    ASSERT_EQ(clonedClips.items.size(), 1u);
    EXPECT_EQ(clonedClips.items[0].targetGuid, clonedPiece->guid);
    for (size_t i = 0; i < clonedClips.items.size(); ++i) {
        EXPECT_NE(clonedClips.items[i].clipGuid, sourceClips.items[i].clipGuid);
        EXPECT_NE(clonedClips.items[i].runtimeId(), harkentest::StudioObjectId(0u));
        EXPECT_NE(clonedClips.items[i].runtimeId(),
                  track->comp<VTrack>()->clips.items[i].runtimeId());
    }

    const MarkerLedger& clonedMarkers = clonedPiece->comp<VPiece>()->markers;
    ASSERT_EQ(clonedMarkers.items.size(), 1u);
    EXPECT_NE(clonedMarkers.items[0].markerGuid, sourceMarker.markerGuid);
    EXPECT_NE(clonedMarkers.items[0].runtimeId(), harkentest::StudioObjectId(0u));
    EXPECT_NE(clonedMarkers.items[0].runtimeId(),
              piece->comp<VPiece>()->markers.items[0].runtimeId());
    EXPECT_EQ(clonedMarkers.items[0].kind, sourceMarker.kind);
    EXPECT_EQ(clonedMarkers.items[0].position, sourceMarker.position);
}

TEST_F(TimelineStructuralIntegrityTest, DuplicateRemapsInternalTransitionReferences)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* external = manager.createItem(
        root_, QStringLiteral("TransitionExternal"), ProjectItem::Type::Sound);
    ProjectItem* source = manager.createItem(
        root_, QStringLiteral("TransitionContainer"), ProjectItem::Type::Container);
    ProjectItem* internalA = manager.createItem(
        source, QStringLiteral("TransitionA"), ProjectItem::Type::Sound);
    ProjectItem* internalB = manager.createItem(
        source, QStringLiteral("TransitionB"), ProjectItem::Type::Piece);
    ASSERT_NE(external, nullptr);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(internalA, nullptr);
    ASSERT_NE(internalB, nullptr);

    TransitionTableDef transitions = AuthoringPolicy::defaultTransitionTable();
    TransitionRuleDef internalRule = AuthoringPolicy::defaultTransitionRule();
    internalRule.source = internalA->guid;
    internalRule.target = internalB->guid;
    internalRule.bridge = external->guid;
    ASSERT_TRUE(transitions.addRule(internalRule));
    TransitionRuleDef reverseRule = AuthoringPolicy::defaultTransitionRule();
    reverseRule.source = internalB->guid;
    reverseRule.target = internalA->guid;
    ASSERT_TRUE(transitions.addRule(reverseRule));
    source->comp<VSelection>()->transitions = transitions;

    ProjectItem* clone = manager.duplicateItem(source);
    ASSERT_NE(clone, nullptr);
    ProjectItem* clonedSound = directChildOfType(clone, ProjectItem::Type::Sound);
    ProjectItem* clonedPiece = directChildOfType(clone, ProjectItem::Type::Piece);
    ASSERT_NE(clonedSound, nullptr);
    ASSERT_NE(clonedPiece, nullptr);

    const auto& clonedRules = clone->comp<VSelection>()->transitions.rules;
    ASSERT_EQ(clonedRules.size(), 3u);
    EXPECT_EQ(clonedRules[1].source, clonedSound->guid);
    EXPECT_EQ(clonedRules[1].target, clonedPiece->guid);
    EXPECT_EQ(clonedRules[1].bridge, external->guid);
    EXPECT_EQ(clonedRules[2].source, clonedPiece->guid);
    EXPECT_EQ(clonedRules[2].target, clonedSound->guid);
}

TEST_F(TimelineStructuralIntegrityTest, MusicalTimeCannotConvertCoordinatesOrCreateUndo)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* piece = manager.createItem(
        root_, QStringLiteral("DomainPiece"), ProjectItem::Type::Piece);
    ProjectItem* track = manager.createItem(
        piece, QStringLiteral("DomainTrack"), ProjectItem::Type::Track);
    ProjectItem* sound = manager.createItem(
        track, QStringLiteral("DomainSound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(piece, nullptr);
    ASSERT_NE(track, nullptr);

    ClipLedger absoluteClips = makeClipLedger(
        {clipFor(sound, -500000000, 1500000000)});
    ASSERT_TRUE(TimelineEditing::replaceClips(&manager, track, absoluteClips).success);
    MarkerLedger absoluteMarkers{{
        {QUuid::createUuid(), MarkerKind::Entry, {}, -500000000},
        {QUuid::createUuid(), MarkerKind::Custom, QStringLiteral("Beat"), 1000000000}
    }};
    ASSERT_TRUE(TimelineEditing::replaceMarkers(&manager, piece, absoluteMarkers).success);
    absoluteClips = track->comp<VTrack>()->clips;
    absoluteMarkers = piece->comp<VPiece>()->markers;
    UndoRedoManager::instance().clear();

    const PieceTimeSpec musical = PieceTimeSpec::musical(500000, 4, 4);
    const ActionResult result = ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(piece->objectID)},
         {QStringLiteral("property"), QStringLiteral("pieceTime")},
         {QStringLiteral("value"), musical.toJson()}},
        ActionSource::GUI);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
    EXPECT_EQ(piece->comp<VPiece>()->pieceTime, PieceTimeSpec{});
    EXPECT_EQ(track->comp<VTrack>()->clips, absoluteClips);
    EXPECT_EQ(piece->comp<VPiece>()->markers, absoluteMarkers);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);

    piece->comp<VPiece>()->pieceTime = musical;
    const auto conversion = TimelineEditing::replacePieceTime(&manager, piece, PieceTimeSpec{});
    EXPECT_FALSE(conversion.success);
    EXPECT_EQ(conversion.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
    EXPECT_EQ(piece->comp<VPiece>()->pieceTime, musical);
    EXPECT_EQ(track->comp<VTrack>()->clips, absoluteClips);
    EXPECT_EQ(piece->comp<VPiece>()->markers, absoluteMarkers);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(TimelineStructuralIntegrityTest, TrackAbsoluteMoveRepeatedUndoRedoHasNoDrift)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* track = manager.createItem(
        root_, QStringLiteral("MovedTrack"), ProjectItem::Type::Track);
    ProjectItem* sound = manager.createItem(
        track, QStringLiteral("MoveSound"), ProjectItem::Type::Sound);
    ProjectItem* piece = manager.createItem(
        root_, QStringLiteral("MoveDestination"), ProjectItem::Type::Piece);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(piece, nullptr);
    ClipLedger absolute = makeClipLedger(
        {clipFor(sound, 1000000000, 1000000000)});
    ASSERT_TRUE(TimelineEditing::replaceClips(&manager, track, absolute).success);
    absolute = track->comp<VTrack>()->clips;
    UndoRedoManager::instance().clear();

    ASSERT_TRUE(manager.moveItem(track, piece));
    const ClipLedger moved = track->comp<VTrack>()->clips;
    ASSERT_EQ(track->parent, piece);
    EXPECT_EQ(moved, absolute);

    QUndoStack* undo = UndoRedoManager::instance().undoStack();
    for (int iteration = 0; iteration < 5; ++iteration) {
        undo->undo();
        EXPECT_EQ(track->parent, root_);
        EXPECT_EQ(track->comp<VTrack>()->clips, absolute);
        undo->redo();
        EXPECT_EQ(track->parent, piece);
        EXPECT_EQ(track->comp<VTrack>()->clips, absolute);
    }
}

TEST_F(TimelineStructuralIntegrityTest, LegacyMusicalTrackCannotBeReparentedOrCopied)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* piece = manager.createItem(
        root_, QStringLiteral("OverflowPiece"), ProjectItem::Type::Piece);
    ProjectItem* track = manager.createItem(
        piece, QStringLiteral("OverflowTrack"), ProjectItem::Type::Track);
    ProjectItem* sound = manager.createItem(
        track, QStringLiteral("OverflowSound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(piece, nullptr);
    ASSERT_NE(track, nullptr);
    ClipLedger original = makeClipLedger(
        {clipFor(sound, 1000000000000000LL, 1)});
    ASSERT_TRUE(TimelineEditing::replaceClips(&manager, track, original).success);
    original = track->comp<VTrack>()->clips;
    piece->comp<VPiece>()->pieceTime = PieceTimeSpec::musical(500000, 4, 4);
    UndoRedoManager::instance().clear();

    EXPECT_FALSE(manager.moveItem(track, root_));
    EXPECT_FALSE(manager.moveItemToBucketRoot(track, QStringLiteral("Layers")));
    EXPECT_EQ(manager.duplicateItem(track), nullptr);
    EXPECT_EQ(manager.createItem(piece, QStringLiteral("RejectedTrack"),
                                 ProjectItem::Type::Track), nullptr);
    EXPECT_EQ(manager.createAudioItem(track, QStringLiteral("RejectedSound"), {}, 0), nullptr);
    EXPECT_EQ(track->parent, piece);
    EXPECT_EQ(track->comp<VTrack>()->clips, original);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(TimelineStructuralIntegrityTest, CopiedTrackKeepsTheReferencedChildState)
{
    auto& manager = ProjectManager::instance();
    auto* track = manager.createItem(root_, QStringLiteral("CopiedTrack"), ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    auto* piece = manager.createItem(track, QStringLiteral("CopiedPiece"), ProjectItem::Type::Piece);
    ASSERT_NE(piece, nullptr);
    const auto copiedTime = piece->comp<VPiece>()->pieceTime;
    const auto clips = track->comp<VTrack>()->clips;
    ASSERT_FALSE(clips.items.empty());
    ASSERT_EQ(manager.setClipboard({track}, ProjectManager::ClipboardOp::Copy), 1);
    piece->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    ASSERT_EQ(manager.setClipboard({track}, ProjectManager::ClipboardOp::Copy), 0);

    for (int i = 0; i < 2; ++i) {
        const auto pasted = manager.pasteClipboard(root_);
        ASSERT_EQ(pasted.size(), 1u);
        auto* copy = pasted.front();
        ASSERT_EQ(copy->children.size(), 1u);
        const auto* child = copy->children.front().get();
        EXPECT_EQ(child->comp<VPiece>()->pieceTime, copiedTime);
        EXPECT_NE(child->guid, piece->guid);
        const auto& copiedClips = copy->comp<VTrack>()->clips.items;
        ASSERT_EQ(copiedClips.size(), clips.items.size());
        EXPECT_EQ(copiedClips.front().targetGuid, child->guid);
        EXPECT_NE(copiedClips.front().clipGuid, clips.items.front().clipGuid);
    }
}

TEST_F(TimelineStructuralIntegrityTest, ReferencedMusicalPieceRejectsCutClipboardAndGrouping)
{
    auto& manager = ProjectManager::instance();
    auto* ordinary = manager.createItem(root_, QStringLiteral("Ordinary"), ProjectItem::Type::Sound);
    auto* wrapper = manager.createItem(root_, QStringLiteral("Wrapper"), ProjectItem::Type::Container);
    auto* track = manager.createItem(wrapper, QStringLiteral("WrapperTrack"), ProjectItem::Type::Track);
    auto* piece = manager.createItem(track, QStringLiteral("ReferencedLegacy"), ProjectItem::Type::Piece);
    auto* destination = manager.createItem(nullptr, QStringLiteral("Destination"), ProjectItem::Type::Layer);
    ASSERT_NE(ordinary, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(piece, nullptr);
    ASSERT_NE(destination, nullptr);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, track, clipFor(piece, 0, 100)).success);
    ASSERT_EQ(manager.setClipboard({ordinary, wrapper}, ProjectManager::ClipboardOp::Cut), 2);
    piece->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    UndoRedoManager::instance().clear();
    const auto nextObject = manager.currentProject()->nextObjectID;
    const auto sourceCount = root_->children.size();
    const auto rootCount = manager.currentProject()->layers.size();

    EXPECT_TRUE(manager.pasteClipboard(destination).empty());
    EXPECT_TRUE(manager.pasteClipboardAtBucketRoot(QStringLiteral("Layers")).empty());
    EXPECT_EQ(manager.duplicateItem(wrapper), nullptr);
    EXPECT_FALSE(manager.moveItem(wrapper, destination));
    EXPECT_EQ(manager.createParentContainer({ordinary, wrapper}, ProjectItem::Type::Container,
                                            QStringLiteral("RejectedParent")), nullptr);
    EXPECT_EQ(manager.setClipboard({ordinary, wrapper}, ProjectManager::ClipboardOp::Copy), 0);
    EXPECT_TRUE(destination->children.empty());
    EXPECT_EQ(root_->children.size(), sourceCount);
    EXPECT_EQ(manager.currentProject()->layers.size(), rootCount);
    EXPECT_EQ(ordinary->parent, root_);
    EXPECT_EQ(wrapper->parent, root_);
    EXPECT_EQ(manager.currentProject()->nextObjectID, nextObject);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}
