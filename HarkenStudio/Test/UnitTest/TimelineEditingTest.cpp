#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ActionManager.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "ProjectManager.h"
#include "Timeline/TimelineEditing.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

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

Marker markerAt(MarkerKind kind, int64_t position, const QString& name = {})
{
    return {QUuid::createUuid(), kind, name, position};
}

class TimelineEditingTest : public ::testing::Test
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
        config.name = QStringLiteral("TimelineEditingProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));

        layer_ = manager.createItem(nullptr, QStringLiteral("TimelineLayer"),
                                    ProjectItem::Type::Layer);
        source_ = manager.createItem(layer_, QStringLiteral("SourceTrack"),
                                     ProjectItem::Type::Track);
        destination_ = manager.createItem(layer_, QStringLiteral("DestinationTrack"),
                                          ProjectItem::Type::Track);
        sound_ = manager.createItem(source_, QStringLiteral("TargetSound"),
                                    ProjectItem::Type::Sound);
        piece_ = manager.createItem(layer_, QStringLiteral("MarkerPiece"),
                                    ProjectItem::Type::Piece);
        ASSERT_NE(layer_, nullptr);
        ASSERT_NE(source_, nullptr);
        ASSERT_NE(destination_, nullptr);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(piece_, nullptr);
        source_->comp<VTrack>()->clips = {};
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
    ProjectItem* layer_ = nullptr;
    ProjectItem* source_ = nullptr;
    ProjectItem* destination_ = nullptr;
    ProjectItem* sound_ = nullptr;
    ProjectItem* piece_ = nullptr;
};

class TrackClipOwnershipTest : public TimelineEditingTest,
                               public ::testing::WithParamInterface<ProjectItem::Type>
{
protected:
    void SetUp() override
    {
        TimelineEditingTest::SetUp();
        ASSERT_FALSE(HasFatalFailure());
        auto& manager = ProjectManager::instance();
        host_ = GetParam() == ProjectItem::Type::Layer ? layer_
            : manager.createItem(layer_, QStringLiteral("TrackHost"), GetParam());
        ASSERT_NE(host_, nullptr);
        ASSERT_TRUE(manager.moveItem(source_, host_));
        ASSERT_TRUE(manager.moveItem(destination_, host_));
        container_ = manager.createItem(source_, QStringLiteral("Container_T"),
                                        ProjectItem::Type::Container);
        nestedPiece_ = manager.createItem(source_, QStringLiteral("Piece_T"),
                                          ProjectItem::Type::Piece);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(nestedPiece_, nullptr);
        containerSound_ = manager.createItem(container_, QStringLiteral("Sound_TN"),
                                             ProjectItem::Type::Sound);
        innerTrack_ = manager.createItem(nestedPiece_, QStringLiteral("Track_Inner"),
                                         ProjectItem::Type::Track);
        ASSERT_NE(containerSound_, nullptr);
        ASSERT_NE(innerTrack_, nullptr);
        innerSound_ = manager.createItem(innerTrack_, QStringLiteral("Sound_I"),
                                         ProjectItem::Type::Sound);
        ASSERT_NE(innerSound_, nullptr);
        source_->comp<VTrack>()->clips = {};
        innerTrack_->comp<VTrack>()->clips = {};
        ASSERT_TRUE(manager.saveProject());
        UndoRedoManager::instance().clear();
    }

    ActionResult setClips(const ClipLedger& clips, ActionSource source = ActionSource::GUI)
    {
        return ActionManager::instance().execute(QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(source_->objectID)},
             {QStringLiteral("property"), QStringLiteral("clips")},
             {QStringLiteral("value"), clips.toJsonObject()}}, source);
    }

    void expectRejected(const QUuid& target, TimelineError code)
    {
        auto& manager = ProjectManager::instance();
        auto* stack = UndoRedoManager::instance().undoStack();
        const ClipLedger before = source_->comp<VTrack>()->clips;
        const bool dirty = source_->dirtyBit;
        const int count = stack->count();
        const int index = stack->index();
        const bool clean = stack->isClean();
        ProjectItem* object = manager.findItemByGuid(target);
        ProjectItem* parent = object ? object->parent : nullptr;
        const size_t children = source_->children.size();
        ClipLedger candidate = before;
        Clip invalid{QUuid::createUuid(), target, 80, 20};
        ASSERT_TRUE(candidate.mintOccurrenceOrdinal(&invalid));
        candidate.items.push_back(invalid);
        const auto placement = TimelineEditing::validateClipPlacement(&manager, source_, candidate);
        EXPECT_EQ(placement.error, code) << placement.message.toStdString();
        EXPECT_EQ(placement.index, static_cast<int>(before.items.size()));
        EXPECT_TRUE(placement.message.contains(source_->name));
        EXPECT_TRUE(placement.message.contains(target.toString(QUuid::WithoutBraces)));
        EXPECT_TRUE(placement.message.contains(invalid.clipGuid.toString(QUuid::WithoutBraces)));
        for (const ActionResult& result : {
                 TimelineEditing::addClip(&manager, source_, invalid),
                 TimelineEditing::replaceClips(&manager, source_, candidate),
                 setClips(candidate, ActionSource::GUI),
                 setClips(candidate, ActionSource::IPC)}) {
            EXPECT_FALSE(result.success);
            EXPECT_FALSE(result.errorMessage.isEmpty());
            EXPECT_FALSE(result.undoFn);
            EXPECT_FALSE(result.redoFn);
        }
        EXPECT_EQ(source_->comp<VTrack>()->clips, before);
        EXPECT_EQ(source_->dirtyBit, dirty);
        EXPECT_EQ(source_->children.size(), children);
        if (object) EXPECT_EQ(object->parent, parent);
        EXPECT_EQ(stack->count(), count);
        EXPECT_EQ(stack->index(), index);
        EXPECT_EQ(stack->isClean(), clean);
    }

    void reload(const ClipLedger& expected)
    {
        auto& manager = ProjectManager::instance();
        const QUuid trackGuid = source_->guid;
        const QString path = manager.currentProject()->projectPath;
        ASSERT_TRUE(manager.saveProject());
        ASSERT_TRUE(manager.loadProject(path));
        source_ = manager.findItemByGuid(trackGuid);
        ASSERT_NE(source_, nullptr);
        EXPECT_EQ(source_->comp<VTrack>()->clips, expected);
        for (const auto& clip : expected.items) {
            const auto* target = manager.findItemByGuid(clip.targetGuid);
            ASSERT_NE(target, nullptr);
            EXPECT_EQ(target->parent, source_);
        }
    }

    ProjectItem* host_ = nullptr;
    ProjectItem* container_ = nullptr;
    ProjectItem* nestedPiece_ = nullptr;
    ProjectItem* containerSound_ = nullptr;
    ProjectItem* innerTrack_ = nullptr;
    ProjectItem* innerSound_ = nullptr;
};

