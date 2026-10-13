#include "Core/AuthoringPolicy.h"
#include "Core/Components/VPiece.h"
#include "Core/Components/VSelection.h"
#include "Core/Components/VTrack.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectTypes.h"
#include "Core/Timeline/TimelineTypes.h"
#include "Core/TransitionFieldGating.h"
#include "Core/TransitionSemantics.h"

#include <gtest/gtest.h>

#include <QHash>

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

void adopt(ProjectItem& parent, const std::shared_ptr<ProjectItem>& child)
{
    child->parent = &parent;
    parent.children.push_back(child);
}

void setMusical(ProjectItem& piece)
{
    piece.comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
}

void addMarker(ProjectItem& piece, MarkerKind kind, int64_t position = 0)
{
    piece.comp<VPiece>()->markers.items.push_back(
        {QUuid::createUuid(), kind, {}, position});
}

TransitionRuleDef overrideRule(const QUuid& source, const QUuid& target)
{
    TransitionRuleDef rule = canonicalTransitionRule();
    rule.source = source;
    rule.target = target;
    return rule;
}

struct ClockFixture
{
    std::shared_ptr<ProjectItem> container;
    std::shared_ptr<ProjectItem> clockedChild;
    std::shared_ptr<ProjectItem> silentChild;
    std::shared_ptr<ProjectItem> clockedPiece;
    std::shared_ptr<ProjectItem> silentPiece;
    std::vector<std::shared_ptr<ProjectItem>> items;
    TimelineItemResolver resolve;
};

ClockFixture makeClockedChildFixture()
{
    ClockFixture fixture;
    fixture.container = makeItem(ProjectItem::Type::Container,
                                 QStringLiteral("Container"));
    fixture.clockedChild = makeItem(ProjectItem::Type::Piece,
                                    QStringLiteral("ClockedChild"));
    fixture.silentChild = makeItem(ProjectItem::Type::Piece,
                                   QStringLiteral("SilentChild"));
    fixture.clockedPiece = fixture.clockedChild;
    fixture.silentPiece = fixture.silentChild;
    setMusical(*fixture.clockedChild);
    adopt(*fixture.container, fixture.clockedChild);
    adopt(*fixture.container, fixture.silentChild);
    fixture.items = {fixture.container, fixture.clockedChild, fixture.silentChild};
    fixture.resolve = resolverFor(fixture.items);
    return fixture;
}

}

TEST(TransitionFieldGating, LegacyUpwardClockCannotEnableMusicalExitTiming)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto child = makeItem(ProjectItem::Type::Sound, QStringLiteral("Child"));
    auto piece = makeItem(ProjectItem::Type::Piece, QStringLiteral("Piece"));
    auto track = makeItem(ProjectItem::Type::Track, QStringLiteral("Track"));
    setMusical(*piece);
    addMarker(*piece, MarkerKind::Entry);
    addMarker(*piece, MarkerKind::Exit, 1920);
    adopt(*container, child);
    track->parent = piece.get();
    piece->children = {track};
    container->parent = track.get();
    const auto resolve = resolverFor({container, child, piece, track});

    const TransitionWatchAvailability watch = TS::watchAvailabilityFor(
        *container, std::nullopt, nullptr, resolve);
    EXPECT_EQ(watch.clock, FieldAvail::Available);
    EXPECT_EQ(watch.anyMarker, FieldAvail::Available);
    EXPECT_EQ(watch.exitMarker, FieldAvail::Available);

    const TransitionFieldGating gating = TS::gatingFor(
        *container, canonicalTransitionRule(), watch);
    EXPECT_EQ(gating.exit.nextBeat, FieldAvail::Unavailable);
    EXPECT_EQ(gating.exit.nextBar, FieldAvail::Unavailable);
    EXPECT_EQ(gating.exit.reasonNextBar, AuthoringPolicy::musicalTimeRestriction());
}

TEST(TransitionFieldGating, MixedLegacyClocksCannotEnableMusicalExitTiming)
{
    ClockFixture fixture = makeClockedChildFixture();
    const TransitionWatchAvailability watch = TS::watchAvailabilityFor(
        *fixture.container, std::nullopt, nullptr, fixture.resolve);
    EXPECT_EQ(watch.clock, FieldAvail::Uncertain);

    const TransitionFieldGating gating = TS::gatingFor(
        *fixture.container, canonicalTransitionRule(), watch);
    EXPECT_EQ(gating.exit.nextBeat, FieldAvail::Unavailable);
    EXPECT_EQ(gating.exit.nextBar, FieldAvail::Unavailable);
    EXPECT_EQ(gating.exit.reasonNextBar, AuthoringPolicy::musicalTimeRestriction());
}

