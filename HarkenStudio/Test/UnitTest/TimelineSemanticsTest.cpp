#include "Core/Components/VAudio.h"
#include "Core/Components/VPiece.h"
#include "Core/Components/VSelection.h"
#include "Core/Components/VTrack.h"
#include "Core/ProjectTypes.h"
#include "Core/Timeline/TimelineSemantics.h"

#include <gtest/gtest.h>

#include <QHash>

#include <limits>
#include <memory>

using namespace VNX;

namespace {

std::shared_ptr<ProjectItem> makeItem(ProjectItem::Type type, const QString& name)
{
    auto item = std::make_shared<ProjectItem>(name, type);
    item->guid = QUuid::createUuid();
    item->attachDefaultComponents();
    return item;
}

Clip clipTo(const ProjectItem& target, int64_t start, int64_t duration)
{
    return {QUuid::createUuid(), target.guid, start, duration};
}

ClipLedger makeLedger(std::initializer_list<Clip> clips)
{
    ClipLedger ledger;
    for (Clip clip : clips) {
        EXPECT_TRUE(ledger.mintOccurrenceOrdinal(&clip));
        ledger.items.push_back(std::move(clip));
    }
    return ledger;
}

TimelineItemResolver resolverFor(const std::vector<std::shared_ptr<ProjectItem>>& items)
{
    auto index = std::make_shared<QHash<QUuid, const ProjectItem*>>();
    for (const auto& item : items) index->insert(item->guid, item.get());
    return [index](const QUuid& guid) { return index->value(guid, nullptr); };
}

}

TEST(TimelineSemantics, ContentBoundsAndTrackNominalUseAuthoredClipGeometry)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    auto track = makeItem(ProjectItem::Type::Track, QStringLiteral("Track"));
    track->comp<VTrack>()->clips = makeLedger({
        clipTo(*sound, -10, 4), clipTo(*sound, 3, 8)
    });
    const auto resolve = resolverFor({sound, track});
    const TimelineSemanticsResult result = TimelineSemantics::trackNominalSpan(
        *track, resolve);
    ASSERT_TRUE(result) << result.validation.message.toStdString();
    EXPECT_EQ(result.content, (TimelineSpan{-10, 11}));
    EXPECT_EQ(result.nominal, (TimelineSpan{0, 11}));

    track->comp<VTrack>()->clips = makeLedger({clipTo(*sound, -10, 3)});
    const TimelineSemanticsResult negative = TimelineSemantics::trackNominalSpan(
        *track, resolve);
    ASSERT_TRUE(negative);
    EXPECT_EQ(negative.nominal, (TimelineSpan{0, 0}));
}

TEST(TimelineSemantics, OverlapRegionsExcludeEmptySingleAndTouchingClips)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    EXPECT_TRUE(TimelineSemantics::overlapRegions({}).empty());
    EXPECT_TRUE(TimelineSemantics::overlapRegions(
        makeLedger({clipTo(*sound, 0, 10)})).empty());
    EXPECT_TRUE(TimelineSemantics::overlapRegions(
        makeLedger({clipTo(*sound, 0, 10), clipTo(*sound, 10, 10)})).empty());
}

TEST(TimelineSemantics, OverlapRegionsMergeContinuousDoubleAndTripleCoverage)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    const ClipLedger clips = makeLedger({
        clipTo(*sound, 0, 10),
        clipTo(*sound, 2, 6),
        clipTo(*sound, 4, 8),
        clipTo(*sound, 8, 6),
    });
    EXPECT_EQ(TimelineSemantics::overlapRegions(clips),
              (std::vector<TimelineSpan>{{2, 12}}));
}

TEST(TimelineSemantics, OverlapRegionsKeepSeparatedNegativeAndPositiveRegions)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    const ClipLedger clips = makeLedger({
        clipTo(*sound, -10, 10),
        clipTo(*sound, -8, 3),
        clipTo(*sound, 5, 5),
        clipTo(*sound, 6, 2),
    });
    EXPECT_EQ(TimelineSemantics::overlapRegions(clips),
              (std::vector<TimelineSpan>{{-8, -5}, {6, 8}}));
}