INSTANTIATE_TEST_SUITE_P(Hosts, TrackClipOwnershipTest,
    ::testing::Values(ProjectItem::Type::Layer, ProjectItem::Type::Container,
                      ProjectItem::Type::Piece),
    [](const ::testing::TestParamInfo<ProjectItem::Type>& info) {
        return info.param == ProjectItem::Type::Layer ? "Layer"
            : info.param == ProjectItem::Type::Container ? "Container" : "Piece";
    });

}

TEST_P(TrackClipOwnershipTest, DirectContentKindsRoundTripAcrossHosts)
{
    auto& manager = ProjectManager::instance();
    ClipLedger candidate;
    for (ProjectItem* target : {sound_, container_, nestedPiece_}) {
        Clip clip = clipFor(target, static_cast<int64_t>(candidate.items.size()) * 100, 50);
        ASSERT_TRUE(candidate.mintOccurrenceOrdinal(&clip));
        candidate.items.push_back(clip);
        EXPECT_TRUE(TimelineEditing::validateClipPlacement(&manager, source_, candidate));
    }
    ASSERT_TRUE(setClips(candidate, ActionSource::IPC).success);
    ASSERT_EQ(source_->comp<VTrack>()->clips.items.size(), 3u);
    EXPECT_EQ(containerSound_->parent, container_);
    EXPECT_EQ(innerSound_->parent, innerTrack_);
    EXPECT_TRUE(innerTrack_->comp<VTrack>()->clips.empty());
    const ClipLedger authored = source_->comp<VTrack>()->clips;
    reload(authored);
    ASSERT_FALSE(HasFatalFailure());
    Clip copy = authored.items.back();
    copy.clipGuid = QUuid::createUuid();
    ASSERT_TRUE(TimelineEditing::addClip(&manager, source_, copy).success);
    EXPECT_EQ(source_->comp<VTrack>()->clips.items.size(), 4u);
}

