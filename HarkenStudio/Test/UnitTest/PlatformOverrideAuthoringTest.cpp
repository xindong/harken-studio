#include "Core/Components/VSelection.h"
#include "Support/FieldGroupTestUtils.h"
#include "Components/VMixNode.h"
#include "Components/VTrack.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"

#include <gtest/gtest.h>

#include <QJsonValue>
#include <QUuid>
#include "Support/ProjectPlatformTestUtils.h"

#include <array>

namespace {

using VNX::PlayablePropertyAuthoringV1;
using VNX::PlayablePropertyStateV1;
using VNX::Platform;
using VNX::ProjectItem;
using VNX::VMixNode;

constexpr uint8_t kVolumeId = 1;
constexpr uint8_t kLoopCountId = 4;

ProjectItem makeDefinition(ProjectItem::Type type)
{
    ProjectItem item(QStringLiteral("definition"), type);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    return item;
}

void expectState(const std::optional<PlayablePropertyStateV1>& state,
                 bool present,
                 double value)
{
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->present, present);
    EXPECT_DOUBLE_EQ(state->value.toDouble(), value);
}

TEST(PlatformOverrideAuthoring, EmptyLedgerMatchesSharedRead)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -6.0, std::nullopt));

    const auto shared = PlayablePropertyAuthoringV1::readProperty(
        sound, *locator, kVolumeId, std::nullopt);
    const auto android = PlayablePropertyAuthoringV1::readProperty(
        sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android);
    ASSERT_TRUE(shared.has_value());
    ASSERT_TRUE(android.has_value());
    EXPECT_EQ(*shared, *android);
    expectState(shared, true, -6.0);
}

TEST(PlatformOverrideAuthoring, SetOverrideWritesOnlyNamedPlatform)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));

    expectState(PlayablePropertyAuthoringV1::readProperty(
                    sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android),
                true, -12.0);
    expectState(PlayablePropertyAuthoringV1::readProperty(
                    sound, *locator, kVolumeId, std::nullopt),
                true, -6.0);
    expectState(PlayablePropertyAuthoringV1::readProperty(
                    sound, *locator, kVolumeId, HarkenTest::PlatformIds::Windows),
                true, -6.0);
    EXPECT_FLOAT_EQ(sound.comp<VMixNode>()->volume, -6.0f);
    EXPECT_TRUE(sound.playablePropertyPresence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::Volume));
}

TEST(PlatformOverrideAuthoring, ClearOverrideDeletesEmptyRow)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));
    ASSERT_TRUE(sound.platformOverrides.find(HarkenTest::PlatformIds::Android) != nullptr);

    ASSERT_TRUE(PlayablePropertyAuthoringV1::clearOverride(
        sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android));

    expectState(PlayablePropertyAuthoringV1::readProperty(
                    sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android),
                true, -6.0);
    expectState(PlayablePropertyAuthoringV1::readProperty(
                    sound, *locator, kVolumeId, std::nullopt),
                true, -6.0);
    EXPECT_EQ(sound.platformOverrides.find(HarkenTest::PlatformIds::Android), nullptr);
    EXPECT_TRUE(sound.platformOverrides.rows.empty());
    EXPECT_FLOAT_EQ(sound.comp<VMixNode>()->volume, -6.0f);
}

TEST(PlatformOverrideAuthoring, OverlayPresenceNotRowExistence)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, 2, 3.0, HarkenTest::PlatformIds::Android));

    const auto* row = sound.platformOverrides.find(HarkenTest::PlatformIds::Android);
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->properties.presence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::Volume));
    expectState(PlayablePropertyAuthoringV1::readProperty(
                    sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android),
                true, -6.0);
}

TEST(PlatformOverrideAuthoring, OverlayPresentMasksSharedAbsence)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    expectState(PlayablePropertyAuthoringV1::readProperty(
                    sound, *locator, kVolumeId, std::nullopt),
                false, 0.0);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Android));

    expectState(PlayablePropertyAuthoringV1::readProperty(
                    sound, *locator, kVolumeId, HarkenTest::PlatformIds::Android),
                true, -12.0);
    expectState(PlayablePropertyAuthoringV1::readProperty(
                    sound, *locator, kVolumeId, std::nullopt),
                false, 0.0);
    EXPECT_FALSE(sound.playablePropertyPresence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::Volume));
}