TEST(TimelineSemantics, OverlapRegionsRejectInvalidClipStructure)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    Clip invalidDuration = clipTo(*sound, 0, 1);
    invalidDuration.duration = 0;
    EXPECT_TRUE(TimelineSemantics::overlapRegions(
        makeLedger({invalidDuration})).empty());

    Clip overflow = clipTo(*sound, std::numeric_limits<int64_t>::max(), 1);
    EXPECT_TRUE(TimelineSemantics::overlapRegions(makeLedger({overflow})).empty());

    Clip first = clipTo(*sound, 0, 10);
    Clip duplicate = clipTo(*sound, 2, 5);
    duplicate.clipGuid = first.clipGuid;
    EXPECT_TRUE(TimelineSemantics::overlapRegions(
        makeLedger({first, duplicate})).empty());
}

TEST(TimelineSemantics, PieceCombinesDirectTracksAndDerivesMarkerRegions)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    auto piece = makeItem(ProjectItem::Type::Piece, QStringLiteral("Piece"));
    auto first = makeItem(ProjectItem::Type::Track, QStringLiteral("First"));
    auto second = makeItem(ProjectItem::Type::Track, QStringLiteral("Second"));
    first->comp<VTrack>()->clips = makeLedger({clipTo(*sound, -20, 30)});
    second->comp<VTrack>()->clips = makeLedger({clipTo(*sound, 5, 30)});
    first->parent = piece.get();
    second->parent = piece.get();
    piece->children = {first, second};
    piece->comp<VPiece>()->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, QString(), -5},
        {QUuid::createUuid(), MarkerKind::Exit, QString(), 25}
    };

    const TimelineSemanticsResult result = TimelineSemantics::pieceNominalSpan(
        *piece, resolverFor({sound, piece, first, second}));
    ASSERT_TRUE(result) << result.validation.message.toStdString();
    EXPECT_EQ(result.content, (TimelineSpan{-20, 35}));
    EXPECT_EQ(result.nominal, (TimelineSpan{-5, 25}));
    EXPECT_EQ(result.pickup, 15);
    EXPECT_EQ(result.tail, 10);
}

TEST(TimelineSemantics, MissingMarkersDegenerateToContentAndEmptyIsKnownZero)
{
    auto piece = makeItem(ProjectItem::Type::Piece, QStringLiteral("Piece"));
    const TimelineSemanticsResult empty = TimelineSemantics::pieceNominalSpan(
        *piece, resolverFor({piece}));
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty.content, (TimelineSpan{0, 0}));
    EXPECT_EQ(empty.nominal, (TimelineSpan{0, 0}));

    piece->comp<VPiece>()->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, QString(), -2}
    };
    const TimelineSemanticsResult entryOnly = TimelineSemantics::pieceNominalSpan(
        *piece, resolverFor({piece}));
    ASSERT_TRUE(entryOnly);
    EXPECT_EQ(entryOnly.nominal, (TimelineSpan{-2, 0}));
}

