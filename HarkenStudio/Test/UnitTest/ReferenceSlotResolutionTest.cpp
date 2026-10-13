#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/FieldGroupTestUtils.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Core/ReferenceSlotResolution.h"
#include "Core/PlayablePropertyAuthoringV1.h"

#include "Core/Components/VMixNode.h"
#include "Core/Components/VSpatial.h"
#include "Core/Components/VTrack.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

namespace Slots = VNX::ReferenceSlotResolution;
using VNX::ProjectItem;

namespace {

// self > organizational chain: grandparent (Layer) > parent (Layer) > leaf (Sound).
struct Chain {
    ProjectItem grandparent{ QStringLiteral("Outer"), ProjectItem::Type::Layer };
    ProjectItem parent{ QStringLiteral("Inner"), ProjectItem::Type::Layer };
    ProjectItem leaf{ QStringLiteral("Shot"), ProjectItem::Type::Sound };

    Chain()
    {
        parent.parent = &grandparent;
        leaf.parent = &parent;
    }
};

Slots::PlacementItemResolver resolverFor(
    const std::vector<const ProjectItem*>& items)
{
    return [items](const QUuid& guid) -> const ProjectItem* {
        for (const ProjectItem* item : items) {
            if (item && item->guid == guid) return item;
        }
        return nullptr;
    };
}

void assignIdentity(ProjectItem& item)
{
    item.guid = QUuid::createUuid();
}

VNX::PlacementOccurrence place(ProjectItem& track, ProjectItem& target)
{
    assignIdentity(track);
    if (target.guid.isNull()) assignIdentity(target);
    VNX::Clip clip{QUuid::createUuid(), target.guid, 0, 1};
    track.compOrCreate<VNX::VTrack>()->clips.items.push_back(clip);
    return {track.guid, clip.clipGuid, target.guid};
}

}

namespace {

struct GroupProject {
    VNX::ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    uint32_t nextId = 1;
    ProjectItem* master = nullptr;

    GroupProject() { master = create(QStringLiteral("Master"), ProjectItem::Type::Bus); }

    ProjectItem* create(const QString& name, ProjectItem::Type type, ProjectItem* parent = nullptr)
    {
        auto value = std::make_shared<ProjectItem>(name, type);
        value->guid = QUuid::createUuid();
        value->objectID = nextId++;
        value->guid = VNX::authoringUuid(harkentest::StudioObjectId(value->objectID));
        value->attachDefaultComponents();
        if (!parent && VNX::PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(type))
            value->fieldGroups = VNX::MakeRootFieldGroups(VNX::toFieldGroupGuid(master->guid));
        auto* item = value.get();
        if (parent) {
            item->parent = parent;
            item->parentRef = {type == ProjectItem::Type::Bus
                ? VNX::ParentRef::Kind::Bus : VNX::ParentRef::Kind::Item, parent->guid};
            parent->children.push_back(std::move(value));
        } else {
            auto* bucket = type == ProjectItem::Type::Bus ? &project.busses
                : type == ProjectItem::Type::Attenuation ? &project.attenuationItems : &project.layers;
            bucket->push_back({std::move(value), {}});
        }
        return item;
    }

    Slots::ResolutionScope scope(ProjectItem* item, VNX::FieldGroupScope platform = {})
    {
        return Slots::buildResolutionScope(project, item, platform);
    }

    void output(ProjectItem* item, ProjectItem* target)
    {
        item->fieldGroups.mainOutput.shared = {VNX::FieldGroupMode::Local,
            VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(target->guid), -3.0f}};
    }

    void attenuation(ProjectItem* item, ProjectItem* target)
    {
        VNX::SpatialRatingFieldGroup rating;
        if (target) rating.spatialAttenuationRef = VNX::toFieldGroupGuid(target->guid);
        item->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Local, rating};
    }
};

}

