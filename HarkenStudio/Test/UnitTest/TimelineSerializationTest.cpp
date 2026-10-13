#include "Core/Components/VPiece.h"
#include "Core/Components/VTrack.h"
#include "Core/ProjectFormatVersion.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/ProjectTypes.h"
#include "Core/UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

#include <memory>

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

struct ProjectCloser
{
    ~ProjectCloser()
    {
        UndoRedoManager::instance().clear();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
    }
};

ClipLedger makeClipLedger(std::initializer_list<Clip> clips)
{
    ClipLedger ledger;
    for (Clip clip : clips) {
        EXPECT_TRUE(ledger.mintOccurrenceOrdinal(&clip));
        ledger.items.push_back(std::move(clip));
    }
    return ledger;
}

}

TEST(TimelineSerialization, EmptyComponentsRoundtrip)
{
    ProjectSerializer serializer;
    ProjectItem track(QStringLiteral("Track"), ProjectItem::Type::Track);
    track.guid = QUuid::createUuid();
    track.attachDefaultComponents();
    const QJsonObject saved = serializer.projectItemToJson(track);
    EXPECT_FALSE(saved.value(QStringLiteral("components")).toObject()
                     .contains(QStringLiteral("tracktimeline")));

    ProjectItem loaded;
    ASSERT_TRUE(serializer.projectItemFromJson(saved, loaded));
    ASSERT_NE(loaded.comp<VTrack>(), nullptr);
    EXPECT_TRUE(loaded.comp<VTrack>()->clips.empty());
}

TEST(TimelineSerialization, TimelineComponentsRoundtripWithStableTagsAndIntegers)
{
    ProjectSerializer serializer;
    ProjectItem track(QStringLiteral("Track"), ProjectItem::Type::Track);
    track.guid = QUuid::createUuid();
    track.attachDefaultComponents();
    const QUuid target = QUuid::createUuid();
    track.comp<VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), target, -9007199254740993LL, 9007199254740994LL}
    });

    const QJsonObject saved = serializer.projectItemToJson(track);
    const QJsonObject components = saved.value(QStringLiteral("components")).toObject();
    ASSERT_TRUE(components.contains(QStringLiteral("tracktimeline")));
    EXPECT_FALSE(components.contains(QStringLiteral("piecetimeline")));
    const QJsonObject timeline = components.value(QStringLiteral("tracktimeline")).toObject();
    EXPECT_EQ(timeline.keys(),
              (QStringList{QStringLiteral("clips"),
                           QStringLiteral("occurrenceOrdinalHighWater")}));
    EXPECT_FALSE(timeline.contains(QStringLiteral("fade")));
    EXPECT_FALSE(timeline.contains(QStringLiteral("crossfade")));
    EXPECT_EQ(timeline.value(QStringLiteral("occurrenceOrdinalHighWater")).toInteger(),
              1);

    ProjectItem loaded;
    ASSERT_TRUE(serializer.projectItemFromJson(saved, loaded));
    ASSERT_NE(loaded.comp<VTrack>(), nullptr);
    EXPECT_EQ(loaded.comp<VTrack>()->clips,
              track.comp<VTrack>()->clips);
    EXPECT_EQ(serializer.projectItemToJson(loaded), saved);
}

TEST(TimelineSerialization, TrackTimelineRequiresExactPersistedOccurrenceHighWater)
{
    VTrack source;
    source.clips = makeClipLedger({
        {QUuid::createUuid(), QUuid::createUuid(), 0, 10},
        {QUuid::createUuid(), QUuid::createUuid(), 20, 10},
    });
    QJsonObject json = source.toJson();
    VTrack restored;
    ASSERT_TRUE(restored.fromJson(json));
    EXPECT_EQ(restored.clips, source.clips);

    QJsonObject missing = json;
    missing.remove(QStringLiteral("occurrenceOrdinalHighWater"));
    VTrack sentinel;
    sentinel.clips = makeClipLedger({
        {QUuid::createUuid(), QUuid::createUuid(), 0, 1}});
    const ClipLedger before = sentinel.clips;
    EXPECT_FALSE(sentinel.fromJson(missing));
    EXPECT_EQ(sentinel.clips, before);

    QJsonObject tooLow = json;
    tooLow[QStringLiteral("occurrenceOrdinalHighWater")] = 1;
    EXPECT_FALSE(sentinel.fromJson(tooLow));
    EXPECT_EQ(sentinel.clips, before);

    QJsonObject retainedAfterDelete;
    retainedAfterDelete[QStringLiteral("occurrenceOrdinalHighWater")] = 2;
    ASSERT_TRUE(restored.fromJson(retainedAfterDelete));
    EXPECT_TRUE(restored.clips.empty());
    EXPECT_EQ(restored.clips.occurrenceOrdinalHighWater, 2u);
}