TEST(TimelineSemantics, QueuePreviewComparesButtAndMarkerSpliceInSeconds)
{
    constexpr int64_t second = 1000000000;
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    auto queue = makeItem(ProjectItem::Type::Container, QStringLiteral("Queue"));
    auto absolute = makeItem(ProjectItem::Type::Piece, QStringLiteral("Absolute"));
    auto absoluteTrack = makeItem(ProjectItem::Type::Track, QStringLiteral("AbsoluteTrack"));
    auto musical = makeItem(ProjectItem::Type::Piece, QStringLiteral("Musical"));
    auto musicalTrack = makeItem(ProjectItem::Type::Track, QStringLiteral("MusicalTrack"));

    queue->comp<VSelection>()->mode = SelectionMode::Sequence;
    queue->comp<VSelection>()->playType = SelectionPlayType::Queue;
    absoluteTrack->comp<VTrack>()->clips = makeLedger({
        clipTo(*sound, -second, 5 * second)
    });
    absoluteTrack->parent = absolute.get();
    absolute->children = {absoluteTrack};
    absolute->comp<VPiece>()->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, {}, 0},
        {QUuid::createUuid(), MarkerKind::Exit, {}, 3 * second}
    };

    musical->comp<VPiece>()->pieceTime = PieceTimeSpec::musical(500000);
    musicalTrack->comp<VTrack>()->clips = makeLedger({
        clipTo(*sound, -960, 6720)
    });
    musicalTrack->parent = musical.get();
    musical->children = {musicalTrack};
    musical->comp<VPiece>()->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, {}, 0},
        {QUuid::createUuid(), MarkerKind::Exit, {}, 3840}
    };

    absolute->parent = queue.get();
    musical->parent = queue.get();
    queue->children = {absolute, musical};
    const auto resolve = resolverFor(
        {sound, queue, absolute, absoluteTrack, musical, musicalTrack});

    const QueuePreviewResult butt = TimelineSemantics::queuePreview(
        *queue, resolve, false);
    ASSERT_TRUE(butt);
    ASSERT_EQ(butt.items.size(), 2u);
    ASSERT_TRUE(butt.items[0]);
    ASSERT_TRUE(butt.items[1]);
    EXPECT_EQ(butt.items[0].contentNanoseconds, (TimelineSpan{0, 5 * second}));
    EXPECT_EQ(butt.items[0].nominalNanoseconds, (TimelineSpan{second, 4 * second}));
    EXPECT_EQ(butt.items[1].contentNanoseconds,
              (TimelineSpan{5 * second, 8500000000LL}));
    EXPECT_EQ(butt.items[1].nominalNanoseconds,
              (TimelineSpan{5500000000LL, 7500000000LL}));
    EXPECT_EQ(butt.endNanoseconds, 8500000000LL);

    const QueuePreviewResult splice = TimelineSemantics::queuePreview(
        *queue, resolve, true);
    ASSERT_TRUE(splice);
    ASSERT_EQ(splice.items.size(), 2u);
    EXPECT_EQ(splice.items[0].contentNanoseconds,
              (TimelineSpan{-second, 4 * second}));
    EXPECT_EQ(splice.items[0].nominalNanoseconds,
              (TimelineSpan{0, 3 * second}));
    EXPECT_EQ(splice.items[1].contentNanoseconds,
              (TimelineSpan{2500000000LL, 6 * second}));
    EXPECT_EQ(splice.items[1].nominalNanoseconds,
              (TimelineSpan{3 * second, 5 * second}));
    EXPECT_EQ(splice.items[1].pickupNanoseconds, 500000000);
    EXPECT_EQ(splice.items[1].tailNanoseconds, second);
    EXPECT_TRUE(splice.items[0].entryAnchor);
    EXPECT_TRUE(splice.items[1].entryAnchor);
    EXPECT_EQ(splice.endNanoseconds, 5 * second);
}

TEST(TimelineSemantics, QueuePreviewUnknownBlocksLaterCoordinatesWithoutFakeWidth)
{
    auto queue = makeItem(ProjectItem::Type::Container, QStringLiteral("Queue"));
    auto unknown = makeItem(ProjectItem::Type::Sound, QStringLiteral("Unknown"));
    auto known = makeItem(ProjectItem::Type::Track, QStringLiteral("Known"));
    queue->comp<VSelection>()->mode = SelectionMode::Random;
    queue->comp<VSelection>()->playType = SelectionPlayType::Queue;
    unknown->parent = queue.get();
    known->parent = queue.get();
    queue->children = {unknown, known};

    const QueuePreviewResult preview = TimelineSemantics::queuePreview(
        *queue, resolverFor({queue, unknown, known}), true);
    ASSERT_TRUE(preview);
    ASSERT_EQ(preview.items.size(), 2u);
    EXPECT_FALSE(preview.items[0]);
    EXPECT_EQ(preview.items[0].validation.error, TimelineError::InvalidTargetType);
    EXPECT_FALSE(preview.items[1]);
    EXPECT_TRUE(preview.items[1].validation.message.contains(
        QStringLiteral("after unknown Queue item")));
    EXPECT_EQ(preview.endNanoseconds, 0);
}