TEST_P(TrackClipOwnershipTest, InvalidTargetsAreRejectedAcrossEntryPoints)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* peerSound = manager.createItem(destination_, QStringLiteral("PeerSound"),
                                               ProjectItem::Type::Sound);
    ProjectItem* sibling = manager.createItem(layer_, QStringLiteral("Sibling"),
                                             ProjectItem::Type::Sound);
    ProjectItem* otherLayer = manager.createItem(nullptr, QStringLiteral("Layer_X"),
                                                ProjectItem::Type::Layer);
    ASSERT_NE(otherLayer, nullptr);
    ProjectItem* external = manager.createItem(otherLayer, QStringLiteral("Sound_X"),
                                              ProjectItem::Type::Sound);
    ASSERT_NE(peerSound, nullptr);
    ASSERT_NE(sibling, nullptr);
    ASSERT_NE(external, nullptr);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, source_, clipFor(sound_, 10, 50)).success);
    for (ProjectItem* target : {containerSound_, innerSound_, peerSound, sibling, external})
        expectRejected(target->guid, TimelineError::InvalidOwnership);
    expectRejected(host_->guid, GetParam() == ProjectItem::Type::Layer
                                   ? TimelineError::InvalidTargetType
                                   : TimelineError::InvalidOwnership);
    for (ProjectItem* target : {source_, destination_, layer_})
        expectRejected(target->guid, TimelineError::InvalidTargetType);
    for (const auto type : {ProjectItem::Type::Variable, ProjectItem::Type::Action}) {
        ProjectItem* target = manager.createItem(nullptr, QStringLiteral("InvalidTarget"), type);
        ASSERT_NE(target, nullptr);
        expectRejected(target->guid, TimelineError::InvalidTargetType);
    }
    expectRejected(manager.currentProject()->busses.front().rootItem->guid,
                   TimelineError::InvalidTargetType);
    expectRejected(QUuid::createUuid(), TimelineError::MissingTarget);
    ASSERT_TRUE(manager.saveProject());
    expectRejected(external->guid, TimelineError::InvalidOwnership);
    ClipLedger empty = source_->comp<VTrack>()->clips;
    empty.items.clear();
    ASSERT_TRUE(setClips(empty).success);
}

TEST_P(TrackClipOwnershipTest, AncestorPieceIsRejectedBeforeCycleExpansion)
{
    source_ = innerTrack_;
    expectRejected(nestedPiece_->guid, TimelineError::InvalidOwnership);
}

TEST_P(TrackClipOwnershipTest, ExistingPieceCycleCheckRemainsEffective)
{
    auto& manager = ProjectManager::instance();
    auto* hostTrack = manager.createItem(piece_, QStringLiteral("CycleHostTrack"),
                                         ProjectItem::Type::Track);
    ASSERT_NE(hostTrack, nullptr);
    ASSERT_TRUE(manager.moveItem(nestedPiece_, hostTrack));
    const ClipLedger original = hostTrack->comp<VTrack>()->clips;
    Clip backReference = clipFor(piece_, 0, 10);
    auto& nestedClips = innerTrack_->comp<VTrack>()->clips;
    ASSERT_TRUE(nestedClips.mintOccurrenceOrdinal(&backReference));
    nestedClips.items.push_back(backReference);
    ClipLedger candidate;
    Clip clip = clipFor(nestedPiece_, 0, 10);
    ASSERT_TRUE(candidate.mintOccurrenceOrdinal(&clip));
    candidate.items.push_back(clip);
    EXPECT_EQ(TimelineEditing::validateClipPlacement(&manager, hostTrack, candidate).error,
              TimelineError::Cycle);
    EXPECT_FALSE(TimelineEditing::replaceClips(&manager, hostTrack, candidate).success);
    EXPECT_EQ(hostTrack->comp<VTrack>()->clips, original);
}