TEST(TimelineSerialization, PieceTimeAndMarkersRoundtripAndRejectUnknownTimelineFields)
{
    ProjectSerializer serializer;
    ProjectItem piece(QStringLiteral("Piece"), ProjectItem::Type::Piece);
    piece.guid = QUuid::createUuid();
    piece.attachDefaultComponents();
    auto* timeline = piece.comp<VPiece>();
    timeline->pieceTime = PieceTimeSpec::musical(600000, 3, 4);
    timeline->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, QStringLiteral("Ignored by UI"), -960},
        {QUuid::createUuid(), MarkerKind::Custom, QStringLiteral("Hit"), 0},
        {QUuid::createUuid(), MarkerKind::Exit, QString(), 1920}
    };

    QJsonObject saved = serializer.projectItemToJson(piece);
    QJsonObject components = saved.value(QStringLiteral("components")).toObject();
    components[QStringLiteral("futurecomponent")] = QJsonObject{{QStringLiteral("x"), 1}};
    saved[QStringLiteral("components")] = components;

    ProjectItem loaded;
    ASSERT_TRUE(serializer.projectItemFromJson(saved, loaded));
    ASSERT_NE(loaded.comp<VPiece>(), nullptr);
    EXPECT_EQ(loaded.comp<VPiece>()->pieceTime, timeline->pieceTime);
    EXPECT_EQ(loaded.comp<VPiece>()->markers, timeline->markers);
    const QJsonObject reserialized = serializer.projectItemToJson(loaded);
    const QJsonObject reserializedComponents =
        reserialized.value(QStringLiteral("components")).toObject();
    EXPECT_FALSE(reserializedComponents.contains(QStringLiteral("futurecomponent")));
    QJsonObject invalidTimeline = components.value(QStringLiteral("piecetimeline")).toObject();
    invalidTimeline[QStringLiteral("futureField")] = 7;
    EXPECT_FALSE(loaded.comp<VPiece>()->fromJson(invalidTimeline));
    EXPECT_EQ(loaded.comp<VPiece>()->pieceTime, timeline->pieceTime);
    EXPECT_EQ(loaded.comp<VPiece>()->markers, timeline->markers);
    components[QStringLiteral("piecetimeline")] = invalidTimeline;
    saved[QStringLiteral("components")] = components;
    ProjectItem rejected;
    EXPECT_FALSE(serializer.projectItemFromJson(saved, rejected));
}

