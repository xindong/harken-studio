#include "Support/ProjectPlatformTestUtils.h"
#include "FieldGroupProjectAdapter.h"

#include "CanonicalRuntimeProjectAdapterPrivate.h"
#include "ProjectTypes.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>

namespace VNX {
namespace {

using namespace harkenprojectschema;
using States = std::map<QUuid, DefinitionFieldGroups>;

std::shared_ptr<ProjectItem> item(std::uint64_t id, ProjectItem::Type type)
{
    auto result = std::make_shared<ProjectItem>(QString::number(id), type);
    result->guid = QUuid(QStringLiteral("00000000-0000-0000-0000-%1")
                            .arg(id, 12, 16, QLatin1Char('0')));
    return result;
}

void own(const std::shared_ptr<ProjectItem>& parent,
         const std::shared_ptr<ProjectItem>& child)
{
    parent->children.push_back(child);
    child->parent = parent.get();
}

FieldGroupProjectView projectView(ProjectData& project, const States& states)
{
    return buildFieldGroupProjectView(project, [&](const ProjectItem& definition) {
        const auto found = states.find(definition.guid);
        return found == states.end() ? nullptr : &found->second;
    });
}

TEST(FieldGroupProjectAdapter, FileBindingEnumerationDoesNotCreateRootsOrReorderChildren)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto segment = item(1, ProjectItem::Type::Segment);
    auto first = item(2, ProjectItem::Type::Track);
    auto second = item(3, ProjectItem::Type::Track);
    own(segment, first);
    own(segment, second);
    project.layers = {{second, {}}, {first, {}}, {segment, {}}};
    States states{{segment->guid, MakeRootFieldGroups({0, 100})},
                  {first->guid, {}}, {second->guid, {}}};
    const auto index = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(project);
    ASSERT_TRUE(index->valid);
    EXPECT_EQ(index->parentDefinition.at(first.get()), segment.get());
    EXPECT_EQ(index->parentDefinition.at(second.get()), segment.get());
    ASSERT_EQ(index->children.at(segment.get()).size(), 2u);
    EXPECT_EQ(index->children.at(segment.get())[0], first.get());
    EXPECT_EQ(index->children.at(segment.get())[1], second.get());
    const auto view = projectView(project, states);
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view.definitions.size(), 3u);
    EXPECT_TRUE(FieldGroupResolver(view.definitions, {0, 100}).ok());
}

TEST(FieldGroupProjectAdapter, RefusesDetachedParentReferences)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto sound = item(1, ProjectItem::Type::Sound);
    sound->parentRef.kind = ParentRef::Kind::Item;
    sound->parentRef.guid = item(2, ProjectItem::Type::Layer)->guid;
    project.layers.push_back({sound, {}});
    const auto view = projectView(project, {{sound->guid, {}}});
    EXPECT_FALSE(view.ok());
    EXPECT_TRUE(view.definitions.empty());
    ASSERT_FALSE(view.diagnostics.empty());
}

TEST(FieldGroupProjectAdapter, RefusesMultipleOwnersAndDuplicateEdges)
{
    for (bool duplicateEdge : {false, true}) {
        ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
        auto first = item(1, ProjectItem::Type::Layer);
        auto second = item(2, ProjectItem::Type::Layer);
        auto sound = item(3, ProjectItem::Type::Sound);
        own(first, sound);
        (duplicateEdge ? first : second)->children.push_back(sound);
        project.layers = {{first, {}}, {second, {}}};
        const auto view = projectView(project,
            {{first->guid, {}}, {second->guid, {}}, {sound->guid, {}}});
        EXPECT_FALSE(view.ok());
        EXPECT_TRUE(view.definitions.empty());
        EXPECT_TRUE(view.referenceTargets.empty());
    }
}

TEST(FieldGroupProjectAdapter, RefusesCyclesWithoutRecursingThroughDocumentPaths)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto first = item(1, ProjectItem::Type::Layer);
    auto second = item(2, ProjectItem::Type::Layer);
    own(first, second);
    own(second, first);
    project.layers.push_back({first, {}});
    const auto view = projectView(project, {{first->guid, {}}, {second->guid, {}}});
    EXPECT_FALSE(view.ok());
    EXPECT_TRUE(view.definitions.empty());
    first->children.clear();
    second->children.clear();
    first->parent = nullptr;
    second->parent = nullptr;
}

TEST(FieldGroupProjectAdapter, RefusesMismatchedBackPointersAndDuplicateGuids)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto layer = item(1, ProjectItem::Type::Layer);
    auto sound = item(2, ProjectItem::Type::Sound);
    auto wrong = item(3, ProjectItem::Type::Layer);
    own(layer, sound);
    sound->parent = wrong.get();
    project.layers = {{layer, {}}, {wrong, {}}};
    States states{{layer->guid, {}}, {sound->guid, {}}, {wrong->guid, {}}};
    EXPECT_FALSE(projectView(project, states).ok());
    sound->parent = layer.get();
    wrong->guid = sound->guid;
    EXPECT_FALSE(projectView(project, states).ok());
}

}
}
