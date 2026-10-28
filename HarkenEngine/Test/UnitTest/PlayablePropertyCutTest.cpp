#include "Common/ObjectIdTestSupport.h"
#include "harken/Harken.h"
#include "harken/core/PlayablePropertyCut.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>

namespace {

using PropertyId = harken::playableproperties::PlayablePropertyIdV1;

constexpr std::size_t Slot(PropertyId id) {
    return static_cast<std::size_t>(id) - 1;
}

void SetF32(harken::PlayablePropertyPayloadViewV1& payload,
            PropertyId id, float value) {
    payload.presentMask |= UINT64_C(1) << Slot(id);
    payload.values[Slot(id)].bits = std::bit_cast<std::uint32_t>(value);
}

void SetU32(harken::PlayablePropertyPayloadViewV1& payload,
            PropertyId id, std::uint32_t value) {
    payload.presentMask |= UINT64_C(1) << Slot(id);
    payload.values[Slot(id)].bits = value;
}

void SetU64(harken::PlayablePropertyPayloadViewV1& payload,
            PropertyId id, std::uint64_t value) {
    payload.presentMask |= UINT64_C(1) << Slot(id);
    payload.values[Slot(id)].objectId =
        id == PropertyId::MainOutputBusRef && value == 1
            ? VN_BUS_MASTER : harkentest::ObjectId(value);
}

harken::PlayablePropertyPayloadViewV1 ResolvedPayload() {
    harken::PlayablePropertyPayloadViewV1 payload{};
    payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    payload.values[3].bits = 1;
    payload.values[6].objectId = VN_BUS_MASTER;
    payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    return payload;
}

harken::PlayablePropertyPayloadViewV1 MixingContribution(
    const harken::PlayablePropertyPayloadViewV1& definition) {
    auto payload = definition;
    constexpr std::uint64_t localMask =
        harken::kResolvedSectionGroupPropertyMaskV1 |
        (UINT64_C(1) << 4) | (UINT64_C(1) << 15);
    payload.presentMask &= ~localMask;
    for (std::size_t index = 0; index < payload.values.size(); ++index)
        if ((localMask & (UINT64_C(1) << index)) != 0) {
            payload.values[index] = {};
            payload.values[index].bits =
                index == 11 || index == 16 ? 0x3f800000 : 0;
        }
    payload.sendCount = 0;
    payload.sends = nullptr;
    payload.effectNodeCount = 0;
    payload.effectNodes = nullptr;
    payload.effectEdgeCount = 0;
    payload.effectEdges = nullptr;
    return payload;
}

harken::PlayableRefV1 Ref(harken::PlayableKindV1 kind,
                         std::uint64_t id,
                         std::uint64_t fingerprint) {
    harken::PlayableRefV1 ref{};
    ref.kind = kind;
    ref.stableId = harkentest::ObjectId(id);
    ref.expectedProgramSemanticFingerprint = fingerprint;
    ref.contentRevision = id * 10;
    ref.routeStableId = harkentest::ObjectId(id * 100);
    ref.compatibilityFingerprint = id * 1000;
    return ref;
}

harken::PlayablePropertyProgramViewV1 Property(
    harken::playableproperties::ContentPropertyKindV1 kind,
    const harken::PlayableRefV1& ref) {
    harken::PlayablePropertyProgramViewV1 row{};
    row.payload = ResolvedPayload();
    row.contentPropertyKind = kind;
    row.stableId = ref.stableId;
    row.programSemanticFingerprint =
        ref.expectedProgramSemanticFingerprint;
    row.contentRevision = ref.contentRevision;
    row.routeStableId = ref.routeStableId;
    row.compatibilityFingerprint = ref.compatibilityFingerprint;
    return row;
}

struct WorldFixture {
    harken::PlayableRefV1 root = Ref(harken::PlayableKindV1::Container, 10, 110);
    harken::PlayableRefV1 nested = Ref(harken::PlayableKindV1::Container, 20, 120);
    harken::PlayableRefV1 leaf = Ref(harken::PlayableKindV1::Sound, 30, 130);
    std::array<harken::PlayablePropertyProgramViewV1, 6> properties{};
    std::array<harken::LayerAncestorRefV1, 2> rootLayers{{{harkentest::ObjectId(1), 101}, {harkentest::ObjectId(2), 102}}};
    std::array<harken::LayerAncestorRefV1, 1> nestedLayers{{{harkentest::ObjectId(3), 103}}};
    std::array<harken::PlayableOccurrencePropertyViewV1, 2> occurrences{};
    std::array<harken::ContainerChildProgramViewV2, 1> rootChildren{};
    std::array<harken::ContainerChildProgramViewV2, 1> nestedChildren{};
    std::array<harken::ContainerProgramViewV3, 2> containers{};
    std::array<harken::SendV1, 1> rootSends{{{0, 0, harkentest::ObjectId(10), -1.0f, 0}}};
    std::array<harken::SendV1, 2> leafSends{{
        {0, 0, harkentest::ObjectId(10), -3.0f, 0}, {1, 0, harkentest::ObjectId(20), -4.0f, 0}}};
    std::array<harken::EffectParamV1, 1> effectParams{{{0, 55}}};
    std::array<harken::EffectNodeV1, 1> effectNodes{{{8, 3, 1, effectParams.data()}}};
    harken::PlayableWorldViewV1 world{};

