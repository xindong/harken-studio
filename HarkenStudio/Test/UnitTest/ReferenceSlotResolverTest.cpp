// Reference-slot resolution goldens.
//
// The rule under test is a precedence order, so most of these cases exist to
// pin a distinction that a passing "it resolved to something" assertion would
// hide: which level won when more than one had an opinion, whether "nearest"
// really means nearest, and what the answer is allowed to be when nothing was
// authored anywhere.
//
// Two cases here are deliberately awkward and must not be folded into their
// neighbours. One pins that the placement chain outranks the organizational
// chain even when both hold a value. The other pins that the guarantee about
// direct-trigger and placement agreeing applies only to an object that
// overrides the slot itself — without it, a later reader would take the
// agreement as universal and treat the normal disagreement as a defect.

#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_reference_slots/ReferenceSlotResolver.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

using harkenreferenceslots::kReferenceSlotCount;
using harkenreferenceslots::kResolutionLevelCount;
using harkenreferenceslots::ReferenceSlot;
using harkenreferenceslots::ReferenceSlotSource;
using harkenreferenceslots::ResolutionLevel;
using harkenreferenceslots::ResolutionOutcome;
using harkenreferenceslots::ResolveReferenceSlot;

namespace {

constexpr std::uint32_t kCeiling = 8;

// An authored world small enough to state a topology in a line or two. Chains
// are stored nearest-first, which is the order the callbacks hand them over.
struct Model {
    std::map<std::pair<harkenstudio::ObjectId, std::uint32_t>, harkenstudio::ObjectId> values;
    std::map<harkenstudio::ObjectId, std::vector<harkenstudio::ObjectId>> placement;
    std::map<harkenstudio::ObjectId, std::vector<harkenstudio::ObjectId>> organizational;
    bool hasDefault = false;
    harkenstudio::ObjectId defaultValue{};
    // Makes the placement chain answer forever, standing in for a source whose
    // ancestor walk cycles.
    bool endlessPlacement = false;

    void Author(harkenstudio::ObjectId objectId, ReferenceSlot slot,
                harkenstudio::ObjectId value)
    {
        values[{objectId, static_cast<std::uint32_t>(slot)}] = value;
    }
};

bool SlotValue(void* ctx, harkenstudio::ObjectId objectId, ReferenceSlot slot,
               harkenstudio::ObjectId* outValue)
{
    const Model* model = static_cast<const Model*>(ctx);
    const auto it =
        model->values.find({objectId, static_cast<std::uint32_t>(slot)});
    if (it == model->values.end()) {
        return false;
    }
    *outValue = it->second;
    return true;
}

bool PlacementAncestor(void* ctx, harkenstudio::ObjectId subjectId,
                       std::uint32_t index, harkenstudio::ObjectId* outObjectId)
{
    const Model* model = static_cast<const Model*>(ctx);
    if (model->endlessPlacement) {
        *outObjectId = subjectId; // never terminates on its own
        return true;
    }
    const auto it = model->placement.find(subjectId);
    if (it == model->placement.end() || index >= it->second.size()) {
        return false;
    }
    *outObjectId = it->second[index];
    return true;
}

bool OrganizationalAncestor(void* ctx, harkenstudio::ObjectId subjectId,
                            std::uint32_t index, harkenstudio::ObjectId* outObjectId)
{
    const Model* model = static_cast<const Model*>(ctx);
    const auto it = model->organizational.find(subjectId);
    if (it == model->organizational.end() || index >= it->second.size()) {
        return false;
    }
    *outObjectId = it->second[index];
    return true;
}

bool SlotDefault(void* ctx, ReferenceSlot, harkenstudio::ObjectId* outValue)
{
    const Model* model = static_cast<const Model*>(ctx);
    if (!model->hasDefault) {
        return false;
    }
    *outValue = model->defaultValue;
    return true;
}

ReferenceSlotSource SourceOver(Model& model)
{
    ReferenceSlotSource source;
    source.ctx = &model;
    source.slotValue = &SlotValue;
    source.placementAncestor = &PlacementAncestor;
    source.organizationalAncestor = &OrganizationalAncestor;
    source.slotDefault = &SlotDefault;
    return source;
}

// The candidate emitted for one level, or nullptr when that level had nothing
// to say.
const harkenreferenceslots::ReferenceSlotCandidate* CandidateAt(
    const harkenreferenceslots::ReferenceSlotResolution& resolution,
    ResolutionLevel level)
{
    for (const auto& candidate : resolution.candidates) {
        if (candidate.level == level) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Precedence
// ---------------------------------------------------------------------------

// Every level holds a value. The object's own override takes it, and the losers
// stay in the report so a tool can say what was shadowed rather than only what
// won.
TEST(ReferenceSlotResolution, ObjectOverrideOutranksEveryChainAndTheDefault)
{
    Model model;
    model.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10)};
    model.organizational[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(20)};
    model.hasDefault = true;
    model.defaultValue = harkentest::StudioObjectId(900);
    model.Author(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, harkentest::StudioObjectId(100));
    model.Author(harkentest::StudioObjectId(10), ReferenceSlot::OutputBus, harkentest::StudioObjectId(110));
    model.Author(harkentest::StudioObjectId(20), ReferenceSlot::OutputBus, harkentest::StudioObjectId(120));

    ReferenceSlotSource source = SourceOver(model);
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, kCeiling);