TEST(TimelineSemantics, QueuePreviewUsesKnownSoundNaturalDuration)
{
    constexpr int64_t second = 1000000000;
    auto queue = makeItem(ProjectItem::Type::Container, QStringLiteral("Queue"));
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    queue->comp<VSelection>()->mode = SelectionMode::Sequence;
    queue->comp<VSelection>()->playType = SelectionPlayType::Queue;
    sound->parent = queue.get();
    queue->children = {sound};

    const QueuePreviewResult preview = TimelineSemantics::queuePreview(
        *queue, resolverFor({queue, sound}), false,
        [](const ProjectItem&) { return std::optional<int64_t>{2 * second}; });
    ASSERT_TRUE(preview);
    ASSERT_EQ(preview.items.size(), 1u);
    ASSERT_TRUE(preview.items[0]);
    EXPECT_EQ(preview.items[0].contentNanoseconds, (TimelineSpan{0, 2 * second}));
    EXPECT_EQ(preview.items[0].nominalNanoseconds, (TimelineSpan{0, 2 * second}));
    EXPECT_FALSE(preview.items[0].entryAnchor);
}

TEST(TimelineSemantics, SoundNominalSpanUsesTheSharedNaturalDurationResolver)
{
    constexpr int64_t second = 1000000000;
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    const TimelineSemanticsResult known = TimelineSemantics::nominalSpanOf(
        *sound, resolverFor({sound}),
        [](const ProjectItem&) { return std::optional<int64_t>{2 * second}; });
    ASSERT_TRUE(known);
    EXPECT_EQ(known.content, (TimelineSpan{0, 2 * second}));
    EXPECT_EQ(known.nominal, known.content);

    const TimelineSemanticsResult invalid = TimelineSemantics::nominalSpanOf(
        *sound, resolverFor({sound}),
        [](const ProjectItem&) { return std::optional<int64_t>{0}; });
    EXPECT_FALSE(invalid);
    EXPECT_EQ(invalid.validation.error, TimelineError::NonPositiveDuration);
}

TEST(TimelineSemantics, SoundLoopCountDoesNotChangeNominalSpan)
{
    constexpr int64_t duration = 2000000000;
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    for (const uint32_t loopCount : {0u, 1u, 2u, 17u}) {
        sound->comp<VAudio>()->loopCount = loopCount;
        const TimelineSemanticsResult result = TimelineSemantics::nominalSpanOf(
            *sound, resolverFor({sound}),
            [](const ProjectItem&) { return std::optional<int64_t>{2000000000}; });
        ASSERT_TRUE(result);
        EXPECT_EQ(result.nominal, (TimelineSpan{0, duration}));
    }
}

TEST(TimelineSemantics, QueuePreviewProjectsLoopBadgeWithoutExpandingSpan)
{
    constexpr int64_t duration = 2000000000;
    auto queue = makeItem(ProjectItem::Type::Container, QStringLiteral("Queue"));
    auto once = makeItem(ProjectItem::Type::Sound, QStringLiteral("Once"));
    auto finite = makeItem(ProjectItem::Type::Sound, QStringLiteral("Finite"));
    auto infinite = makeItem(ProjectItem::Type::Sound, QStringLiteral("Infinite"));
    queue->comp<VSelection>()->mode = SelectionMode::Sequence;
    queue->comp<VSelection>()->playType = SelectionPlayType::Queue;
    once->comp<VAudio>()->loopCount = 1;
    finite->comp<VAudio>()->loopCount = 4;
    infinite->comp<VAudio>()->loopCount = 0;
    for (const auto& child : {once, finite, infinite}) child->parent = queue.get();
    queue->children = {once, finite, infinite};

    const QueuePreviewResult preview = TimelineSemantics::queuePreview(
        *queue, resolverFor({queue, once, finite, infinite}), false,
        [](const ProjectItem&) { return std::optional<int64_t>{2000000000}; });
    ASSERT_TRUE(preview);
    ASSERT_EQ(preview.items.size(), 3u);
    EXPECT_EQ(preview.items[0].loopKind, LoopPreviewKind::Off);
    EXPECT_EQ(preview.items[1].loopKind, LoopPreviewKind::Finite);
    EXPECT_EQ(preview.items[1].loopCount, 4u);
    EXPECT_EQ(preview.items[2].loopKind, LoopPreviewKind::Infinite);
    EXPECT_TRUE(preview.items[2].stopCompletesInfiniteBody);
    EXPECT_EQ(preview.endNanoseconds, 3 * duration);
}

