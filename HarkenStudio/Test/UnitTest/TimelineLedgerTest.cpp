#include "Core/ProjectTypes.h"
#include "Core/Timeline/TimelineTypes.h"

#include <gtest/gtest.h>

#include <QHash>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace VNX;

namespace {

Clip makeClip(const QUuid& target, int64_t start = 0, int64_t duration = 1)
{
    return {QUuid::createUuid(), target, start, duration};
}

Marker makeMarker(MarkerKind kind, int64_t position, const QString& name = {})
{
    return {QUuid::createUuid(), kind, name, position};
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

QJsonObject ledgerJson(const QJsonArray& rows, uint64_t highWater)
{
    return {{QStringLiteral("occurrenceOrdinalHighWater"),
             static_cast<qint64>(highWater)},
            {QStringLiteral("rows"), rows}};
}

}

TEST(TimelineLedger, ClipJsonRoundtripPreservesSignedIntegersAndGuids)
{
    ClipLedger ledger = makeLedger({
        makeClip(QUuid::createUuid(), -9007199254740993LL, 17)});
    const QJsonObject json = ledger.toJsonObject();
    QString error;
    const auto parsed = ClipLedger::fromJson(json, &error);
    ASSERT_TRUE(parsed.has_value()) << error.toStdString();
    EXPECT_EQ(*parsed, ledger);
    EXPECT_EQ(json.value(QStringLiteral("rows")).toArray()[0].toObject()[
                  QStringLiteral("start")].toInteger(),
              -9007199254740993LL);
}

TEST(TimelineLedger, ClipParserRejectsTheWholeInvalidLedger)
{
    const QUuid target = QUuid::createUuid();
    const Clip valid = makeClip(target, -5, 10);
    Clip invalid = makeClip(target, 0, 0);
    const QJsonObject json = ledgerJson(
        QJsonArray{valid.toJson(), invalid.toJson()}, 2);
    QString error;
    EXPECT_FALSE(ClipLedger::fromJson(json, &error).has_value());
    EXPECT_FALSE(error.isEmpty());

    invalid.duration = 1;
    invalid.clipGuid = valid.clipGuid;
    EXPECT_FALSE(ClipLedger::fromJson(
        ledgerJson(QJsonArray{valid.toJson(), invalid.toJson()}, 2)).has_value());

    invalid.clipGuid = QUuid::createUuid();
    invalid.start = std::numeric_limits<int64_t>::max();
    invalid.duration = 1;
    EXPECT_FALSE(ClipLedger::fromJson(
        ledgerJson(QJsonArray{invalid.toJson()}, 1)).has_value());

    QJsonObject outOfRange = valid.toJson();
    outOfRange[QStringLiteral("start")] = std::ldexp(1.0, 63);
    EXPECT_FALSE(ClipLedger::fromJson(
        ledgerJson(QJsonArray{outOfRange}, 1)).has_value());
}

TEST(TimelineLedger, ClipTargetsRequireDirectSoundContainerAndPieceChildren)
{
    ProjectItem track(QStringLiteral("Track"), ProjectItem::Type::Track);
    ProjectItem layer(QStringLiteral("Layer"), ProjectItem::Type::Layer);
    for (const auto type : {ProjectItem::Type::Sound, ProjectItem::Type::Container,
                            ProjectItem::Type::Piece}) {
        auto child = std::make_shared<ProjectItem>(QStringLiteral("Child"), type);
        child->guid = QUuid::createUuid();
        child->parent = &track;
        track.children.push_back(std::move(child));
    }
    auto& sound = *track.children[0];
    auto& container = *track.children[1];
    auto& piece = *track.children[2];
    for (ProjectItem* item : {&track, &layer})
        item->guid = QUuid::createUuid();

    QHash<QUuid, const ProjectItem*> items{
        {sound.guid, &sound}, {container.guid, &container}, {piece.guid, &piece},
        {track.guid, &track}, {layer.guid, &layer}
    };
    const TimelineItemResolver resolve = [&](const QUuid& guid) {
        return items.value(guid, nullptr);
    };

    ClipLedger valid = makeLedger(
        {makeClip(sound.guid), makeClip(container.guid), makeClip(piece.guid)});
    EXPECT_TRUE(valid.validateTargets(track, resolve));
    EXPECT_TRUE(ClipLedger::fromJson(valid.toJsonObject(), track, resolve).has_value());

    ClipLedger trackTarget = makeLedger({makeClip(track.guid)});
    EXPECT_EQ(trackTarget.validateTargets(track, resolve).error,
              TimelineError::InvalidTargetType);

    ClipLedger layerTarget = makeLedger({makeClip(layer.guid)});
    EXPECT_EQ(layerTarget.validateTargets(track, resolve).error,
              TimelineError::InvalidTargetType);

    ClipLedger missing = makeLedger({makeClip(QUuid::createUuid())});
    EXPECT_EQ(missing.validateTargets(track, resolve).error, TimelineError::MissingTarget);
    EXPECT_FALSE(ClipLedger::fromJson(
        missing.toJsonObject(), track, resolve).has_value());

    sound.parent = &container;
    EXPECT_EQ(valid.validateTargets(track, resolve).error, TimelineError::InvalidOwnership);
    EXPECT_FALSE(ClipLedger::fromJson(valid.toJsonObject(), track, resolve).has_value());
    EXPECT_TRUE(ClipLedger::fromJson(valid.toJsonObject()).has_value());
    sound.parent = &track;
    auto impostor = std::make_shared<ProjectItem>(QStringLiteral("Impostor"), ProjectItem::Type::Sound);
    impostor->guid = sound.guid;
    impostor->parent = &track;
    items[sound.guid] = impostor.get();
    EXPECT_EQ(valid.validateTargets(track, resolve).error, TimelineError::InvalidOwnership);
}

TEST(TimelineLedger, ClipOccurrenceEnvelopeOrdinalAndHighWaterAreExact)
{
    Clip first = makeClip(QUuid::createUuid());
    first.playableProperties.presence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::LoopCount);
    ASSERT_TRUE(first.playableProperties.setValue(
        harkenplayableproperties::PlayablePropertyIdV1::LoopCount, 1u));
    ClipLedger ledger = makeLedger({first, makeClip(first.targetGuid)});
    const QJsonArray json = ledger.toJson();
    ASSERT_EQ(json.size(), 2);
    EXPECT_EQ(json[0].toObject().value(
                  QStringLiteral("stableOccurrenceOrdinal")).toInteger(), 0);
    EXPECT_TRUE(json[0].toObject().value(
        QLatin1String(harkenplayableproperties::studio::kEnvelopeJsonKeyV1)).isObject());