TEST(TimelineSerialization, ComponentReadsAreAtomicWhenKnownDataIsInvalid)
{
    VTrack track;
    track.clips = makeClipLedger(
        {{QUuid::createUuid(), QUuid::createUuid(), 0, 10}});
    const ClipLedger originalClips = track.clips;
    QJsonObject invalidTrack;
    invalidTrack[QStringLiteral("clips")] = QJsonArray{
        QJsonObject{{QStringLiteral("clipGuid"), QUuid::createUuid().toString()},
                    {QStringLiteral("targetGuid"), QUuid::createUuid().toString()},
                    {QStringLiteral("start"), 0},
                    {QStringLiteral("duration"), 0}}
    };
    EXPECT_FALSE(track.fromJson(invalidTrack));
    EXPECT_EQ(track.clips, originalClips);

    VPiece piece;
    piece.pieceTime = PieceTimeSpec::musical();
    piece.markers.items = {{QUuid::createUuid(), MarkerKind::Custom, QStringLiteral("A"), 1}};
    const PieceTimeSpec originalTime = piece.pieceTime;
    const MarkerLedger originalMarkers = piece.markers;
    QJsonObject invalidPiece;
    invalidPiece[QStringLiteral("pieceTime")] = PieceTimeSpec{}.toJson();
    Marker first{QUuid::createUuid(), MarkerKind::Entry, QString(), 2};
    Marker second{QUuid::createUuid(), MarkerKind::Exit, QString(), 1};
    invalidPiece[QStringLiteral("markers")] = QJsonArray{first.toJson(), second.toJson()};
    EXPECT_FALSE(piece.fromJson(invalidPiece));
    EXPECT_EQ(piece.pieceTime, originalTime);
    EXPECT_EQ(piece.markers, originalMarkers);
}

TEST(TimelineSerialization, InvalidTimelineDataRejectsProjectItemLoad)
{
    ProjectSerializer serializer;
    ProjectItem track(QStringLiteral("Track"), ProjectItem::Type::Track);
    track.guid = QUuid::createUuid();
    track.attachDefaultComponents();
    track.comp<VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), QUuid::createUuid(), 0, 10}
    });
    QJsonObject json = serializer.projectItemToJson(track);
    QJsonObject components = json.value(QStringLiteral("components")).toObject();
    QJsonObject timeline = components.value(QStringLiteral("tracktimeline")).toObject();
    QJsonArray clips = timeline.value(QStringLiteral("clips")).toArray();
    QJsonObject invalidClip = clips[0].toObject();
    invalidClip[QStringLiteral("duration")] = 0;
    timeline[QStringLiteral("clips")] = QJsonArray{invalidClip};
    components[QStringLiteral("tracktimeline")] = timeline;
    json[QStringLiteral("components")] = components;

    ProjectItem loaded;
    EXPECT_FALSE(serializer.projectItemFromJson(json, loaded));
}

TEST(TimelineSerialization, LayersObjectsRoundtripPreserveTimelineData)
{
    ProjectSerializer serializer;
    ProjectItem piece(QStringLiteral("Piece"), ProjectItem::Type::Piece);
    piece.guid = QUuid::createUuid();
    piece.attachDefaultComponents();
    piece.comp<VPiece>()->pieceTime = PieceTimeSpec::musical(750000, 6, 8);
    piece.comp<VPiece>()->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, QString(), -480},
        {QUuid::createUuid(), MarkerKind::Exit, QString(), 1920}
    };
    auto track = std::make_shared<ProjectItem>(
        QStringLiteral("Track"), ProjectItem::Type::Track);
    track->guid = QUuid::createUuid();
    track->attachDefaultComponents();
    track->comp<VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), QUuid::createUuid(), -480, 2400}
    });
    const QJsonObject savedPiece = serializer.audioItemToJson(piece);
    const QJsonObject savedTrack = serializer.audioItemToJson(*track);
    ProjectItem loadedPiece;
    ProjectItem loadedTrack;
    ASSERT_TRUE(serializer.audioItemFromJson(savedPiece, loadedPiece));
    ASSERT_TRUE(serializer.audioItemFromJson(savedTrack, loadedTrack));
    ASSERT_NE(loadedPiece.comp<VPiece>(), nullptr);
    ASSERT_NE(loadedTrack.comp<VTrack>(), nullptr);
    EXPECT_EQ(loadedPiece.comp<VPiece>()->pieceTime,
              piece.comp<VPiece>()->pieceTime);
    EXPECT_EQ(loadedPiece.comp<VPiece>()->markers,
              piece.comp<VPiece>()->markers);
    EXPECT_EQ(loadedTrack.comp<VTrack>()->clips,
              track->comp<VTrack>()->clips);
    EXPECT_EQ(serializer.audioItemToJson(loadedPiece), savedPiece);
    EXPECT_EQ(serializer.audioItemToJson(loadedTrack), savedTrack);

    QJsonObject invalid = savedTrack;
    QJsonObject components = invalid.value(QStringLiteral("components")).toObject();
    QJsonObject timeline = components.value(QStringLiteral("tracktimeline")).toObject();
    QJsonArray clips = timeline.value(QStringLiteral("clips")).toArray();
    QJsonObject invalidClip = clips.at(0).toObject();
    invalidClip[QStringLiteral("duration")] = 0;
    timeline[QStringLiteral("clips")] = QJsonArray{invalidClip};
    components[QStringLiteral("tracktimeline")] = timeline;
    invalid[QStringLiteral("components")] = components;
    ProjectItem rejected;
    EXPECT_FALSE(serializer.audioItemFromJson(invalid, rejected));
}

