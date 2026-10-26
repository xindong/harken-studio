#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/ProjectInput.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace harkenprojectschema {
namespace {

AuthoringGuid G(std::uint64_t n) { return harkentest::StudioObjectId(n); }

TEST(FrontEnd, ClipDurationRejectsOverflowingEnd) {
    ProjectInput input;
    InputNode track;
    track.kind = ContentKind::Track;
    track.guid = G(1);
    track.ownedChildren = {G(2)};
    InputClip clip;
    clip.guid = G(3);
    clip.targetGuid = G(2);
    clip.time = INT64_MAX - 1;
    clip.duration = 2;
    track.clips = {clip};
    InputNode sound;
    sound.kind = ContentKind::Sound;
    sound.guid = G(2);
    input.nodes = {track, sound};
    input.roots = {track.guid};

    const FrontEndResult result = CompileFrontEnd(input);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, DeepOwnershipChainCompilesWithoutUsingCallStack) {
    constexpr std::size_t kNodeCount = 20'000;
    ProjectInput in;
    in.nodes.reserve(kNodeCount);
    for (std::size_t i = 0; i < kNodeCount; ++i) {
        InputNode node;
        node.kind = ContentKind::Layer;
        node.guid = G(i + 1);
        if (i + 1 < kNodeCount) {
            node.ownedChildren = {G(i + 2)};
        }
        in.nodes.push_back(node);
    }
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.project.has_value());
    EXPECT_EQ(result.project->objects.size(), kNodeCount);
}

}
}