TEST_P(TrackClipOwnershipTest, RepeatedTargetsKeepIndependentOccurrencesAndZeroClips)
{
    auto& manager = ProjectManager::instance();
    ClipLedger candidate;
    for (uint32_t loopCount : {1u, 3u}) {
        Clip clip = clipFor(sound_, loopCount * 100, loopCount * 20);
        clip.playableProperties.presence.Set(harkenplayableproperties::PlayablePropertyIdV1::LoopCount);
        ASSERT_TRUE(clip.playableProperties.setValue(
            harkenplayableproperties::PlayablePropertyIdV1::LoopCount, loopCount));
        ASSERT_TRUE(candidate.mintOccurrenceOrdinal(&clip));
        candidate.items.push_back(clip);
    }
    ASSERT_TRUE(setClips(candidate).success);
    const ClipLedger both = source_->comp<VTrack>()->clips;
    const QUuid soundGuid = sound_->guid;
    reload(both);
    ASSERT_FALSE(HasFatalFailure());
    sound_ = manager.findItemByGuid(soundGuid);
    ASSERT_NE(sound_, nullptr);
    EXPECT_NE(both.items[0].clipGuid, both.items[1].clipGuid);
    EXPECT_NE(both.items[0].runtimeId(), both.items[1].runtimeId());
    EXPECT_NE(both.items[0].stableOccurrenceOrdinal, both.items[1].stableOccurrenceOrdinal);
    ActionResult removed = TimelineEditing::removeClip(&manager, source_, both.items[0].clipGuid);
    ASSERT_TRUE(removed.success);
    ASSERT_EQ(source_->comp<VTrack>()->clips.items.size(), 1u);
    EXPECT_EQ(source_->comp<VTrack>()->clips.items.front(), both.items[1]);
    ASSERT_TRUE(removed.undoFn);
    ASSERT_TRUE(removed.redoFn);
    removed.undoFn();
    EXPECT_EQ(source_->comp<VTrack>()->clips, both);
    removed.redoFn();
    EXPECT_EQ(source_->comp<VTrack>()->clips.items.front(), both.items[1]);
    ASSERT_TRUE(TimelineEditing::removeClip(&manager, source_, both.items[1].clipGuid).success);
    EXPECT_EQ(manager.findItemByGuid(sound_->guid), sound_);
    EXPECT_EQ(sound_->parent, source_);
    const ClipLedger empty = source_->comp<VTrack>()->clips;
    ASSERT_TRUE(empty.empty());
    EXPECT_EQ(empty.occurrenceOrdinalHighWater, 2u);
    reload(empty);
    ASSERT_FALSE(HasFatalFailure());
    sound_ = manager.findItemByGuid(soundGuid);
    ASSERT_NE(sound_, nullptr);
    EXPECT_EQ(sound_->parent, source_);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, source_, clipFor(sound_, 300, 40)).success);
    EXPECT_EQ(source_->comp<VTrack>()->clips.items.front().stableOccurrenceOrdinal, 2u);
}

TEST_P(TrackClipOwnershipTest, TargetReplacementPreservesIdentityAndRejectedEditsPreserveHistory)
{
    auto* external = ProjectManager::instance().createItem(
        layer_, QStringLiteral("Sound_X"), ProjectItem::Type::Sound);
    ASSERT_NE(external, nullptr);
    ClipLedger candidate;
    Clip clip = clipFor(sound_, -100, 250);
    clip.playableProperties.presence.Set(harkenplayableproperties::PlayablePropertyIdV1::LoopCount);
    ASSERT_TRUE(clip.playableProperties.setValue(
        harkenplayableproperties::PlayablePropertyIdV1::LoopCount, 2u));
    ASSERT_TRUE(candidate.mintOccurrenceOrdinal(&clip));
    candidate.items.push_back(clip);
    ASSERT_TRUE(setClips(candidate).success);
    const ClipLedger original = source_->comp<VTrack>()->clips;
    candidate = original;
    candidate.items.front().targetGuid = container_->guid;
    auto* stack = UndoRedoManager::instance().undoStack();
    const int count = stack->count();
    ASSERT_TRUE(setClips(candidate, ActionSource::IPC).success);
    EXPECT_EQ(source_->comp<VTrack>()->clips, candidate);
    EXPECT_EQ(stack->count(), count + 1);
    auto rejectReplacement = [&] {
        ClipLedger invalid = candidate;
        invalid.items.front().targetGuid = external->guid;
        const ClipLedger before = source_->comp<VTrack>()->clips;
        const int index = stack->index();
        const int history = stack->count();
        const bool dirty = source_->dirtyBit;
        EXPECT_FALSE(setClips(invalid).success);
        EXPECT_EQ(source_->comp<VTrack>()->clips, before);
        EXPECT_EQ(source_->dirtyBit, dirty);
        EXPECT_EQ(stack->index(), index);
        EXPECT_EQ(stack->count(), history);
    };
    rejectReplacement();
    for (int i = 0; i < 2; ++i) {
        stack->undo();
        EXPECT_EQ(source_->comp<VTrack>()->clips, original);
        rejectReplacement();
        EXPECT_TRUE(stack->canRedo());
        stack->redo();
        EXPECT_EQ(source_->comp<VTrack>()->clips, candidate);
    }
    reload(candidate);
    ASSERT_FALSE(HasFatalFailure());
    candidate.items.front().start = 500;
    ASSERT_TRUE(setClips(candidate).success);
    EXPECT_EQ(source_->comp<VTrack>()->clips, candidate);
}