TEST(ReferenceSlotResolution, GroupsUseRootCurrentValuesWithTheRetainedInheritanceFlag)
{
    GroupProject fixture;
    auto* sound = fixture.create(QStringLiteral("Sound"), ProjectItem::Type::Sound);
    const auto scope = fixture.scope(sound);
    const auto output = Slots::resolveOutputBusSlot(scope);
    EXPECT_TRUE(output.valid) << output.error.toStdString();
    EXPECT_EQ(output.owner, sound);
    EXPECT_EQ(output.source, Slots::ResolutionSource::Self);
    EXPECT_FALSE(output.authoringScope.has_value());
    EXPECT_EQ(output.target, fixture.master);
    EXPECT_EQ(Slots::resolveEffectiveOutputBusId(scope), fixture.master->objectID);
    const auto attenuation = Slots::resolveEffectiveAttenuation(scope);
    EXPECT_TRUE(attenuation.valid) << attenuation.error.toStdString();
    EXPECT_EQ(attenuation.owner, sound);
    EXPECT_EQ(attenuation.source, Slots::ResolutionSource::Self);
    EXPECT_EQ(attenuation.ref, harkentest::StudioObjectId(0u));
    const auto sends = Slots::resolveEffectiveSends(scope);
    EXPECT_TRUE(sends.valid) << sends.error.toStdString();
    EXPECT_EQ(sends.owner, sound);
    EXPECT_EQ(sends.source, Slots::ResolutionSource::Self);
    EXPECT_TRUE(sends.sends.empty());
    EXPECT_EQ(Slots::resolveEffectiveOutputBusId(nullptr), VNX::kMasterBusID);

    auto* bus = fixture.create(QStringLiteral("Output"), ProjectItem::Type::Bus, fixture.master);
    auto* sendBus = fixture.create(QStringLiteral("Send"), ProjectItem::Type::Bus, fixture.master);
    auto* distance = fixture.create(QStringLiteral("Distance"), ProjectItem::Type::Attenuation);
    auto& groups = sound->fieldGroups;
    groups.mainOutput.shared.localHistory->mainOutputBusRef = VNX::toFieldGroupGuid(bus->guid);
    groups.send.shared.localHistory->sends.push_back({VNX::toFieldGroupGuid(sendBus->guid), -6.0f});
    groups.spatialRating.shared.localHistory->spatialAttenuationRef = VNX::toFieldGroupGuid(distance->guid);
    EXPECT_EQ(groups.mainOutput.shared.mode, VNX::FieldGroupMode::Inherit);
    EXPECT_EQ(groups.send.shared.mode, VNX::FieldGroupMode::Inherit);
    EXPECT_EQ(groups.spatialRating.shared.mode, VNX::FieldGroupMode::Inherit);
    const auto currentScope = fixture.scope(sound);
    const auto currentOutput = Slots::resolveOutputBusSlot(currentScope);
    EXPECT_TRUE(currentOutput.valid) << currentOutput.error.toStdString();
    EXPECT_EQ(currentOutput.owner, sound);
    EXPECT_EQ(currentOutput.target, bus);
    EXPECT_EQ(Slots::resolveEffectiveOutputBusId(currentScope), bus->objectID);
    const auto currentAttenuation = Slots::resolveEffectiveAttenuation(currentScope);
    EXPECT_TRUE(currentAttenuation.valid) << currentAttenuation.error.toStdString();
    EXPECT_EQ(currentAttenuation.ref, distance->runtimeId());
    const auto currentSends = Slots::resolveEffectiveSends(currentScope);
    EXPECT_TRUE(currentSends.valid) << currentSends.error.toStdString();
    ASSERT_EQ(currentSends.sends.size(), 1u);
    EXPECT_EQ(currentSends.sends.front().targetBusID, sendBus->objectID);
    EXPECT_FLOAT_EQ(currentSends.sends.front().sendLevel, VNX::VMixNode::dbToLinear(-6.0f));
}

