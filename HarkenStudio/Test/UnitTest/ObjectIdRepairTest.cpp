#include "ProjectTypes.h"

#include <gtest/gtest.h>

#include <memory>

using namespace VNX;

namespace {

std::shared_ptr<ProjectItem> makeItem(const QString& name,
                                      ProjectItem::Type type,
                                      uint32_t objectId)
{
    auto item = std::make_shared<ProjectItem>(name, type);
    item->objectID = objectId;
    item->guid = QUuid::createUuid();
    return item;
}

void appendChild(const std::shared_ptr<ProjectItem>& parent,
                 const std::shared_ptr<ProjectItem>& child)
{
    child->parent = parent.get();
    parent->children.push_back(child);
}

ProjectFileBinding makeBinding(const QString& parentFolder,
                               std::shared_ptr<ProjectItem> rootItem)
{
    ProjectFileBinding binding;
    binding.parentFolder = parentFolder;
    binding.rootItem = std::move(rootItem);
    return binding;
}

} // namespace

TEST(EditorObjectIdRepair, RaisesNextObjectIdAboveAllLoadedIds)
{
    ProjectData project;
    project.nextObjectID = 3;

    auto bus = makeItem(QStringLiteral("Master Bus"), ProjectItem::Type::Bus, 12);
    project.busses.push_back(makeBinding(QStringLiteral("Busses"), bus));

    auto layer = makeItem(QStringLiteral("Layer"), ProjectItem::Type::Layer, 20);
    auto sound = makeItem(QStringLiteral("Shot"), ProjectItem::Type::Sound, 50);
    appendChild(layer, sound);
    project.layers.push_back(makeBinding(QStringLiteral("Layers"), layer));

    const ObjectIdRepairResult result = ensureUniqueObjectIds(project);

    EXPECT_TRUE(result.remaps.empty());
    EXPECT_EQ(project.nextObjectID, 51u);
    EXPECT_EQ(result.nextObjectID, 51u);
    EXPECT_GT(project.generateObjectID(), 50u);
}

TEST(EditorObjectIdRepair, RemapsDuplicateKeepingFirstLoadedItem)
{
    ProjectData project;
    project.nextObjectID = 3;

    auto bus = makeItem(QStringLiteral("Reverb Bus"), ProjectItem::Type::Bus, 7);
    project.busses.push_back(makeBinding(QStringLiteral("Busses"), bus));

    auto layer = makeItem(QStringLiteral("Layer"), ProjectItem::Type::Layer, 20);
    auto sound = makeItem(QStringLiteral("Shot"), ProjectItem::Type::Sound, 7);
    appendChild(layer, sound);
    project.layers.push_back(makeBinding(QStringLiteral("Layers"), layer));

    const ObjectIdRepairResult result = ensureUniqueObjectIds(project);

    EXPECT_EQ(bus->objectID, 7u);
    EXPECT_NE(sound->objectID, 7u);
    EXPECT_GT(sound->objectID, 20u);
    ASSERT_EQ(result.remaps.size(), 1u);
    EXPECT_EQ(result.remaps[0].oldObjectID, 7u);
    EXPECT_EQ(result.remaps[0].newObjectID, sound->objectID);
    EXPECT_EQ(result.remaps[0].itemName, QStringLiteral("Shot"));
    EXPECT_GT(project.nextObjectID, sound->objectID);
}

TEST(EditorObjectIdRepair, NestedBindingOfAnyTypeIsNotSelfRemapped)
{
    ProjectData project;
    project.nextObjectID = 1;

    auto rootLayer = makeItem(QStringLiteral("Root Layer"),
                              ProjectItem::Type::Layer, 2);
    rootLayer->parentRef.kind = ParentRef::Kind::Root;

    auto nestedLayer = makeItem(QStringLiteral("Nested Layer"),
                                ProjectItem::Type::Layer, 3);
    nestedLayer->parentRef.kind = ParentRef::Kind::Item;
    nestedLayer->parentRef.guid = rootLayer->guid;
    appendChild(rootLayer, nestedLayer);

    auto nestedSound = makeItem(QStringLiteral("Nested Shot"),
                                ProjectItem::Type::Sound, 4);
    nestedSound->parentRef.kind = ParentRef::Kind::Item;
    nestedSound->parentRef.guid = nestedLayer->guid;
    appendChild(nestedLayer, nestedSound);

    project.layers.push_back(
        makeBinding(QStringLiteral("Layers"), rootLayer));
    project.layers.push_back(
        makeBinding(QStringLiteral("Layers"), nestedLayer));
    project.layers.push_back(
        makeBinding(QStringLiteral("Layers"), nestedSound));

    const ObjectIdRepairResult result = ensureUniqueObjectIds(project);

    EXPECT_TRUE(result.remaps.empty());
    EXPECT_EQ(rootLayer->objectID, 2u);
    EXPECT_EQ(nestedLayer->objectID, 3u);
    EXPECT_EQ(nestedSound->objectID, 4u);
    EXPECT_EQ(project.nextObjectID, 5u);
}