TEST_F(TimelineEditingTest, ClipCrudAndSameTrackCopyPreserveIdentityAndOwnership)
{
    auto& manager = ProjectManager::instance();
    const QUuid ownerBefore = sound_->parent->guid;
    const Clip authored = clipFor(sound_, -20, 80);

    ActionResult result = TimelineEditing::addClip(&manager, source_, authored);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_EQ(source_->comp<VTrack>()->clips.items.size(), 1u);
    EXPECT_NE(source_->comp<VTrack>()->clips.items.front().runtimeId(), harkentest::StudioObjectId(0u));
    EXPECT_EQ(source_->comp<VTrack>()->clips.items.front().clipGuid,
              authored.clipGuid);

    result = TimelineEditing::moveClip(&manager, source_, authored.clipGuid, 25);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(source_->comp<VTrack>()->clips.items[0].start, 25);

    result = TimelineEditing::resizeClip(&manager, source_, authored.clipGuid, 10, 150);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(source_->comp<VTrack>()->clips.items[0].start, 10);
    EXPECT_EQ(source_->comp<VTrack>()->clips.items[0].duration, 150);
    ASSERT_TRUE(static_cast<bool>(result.undoFn));
    result.undoFn();
    EXPECT_EQ(source_->comp<VTrack>()->clips.items[0].start, 25);
    EXPECT_EQ(source_->comp<VTrack>()->clips.items[0].duration, 80);
    result.redoFn();

    Clip copy = source_->comp<VTrack>()->clips.items[0];
    copy.clipGuid = QUuid::createUuid();
    copy.start = -40;
    copy.clipGuid = QUuid::createUuid();
    copy.stableOccurrenceOrdinal = 0;
    result = TimelineEditing::addClip(&manager, source_, copy);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_EQ(source_->comp<VTrack>()->clips.items.size(), 2u);
    EXPECT_EQ(source_->comp<VTrack>()->clips.items[0].clipGuid,
              authored.clipGuid);
    EXPECT_EQ(source_->comp<VTrack>()->clips.items[1].targetGuid,
              sound_->guid);
    EXPECT_EQ(source_->comp<VTrack>()->clips.items[1].start, -40);
    EXPECT_EQ(sound_->parent->guid, ownerBefore);

    result = TimelineEditing::removeClip(&manager, source_, copy.clipGuid);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(source_->comp<VTrack>()->clips.items.size(), 1u);
    EXPECT_EQ(manager.findItemByGuid(sound_->guid), sound_);
}

TEST_F(TimelineEditingTest, ClipOccurrenceOrdinalsDoNotReuseOnSameTrackCopy)
{
    auto& manager = ProjectManager::instance();
    Clip destinationSeed = clipFor(sound_, 0, 10);
    ASSERT_TRUE(TimelineEditing::addClip(
        &manager, source_, destinationSeed).success);
    ASSERT_TRUE(TimelineEditing::removeClip(
        &manager, source_, destinationSeed.clipGuid).success);
    EXPECT_EQ(source_->comp<VTrack>()
                  ->clips.occurrenceOrdinalHighWater, 1u);

    Clip moving = clipFor(sound_, 10, 20);
    moving.playableProperties.presence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::LoopCount);
    ASSERT_TRUE(moving.playableProperties.setValue(
        harkenplayableproperties::PlayablePropertyIdV1::LoopCount, 1u));
    ASSERT_TRUE(TimelineEditing::addClip(&manager, source_, moving).success);
    ASSERT_EQ(source_->comp<VTrack>()->clips.items.size(), 1u);
    EXPECT_EQ(source_->comp<VTrack>()->clips.items[0]
                  .stableOccurrenceOrdinal, 1u);

    Clip copy = source_->comp<VTrack>()->clips.items[0];
    copy.clipGuid = QUuid::createUuid();
    copy.start = 30;
    copy.clipGuid = QUuid::createUuid();
    copy.stableOccurrenceOrdinal = 0;
    ActionResult added = TimelineEditing::addClip(&manager, source_, copy);
    ASSERT_TRUE(added.success) << added.errorMessage.toStdString();
    ASSERT_EQ(source_->comp<VTrack>()->clips.items.size(), 2u);
    const Clip& copiedClip = source_->comp<VTrack>()->clips.items[1];
    EXPECT_EQ(copiedClip.stableOccurrenceOrdinal, 2u);
    EXPECT_TRUE(copiedClip.playableProperties.presence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::LoopCount));
    EXPECT_EQ(copiedClip.playableProperties, moving.playableProperties);
    EXPECT_EQ(source_->comp<VTrack>()
                  ->clips.occurrenceOrdinalHighWater, 3u);

    ASSERT_TRUE(TimelineEditing::removeClip(
        &manager, source_, copiedClip.clipGuid).success);
    Clip fresh = clipFor(sound_, 40, 10);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, source_, fresh).success);
    EXPECT_EQ(source_->comp<VTrack>()->clips.items.back()
                  .stableOccurrenceOrdinal, 3u);
}