TEST(ReferenceSlotResolution, GroupsReportNearestWholeProviderAndExplicitEmptyValues)
{
    GroupProject fixture;
    auto* outer = fixture.create(QStringLiteral("Outer"), ProjectItem::Type::Layer);
    auto* inner = fixture.create(QStringLiteral("Inner"), ProjectItem::Type::Layer, outer);
    auto* sound = fixture.create(QStringLiteral("Sound"), ProjectItem::Type::Sound, inner);
    auto* bus = fixture.create(QStringLiteral("Output"), ProjectItem::Type::Bus, fixture.master);
    auto* attenuation = fixture.create(QStringLiteral("Distance"), ProjectItem::Type::Attenuation);
    fixture.output(outer, bus);
    fixture.attenuation(outer, attenuation);
    const auto inherited = fixture.scope(sound);
    EXPECT_EQ(Slots::resolveOutputBusSlot(inherited).owner, outer);
    EXPECT_EQ(Slots::resolveEffectiveOutputBusId(inherited), bus->objectID);
    EXPECT_EQ(Slots::resolveEffectiveAttenuation(inherited).owner, outer);
    EXPECT_EQ(Slots::resolveEffectiveAttenuation(inherited).ref, attenuation->runtimeId());
    fixture.output(inner, fixture.master);
    fixture.attenuation(inner, nullptr);
    const auto blocked = fixture.scope(sound);
    EXPECT_EQ(Slots::resolveOutputBusSlot(blocked).owner, inner);
    EXPECT_EQ(Slots::resolveEffectiveOutputBusId(blocked), fixture.master->objectID);
    EXPECT_EQ(Slots::resolveEffectiveAttenuation(blocked).owner, inner);
    EXPECT_EQ(Slots::resolveEffectiveAttenuation(blocked).ref, harkentest::StudioObjectId(0u));
    fixture.output(sound, bus);
    EXPECT_EQ(Slots::resolveOutputBusSlot(fixture.scope(sound)).source, Slots::ResolutionSource::Self);
}

TEST(ReferenceSlotResolution, PlatformUseSharedKeepsTheParentPlatformAndDormantHistoryIsIgnored)
{
    GroupProject fixture;
    auto* layer = fixture.create(QStringLiteral("Parent"), ProjectItem::Type::Layer);
    auto* sound = fixture.create(QStringLiteral("Sound"), ProjectItem::Type::Sound, layer);
    auto* bus = fixture.create(QStringLiteral("Android"), ProjectItem::Type::Bus, fixture.master);
    layer->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(bus->guid), -6.0f}};
    sound->fieldGroups.mainOutput.shared.localHistory = VNX::MainOutputFieldGroup{
        VNX::toFieldGroupGuid(QUuid::createUuid()), 0.0f};
    sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        VNX::FieldGroupMode::UseShared, {}};
    const auto scope = fixture.scope(sound, HarkenTest::FieldGroupPlatformIds::Android);
    const auto output = Slots::resolveOutputBusSlot(scope);
    EXPECT_TRUE(output.valid) << output.error.toStdString();
    EXPECT_EQ(output.owner, layer);
    EXPECT_EQ(output.authoringScope, HarkenTest::FieldGroupPlatformIds::Android);
    EXPECT_EQ(output.target, bus);
    EXPECT_EQ(Slots::resolveEffectiveOutputBusId(fixture.scope(sound)), fixture.master->objectID);
}

TEST(ReferenceSlotResolution, LocalEmptySendTableBlocksTheWholeInheritedTable)
{
    GroupProject fixture;
    auto* layer = fixture.create(QStringLiteral("Parent"), ProjectItem::Type::Layer);
    auto* sound = fixture.create(QStringLiteral("Sound"), ProjectItem::Type::Sound, layer);
    auto* bus = fixture.create(QStringLiteral("Send"), ProjectItem::Type::Bus, fixture.master);
    layer->fieldGroups.send.shared = {VNX::FieldGroupMode::Local,
        VNX::SendFieldGroup{{{VNX::toFieldGroupGuid(bus->guid), -6.0f}}}};
    const auto sends = Slots::resolveEffectiveSends(fixture.scope(sound));
    EXPECT_TRUE(sends.valid) << sends.error.toStdString();
    EXPECT_EQ(sends.owner, layer);
    ASSERT_EQ(sends.sends.size(), 1u);
    EXPECT_EQ(sends.sends.front().targetBusID, bus->objectID);
    EXPECT_FLOAT_EQ(sends.sends.front().sendLevel, VNX::VMixNode::dbToLinear(-6.0f));
    sound->fieldGroups.send.shared = {VNX::FieldGroupMode::Local, VNX::SendFieldGroup{}};
    const auto empty = Slots::resolveEffectiveSends(fixture.scope(sound));
    EXPECT_EQ(empty.owner, sound);
    EXPECT_TRUE(empty.sends.empty());
}