    EXPECT_EQ(resolution.outcome, ResolutionOutcome::Resolved);
    EXPECT_EQ(resolution.level, ResolutionLevel::ObjectOverride);
    EXPECT_EQ(resolution.value, harkentest::StudioObjectId(100u));
    EXPECT_EQ(resolution.sourceObjectId, harkentest::StudioObjectId(1u));
    EXPECT_EQ(resolution.ancestorDepth, 0u);

    ASSERT_EQ(resolution.candidates.size(), 4u);
    EXPECT_TRUE(resolution.candidates.front().winner);
    for (std::size_t i = 1; i < resolution.candidates.size(); ++i) {
        EXPECT_FALSE(resolution.candidates[i].winner);
    }
}

// Must not be merged with the case above. Both chains hold a value and the
// object holds none, which is the only shape that distinguishes "placement
// outranks organizational" from "whichever chain was consulted first".
TEST(ReferenceSlotResolution, PlacementChainOutranksOrganizationalChain)
{
    Model model;
    model.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10)};
    model.organizational[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(20)};
    model.Author(harkentest::StudioObjectId(10), ReferenceSlot::OutputBus, harkentest::StudioObjectId(110));
    model.Author(harkentest::StudioObjectId(20), ReferenceSlot::OutputBus, harkentest::StudioObjectId(120));

    ReferenceSlotSource source = SourceOver(model);
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, kCeiling);

    EXPECT_EQ(resolution.level, ResolutionLevel::PlacementAncestor);
    EXPECT_EQ(resolution.value, harkentest::StudioObjectId(110u));
    EXPECT_EQ(resolution.sourceObjectId, harkentest::StudioObjectId(10u));
    EXPECT_EQ(resolution.ancestorDepth, 1u);

    // The organizational value is reported as shadowed, not dropped: a tool has
    // to be able to explain why it did not win.
    const auto* shadowed =
        CandidateAt(resolution, ResolutionLevel::OrganizationalAncestor);
    ASSERT_NE(shadowed, nullptr);
    EXPECT_EQ(shadowed->value, harkentest::StudioObjectId(120u));
    EXPECT_FALSE(shadowed->winner);
}

// Must not be merged either. An unauthored ancestor between the subject and the
// authored one distinguishes "nearest authored value" from "first ancestor
// visited", and also proves an unset intermediate does not block the walk.
TEST(ReferenceSlotResolution, TakesTheNearestAuthoredAncestorNotTheFirstVisited)
{
    Model model;
    model.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10), harkentest::StudioObjectId(11), harkentest::StudioObjectId(12)};
    model.Author(harkentest::StudioObjectId(11), ReferenceSlot::OutputBus, harkentest::StudioObjectId(111));
    model.Author(harkentest::StudioObjectId(12), ReferenceSlot::OutputBus, harkentest::StudioObjectId(112));

    ReferenceSlotSource source = SourceOver(model);
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, kCeiling);

    EXPECT_EQ(resolution.level, ResolutionLevel::PlacementAncestor);
    EXPECT_EQ(resolution.value, harkentest::StudioObjectId(111u));
    EXPECT_EQ(resolution.ancestorDepth, 2u);
    // The whole chain was walked even though the answer came from depth 2, so
    // the reported depth describes the topology rather than the search.
    EXPECT_EQ(resolution.placementDepthWalked, 3u);
}