TEST_F(TimelineEditingTest, LedgerReplacementRejectsCollisionsAndPreservesIdentity)
{
    auto& manager = ProjectManager::instance();
    Clip clip = clipFor(sound_, 0, 10);
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(sound_->runtimeId()));
    ClipLedger clips;
    ASSERT_TRUE(clips.mintOccurrenceOrdinal(&clip));
    clips.items.push_back(clip);

    ActionResult result = TimelineEditing::replaceClips(&manager, source_, clips);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(source_->comp<VTrack>()->clips.empty());
    clips.items.front().clipGuid = QUuid::createUuid();
    result = TimelineEditing::replaceClips(&manager, source_, clips);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    const harkenstudio::ObjectId mintedClipId = source_->comp<VTrack>()
                                      ->clips.items.front().runtimeId();
    EXPECT_NE(mintedClipId, harkentest::StudioObjectId(0u));
    EXPECT_NE(mintedClipId, sound_->runtimeId());

    clips.items.front().start = 5;
    result = TimelineEditing::replaceClips(&manager, source_, clips);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(source_->comp<VTrack>()->clips.items.front().runtimeId(),
              mintedClipId);

    Marker marker = markerAt(MarkerKind::Custom, 20, QStringLiteral("Marker"));
    marker.markerGuid = VNX::authoringUuid(harkentest::StudioObjectId(mintedClipId));
    MarkerLedger markers;
    markers.items.push_back(marker);
    result = TimelineEditing::replaceMarkers(&manager, piece_, markers);
    EXPECT_FALSE(result.success);
    markers.items.front().markerGuid = QUuid::createUuid();
    result = TimelineEditing::replaceMarkers(&manager, piece_, markers);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    const harkenstudio::ObjectId mintedMarkerId = piece_->comp<VPiece>()
                                    ->markers.items.front().runtimeId();
    EXPECT_NE(mintedMarkerId, harkentest::StudioObjectId(0u));
    EXPECT_NE(mintedMarkerId, mintedClipId);

    markers.items.front().position = 25;
    result = TimelineEditing::replaceMarkers(&manager, piece_, markers);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(piece_->comp<VPiece>()->markers.items.front().runtimeId(),
              mintedMarkerId);
}

TEST_F(TimelineEditingTest, ClipReplacementRequiresExplicitNondecreasingHighWater)
{
    auto& manager = ProjectManager::instance();
    ClipLedger missingAuthority;
    missingAuthority.items.push_back(clipFor(sound_, 0, 10));
    const ActionResult missing = TimelineEditing::replaceClips(
        &manager, source_, missingAuthority);
    EXPECT_FALSE(missing.success);
    EXPECT_TRUE(source_->comp<VTrack>()->clips.empty());

    Clip authored = clipFor(sound_, 0, 10);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, source_, authored).success);
    ASSERT_TRUE(TimelineEditing::removeClip(
        &manager, source_, authored.clipGuid).success);
    ASSERT_EQ(source_->comp<VTrack>()
                  ->clips.occurrenceOrdinalHighWater, 1u);
    const ActionResult lowered = TimelineEditing::replaceClips(
        &manager, source_, ClipLedger{});
    EXPECT_FALSE(lowered.success);
    EXPECT_EQ(source_->comp<VTrack>()
                  ->clips.occurrenceOrdinalHighWater, 1u);
}