TEST(TimelineSerialization, ProjectManagerClonePreservesAbsoluteTimelineAndRejectsMusical)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    auto& manager = ProjectManager::instance();
    if (manager.isProjectOpen()) manager.closeProject();
    ProjectCloser closer;
    ProjectConfig config;
    config.name = QStringLiteral("TimelineCloneProject");
    config.location = tempDir.path();
    ASSERT_TRUE(manager.createProject(config));

    ProjectItem* piece = manager.createItem(
        nullptr, QStringLiteral("Piece"), ProjectItem::Type::Piece);
    ASSERT_NE(piece, nullptr);
    ProjectItem* track = manager.createItem(
        piece, QStringLiteral("Track"), ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    ProjectItem* sound = manager.createItem(
        track, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    piece->comp<VPiece>()->markers.items = {
        {QUuid::createUuid(), MarkerKind::Custom, QStringLiteral("Hit"), 960}
    };
    track->comp<VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), sound->guid, -960, 1920}
    });

    ProjectItem* clone = manager.duplicateItem(piece);
    ASSERT_NE(clone, nullptr);
    ASSERT_NE(clone->comp<VPiece>(), nullptr);
    ASSERT_EQ(clone->children.size(), 1u);
    ASSERT_NE(clone->children.front()->comp<VTrack>(), nullptr);
    EXPECT_NE(clone->guid, piece->guid);
    EXPECT_EQ(clone->comp<VPiece>()->pieceTime,
              piece->comp<VPiece>()->pieceTime);
    const Marker& sourceMarker = piece->comp<VPiece>()->markers.items.front();
    const Marker& clonedMarker = clone->comp<VPiece>()->markers.items.front();
    EXPECT_NE(clonedMarker.markerGuid, sourceMarker.markerGuid);
    EXPECT_EQ(clonedMarker.kind, sourceMarker.kind);
    EXPECT_EQ(clonedMarker.name, sourceMarker.name);
    EXPECT_EQ(clonedMarker.position, sourceMarker.position);
    const Clip& sourceClip = track->comp<VTrack>()->clips.items.front();
    const Clip& clonedClip =
        clone->children.front()->comp<VTrack>()->clips.items.front();
    EXPECT_NE(clonedClip.clipGuid, sourceClip.clipGuid);
    ASSERT_EQ(clone->children.front()->children.size(), 1u);
    EXPECT_NE(clonedClip.targetGuid, sourceClip.targetGuid);
    EXPECT_EQ(clonedClip.targetGuid, clone->children.front()->children.front()->guid);
    EXPECT_EQ(clonedClip.start, sourceClip.start);
    EXPECT_EQ(clonedClip.duration, sourceClip.duration);

    const auto sourceTime = PieceTimeSpec::musical();
    piece->comp<VPiece>()->pieceTime = sourceTime;
    const auto nextObject = manager.currentProject()->nextObjectID;
    const auto rootCount = manager.currentProject()->layers.size();
    const auto undoCount = UndoRedoManager::instance().undoStack()->count();
    EXPECT_EQ(manager.duplicateItem(piece), nullptr);
    EXPECT_EQ(manager.currentProject()->nextObjectID, nextObject);
    EXPECT_EQ(manager.currentProject()->layers.size(), rootCount);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
    EXPECT_EQ(piece->comp<VPiece>()->pieceTime, sourceTime);
}
