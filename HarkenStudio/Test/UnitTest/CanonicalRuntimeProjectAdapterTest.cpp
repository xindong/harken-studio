#include "UnitTest/StudioObjectIdTestSupport.h"
#include "CanonicalRuntimeProjectAdapter.h"
#include "FieldGroupProjectAdapter.h"
#include "Core/ProjectTypes.h"

#include <gtest/gtest.h>

using namespace VNX;
using namespace HarkenEditor::Bridge;

namespace {

std::shared_ptr<ProjectItem> makeItem(ProjectItem::Type type, uint32_t objectId,
                                      harkenstudio::ObjectId stableId)
{
    auto value = std::make_shared<ProjectItem>(QStringLiteral("Item"), type);
    value->objectID = objectId;
    value->guid = VNX::authoringUuid(harkentest::StudioObjectId(stableId));
    value->attachDefaultComponents();
    return value;
}

ProjectData projectWithRoot(const std::shared_ptr<ProjectItem>& root)
{
    ProjectData project;
    auto master = makeItem(ProjectItem::Type::Bus, 1000, harkentest::StudioObjectId(1000));
    root->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    ProjectFileBinding busBinding;
    busBinding.rootItem = std::move(master);
    project.busses.push_back(std::move(busBinding));
    ProjectFileBinding binding;
    binding.rootItem = root;
    project.layers.push_back(std::move(binding));
    return project;
}

}

TEST(CanonicalRuntimeProjectAdapterTest, RejectsDuplicateStableIdentity)
{
    auto layer = makeItem(ProjectItem::Type::Layer, 1, harkentest::StudioObjectId(11));
    auto first = makeItem(ProjectItem::Type::Sound, 2, harkentest::StudioObjectId(12));
    auto second = makeItem(ProjectItem::Type::Sound, 3, harkentest::StudioObjectId(12));
    layer->children = {first, second};
    first->parent = layer.get();
    second->parent = layer.get();
    const auto result = compileStudioProjectInput(projectWithRoot(layer));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.diagnostics.front().code,
              ProjectInputDiagnosticCode::DuplicateIdentity);
}
