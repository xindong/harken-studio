#include "ProjectTypes.h"

#include <gtest/gtest.h>

#include <memory>

// Free resolver functions defined in IPCHandlers_Project.cpp. These are the
// exact functions the write commands use: delete_item / rename_item /
// move_item / set_item_property / paste_* and every Container write resolve
// their object_id through resolveItemByPathOrObjectID, and several Spatial /
// Asset writes go through findItemByObjectIDInProject.
namespace VNX {
uint32_t itemShortID(const ProjectItem* item);
ProjectItem* findItemByObjectIDInProject(ProjectData* project, uint32_t objectID);
ProjectItem* findItemByAuthoringHandleInProject(ProjectData* project, uint32_t authoringHandle);
ProjectItem* resolveItemByPathOrObjectID(ProjectData* project, const QString& path,
                                         uint32_t objectID);
}

using namespace VNX;

namespace {

// Builds a project holding one item named "SFX" with a small sequential
// authoring handle (8), mirroring what read commands expose: object_id is the
// name-hash short id and authoring_handle is the raw objectID.
struct ContractFixture
{
    ProjectData project;
    ProjectItem* sfx = nullptr;

    ContractFixture()
    {
        ProjectFileBinding binding;
        binding.rootItem = std::make_shared<ProjectItem>();
        binding.rootItem->name = QStringLiteral("Actions");
        binding.rootItem->type = ProjectItem::Type::None;
        binding.rootItem->objectID = 1;

        auto child = std::make_shared<ProjectItem>();
        child->name = QStringLiteral("SFX");
        child->type = ProjectItem::Type::Sound;
        child->objectID = 8;
        sfx = child.get();
        binding.rootItem->children.push_back(std::move(child));

        project.actions.push_back(std::move(binding));
    }
};

} // namespace

TEST(ObjectIdContract, WriteResolverAcceptsBothShortIdAndAuthoringHandle)
{
    ContractFixture f;
    const uint32_t shortId = itemShortID(f.sfx);   // object_id in read responses
    const uint32_t handle = f.sfx->objectID;       // authoring_handle in read responses

    // The two identifiers a read hands out are genuinely different values.
    ASSERT_NE(shortId, handle);
    ASSERT_EQ(handle, 8u);

    // Both must resolve to the same item through the shared write resolver, so
    // whichever identifier the model passes to delete_item/rename_item/etc.
    // locates the object instead of returning ITEM_NOT_FOUND.
    EXPECT_EQ(resolveItemByPathOrObjectID(&f.project, QString(), shortId), f.sfx);
    EXPECT_EQ(resolveItemByPathOrObjectID(&f.project, QString(), handle), f.sfx);
}

TEST(ObjectIdContract, FindItemByObjectIdResolvesBothIdentifiers)
{
    ContractFixture f;
    const uint32_t shortId = itemShortID(f.sfx);
    const uint32_t handle = f.sfx->objectID;

    // The lenient resolver used by Spatial/Asset writes already accepts both;
    // this pins that the aligned read emits (object_id = short id) stay valid.
    EXPECT_EQ(findItemByObjectIDInProject(&f.project, shortId), f.sfx);
    EXPECT_EQ(findItemByObjectIDInProject(&f.project, handle), f.sfx);
}

TEST(ObjectIdContract, AuthoringHandleDisambiguatesDuplicateNames)
{
    ContractFixture f;
    auto twin = std::make_shared<ProjectItem>();
    twin->name = f.sfx->name;
    twin->type = ProjectItem::Type::Sound;
    twin->objectID = 9;
    ProjectItem* twinPtr = twin.get();
    f.project.actions.front().rootItem->children.push_back(std::move(twin));

    ASSERT_EQ(itemShortID(f.sfx), itemShortID(twinPtr));
    EXPECT_EQ(findItemByObjectIDInProject(&f.project, itemShortID(twinPtr)), nullptr);

    // The authoring handle stays unique per item; GUI writes pass it via
    // set_item_property's authoring_handle arg so edits land on the exact item.
    EXPECT_EQ(findItemByAuthoringHandleInProject(&f.project, 8u), f.sfx);
    EXPECT_EQ(findItemByAuthoringHandleInProject(&f.project, 9u), twinPtr);
}

TEST(ObjectIdContract, UnknownIdentifierStillFailsCleanly)
{
    ContractFixture f;
    // An id matching neither the short id nor any authoring handle resolves to
    // nothing (no accidental match from the added fallback).
    const uint32_t bogus = 999999u;
    ASSERT_NE(bogus, itemShortID(f.sfx));
    ASSERT_NE(bogus, f.sfx->objectID);
    EXPECT_EQ(resolveItemByPathOrObjectID(&f.project, QString(), bogus), nullptr);
}