// With no placement chain at all — the shape a directly triggered object has —
// the organizational chain is what supplies the value.
TEST(ReferenceSlotResolution, FallsToTheOrganizationalChainWhenNothingIsPlaced)
{
    Model model;
    model.organizational[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(20), harkentest::StudioObjectId(21)};
    model.Author(harkentest::StudioObjectId(21), ReferenceSlot::OutputBus, harkentest::StudioObjectId(121));

    ReferenceSlotSource source = SourceOver(model);
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, kCeiling);

    EXPECT_EQ(resolution.level, ResolutionLevel::OrganizationalAncestor);
    EXPECT_EQ(resolution.ancestorDepth, 2u);
    EXPECT_EQ(resolution.placementDepthWalked, 0u);
    EXPECT_EQ(resolution.organizationalDepthWalked, 2u);
}

// ---------------------------------------------------------------------------
// The default is a level
// ---------------------------------------------------------------------------

// Nothing is authored anywhere, so the slot's own default answers — and it
// answers as a named level, attributable like any other.
TEST(ReferenceSlotResolution, NamesTheDefaultAsTheLevelThatSuppliedTheValue)
{
    Model model;
    model.hasDefault = true;
    model.defaultValue = harkentest::StudioObjectId(900);

    ReferenceSlotSource source = SourceOver(model);
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, kCeiling);

    EXPECT_EQ(resolution.outcome, ResolutionOutcome::Resolved);
    EXPECT_EQ(resolution.level, ResolutionLevel::SlotDefault);
    EXPECT_EQ(resolution.value, harkentest::StudioObjectId(900u));
    // Not authored on any object, and reported that way rather than as a
    // pseudo-object.
    EXPECT_EQ(resolution.sourceObjectId, harkentest::StudioObjectId(0u));
    ASSERT_EQ(resolution.candidates.size(), 1u);
    EXPECT_TRUE(resolution.candidates.front().winner);
}

// The default appears in the report even when it loses. A level visible only
// when it wins would read as an absence of a level, which is the shape an
// unnamed fallback hides in.
TEST(ReferenceSlotResolution, ReportsTheDefaultEvenWhenAnotherLevelWins)
{
    Model model;
    model.hasDefault = true;
    model.defaultValue = harkentest::StudioObjectId(900);
    model.Author(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, harkentest::StudioObjectId(100));

    ReferenceSlotSource source = SourceOver(model);
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, kCeiling);

    EXPECT_EQ(resolution.level, ResolutionLevel::ObjectOverride);
    const auto* fallback = CandidateAt(resolution, ResolutionLevel::SlotDefault);
    ASSERT_NE(fallback, nullptr);
    EXPECT_EQ(fallback->value, harkentest::StudioObjectId(900u));
    EXPECT_FALSE(fallback->winner);
}

// No level held a value and the slot has no default, so nothing is invented.
// Whether that refuses the build is the caller's decision, not the resolver's.
TEST(ReferenceSlotResolution, ResolvesToNothingRatherThanInventingAValue)
{
    Model model;
    ReferenceSlotSource source = SourceOver(model);
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, kCeiling);

    EXPECT_EQ(resolution.outcome, ResolutionOutcome::Unresolved);
    EXPECT_EQ(resolution.value, harkentest::StudioObjectId(0u));
    EXPECT_TRUE(resolution.candidates.empty());
}

// ---------------------------------------------------------------------------
// Direct trigger versus placement
// ---------------------------------------------------------------------------