TEST(PlatformOverrideAuthoring, UniversalScopeIsRejected)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -6.0, std::nullopt));
    const auto before = PlayablePropertyAuthoringV1::readProperty(
        sound, *locator, kVolumeId, std::nullopt);
    ASSERT_TRUE(before.has_value());

    EXPECT_FALSE(PlayablePropertyAuthoringV1::readProperty(
                     sound, *locator, kVolumeId, HarkenTest::PlatformIds::Universal)
                     .has_value());
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        sound, *locator, kVolumeId, -12.0, HarkenTest::PlatformIds::Universal));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::clearOverride(
        sound, *locator, kVolumeId, HarkenTest::PlatformIds::Universal));

    const auto after = PlayablePropertyAuthoringV1::readProperty(
        sound, *locator, kVolumeId, std::nullopt);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*before, *after);
    EXPECT_TRUE(sound.platformOverrides.rows.empty());
    EXPECT_FLOAT_EQ(sound.comp<VMixNode>()->volume, -6.0f);
}

TEST(PlatformOverrideAuthoring, ContainerChildOccurrenceRejectsPlaybackPropertyEditing)
{
    ProjectItem container = makeDefinition(ProjectItem::Type::Container);
    const QUuid childGuid = QUuid::createUuid();
    container.comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater = 1;
    container.comp<VNX::VSelection>()->selections.shared.setOccurrenceRows({{childGuid, 0}});
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    const auto before = container.comp<VNX::VSelection>()->selections.shared;
    const auto locator = PlayablePropertyAuthoringV1::containerChildLocator(
        container, childGuid);
    ASSERT_TRUE(locator.has_value());
    for (const auto property : {Property::Volume, Property::Pitch}) {
        const auto id = static_cast<uint8_t>(property);
        EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
            container, *locator, id, -3.0, std::nullopt));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
            container, *locator, id, -9.0, HarkenTest::PlatformIds::Android));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::clearOverride(
            container, *locator, id, std::nullopt));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::clearOverride(
            container, *locator, id, HarkenTest::PlatformIds::Android));
        expectState(PlayablePropertyAuthoringV1::readProperty(
                        container, *locator, id, std::nullopt),
                    false, 0.0);
        expectState(PlayablePropertyAuthoringV1::readProperty(
                        container, *locator, id, HarkenTest::PlatformIds::Android),
                    false, 0.0);
        const auto state = PlayablePropertyAuthoringV1::readPlatformOverlay(
            container, *locator, id, HarkenTest::PlatformIds::Android);
        ASSERT_TRUE(state.has_value());
        EXPECT_FALSE(state->present);
    }
    EXPECT_EQ(container.comp<VNX::VSelection>()->selections.shared, before);
}

