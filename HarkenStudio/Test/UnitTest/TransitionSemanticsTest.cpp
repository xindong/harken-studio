#include "Core/Components/VPiece.h"
#include "Core/Components/VSelection.h"
#include "Core/Components/VTrack.h"
#include "Core/PlacementContext.h"
#include "Core/ProjectSerializer.h"
#include "Core/ProjectTypes.h"
#include "Core/Timeline/TimelineCoordinates.h"
#include "Core/TransitionSemantics.h"

#include <gtest/gtest.h>

#include <QHash>

#include <limits>
#include <memory>

using namespace VNX;
namespace TS = VNX::TransitionSemantics;

namespace {

std::shared_ptr<ProjectItem> makeItem(ProjectItem::Type type,
                                      const QString& name)
{
    auto item = std::make_shared<ProjectItem>(name, type);
    item->guid = QUuid::createUuid();
    item->attachDefaultComponents();
    return item;
}

TimelineItemResolver resolverFor(
    const std::vector<std::shared_ptr<ProjectItem>>& items)
{
    auto index = std::make_shared<QHash<QUuid, const ProjectItem*>>();
    for (const auto& item : items) index->insert(item->guid, item.get());
    return [index](const QUuid& guid) { return index->value(guid, nullptr); };
}

void setNominal(ProjectItem& piece, int64_t entry, int64_t exit)
{
    piece.comp<VPiece>()->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, {}, entry},
        {QUuid::createUuid(), MarkerKind::Exit, {}, exit}
    };
}

TransitionRuleDef ruleFor(std::optional<QUuid> source,
                          std::optional<QUuid> target)
{
    TransitionRuleDef rule = canonicalTransitionRule();
    rule.source = source;
    rule.target = target;
    return rule;
}

}

TEST(TransitionSemantics, CanonicalTableRoundtripsAndSelectionOmitsIt)
{
    const TransitionTableDef canonical = canonicalTransitionTable();
    ASSERT_TRUE(canonical.validate());
    ASSERT_EQ(canonical.rules.size(), 1u);
    const TransitionFadeDef& fade = canonical.rules.front().sourceFadeOut;
    EXPECT_FLOAT_EQ(fade.durationSeconds, 0.5f);
    ASSERT_EQ(fade.curve.size(), 2);
    ASSERT_EQ(fade.handles.size(), 1);
    EXPECT_FLOAT_EQ(fade.handles.front().h1x, 0.45f);
    EXPECT_FLOAT_EQ(fade.handles.front().h2x, 0.55f);

    const auto loaded = TransitionTableDef::fromJson(canonical.toJson());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(*loaded, canonical);

    VSelection selection;
    EXPECT_FALSE(selection.toJson().contains(QStringLiteral("transitions")));
}

TEST(TransitionSemantics, TableMutationsRejectInvariantViolationsAtomically)
{
    const QUuid source = QUuid::createUuid();
    const QUuid target = QUuid::createUuid();
    TransitionTableDef table = canonicalTransitionTable();
    ASSERT_TRUE(table.addRule(ruleFor(source, target)));
    const TransitionTableDef before = table;

    EXPECT_EQ(table.addRule(ruleFor(source, target)).error,
              TransitionError::DuplicateEndpoints);
    EXPECT_EQ(table, before);
    EXPECT_EQ(table.removeRule(0).error, TransitionError::DefaultRuleRemoval);
    EXPECT_EQ(table, before);

    TransitionRuleDef invalid = ruleFor(source, std::nullopt);
    invalid.bridge = QUuid::createUuid();
    invalid.entryMode = TransitionEntryMode::SamePosition;
    EXPECT_EQ(table.addRule(invalid).error,
              TransitionError::BridgeSamePosition);
    EXPECT_EQ(table, before);

    TransitionRuleDef movedDefault = table.rules.front();
    movedDefault.source = source;
    EXPECT_EQ(table.updateRule(0, movedDefault).error,
              TransitionError::DefaultEndpoints);
    EXPECT_EQ(table, before);
}