TEST(TransitionFieldGating, AnySourceUnavailableWhenNoChildReachesClock)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto child = makeItem(ProjectItem::Type::Sound, QStringLiteral("Child"));
    adopt(*container, child);
    const auto resolve = resolverFor({container, child});

    const TransitionWatchAvailability watch = TS::watchAvailabilityFor(
        *container, std::nullopt, nullptr, resolve);
    EXPECT_EQ(watch.clock, FieldAvail::Unavailable);
    EXPECT_EQ(watch.anyMarker, FieldAvail::Unavailable);
    EXPECT_EQ(watch.exitMarker, FieldAvail::Unavailable);

    const TransitionFieldGating gating = TS::gatingFor(
        *container, canonicalTransitionRule(), watch);
    EXPECT_EQ(gating.exit.nextBeat, FieldAvail::Unavailable);
    EXPECT_EQ(gating.exit.nextBar, FieldAvail::Unavailable);
    EXPECT_EQ(gating.exit.nextMarker, FieldAvail::Unavailable);
    EXPECT_EQ(gating.exit.exitMarker, FieldAvail::Available);
    EXPECT_EQ(gating.exit.reasonNextBar, AuthoringPolicy::musicalTimeRestriction());
    EXPECT_TRUE(gating.exit.reasonNextMarker.contains(QStringLiteral("No marker")));
    EXPECT_TRUE(gating.exit.reasonExitMarker.contains(QStringLiteral("Natural End")));
}

TEST(TransitionFieldGating, AnySourceUncertainWhenNestedChildTrackReachesClock)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto clockedTrack = makeItem(ProjectItem::Type::Track,
                                 QStringLiteral("ClockedTrack"));
    auto silentSound = makeItem(ProjectItem::Type::Sound,
                                QStringLiteral("SilentSound"));
    auto clockedPiece = makeItem(ProjectItem::Type::Piece,
                                 QStringLiteral("ClockedPiece"));
    setMusical(*clockedPiece);
    adopt(*container, clockedTrack);
    adopt(*container, silentSound);
    clockedTrack->comp<VTrack>()->clips.items = {
        {QUuid::createUuid(), clockedPiece->guid, 0, 1}
    };
    const auto resolve = resolverFor(
        {container, clockedTrack, silentSound, clockedPiece});

    const TransitionWatchAvailability watch = TS::watchAvailabilityFor(
        *container, std::nullopt, nullptr, resolve);
    EXPECT_EQ(watch.clock, FieldAvail::Uncertain);
}

TEST(TransitionFieldGating, ConcreteSourceNeverProducesUncertain)
{
    ClockFixture fixture = makeClockedChildFixture();
    const TransitionWatchAvailability clocked = TS::watchAvailabilityFor(
        *fixture.container, fixture.clockedChild->guid, nullptr, fixture.resolve);
    EXPECT_EQ(clocked.clock, FieldAvail::Available);

    const TransitionWatchAvailability silent = TS::watchAvailabilityFor(
        *fixture.container, fixture.silentChild->guid, nullptr, fixture.resolve);
    EXPECT_EQ(silent.clock, FieldAvail::Unavailable);
    EXPECT_NE(silent.clock, FieldAvail::Uncertain);
}

TEST(TransitionFieldGating, EntryMarkerWithoutExitMarkerKeepsNextMarkerAndCoalescesExitMarker)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto source = makeItem(ProjectItem::Type::Piece, QStringLiteral("Source"));
    setMusical(*source);
    addMarker(*source, MarkerKind::Entry);
    adopt(*container, source);
    const auto resolve = resolverFor({container, source});
    const TransitionWatchAvailability watch = TS::watchAvailabilityFor(
        *container, source->guid, nullptr, resolve);
    EXPECT_EQ(watch.clock, FieldAvail::Available);
    EXPECT_EQ(watch.anyMarker, FieldAvail::Available);
    EXPECT_EQ(watch.exitMarker, FieldAvail::Unavailable);

    const TransitionFieldGating gating = TS::gatingFor(
        *container, overrideRule(source->guid, source->guid), watch);
    EXPECT_EQ(gating.exit.nextMarker, FieldAvail::Available);
    EXPECT_EQ(gating.exit.exitMarker, FieldAvail::Available);
    EXPECT_TRUE(gating.exit.reasonNextMarker.isEmpty());
    EXPECT_TRUE(gating.exit.reasonExitMarker.contains(QStringLiteral("Natural End")));
}

