
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "BindingRuntimeLowering.h"
#include "Support/RuntimeIdentityTestUtils.h"
#include "Components/VMixNode.h"
#include "Components/VVariable.h"
#include "CurrentWorldProjectAdapter.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"

#include <harken/param/ParamTargets.h>
#include <harken/param/ParamTargetTable.h>

#include <gtest/gtest.h>

#include <QJsonObject>
#include <QJsonValue>
#include <QUuid>

#include <memory>
#include <vector>

using namespace VNX;

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

}

TEST(BindingRuntimeLowering, StoredXMapsToAbsoluteVariableDomain)
{
    const QVector<CurvePointDef> stored{{0.0f, -12.0f}, {0.5f, -6.0f}, {1.0f, 0.0f}};
    const auto mapped = BindingRuntimeLoweringSemantics::storedCurveToEngineX(
        stored, 0.0f, 100.0f);
    ASSERT_EQ(mapped.size(), 3);
    EXPECT_FLOAT_EQ(mapped[0].x, 0.0f);
    EXPECT_FLOAT_EQ(mapped[1].x, 50.0f);
    EXPECT_FLOAT_EQ(mapped[2].x, 100.0f);
    EXPECT_FLOAT_EQ(mapped[1].value, -6.0f);
}

TEST(BindingRuntimeLowering, DegenerateRangeKeepsStoredXAtRangeMin)
{
    const QVector<CurvePointDef> stored{{0.0f, 0.0f}, {1.0f, 1200.0f}};
    const auto mapped = BindingRuntimeLoweringSemantics::storedCurveToEngineX(
        stored, 7.0f, 7.0f);
    ASSERT_EQ(mapped.size(), 2);
    EXPECT_FLOAT_EQ(mapped[0].x, 7.0f);
    EXPECT_FLOAT_EQ(mapped[1].x, 7.0f);
    EXPECT_FLOAT_EQ(mapped[1].value, 1200.0f);
}

TEST(BindingRuntimeLowering, AuthoredVoiceBindingAttachesToMixNodeHost)
{
    ProjectData project;
    auto master = std::make_shared<ProjectItem>(
        QStringLiteral("Master"), ProjectItem::Type::Bus);
    master->objectID = project.generateObjectID();
    master->guid = VNX::authoringUuid(harkentest::StudioObjectId(0x4d4153544552));
    master->attachDefaultComponents();
    auto layer = makeItem(project, ProjectItem::Type::Layer, QStringLiteral("Layer"));
    layer->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("Leaf"));
    auto variable = makeItem(project, ProjectItem::Type::Variable, QStringLiteral("Drive"));
    attach(layer, sound);
    bindRoot(project, master, project.busses, QStringLiteral("Busses"));
    bindRoot(project, layer, project.layers, QStringLiteral("Layers"));
    bindRoot(project, variable, project.variables, QStringLiteral("Variables"));

    auto* variableComp = variable->comp<VVariable>();
    ASSERT_NE(variableComp, nullptr);
    variableComp->kind = VariableKind::Smooth;
    variableComp->rangeMin = 0.0f;
    variableComp->rangeMax = 10.0f;
    variableComp->defaultValue = 0.0f;

    ParameterBindingDef binding;
    binding.guid = QUuid::createUuid();
    binding.variable = variable->guid;
    binding.address = BindingSemantics::volumeAddress();
    binding.curve = {{0.0f, -12.0f}, {1.0f, 0.0f}};
    binding.outputMin = -12.0f;
    binding.outputMax = 0.0f;
    ASSERT_TRUE(layer->comp<VMixNode>()->bindings.upsert(binding));
    const harkenstudio::ObjectId stableId = layer->comp<VMixNode>()->bindings.entries().front().runtimeId();

    const BindingRuntimeLowering lowering =
        BindingRuntimeLoweringSemantics::lowerProject(project);
    ASSERT_EQ(lowering.programs.size(), 1u);
    EXPECT_EQ(lowering.programs.front().bindingId, stableId);
    EXPECT_EQ(lowering.programs.front().variableId, variable->runtimeId());
    EXPECT_EQ(lowering.programs.front().targetScopeKind, harken::kTargetScopeVoice);
    EXPECT_EQ(lowering.programs.front().targetId, harken::kTargetVoiceVolume);
    ASSERT_EQ(lowering.programs.front().points.size(), 2);
    EXPECT_FLOAT_EQ(lowering.programs.front().points[0].x, 0.0f);
    EXPECT_FLOAT_EQ(lowering.programs.front().points[1].x, 10.0f);
    ASSERT_EQ(lowering.attachments.size(), 1u);
    EXPECT_EQ(lowering.attachments.front().objectId, layer->runtimeId());
    EXPECT_EQ(lowering.attachments.front().bindingId, stableId);

    QString error;
    auto request = buildCurrentWorldSyncRequest(project, std::nullopt, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    EXPECT_FALSE(sound->playablePropertyPresence.bits & (UINT64_C(1) << 14));
}

