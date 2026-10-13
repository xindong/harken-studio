#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"
#include "Widgets/Inspector/FieldGroupInspectorView.h"

#include "Components/VTrack.h"
#include "ProjectTypes.h"
#include "FieldGroupJson.h"
#include "FieldGroupProjectAdapter.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>

#include <array>
#include <memory>

namespace VNX {
namespace {

using Group = FieldGroupId;
using Mode = FieldGroupMode;
using Guid = harkenprojectschema::AuthoringGuid;

constexpr Guid guid(std::uint64_t value)
{
    return {0, value};
}

std::shared_ptr<ProjectItem> item(std::uint64_t id, ProjectItem::Type type)
{
    auto result = std::make_shared<ProjectItem>(QString::number(id), type);
    result->guid = fromFieldGroupGuid(guid(id));
    result->objectID = static_cast<uint32_t>(id);
    return result;
}

void own(const std::shared_ptr<ProjectItem>& parent, const std::shared_ptr<ProjectItem>& child)
{
    parent->children.push_back(child);
    child->parent = parent.get();
}

template <typename Payload>
GroupState<Payload> local(Payload payload)
{
    return {Mode::Local, std::move(payload)};
}

PlatformContext shared()
{
    return {};
}

PlatformContext platform(harkenprojectschema::AuthoringGuid value)
{
    const auto id = fromFieldGroupGuid(value);
    return {{id}, id, false};
}

PlatformContext allPlatforms()
{
    return {{HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Windows, true};
}

struct World {
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    std::shared_ptr<ProjectItem> layer = item(1, ProjectItem::Type::Layer);
    std::shared_ptr<ProjectItem> sound = item(2, ProjectItem::Type::Sound);
    std::shared_ptr<ProjectItem> master = item(100, ProjectItem::Type::Bus);
    std::shared_ptr<ProjectItem> busA = item(101, ProjectItem::Type::Bus);
    std::shared_ptr<ProjectItem> busB = item(102, ProjectItem::Type::Bus);
    std::shared_ptr<ProjectItem> attenuation = item(200, ProjectItem::Type::Attenuation);

    World()
    {
        own(layer, sound);
        own(master, busA);
        own(master, busB);
        project.layers = {{layer, {}}};
        project.busses = {{master, {}}};
        project.attenuationItems = {{attenuation, {}}};
        project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
        layer->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{guid(100), 0.0f});
        layer->fieldGroups.send.shared = local(SendFieldGroup{});
        layer->fieldGroups.spatialRating.shared = local(SpatialRatingFieldGroup{});
    }

    FieldGroupInspectorView view(Group group, PlatformContext context = shared()) const
    {
        return buildFieldGroupInspectorView(project, sound->guid, group, context);
    }
};

TEST(FieldGroupInspectorView, InheritedGroupsResolveTheirParentAndDisableLocalEditing)
{
    for (const auto type : {ProjectItem::Type::Sound, ProjectItem::Type::Container,
                            ProjectItem::Type::Layer, ProjectItem::Type::Track,
                            ProjectItem::Type::Piece}) {
        World world;
        auto definition = item(3, type);
        own(world.layer, definition);
        for (const auto group : {Group::MainOutput, Group::Send, Group::SpatialRating}) {
            const auto view = buildFieldGroupInspectorView(world.project, definition->guid, group, shared());
            ASSERT_TRUE(view.valid) << view.diagnostics.join(';').toStdString();
            EXPECT_TRUE(view.effectiveInherit);
            EXPECT_TRUE(view.canInherit);
            EXPECT_FALSE(view.fieldsEditable);
            EXPECT_FALSE(view.inheritanceMixed);
            EXPECT_FALSE(view.allExplicitLocal);
            EXPECT_TRUE(view.anyUseShared);
            ASSERT_EQ(view.perScope.size(), 2u);
            EXPECT_EQ(view.perScope.front().resolved.requestedMode, Mode::UseShared);
            EXPECT_FALSE(view.perScope.front().resolved.source.isDefault);
            EXPECT_EQ(view.perScope.front().resolved.source.definition, guid(1));

        }
    }
}

TEST(FieldGroupInspectorView, RootDefinitionPreservesEachCheckboxStateWithEditableGroups)
{
    World world;
    for (const auto mode : {Mode::Local, Mode::Inherit}) {
        world.layer->fieldGroups.mainOutput.shared.mode = mode;
        world.layer->fieldGroups.send.shared.mode = mode;
        world.layer->fieldGroups.spatialRating.shared.mode = mode;
        for (const auto context : {shared(), allPlatforms(), platform(HarkenTest::FieldGroupPlatformIds::Android)}) {
            for (const auto group : {Group::MainOutput, Group::Send, Group::SpatialRating}) {
                const auto view = buildFieldGroupInspectorView(world.project, world.layer->guid, group, context);
                ASSERT_TRUE(view.valid) << view.diagnostics.join(';').toStdString();
                EXPECT_FALSE(view.canInherit);
                EXPECT_EQ(view.effectiveInherit, mode == Mode::Inherit);
                EXPECT_TRUE(view.fieldsEditable);
            }
        }
    }
}

TEST(FieldGroupInspectorView, UseSharedContinuesOwnershipParentAtRequestedPlatform)
{
    World world;
    auto otherLayer = item(3, ProjectItem::Type::Layer);
    otherLayer->fieldGroups = world.layer->fieldGroups;
    auto track = item(4, ProjectItem::Type::Track);
    own(otherLayer, track);
    world.project.layers.push_back({otherLayer, {}});
    Clip clip;
    clip.clipGuid = fromFieldGroupGuid(guid(300));
    clip.targetGuid = world.sound->guid;
    track->compOrCreate<VTrack>()->clips.items.push_back(clip);
    world.layer->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{guid(100), -2.0f});
    world.layer->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        local(MainOutputFieldGroup{guid(101), -8.0f});
    otherLayer->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{guid(102), -15.0f});