TEST(TransitionFieldGating, NoClockWithMarkerKeepsNextMarkerAndDisablesBeatBar)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto source = makeItem(ProjectItem::Type::Piece, QStringLiteral("Source"));
    addMarker(*source, MarkerKind::Entry);
    addMarker(*source, MarkerKind::Custom, 480);
    adopt(*container, source);
    const auto resolve = resolverFor({container, source});
    const TransitionWatchAvailability watch = TS::watchAvailabilityFor(
        *container, source->guid, nullptr, resolve);
    EXPECT_EQ(watch.clock, FieldAvail::Unavailable);
    EXPECT_EQ(watch.anyMarker, FieldAvail::Available);

    const TransitionFieldGating gating = TS::gatingFor(
        *container, overrideRule(source->guid, source->guid), watch);
    EXPECT_EQ(gating.exit.nextBeat, FieldAvail::Unavailable);
    EXPECT_EQ(gating.exit.nextBar, FieldAvail::Unavailable);
    EXPECT_EQ(gating.exit.nextMarker, FieldAvail::Available);
    EXPECT_EQ(gating.exit.exitMarker, FieldAvail::Available);
    EXPECT_EQ(gating.exit.reasonNextBar, AuthoringPolicy::musicalTimeRestriction());
    EXPECT_TRUE(gating.exit.reasonNextMarker.isEmpty());
}

TEST(TransitionFieldGating, ImmediateAndNaturalEndStayAvailableWithoutReference)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    const auto resolve = resolverFor({container});
    const TransitionWatchAvailability watch = TS::watchAvailabilityFor(
        *container, std::nullopt, nullptr, resolve);
    const TransitionFieldGating gating = TS::gatingFor(
        *container, canonicalTransitionRule(), watch);
    EXPECT_EQ(gating.exit.immediate, FieldAvail::Available);
    EXPECT_EQ(gating.exit.naturalEnd, FieldAvail::Available);
}

TEST(TransitionFieldGating, DefaultRuleEndpointsAreNotEditable)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    const TransitionFieldGating gating = TS::gatingFor(
        *container, canonicalTransitionRule(), {});
    EXPECT_FALSE(gating.sourceEndpointEditable);
    EXPECT_FALSE(gating.targetEndpointEditable);
}

TEST(TransitionFieldGating, OverrideRuleEndpointsStayEditable)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    const TransitionFieldGating gating = TS::gatingFor(
        *container, overrideRule(QUuid::createUuid(), QUuid::createUuid()), {});
    EXPECT_TRUE(gating.sourceEndpointEditable);
    EXPECT_TRUE(gating.targetEndpointEditable);
}

TEST(TransitionFieldGating, BridgeAndSamePositionAreMutuallyUnavailable)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    TransitionRuleDef withBridge = overrideRule(
        QUuid::createUuid(), QUuid::createUuid());
    withBridge.bridge = QUuid::createUuid();
    const TransitionFieldGating gatedByBridge = TS::gatingFor(
        *container, withBridge, {});
    EXPECT_EQ(gatedByBridge.bridge, FieldAvail::Available);
    EXPECT_EQ(gatedByBridge.samePosition, FieldAvail::Unavailable);
    EXPECT_TRUE(gatedByBridge.bridgeFadesLive);
    EXPECT_FALSE(gatedByBridge.samePositionReadoutVisible);
    EXPECT_TRUE(gatedByBridge.reasonSamePosition.contains(QStringLiteral("bridge")));

    TransitionRuleDef samePosition = overrideRule(
        QUuid::createUuid(), QUuid::createUuid());
    samePosition.entryMode = TransitionEntryMode::SamePosition;
    const TransitionFieldGating gatedBySame = TS::gatingFor(
        *container, samePosition, {});
    EXPECT_EQ(gatedBySame.bridge, FieldAvail::Unavailable);
    EXPECT_EQ(gatedBySame.samePosition, FieldAvail::Available);
    EXPECT_FALSE(gatedBySame.bridgeFadesLive);
    EXPECT_TRUE(gatedBySame.samePositionReadoutVisible);
    EXPECT_TRUE(gatedBySame.reasonBridge.contains(QStringLiteral("Same Position")));
}

TEST(TransitionFieldGating, SamePositionStaysAvailableWhenTargetLengthUnknown)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    TransitionRuleDef rule = overrideRule(
        QUuid::createUuid(), QUuid::createUuid());
    rule.entryMode = TransitionEntryMode::SamePosition;
    const TransitionFieldGating gating = TS::gatingFor(*container, rule, {});
    EXPECT_EQ(gating.samePosition, FieldAvail::Available);
    EXPECT_TRUE(gating.samePositionReadoutVisible);
}