TEST(ReferenceSlotResolution, InvalidProjectDoesNotMasqueradeAsTheMasterDefault)
{
    GroupProject fixture;
    auto* sound = fixture.create(QStringLiteral("Sound"), ProjectItem::Type::Sound);
    fixture.output(sound, fixture.master);
    sound->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef =
        VNX::toFieldGroupGuid(QUuid::createUuid());
    const auto scope = fixture.scope(sound);
    EXPECT_FALSE(Slots::resolveOutputBusSlot(scope).valid);
    EXPECT_EQ(Slots::resolveEffectiveOutputBusId(scope), 0u);
}

TEST(ReferenceSlotResolution, NoBindingsProduceNoParameterContributions)
{
    Chain c;
    EXPECT_TRUE(Slots::resolveParameterBindings(
        &c.leaf, VNX::BindingSemantics::volumeAddress()).empty());
    EXPECT_TRUE(Slots::resolveParameterBindings(
        nullptr, VNX::BindingSemantics::volumeAddress()).empty());
}

TEST(ReferenceSlotResolution, ParameterBindingsCollectContributionsAlongTheSignalChain)
{
    Chain c;
    auto gpVolume = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), QUuid::createUuid());
    auto parentVolume = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), QUuid::createUuid());
    auto leafVolume = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), QUuid::createUuid());
    auto gpPitch = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), QUuid::createUuid());
    gpVolume.guid = VNX::authoringUuid(harkentest::StudioObjectId(1));
    parentVolume.guid = VNX::authoringUuid(harkentest::StudioObjectId(2));
    leafVolume.guid = VNX::authoringUuid(harkentest::StudioObjectId(3));
    gpPitch.guid = VNX::authoringUuid(harkentest::StudioObjectId(4));
    c.grandparent.compOrCreate<VNX::VMixNode>()->bindings.upsert(std::move(gpVolume));
    c.parent.compOrCreate<VNX::VMixNode>()->bindings.upsert(std::move(parentVolume));
    c.leaf.compOrCreate<VNX::VMixNode>()->bindings.upsert(std::move(leafVolume));
    c.grandparent.comp<VNX::VMixNode>()->bindings.upsert(std::move(gpPitch));

    const auto volume = Slots::resolveParameterBindings(
        &c.leaf, VNX::BindingSemantics::volumeAddress());
    ASSERT_EQ(volume.size(), 3u);
    EXPECT_EQ(volume[0].owner, &c.leaf);
    EXPECT_EQ(volume[1].owner, &c.parent);
    EXPECT_EQ(volume[2].owner, &c.grandparent);

    const auto pitch = Slots::resolveParameterBindings(
        &c.leaf, VNX::BindingSemantics::pitchAddress());
    ASSERT_EQ(pitch.size(), 1u);
    EXPECT_EQ(pitch[0].owner, &c.grandparent);

    c.leaf.comp<VNX::VMixNode>()->bindings.removeAll(
        VNX::BindingSemantics::volumeAddress());
    const auto remaining = Slots::resolveParameterBindings(
        &c.leaf, VNX::BindingSemantics::volumeAddress());
    ASSERT_EQ(remaining.size(), 2u);
    EXPECT_EQ(remaining[0].owner, &c.parent);
    EXPECT_EQ(remaining[1].owner, &c.grandparent);
}