    const auto view = world.view(Group::MainOutput, platform(HarkenTest::FieldGroupPlatformIds::Android));
    ASSERT_TRUE(view.valid) << view.diagnostics.join(';').toStdString();
    EXPECT_TRUE(view.allUseShared);
    EXPECT_TRUE(view.effectiveInherit);
    EXPECT_FALSE(view.allExplicitLocal);
    EXPECT_EQ(view.field("mainOutputBusRef").value.toUuid(), world.busA->guid);
    EXPECT_FLOAT_EQ(view.field("outputBusVolume").value.toFloat(), -8.0f);
    EXPECT_EQ(view.perScope.front().resolved.source.definition, guid(1));
    EXPECT_EQ(view.perScope.front().resolved.source.scope, HarkenTest::FieldGroupPlatformIds::Android);
    EXPECT_TRUE(view.sourceTooltip(world.project).contains(QStringLiteral("Android: 1 / Android")));
}

TEST(FieldGroupInspectorView, PlatformStorageModesExposeIndependentInheritanceAndEditPermission)
{
    struct Case {
        Mode sharedMode;
        Mode platformMode;
        bool inherited;
        bool writable;
        bool usesShared;
        float volume;
    };
    const std::array cases{
        Case{Mode::Local, Mode::Local, false, true, false, -4.0f},
        Case{Mode::Local, Mode::Inherit, true, false, false, -9.0f},
        Case{Mode::Local, Mode::UseShared, false, true, true, -4.0f},
        Case{Mode::Inherit, Mode::UseShared, true, false, true, -9.0f}};
    for (const auto& test : cases) {
        World world;
        world.layer->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
            local(MainOutputFieldGroup{guid(101), -9.0f});
        world.sound->fieldGroups.mainOutput.shared = {
            test.sharedMode, MainOutputFieldGroup{guid(100), -2.0f}};
        world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
            test.platformMode, MainOutputFieldGroup{guid(102), -4.0f}};
        const auto view = world.view(Group::MainOutput, platform(HarkenTest::FieldGroupPlatformIds::Windows));
        ASSERT_TRUE(view.valid) << view.diagnostics.join(';').toStdString();
        EXPECT_EQ(view.effectiveInherit, test.inherited);
        EXPECT_EQ(view.fieldsEditable, test.writable);
        EXPECT_EQ(view.allUseShared, test.usesShared);
        EXPECT_EQ(view.anyUseShared, test.usesShared);
        EXPECT_FALSE(view.inheritanceMixed);
        EXPECT_FLOAT_EQ(view.field("outputBusVolume").value.toFloat(), test.volume);
    }
}