TEST(TransitionSemantics, FadeValidationRejectsBadValuesAndJsonNormalizesShape)
{
    TransitionTableDef table = canonicalTransitionTable();
    table.rules.front().sourceFadeOut.durationSeconds = -1.0f;
    EXPECT_EQ(table.validate().error, TransitionError::InvalidFade);
    table = canonicalTransitionTable();
    table.rules.front().sourceFadeOut.curve.front().value = 2.0f;
    EXPECT_EQ(table.validate().error, TransitionError::InvalidFade);

    QJsonObject json = canonicalTransitionTable().toJson();
    QJsonArray rules = json.value(QStringLiteral("rules")).toArray();
    QJsonObject rule = rules.first().toObject();
    QJsonObject fade = rule.value(QStringLiteral("sourceFadeOut")).toObject();
    fade[QStringLiteral("curve")] = QJsonArray{
        QJsonObject{{QStringLiteral("x"), 1.0}, {QStringLiteral("value"), 2.0}},
        QJsonObject{{QStringLiteral("x"), 0.0}, {QStringLiteral("value"), -1.0}}
    };
    rule[QStringLiteral("sourceFadeOut")] = fade;
    rules[0] = rule;
    json[QStringLiteral("rules")] = rules;
    const auto normalized = TransitionTableDef::fromJson(json);
    ASSERT_TRUE(normalized);
    const auto& points = normalized->rules.front().sourceFadeOut.curve;
    ASSERT_EQ(points.size(), 2);
    EXPECT_FLOAT_EQ(points.front().x, 0.0f);
    EXPECT_FLOAT_EQ(points.front().value, 0.0f);
    EXPECT_FLOAT_EQ(points.back().value, 1.0f);
}

TEST(TransitionSemantics, SpecificityDrivesBothDisplayAndMatching)
{
    const QUuid source = QUuid::createUuid();
    const QUuid target = QUuid::createUuid();
    TransitionTableDef table = canonicalTransitionTable();
    ASSERT_TRUE(table.addRule(ruleFor(std::nullopt, target)));
    ASSERT_TRUE(table.addRule(ruleFor(source, std::nullopt)));
    ASSERT_TRUE(table.addRule(ruleFor(source, target)));

    EXPECT_EQ(TS::rulesForDisplay(table), (std::vector<int>{3, 2, 1, 0}));
    int index = -1;
    EXPECT_EQ(TS::matchRule(table, source, target, &index), &table.rules[3]);
    EXPECT_EQ(index, 3);
    EXPECT_EQ(TS::matchRule(table, source, QUuid::createUuid(), &index),
              &table.rules[2]);
    EXPECT_EQ(TS::matchRule(table, QUuid::createUuid(), target, &index),
              &table.rules[1]);
    EXPECT_EQ(TS::matchRule(table, QUuid::createUuid(), QUuid::createUuid(),
                            &index),
              &table.rules[0]);
}

TEST(TransitionSemantics, EndpointAndBridgeCandidatesUseDomainCatalogs)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto child = makeItem(ProjectItem::Type::Sound, QStringLiteral("Child"));
    auto outsider = makeItem(ProjectItem::Type::Sound, QStringLiteral("Outsider"));
    auto action = makeItem(ProjectItem::Type::Action, QStringLiteral("Action"));
    child->parent = container.get();
    container->children = {child};

    EXPECT_TRUE(TS::isEndpointCandidate(*container, *child));
    EXPECT_FALSE(TS::isEndpointCandidate(*container, *outsider));
    EXPECT_TRUE(TS::isBridgeCandidate(*outsider));
    EXPECT_FALSE(TS::isBridgeCandidate(*action));
}

TEST(TransitionSemantics, SamePositionMapsMusicalPhaseAndNegativePickup)
{
    auto source = makeItem(ProjectItem::Type::Piece, QStringLiteral("Source"));
    auto target = makeItem(ProjectItem::Type::Piece, QStringLiteral("Target"));
    source->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    target->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    setNominal(*source, -960, 7680);
    setNominal(*target, 480, 4320);
    const auto resolve = resolverFor({source, target});

    const TS::SamePositionResult wrapped = TS::samePositionMap(
        7200, *source, *target, resolve);
    ASSERT_TRUE(wrapped) << wrapped.validation.message.toStdString();
    EXPECT_EQ(wrapped.targetNominalLength, 3840);
    EXPECT_EQ(wrapped.targetOffset, 480);
    EXPECT_EQ(wrapped.targetPosition, 960);

    const TS::SamePositionResult pickup = TS::samePositionMap(
        -1440, *source, *target, resolve);
    ASSERT_TRUE(pickup);
    EXPECT_EQ(pickup.targetOffset, 3360);
    EXPECT_EQ(pickup.targetPosition, 3840);
}