    WorldFixture() {
        for (auto& property : properties) property.payload = ResolvedPayload();
        properties[0].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Layer;
        properties[0].stableId = harkentest::ObjectId(1);
        properties[0].programSemanticFingerprint = 101;
        SetF32(properties[0].payload, PropertyId::Volume, 1.0f);
        SetU64(properties[0].payload, PropertyId::SpatialAttenuationRef, 400);

        properties[1].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Layer;
        properties[1].stableId = harkentest::ObjectId(2);
        properties[1].programSemanticFingerprint = 102;
        SetF32(properties[1].payload, PropertyId::Volume, 2.0f);
        SetU32(properties[1].payload, PropertyId::SpatialIs3DEnabled, 1);

        properties[2].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Layer;
        properties[2].stableId = harkentest::ObjectId(3);
        properties[2].programSemanticFingerprint = 103;
        SetF32(properties[2].payload, PropertyId::Volume, 100.0f);

        properties[3] = Property(
            harken::playableproperties::ContentPropertyKindV1::Container,
            root);
        properties[3].organizationLayerAncestorCount = 2;
        properties[3].organizationLayerAncestors = rootLayers.data();
        SetF32(properties[3].payload, PropertyId::Volume, 3.0f);
        SetF32(properties[3].payload, PropertyId::Pitch, 2000.0f);
        SetU64(properties[3].payload, PropertyId::MainOutputBusRef, 100);
        SetF32(properties[3].payload, PropertyId::SpatialPanning, 0.1f);
        properties[3].payload.presentMask |=
            UINT64_C(1) << Slot(PropertyId::Sends);
        properties[3].payload.sendCount = 1;
        properties[3].payload.sends = rootSends.data();

        properties[4] = Property(
            harken::playableproperties::ContentPropertyKindV1::Container,
            nested);
        properties[4].organizationLayerAncestorCount = 1;
        properties[4].organizationLayerAncestors = nestedLayers.data();
        SetF32(properties[4].payload, PropertyId::Volume, 5.0f);
        SetF32(properties[4].payload, PropertyId::MakeUpGain, 2.0f);
        properties[4].payload.presentMask |=
            UINT64_C(1) << Slot(PropertyId::SignalGraph);
        properties[4].payload.effectNodeCount = 1;
        properties[4].payload.effectNodes = effectNodes.data();

        properties[5] = Property(
            harken::playableproperties::ContentPropertyKindV1::Sound,
            leaf);
        SetF32(properties[5].payload, PropertyId::Volume, 7.0f);
        SetF32(properties[5].payload, PropertyId::Pitch, 6000.0f);
        SetF32(properties[5].payload, PropertyId::SpatialPanning, 0.0f);
        SetU32(properties[5].payload, PropertyId::StreamingMode, 1);
        SetU64(properties[5].payload, PropertyId::MainOutputBusRef, 200);

        occurrences[0].parentKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        occurrences[0].parentStableId = root.stableId;
        occurrences[0].parentProgramSemanticFingerprint =
            root.expectedProgramSemanticFingerprint;
        occurrences[0].stableOccurrenceOrdinal = 4;
        occurrences[0].childPlayable = nested;
        occurrences[0].occurrenceSemanticFingerprint = 204;
        SetF32(occurrences[0].payload, PropertyId::Volume, 4.0f);
        SetU32(occurrences[0].payload, PropertyId::LoopCount, 1);

        occurrences[1].parentKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        occurrences[1].parentStableId = nested.stableId;
        occurrences[1].parentProgramSemanticFingerprint =
            nested.expectedProgramSemanticFingerprint;
        occurrences[1].stableOccurrenceOrdinal = 6;
        occurrences[1].childPlayable = leaf;
        occurrences[1].occurrenceSemanticFingerprint = 206;
        SetF32(occurrences[1].payload, PropertyId::Volume, 6.0f);
        SetF32(properties[5].payload, PropertyId::SpatialSpread, 0.75f);
        SetU32(properties[5].payload, PropertyId::SpatialIs3DEnabled, 1);
        SetU64(properties[5].payload, PropertyId::SpatialAttenuationRef, 500);
        properties[5].payload.sendCount = 2;
        properties[5].payload.sends = leafSends.data();

        rootChildren[0].playable = nested;
        rootChildren[0].stableChildOrdinal = 4;
        nestedChildren[0].playable = leaf;
        nestedChildren[0].stableChildOrdinal = 6;
        containers[0].containerStableId = root.stableId;
        containers[0].programSemanticFingerprint =
            root.expectedProgramSemanticFingerprint;
        containers[0].mode = harken::ContainerModeV1::Sequence;
        containers[0].childCount = 1;
        containers[0].children = rootChildren.data();
        containers[1].containerStableId = nested.stableId;
        containers[1].programSemanticFingerprint =
            nested.expectedProgramSemanticFingerprint;
        containers[1].mode = harken::ContainerModeV1::Sequence;
        containers[1].childCount = 1;
        containers[1].children = nestedChildren.data();

        world.generation = 9;
        world.identity.runtimeSemanticFingerprint = 900;
        world.properties = properties.data();
        world.propertyCount = static_cast<std::uint32_t>(properties.size());
        world.occurrences = occurrences.data();
        world.occurrenceCount =
            static_cast<std::uint32_t>(occurrences.size());
        world.containers = containers.data();
        world.containerCount = static_cast<std::uint32_t>(containers.size());
    }
};

TEST(PlayablePropertyCut, FoldsTheActualPathAndAppliesRootLayersOnce) {
    WorldFixture fixture;
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    const std::uint32_t selections[] = {4, 6};
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(fixture.world, fixture.root, selections, 2));
    const auto& cut = storage.View();
    EXPECT_EQ(9u, cut.world);
    EXPECT_EQ(900u, cut.worldSemanticFingerprint);
    EXPECT_EQ(7u, cut.pathCount);
    EXPECT_EQ(
        harken::playableproperties::ContentPropertyKindV1::Layer,
        cut.path[0].contentPropertyKind);
    EXPECT_EQ(harkentest::ObjectId(1u), cut.path[0].stableId);
    EXPECT_EQ(harkentest::ObjectId(2u), cut.path[1].stableId);
    EXPECT_EQ(harkentest::ObjectId(30u), cut.leaf.stableId);
    EXPECT_FLOAT_EQ(28.0f, cut.volumeDb);
    EXPECT_FLOAT_EQ(7200.0f, cut.pitchCents);
    EXPECT_FLOAT_EQ(2.0f, cut.makeUpGainDb);
    EXPECT_EQ(cut.loopCount, 1u);
    EXPECT_EQ(1u, cut.streamingMode);
    EXPECT_EQ(harkentest::ObjectId(200u), cut.mainOutputBusRef);
    EXPECT_FLOAT_EQ(0.0f, cut.spatial.panning);
    EXPECT_FLOAT_EQ(0.75f, cut.spatial.spread);
    EXPECT_TRUE(cut.spatial.is3DEnabled);
    EXPECT_EQ(harkentest::ObjectId(500u), cut.spatial.attenuationRef);
    ASSERT_EQ(2u, cut.sendCount);
    EXPECT_EQ(harkentest::ObjectId(10u), cut.sends[0].targetBusId);
    EXPECT_FLOAT_EQ(-3.0f, cut.sends[0].sendLevel);
    EXPECT_EQ(harkentest::ObjectId(20u), cut.sends[1].targetBusId);
    EXPECT_EQ(0u, cut.effectNodeCount);
}

