#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "FieldGroupProjectAdapter.h"
#include "Core/ProjectTypes.h"
#include "Core/Components/VMixNode.h"
#include "Core/RuntimeCapabilityMatrix.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <harken_project_schema/ProjectInput.h>
#include <harken_project_schema/RuntimeIdentityIndex.h>

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

TEST(CanonicalRuntimeProjectAdapterTest, EffectiveGroupsShareAuthoringIdentityAcrossPlatformProjections)
{
    auto layer = makeItem(ProjectItem::Type::Layer, 1, harkentest::StudioObjectId(11));
    auto sound = makeItem(ProjectItem::Type::Sound, 2, harkentest::StudioObjectId(12));
    layer->children = {sound};
    sound->parent = layer.get();
    auto project = projectWithRoot(layer);
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
    EffectEntry shared;
    shared.guid = QUuid::createUuid();
    shared.mode = EffectMode::Custom;
    EffectFieldGroup sharedEffects;
    ASSERT_TRUE(sharedEffects.effects.replace({shared}));
    layer->fieldGroups.effects.shared = {FieldGroupMode::Local, sharedEffects};
    auto platform = shared;
    platform.bypass = true;
    EffectFieldGroup platformEffects;
    ASSERT_TRUE(platformEffects.effects.replace({platform}));
    layer->fieldGroups.effects.platforms[toFieldGroupGuid(HarkenTest::PlatformIds::Windows)] =
        {FieldGroupMode::Local, platformEffects};
    layer->fieldGroups.effects.platforms[toFieldGroupGuid(HarkenTest::PlatformIds::Android)] =
        {FieldGroupMode::Local, EffectFieldGroup{}};
    auto dormant = shared;
    dormant.guid = QUuid::createUuid();
    dormant.bypass = true;
    EffectFieldGroup dormantEffects;
    ASSERT_TRUE(dormantEffects.effects.replace({dormant}));
    sound->fieldGroups.effects.shared = {FieldGroupMode::Inherit, dormantEffects};
    auto busEffect = shared;
    busEffect.guid = QUuid::createUuid();
    auto* busMix = project.busses.front().rootItem->comp<VMixNode>();
    ASSERT_TRUE(busMix->effects.replace({busEffect}));
    busMix->effectPlatforms[HarkenTest::PlatformIds::Android] = EffectChainLedger{};
    busEffect.bypass = true;
    ASSERT_TRUE(busMix->effectPlatforms[HarkenTest::PlatformIds::Windows].replace({busEffect}));
    const auto identity = canonicalRuntimeIdentityIndex(project);
    ASSERT_TRUE(identity);
    const auto* inheritedIdentity = identity->find(shared.runtimeId());
    ASSERT_NE(nullptr, inheritedIdentity);
    EXPECT_EQ(inheritedIdentity->sourceKey.owningGuid, toFieldGroupGuid(layer->guid));
    const auto* dormantIdentity = identity->find(dormant.runtimeId());
    ASSERT_NE(nullptr, dormantIdentity);
    EXPECT_EQ(dormantIdentity->sourceKey.owningGuid, toFieldGroupGuid(sound->guid));
    const auto source = canonicalRuntimeProjectSourceIndex(project);
    ASSERT_TRUE(source->valid);
    for (const PlatformScope scope : {PlatformScope{}, PlatformScope{HarkenTest::PlatformIds::Windows},
                                    PlatformScope{HarkenTest::PlatformIds::Android}}) {
        const auto groups = buildFieldGroupEffectiveSnapshot(project, toFieldGroupScope(scope));
        ASSERT_TRUE(groups.ok());
        harkenprojectschema::InputNode node;
        QString error;
        ASSERT_TRUE(copyStudioDefinitionPayload(*sound, scope, *source, groups, *identity, &node, &error))
            << error.toStdString();
        EXPECT_EQ(node.outputBusRef, project.busses.front().rootItem->runtimeId());
        EXPECT_EQ(node.signalGraph.nodes.size(), scope == HarkenTest::PlatformIds::Android ? 0u : 1u);
        if (!node.signalGraph.nodes.empty()) {
            EXPECT_EQ(node.signalGraph.nodes.front().effectId, shared.runtimeId());
            EXPECT_EQ(node.signalGraph.nodes.front().flags,
                      scope == HarkenTest::PlatformIds::Windows ? 1u : 0u);
        }
        const auto runtime = canonicalRuntimeProjectContext(project, 0, {}, scope, source);
        ASSERT_TRUE(runtime.ok()) << (runtime.diagnostics.empty() ? "No runtime context"
            : runtime.diagnostics.front().message.toStdString());
        ASSERT_EQ(runtime.busResult->effects.size(), scope == HarkenTest::PlatformIds::Android ? 0u : 1u);
        if (!runtime.busResult->effects.empty()) {
            EXPECT_EQ(runtime.busResult->effects.front().effectId, busEffect.runtimeId());
            EXPECT_EQ(runtime.busResult->effects.front().flags,
                      scope == HarkenTest::PlatformIds::Windows ? 1u : 0u);
        }
        RuntimeCapabilityContext context;
        context.platform = scope;
        context.deferCurrentWorldCompilation = true;
        const auto capability = RuntimeCapabilityMatrix::evaluateForContext(*sound, project,
            RuntimeCapabilityOperation::PackBuild, RuntimeCapabilityFeature::TypedEffect, {}, context);
        EXPECT_EQ(capability.state, RuntimeCapabilityState::Ready);
    }
}