TEST(TransitionSemantics, SamePositionConvertsAcrossDomainsAndRejectsUnknown)
{
    constexpr int64_t second = TimelineCoordinates::kNanosecondsPerSecond;
    auto source = makeItem(ProjectItem::Type::Piece, QStringLiteral("Source"));
    auto target = makeItem(ProjectItem::Type::Piece, QStringLiteral("Target"));
    target->comp<VPiece>()->pieceTime = PieceTimeSpec::musical(500000);
    setNominal(*source, 0, 10 * second);
    setNominal(*target, -960, 2880);
    const auto resolve = resolverFor({source, target});

    const TS::SamePositionResult result = TS::samePositionMap(
        7 * second, *source, *target, resolve);
    ASSERT_TRUE(result) << result.validation.message.toStdString();
    EXPECT_EQ(result.targetNominalLength, 3840);
    EXPECT_EQ(result.targetOffset, 1920);
    EXPECT_EQ(result.targetPosition, 960);

    setNominal(*target, 0, 0);
    EXPECT_EQ(TS::samePositionMap(0, *source, *target, resolve).validation.error,
              TransitionError::UnknownTimeline);
}

TEST(TransitionSemantics, SamePositionUsesUnifiedSoundNominalSpan)
{
    constexpr int64_t second = TimelineCoordinates::kNanosecondsPerSecond;
    auto source = makeItem(ProjectItem::Type::Piece, QStringLiteral("Source"));
    auto target = makeItem(ProjectItem::Type::Sound, QStringLiteral("Target"));
    setNominal(*source, 0, 10 * second);
    const auto result = TS::samePositionMap(
        7 * second, *source, *target, resolverFor({source, target}),
        [target](const ProjectItem& item) -> std::optional<int64_t> {
            return item.guid == target->guid
                ? std::optional<int64_t>{2 * TimelineCoordinates::kNanosecondsPerSecond}
                : std::nullopt;
        });
    ASSERT_TRUE(result) << result.validation.message.toStdString();
    EXPECT_EQ(result.targetNominalLength, 2 * second);
    EXPECT_EQ(result.targetOffset, second);
    EXPECT_EQ(result.targetPosition, second);
}

TEST(TransitionSemantics, SamePositionReportsCoordinateOverflow)
{
    auto source = makeItem(ProjectItem::Type::Piece, QStringLiteral("Source"));
    auto target = makeItem(ProjectItem::Type::Piece, QStringLiteral("Target"));
    setNominal(*source, std::numeric_limits<int64_t>::min(), 0);
    setNominal(*target, 0, 10);
    const auto result = TS::samePositionMap(
        std::numeric_limits<int64_t>::max(), *source, *target,
        resolverFor({source, target}));
    EXPECT_EQ(result.validation.error, TransitionError::CoordinateOverflow);
}

TEST(TransitionSemantics, WatchResolvesDownByStableClipOrder)
{
    auto source = makeItem(ProjectItem::Type::Track, QStringLiteral("Source"));
    auto first = makeItem(ProjectItem::Type::Piece, QStringLiteral("First"));
    auto second = makeItem(ProjectItem::Type::Piece, QStringLiteral("Second"));
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    source->comp<VTrack>()->clips.items = {
        {QUuid::createUuid(), first->guid, 10, 1},
        {QUuid::createUuid(), second->guid, 10, 1}
    };
    const TS::TransitionWatchResult result = TS::resolveWatchReference(
        source.get(), *container, nullptr,
        resolverFor({source, first, second, container}));
    ASSERT_TRUE(result);
    EXPECT_EQ(result.referencePiece, first.get());
    EXPECT_EQ(result.direction, TS::TransitionWatchDirection::Down);
    EXPECT_EQ(result.path, (std::vector<QUuid>{source->guid, first->guid}));
}