TEST(BindingRuntimeLowering, BusVolumeUsesBusTargetAndDoesNotAttachToVoiceLeaf)
{
    ProjectData project;
    auto bus = makeItem(project, ProjectItem::Type::Bus, QStringLiteral("Music"));
    auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("Leaf"));
    auto variable = makeItem(project, ProjectItem::Type::Variable, QStringLiteral("Mix"));
    bindRoot(project, bus, project.busses, QStringLiteral("Busses"));
    bindRoot(project, sound, project.layers, QStringLiteral("Layers"));
    bindRoot(project, variable, project.variables, QStringLiteral("Variables"));

    ParameterBindingDef binding;
    binding.guid = QUuid::createUuid();
    binding.variable = variable->guid;
    binding.address = BindingSemantics::volumeAddress(ProjectItemType::Bus);
    binding.curve = {{0.0f, -6.0f}, {1.0f, 0.0f}};
    binding.outputMin = -6.0f;
    binding.outputMax = 0.0f;
    ASSERT_TRUE(bus->comp<VMixNode>()->bindings.upsert(binding));

    const BindingRuntimeLowering lowering =
        BindingRuntimeLoweringSemantics::lowerProject(project);
    ASSERT_EQ(lowering.programs.size(), 1u);
    EXPECT_EQ(lowering.programs.front().targetScopeKind, harken::kTargetScopeBus);
    EXPECT_EQ(lowering.programs.front().targetId, harken::kBusTargetBusVolume);
    ASSERT_EQ(lowering.attachments.size(), 1u);
    EXPECT_EQ(lowering.attachments.front().objectId, bus->runtimeId());
}

TEST(BindingRuntimeLowering, DefaultTransitionMsLowersForVolumeAndPitch)
{
    ProjectData project;
    auto bus = makeItem(project, ProjectItem::Type::Bus, QStringLiteral("Music"));
    auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("Leaf"));
    auto variable = makeItem(project, ProjectItem::Type::Variable, QStringLiteral("Drive"));
    bindRoot(project, bus, project.busses, QStringLiteral("Busses"));
    bindRoot(project, sound, project.layers, QStringLiteral("Layers"));
    bindRoot(project, variable, project.variables, QStringLiteral("Variables"));

    auto voiceVolume = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), variable->guid);
    voiceVolume.guid = QUuid::createUuid();
    auto voicePitch = BindingSemantics::defaultBinding(
        BindingSemantics::pitchAddress(), variable->guid);
    voicePitch.guid = QUuid::createUuid();
    auto busVolume = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(ProjectItemType::Bus), variable->guid);
    busVolume.guid = QUuid::createUuid();
    EXPECT_EQ(voiceVolume.transitionMs, 20u);
    EXPECT_EQ(voicePitch.transitionMs, 20u);
    EXPECT_EQ(busVolume.transitionMs, 20u);
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(voiceVolume));
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(voicePitch));
    ASSERT_TRUE(bus->comp<VMixNode>()->bindings.upsert(busVolume));

    const BindingRuntimeLowering lowering =
        BindingRuntimeLoweringSemantics::lowerProject(project);
    ASSERT_EQ(lowering.programs.size(), 3u);
    bool sawVoiceVolume = false;
    bool sawVoicePitch = false;
    bool sawBusVolume = false;
    for (const BindingRuntimeProgram& program : lowering.programs) {
        EXPECT_EQ(program.transitionMs, 20u);
        if (program.targetScopeKind == harken::kTargetScopeVoice
            && program.targetId == harken::kTargetVoiceVolume)
            sawVoiceVolume = true;
        if (program.targetScopeKind == harken::kTargetScopeVoice
            && program.targetId == harken::kTargetVoicePitch)
            sawVoicePitch = true;
        if (program.targetScopeKind == harken::kTargetScopeBus
            && program.targetId == harken::kBusTargetBusVolume)
            sawBusVolume = true;
    }
    EXPECT_TRUE(sawVoiceVolume);
    EXPECT_TRUE(sawVoicePitch);
    EXPECT_TRUE(sawBusVolume);
}