TEST_F(TimelineEditingTest, MarkerCrudRejectsInvalidReplacementAndRoundtripsCallbacks)
{
    auto& manager = ProjectManager::instance();
    Marker entry = markerAt(MarkerKind::Entry, -10);
    Marker custom = markerAt(MarkerKind::Custom, 20, QStringLiteral("Hit"));

    ActionResult result = TimelineEditing::addMarker(&manager, piece_, entry);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    result = TimelineEditing::addMarker(&manager, piece_, custom);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_EQ(piece_->comp<VPiece>()->markers.items.size(), 2u);

    custom.markerGuid = VNX::authoringUuid(harkentest::StudioObjectId(piece_->comp<VPiece>()->markers.items[1].runtimeId()));
    custom.position = 35;
    custom.name = QStringLiteral("Accent");
    result = TimelineEditing::updateMarker(&manager, piece_, custom);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(piece_->comp<VPiece>()->markers.items[1], custom);

    MarkerLedger invalid = piece_->comp<VPiece>()->markers;
    invalid.items.push_back(markerAt(MarkerKind::Entry, 0));
    const MarkerLedger beforeRejectedWrite = piece_->comp<VPiece>()->markers;
    result = TimelineEditing::replaceMarkers(&manager, piece_, invalid);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_ARGS"));
    EXPECT_EQ(piece_->comp<VPiece>()->markers, beforeRejectedWrite);

    result = TimelineEditing::removeMarker(&manager, piece_, custom.markerGuid);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_EQ(piece_->comp<VPiece>()->markers.items.size(), 1u);
    ASSERT_TRUE(result.undoFn);
    ASSERT_TRUE(result.redoFn);
    result.undoFn();
    EXPECT_EQ(piece_->comp<VPiece>()->markers, beforeRejectedWrite);
    result.redoFn();
    ASSERT_EQ(piece_->comp<VPiece>()->markers.items.size(), 1u);
    EXPECT_EQ(piece_->comp<VPiece>()->markers.items[0].markerGuid, entry.markerGuid);
    EXPECT_NE(piece_->comp<VPiece>()->markers.items[0].runtimeId(), harkentest::StudioObjectId(0u));
}

TEST_F(TimelineEditingTest, ClipReplacementRejectsDirectAndIndirectPieceCyclesAtomically)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* host = manager.createItem(
        layer_, QStringLiteral("HostPiece"), ProjectItem::Type::Piece);
    ProjectItem* hostTrack = manager.createItem(
        host, QStringLiteral("HostTrack"), ProjectItem::Type::Track);
    ProjectItem* nested = manager.createItem(
        layer_, QStringLiteral("NestedPiece"), ProjectItem::Type::Piece);
    ProjectItem* nestedTrack = manager.createItem(
        nested, QStringLiteral("NestedTrack"), ProjectItem::Type::Track);
    ASSERT_NE(hostTrack, nullptr);
    ASSERT_NE(nestedTrack, nullptr);

    const Clip direct = clipFor(host, 0, 10);
    ActionResult result = TimelineEditing::addClip(&manager, hostTrack, direct);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(hostTrack->comp<VTrack>()->clips.empty());

    const Clip backReference = clipFor(host, 0, 10);
    EXPECT_FALSE(TimelineEditing::addClip(&manager, nestedTrack, backReference).success);
    EXPECT_TRUE(nestedTrack->comp<VTrack>()->clips.empty());
    const Clip closesCycle = clipFor(nested, 0, 10);
    result = TimelineEditing::addClip(&manager, hostTrack, closesCycle);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(hostTrack->comp<VTrack>()->clips.empty());
}

TEST_F(TimelineEditingTest, CrossTrackMoveRejectsPieceCycleWithoutChangingEitherLedger)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* host = manager.createItem(
        layer_, QStringLiteral("MoveHostPiece"), ProjectItem::Type::Piece);
    ProjectItem* hostTrack = manager.createItem(
        host, QStringLiteral("MoveHostTrack"), ProjectItem::Type::Track);
    ProjectItem* nested = manager.createItem(
        layer_, QStringLiteral("MoveNestedPiece"), ProjectItem::Type::Piece);
    ProjectItem* nestedTrack = manager.createItem(
        nested, QStringLiteral("MoveNestedTrack"), ProjectItem::Type::Track);
    ASSERT_NE(hostTrack, nullptr);
    ASSERT_NE(nestedTrack, nullptr);

    const Clip backReference = clipFor(host, 0, 10);
    EXPECT_FALSE(TimelineEditing::addClip(&manager, nestedTrack, backReference).success);
    ProjectItem* nestedOwned = manager.createItem(
        source_, QStringLiteral("OwnedNestedPiece"), ProjectItem::Type::Piece);
    ASSERT_NE(nestedOwned, nullptr);
    const Clip moving = clipFor(nestedOwned, 20, 30);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, source_, moving).success);

    const ClipLedger sourceBefore = source_->comp<VTrack>()->clips;
    const ClipLedger destinationBefore = hostTrack->comp<VTrack>()->clips;
    const ActionResult result = TimelineEditing::moveClipAcrossTracks(
        &manager, source_, hostTrack, moving.clipGuid, 40);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_ARGS"));
    EXPECT_EQ(source_->comp<VTrack>()->clips, sourceBefore);
    EXPECT_EQ(hostTrack->comp<VTrack>()->clips, destinationBefore);
}