TEST(FieldGroupInspectorView, AllAggregatesInheritanceAndDisablesFieldsIfAnyPlatformInherits)
{
    World world;
    world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows].mode = Mode::Inherit;
    auto view = world.view(Group::MainOutput, allPlatforms());
    ASSERT_TRUE(view.valid);
    EXPECT_TRUE(view.modesMixed);
    EXPECT_FALSE(view.inheritanceMixed);
    EXPECT_TRUE(view.effectiveInherit);
    EXPECT_TRUE(view.anyUseShared);
    EXPECT_FALSE(view.allUseShared);
    EXPECT_FALSE(view.allExplicitLocal);

    world.sound->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{guid(100), -2.0f});
    view = world.view(Group::MainOutput, allPlatforms());
    ASSERT_TRUE(view.valid);
    EXPECT_TRUE(view.inheritanceMixed);
    EXPECT_TRUE(view.effectiveInherit);
    EXPECT_FALSE(view.fieldsEditable);

    world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        local(MainOutputFieldGroup{guid(101), -12.0f});
    view = world.view(Group::MainOutput, allPlatforms());
    ASSERT_TRUE(view.valid);
    EXPECT_TRUE(view.modesMixed);
    EXPECT_FALSE(view.inheritanceMixed);
    EXPECT_FALSE(view.effectiveInherit);
    EXPECT_TRUE(view.fieldsEditable);
    EXPECT_FALSE(view.field("outputBusVolume").uniform);
    ASSERT_EQ(view.perScope.size(), 2u);
    EXPECT_EQ(view.perScope.front().scope, HarkenTest::FieldGroupPlatformIds::Windows);
}

TEST(FieldGroupInspectorView, AllAggregatesPlatformScalarsAndKeepsCanonicalAliases)
{
    World world;
    world.sound->fieldGroups.spatialRating.shared =
        local(SpatialRatingFieldGroup{0.25f, true, guid(200), 3.0f});
    world.sound->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        local(SpatialRatingFieldGroup{-0.5f, true, guid(200), 2.0f});
    world.sound->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        local(SpatialRatingFieldGroup{0.75f, true, {}, 4.0f});
    const auto view = world.view(Group::SpatialRating, allPlatforms());
    ASSERT_TRUE(view.valid) << view.diagnostics.join(';').toStdString();
    EXPECT_TRUE(view.allExplicitLocal);
    EXPECT_FALSE(view.inheritanceMixed);
    EXPECT_FALSE(view.field("spatialPanning").uniform);
    EXPECT_FALSE(view.field("spatial.panning").uniform);
    EXPECT_FLOAT_EQ(view.field("spatialPanning").value.toFloat(), -0.5f);
    EXPECT_EQ(view.field("panning").value, view.field("spatialPanning").value);
    const auto attenuation = view.field("spatialAttenuationRef");
    EXPECT_FALSE(attenuation.uniform);
    ASSERT_EQ(attenuation.perScope.size(), 2);
    EXPECT_EQ(attenuation.perScope[0].second.toUuid(), world.attenuation->guid);
    EXPECT_FALSE(view.field("spatialAttenuationDistanceScaling").uniform);
    EXPECT_FLOAT_EQ(view.field("spatialAttenuationDistanceScaling").value.toFloat(), 2.0f);
    EXPECT_TRUE(view.field("spatial.is3DEnabled").uniform);
    EXPECT_TRUE(view.field("spatialIs3DEnabled").value.toBool());
    EXPECT_FALSE(view.field("spatialSpread").value.isValid());
    EXPECT_FALSE(view.field("outputBusVolume").value.isValid());
    EXPECT_FALSE(view.field("unknown").value.isValid());
}