TEST(PlayablePropertyCut, MainOutputComesFromTheCompleteLeafSnapshot) {
    WorldFixture fixture;
    SetF32(fixture.properties[0].payload, PropertyId::OutputBusVolume, -12.0f);
    SetF32(fixture.properties[3].payload, PropertyId::OutputBusVolume, -6.0f);
    SetF32(fixture.properties[5].payload, PropertyId::OutputBusVolume, -3.0f);
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    const std::uint32_t selections[] = {4, 6};
    const auto resolve = [&] {
        return storage.Resolve(fixture.world, fixture.root, selections, 2);
    };
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok, resolve());
    EXPECT_FLOAT_EQ(-3.0f, storage.View().outputBusVolumeDb);
    EXPECT_FLOAT_EQ(28.0f, storage.View().volumeDb);
    EXPECT_EQ(harkentest::ObjectId(200u), storage.View().mainOutputBusRef);

    fixture.properties[5].payload.presentMask &=
        ~(UINT64_C(1) << Slot(PropertyId::OutputBusVolume));
    EXPECT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::InvalidInput, resolve());
    EXPECT_EQ(harkentest::ObjectId(200u), storage.View().mainOutputBusRef);
    EXPECT_FLOAT_EQ(-3.0f, storage.View().outputBusVolumeDb);
}

