#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_project_schema/FrontEnd.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace harkenprojectschema {
namespace {

AuthoringGuid G(std::uint64_t low) { return harkentest::StudioObjectId(low); }

InputNode Node(ContentKind kind, std::uint64_t guid) {
    InputNode node;
    node.kind = kind;
    node.guid = G(guid);
    return node;
}

TEST(PlayablePropertyRegistryV1, TrackClipOccurrenceOrdinalsAreUniquePerTrack)
{
    InputNode track = Node(ContentKind::Track, 1);
    track.ownedChildren = {G(2)};
    InputClip first;
    first.guid = G(3);
    first.targetGuid = G(2);
    first.stableOccurrenceOrdinal = 7;
    InputClip second = first;
    second.guid = G(4);
    track.clips = {first, second};
    ProjectInput input;
    input.nodes = {track, Node(ContentKind::Sound, 2)};
    input.roots = {G(1)};

    FrontEndResult result = CompileFrontEnd(input);
    ASSERT_FALSE(result.ok());
    EXPECT_FALSE(result.project.has_value());
}

}
}
