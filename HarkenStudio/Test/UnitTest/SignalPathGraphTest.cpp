#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Widgets/Blueprint/SignalPathGraph.h"

#include "Core/FieldGroupProjectAdapter.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>

namespace {

using namespace VNX;
using T = ProjectItemType;

class SignalPathGraphTest : public ::testing::Test
{
protected:
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    uint32_t nextID = 1;
    ProjectItem* master = create("Master", T::Bus);
    ProjectItem* b1 = create("B1", T::Bus, master);
    ProjectItem* b2 = create("B2", T::Bus, master);
    ProjectItem* b3 = create("B3", T::Bus, master);
    ProjectItem* l3 = create("L3", T::Layer);
    ProjectItem* l1 = create("L1", T::Layer, l3);
    ProjectItem* p1 = create("P1", T::Piece, l1);
    ProjectItem* t1 = create("T1", T::Track, p1);
    ProjectItem* s1 = create("S1", T::Sound, t1);
    ProjectItem* s2 = create("S2", T::Sound, t1);
    ProjectItem* t2 = create("T2", T::Track, p1);
    ProjectItem* s3 = create("S3", T::Sound, t2);
    ProjectItem* c1 = create("C1", T::Container, l1);
    ProjectItem* s4 = create("S4", T::Sound, c1);
    ProjectItem* s5 = create("S5", T::Sound, c1);
    ProjectItem* l2 = create("L2", T::Layer, l3);
    ProjectItem* t3 = create("T3", T::Track, l2);
    ProjectItem* s6 = create("S6", T::Sound, t3);
    std::array<ProjectItem*, 6> sounds{s1, s2, s3, s4, s5, s6};

    void SetUp() override
    {
        output(p1, b1);
        output(c1, b1);
        output(s5, b3);
        output(l2, b1);
        send(c1, {{toFieldGroupGuid(b2->guid), -6.0f}});
    }

    ProjectItem* create(const char* name, T type, ProjectItem* parent = nullptr)
    {
        auto item = std::make_shared<ProjectItem>(QString::fromLatin1(name), type);
        item->guid = QUuid::createUuid();
        item->objectID = nextID++;
        item->guid = VNX::authoringUuid(harkentest::StudioObjectId(item->objectID));
        item->attachDefaultComponents();
        auto* result = item.get();
        if (parent) {
            item->parent = parent;
            item->parentRef = {type == T::Bus ? ParentRef::Kind::Bus : ParentRef::Kind::Item,
                               parent->guid};
            parent->children.push_back(std::move(item));
        } else {
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(type))
                item->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
            auto& bucket = type == T::Bus ? project.busses : project.layers;
            bucket.push_back({std::move(item), {}});
        }
        return result;
    }

    void output(ProjectItem* item, ProjectItem* bus)
    {
        item->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
            MainOutputFieldGroup{toFieldGroupGuid(bus->guid), 0.0f}};
    }

    void send(ProjectItem* item, std::vector<SendFieldGroupRow> rows)
    {
        item->fieldGroups.send.shared = {FieldGroupMode::Local, SendFieldGroup{std::move(rows)}};
    }

    std::vector<SignalPathIdentity> inputs() const
    {
        std::vector<SignalPathIdentity> result;
        for (const auto* sound : sounds) result.push_back({sound->guid, {}});
        return result;
    }

    SignalPathGraph build(FieldGroupScope platform = {}) const
    {
        return buildSignalPathGraph(project, inputs(), platform);
    }

};

TEST_F(SignalPathGraphTest, IdentitySurvivesInputOrderAndDuplicateRequests)
{
    const auto original = build();
    auto reordered = inputs();
    std::reverse(reordered.begin(), reordered.end());
    reordered.push_back(reordered.front());
    const auto rebuilt = buildSignalPathGraph(project, reordered);
    ASSERT_TRUE(original.ok());
    ASSERT_FALSE(original.nodes.empty());
    ASSERT_FALSE(original.paths.empty());
    ASSERT_TRUE(rebuilt.ok());
    ASSERT_EQ(rebuilt.nodes.size(), original.nodes.size());
    for (const auto& node : original.nodes) {
        EXPECT_EQ(std::count_if(rebuilt.nodes.begin(), rebuilt.nodes.end(), [&](const auto& candidate) {
            return candidate.key == node.key;
        }), 1);
    }
    for (const auto& path : original.paths) {
        const auto found = std::find_if(rebuilt.paths.begin(), rebuilt.paths.end(), [&](const auto& candidate) {
            return candidate.identity == path.identity;
        });
        ASSERT_NE(found, rebuilt.paths.end());
        EXPECT_EQ(found->routing, path.routing);
    }
}

}