TEST(BindingRuntimeLowering, ExplicitZeroTransitionMsStaysZero)
{
    ProjectData project;
    auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("Leaf"));
    auto variable = makeItem(project, ProjectItem::Type::Variable, QStringLiteral("Drive"));
    bindRoot(project, sound, project.layers, QStringLiteral("Layers"));
    bindRoot(project, variable, project.variables, QStringLiteral("Variables"));

    auto binding = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), variable->guid);
    binding.guid = QUuid::createUuid();
    binding.transitionMs = 0;
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(binding));

    const BindingRuntimeLowering lowering =
        BindingRuntimeLoweringSemantics::lowerProject(project);
    ASSERT_EQ(lowering.programs.size(), 1u);
    EXPECT_EQ(lowering.programs.front().transitionMs, 0u);
}

TEST(BindingRuntimeLowering, IllegalTransitionMsFailsWholeLower)
{
    const QJsonValue illegals[] = {
        QJsonValue(-1),
        QJsonValue(20.5),
        QJsonValue(QStringLiteral("20")),
        QJsonValue(60001),
    };
    for (const QJsonValue& illegal : illegals) {
        ProjectData project;
        auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("Leaf"));
        auto variable = makeItem(project, ProjectItem::Type::Variable, QStringLiteral("Drive"));
        bindRoot(project, sound, project.layers, QStringLiteral("Layers"));
        bindRoot(project, variable, project.variables, QStringLiteral("Variables"));

        auto legal = BindingSemantics::defaultBinding(
            BindingSemantics::pitchAddress(), variable->guid);
        legal.guid = QUuid::createUuid();
        auto volume = BindingSemantics::defaultBinding(
            BindingSemantics::volumeAddress(), variable->guid);
        volume.guid = QUuid::createUuid();
        QJsonObject obj = volume.toJson().toObject();
        obj.insert(QStringLiteral("transitionMs"), illegal);
        const ParameterBindingDef loaded = ParameterBindingDef::fromJson(obj);
        EXPECT_FALSE(loaded.typed);
        ASSERT_TRUE(sound->comp<VMixNode>()->bindings.replace({loaded, legal}));

        const BindingRuntimeLowering lowering =
            BindingRuntimeLoweringSemantics::lowerProject(project);
        EXPECT_TRUE(lowering.failed);
        EXPECT_TRUE(lowering.programs.empty());
        EXPECT_TRUE(lowering.attachments.empty());
    }
}

TEST(BindingRuntimeLowering, MissingTransitionMsStillLowersTwenty)
{
    ProjectData project;
    auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("Leaf"));
    auto variable = makeItem(project, ProjectItem::Type::Variable, QStringLiteral("Drive"));
    bindRoot(project, sound, project.layers, QStringLiteral("Layers"));
    bindRoot(project, variable, project.variables, QStringLiteral("Variables"));

    auto volume = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), variable->guid);
    volume.guid = QUuid::createUuid();
    QJsonObject obj = volume.toJson().toObject();
    obj.remove(QStringLiteral("transitionMs"));
    const ParameterBindingDef loaded = ParameterBindingDef::fromJson(obj);
    EXPECT_TRUE(loaded.typed);
    EXPECT_EQ(loaded.transitionMs, 20u);
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.replace({loaded}));

    const BindingRuntimeLowering lowering =
        BindingRuntimeLoweringSemantics::lowerProject(project);
    EXPECT_FALSE(lowering.failed);
    ASSERT_EQ(lowering.programs.size(), 1u);
    EXPECT_EQ(lowering.programs.front().transitionMs, 20u);
}