TEST(PlayablePropertyCut, SpatializationMixPreservesZeroMidpointAndDefault) {
    WorldFixture fixture;
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    const std::uint32_t selections[] = {4, 6};
    for (const float value : {0.0f, 0.5f, 1.0f}) {
        SetF32(fixture.properties[5].payload,
               PropertyId::SpatializationMix, value);
        ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
                  storage.Resolve(fixture.world, fixture.root, selections, 2));
        EXPECT_FLOAT_EQ(value, storage.View().spatial.spatializationMix);
    }
}

TEST(PlayablePropertyCut, RejectsOccurrenceGroupValuesWithoutPublishing) {
    WorldFixture fixture;
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    const std::uint32_t selections[] = {4, 6};
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(fixture.world, fixture.root, selections, 2));
    SetF32(fixture.occurrences[1].payload, PropertyId::SpatialPanning, 0.5f);
    EXPECT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::InvalidInput,
              storage.Resolve(fixture.world, fixture.root, selections, 2));
    EXPECT_EQ(harkentest::ObjectId(30u), storage.View().leaf.stableId);
    EXPECT_FLOAT_EQ(0.0f, storage.View().spatial.panning);
}

TEST(PlayablePropertyCut, ContainerLoopCountDoesNotBecomeVoiceLoop) {
    WorldFixture fixture;
    SetU32(fixture.properties[3].payload, PropertyId::LoopCount, 2);
    SetU32(fixture.properties[4].payload, PropertyId::LoopCount, 3);
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    const std::uint32_t selections[] = {4, 6};
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(fixture.world, fixture.root, selections, 2));
    EXPECT_EQ(1u, storage.View().loopCount);
}

