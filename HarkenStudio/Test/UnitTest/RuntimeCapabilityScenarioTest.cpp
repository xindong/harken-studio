#include "UnitTest/StudioObjectIdTestSupport.h"
#include <harken_project_schema/FrontEnd.h>

#include "Support/RuntimeIdentityTestUtils.h"

#include "Core/Components/VAction.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSound.h"
#include "Core/Components/VVariable.h"
#include "Core/BindingRuntimeLowering.h"
#include "Core/CurrentWorldProjectAdapter.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/RuntimeCapabilityMatrix.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

using namespace VNX;
using namespace HarkenEditor::Bridge;

namespace {

std::shared_ptr<ProjectItem> makeItem(ProjectData& project,
                                      ProjectItem::Type type,
                                      const QString& name)
{
    auto item = std::make_shared<ProjectItem>(name, type);
    item->objectID = project.generateObjectID();
    item->guid = QUuid::createUuid();
    item->guid = QUuid::createUuid();
    item->attachDefaultComponents();
    return item;
}

void attach(const std::shared_ptr<ProjectItem>& parent,
            const std::shared_ptr<ProjectItem>& child)
{
    child->parent = parent.get();
    parent->children.push_back(child);
}

void bindRoot(ProjectData& project, const std::shared_ptr<ProjectItem>& root,
              std::vector<ProjectFileBinding>& bucket, const QString& folder)
{
    ProjectFileBinding binding;
    binding.parentFolder = folder;
    binding.rootItem = root;
    bucket.push_back(std::move(binding));
}

bool hasFeature(const RuntimeCapabilityAssessment& assessment,
                RuntimeCapabilityFeature feature)
{
    return std::any_of(assessment.issues.begin(), assessment.issues.end(),
        [feature](const auto& issue) { return issue.feature == feature; });
}

}

TEST(RuntimeCapabilityScenario,
     CurrentWorldUsesLocalEffectsWhileRetiredGraphAndBindingsStaySeparate)
{
    ProjectData project;
    auto master = makeItem(project, ProjectItem::Type::Bus,
                           QStringLiteral("Master Bus"));
    master->guid = VNX::authoringUuid(harkentest::StudioObjectId(0x4d4153544552));
    bindRoot(project, master, project.busses, QStringLiteral("Busses"));
    auto layer = makeItem(project, ProjectItem::Type::Layer, QStringLiteral("Layer"));
    layer->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    auto container = makeItem(project, ProjectItem::Type::Container, QStringLiteral("Container"));
    auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("Sound"));
    auto variable = makeItem(project, ProjectItem::Type::Variable, QStringLiteral("Intensity"));
    auto action = makeItem(project, ProjectItem::Type::Action, QStringLiteral("SetIntensity"));
    attach(layer, container);
    attach(container, sound);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, sound->guid));
    bindRoot(project, layer, project.layers, QStringLiteral("Layers"));
    bindRoot(project, variable, project.variables, QStringLiteral("Variables"));
    bindRoot(project, action, project.actions, QStringLiteral("Actions"));

    auto* variableComponent = variable->comp<VVariable>();
    ASSERT_NE(variableComponent, nullptr);
    variableComponent->kind = VariableKind::Smooth;
    variableComponent->rangeMin = 0.0f;
    variableComponent->rangeMax = 1.0f;
    variableComponent->defaultValue = 0.25f;

    ParameterBindingDef binding;
    binding.guid = QUuid::createUuid();
    binding.variable = variable->guid;
    binding.address = BindingSemantics::volumeAddress();
    binding.curve = {{0.0f, -24.0f}, {1.0f, 0.0f}};
    binding.outputMin = -24.0f;
    binding.outputMax = 0.0f;
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(binding));
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*sound);
    ASSERT_TRUE(locator.has_value());
    const QJsonArray signalGraph{QJsonObject{
        {QStringLiteral("nodeId"), 1},
        {QStringLiteral("effectType"), 2},
        {QStringLiteral("params"), QJsonArray{QJsonObject{
             {QStringLiteral("paramId"), 0},
             {QStringLiteral("value"), -3.0}}}}}};
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        *sound, *locator, 16, signalGraph, std::nullopt));

    VActionStep setValue;
    setValue.type = VActionStep::Type::SetVariable;
    setValue.targetObjectID = variable->objectID;
    setValue.targetName = variable->name;
    setValue.value = 0.75f;
    action->comp<VAction>()->steps.push_back(setValue);

    QString error;
    auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    ASSERT_FALSE(request->project.nodes.empty());
    for (const auto& node : request->project.nodes) {
        EXPECT_TRUE(node.hasSignalGraph);
        EXPECT_TRUE(node.presence.Has(
            harkenplayableproperties::PlayablePropertyIdV1::SignalGraph));
        EXPECT_TRUE(node.signalGraph.nodes.empty());
        EXPECT_TRUE(node.signalGraph.edges.empty());
    }
    ASSERT_EQ(request->variables.size(), 1u);
    EXPECT_EQ(request->variables.front().variableId, variable->runtimeId());
    ASSERT_EQ(request->containers.size(), 1u);
    EXPECT_EQ(request->containers.front().containerStableId,
              container->runtimeId());
    const auto bindings = BindingRuntimeLoweringSemantics::lowerProject(project);
    ASSERT_EQ(bindings.programs.size(), 1u);
    EXPECT_EQ(bindings.programs.front().bindingId, binding.runtimeId());
    ASSERT_EQ(bindings.attachments.size(), 1u);
    EXPECT_EQ(bindings.attachments.front().objectId, sound->runtimeId());
    EXPECT_EQ(bindings.attachments.front().bindingId, binding.runtimeId());

    const auto frontEnd = harkenprojectschema::CompileFrontEnd(request->project);
    ASSERT_TRUE(frontEnd.ok());
    ASSERT_TRUE(frontEnd.project.has_value());
    ASSERT_TRUE(frontEnd.resolution.has_value());
    ASSERT_TRUE(frontEnd.sends.has_value());
    ASSERT_TRUE(frontEnd.effectChains.has_value());
    for (auto& materialization : request->materializations)
        if (materialization.kind == harkenplayableproperties::ContentPropertyKindV1::Sound)
            materialization.contentRevision = 1;
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        *frontEnd.project, request->variables, request->materializations,
        request->containers);
    EXPECT_TRUE(compiled.ok());

    const auto actionAssessment = RuntimeCapabilityMatrix::assess(
        *action, project, RuntimeCapabilityOperation::ActionRegistration);
    EXPECT_TRUE(actionAssessment.permitted());
    EXPECT_FALSE(hasFeature(actionAssessment,
                            RuntimeCapabilityFeature::ActionSetVariable));
}