TEST(ReferenceSlotResolution, NestedPlacementIsNearestFirst)
{
    ProjectItem sound{QStringLiteral("Sound"), ProjectItem::Type::Sound};
    ProjectItem innerPiece{QStringLiteral("InnerPiece"), ProjectItem::Type::Piece};
    ProjectItem innerTrack{QStringLiteral("InnerTrack"), ProjectItem::Type::Track};
    ProjectItem outerPiece{QStringLiteral("OuterPiece"), ProjectItem::Type::Piece};
    ProjectItem outerTrack{QStringLiteral("OuterTrack"), ProjectItem::Type::Track};
    assignIdentity(sound);
    assignIdentity(innerPiece);
    assignIdentity(outerPiece);
    innerTrack.parent = &innerPiece;
    outerTrack.parent = &outerPiece;
    const VNX::PlacementOccurrence inner = place(innerTrack, sound);
    const VNX::PlacementOccurrence outer = place(outerTrack, innerPiece);
    const VNX::PlacementContext context(
        std::vector<VNX::PlacementOccurrence>{inner, outer});

    const auto scope = Slots::buildResolutionScope(
        &sound, context,
        resolverFor({&sound, &innerPiece, &innerTrack, &outerPiece, &outerTrack}));

    ASSERT_TRUE(scope.placementContextValid);
    ASSERT_EQ(scope.placementChain.size(), 4u);
    EXPECT_EQ(scope.placementChain[0], &innerTrack);
    EXPECT_EQ(scope.placementChain[1], &innerPiece);
    EXPECT_EQ(scope.placementChain[2], &outerTrack);
    EXPECT_EQ(scope.placementChain[3], &outerPiece);
}

TEST(ReferenceSlotResolution, RepeatedObjectsAcrossChainsContributeOnce)
{
    ProjectItem piece{QStringLiteral("Piece"), ProjectItem::Type::Piece};
    ProjectItem track{QStringLiteral("Track"), ProjectItem::Type::Track};
    ProjectItem sound{QStringLiteral("Sound"), ProjectItem::Type::Sound};
    assignIdentity(piece);
    assignIdentity(sound);
    track.parent = &piece;
    sound.parent = &track;
    const VNX::PlacementOccurrence occurrence = place(track, sound);
    auto trackBinding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), QUuid::createUuid());
    auto pieceBinding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), QUuid::createUuid());
    trackBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(6));
    pieceBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(7));
    track.compOrCreate<VNX::VMixNode>()->bindings.upsert(std::move(trackBinding));
    piece.compOrCreate<VNX::VMixNode>()->bindings.upsert(std::move(pieceBinding));

    const auto scope = Slots::buildResolutionScope(
        &sound, VNX::PlacementContext::fromOccurrence(occurrence),
        resolverFor({&piece, &track, &sound}));
    EXPECT_TRUE(scope.organizationChain.empty());
    const auto bindings = Slots::resolveParameterBindings(
        scope, VNX::BindingSemantics::volumeAddress());
    ASSERT_EQ(bindings.size(), 2u);
    EXPECT_EQ(bindings[0].owner, &track);
    EXPECT_EQ(bindings[0].source, Slots::ResolutionSource::Placement);
    EXPECT_EQ(bindings[1].owner, &piece);
    EXPECT_EQ(bindings[1].source, Slots::ResolutionSource::Placement);
}