TEST_F(TimelineEditingTest, MusicalParentsAndReferencedSubtreesRejectAllTimelineWrites)
{
    auto& manager = ProjectManager::instance();
    auto* musicalTrack = manager.createItem(piece_, QStringLiteral("LegacyTrack"),
                                            ProjectItem::Type::Track);
    ASSERT_NE(musicalTrack, nullptr);
    const Clip original = clipFor(sound_, 80, 20);
    ASSERT_TRUE(TimelineEditing::addClip(&manager, source_, original).success);
    piece_->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    const auto sourceBefore = source_->comp<VTrack>()->clips;

    EXPECT_FALSE(TimelineEditing::addClip(&manager, musicalTrack,
                                         clipFor(sound_, 0, 10)).success);
    EXPECT_FALSE(TimelineEditing::addMarker(&manager, piece_,
                                        markerAt(MarkerKind::Custom, 960)).success);
    EXPECT_FALSE(TimelineEditing::moveClipAcrossTracks(
        &manager, source_, musicalTrack, original.clipGuid, 0).success);
    EXPECT_FALSE(TimelineEditing::addClip(&manager, destination_,
                                         clipFor(piece_, 0, 10)).success);
    auto* wrapper = manager.createItem(layer_, QStringLiteral("LegacyWrapper"),
                                       ProjectItem::Type::Container);
    ASSERT_NE(wrapper, nullptr);
    auto* wrapperTrack = manager.createItem(wrapper, QStringLiteral("WrapperTrack"),
                                            ProjectItem::Type::Track);
    ASSERT_NE(wrapperTrack, nullptr);
    Clip nested = clipFor(piece_, 0, 10);
    auto& nestedClips = wrapperTrack->comp<VTrack>()->clips;
    ASSERT_TRUE(nestedClips.mintOccurrenceOrdinal(&nested));
    nestedClips.items.push_back(nested);
    EXPECT_FALSE(TimelineEditing::addClip(&manager, destination_,
                                         clipFor(wrapper, 0, 10)).success);
    EXPECT_EQ(source_->comp<VTrack>()->clips, sourceBefore);
    EXPECT_TRUE(musicalTrack->comp<VTrack>()->clips.empty());
    EXPECT_TRUE(destination_->comp<VTrack>()->clips.empty());
    EXPECT_TRUE(piece_->comp<VPiece>()->markers.empty());
}

TEST_F(TimelineEditingTest, PreparedTrackRejectsExternalClipBeforeAnyIdentityOrSignal)
{
    auto& manager = ProjectManager::instance();
    const auto nextObject = manager.currentProject()->nextObjectID;
    const auto children = piece_->children;
    const bool dirty = piece_->dirtyBit;
    QObject observer;
    int createdCount = 0;
    QObject::connect(&manager, &ProjectManager::itemCreated, &observer,
                     [&](ProjectItem*) { ++createdCount; });
    EXPECT_EQ(manager.createTrackWithClip(piece_, QStringLiteral("PreparedTrack"),
                                          clipFor(sound_, 80000000, 1000000000)), nullptr);
    EXPECT_EQ(createdCount, 0);
    EXPECT_EQ(piece_->children, children);
    EXPECT_EQ(piece_->dirtyBit, dirty);
    EXPECT_EQ(sound_->parent, source_);
    EXPECT_EQ(manager.currentProject()->nextObjectID, nextObject);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(TimelineEditingTest, PreparedTrackRejectsUnsupportedTargetBeforeAnyIdentityOrSignal)
{
    auto& manager = ProjectManager::instance();
    auto* legacy = manager.createItem(layer_, QStringLiteral("LegacyTransitions"),
                                      ProjectItem::Type::Container);
    ASSERT_NE(legacy, nullptr);
    legacy->comp<VSelection>()->transitions = canonicalTransitionTable();
    UndoRedoManager::instance().clear();
    const auto nextObject = manager.currentProject()->nextObjectID;
    QObject observer;
    int createdCount = 0;
    QObject::connect(&manager, &ProjectManager::itemCreated, &observer,
                     [&](ProjectItem*) { ++createdCount; });

    for (const auto& clip : {clipFor(legacy, 0, 100), clipFor(piece_, 0, 100),
                             clipFor(sound_, 0, 0)})
        EXPECT_EQ(manager.createTrackWithClip(piece_, QStringLiteral("RejectedTrack"), clip), nullptr);
    EXPECT_TRUE(piece_->children.empty());
    EXPECT_EQ(createdCount, 0);
    EXPECT_EQ(manager.currentProject()->nextObjectID, nextObject);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}