TEST(RuntimeCapabilityScenario, StableIdentitySurvivesRenameAndDeletedIdentityDoesNotResolve)
{
    ProjectData project;
    auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("BeforeRename"));
    bindRoot(project, sound, project.layers, QStringLiteral("Layers"));

    const harkenstudio::ObjectId stableId = sound->runtimeId();
    sound->name = QStringLiteral("AfterRename");
    EXPECT_EQ(sound->runtimeId(), stableId);
    EXPECT_EQ(RuntimeCapabilityMatrix::assess(
                  project, stableId, RuntimeCapabilityOperation::LiveSync)
                  .rootStableId,
              stableId);

    project.layers.clear();
    const auto deleted = RuntimeCapabilityMatrix::assess(
        project, stableId, RuntimeCapabilityOperation::LiveSync);
    EXPECT_FALSE(deleted.permitted());
    EXPECT_EQ(deleted.state, RuntimeCapabilityState::InvalidAuthoring);

    auto replacement = makeItem(project, ProjectItem::Type::Sound,
                                QStringLiteral("Replacement"));
    bindRoot(project, replacement, project.layers, QStringLiteral("Layers"));
    EXPECT_NE(replacement->runtimeId(), stableId);
}

TEST(RuntimeCapabilityScenario, UnsupportedAuthoredFeaturesStayExplicitlyGated)
{
    using F = RuntimeCapabilityFeature;
    using O = RuntimeCapabilityOperation;
    using S = RuntimeCapabilityState;
    using T = ProjectItem::Type;
    const auto clipDuration = RuntimeCapabilityMatrix::query(
        T::Track, O::ProjectInputCompile, F::ClipAuthoredDuration);
    ASSERT_TRUE(clipDuration.has_value());
    EXPECT_EQ(clipDuration->state, S::Ready);
    const struct Row { T type; O operation; F feature; } rows[] = {
        {T::Track, O::ProjectInputCompile, F::ClipTargetTrack},
        {T::Attenuation, O::Delete, F::AttenuationUnregister},
    };
    for (const auto& row : rows) {
        const auto verdict = RuntimeCapabilityMatrix::query(
            row.type, row.operation, row.feature);
        ASSERT_TRUE(verdict.has_value());
        EXPECT_EQ(verdict->state, S::UnavailableEngineContract);
        EXPECT_FALSE(verdict->diagnostic.isEmpty());
    }

    ProjectData project;
    auto master = makeItem(project, T::Bus, QStringLiteral("Master"));
    master->guid = VNX::authoringUuid(harkentest::StudioObjectId(0x4d4153544552));
    bindRoot(project, master, project.busses, QStringLiteral("Busses"));
    auto action = makeItem(project, T::Action, QStringLiteral("GatedAction"));
    bindRoot(project, action, project.actions, QStringLiteral("Actions"));
    VActionStep step;
    step.type = VActionStep::Type::Play;
    step.quantize.timing = QuantizeTiming::NextMarker;
    step.quantize.ifSilent = QuantizeIfSilent::Drop;
    action->comp<VAction>()->steps.push_back(step);
    const auto assessment = RuntimeCapabilityMatrix::assess(
        *action, project, O::ActionRegistration);
    EXPECT_FALSE(assessment.permitted());
    EXPECT_FALSE(hasFeature(assessment, F::ActionReset));
    EXPECT_EQ(assessment.state, S::InvalidAuthoring);
    ASSERT_EQ(assessment.issues.size(), 1u);
    EXPECT_TRUE(assessment.issues.front().diagnostic.contains(QStringLiteral("target")));
}