TEST(BindingRuntimeLowering, AddingThenRemovingBindingDoesNotDriftRemainingId)
{
    ProjectData project;
    auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("Leaf"));
    auto first = makeItem(project, ProjectItem::Type::Variable, QStringLiteral("A"));
    auto second = makeItem(project, ProjectItem::Type::Variable, QStringLiteral("B"));
    bindRoot(project, sound, project.layers, QStringLiteral("Layers"));
    bindRoot(project, first, project.variables, QStringLiteral("Variables"));
    bindRoot(project, second, project.variables, QStringLiteral("Variables"));

    ParameterBindingDef a;
    a.guid = QUuid::createUuid();
    a.variable = first->guid;
    a.address = BindingSemantics::volumeAddress();
    a.curve = {{0.0f, -12.0f}, {1.0f, 0.0f}};
    a.outputMin = -12.0f;
    a.outputMax = 0.0f;
    ParameterBindingDef b;
    b.guid = QUuid::createUuid();
    b.variable = second->guid;
    b.address = BindingSemantics::pitchAddress();
    b.curve = {{0.0f, 0.0f}, {1.0f, 1200.0f}};
    b.outputMin = 0.0f;
    b.outputMax = 1200.0f;
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(a));
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(b));
    const harkenstudio::ObjectId pitchId = sound->comp<VMixNode>()->bindings.entries().back().runtimeId();
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.remove(
        sound->comp<VMixNode>()->bindings.entries().front().runtimeId()));

    const BindingRuntimeLowering lowering =
        BindingRuntimeLoweringSemantics::lowerProject(project);
    ASSERT_EQ(lowering.programs.size(), 1u);
    EXPECT_EQ(lowering.programs.front().bindingId, pitchId);
    EXPECT_EQ(lowering.programs.front().targetId, harken::kTargetVoicePitch);
}

TEST(BindingRuntimeLowering, AncestorCollectionMatchesLowering)
{
    ProjectData project;
    auto layer = makeItem(project, ProjectItem::Type::Layer, QStringLiteral("Layer"));
    auto folderish = makeItem(project, ProjectItem::Type::None, QStringLiteral("Folder"));
    auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("Leaf"));
    auto variable = makeItem(project, ProjectItem::Type::Variable, QStringLiteral("Drive"));
    attach(layer, folderish);
    attach(folderish, sound);
    bindRoot(project, layer, project.layers, QStringLiteral("Layers"));
    bindRoot(project, variable, project.variables, QStringLiteral("Variables"));

    ParameterBindingDef layerBinding;
    layerBinding.guid = QUuid::createUuid();
    layerBinding.variable = variable->guid;
    layerBinding.address = BindingSemantics::volumeAddress();
    layerBinding.curve = {{0.0f, -12.0f}, {1.0f, 0.0f}};
    layerBinding.outputMin = -12.0f;
    layerBinding.outputMax = 0.0f;
    ASSERT_TRUE(layer->comp<VMixNode>()->bindings.upsert(layerBinding));

    ParameterBindingDef folderBinding = layerBinding;
    folderBinding.guid = QUuid::createUuid();
    ASSERT_TRUE(folderish->compOrCreate<VMixNode>()->bindings.upsert(folderBinding));

    std::vector<const ParameterBindingDef*> ancestors;
    BindingRuntimeLoweringSemantics::collectAncestorBindings(
        *sound, BindingHostClass::Voice, &ancestors);
    ASSERT_EQ(ancestors.size(), 1u);
    EXPECT_EQ(ancestors.front()->runtimeId(), layerBinding.runtimeId());

    const BindingRuntimeLowering lowering =
        BindingRuntimeLoweringSemantics::lowerProject(project);
    ASSERT_EQ(lowering.programs.size(), 1u);
    EXPECT_EQ(lowering.programs.front().bindingId, layerBinding.runtimeId());
    ASSERT_EQ(lowering.attachments.size(), 1u);
    EXPECT_EQ(lowering.attachments.front().objectId, layer->runtimeId());
}

TEST(BindingRuntimeLowering, TransientRegisterFailureKeepsPreviousLiveBinding)
{
    BindingRuntimeLowering desired;
    BindingRuntimeProgram program;
    program.bindingId = harkentest::StudioObjectId(7);
    desired.programs.push_back(program);
    BindingRuntimeAttachment attachment;
    attachment.bindingId = harkentest::StudioObjectId(7);
    attachment.scopeKind = 0;
    attachment.objectId = harkentest::StudioObjectId(11);
    desired.attachments.push_back(attachment);

    PreviewBindingSyncState previous;
    previous.ids = {harkentest::StudioObjectId(7)};
    previous.attachments = {attachment};

    const PreviewBindingSyncState next = BindingRuntimeLoweringSemantics::reconcilePreviewSync(
        desired, previous, {}, {});
    ASSERT_EQ(next.ids.size(), 1u);
    EXPECT_EQ(next.ids.front(), harkentest::StudioObjectId(7u));
    ASSERT_EQ(next.attachments.size(), 1u);
    EXPECT_TRUE(BindingRuntimeLoweringSemantics::sameAttachment(
        next.attachments.front(), attachment));
}