TEST(TransitionSemantics, WatchPrefersPlacementThenFallsBackToOrganization)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto organizationPiece = makeItem(ProjectItem::Type::Piece,
                                      QStringLiteral("Organization"));
    auto organizationTrack = makeItem(ProjectItem::Type::Track,
                                      QStringLiteral("OrganizationTrack"));
    organizationTrack->parent = organizationPiece.get();
    container->parent = organizationTrack.get();

    auto placementPiece = makeItem(ProjectItem::Type::Piece,
                                   QStringLiteral("Placement"));
    auto placementTrack = makeItem(ProjectItem::Type::Track,
                                   QStringLiteral("PlacementTrack"));
    placementTrack->parent = placementPiece.get();
    const Clip placementClip{QUuid::createUuid(), container->guid, 0, 1};
    placementTrack->comp<VTrack>()->clips.items = {placementClip};
    const auto resolve = resolverFor({container, organizationPiece,
                                      organizationTrack, placementPiece,
                                      placementTrack});
    const PlacementContext placement = PlacementContext::fromOccurrence(
        {placementTrack->guid, placementClip.clipGuid, container->guid});

    const auto placed = TS::resolveWatchReference(
        container.get(), *container, &placement, resolve);
    ASSERT_TRUE(placed) << placed.failureReason.toStdString();
    EXPECT_EQ(placed.referencePiece, placementPiece.get());
    EXPECT_EQ(placed.direction, TS::TransitionWatchDirection::UpPlacement);
    EXPECT_TRUE(placed.placementContextProvided);
    EXPECT_TRUE(placed.placementContextValid);

    const auto organized = TS::resolveWatchReference(
        container.get(), *container, nullptr, resolve);
    ASSERT_TRUE(organized);
    EXPECT_EQ(organized.referencePiece, organizationPiece.get());
    EXPECT_EQ(organized.direction,
              TS::TransitionWatchDirection::UpOrganization);
}

TEST(TransitionSemantics, WatchReportsNoneAndDefendsCycles)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto track = makeItem(ProjectItem::Type::Track, QStringLiteral("Track"));
    track->comp<VTrack>()->clips.items = {
        {QUuid::createUuid(), container->guid, 0, 1}
    };
    track->parent = container.get();
    container->children = {track};
    const auto cyclic = TS::resolveWatchReference(
        container.get(), *container, nullptr, resolverFor({container, track}));
    EXPECT_FALSE(cyclic);
    EXPECT_TRUE(cyclic.failureReason.contains(QStringLiteral("Cycle")));

    auto isolated = makeItem(ProjectItem::Type::Container,
                             QStringLiteral("Isolated"));
    const auto none = TS::resolveWatchReference(
        isolated.get(), *isolated, nullptr, resolverFor({isolated}));
    EXPECT_FALSE(none);
    EXPECT_EQ(none.direction, TS::TransitionWatchDirection::None);
    EXPECT_TRUE(none.failureReason.contains(QStringLiteral("No placement")));
}

TEST(TransitionSemantics, WatchViewReportsNoSourceWithoutClockLookup)
{
    auto container = makeItem(ProjectItem::Type::Container, QStringLiteral("Container"));
    const TS::TransitionWatchView view = TS::buildTransitionWatchView(
        *container, std::nullopt, nullptr, resolverFor({container}));
    EXPECT_FALSE(view.hasSource);
    EXPECT_FALSE(view.found());
    EXPECT_EQ(view.direction, TS::TransitionWatchDirection::None);
    EXPECT_EQ(view.containerGuid, container->guid);
    EXPECT_EQ(view.caption, QStringLiteral("no source — no clock lookup"));
}

TEST(TransitionSemantics, WatchViewCapturesDownPathAndNamesWithoutFallbackSuffix)
{
    auto source = makeItem(ProjectItem::Type::Track, QStringLiteral("Source"));
    auto first = makeItem(ProjectItem::Type::Piece, QStringLiteral("First"));
    auto second = makeItem(ProjectItem::Type::Piece, QStringLiteral("Second"));
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    source->comp<VTrack>()->clips.items = {
        {QUuid::createUuid(), first->guid, 10, 1},
        {QUuid::createUuid(), second->guid, 10, 1}
    };
    const auto resolve = resolverFor({source, first, second, container});

    const TS::TransitionWatchView view = TS::buildTransitionWatchView(
        *container, source->guid, nullptr, resolve);
    EXPECT_TRUE(view.hasSource);
    ASSERT_TRUE(view.found());
    EXPECT_EQ(view.direction, TS::TransitionWatchDirection::Down);
    EXPECT_EQ(*view.referencePiece, first->guid);
    EXPECT_EQ(view.path, (std::vector<QUuid>{source->guid, first->guid}));
    EXPECT_EQ(view.caption, QStringLiteral("Source -> First"));
}