TEST(FieldGroupInspectorView, AllRetainsEachEffectiveSendTableForReferenceValidation)
{
    World world;
    const SendFieldGroup sends{{{guid(101), -5.0f}, {guid(102), -8.0f}}};
    world.sound->fieldGroups.send.shared = local(SendFieldGroup{});
    world.sound->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = local(sends);
    world.sound->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = local(SendFieldGroup{});
    auto view = world.view(Group::Send, allPlatforms());
    ASSERT_TRUE(view.valid);
    EXPECT_TRUE(view.allExplicitLocal);
    EXPECT_FALSE(view.sendsUniform);
    EXPECT_FALSE(view.field("sends").uniform);
    ASSERT_EQ(view.field("sends").perScope.size(), 2);
    EXPECT_EQ(view.field("sends").perScope.front().second.toList().size(), 2);
    EXPECT_TRUE(view.field("sends").perScope.back().second.toList().isEmpty());

    world.sound->fieldGroups.send.shared = local(sends);
    view = world.view(Group::Send, allPlatforms());
    ASSERT_TRUE(view.valid);
    EXPECT_TRUE(view.allExplicitLocal);
    EXPECT_FALSE(view.sendsUniform);
    const auto rows = view.field("sends").perScope.front().second.toList();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0].toMap().value("targetBusID").toUuid(), world.busA->guid);
    EXPECT_FLOAT_EQ(rows[1].toMap().value("sendLevel").toFloat(), -8.0f);
}

TEST(FieldGroupInspectorView, ActionTargetsImplicitSharedOrCurrentPlatformAndCaptureHistory)
{
    World world;
    world.sound->fieldGroups.mainOutput.shared.localHistory = MainOutputFieldGroup{guid(999), -7.0f};
    const auto initial = world.sound->fieldGroups;
    const auto sharedView = world.view(Group::MainOutput);
    ASSERT_TRUE(sharedView.valid);
    const auto sharedArgs = sharedView.actionArgs();
    EXPECT_EQ(sharedArgs.value("scope"), QJsonValue("Shared"));
    EXPECT_FALSE(sharedArgs.contains("platform_ids"));
    EXPECT_FALSE(sharedArgs.contains("occurrence_target"));
    EXPECT_EQ(sharedArgs.value("definition_guid"), world.sound->guid.toString(QUuid::WithoutBraces));

    EXPECT_EQ(world.view(Group::MainOutput, allPlatforms()).actionArgs(), sharedArgs);
    const auto platformView = world.view(Group::MainOutput, platform(HarkenTest::FieldGroupPlatformIds::Android));
    ASSERT_TRUE(platformView.valid);
    const auto platformArgs = platformView.actionArgs();
    EXPECT_FALSE(platformArgs.contains("scope"));
    EXPECT_EQ(platformArgs.value("platform_ids").toArray(), (QJsonArray{ProjectPlatformSemantics::idString(HarkenTest::PlatformIds::Android)}));
    EXPECT_EQ(platformArgs.value("group"), QJsonValue("mainOutput"));
    DefinitionFieldGroups decoded;
    std::string error;
    ASSERT_TRUE(ReadFieldGroupsJson(
        QJsonDocument(platformArgs.value("expected_before").toObject()).toJson(QJsonDocument::Compact).toStdString(),
        decoded, error)) << error;
    EXPECT_EQ(decoded, initial);
    world.sound->fieldGroups.mainOutput.shared.localHistory->outputBusVolume = -12.0f;
    EXPECT_EQ(sharedView.expectedBefore, initial);
    EXPECT_EQ(platformView.expectedBefore, initial);
}