TEST(PlayablePropertyCut, StructuralAncestorsApplyOnceAndOccurrenceOnlyOnPlacement) {
    using Kind = harken::playableproperties::ContentPropertyKindV1;
    using Playable = harken::PlayableKindV1;
    harken::PlayablePropertyProgramViewV1 layer{};
    layer.payload = ResolvedPayload();
    layer.contentPropertyKind = Kind::Layer;
    layer.stableId = harkentest::ObjectId(1);
    layer.programSemanticFingerprint = 101;

    harken::PlayablePropertyProgramViewV1 container{};
    container.payload = ResolvedPayload();
    container.contentPropertyKind = Kind::Container;
    container.stableId = harkentest::ObjectId(2);
    container.programSemanticFingerprint = 102;
    container.contentRevision = 20;
    container.routeStableId = harkentest::ObjectId(200);
    container.compatibilityFingerprint = 2000;
    harken::StructuralAncestorRefV1 containerParent{Kind::Layer, 0, harkentest::ObjectId(1), 101};
    containerParent.payload = MixingContribution(layer.payload);
    container.structuralAncestorCount = 1;
    container.structuralAncestors = &containerParent;
    SetF32(container.payload, PropertyId::Pitch, 1200.0f);

    harken::PlayablePropertyProgramViewV1 sound{};
    sound.payload = ResolvedPayload();
    sound.contentPropertyKind = Kind::Sound;
    sound.stableId = harkentest::ObjectId(3);
    sound.programSemanticFingerprint = 103;
    sound.contentRevision = 30;
    sound.routeStableId = harkentest::ObjectId(300);
    sound.compatibilityFingerprint = 3000;
    harken::StructuralAncestorRefV1 soundParents[] = {
        {Kind::Layer, 0, harkentest::ObjectId(1), 101, MixingContribution(layer.payload)},
        {Kind::Container, 0, harkentest::ObjectId(2), 102, MixingContribution(container.payload)}};
    sound.structuralAncestorCount = 2;
    sound.structuralAncestors = soundParents;

    harken::PlayableRefV1 containerRef = Ref(Playable::Container, 2, 102);
    harken::PlayableRefV1 soundRef = Ref(Playable::Sound, 3, 103);
    harken::PlayableOccurrencePropertyViewV1 occurrence{};
    occurrence.parentKind = Kind::Container;
    occurrence.parentStableId = harkentest::ObjectId(2);
    occurrence.parentProgramSemanticFingerprint = 102;
    occurrence.stableOccurrenceOrdinal = 7;
    occurrence.childPlayable = soundRef;
    occurrence.occurrenceSemanticFingerprint = 107;
    SetF32(occurrence.payload, PropertyId::Pitch, 300.0f);

    harken::ContainerChildProgramViewV2 child{};
    child.playable = soundRef;
    child.stableChildOrdinal = 7;
    harken::ContainerProgramViewV3 containerProgram{};
    containerProgram.containerStableId = harkentest::ObjectId(2);
    containerProgram.programSemanticFingerprint = 102;
    containerProgram.mode = harken::ContainerModeV1::Sequence;
    containerProgram.childCount = 1;
    containerProgram.children = &child;

    harken::PlayablePropertyProgramViewV1 properties[] = {layer, container, sound};
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = properties;
    world.propertyCount = 3;
    world.occurrences = &occurrence;
    world.occurrenceCount = 1;
    world.containers = &containerProgram;
    world.containerCount = 1;

    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(world, soundRef, nullptr, 0));
    EXPECT_FLOAT_EQ(1200.0f, storage.View().pitchCents);

    const std::uint32_t selection = 7;
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(world, containerRef, &selection, 1));
    EXPECT_FLOAT_EQ(1500.0f, storage.View().pitchCents);
}