TEST(TimelineSemantics, QueuePreviewRejectsUnrepresentablePickupAndBlocksFollowers)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    auto queue = makeItem(ProjectItem::Type::Container, QStringLiteral("Queue"));
    auto extreme = makeItem(ProjectItem::Type::Track, QStringLiteral("Extreme"));
    auto follower = makeItem(ProjectItem::Type::Track, QStringLiteral("Follower"));
    queue->comp<VSelection>()->mode = SelectionMode::Sequence;
    queue->comp<VSelection>()->playType = SelectionPlayType::Queue;
    extreme->comp<VTrack>()->clips = makeLedger({
        clipTo(*sound, std::numeric_limits<int64_t>::min(),
               std::numeric_limits<int64_t>::max())
    });
    extreme->parent = queue.get();
    follower->parent = queue.get();
    queue->children = {extreme, follower};

    const QueuePreviewResult preview = TimelineSemantics::queuePreview(
        *queue, resolverFor({sound, queue, extreme, follower}), true);
    ASSERT_TRUE(preview);
    ASSERT_EQ(preview.items.size(), 2u);
    EXPECT_FALSE(preview.items[0]);
    EXPECT_EQ(preview.items[0].validation.error, TimelineError::CoordinateOverflow);
    EXPECT_TRUE(preview.items[0].validation.message.contains(QStringLiteral("pickup")));
    EXPECT_FALSE(preview.items[1]);
    EXPECT_TRUE(preview.items[1].validation.message.contains(
        QStringLiteral("after unknown Queue item")));
}

TEST(TimelineSemantics, NestedPieceReferencesPropagateMissingAndCycleUnknown)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    auto outer = makeItem(ProjectItem::Type::Piece, QStringLiteral("Outer"));
    auto outerTrack = makeItem(ProjectItem::Type::Track, QStringLiteral("OuterTrack"));
    auto inner = makeItem(ProjectItem::Type::Piece, QStringLiteral("Inner"));
    auto innerTrack = makeItem(ProjectItem::Type::Track, QStringLiteral("InnerTrack"));
    innerTrack->comp<VTrack>()->clips = makeLedger({clipTo(*sound, 0, 4)});
    innerTrack->parent = inner.get();
    inner->children = {innerTrack};
    outerTrack->comp<VTrack>()->clips = makeLedger({clipTo(*inner, 10, 20)});
    outerTrack->parent = outer.get();
    outer->children = {outerTrack};

    const auto all = resolverFor({sound, outer, outerTrack, inner, innerTrack});
    const TimelineSemanticsResult known = TimelineSemantics::pieceNominalSpan(*outer, all);
    ASSERT_TRUE(known) << known.validation.message.toStdString();
    EXPECT_EQ(known.content, (TimelineSpan{10, 30}));

    innerTrack->comp<VTrack>()->clips = makeLedger({
        {QUuid::createUuid(), QUuid::createUuid(), 0, 1}
    });
    const TimelineSemanticsResult missing = TimelineSemantics::pieceNominalSpan(*outer, all);
    EXPECT_FALSE(missing);
    EXPECT_EQ(missing.validation.error, TimelineError::MissingTarget);

    innerTrack->comp<VTrack>()->clips = makeLedger({clipTo(*outer, 0, 1)});
    const TimelineSemanticsResult cycle = TimelineSemantics::pieceNominalSpan(*outer, all);
    EXPECT_FALSE(cycle);
    EXPECT_EQ(cycle.validation.error, TimelineError::Cycle);
}

TEST(TimelineSemantics, SharedNestedPieceInDagDoesNotLookLikeCycle)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    auto shared = makeItem(ProjectItem::Type::Piece, QStringLiteral("Shared"));
    auto sharedTrack = makeItem(ProjectItem::Type::Track, QStringLiteral("SharedTrack"));
    sharedTrack->comp<VTrack>()->clips = makeLedger({clipTo(*sound, 0, 1)});
    shared->children = {sharedTrack};

    auto root = makeItem(ProjectItem::Type::Piece, QStringLiteral("Root"));
    auto rootTrack = makeItem(ProjectItem::Type::Track, QStringLiteral("RootTrack"));
    rootTrack->comp<VTrack>()->clips = makeLedger({
        clipTo(*shared, 0, 1), clipTo(*shared, 2, 1)
    });
    root->children = {rootTrack};

    const TimelineSemanticsResult result = TimelineSemantics::pieceNominalSpan(
        *root, resolverFor({sound, shared, sharedTrack, root, rootTrack}));
    EXPECT_TRUE(result) << result.validation.message.toStdString();
}