TEST(TransitionSemantics, WatchViewCapturesUpPlacementCaptionWithNoFallbackSuffix)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto placementPiece = makeItem(ProjectItem::Type::Piece,
                                   QStringLiteral("Placement"));
    auto placementTrack = makeItem(ProjectItem::Type::Track,
                                   QStringLiteral("PlacementTrack"));
    placementTrack->parent = placementPiece.get();
    const Clip placementClip{QUuid::createUuid(), container->guid, 0, 1};
    placementTrack->comp<VTrack>()->clips.items = {placementClip};
    const auto resolve = resolverFor({container, placementPiece, placementTrack});
    const PlacementContext placement = PlacementContext::fromOccurrence(
        {placementTrack->guid, placementClip.clipGuid, container->guid});

    const TS::TransitionWatchView view = TS::buildTransitionWatchView(
        *container, container->guid, &placement, resolve);
    EXPECT_TRUE(view.hasSource);
    ASSERT_TRUE(view.found());
    EXPECT_EQ(view.direction, TS::TransitionWatchDirection::UpPlacement);
    EXPECT_EQ(*view.referencePiece, placementPiece->guid);
    EXPECT_TRUE(view.placementContextProvided);
    EXPECT_TRUE(view.placementContextValid);
    EXPECT_EQ(view.caption, QStringLiteral("Container -> PlacementTrack -> Placement"));
}

TEST(TransitionSemantics, WatchViewAnnotatesOrganizationFallbackWhenNoPlacementProvided)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto organizationPiece = makeItem(ProjectItem::Type::Piece,
                                      QStringLiteral("Organization"));
    auto organizationTrack = makeItem(ProjectItem::Type::Track,
                                      QStringLiteral("OrganizationTrack"));
    organizationTrack->parent = organizationPiece.get();
    container->parent = organizationTrack.get();
    const auto resolve = resolverFor({container, organizationPiece, organizationTrack});

    const TS::TransitionWatchView view = TS::buildTransitionWatchView(
        *container, container->guid, nullptr, resolve);
    ASSERT_TRUE(view.found());
    EXPECT_EQ(view.direction, TS::TransitionWatchDirection::UpOrganization);
    EXPECT_EQ(*view.referencePiece, organizationPiece->guid);
    EXPECT_FALSE(view.placementContextProvided);
    EXPECT_EQ(view.caption,
              QStringLiteral("Container -> OrganizationTrack -> Organization"
                             " (no placement context; organization scope only)"));
}

TEST(TransitionSemantics, WatchViewAnnotatesOrganizationFallbackWhenPlacementInvalid)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto organizationPiece = makeItem(ProjectItem::Type::Piece,
                                      QStringLiteral("Organization"));
    auto organizationTrack = makeItem(ProjectItem::Type::Track,
                                      QStringLiteral("OrganizationTrack"));
    organizationTrack->parent = organizationPiece.get();
    container->parent = organizationTrack.get();

    auto placementPiece = makeItem(ProjectItem::Type::Piece,
                                   QStringLiteral("Placement"));
    auto placementTrack = makeItem(ProjectItem::Type::Track,
                                   QStringLiteral("PlacementTrack"));
    placementTrack->parent = placementPiece.get();
    const Clip placementClip{QUuid::createUuid(), container->guid, 0, 1};
    placementTrack->comp<VTrack>()->clips.items = {placementClip};
    const auto resolve = resolverFor({container, organizationPiece, organizationTrack,
                                      placementPiece, placementTrack});
    // targetGuid deliberately does not match the clip's actual target, so
    // materializePlacement() must reject this occurrence and the resolver
    // must fall back to the organization chain instead of silently using it.
    const PlacementContext invalidPlacement = PlacementContext::fromOccurrence(
        {placementTrack->guid, placementClip.clipGuid, QUuid::createUuid()});

    const TS::TransitionWatchView view = TS::buildTransitionWatchView(
        *container, container->guid, &invalidPlacement, resolve);
    ASSERT_TRUE(view.found());
    EXPECT_EQ(view.direction, TS::TransitionWatchDirection::UpOrganization);
    EXPECT_EQ(*view.referencePiece, organizationPiece->guid);
    EXPECT_TRUE(view.placementContextProvided);
    EXPECT_FALSE(view.placementContextValid);
    EXPECT_EQ(view.caption,
              QStringLiteral("Container -> OrganizationTrack -> Organization"
                             " (placement context invalid: Placement Clip target no longer"
                             " matches the context.; fell back to organization scope)"));
}