    QJsonObject missingEnvelope = json[0].toObject();
    missingEnvelope.remove(
        QLatin1String(harkenplayableproperties::studio::kEnvelopeJsonKeyV1));
    EXPECT_FALSE(Clip::fromJson(missingEnvelope).has_value());
    QJsonObject missingOrdinal = json[0].toObject();
    missingOrdinal.remove(QStringLiteral("stableOccurrenceOrdinal"));
    EXPECT_FALSE(Clip::fromJson(missingOrdinal).has_value());

    ClipLedger duplicate = ledger;
    duplicate.items[1].stableOccurrenceOrdinal = 0;
    EXPECT_FALSE(duplicate.validateStructure());
    ClipLedger insufficient = ledger;
    insufficient.occurrenceOrdinalHighWater = 1;
    EXPECT_FALSE(insufficient.validateStructure());

    ClipLedger reordered = ledger;
    std::swap(reordered.items[0], reordered.items[1]);
    ASSERT_TRUE(reordered.validateStructure());
    EXPECT_EQ(reordered.items[0].stableOccurrenceOrdinal, 1u);
    EXPECT_EQ(reordered.items[1].stableOccurrenceOrdinal, 0u);

    ClipLedger exhausted;
    exhausted.occurrenceOrdinalHighWater = UINT64_C(0x100000000);
    Clip fresh = makeClip(first.targetGuid);
    EXPECT_FALSE(exhausted.mintOccurrenceOrdinal(&fresh));
    EXPECT_EQ(exhausted.occurrenceOrdinalHighWater, UINT64_C(0x100000000));
}