TEST(PlayablePropertyCut, GroupsStreamingAndEffectsUseOnlyTheLeafDefinition) {
    using Kind = harken::playableproperties::ContentPropertyKindV1;
    const auto parentRef = Ref(harken::PlayableKindV1::Container, 2, 102);
    const auto childRef = Ref(harken::PlayableKindV1::Sound, 3, 103);
    auto parent = Property(Kind::Container, parentRef);
    auto child = Property(Kind::Sound, childRef);
    SetF32(parent.payload, PropertyId::Volume, -6.0f);
    SetU32(parent.payload, PropertyId::LoopCount, 4);
    SetU32(parent.payload, PropertyId::StreamingMode, 1);
    SetU64(parent.payload, PropertyId::MainOutputBusRef, 20);
    SetF32(parent.payload, PropertyId::SpatialPanning, 0.5f);
    SetF32(parent.payload, PropertyId::SpatialSpread, 25.0f);
    SetU32(parent.payload, PropertyId::SpatialIs3DEnabled, 1);
    SetF32(parent.payload, PropertyId::SpatialAttenuationDistanceScaling, 2.0f);
    SetU64(parent.payload, PropertyId::SpatialAttenuationRef, 30);
    parent.payload.presentMask |= UINT64_C(1) << Slot(PropertyId::SignalGraph);
    harken::EffectNodeV1 effect{};
    effect.nodeId = 50;
    parent.payload.effectNodeCount = 1;
    parent.payload.effectNodes = &effect;
    harken::StructuralAncestorRefV1 ancestor{Kind::Container, 0, harkentest::ObjectId(2), 102, MixingContribution(parent.payload)};
    child.structuralAncestorCount = 1;
    child.structuralAncestors = &ancestor;
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = &child;
    world.propertyCount = 1;
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(world, childRef, nullptr, 0));
    EXPECT_EQ(VN_BUS_MASTER, storage.View().mainOutputBusRef);
    EXPECT_FLOAT_EQ(0.0f, storage.View().spatial.panning);
    EXPECT_FLOAT_EQ(0.0f, storage.View().spatial.spread);
    EXPECT_FALSE(storage.View().spatial.is3DEnabled);
    EXPECT_FLOAT_EQ(1.0f, storage.View().spatial.attenuationDistanceScaling);
    EXPECT_EQ(harkentest::ObjectId(0u), storage.View().spatial.attenuationRef);
    EXPECT_EQ(0u, storage.View().effectNodeCount);
    EXPECT_EQ(1u, storage.View().loopCount);
    EXPECT_EQ(0u, storage.View().streamingMode);

    SetF32(child.payload, PropertyId::Volume, -3.0f);
    SetU32(child.payload, PropertyId::StreamingMode, 0);
    SetU64(child.payload, PropertyId::MainOutputBusRef, 1);
    SetF32(child.payload, PropertyId::SpatialPanning, 0.0f);
    SetF32(child.payload, PropertyId::SpatialSpread, 0.0f);
    SetU32(child.payload, PropertyId::SpatialIs3DEnabled, 0);
    SetF32(child.payload, PropertyId::SpatialAttenuationDistanceScaling, 1.0f);
    SetU64(child.payload, PropertyId::SpatialAttenuationRef, 0);
    child.payload.presentMask |= UINT64_C(1) << Slot(PropertyId::SignalGraph);
    child.payload.effectNodeCount = 1;
    child.payload.effectNodes = &effect;
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(world, childRef, nullptr, 0));
    EXPECT_FLOAT_EQ(-9.0f, storage.View().volumeDb);
    EXPECT_EQ(0u, storage.View().streamingMode);
    EXPECT_EQ(VN_BUS_MASTER, storage.View().mainOutputBusRef);
    EXPECT_FLOAT_EQ(0.0f, storage.View().spatial.panning);
    EXPECT_FLOAT_EQ(0.0f, storage.View().spatial.spread);
    EXPECT_FALSE(storage.View().spatial.is3DEnabled);
    EXPECT_FLOAT_EQ(1.0f, storage.View().spatial.attenuationDistanceScaling);
    EXPECT_EQ(harkentest::ObjectId(0u), storage.View().spatial.attenuationRef);
    ASSERT_EQ(1u, storage.View().effectNodeCount);
    EXPECT_EQ(50u, storage.View().effectNodes[0].nodeId);
}

