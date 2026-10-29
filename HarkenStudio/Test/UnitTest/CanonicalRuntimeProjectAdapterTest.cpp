#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "FieldGroupProjectAdapter.h"
#include "Core/ProjectTypes.h"

#include <gtest/gtest.h>
#include <QDir>
#include <QTemporaryDir>

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

TEST(CanonicalRuntimeProjectAdapterTest, SourcePathsPreserveIndependentDocumentsAcrossProjectLocations)
{
    QTemporaryDir directory(QDir::current().filePath(QStringLiteral("harken-source-index-XXXXXX")));
    ASSERT_TRUE(directory.isValid());
    auto layer = makeItem(ProjectItem::Type::Layer, 1, harkentest::StudioObjectId(11));
    auto first = makeItem(ProjectItem::Type::Sound, 2, harkentest::StudioObjectId(12));
    auto relative = makeItem(ProjectItem::Type::Sound, 3, harkentest::StudioObjectId(13));
    auto missing = makeItem(ProjectItem::Type::Sound, 4, harkentest::StudioObjectId(14));
    layer->children = {first, relative, missing};
    for (const auto& child : layer->children) child->parent = layer.get();
    auto project = projectWithRoot(layer);
    const QDir root(directory.path());
    const QString layerPath = root.filePath(QStringLiteral("Layers/Layer.vp"));
    const QString soundPath = root.filePath(QStringLiteral("Layers/Sound.vp"));
    project.setObjectFileLocation(layer.get(), nullptr, layerPath);
    project.setObjectFileLocation(first.get(), nullptr, soundPath);
    project.setObjectFileLocation(relative.get(), nullptr,
                                  QStringLiteral("Layers/Nested/../Relative.vp"));
    const QString fallback = QStringLiteral("Project/%1.vp")
        .arg(missing->guid.toString(QUuid::WithoutBraces));
    const QString filePath = root.filePath(QStringLiteral("Session.hsp"));
    const QString relativeDirectory = QDir::current().relativeFilePath(directory.path());
    const QString relativeFile = QDir::current().relativeFilePath(filePath);
    ASSERT_FALSE(QDir::isAbsolutePath(relativeDirectory));
    ASSERT_FALSE(QDir::isAbsolutePath(relativeFile));
    for (const QString& location : {directory.path(), filePath,
             relativeDirectory, relativeFile, QString{}}) {
        SCOPED_TRACE(location.toStdString());
        project.projectPath = location;
        const auto index = canonicalRuntimeProjectSourceIndex(project);
        ASSERT_TRUE(index->valid);
        EXPECT_TRUE(index->structureDiagnostics.empty());
        EXPECT_EQ(index->definitions.size(), 4u);
        EXPECT_EQ(index->byObjectId.at(first->objectID), first.get());
        EXPECT_EQ(index->parentDefinition.at(first.get()), layer.get());
        EXPECT_EQ(index->documentPaths.at(layer.get()),
            (location.isEmpty() ? layerPath : QStringLiteral("Layers/Layer.vp")).toStdString());
        EXPECT_EQ(index->documentPaths.at(first.get()),
            (location.isEmpty() ? soundPath : QStringLiteral("Layers/Sound.vp")).toStdString());
        EXPECT_EQ(index->documentPaths.at(relative.get()), "Layers/Relative.vp");
        EXPECT_EQ(index->documentPaths.at(missing.get()), fallback.toStdString());
    }
    project.projectPath = filePath;
    project.setObjectFileLocation(first.get(), nullptr,
        root.filePath(QStringLiteral("Relocated/Sound.vp")));
    const auto relocated = canonicalRuntimeProjectSourceIndex(project);
    ASSERT_TRUE(relocated->valid);
    EXPECT_EQ(relocated->documentPaths.at(first.get()), "Relocated/Sound.vp");
    EXPECT_EQ(relocated->documentPaths.at(layer.get()), "Layers/Layer.vp");
}