TEST(ReferenceSlotResolution, PlacementChangesBindingsButNeverGroupProviders)
{
    GroupProject fixture;
    auto* layer = fixture.create(QStringLiteral("Layer"), ProjectItem::Type::Layer);
    auto* sound = fixture.create(QStringLiteral("Sound"), ProjectItem::Type::Sound, layer);
    auto* piece = fixture.create(QStringLiteral("Piece"), ProjectItem::Type::Piece);
    auto* track = fixture.create(QStringLiteral("Track"), ProjectItem::Type::Track, piece);
    auto* ownedBus = fixture.create(QStringLiteral("Owned"), ProjectItem::Type::Bus, fixture.master);
    auto* placementBus = fixture.create(QStringLiteral("Placement"), ProjectItem::Type::Bus, fixture.master);
    auto* ownedAttenuation = fixture.create(QStringLiteral("OwnedDistance"), ProjectItem::Type::Attenuation);
    auto* placementAttenuation = fixture.create(QStringLiteral("PlacementDistance"), ProjectItem::Type::Attenuation);
    fixture.output(layer, ownedBus);
    fixture.output(track, placementBus);
    fixture.attenuation(layer, ownedAttenuation);
    fixture.attenuation(track, placementAttenuation);
    VNX::Clip clip{QUuid::createUuid(), sound->guid, 0, 1};
    track->comp<VNX::VTrack>()->clips.items.push_back(clip);
    VNX::PlacementOccurrence occurrence{track->guid, clip.clipGuid, sound->guid};
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), QUuid::createUuid());
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(50));
    track->comp<VNX::VMixNode>()->bindings.upsert(std::move(binding));
    const auto scope = Slots::buildResolutionScope(fixture.project, sound,
        VNX::PlacementContext::fromOccurrence(occurrence),
        resolverFor({layer, sound, piece, track}));
    ASSERT_TRUE(scope.placementContextValid);
    EXPECT_EQ(Slots::resolveOutputBusSlot(scope).owner, layer);
    EXPECT_EQ(Slots::resolveOutputBusSlot(scope).source, Slots::ResolutionSource::Organization);
    EXPECT_EQ(Slots::resolveEffectiveOutputBusId(scope), ownedBus->objectID);
    EXPECT_EQ(Slots::resolveEffectiveAttenuation(scope).owner, layer);
    EXPECT_EQ(Slots::resolveEffectiveAttenuation(scope).ref, ownedAttenuation->runtimeId());
    const auto bindings = Slots::resolveParameterBindings(scope, VNX::BindingSemantics::volumeAddress());
    ASSERT_EQ(bindings.size(), 1u);
    EXPECT_EQ(bindings.front().owner, track);
    EXPECT_EQ(bindings.front().source, Slots::ResolutionSource::Placement);
    track->comp<VNX::VTrack>()->clips.items.clear();
    const auto stale = Slots::buildResolutionScope(fixture.project, sound,
        VNX::PlacementContext::fromOccurrence(occurrence), resolverFor({layer, sound, piece, track}));
    EXPECT_FALSE(stale.placementContextValid);
    EXPECT_EQ(Slots::resolveOutputBusSlot(stale).owner, layer);
    EXPECT_TRUE(Slots::resolveParameterBindings(stale, VNX::BindingSemantics::volumeAddress()).empty());
}

TEST(ReferenceSlotResolution, AnOwningPieceRemainsAnOrganizationProviderInTrackContext)
{
    GroupProject fixture;
    auto* piece = fixture.create(QStringLiteral("Piece"), ProjectItem::Type::Piece);
    auto* track = fixture.create(QStringLiteral("Track"), ProjectItem::Type::Track, piece);
    fixture.output(piece, fixture.master);
    const auto context = Slots::buildResolutionScope(fixture.project, track,
        VNX::PlacementContext{}, resolverFor({piece, track}));
    EXPECT_EQ(Slots::resolveOutputBusSlot(context).owner, piece);
    EXPECT_EQ(Slots::resolveOutputBusSlot(context).source, Slots::ResolutionSource::Organization);
    EXPECT_EQ(Slots::resolveOutputBusSlot(fixture.scope(track)).source, Slots::ResolutionSource::Organization);
}

TEST(ReferenceSlotResolution, BusLocalRoutingAndSendValuesRemainUnchanged)
{
    GroupProject fixture;
    auto* bus = fixture.create(QStringLiteral("Bus"), ProjectItem::Type::Bus, fixture.master);
    bus->comp<VNX::VMixNode>()->outputBusId = fixture.master->objectID;
    bus->comp<VNX::VMixNode>()->sends.push_back({99, 0.25f});
    const auto scope = fixture.scope(bus);
    EXPECT_EQ(Slots::resolveOutputBusSlot(scope).owner, bus);
    EXPECT_EQ(Slots::resolveEffectiveOutputBusId(scope), fixture.master->objectID);
    const auto sends = Slots::resolveEffectiveSends(scope);
    ASSERT_EQ(sends.sends.size(), 1u);
    EXPECT_EQ(sends.sends.front().targetBusID, 99u);
    EXPECT_FLOAT_EQ(sends.sends.front().sendLevel, 0.25f);
}