TEST(PlayablePropertyCut, ExplicitEmptyOverridesAndFailurePreservesTheCut) {
    WorldFixture fixture;
    fixture.properties[5].payload.sendCount = 0;
    fixture.properties[5].payload.sends = nullptr;
    fixture.properties[5].payload.presentMask |=
        (UINT64_C(1) << Slot(PropertyId::SignalGraph));
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    const std::uint32_t selections[] = {4, 6};
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(fixture.world, fixture.root, selections, 2));
    EXPECT_EQ(0u, storage.View().effectNodeCount);
    EXPECT_EQ(0u, storage.View().sendCount);
    EXPECT_EQ(harkentest::ObjectId(30u), storage.View().leaf.stableId);

    const std::uint32_t wrong[] = {4, 7};
    EXPECT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::PathMismatch,
              storage.Resolve(fixture.world, fixture.root, wrong, 2));
    EXPECT_EQ(harkentest::ObjectId(30u), storage.View().leaf.stableId);
    EXPECT_EQ(28.0f, storage.View().volumeDb);
}

TEST(PlayablePropertyCut, RefusesUnknownTrackAndCapacityWithoutPublishing) {
    WorldFixture fixture;
    fixture.nestedChildren[0].playable =
        Ref(harken::PlayableKindV1::Track, 40, 140);
    fixture.occurrences[1].childPlayable = fixture.nestedChildren[0].playable;
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    const std::uint32_t selections[] = {4, 6};
    EXPECT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::UnknownPlayable,
              storage.Resolve(fixture.world, fixture.root, selections, 2));
    EXPECT_EQ(0u, storage.View().world);

    fixture.nestedChildren[0].playable = fixture.leaf;
    fixture.occurrences[1].childPlayable = fixture.leaf;
    harken::ResolvedPlayablePropertyCutConfigV1 config{};
    config.maxPathRows = 6;
    harken::ResolvedPlayablePropertyCutStorageV1 narrow;
    ASSERT_TRUE(narrow.Reset(config));
    EXPECT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Capacity,
              narrow.Resolve(fixture.world, fixture.root, selections, 2));
    EXPECT_EQ(0u, narrow.View().world);
}

TEST(PlayablePropertyCut, SendCapacityRefusalPreservesThePublishedCut) {
    WorldFixture fixture;
    fixture.properties[5].payload.sendCount = 1;
    harken::ResolvedPlayablePropertyCutConfigV1 config{};
    config.maxSends = 1;
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset(config));
    const std::uint32_t selections[] = {4, 6};
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(fixture.world, fixture.root, selections, 2));
    fixture.properties[5].payload.sendCount = 2;
    EXPECT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Capacity,
              storage.Resolve(fixture.world, fixture.root, selections, 2));
    ASSERT_EQ(1u, storage.View().sendCount);
    EXPECT_EQ(harkentest::ObjectId(10u), storage.View().sends[0].targetBusId);
    EXPECT_EQ(harkentest::ObjectId(30u), storage.View().leaf.stableId);
}