TEST(TransitionFieldGating, BridgeCandidatePredicateMatchesContentFour)
{
    auto sound = makeItem(ProjectItem::Type::Sound, QStringLiteral("Sound"));
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    auto track = makeItem(ProjectItem::Type::Track, QStringLiteral("Track"));
    auto piece = makeItem(ProjectItem::Type::Piece, QStringLiteral("Piece"));
    auto folder = makeItem(ProjectItem::Type::None, QStringLiteral("Folder"));
    auto layer = makeItem(ProjectItem::Type::Layer, QStringLiteral("Layer"));
    auto bus = makeItem(ProjectItem::Type::Bus, QStringLiteral("Bus"));
    auto action = makeItem(ProjectItem::Type::Action, QStringLiteral("Action"));
    auto variable = makeItem(ProjectItem::Type::Variable,
                             QStringLiteral("Variable"));
    auto attenuation = makeItem(ProjectItem::Type::Attenuation,
                                QStringLiteral("Attenuation"));

    EXPECT_TRUE(TS::isBridgeCandidate(*sound));
    EXPECT_TRUE(TS::isBridgeCandidate(*container));
    EXPECT_TRUE(TS::isBridgeCandidate(*track));
    EXPECT_TRUE(TS::isBridgeCandidate(*piece));
    EXPECT_TRUE(ItemTypeCatalog::isContentObject(sound->type));
    EXPECT_FALSE(TS::isBridgeCandidate(*folder));
    EXPECT_FALSE(TS::isBridgeCandidate(*layer));
    EXPECT_FALSE(TS::isBridgeCandidate(*bus));
    EXPECT_FALSE(TS::isBridgeCandidate(*action));
    EXPECT_FALSE(TS::isBridgeCandidate(*variable));
    EXPECT_FALSE(TS::isBridgeCandidate(*attenuation));
}

TEST(TransitionFieldGating, GatingDoesNotRewriteRuleValues)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    TransitionRuleDef rule = canonicalTransitionRule();
    rule.exitTiming = TransitionExitTiming::NextBar;
    rule.entryMode = TransitionEntryMode::EntryMarker;
    const TransitionWatchAvailability watch = TS::watchAvailabilityFor(
        *container, std::nullopt, nullptr, resolverFor({container}));
    ASSERT_EQ(watch.clock, FieldAvail::Unavailable);
    const TransitionFieldGating gating = TS::gatingFor(*container, rule, watch);
    EXPECT_EQ(gating.exit.nextBar, FieldAvail::Unavailable);
    EXPECT_EQ(rule.exitTiming, TransitionExitTiming::NextBar);
}

TEST(TransitionFieldGating, MountStateDoesNotChangeGatingStructure)
{
    auto container = makeItem(ProjectItem::Type::Container,
                              QStringLiteral("Container"));
    VSelection* selection = container->comp<VSelection>();
    selection->mode = SelectionMode::Random;
    EXPECT_EQ(TS::transitionMountState(*container), TransitionMountState::None);
    const TransitionFieldGating noneGating = TS::gatingFor(
        *container, canonicalTransitionRule(), {});
    EXPECT_FALSE(noneGating.sourceEndpointEditable);

    selection->mode = SelectionMode::Random;
    selection->playType = SelectionPlayType::Queue;
    EXPECT_EQ(TS::transitionMountState(*container),
              TransitionMountState::Editable);
    const TransitionFieldGating editableGating = TS::gatingFor(
        *container, canonicalTransitionRule(), {});
    EXPECT_EQ(editableGating.sourceEndpointEditable,
              noneGating.sourceEndpointEditable);
    EXPECT_EQ(editableGating.exit.immediate, noneGating.exit.immediate);

    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Single;
    EXPECT_EQ(TS::transitionMountState(*container), TransitionMountState::None);
    const TransitionFieldGating sequenceGating = TS::gatingFor(
        *container, canonicalTransitionRule(), {});
    EXPECT_EQ(sequenceGating.exit.nextBar, noneGating.exit.nextBar);
    EXPECT_EQ(sequenceGating.exit.nextMarker, noneGating.exit.nextMarker);
}

TEST(TransitionFieldGating, NewRuleCombinationSkipsLegacyChildrenWithoutBlockingSupportedSiblings)
{
    auto container = makeItem(ProjectItem::Type::Container, QStringLiteral("Container"));
    auto legacy = makeItem(ProjectItem::Type::Piece, QStringLiteral("Legacy"));
    auto source = makeItem(ProjectItem::Type::Sound, QStringLiteral("Source"));
    auto target = makeItem(ProjectItem::Type::Piece, QStringLiteral("Target"));
    setMusical(*legacy);
    adopt(*container, legacy);
    adopt(*container, source);
    adopt(*container, target);
    const auto resolve = resolverFor({container, legacy, source, target});
    const auto table = AuthoringPolicy::defaultTransitionTable();
    const auto combination = TS::firstUnusedCombination(*container, table, resolve);
    ASSERT_TRUE(combination);
    EXPECT_EQ(combination->first, source->guid);
    EXPECT_EQ(combination->second, target->guid);
    EXPECT_EQ(TS::ledgerGatingFor(*container, table, nullptr, resolve).addRule,
              FieldAvail::Available);
}
