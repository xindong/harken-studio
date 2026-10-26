#include "ProjectSerializer.h"
#include "ProjectTypes.h"
#include "ProjectObjectIndex.h"
#include "Components/VMixNode.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>
#include <QJsonObject>

namespace {

std::shared_ptr<VNX::ProjectItem> makeItem(const QString& name,
                                         VNX::ProjectItem::Type type,
                                         uint32_t objectId)
{
    auto item = std::make_shared<VNX::ProjectItem>(name, type);
    item->objectID = objectId;
    item->guid = QUuid::createUuid();
    return item;
}

void appendChild(const std::shared_ptr<VNX::ProjectItem>& parent,
                 const std::shared_ptr<VNX::ProjectItem>& child)
{
    child->parent = parent.get();
    parent->children.push_back(child);
}

}

TEST(EditorStableIds, IndependentBranchesDoNotShareObjectIdentity)
{
    auto first = makeItem(QStringLiteral("New Sound"), VNX::ProjectItem::Type::Sound, 5);
    auto second = makeItem(QStringLiteral("New Sound"), VNX::ProjectItem::Type::Sound, 5);
    EXPECT_EQ(first->objectID, second->objectID);
    EXPECT_NE(first->runtimeId(), second->runtimeId());
    EXPECT_EQ(VNX::authoringUuid(first->runtimeId()), first->guid);
    const auto original = first->runtimeId();
    first->name = QStringLiteral("Renamed");
    first->sourcePath = QStringLiteral("Other/Source.wav");
    first->objectID = 99;
    EXPECT_EQ(first->runtimeId(), original);
}

TEST(EditorStableIds, DuplicateIdentityIsRejectedWithoutReminting)
{
    auto project = HarkenTest::projectWithPlatforms();
    project.root = makeItem(QStringLiteral("Root"), VNX::ProjectItem::Type::None, 1);
    auto first = makeItem(QStringLiteral("One"), VNX::ProjectItem::Type::Sound, 2);
    auto second = makeItem(QStringLiteral("Two"), VNX::ProjectItem::Type::Action, 3);
    second->guid = first->guid;
    appendChild(project.root, first);
    appendChild(project.root, second);
    QString error;
    EXPECT_FALSE(VNX::validateProjectObjectIdentities(project, &error));
    EXPECT_EQ(first->guid, second->guid);
    EXPECT_EQ(VNX::ProjectObjectIndex::fromProject(&project).find(first->runtimeId()), nullptr);
    second->guid = QUuid::createUuid();
    EXPECT_TRUE(VNX::validateProjectObjectIdentities(project, &error));
}

TEST(EditorStableIds, MissingIdentityIsRejectedWithoutAssigningOne)
{
    auto project = HarkenTest::projectWithPlatforms();
    project.root = makeItem(QStringLiteral("Root"), VNX::ProjectItem::Type::None, 1);
    QString error;
    ASSERT_TRUE(VNX::validateProjectObjectIdentities(project, &error));
    project.root->guid = QUuid{};
    EXPECT_FALSE(VNX::validateProjectObjectIdentities(project, &error));
    EXPECT_TRUE(project.root->guid.isNull());
}

TEST(EditorStableIds, EmbeddedEntitiesShareTheProjectIdentityDomain)
{
    auto project = HarkenTest::projectWithPlatforms();
    project.root = makeItem(QStringLiteral("Root"), VNX::ProjectItem::Type::None, 1);
    auto sound = makeItem(QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound, 2);
    appendChild(project.root, sound);
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), QUuid::createUuid());
    binding.guid = sound->guid;
    binding.address = VNX::BindingSemantics::volumeAddress();
    binding.variable = QUuid::createUuid();
    ASSERT_TRUE(sound->compOrCreate<VNX::VMixNode>()->bindings.replace({binding}));
    QString error;
    EXPECT_FALSE(VNX::validateProjectObjectIdentities(project, &error));
    binding.guid = QUuid::createUuid();
    ASSERT_TRUE(sound->comp<VNX::VMixNode>()->bindings.replace({binding}));
    EXPECT_TRUE(VNX::validateProjectObjectIdentities(project, &error));
    const auto index = VNX::ProjectObjectIndex::fromProject(&project);
    const auto* location = index.find(binding.runtimeId());
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->kind, VNX::ProjectObjectLocation::Kind::Binding);
    EXPECT_EQ(location->ownerGuid, sound->guid);
}

TEST(EditorStableIds, ProjectItemJsonRoundTripHasOneUuidIdentity)
{
    auto root = makeItem(QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer, 100);
    auto child = makeItem(QStringLiteral("Shot"), VNX::ProjectItem::Type::Sound, 101);
    appendChild(root, child);
    VNX::ProjectSerializer serializer;
    const QJsonObject json = serializer.projectItemToJson(*root);
    VNX::ProjectItem loaded;
    ASSERT_TRUE(serializer.projectItemFromJson(json, loaded));
    EXPECT_EQ(loaded.runtimeId(), root->runtimeId());
    ASSERT_EQ(loaded.children.size(), 1u);
    EXPECT_EQ(loaded.children[0]->runtimeId(), child->runtimeId());
    EXPECT_EQ(loaded.children[0]->parent, &loaded);
}

TEST(EditorStableIds, EqualLowHalvesRemainDistinctInProjectIndex)
{
    auto project = HarkenTest::projectWithPlatforms();
    project.root = makeItem(QStringLiteral("Root"), VNX::ProjectItem::Type::None, 1);
    auto first = makeItem(QStringLiteral("One"), VNX::ProjectItem::Type::Sound, 2);
    auto second = makeItem(QStringLiteral("Two"), VNX::ProjectItem::Type::Sound, 3);
    first->guid = QUuid(QStringLiteral("11111111-1111-4111-8111-111111111111"));
    second->guid = QUuid(QStringLiteral("22222222-2222-4222-8111-111111111111"));
    appendChild(project.root, first);
    appendChild(project.root, second);
    ASSERT_EQ(first->runtimeId().low, second->runtimeId().low);
    const auto index = VNX::ProjectObjectIndex::fromProject(&project);
    ASSERT_NE(index.find(first->runtimeId()), nullptr);
    ASSERT_NE(index.find(second->runtimeId()), nullptr);
    EXPECT_EQ(index.find(first->runtimeId())->name, first->name);
    EXPECT_EQ(index.find(second->runtimeId())->name, second->name);
}

TEST(EditorStableIds, JsonWithoutUuidCannotUseDestinationIdentity)
{
    auto source = makeItem(QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer, 100);
    VNX::ProjectSerializer serializer;
    auto json = serializer.projectItemToJson(*source);
    VNX::ProjectItem valid;
    ASSERT_TRUE(serializer.projectItemFromJson(json, valid));
    json.remove(QStringLiteral("guid"));
    VNX::ProjectItem destination;
    const auto initialGuid = destination.guid;
    EXPECT_FALSE(serializer.projectItemFromJson(json, destination));
    EXPECT_EQ(destination.guid, initialGuid);
}
