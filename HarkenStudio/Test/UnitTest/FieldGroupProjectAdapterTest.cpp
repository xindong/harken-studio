#include "Support/ProjectPlatformTestUtils.h"
#include "FieldGroupProjectAdapter.h"
#include "ConversionDefaults.h"
#include "ProjectSerializer.h"
#include "ReferenceSlotResolution.h"
#include "TemplateApplyService.h"

#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "ProjectTypes.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>

namespace harken::studio {
namespace {

using namespace harken::studio::project_schema;
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
    const auto index = harken::studio::bridge::canonicalRuntimeProjectSourceIndex(project);
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

TEST(FieldGroupProjectAdapter, TemplateInstancesUseDestinationDefaultsThroughoutTheirOwnershipTree)
{
    auto project = HarkenTest::projectWithPlatforms();
    auto master = item(100, ProjectItem::Type::Bus);
    auto original = item(101, ProjectItem::Type::Conversion);
    auto destination = item(102, ProjectItem::Type::Conversion);
    auto layer = item(1, ProjectItem::Type::Layer);
    auto sound = item(2, ProjectItem::Type::Sound);
    for (const auto& value : {master, original, destination, layer, sound}) {
        value->attachDefaultComponents();
        value->objectID = project.generateObjectID();
    }
    master->name = QStringLiteral("Master Bus");
    project.busses.push_back({master, {}});
    const auto platform = project.platforms.front().id;
    for (const auto& preset : {original, destination}) {
        preset->comp<VConversion>()->config = ConversionDefaults::newPreset();
        project.conversionItems.push_back({preset, {}});
    }
    project.defaultConversionRefs.insert(platform, original->guid);
    layer->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    layer->fieldGroups.conversion.shared.localHistory = ConversionFieldGroup{toFieldGroupGuid(original->guid)};
    own(layer, sound);
    project.layers.push_back({layer, {}});
    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(project));
    ProjectTemplateData data;
    data.displayName = QStringLiteral("Ownership Tree");
    data.bus.name = master->name;
    data.bus.authoring = serializer.projectItemToJson(*master);
    data.content = {serializer.projectItemToJson(*layer)};
    ASSERT_FALSE(data.bus.authoring.isEmpty());
    ASSERT_FALSE(data.content.first().toObject().isEmpty());
    project.layers.clear();
    for (const auto& defaultRef : {destination->guid, QUuid{}}) {
        SCOPED_TRACE(defaultRef.toString().toStdString());
        project.defaultConversionRefs.clear();
        if (!defaultRef.isNull()) project.defaultConversionRefs.insert(platform, defaultRef);
        QString error;
        ASSERT_TRUE(TemplateApplyService::seedTemplateInto(project, data, &error)) << error.toStdString();
        ASSERT_EQ(project.layers.size(), 1u);
        const auto root = project.layers.front().rootItem;
        ASSERT_NE(root, nullptr);
        ASSERT_EQ(root->children.size(), 1u);
        EXPECT_NE(root->guid, layer->guid);
        const auto effective = std::make_shared<const FieldGroupEffectiveSnapshot>(
            buildFieldGroupEffectiveSnapshot(project, toFieldGroupGuid(platform)));
        ASSERT_TRUE(effective->ok());
        for (const auto* instance : {root.get(), root->children.front().get()}) {
            const auto resolved = ReferenceSlotResolution::resolveEffectiveConversion(
                ReferenceSlotResolution::buildResolutionScope(instance, effective));
            ASSERT_TRUE(resolved.valid) << resolved.error.toStdString();
            EXPECT_EQ(resolved.target, defaultRef.isNull() ? nullptr : destination.get());
        }
        project.layers.clear();
    }
}

}
}