TEST(FieldGroupInspectorView, InvalidPlatformContextsExposeNoValuesOrActionArguments)
{
    World world;
    const std::array contexts{
        PlatformContext{{HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Android, false},
        PlatformContext{{HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false},
        PlatformContext{{HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Windows, false},
        PlatformContext{{HarkenTest::PlatformIds::Linux}, HarkenTest::PlatformIds::Linux, false},
        PlatformContext{{HarkenTest::PlatformIds::Universal}, HarkenTest::PlatformIds::Universal, false}};
    for (const auto& context : contexts) {
        const auto view = world.view(Group::MainOutput, context);
        EXPECT_FALSE(view.valid);
        EXPECT_FALSE(view.allExplicitLocal);
        EXPECT_TRUE(view.perScope.empty());
        EXPECT_FALSE(view.diagnostics.isEmpty());
        EXPECT_TRUE(view.actionArgs().isEmpty());
        EXPECT_FALSE(view.field("mainOutputBusRef").value.isValid());
    }
}

TEST(FieldGroupInspectorView, UnsupportedObjectsAndUnknownGroupsCannotAcquireInheritanceControls)
{
    World world;
    auto action = item(300, ProjectItem::Type::Action);
    world.project.actions = {{action, {}}};
    for (const auto definition : {world.master->guid, world.attenuation->guid, action->guid, QUuid::createUuid(), QUuid{}}) {
        const auto view = buildFieldGroupInspectorView(world.project, definition, Group::MainOutput, shared());
        EXPECT_FALSE(view.valid);
        EXPECT_TRUE(view.actionArgs().isEmpty());
    }
    EXPECT_FALSE(world.view(static_cast<Group>(255)).valid);
}

TEST(FieldGroupInspectorView, AllValidatesReferencesAcrossEveryActualPlatform)
{
    World world;
    world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        local(MainOutputFieldGroup{guid(101), -4.0f});
    world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        local(MainOutputFieldGroup{guid(999), -6.0f});
    EXPECT_FALSE(world.view(Group::MainOutput, allPlatforms()).valid);
    EXPECT_TRUE(world.view(Group::MainOutput, platform(HarkenTest::FieldGroupPlatformIds::Windows)).valid);
    const auto view = world.view(Group::MainOutput, platform(HarkenTest::FieldGroupPlatformIds::Android));
    EXPECT_FALSE(view.valid);
    EXPECT_FALSE(view.allExplicitLocal);
    EXPECT_TRUE(view.perScope.empty());
    EXPECT_FALSE(view.diagnostics.isEmpty());
    EXPECT_TRUE(view.actionArgs().isEmpty());
    EXPECT_FALSE(view.field("outputBusVolume").value.isValid());
}

TEST(FieldGroupInspectorView, EffectiveMainOutputSendConflictOnAnotherDefinitionDisablesTheView)
{
    World world;
    auto other = item(3, ProjectItem::Type::Sound);
    own(world.layer, other);
    other->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{guid(101), -3.0f});
    other->fieldGroups.send.shared = local(SendFieldGroup{{{guid(101), -4.0f}}});
    world.sound->fieldGroups.spatialRating.shared = local(SpatialRatingFieldGroup{});
    const auto view = world.view(Group::SpatialRating);
    EXPECT_FALSE(view.valid);
    EXPECT_FALSE(view.allExplicitLocal);
    EXPECT_TRUE(view.perScope.empty());
    EXPECT_TRUE(view.actionArgs().isEmpty());
    EXPECT_TRUE(view.diagnostics.join(';').contains(QStringLiteral("effective Main Output")));
}

}
}