TEST(PlatformOverrideAuthoring, TrackClipOccurrenceRejectsEditingAndIgnoresLegacyValues)
{
    ProjectItem track = makeDefinition(ProjectItem::Type::Track);
    auto* timeline = track.comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = QUuid::createUuid();
    clip.start = 120;
    clip.duration = 480;
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    for (const auto id : {Property::Volume, Property::Pitch})
        clip.playableProperties.presence.Set(id);
    ASSERT_TRUE(clip.playableProperties.setValue(Property::Volume, -6.0f));
    ASSERT_TRUE(clip.playableProperties.setValue(Property::Pitch, 1200.0f));
    auto* overlay = clip.platformOverrides.ensure(HarkenTest::PlatformIds::Android);
    ASSERT_NE(overlay, nullptr);
    overlay->properties = clip.playableProperties;
    ASSERT_TRUE(overlay->properties.setValue(Property::Volume, -12.0f));
    ASSERT_TRUE(overlay->properties.setValue(Property::Pitch, -600.0f));
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);
    const auto before = timeline->clips;
    const auto locator = PlayablePropertyAuthoringV1::trackClipLocator(
        track, clip.clipGuid);
    ASSERT_TRUE(locator.has_value());
    for (const auto property : {Property::Volume, Property::Pitch}) {
        const auto id = static_cast<uint8_t>(property);
        EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
            track, *locator, id, -3.0, std::nullopt));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
            track, *locator, id, -9.0, HarkenTest::PlatformIds::Android));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::clearOverride(
            track, *locator, id, std::nullopt));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::clearOverride(
            track, *locator, id, HarkenTest::PlatformIds::Android));
        expectState(PlayablePropertyAuthoringV1::readProperty(
                        track, *locator, id, std::nullopt),
                    false, 0.0);
        expectState(PlayablePropertyAuthoringV1::readProperty(
                        track, *locator, id, HarkenTest::PlatformIds::Android),
                    false, 0.0);
        const auto state = PlayablePropertyAuthoringV1::readPlatformOverlay(
            track, *locator, id, HarkenTest::PlatformIds::Android);
        ASSERT_TRUE(state.has_value());
        EXPECT_FALSE(state->present);
    }
    EXPECT_EQ(timeline->clips, before);
}