TEST(TimelineSemantics, RejectsDerivedInvertedNominalAndUnrepresentableSpan)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    auto piece = makeItem(ProjectItem::Type::Piece, QStringLiteral("Piece"));
    piece->comp<VPiece>()->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, QString(), 1}
    };
    const auto resolve = resolverFor({sound, piece});
    const TimelineSemanticsResult inverted = TimelineSemantics::pieceNominalSpan(
        *piece, resolve);
    EXPECT_FALSE(inverted);
    EXPECT_EQ(inverted.validation.error, TimelineError::MarkerOrder);

    ClipLedger clips = makeLedger({
        clipTo(*sound, std::numeric_limits<int64_t>::min(), 1),
        clipTo(*sound, std::numeric_limits<int64_t>::max() - 1, 1)
    });
    const TimelineSemanticsResult overflow = TimelineSemantics::contentBounds(clips);
    EXPECT_FALSE(overflow);
    EXPECT_EQ(overflow.validation.error, TimelineError::CoordinateOverflow);
}

TEST(TimelineSemantics, RejectsInvalidPieceOwnershipStructure)
{
    auto piece = makeItem(ProjectItem::Type::Piece, QStringLiteral("Piece"));
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    piece->children = {sound};
    const TimelineSemanticsResult result = TimelineSemantics::pieceNominalSpan(
        *piece, resolverFor({piece, sound}));
    EXPECT_FALSE(result);
    EXPECT_EQ(result.validation.error, TimelineError::InvalidTargetType);
}

TEST(TimelineSemantics, DeepPieceReferenceGraphIsResolvedIteratively)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    std::vector<std::shared_ptr<ProjectItem>> items = {sound};
    constexpr int kDepth = 2048;
    for (int i = 0; i < kDepth; ++i) {
        auto piece = makeItem(ProjectItem::Type::Piece, QStringLiteral("Piece"));
        auto track = makeItem(ProjectItem::Type::Track, QStringLiteral("Track"));
        const ProjectItem& target = i == 0 ? *sound : *items[items.size() - 2];
        track->comp<VTrack>()->clips = makeLedger({clipTo(target, 0, 1)});
        piece->children = {track};
        items.push_back(piece);
        items.push_back(track);
    }
    const TimelineSemanticsResult result = TimelineSemantics::pieceNominalSpan(
        *items[items.size() - 2], resolverFor(items));
    EXPECT_TRUE(result) << result.validation.message.toStdString();
}

TEST(TimelineSemantics, TimelineEntityGuidsAreUniqueAcrossProject)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    auto piece = makeItem(ProjectItem::Type::Piece, QStringLiteral("Piece"));
    auto first = makeItem(ProjectItem::Type::Track, QStringLiteral("First"));
    auto second = makeItem(ProjectItem::Type::Track, QStringLiteral("Second"));
    const QUuid sharedGuid = QUuid::createUuid();
    first->comp<VTrack>()->clips = makeLedger({
        {sharedGuid, sound->guid, 0, 1}
    });
    second->comp<VTrack>()->clips = makeLedger({
        {sharedGuid, sound->guid, 2, 1}
    });
    piece->children = {first, second};
    TimelineValidation validity = TimelineSemantics::validateProjectTimelineIdentities(
        {piece.get(), sound.get()});
    EXPECT_FALSE(validity);
    EXPECT_EQ(validity.error, TimelineError::DuplicateGuid);

    second->comp<VTrack>()->clips.items.front().clipGuid = QUuid::createUuid();
    piece->comp<VPiece>()->markers.items = {
        {first->guid, MarkerKind::Custom, QStringLiteral("Marker"), 0}
    };
    validity = TimelineSemantics::validateProjectTimelineIdentities(
        {piece.get(), sound.get()});
    EXPECT_FALSE(validity);
    EXPECT_EQ(validity.error, TimelineError::DuplicateGuid);

    piece->comp<VPiece>()->markers.items.front().markerGuid = QUuid::createUuid();
    validity = TimelineSemantics::validateProjectTimelineIdentities(
        {piece.get(), sound.get()});
    EXPECT_TRUE(validity) << validity.message.toStdString();
}
