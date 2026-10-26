#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/ReferenceResolution.h"
#include "harken_project_schema/Serialize.h"

#include "harken_reference_slots/ReferenceSlotResolver.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace harkenprojectschema {
namespace {

using fixtures::G;
using harkenreferenceslots::ReferenceSlot;

constexpr std::size_t kOutputBus =
    static_cast<std::size_t>(ReferenceSlot::OutputBus);
constexpr std::size_t kAttenuation =
    static_cast<std::size_t>(ReferenceSlot::Attenuation);

const OccurrenceReferenceResolution* FindOccurrence(
    const ReferenceResolutionFacts& facts, StableId clipId) {
    for (const OccurrenceReferenceResolution& occ : facts.occurrences) {
        if (occ.clipId == clipId) {
            return &occ;
        }
    }
    return nullptr;
}

const ContentObject* FindObject(const CanonicalProject& project, StableId id) {
    for (const ContentObject& object : project.objects) {
        if (object.chassis().id == id) {
            return &object;
        }
    }
    return nullptr;
}

ProjectInput LayeredWorld() {
    ProjectInput in;
    InputNode layer = fixtures::Node(ContentKind::Layer, 1);
    layer.guid = harkentest::StudioObjectId(1);
    layer.ownedChildren = {G(2)};
    InputNode container = fixtures::Node(ContentKind::Container, 2);
    container.guid = harkentest::StudioObjectId(2);
    container.ownedChildren = {G(4)};
    InputNode sound = fixtures::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);
    InputNode track = fixtures::Node(ContentKind::Track, 4);
    track.guid = harkentest::StudioObjectId(4);
    track.ownedChildren = {G(3)};
    InputClip clip = fixtures::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};
    in.nodes = {layer, container, sound, track};
    in.roots = {G(1)};
    return in;
}

TEST(ProjectSchemaReferenceResolution,
     WideReferenceValuesPassThroughResolution) {
    constexpr harkenstudio::ObjectId outputBus{0x1122334455664788ull, 0x100000001ull};
    constexpr harkenstudio::ObjectId attenuation{0x8877665544334211ull, 0x100000002ull};
    ProjectInput in = LayeredWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(outputBus);
    in.nodes[2].spatial.attenuationRef = harkentest::StudioObjectId(attenuation);
    in.nodes[2].presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.project.has_value());
    ASSERT_TRUE(front.facts.has_value());
    const ContentObject* sound = FindObject(*front.project, harkentest::StudioObjectId(3));
    ASSERT_NE(sound, nullptr);
    EXPECT_EQ(sound->chassis().mainOutputBusRef, outputBus);
    EXPECT_EQ(sound->chassis().spatial.attenuationRef, attenuation);

    const ReferenceResolutionResult resolution =
        BuildReferenceResolution(*front.project, *front.facts);
    ASSERT_TRUE(resolution.ok()) << RenderDiagnostics(resolution.diagnostics);
    ASSERT_TRUE(resolution.facts.has_value());
    const OccurrenceReferenceResolution* occurrence =
        FindOccurrence(*resolution.facts, harkentest::StudioObjectId(100));
    ASSERT_NE(occurrence, nullptr);
    EXPECT_EQ(occurrence->resolvedSlots[kOutputBus].value, outputBus);
    EXPECT_EQ(occurrence->resolvedSlots[kAttenuation].value, attenuation);
}

}
}