TEST(PlatformOverrideAuthoring, RatingSourceFollowsOwnershipParentsAtRequestedPlatform)
{
    constexpr uint8_t kPanningId = 9;
    ProjectItem layer = makeDefinition(ProjectItem::Type::Layer);
    layer.fieldGroups = VNX::MakeRootFieldGroups({0, 100});
    ProjectItem container = makeDefinition(ProjectItem::Type::Container);
    ProjectItem child = makeDefinition(ProjectItem::Type::Sound);
    container.parent = &layer;
    child.parent = &container;
    container.comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater = 1;
    container.comp<VNX::VSelection>()->selections.shared.setOccurrenceRows({{child.guid, 0}});
    const auto occurrence = PlayablePropertyAuthoringV1::containerChildLocator(
        container, child.guid);
    ASSERT_TRUE(occurrence);
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        container, *occurrence, kPanningId, -0.03, std::nullopt));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        container, *occurrence, kPanningId, -0.13, HarkenTest::PlatformIds::Android));
    const auto rating = [](float panning) {
        VNX::SpatialRatingFieldGroup value;
        value.spatialPanning = panning;
        return value;
    };
    using Mode = VNX::FieldGroupMode;
    using Kind = harkenprojectschema::ContentKind;
    layer.fieldGroups.spatialRating.shared = {Mode::Local, rating(-0.01f)};
    container.fieldGroups.spatialRating.shared = {Mode::Local, rating(-0.02f)};
    child.fieldGroups.spatialRating.shared = {Mode::Local, rating(-0.04f)};
    const auto layerGuid = VNX::toFieldGroupGuid(layer.guid);
    const auto containerGuid = VNX::toFieldGroupGuid(container.guid);
    const auto childGuid = VNX::toFieldGroupGuid(child.guid);
    const std::array definitions{
        VNX::FieldGroupDefinitionView{layerGuid, Kind::Layer, {}, &layer.fieldGroups},
        VNX::FieldGroupDefinitionView{containerGuid, Kind::Container, layerGuid, &container.fieldGroups},
        VNX::FieldGroupDefinitionView{childGuid, Kind::Sound, containerGuid, &child.fieldGroups}};
    const auto resolve = [&](VNX::FieldGroupScope scope) {
        return VNX::FieldGroupResolver(definitions, {0, 100}).Resolve(
            childGuid, scope, VNX::FieldGroupId::SpatialRating);
    };

    auto shared = resolve(std::nullopt);
    auto android = resolve(HarkenTest::FieldGroupPlatformIds::Android);
    ASSERT_TRUE(shared.ok());
    ASSERT_TRUE(android.ok());
    EXPECT_EQ(shared.value->source.definition, childGuid);
    EXPECT_EQ(android.value->source.definition, childGuid);
    EXPECT_FALSE(shared.value->source.scope);
    EXPECT_FALSE(android.value->source.scope);
    EXPECT_FALSE(shared.value->effectiveInherit);
    EXPECT_EQ(android.value->requestedMode, Mode::UseShared);
    EXPECT_FLOAT_EQ(std::get<VNX::SpatialRatingFieldGroup>(android.value->payload).spatialPanning, -0.04f);

    child.fieldGroups.spatialRating.shared.mode = Mode::Inherit;
    child.fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {Mode::Local, rating(-0.14f)};
    shared = resolve(std::nullopt);
    android = resolve(HarkenTest::FieldGroupPlatformIds::Android);
    ASSERT_TRUE(shared.ok());
    ASSERT_TRUE(android.ok());
    EXPECT_EQ(shared.value->source.definition, containerGuid);
    EXPECT_EQ(android.value->source.definition, childGuid);
    EXPECT_EQ(android.value->source.scope, HarkenTest::FieldGroupPlatformIds::Android);
    EXPECT_TRUE(shared.value->effectiveInherit);
    EXPECT_FALSE(android.value->effectiveInherit);
    EXPECT_FLOAT_EQ(std::get<VNX::SpatialRatingFieldGroup>(shared.value->payload).spatialPanning, -0.02f);
    EXPECT_FLOAT_EQ(std::get<VNX::SpatialRatingFieldGroup>(android.value->payload).spatialPanning, -0.14f);

    child.fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android].mode = Mode::UseShared;
    container.fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {Mode::Local, rating(-0.12f)};
    shared = resolve(std::nullopt);
    android = resolve(HarkenTest::FieldGroupPlatformIds::Android);
    ASSERT_TRUE(shared.ok());
    ASSERT_TRUE(android.ok());
    EXPECT_EQ(shared.value->source.definition, containerGuid);
    EXPECT_EQ(android.value->source.definition, containerGuid);
    EXPECT_FALSE(shared.value->source.scope);
    EXPECT_EQ(android.value->source.scope, HarkenTest::FieldGroupPlatformIds::Android);
    EXPECT_FLOAT_EQ(std::get<VNX::SpatialRatingFieldGroup>(shared.value->payload).spatialPanning, -0.02f);
    EXPECT_FLOAT_EQ(std::get<VNX::SpatialRatingFieldGroup>(android.value->payload).spatialPanning, -0.12f);

    container.fieldGroups.spatialRating.shared.mode = Mode::Inherit;
    container.fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android].mode = Mode::Inherit;
    layer.fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {Mode::Local, rating(-0.11f)};
    shared = resolve(std::nullopt);
    android = resolve(HarkenTest::FieldGroupPlatformIds::Android);
    ASSERT_TRUE(shared.ok());
    ASSERT_TRUE(android.ok());
    EXPECT_EQ(shared.value->source.definition, layerGuid);
    EXPECT_EQ(android.value->source.definition, layerGuid);
    EXPECT_FALSE(shared.value->source.scope);
    EXPECT_EQ(android.value->source.scope, HarkenTest::FieldGroupPlatformIds::Android);
    EXPECT_TRUE(shared.value->effectiveInherit);
    EXPECT_TRUE(android.value->effectiveInherit);
    EXPECT_FLOAT_EQ(std::get<VNX::SpatialRatingFieldGroup>(shared.value->payload).spatialPanning, -0.01f);
    EXPECT_FLOAT_EQ(std::get<VNX::SpatialRatingFieldGroup>(android.value->payload).spatialPanning, -0.11f);
}

TEST(PlatformOverrideAuthoring, InapplicableDefinitionSlotRejectsOverlay)
{
    ProjectItem layer = makeDefinition(ProjectItem::Type::Layer);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(layer);
    ASSERT_TRUE(locator.has_value());
    EXPECT_FALSE(PlayablePropertyAuthoringV1::isSlotApplicable(
        layer.type, kLoopCountId));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        layer, *locator, kLoopCountId, QJsonValue(6), HarkenTest::PlatformIds::Android));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::readProperty(
                     layer, *locator, kLoopCountId, HarkenTest::PlatformIds::Android)
                     .has_value());
    EXPECT_TRUE(layer.platformOverrides.rows.empty());
}

}  // namespace