// The guarantee: an object that sets its own output bus keeps it when placed on
// a timeline. Both runs are the same subject with the same override; only the
// placement chain differs, and the placed run's ancestors deliberately hold
// conflicting values.
TEST(ReferenceSlotResolution, OverridingObjectResolvesAlikeTriggeredOrPlaced)
{
    Model direct;
    direct.Author(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, harkentest::StudioObjectId(100));

    Model placed;
    placed.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10), harkentest::StudioObjectId(11)};
    placed.Author(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, harkentest::StudioObjectId(100));
    placed.Author(harkentest::StudioObjectId(10), ReferenceSlot::OutputBus, harkentest::StudioObjectId(110));
    placed.Author(harkentest::StudioObjectId(11), ReferenceSlot::OutputBus, harkentest::StudioObjectId(111));

    ReferenceSlotSource directSource = SourceOver(direct);
    ReferenceSlotSource placedSource = SourceOver(placed);
    const auto a =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, directSource, kCeiling);
    const auto b =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, placedSource, kCeiling);

    EXPECT_EQ(a.outcome, b.outcome);
    EXPECT_EQ(a.level, b.level);
    EXPECT_EQ(a.value, b.value);
    EXPECT_EQ(a.sourceObjectId, b.sourceObjectId);
    EXPECT_EQ(a.ancestorDepth, b.ancestorDepth);
    EXPECT_EQ(a.value, harkentest::StudioObjectId(100u));
}

// The counterpart, and the reason it exists: with no override on the object the
// two paths resolve differently, and that is correct rather than a defect. Read
// the case above without this one and the agreement looks universal, which
// would make somebody "fix" the normal behaviour.
TEST(ReferenceSlotResolution, WithoutAnOverrideTheTwoPathsMayDifferLegitimately)
{
    Model direct;
    direct.organizational[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(20)};
    direct.Author(harkentest::StudioObjectId(20), ReferenceSlot::OutputBus, harkentest::StudioObjectId(120));

    Model placed;
    placed.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10)};
    placed.organizational[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(20)};
    placed.Author(harkentest::StudioObjectId(10), ReferenceSlot::OutputBus, harkentest::StudioObjectId(110));
    placed.Author(harkentest::StudioObjectId(20), ReferenceSlot::OutputBus, harkentest::StudioObjectId(120));

    ReferenceSlotSource directSource = SourceOver(direct);
    ReferenceSlotSource placedSource = SourceOver(placed);
    const auto a =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, directSource, kCeiling);
    const auto b =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, placedSource, kCeiling);

    EXPECT_EQ(a.level, ResolutionLevel::OrganizationalAncestor);
    EXPECT_EQ(a.value, harkentest::StudioObjectId(120u));
    EXPECT_EQ(b.level, ResolutionLevel::PlacementAncestor);
    EXPECT_EQ(b.value, harkentest::StudioObjectId(110u));
    EXPECT_NE(a.value, b.value);
}

// ---------------------------------------------------------------------------
// The reference slots are one mechanism
// ---------------------------------------------------------------------------

// Same subject, same topology, both slots. Identical behaviour is the point:
// the slots must not drift into separate inheritance rules.
TEST(ReferenceSlotResolution, TreatsAllSlotsIdentically)
{
    const ReferenceSlot slots[] = {ReferenceSlot::OutputBus,
                                   ReferenceSlot::Attenuation};
    ASSERT_EQ(std::size(slots), kReferenceSlotCount);

    for (const ReferenceSlot slot : slots) {
        Model model;
        model.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10)};
        model.organizational[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(20)};
        model.Author(harkentest::StudioObjectId(10), slot, harkentest::StudioObjectId(110));
        model.Author(harkentest::StudioObjectId(20), slot, harkentest::StudioObjectId(120));

        ReferenceSlotSource source = SourceOver(model);
        const auto resolution =
            ResolveReferenceSlot(harkentest::StudioObjectId(1), slot, source, kCeiling);
        EXPECT_EQ(resolution.level, ResolutionLevel::PlacementAncestor);
        EXPECT_EQ(resolution.value, harkentest::StudioObjectId(110u));
        EXPECT_EQ(resolution.candidates.size(), 2u);
    }
}


// ---------------------------------------------------------------------------
// Both lineage depths
// ---------------------------------------------------------------------------