TEST(TimelineLedger, ExactObjectNeverInfersHighWaterFromRows)
{
    ClipLedger ledger = makeLedger({makeClip(QUuid::createUuid())});
    QJsonObject exact = ledger.toJsonObject();
    ASSERT_TRUE(ClipLedger::fromJson(exact).has_value());

    QJsonObject missing = exact;
    missing.remove(QStringLiteral("occurrenceOrdinalHighWater"));
    EXPECT_FALSE(ClipLedger::fromJson(missing).has_value());
    QJsonObject tooLow = exact;
    tooLow[QStringLiteral("occurrenceOrdinalHighWater")] = 0;
    EXPECT_FALSE(ClipLedger::fromJson(tooLow).has_value());
    QJsonObject extra = exact;
    extra[QStringLiteral("legacy")] = true;
    EXPECT_FALSE(ClipLedger::fromJson(extra).has_value());

    const QJsonObject emptyWithHistory{
        {QStringLiteral("occurrenceOrdinalHighWater"), 9},
        {QStringLiteral("rows"), QJsonArray{}}};
    const auto parsed = ClipLedger::fromJson(emptyWithHistory);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->items.empty());
    EXPECT_EQ(parsed->occurrenceOrdinalHighWater, 9u);
    EXPECT_EQ(parsed->toJsonObject(), emptyWithHistory);
}

TEST(TimelineLedger, MarkerValidationEnforcesIdentityKindsAndOrder)
{
    MarkerLedger valid{{
        makeMarker(MarkerKind::Entry, -10),
        makeMarker(MarkerKind::Custom, -10, QStringLiteral("Marker")),
        makeMarker(MarkerKind::Custom, 15, QStringLiteral("Marker")),
        makeMarker(MarkerKind::Exit, 20)
    }};
    EXPECT_TRUE(valid.validate());
    const auto parsed = MarkerLedger::fromJson(valid.toJson());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, valid);

    MarkerLedger duplicateKind = valid;
    duplicateKind.items.push_back(makeMarker(MarkerKind::Entry, 0));
    EXPECT_EQ(duplicateKind.validate().error, TimelineError::DuplicateMarkerKind);

    MarkerLedger reversed{{makeMarker(MarkerKind::Entry, 2), makeMarker(MarkerKind::Exit, 1)}};
    EXPECT_EQ(reversed.validate().error, TimelineError::MarkerOrder);

    MarkerLedger duplicateGuid{{makeMarker(MarkerKind::Custom, 0), makeMarker(MarkerKind::Custom, 1)}};
    duplicateGuid.items[1].markerGuid = duplicateGuid.items[0].markerGuid;
    EXPECT_EQ(duplicateGuid.validate().error, TimelineError::DuplicateGuid);

    MarkerLedger invalidKind{{makeMarker(static_cast<MarkerKind>(99), 0)}};
    EXPECT_EQ(invalidKind.validate().error, TimelineError::InvalidJson);

    QJsonObject missingName = makeMarker(MarkerKind::Custom, 0).toJson();
    missingName.remove(QStringLiteral("name"));
    EXPECT_FALSE(MarkerLedger::fromJson(QJsonArray{missingName}).has_value());
}

TEST(TimelineLedger, PieceTimeSpecJsonUsesExplicitDomainAndMapShapes)
{
    const PieceTimeSpec musical = PieceTimeSpec::musical(600000, 7, 8);
    const QJsonObject json = musical.toJson();
    EXPECT_EQ(json[QStringLiteral("domain")].toString(), QStringLiteral("Musical"));
    EXPECT_TRUE(json[QStringLiteral("tempoMap")].isArray());
    EXPECT_TRUE(json[QStringLiteral("meterMap")].isArray());
    const auto parsed = PieceTimeSpec::fromJson(json);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, musical);

    QJsonObject invalidAbsolute{{QStringLiteral("domain"),
                                 QStringLiteral("Absolute")}};
    invalidAbsolute[QStringLiteral("tempoMap")] = QJsonArray{};
    EXPECT_FALSE(PieceTimeSpec::fromJson(invalidAbsolute).has_value());
}