TEST(PlayablePropertyCut, SegmentTracksWithTheSameOrdinalResolveDistinctLeafSnapshots) {
    using Kind = harken::playableproperties::ContentPropertyKindV1;
    const auto firstSound = Ref(harken::PlayableKindV1::Sound, 10, 110);
    const auto secondSound = Ref(harken::PlayableKindV1::Sound, 20, 120);
    const auto firstTrack = Ref(harken::PlayableKindV1::Track, 30, 130);
    const auto secondTrack = Ref(harken::PlayableKindV1::Track, 40, 140);
    const auto root = Ref(harken::PlayableKindV1::Segment, 50, 150);
    std::array properties{
        Property(Kind::Sound, firstSound), Property(Kind::Sound, secondSound),
        Property(Kind::Track, firstTrack), Property(Kind::Track, secondTrack),
        Property(Kind::Segment, root)};
    SetU64(properties[0].payload, PropertyId::MainOutputBusRef, 100);
    SetF32(properties[0].payload, PropertyId::OutputBusVolume, -3.0f);
    SetF32(properties[0].payload, PropertyId::SpatialPanning, -0.5f);
    SetU64(properties[0].payload, PropertyId::SpatialAttenuationRef, 300);
    SetU64(properties[1].payload, PropertyId::MainOutputBusRef, 200);
    SetF32(properties[1].payload, PropertyId::OutputBusVolume, -9.0f);
    SetF32(properties[1].payload, PropertyId::SpatialPanning, 0.5f);
    SetU64(properties[1].payload, PropertyId::SpatialAttenuationRef, 400);
    const harken::SendV1 send{0, 0, harkentest::ObjectId(201), -6.0f, 0};
    properties[1].payload.sendCount = 1;
    properties[1].payload.sends = &send;
    std::array<harken::ScheduleClipPlacementV1, 2> placements{};
    placements[0].content = firstSound;
    placements[1].content = secondSound;
    std::array<harken::TrackScheduleProgramViewV1, 2> tracks{};
    tracks[0].rootDefinition = firstTrack;
    tracks[1].rootDefinition = secondTrack;
    std::array<harken::PlayableOccurrencePropertyViewV1, 2> occurrences{};
    std::array<harken::ScheduleTrackRefV1, 2> trackRefs{};
    for (std::uint32_t index = 0; index < tracks.size(); ++index) {
        tracks[index].placementCount = 1;
        tracks[index].placements = &placements[index];
        tracks[index].scheduleSemanticFingerprint = 500 + index;
        occurrences[index].parentKind = Kind::Track;
        occurrences[index].parentStableId = tracks[index].rootDefinition.stableId;
        occurrences[index].parentProgramSemanticFingerprint =
            tracks[index].rootDefinition.expectedProgramSemanticFingerprint;
        occurrences[index].childPlayable = placements[index].content;
        occurrences[index].occurrenceSemanticFingerprint = 600 + index;
        trackRefs[index].trackDefinition = tracks[index].rootDefinition;
        trackRefs[index].trackScheduleSemanticFingerprint =
            tracks[index].scheduleSemanticFingerprint;
        trackRefs[index].stableTrackOrdinal = index;
    }
    harken::SegmentScheduleProgramViewV1 segment{};
    segment.rootDefinition = root;
    segment.trackRefCount = 2;
    segment.trackRefs = trackRefs.data();
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = properties.data();
    world.propertyCount = properties.size();
    world.occurrences = occurrences.data();
    world.occurrenceCount = occurrences.size();
    world.trackSchedules = tracks.data();
    world.trackScheduleCount = tracks.size();
    world.segmentSchedules = &segment;
    world.segmentScheduleCount = 1;
    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    const std::uint32_t ordinal = 0;
    EXPECT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::PathMismatch,
              storage.Resolve(world, root, &ordinal, 1));
    auto trackId = firstTrack.stableId;
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(world, root, &ordinal, 1, &trackId));
    EXPECT_EQ(firstSound.stableId, storage.View().leaf.stableId);
    EXPECT_EQ(harkentest::ObjectId(100u), storage.View().mainOutputBusRef);
    EXPECT_FLOAT_EQ(-3.0f, storage.View().outputBusVolumeDb);
    EXPECT_FLOAT_EQ(-0.5f, storage.View().spatial.panning);
    EXPECT_EQ(harkentest::ObjectId(300u), storage.View().spatial.attenuationRef);
    EXPECT_EQ(0u, storage.View().sendCount);
    trackId = secondTrack.stableId;
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(world, root, &ordinal, 1, &trackId));
    EXPECT_EQ(secondSound.stableId, storage.View().leaf.stableId);
    EXPECT_EQ(harkentest::ObjectId(200u), storage.View().mainOutputBusRef);
    EXPECT_FLOAT_EQ(-9.0f, storage.View().outputBusVolumeDb);
    EXPECT_FLOAT_EQ(0.5f, storage.View().spatial.panning);
    EXPECT_EQ(harkentest::ObjectId(400u), storage.View().spatial.attenuationRef);
    ASSERT_EQ(1u, storage.View().sendCount);
    EXPECT_EQ(harkentest::ObjectId(201u), storage.View().sends[0].targetBusId);
    trackId = harkentest::ObjectId(999);
    EXPECT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::PathMismatch,
              storage.Resolve(world, root, &ordinal, 1, &trackId));
    EXPECT_EQ(secondSound.stableId, storage.View().leaf.stableId);
}

}