// A clip inside a Piece sees a two-deep placement chain; the same clip on a
// directly triggered Track sees a one-deep one. Both resolve, and each reports
// the depth it actually had.
TEST(ReferenceSlotResolution, HandlesBothLineageDepths)
{
    Model insidePiece;
    insidePiece.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10), harkentest::StudioObjectId(30)}; // Track, then Piece
    insidePiece.Author(harkentest::StudioObjectId(30), ReferenceSlot::OutputBus, harkentest::StudioObjectId(130));

    Model directTrack;
    directTrack.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10)}; // Track only
    directTrack.Author(harkentest::StudioObjectId(10), ReferenceSlot::OutputBus, harkentest::StudioObjectId(110));

    ReferenceSlotSource a = SourceOver(insidePiece);
    ReferenceSlotSource b = SourceOver(directTrack);
    const auto deep = ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, a, kCeiling);
    const auto shallow =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, b, kCeiling);

    EXPECT_EQ(deep.ancestorDepth, 2u);
    EXPECT_EQ(deep.value, harkentest::StudioObjectId(130u));
    EXPECT_EQ(shallow.ancestorDepth, 1u);
    EXPECT_EQ(shallow.value, harkentest::StudioObjectId(110u));
}

// Resolution keeps no state between calls, so alternating between the two
// depths cannot leak one topology's answer into the other. The resolver being a
// pure function makes this true today; the case is here so that adding a cache
// later has to prove it stays true.
TEST(ReferenceSlotResolution, AlternatingBetweenDepthsDoesNotCarryStateOver)
{
    Model insidePiece;
    insidePiece.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10), harkentest::StudioObjectId(30)};
    insidePiece.Author(harkentest::StudioObjectId(30), ReferenceSlot::OutputBus, harkentest::StudioObjectId(130));

    Model directTrack;
    directTrack.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10)};
    directTrack.Author(harkentest::StudioObjectId(10), ReferenceSlot::OutputBus, harkentest::StudioObjectId(110));

    ReferenceSlotSource a = SourceOver(insidePiece);
    ReferenceSlotSource b = SourceOver(directTrack);
    for (int round = 0; round < 3; ++round) {
        EXPECT_EQ(
            ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, a, kCeiling).value,
            harkentest::StudioObjectId(130u));
        EXPECT_EQ(
            ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, b, kCeiling).value,
            harkentest::StudioObjectId(110u));
    }
}

// ---------------------------------------------------------------------------
// Bounded and defined failure
// ---------------------------------------------------------------------------

// A chain whose last real ancestor lands exactly at the ceiling is complete.
// The terminating probe is not another ancestor and therefore must not turn the
// full walk into a depth refusal.
TEST(ReferenceSlotResolution, AcceptsAChainExactlyAtTheCeiling)
{
    Model model;
    model.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10), harkentest::StudioObjectId(11), harkentest::StudioObjectId(12)};
    model.Author(harkentest::StudioObjectId(12), ReferenceSlot::OutputBus, harkentest::StudioObjectId(112));

    ReferenceSlotSource source = SourceOver(model);
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, 3);

    EXPECT_EQ(resolution.outcome, ResolutionOutcome::Resolved);
    EXPECT_EQ(resolution.value, harkentest::StudioObjectId(112u));
    EXPECT_EQ(resolution.ancestorDepth, 3u);
    EXPECT_EQ(resolution.placementDepthWalked, 3u);
}

// An object override does not shorten explanation. Even at the exact ceiling,
// the complete chain is walked and its authored ancestor remains visible as a
// shadowed candidate.
TEST(ReferenceSlotResolution,
     ObjectOverrideStillExplainsAChainExactlyAtTheCeiling)
{
    Model model;
    model.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10), harkentest::StudioObjectId(11), harkentest::StudioObjectId(12)};
    model.Author(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, harkentest::StudioObjectId(100));
    model.Author(harkentest::StudioObjectId(12), ReferenceSlot::OutputBus, harkentest::StudioObjectId(112));

    ReferenceSlotSource source = SourceOver(model);
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, 3);

    EXPECT_EQ(resolution.outcome, ResolutionOutcome::Resolved);
    EXPECT_EQ(resolution.level, ResolutionLevel::ObjectOverride);
    EXPECT_EQ(resolution.value, harkentest::StudioObjectId(100u));
    EXPECT_EQ(resolution.placementDepthWalked, 3u);
    const auto* shadowed =
        CandidateAt(resolution, ResolutionLevel::PlacementAncestor);
    ASSERT_NE(shadowed, nullptr);
    EXPECT_EQ(shadowed->ancestorDepth, 3u);
    EXPECT_EQ(shadowed->value, harkentest::StudioObjectId(112u));
    EXPECT_FALSE(shadowed->winner);
}