TEST(BindingRuntimeLowering, RemovedBindingIsNotCarriedForwardOnFailure)
{
    BindingRuntimeLowering desired;
    PreviewBindingSyncState previous;
    previous.ids = {harkentest::StudioObjectId(9)};
    previous.attachments = {{harkentest::StudioObjectId(9), 0, harkentest::StudioObjectId(4)}};

    const PreviewBindingSyncState next = BindingRuntimeLoweringSemantics::reconcilePreviewSync(
        desired, previous, {}, {});
    EXPECT_TRUE(next.ids.empty());
    EXPECT_TRUE(next.attachments.empty());
}

TEST(BindingRuntimeLowering, TypedLedgerRangeRefusalPrecedesIncompleteCurveSkip)
{
    ProjectData project;
    auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("Leaf"));
    auto variable = makeItem(project, ProjectItem::Type::Variable, QStringLiteral("Drive"));
    bindRoot(project, sound, project.layers, QStringLiteral("Layers"));
    bindRoot(project, variable, project.variables, QStringLiteral("Variables"));
    auto binding = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), variable->guid);
    binding.guid = QUuid::createUuid();
    binding.transitionMs = 60000;
    auto& ledger = sound->comp<VMixNode>()->bindings;
    ASSERT_TRUE(ledger.upsert(binding));
    auto lowered = BindingRuntimeLoweringSemantics::lowerProject(project);
    ASSERT_FALSE(lowered.failed);
    ASSERT_EQ(lowered.programs.size(), 1u);
    EXPECT_EQ(lowered.programs[0].transitionMs, 60000u);

    for (uint32_t invalid : {60001u, UINT32_MAX}) {
        auto* edited = ledger.find(binding.runtimeId());
        ASSERT_NE(edited, nullptr);
        edited->transitionMs = invalid;
        lowered = BindingRuntimeLoweringSemantics::lowerProject(project);
        EXPECT_TRUE(lowered.failed);
        EXPECT_TRUE(lowered.programs.empty());
        EXPECT_TRUE(lowered.attachments.empty());
        edited->curve.clear();
        lowered = BindingRuntimeLoweringSemantics::lowerProject(project);
        EXPECT_TRUE(lowered.failed);
        EXPECT_TRUE(lowered.programs.empty());
        ASSERT_TRUE(ledger.replace({binding}));
    }
    auto* edited = ledger.find(binding.runtimeId());
    edited->curve.clear();
    EXPECT_FALSE(BindingRuntimeLoweringSemantics::lowerProject(project).failed);
}

TEST(BindingRuntimeLowering, OpaqueIllegalTransitionFailsEvenWithIncompleteCurve)
{
    ProjectData project;
    auto sound = makeItem(project, ProjectItem::Type::Sound, QStringLiteral("Leaf"));
    bindRoot(project, sound, project.layers, QStringLiteral("Layers"));
    auto binding = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), QUuid::createUuid());
    binding.guid = QUuid::createUuid();
    const QJsonValue invalids[] = {QJsonValue(-1), QJsonValue(0.5), QJsonValue(60001),
        QJsonValue(QStringLiteral("20")), QJsonValue(true), QJsonValue(QJsonValue::Null)};
    for (const auto& invalid : invalids) {
        auto json = binding.toJson().toObject();
        json.remove(QStringLiteral("curve"));
        json.insert(QStringLiteral("transitionMs"), invalid);
        const auto opaque = ParameterBindingDef::fromJson(json);
        ASSERT_FALSE(opaque.typed);
        ASSERT_TRUE(sound->comp<VMixNode>()->bindings.replace({opaque}));
        const auto lowered = BindingRuntimeLoweringSemantics::lowerProject(project);
        EXPECT_TRUE(lowered.failed);
        EXPECT_TRUE(lowered.programs.empty());
        EXPECT_TRUE(lowered.attachments.empty());
    }
}