TEST(TransitionSemantics, WatchViewReportsNoReferenceWithFailureReason)
{
    auto isolated = makeItem(ProjectItem::Type::Container, QStringLiteral("Isolated"));
    const TS::TransitionWatchView view = TS::buildTransitionWatchView(
        *isolated, isolated->guid, nullptr, resolverFor({isolated}));
    EXPECT_TRUE(view.hasSource);
    EXPECT_FALSE(view.found());
    EXPECT_EQ(view.direction, TS::TransitionWatchDirection::None);
    EXPECT_TRUE(view.failureReason.contains(QStringLiteral("No placement context")));
    EXPECT_EQ(view.caption,
              QStringLiteral("no reference — immediate + fade"
                             " (no placement context; organization scope only)"));
}

TEST(TransitionSemantics, HandoverDistinguishesAllRehearsalFacts)
{
    const QUuid source = QUuid::createUuid();
    const QUuid target = QUuid::createUuid();
    const QUuid bridge = QUuid::createUuid();
    TransitionTableDef table = canonicalTransitionTable();
    TransitionRuleDef exact = ruleFor(source, target);
    exact.bridge = bridge;
    ASSERT_TRUE(table.addRule(exact));
    ASSERT_TRUE(table.addRule(ruleFor(source, std::nullopt)));

    const auto transition = TS::deriveHandover(table, source, target);
    EXPECT_EQ(transition.kind, TS::HandoverKind::Transition);
    EXPECT_EQ(transition.referenceChain,
              (std::vector<QUuid>{source, bridge, target}));
    EXPECT_EQ(transition.ruleChain, (std::vector<QUuid>{source, target}));
    EXPECT_EQ(TS::deriveHandover(table, source, std::nullopt).kind,
              TS::HandoverKind::SourceExit);
    EXPECT_EQ(TS::deriveHandover(table, std::nullopt, target).kind,
              TS::HandoverKind::SilentEntry);
    EXPECT_EQ(TS::deriveHandover(table, source, source).kind,
              TS::HandoverKind::SameObject);
}

TEST(TransitionSemantics, SelectionJsonRejectsInvalidTableAtomicallyAndClones)
{
    const QUuid source = QUuid::createUuid();
    const QUuid target = QUuid::createUuid();
    VSelection selection;
    selection.mode = SelectionMode::Sequence;
    selection.playType = SelectionPlayType::Queue;
    ASSERT_TRUE(selection.transitions.addRule(ruleFor(source, target)));
    const QJsonObject before = selection.toJson();

    QJsonObject invalid = before;
    QJsonObject transitions = invalid.value(QStringLiteral("transitions")).toObject();
    QJsonArray rules = transitions.value(QStringLiteral("rules")).toArray();
    rules.append(rules.last());
    transitions[QStringLiteral("rules")] = rules;
    invalid[QStringLiteral("transitions")] = transitions;
    invalid[QStringLiteral("mode")] = static_cast<int>(SelectionMode::Random);
    EXPECT_FALSE(selection.fromJson(invalid));
    EXPECT_EQ(selection.toJson(), before);

    ProjectItem original(QStringLiteral("Container"), ProjectItem::Type::Container);
    original.guid = QUuid::createUuid();
    original.attachDefaultComponents();
    ASSERT_TRUE(original.comp<VSelection>()->fromJson(before));
    ProjectSerializer serializer;
    ProjectItem clone;
    ASSERT_TRUE(serializer.projectItemFromJson(
        serializer.projectItemToJson(original), clone));
    ASSERT_NE(clone.comp<VSelection>(), nullptr);
    EXPECT_EQ(clone.comp<VSelection>()->transitions,
              original.comp<VSelection>()->transitions);
}