// A chain deeper than the ceiling is refused rather than answered from the part
// that was walked: "no nearer value exists" would only be true within the walked
// depth, and a truncated answer would read as a whole one.
TEST(ReferenceSlotResolution, RefusesAChainDeeperThanTheCeiling)
{
    Model model;
    model.placement[harkentest::StudioObjectId(1)] = {harkentest::StudioObjectId(10), harkentest::StudioObjectId(11), harkentest::StudioObjectId(12), harkentest::StudioObjectId(13)};
    model.Author(harkentest::StudioObjectId(13), ReferenceSlot::OutputBus, harkentest::StudioObjectId(113));

    ReferenceSlotSource source = SourceOver(model);
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, 3);

    EXPECT_EQ(resolution.outcome, ResolutionOutcome::DepthCeilingExceeded);
    EXPECT_EQ(resolution.value, harkentest::StudioObjectId(0u));
}

// A source whose ancestor walk never terminates is the reason the ceiling is not
// optional: the library cannot inspect the chain it is handed, so the bound is
// what turns a malformed source into a refusal instead of a hang.
TEST(ReferenceSlotResolution, TerminatesOnASourceThatNeverEndsItsChain)
{
    Model model;
    model.endlessPlacement = true;

    ReferenceSlotSource source = SourceOver(model);
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, kCeiling);

    EXPECT_EQ(resolution.outcome, ResolutionOutcome::DepthCeilingExceeded);
}

// A source that answers nothing degrades to an unresolved slot instead of
// undefined behaviour, which is what makes a partially wired caller safe to
// diagnose.
TEST(ReferenceSlotResolution, TreatsNullCallbacksAsAnEmptyWorld)
{
    ReferenceSlotSource empty;
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, empty, kCeiling);

    EXPECT_EQ(resolution.outcome, ResolutionOutcome::Unresolved);
    EXPECT_EQ(resolution.placementDepthWalked, 0u);
    EXPECT_EQ(resolution.organizationalDepthWalked, 0u);
}

// ---------------------------------------------------------------------------
// The precedence vocabulary
// ---------------------------------------------------------------------------

// The pin that used to sit here compared the compile-side and runtime-side slot
// ordinals, because both were written by hand. Both are now emitted from one
// table, so comparing them to each other would be a tautology that can never
// fail and therefore never report anything. What replaced it is
// reference_slots_generated_check, which compares the checked-in headers to the
// table itself and so still catches a hand edit or an unregenerated change.
//
// The level vocabulary is what an explain row reports, so its size is part of
// the contract a consumer sizes storage against.
TEST(ReferenceSlotResolution, KeepsFourNamedPrecedenceLevels)
{
    EXPECT_EQ(kResolutionLevelCount, 4u);
    EXPECT_LT(static_cast<std::uint32_t>(ResolutionLevel::ObjectOverride),
              static_cast<std::uint32_t>(ResolutionLevel::PlacementAncestor));
    EXPECT_LT(static_cast<std::uint32_t>(ResolutionLevel::PlacementAncestor),
              static_cast<std::uint32_t>(ResolutionLevel::OrganizationalAncestor));
    EXPECT_LT(static_cast<std::uint32_t>(ResolutionLevel::OrganizationalAncestor),
              static_cast<std::uint32_t>(ResolutionLevel::SlotDefault));
    EXPECT_EQ(static_cast<std::uint32_t>(ResolutionLevel::SlotDefault),
              kResolutionLevelCount);
}
