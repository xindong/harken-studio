#include "Core/Components/VSelection.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"
#include "FieldGroupEditing.h"
#include "FieldGroupJson.h"
#include "FieldGroupProjectAdapter.h"

#include <QJsonDocument>

#include "ProjectTypes.h"
#include "Components/VTrack.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>

namespace VNX {
namespace {

using Guid = harkenprojectschema::AuthoringGuid;
using Group = FieldGroupId;
using Mode = FieldGroupMode;
using Platform = harkenprojectschema::AuthoringGuid;
using Property = harkenplayableproperties::PlayablePropertyIdV1;
using Code = FieldGroupEditDiagnosticCode;

constexpr Guid G(std::uint64_t value)
{
    return {0, value};
}

std::shared_ptr<ProjectItem> item(std::uint64_t id, ProjectItem::Type type)
{
    auto result = std::make_shared<ProjectItem>(QString::number(id), type);
    result->guid = QUuid(QStringLiteral("00000000-0000-0000-0000-%1")
                            .arg(id, 12, 16, QLatin1Char('0')));
    result->objectID = static_cast<uint32_t>(id);
    return result;
}

void own(const std::shared_ptr<ProjectItem>& parent,
         const std::shared_ptr<ProjectItem>& child)
{
    parent->children.push_back(child);
    child->parent = parent.get();
}

template <typename Payload>
GroupState<Payload> local(Payload value)
{
    return {Mode::Local, std::move(value)};
}

FieldGroupEditScopes shared()
{
    return {true, {}};
}

FieldGroupEditScopes platforms(std::initializer_list<Platform> values)
{
    return {false, values};
}

bool hasCode(const std::vector<FieldGroupEditDiagnostic>& diagnostics, Code code)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [code](const auto& diagnostic) { return diagnostic.code == code; });
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
        layer->fieldGroups = MakeRootFieldGroups(G(100));
        own(layer, sound);
        own(master, busA);
        own(master, busB);
        project.layers = {{layer, {}}};
        project.busses = {{master, {}}};
        project.attenuationItems = {{attenuation, {}}};
    }

    FieldGroupEditResult edit(Group group, FieldGroupEditOperation operation,
                                FieldGroupEditScopes scopes = shared())
    {
        return prepareFieldGroupEdit(project,
            {sound->guid, group, std::move(scopes), std::move(operation), {}});
    }
};

TEST(FieldGroupEditing, FieldDifferencesStaySparseBetweenTwoAndroidInstances)
{
    World world;
    const auto highId = HarkenTest::platformId(QStringLiteral("AndroidHigh"));
    const auto low = HarkenTest::FieldGroupPlatformIds::Android;
    const auto high = toFieldGroupGuid(highId);
    world.project.platforms = {
        {HarkenTest::PlatformIds::Android, QStringLiteral("AndroidLow"), BasePlatform::Android},
        {highId, QStringLiteral("AndroidHigh"), BasePlatform::Android}};
    world.sound->fieldGroups.spatialRating.shared = local(SpatialRatingFieldGroup{});
    auto pan = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialPanning, 0.25f}, platforms({low}));
    ASSERT_TRUE(pan.ok());
    world.sound->fieldGroups = *pan.after;
    auto scale = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialAttenuationDistanceScaling, 2.5f}, platforms({high}));
    ASSERT_TRUE(scale.ok());
    world.sound->fieldGroups = *scale.after;
    std::string json;
    std::string error;
    ASSERT_TRUE(WriteFieldGroupsJson(world.sound->fieldGroups, json, error)) << error;
    const auto saved = QJsonDocument::fromJson(QByteArray::fromStdString(json)).object()
        .value("platforms").toObject();
    const auto lowValues = saved.value(HarkenTest::platformIdString("Android")).toObject()
        .value("spatialRating").toObject();
    EXPECT_FALSE(lowValues.contains("inherit"));
    EXPECT_EQ(lowValues.value("values").toObject().keys(), QStringList{"spatialPanning"});
    const auto highValues = saved.value(highId.toString(QUuid::WithoutBraces)).toObject()
        .value("spatialRating").toObject();
    EXPECT_EQ(highValues.value("values").toObject().keys(), QStringList{"spatialAttenuationDistanceScaling"});
    DefinitionFieldGroups reopened;
    ASSERT_TRUE(ReadFieldGroupsJson(json, reopened, error)) << error;
    EXPECT_EQ(reopened, world.sound->fieldGroups);
    world.sound->fieldGroups = reopened;
    auto all = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialPanning, -0.5f});
    ASSERT_TRUE(all.ok());
    EXPECT_FALSE(all.after->spatialRating.platforms.contains(low));
    EXPECT_EQ(all.after->spatialRating.platforms.at(high), reopened.spatialRating.platforms.at(high));
    EXPECT_FLOAT_EQ(all.after->spatialRating.shared.localHistory->spatialPanning, -0.5f);
}

TEST(FieldGroupEditing, AllFieldEditPreservesIndependentInheritFlagsAndOtherFields)
{
    World world;
    auto& group = world.sound->fieldGroups.spatialRating;
    group.shared = {Mode::Inherit, SpatialRatingFieldGroup{}};
    for (const auto id : HarkenTest::fieldGroupPlatforms)
        group.platforms[id] = {Mode::Local, {}};
    auto own = SpatialRatingFieldGroup{};
    own.spatialAttenuationDistanceScaling = 3.7f;
    group.platforms[HarkenTest::FieldGroupPlatformIds::Android].localHistory = own;
    group.platforms[HarkenTest::FieldGroupPlatformIds::Android].localFields =
        FieldGroupFieldMask(Property::SpatialAttenuationDistanceScaling);
    const auto result = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialPanning, 0.3f});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.after->spatialRating.shared.mode, Mode::Inherit);
    for (const auto id : HarkenTest::fieldGroupPlatforms)
        EXPECT_EQ(result.after->spatialRating.platforms.at(id).mode, Mode::Local);
    EXPECT_EQ(result.after->spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Android),
              group.platforms.at(HarkenTest::FieldGroupPlatformIds::Android));
}

TEST(FieldGroupEditing, InheritedResultsRemainDynamicAndNeverReplaceDormantAuthoredValues)
{
    World world;
    const auto android = HarkenTest::FieldGroupPlatformIds::Android;
    auto& parent = world.layer->fieldGroups.spatialRating;
    auto inherited = SpatialRatingFieldGroup{};
    inherited.spatialPanning = 0.8f;
    parent.platforms[android] = {Mode::UseShared, inherited,
        FieldGroupFieldMask(Property::SpatialPanning)};
    auto own = SpatialRatingFieldGroup{};
    own.spatialPanning = -0.4f;
    world.sound->fieldGroups.spatialRating.shared = {Mode::Inherit, own};
    world.sound->fieldGroups.spatialRating.platforms[android] = {
        Mode::UseShared, inherited, FieldGroupFieldMask(Property::SpatialPanning)};
    const auto authored = world.sound->fieldGroups;
    auto snapshot = buildFieldGroupEffectiveSnapshot(world.project, android);
    ASSERT_TRUE(snapshot.ok());
    EXPECT_FLOAT_EQ(snapshot.definitions.at(G(2)).spatialRating.spatialPanning, 0.8f);
    parent.platforms[android].localHistory->spatialPanning = 0.6f;
    snapshot = buildFieldGroupEffectiveSnapshot(world.project, android);
    ASSERT_TRUE(snapshot.ok());
    EXPECT_FLOAT_EQ(snapshot.definitions.at(G(2)).spatialRating.spatialPanning, 0.6f);
    EXPECT_EQ(world.sound->fieldGroups, authored);
    const auto localValues = world.edit(Group::SpatialRating, SetFieldGroupMode{Mode::Local});
    ASSERT_TRUE(localValues.ok());
    world.sound->fieldGroups = *localValues.after;
    EXPECT_EQ(world.sound->fieldGroups.spatialRating.shared.localHistory, authored.spatialRating.shared.localHistory);
    snapshot = buildFieldGroupEffectiveSnapshot(world.project, android);
    ASSERT_TRUE(snapshot.ok());
    EXPECT_FLOAT_EQ(snapshot.definitions.at(G(2)).spatialRating.spatialPanning, 0.8f);
    const auto windows = buildFieldGroupEffectiveSnapshot(world.project, HarkenTest::FieldGroupPlatformIds::Windows);
    ASSERT_TRUE(windows.ok());
    EXPECT_FLOAT_EQ(windows.definitions.at(G(2)).spatialRating.spatialPanning, -0.4f);
}

TEST(FieldGroupEditing, AllCannotEditAFieldWhileAnyInstanceInherits)
{
    World world;
    world.sound->fieldGroups.spatialRating.shared = local(SpatialRatingFieldGroup{});
    world.sound->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {Mode::Inherit, {}};
    const auto before = world.sound->fieldGroups;
    const auto rejected = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialPanning, 0.4f});
    EXPECT_FALSE(rejected.ok());
    EXPECT_TRUE(hasCode(rejected.diagnostics, Code::ReadOnly));
    EXPECT_EQ(world.sound->fieldGroups, before);
    const auto unified = world.edit(Group::SpatialRating, SetFieldGroupMode{Mode::Local});
    ASSERT_TRUE(unified.ok());
    world.sound->fieldGroups = *unified.after;
    EXPECT_TRUE(world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialPanning, 0.4f}).ok());
}

TEST(FieldGroupEditing, ResettingOneFieldPreservesOtherDormantDifferences)
{
    World world;
    const auto android = HarkenTest::FieldGroupPlatformIds::Android;
    world.sound->fieldGroups.spatialRating.shared = local(SpatialRatingFieldGroup{});
    auto pan = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialPanning, 0.2f}, platforms({android}));
    ASSERT_TRUE(pan.ok());
    world.sound->fieldGroups = *pan.after;
    auto scale = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialAttenuationDistanceScaling, 2.7f}, platforms({android}));
    ASSERT_TRUE(scale.ok());
    world.sound->fieldGroups = *scale.after;
    const auto reset = world.edit(Group::SpatialRating,
        ClearFieldGroupFieldOverride{Property::SpatialPanning}, platforms({android}));
    ASSERT_TRUE(reset.ok());
    const auto& own = reset.after->spatialRating.platforms.at(android);
    EXPECT_EQ(own.mode, Mode::UseShared);
    EXPECT_EQ(own.localFields, FieldGroupFieldMask(Property::SpatialAttenuationDistanceScaling));
    EXPECT_FLOAT_EQ(own.localHistory->spatialAttenuationDistanceScaling, 2.7f);
}

TEST(FieldGroupEditing, RootFieldsRemainEditableWithoutChangingInheritanceState)
{
    for (const auto mode : {Mode::Inherit, Mode::Local}) {
        World world;
        world.layer->fieldGroups.mainOutput.shared.mode = mode;
        world.layer->fieldGroups.send.shared.mode = mode;
        world.layer->fieldGroups.spatialRating.shared.mode = mode;
        const auto before = world.layer->fieldGroups;
        const auto output = prepareFieldGroupEdit(world.project,
            {world.layer->guid, Group::MainOutput, shared(),
             SetFieldGroupField{Property::OutputBusVolume, -8.0f}, before});
        ASSERT_TRUE(output.ok());
        EXPECT_EQ(output.after->mainOutput.shared.mode, mode);
        EXPECT_EQ(output.after->mainOutput.shared.localHistory,
                  (MainOutputFieldGroup{G(100), -8.0f}));
        EXPECT_TRUE(output.after->mainOutput.platforms.empty());
        const auto sends = prepareFieldGroupEdit(world.project,
            {world.layer->guid, Group::Send, shared(),
             ReplaceFieldGroupSends{SendFieldGroup{{{G(101), -6.0f}}}}, before});
        ASSERT_TRUE(sends.ok());
        EXPECT_EQ(sends.after->send.shared.mode, mode);
        EXPECT_EQ(sends.after->send.shared.localHistory,
                  (SendFieldGroup{{{G(101), -6.0f}}}));
        const auto spatial = prepareFieldGroupEdit(world.project,
            {world.layer->guid, Group::SpatialRating, platforms({HarkenTest::FieldGroupPlatformIds::Android}),
             SetFieldGroupField{Property::SpatialPanning, 0.5f}, before});
        ASSERT_TRUE(spatial.ok());
        EXPECT_EQ(spatial.after->spatialRating.shared, before.spatialRating.shared);
        ASSERT_EQ(spatial.after->spatialRating.platforms.size(), 1u);
        const auto& platform = spatial.after->spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Android);
        EXPECT_EQ(platform.mode, Mode::UseShared);
        EXPECT_EQ(platform.localFields, FieldGroupFieldMask(Property::SpatialPanning));
        EXPECT_EQ(ResolveFieldGroupLocal(spatial.after->spatialRating, HarkenTest::FieldGroupPlatformIds::Android).mode, mode);
        ASSERT_TRUE(platform.localHistory);
        EXPECT_FLOAT_EQ(platform.localHistory->spatialPanning, 0.5f);
        EXPECT_EQ(world.layer->fieldGroups, before);
    }
}

TEST(FieldGroupEditing, RootInheritanceCommandsAreUnavailableButPlatformResetWorks)
{
    World world;
    const auto before = world.layer->fieldGroups;
    for (const auto group : {Group::MainOutput, Group::Send, Group::SpatialRating}) {
        for (const auto& scopes : {shared(), platforms({HarkenTest::FieldGroupPlatformIds::Android})}) {
            for (const auto operation : {
                     FieldGroupEditOperation{SetFieldGroupMode{Mode::Inherit}},
                     FieldGroupEditOperation{SetFieldGroupMode{Mode::Local}},
                     FieldGroupEditOperation{RebuildLocalFieldGroup{}}}) {
                const auto result = prepareFieldGroupEdit(world.project,
                    {world.layer->guid, group, scopes, operation, before});
                EXPECT_FALSE(result.ok());
                EXPECT_TRUE(hasCode(result.diagnostics, Code::ReadOnly));
                EXPECT_FALSE(result.after);
                EXPECT_EQ(world.layer->fieldGroups, before);
            }
        }
    }
    const auto output = prepareFieldGroupEdit(world.project,
        {world.layer->guid, Group::MainOutput, platforms({HarkenTest::FieldGroupPlatformIds::Android}),
         SetFieldGroupField{Property::OutputBusVolume, -8.0f}, before});
    ASSERT_TRUE(output.ok());
    world.layer->fieldGroups = *output.after;
    const auto reset = prepareFieldGroupEdit(world.project,
        {world.layer->guid, Group::MainOutput, platforms({HarkenTest::FieldGroupPlatformIds::Android}),
         SetFieldGroupMode{Mode::UseShared}, output.after});
    ASSERT_TRUE(reset.ok());
    EXPECT_EQ(reset.after->mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).mode, Mode::UseShared);
    EXPECT_EQ(reset.after->mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).localHistory,
              output.after->mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).localHistory);
    EXPECT_EQ(reset.after->mainOutput.shared, before.mainOutput.shared);
}

TEST(FieldGroupEditing, AllFiveDefinitionKindsCanInitializeTheirThreeGroups)
{
    for (const auto type : {ProjectItem::Type::Layer, ProjectItem::Type::Sound,
                            ProjectItem::Type::Container, ProjectItem::Type::Track,
                            ProjectItem::Type::Piece}) {
        World world;
        auto definition = item(3, type);
        own(world.layer, definition);
        const auto before = definition->fieldGroups;
        for (const auto group : {Group::MainOutput, Group::Send, Group::SpatialRating}) {
            const auto result = prepareFieldGroupEdit(world.project,
                {definition->guid, group, shared(), SetFieldGroupMode{Mode::Local}, {}});
            ASSERT_TRUE(result.ok());
            EXPECT_TRUE(result.changed);
            EXPECT_EQ(result.before, before);
            EXPECT_EQ(definition->fieldGroups, before);
            EXPECT_FALSE(definition->dirtyBit);
            switch (group) {
            case Group::MainOutput:
                EXPECT_EQ(result.after->mainOutput.shared.mode, Mode::Local);
                EXPECT_EQ(result.after->mainOutput.shared.localHistory,
                          (MainOutputFieldGroup{G(100), 0.0f}));
                break;
            case Group::Send:
                EXPECT_EQ(result.after->send.shared.mode, Mode::Local);
                EXPECT_EQ(result.after->send.shared.localHistory, SendFieldGroup{});
                break;
            case Group::SpatialRating:
                EXPECT_EQ(result.after->spatialRating.shared.mode, Mode::Local);
                EXPECT_EQ(result.after->spatialRating.shared.localHistory,
                          SpatialRatingFieldGroup{});
                break;
            }
        }
    }
}

TEST(FieldGroupEditing, InitializingEveryPlatformCopiesItsOwnEffectiveValue)
{
    World world;
    world.layer->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{G(101), -1.0f});
    for (std::size_t i = 0; i < HarkenTest::fieldGroupPlatforms.size(); ++i)
        world.layer->fieldGroups.mainOutput.platforms[HarkenTest::fieldGroupPlatforms[i]] =
            local(MainOutputFieldGroup{G(i % 2 == 0 ? 101 : 102), -2.0f - static_cast<float>(i)});
    const auto before = world.sound->fieldGroups;
    const auto result = world.edit(Group::MainOutput, SetFieldGroupMode{Mode::Local},
        {false, {HarkenTest::fieldGroupPlatforms.begin(), HarkenTest::fieldGroupPlatforms.end()}});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.after->mainOutput.shared, before.mainOutput.shared);
    ASSERT_EQ(result.after->mainOutput.platforms.size(), 5u);
    for (const auto platform : HarkenTest::fieldGroupPlatforms) {
        const auto& state = result.after->mainOutput.platforms.at(platform);
        EXPECT_EQ(state.mode, Mode::Local);
        EXPECT_EQ(state.localHistory,
                  world.layer->fieldGroups.mainOutput.platforms.at(platform).localHistory);
    }
    EXPECT_EQ(world.sound->fieldGroups, before);
    EXPECT_TRUE(world.sound->fieldGroups.mainOutput.platforms.empty());
}

TEST(FieldGroupEditing, LocalRestoresHistoryAndModeChangesPreserveEveryMember)
{
    World world;
    const SpatialRatingFieldGroup history{-0.5f, true, G(200), 3.0f};
    world.sound->fieldGroups.spatialRating.shared.localHistory = history;
    world.layer->fieldGroups.spatialRating.shared = local(SpatialRatingFieldGroup{});
    const auto activated = world.edit(Group::SpatialRating, SetFieldGroupMode{Mode::Local});
    ASSERT_TRUE(activated.ok());
    EXPECT_EQ(activated.after->spatialRating.shared.localHistory, history);
    world.sound->fieldGroups = *activated.after;
    const auto inherited = world.edit(Group::SpatialRating, SetFieldGroupMode{Mode::Inherit});
    ASSERT_TRUE(inherited.ok());
    EXPECT_EQ(inherited.after->spatialRating.shared.mode, Mode::Inherit);
    EXPECT_EQ(inherited.after->spatialRating.shared.localHistory, history);
    world.sound->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = local(history);
    const auto usingShared = world.edit(Group::SpatialRating, SetFieldGroupMode{Mode::UseShared},
                                       platforms({HarkenTest::FieldGroupPlatformIds::Windows}));
    ASSERT_TRUE(usingShared.ok());
    EXPECT_TRUE(usingShared.after->spatialRating.platforms.empty());
    EXPECT_EQ(ResolveFieldGroupLocal(usingShared.after->spatialRating, HarkenTest::FieldGroupPlatformIds::Windows).localHistory, history);
}

TEST(FieldGroupEditing, DormantInvalidReferencesAreRetainedUntilActivation)
{
    World world;
    world.sound->fieldGroups.mainOutput.shared.localHistory = MainOutputFieldGroup{G(999), -8.0f};
    world.sound->fieldGroups.spatialRating.shared.localHistory =
        SpatialRatingFieldGroup{0.25f, true, G(998), 4.0f};
    const auto before = world.sound->fieldGroups;
    EXPECT_TRUE(validateFieldGroupProject(world.project).ok());
    const auto activated = world.edit(Group::MainOutput, SetFieldGroupMode{Mode::Local});
    EXPECT_FALSE(activated.ok());
    EXPECT_FALSE(activated.after);
    EXPECT_FALSE(activated.changed);
    EXPECT_TRUE(hasCode(activated.diagnostics, Code::InvalidReference));
    EXPECT_EQ(world.sound->fieldGroups, before);
    ASSERT_FALSE(activated.diagnostics.empty());
    EXPECT_EQ(activated.diagnostics.front().definition, world.sound->guid);
    EXPECT_EQ(activated.diagnostics.front().field, QStringLiteral("mainOutputBusRef"));
    EXPECT_EQ(activated.diagnostics.front().relatedGuid, G(999));
    const auto rebuilt = world.edit(Group::MainOutput, RebuildLocalFieldGroup{});
    ASSERT_TRUE(rebuilt.ok());
    EXPECT_EQ(rebuilt.after->mainOutput.shared.mode, Mode::Local);
    EXPECT_EQ(rebuilt.after->mainOutput.shared.localHistory,
              (MainOutputFieldGroup{G(100), 0.0f}));
    EXPECT_EQ(rebuilt.after->spatialRating, before.spatialRating);
    EXPECT_EQ(world.sound->fieldGroups, before);
}

TEST(FieldGroupEditing, InvalidHistoryOnOnePlatformRejectsTheWholeActivation)
{
    World world;
    world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::Inherit, MainOutputFieldGroup{G(101), -3.0f}};
    world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        {Mode::UseShared, MainOutputFieldGroup{G(999), -4.0f}};
    const auto before = world.sound->fieldGroups;
    const auto targets = platforms({HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android});
    const auto activated = world.edit(Group::MainOutput, SetFieldGroupMode{Mode::Local}, targets);
    EXPECT_FALSE(activated.after);
    EXPECT_FALSE(activated.changed);
    EXPECT_TRUE(hasCode(activated.diagnostics, Code::InvalidReference));
    EXPECT_EQ(world.sound->fieldGroups, before);
    EXPECT_EQ(activated.before, before);
    const auto rebuilt = world.edit(Group::MainOutput, RebuildLocalFieldGroup{}, targets);
    ASSERT_TRUE(rebuilt.ok());
    for (const auto platform : targets.platforms) {
        EXPECT_EQ(rebuilt.after->mainOutput.platforms.at(platform).mode, Mode::Local);
        EXPECT_EQ(rebuilt.after->mainOutput.platforms.at(platform).localHistory,
                  (MainOutputFieldGroup{G(100), 0.0f}));
    }
    EXPECT_EQ(rebuilt.after->mainOutput.shared, before.mainOutput.shared);
}

TEST(FieldGroupEditing, InheritedFieldsRejectEveryTargetAtomically)
{
    World world;
    const auto field = SetFieldGroupField{Property::OutputBusVolume, -6.0f};
    EXPECT_TRUE(hasCode(world.edit(Group::MainOutput, field).diagnostics, Code::ReadOnly));
    world.sound->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{G(101), -1.0f});
    const auto independent = world.edit(Group::MainOutput, field, platforms({HarkenTest::FieldGroupPlatformIds::Windows}));
    ASSERT_TRUE(independent.ok());
    const auto& overlay = independent.after->mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows);
    EXPECT_EQ(overlay.mode, Mode::UseShared);
    EXPECT_EQ(overlay.localFields, FieldGroupFieldMask(Property::OutputBusVolume));
    EXPECT_EQ(ResolveFieldGroupLocal(independent.after->mainOutput, HarkenTest::FieldGroupPlatformIds::Windows).localHistory,
              (MainOutputFieldGroup{G(101), -6.0f}));
    world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        local(MainOutputFieldGroup{G(102), -2.0f});
    world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android].mode = Mode::Inherit;
    const auto before = world.sound->fieldGroups;
    const auto mixed = world.edit(Group::MainOutput, field,
                                  platforms({HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android}));
    EXPECT_FALSE(mixed.after);
    EXPECT_TRUE(hasCode(mixed.diagnostics, Code::ReadOnly));
    EXPECT_EQ(world.sound->fieldGroups, before);
}

TEST(FieldGroupEditing, FieldEditRetainsOtherOwnValuesIndependentlyOfTheInheritFlag)
{
    World world;
    world.sound->fieldGroups.spatialRating.shared = local(
        SpatialRatingFieldGroup{0.5f, true, G(200), 2.0f});
    world.sound->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::UseShared, SpatialRatingFieldGroup{}};
    const auto result = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialPanning, -0.5f}, platforms({HarkenTest::FieldGroupPlatformIds::Windows}));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.after->spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows),
              (GroupState<SpatialRatingFieldGroup>{Mode::UseShared, SpatialRatingFieldGroup{-0.5f, false, {}, 1.0f}}));
    EXPECT_EQ(result.after->spatialRating.shared, world.sound->fieldGroups.spatialRating.shared);
    EXPECT_EQ(result.before, world.sound->fieldGroups);
    EXPECT_EQ(world.sound->fieldGroups.spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).mode,
              Mode::UseShared);
}

TEST(FieldGroupEditing, FirstPlatformSendEditIsIndependentAndInvalidEditsDoNotCreateOverrides)
{
    World world;
    world.sound->fieldGroups.send.shared = local(SendFieldGroup{{{G(101), -8.0f}}});
    const auto before = world.sound->fieldGroups;
    const auto rejected = world.edit(Group::Send,
        ReplaceFieldGroupSends{SendFieldGroup{{{G(100), -6.0f}}}},
        platforms({HarkenTest::FieldGroupPlatformIds::Windows}));
    EXPECT_FALSE(rejected.ok());
    EXPECT_TRUE(hasCode(rejected.diagnostics, Code::MainOutputSendConflict));
    EXPECT_EQ(world.sound->fieldGroups, before);
    const auto cleared = world.edit(Group::Send,
        ReplaceFieldGroupSends{SendFieldGroup{}}, platforms({HarkenTest::FieldGroupPlatformIds::Windows}));
    ASSERT_TRUE(cleared.ok());
    EXPECT_EQ(cleared.after->send.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows),
              (GroupState<SendFieldGroup>{Mode::UseShared, SendFieldGroup{}}));
    EXPECT_EQ(cleared.after->send.shared, before.send.shared);
    EXPECT_EQ(world.sound->fieldGroups, before);
}

TEST(FieldGroupEditing, EqualPlatformValueUsesSharedStorageAndFollowsLaterSharedEdits)
{
    World world;
    world.sound->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{G(101), -3.0f});
    const auto independent = world.edit(Group::MainOutput,
        SetFieldGroupField{Property::OutputBusVolume, -3.0f}, platforms({HarkenTest::FieldGroupPlatformIds::Windows}));
    ASSERT_TRUE(independent.ok());
    EXPECT_FALSE(independent.changed);
    EXPECT_TRUE(independent.after->mainOutput.platforms.empty());
    world.sound->fieldGroups = *independent.after;
    const auto sharedEdit = world.edit(Group::MainOutput,
        SetFieldGroupField{Property::OutputBusVolume, -9.0f});
    ASSERT_TRUE(sharedEdit.ok());
    EXPECT_EQ(sharedEdit.after->mainOutput.shared, local(MainOutputFieldGroup{G(101), -9.0f}));
    EXPECT_FALSE(sharedEdit.after->mainOutput.platforms.contains(HarkenTest::FieldGroupPlatformIds::Windows));
    EXPECT_EQ(ResolveFieldGroupLocal(sharedEdit.after->mainOutput, HarkenTest::FieldGroupPlatformIds::Windows).localHistory,
              (MainOutputFieldGroup{G(101), -9.0f}));
    EXPECT_FALSE(sharedEdit.after->mainOutput.platforms.contains(HarkenTest::FieldGroupPlatformIds::Android));
}

TEST(FieldGroupEditing, MultiPlatformFieldWritesPreserveEveryOtherLocalMember)
{
    World world;
    world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        local(MainOutputFieldGroup{G(101), -7.0f});
    world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        local(MainOutputFieldGroup{G(102), -13.0f});
    const auto before = world.sound->fieldGroups;
    const auto result = world.edit(Group::MainOutput,
        SetFieldGroupField{Property::MainOutputBusRef, G(100)},
        platforms({HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android}));
    ASSERT_TRUE(result.ok());
    for (const auto platform : {HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android}) {
        const auto& payload = *result.after->mainOutput.platforms.at(platform).localHistory;
        EXPECT_EQ(payload.mainOutputBusRef, G(100));
        EXPECT_EQ(payload.outputBusVolume,
                  before.mainOutput.platforms.at(platform).localHistory->outputBusVolume);
    }
    EXPECT_EQ(result.after->mainOutput.shared, before.mainOutput.shared);
    EXPECT_EQ(world.sound->fieldGroups, before);
}

TEST(FieldGroupEditing, SpatialMemberEditsPreserveTheCompleteRatingGroup)
{
    World world;
    const SpatialRatingFieldGroup original{0.25f, true, G(200), 4.0f};
    world.sound->fieldGroups.spatialRating.shared = local(original);
    const auto scaling = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialAttenuationDistanceScaling, 2.0f});
    ASSERT_TRUE(scaling.ok());
    auto expected = original;
    expected.spatialAttenuationDistanceScaling = 2.0f;
    EXPECT_EQ(scaling.after->spatialRating.shared.localHistory, expected);
    const auto disabled = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialIs3DEnabled, false});
    ASSERT_TRUE(disabled.ok());
    expected = original;
    expected.spatialIs3DEnabled = false;
    EXPECT_EQ(disabled.after->spatialRating.shared.localHistory, expected);
    const auto cleared = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialAttenuationRef, Guid{}});
    ASSERT_TRUE(cleared.ok());
    expected = original;
    expected.spatialAttenuationRef = {};
    EXPECT_EQ(cleared.after->spatialRating.shared.localHistory, expected);
    EXPECT_EQ(world.sound->fieldGroups.spatialRating.shared.localHistory, original);
}

TEST(FieldGroupEditing, ClearingMainOutputSelectsPersistentMasterAndCanBeRestored)
{
    World world;
    world.sound->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{G(101), -12.0f});
    const auto before = world.sound->fieldGroups;
    const auto cleared = world.edit(Group::MainOutput,
        SetFieldGroupField{Property::MainOutputBusRef, Guid{}});
    ASSERT_TRUE(cleared.ok());
    EXPECT_EQ(cleared.after->mainOutput.shared.mode, Mode::Local);
    EXPECT_EQ(cleared.after->mainOutput.shared.localHistory,
              (MainOutputFieldGroup{G(100), -12.0f}));
    world.sound->fieldGroups = *cleared.after;
    const auto restored = prepareFieldGroupRestore(world.project, world.sound->guid,
                                                      before, cleared.after);
    ASSERT_TRUE(restored.ok());
    EXPECT_TRUE(restored.changed);
    EXPECT_EQ(restored.after, before);
    EXPECT_EQ(world.sound->fieldGroups, cleared.after);
    const auto staleRestore = prepareFieldGroupRestore(world.project, world.sound->guid,
                                                         before, before);
    EXPECT_FALSE(staleRestore.after);
    EXPECT_TRUE(hasCode(staleRestore.diagnostics, Code::StaleState));
}

TEST(FieldGroupEditing, InvalidValuesAndReferenceKindsDoNotProduceCandidates)
{
    World world;
    world.sound->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{G(101), -2.0f});
    world.sound->fieldGroups.spatialRating.shared = local(SpatialRatingFieldGroup{});
    const auto before = world.sound->fieldGroups;
    for (const auto value : {-std::numeric_limits<float>::infinity(),
                             std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        const auto result = world.edit(Group::MainOutput,
            SetFieldGroupField{Property::OutputBusVolume, value});
        EXPECT_FALSE(result.after);
        EXPECT_TRUE(hasCode(result.diagnostics, Code::InvalidGroups));
    }
    const auto wrongBus = world.edit(Group::MainOutput,
        SetFieldGroupField{Property::MainOutputBusRef, G(200)});
    EXPECT_FALSE(wrongBus.after);
    EXPECT_TRUE(hasCode(wrongBus.diagnostics, Code::InvalidReference));
    const auto wrongAttenuation = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialAttenuationRef, G(101)});
    EXPECT_FALSE(wrongAttenuation.after);
    EXPECT_TRUE(hasCode(wrongAttenuation.diagnostics, Code::InvalidReference));
    const auto invalidScale = world.edit(Group::SpatialRating,
        SetFieldGroupField{Property::SpatialAttenuationDistanceScaling, 101.0f});
    EXPECT_FALSE(invalidScale.after);
    EXPECT_TRUE(hasCode(invalidScale.diagnostics, Code::InvalidGroups));
    EXPECT_EQ(world.sound->fieldGroups, before);
}

TEST(FieldGroupEditing, MainOutputAndSendCannotResolveToTheSameBus)
{
    World world;
    world.sound->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{G(101), -2.0f});
    world.sound->fieldGroups.send.shared = local(SendFieldGroup{{{G(102), -9.0f}}});
    const auto before = world.sound->fieldGroups;
    const auto outputConflict = world.edit(Group::MainOutput,
        SetFieldGroupField{Property::MainOutputBusRef, G(102)});
    EXPECT_FALSE(outputConflict.after);
    EXPECT_TRUE(hasCode(outputConflict.diagnostics, Code::MainOutputSendConflict));
    const auto sendConflict = world.edit(Group::Send,
        ReplaceFieldGroupSends{SendFieldGroup{{{G(101), -7.0f}}}});
    EXPECT_FALSE(sendConflict.after);
    EXPECT_TRUE(hasCode(sendConflict.diagnostics, Code::MainOutputSendConflict));
    EXPECT_EQ(world.sound->fieldGroups, before);
}

TEST(FieldGroupEditing, AncestorEditsValidateDescendantRoutingOnEveryPlatform)
{
    World world;
    world.sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        local(MainOutputFieldGroup{G(101), -2.0f});
    world.layer->fieldGroups.send.shared = local(SendFieldGroup{});
    const auto before = world.layer->fieldGroups;
    const auto result = prepareFieldGroupEdit(world.project,
        {world.layer->guid, Group::Send, shared(),
         ReplaceFieldGroupSends{SendFieldGroup{{{G(101), -3.0f}}}}, {}});
    EXPECT_FALSE(result.after);
    EXPECT_TRUE(hasCode(result.diagnostics, Code::MainOutputSendConflict));
    EXPECT_EQ(world.layer->fieldGroups, before);
    const auto error = std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == Code::MainOutputSendConflict; });
    ASSERT_NE(error, result.diagnostics.end());
    EXPECT_EQ(error->definition, world.sound->guid);
    EXPECT_EQ(error->scope, HarkenTest::FieldGroupPlatformIds::Android);
    EXPECT_EQ(error->row, 0u);
}

TEST(FieldGroupEditing, LocalEmptySendListBlocksTheParentTable)
{
    World world;
    world.layer->fieldGroups.send.shared = local(SendFieldGroup{{{G(101), -1.0f}}});
    world.sound->fieldGroups.send.shared = local(SendFieldGroup{{{G(102), -2.0f}}});
    const auto result = world.edit(Group::Send, ReplaceFieldGroupSends{SendFieldGroup{}});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.after->send.shared.mode, Mode::Local);
    EXPECT_EQ(result.after->send.shared.localHistory, SendFieldGroup{});
    EXPECT_EQ(world.layer->fieldGroups.send.shared.localHistory,
              (SendFieldGroup{{{G(101), -1.0f}}}));
}

TEST(FieldGroupEditing, SendTargetsMustBeUniquePresentBusDefinitions)
{
    World world;
    world.sound->fieldGroups.send.shared = local(SendFieldGroup{});
    for (const auto& sends : {
             SendFieldGroup{{{G(101), -1.0f}, {G(101), -2.0f}}},
             SendFieldGroup{{{G(200), -1.0f}}},
             SendFieldGroup{{{G(999), -1.0f}}}}) {
        const auto result = world.edit(Group::Send, ReplaceFieldGroupSends{sends});
        EXPECT_FALSE(result.after);
        EXPECT_TRUE(hasCode(result.diagnostics, Code::InvalidReference));
    }
    const auto emptyTarget = world.edit(Group::Send,
        ReplaceFieldGroupSends{SendFieldGroup{{{Guid{}, -1.0f}}}});
    EXPECT_FALSE(emptyTarget.after);
    EXPECT_TRUE(hasCode(emptyTarget.diagnostics, Code::InvalidGroups));
    EXPECT_EQ(world.sound->fieldGroups.send.shared.localHistory, SendFieldGroup{});
}

TEST(FieldGroupEditing, MultiPlatformSendReplacementUnifiesTheSendFieldAndPreservesInherit)
{
    World world;
    world.sound->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        local(SendFieldGroup{{{G(101), -1.0f}}});
    world.sound->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        local(SendFieldGroup{{{G(102), -2.0f}}});
    const auto scopes = platforms({HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android});
    const auto mixed = world.edit(Group::Send, ReplaceFieldGroupSends{SendFieldGroup{}}, scopes);
    ASSERT_TRUE(mixed.ok());
    for (const auto platform : scopes.platforms)
        EXPECT_EQ(ResolveFieldGroupLocal(mixed.after->send, platform).localHistory, SendFieldGroup{});
    world.sound->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        world.sound->fieldGroups.send.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows);
    const auto result = world.edit(Group::Send, ReplaceFieldGroupSends{SendFieldGroup{}}, scopes);
    ASSERT_TRUE(result.ok());
    for (const auto platform : scopes.platforms) {
        EXPECT_EQ(result.after->send.platforms.at(platform).mode, Mode::Local);
        EXPECT_EQ(result.after->send.platforms.at(platform).localHistory, SendFieldGroup{});
    }
    EXPECT_EQ(result.after->send.shared, world.sound->fieldGroups.send.shared);
}

TEST(FieldGroupEditing, RebuildUsesEachCurrentEffectiveRatingAndReplacesDormantHistory)
{
    World world;
    const SpatialRatingFieldGroup windows{-0.5f, true, G(200), 5.0f};
    const SpatialRatingFieldGroup android{0.5f, false, {}, 6.0f};
    world.layer->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = local(windows);
    world.layer->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = local(android);
    const auto dormant = SpatialRatingFieldGroup{0.0f, true, G(999), 3.0f};
    for (const auto platform : {HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android})
        world.sound->fieldGroups.spatialRating.platforms[platform] = {Mode::Inherit, dormant};
    const auto result = world.edit(Group::SpatialRating, RebuildLocalFieldGroup{},
                                  platforms({HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android}));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.after->spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).localHistory, windows);
    EXPECT_EQ(result.after->spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).localHistory, android);
    EXPECT_EQ(world.sound->fieldGroups.spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).localHistory,
              dormant);
}

TEST(FieldGroupEditing, InvalidOrAmbiguousScopeSelectionsAreRejected)
{
    World world;
    const std::array invalidScopes{
        FieldGroupEditScopes{},
        FieldGroupEditScopes{true, {HarkenTest::FieldGroupPlatformIds::Windows}},
        platforms({HarkenTest::FieldGroupPlatformIds::Universal}),
        platforms({HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Windows}),
        platforms({harkenprojectschema::AuthoringGuid{}})};
    for (const auto& scopes : invalidScopes) {
        const auto result = world.edit(Group::MainOutput, SetFieldGroupMode{Mode::Local}, scopes);
        EXPECT_FALSE(result.after);
        EXPECT_FALSE(result.changed);
        EXPECT_TRUE(hasCode(result.diagnostics, Code::InvalidScopes));
    }
    const auto sharedUseShared = world.edit(Group::MainOutput, SetFieldGroupMode{Mode::UseShared});
    EXPECT_FALSE(sharedUseShared.after);
    EXPECT_TRUE(hasCode(sharedUseShared.diagnostics, Code::InvalidOperation));
    EXPECT_EQ(world.sound->fieldGroups, DefinitionFieldGroups{});
}

TEST(FieldGroupEditing, InvalidOperationsAndStaleExpectationsCannotBypassTheContract)
{
    World world;
    world.sound->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{G(101), -1.0f});
    const auto before = world.sound->fieldGroups;
    const auto wrongGroup = world.edit(Group::MainOutput,
        SetFieldGroupField{Property::SpatialPanning, 0.5f});
    EXPECT_TRUE(hasCode(wrongGroup.diagnostics, Code::InvalidOperation));
    const auto wrongType = world.edit(Group::MainOutput,
        SetFieldGroupField{Property::OutputBusVolume, true});
    EXPECT_TRUE(hasCode(wrongType.diagnostics, Code::InvalidOperation));
    const auto wrongSend = world.edit(Group::MainOutput, ReplaceFieldGroupSends{});
    EXPECT_TRUE(hasCode(wrongSend.diagnostics, Code::InvalidOperation));
    const auto invalidMode = world.edit(Group::MainOutput,
        SetFieldGroupMode{static_cast<Mode>(255)});
    EXPECT_TRUE(hasCode(invalidMode.diagnostics, Code::InvalidGroups));
    const auto stale = prepareFieldGroupEdit(world.project,
        {world.sound->guid, Group::MainOutput, shared(),
         SetFieldGroupMode{Mode::Inherit}, DefinitionFieldGroups{}});
    EXPECT_FALSE(stale.after);
    EXPECT_TRUE(hasCode(stale.diagnostics, Code::StaleState));
    EXPECT_EQ(world.sound->fieldGroups, before);
}

TEST(FieldGroupEditing, RepeatingAStoredModeOrValueDoesNotCreateAChange)
{
    World world;
    const auto inherit = world.edit(Group::MainOutput, SetFieldGroupMode{Mode::Inherit});
    ASSERT_TRUE(inherit.ok());
    EXPECT_FALSE(inherit.changed);
    const auto useShared = world.edit(Group::MainOutput, SetFieldGroupMode{Mode::UseShared},
                                      platforms({HarkenTest::FieldGroupPlatformIds::Windows}));
    ASSERT_TRUE(useShared.ok());
    EXPECT_FALSE(useShared.changed);
    EXPECT_TRUE(useShared.after->mainOutput.platforms.empty());
    world.sound->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{G(101), -3.0f});
    const auto sameValue = world.edit(Group::MainOutput,
        SetFieldGroupField{Property::OutputBusVolume, -3.0f});
    ASSERT_TRUE(sameValue.ok());
    EXPECT_FALSE(sameValue.changed);
}

TEST(FieldGroupEditing, BusAndUnknownDefinitionsCannotReceiveFieldGroupEdits)
{
    World world;
    const auto bus = prepareFieldGroupEdit(world.project,
        {world.master->guid, Group::MainOutput, shared(), SetFieldGroupMode{Mode::Local}, {}});
    EXPECT_FALSE(bus.after);
    EXPECT_TRUE(hasCode(bus.diagnostics, Code::UnsupportedDefinition));
    const auto unknown = prepareFieldGroupEdit(world.project,
        {QUuid::createUuid(), Group::MainOutput, shared(), SetFieldGroupMode{Mode::Local}, {}});
    EXPECT_FALSE(unknown.after);
    EXPECT_TRUE(hasCode(unknown.diagnostics, Code::UnknownDefinition));
    const auto restoreBus = prepareFieldGroupRestore(world.project, world.master->guid, {});
    EXPECT_TRUE(hasCode(restoreBus.diagnostics, Code::UnsupportedDefinition));
    EXPECT_EQ(world.master->fieldGroups, DefinitionFieldGroups{});
}

TEST(FieldGroupEditing, MissingMasterAndBrokenOwnershipRejectEditsWithoutFallback)
{
    World world;
    world.project.busses.clear();
    const auto noMaster = world.edit(Group::MainOutput, SetFieldGroupMode{Mode::Local});
    EXPECT_FALSE(noMaster.after);
    EXPECT_TRUE(hasCode(noMaster.diagnostics, Code::InvalidMasterBus));
    world.project.busses = {{world.master, {}}};
    world.sound->parentRef.kind = ParentRef::Kind::Item;
    world.sound->parentRef.guid = QUuid::createUuid();
    const auto brokenParent = world.edit(Group::MainOutput, SetFieldGroupMode{Mode::Local});
    EXPECT_FALSE(brokenParent.after);
    EXPECT_TRUE(hasCode(brokenParent.diagnostics, Code::InvalidProject));
    EXPECT_EQ(world.sound->fieldGroups, DefinitionFieldGroups{});
}

TEST(FieldGroupEditing, WholeProjectOverlaysValidateAtomicallyWithoutChangingAuthoring)
{
    World world;
    world.layer->fieldGroups.send.shared = local(SendFieldGroup{});
    world.sound->fieldGroups.mainOutput.shared = local(MainOutputFieldGroup{G(101), -1.0f});
    const auto layerBefore = world.layer->fieldGroups;
    const auto soundBefore = world.sound->fieldGroups;
    auto layerAfter = layerBefore;
    layerAfter.send.shared.localHistory = SendFieldGroup{{{G(101), -2.0f}}};
    auto soundAfter = soundBefore;
    soundAfter.mainOutput.shared.localHistory->mainOutputBusRef = G(102);
    const std::array invalid{FieldGroupDefinitionSnapshot{world.layer->guid, layerAfter}};
    EXPECT_FALSE(validateFieldGroupProject(world.project, invalid).ok());
    const std::array valid{
        FieldGroupDefinitionSnapshot{world.layer->guid, layerAfter},
        FieldGroupDefinitionSnapshot{world.sound->guid, soundAfter}};
    EXPECT_TRUE(validateFieldGroupProject(world.project, valid).ok());
    EXPECT_EQ(world.layer->fieldGroups, layerBefore);
    EXPECT_EQ(world.sound->fieldGroups, soundBefore);
    const std::array duplicate{
        FieldGroupDefinitionSnapshot{world.sound->guid, soundAfter},
        FieldGroupDefinitionSnapshot{world.sound->guid, soundAfter}};
    EXPECT_TRUE(hasCode(validateFieldGroupProject(world.project, duplicate).diagnostics,
                        Code::InvalidOperation));
    const std::array bus{FieldGroupDefinitionSnapshot{world.master->guid, {}}};
    EXPECT_TRUE(hasCode(validateFieldGroupProject(world.project, bus).diagnostics,
                        Code::UnsupportedDefinition));
}

TEST(FieldGroupEditing, LegacyDefinitionGroupSlotsCannotBypassCanonicalState)
{
    for (const auto type : {ProjectItem::Type::Layer, ProjectItem::Type::Sound,
                            ProjectItem::Type::Container, ProjectItem::Type::Track,
                            ProjectItem::Type::Piece}) {
        for (const bool platform : {false, true}) {
            World world;
            world.sound->type = type;
            if (platform)
                world.sound->platformOverrides.ensure(HarkenTest::PlatformIds::Windows)
                    ->properties.presence.bits = kFieldGroupPropertyMask;
            else
                world.sound->playablePropertyPresence.bits = kFieldGroupPropertyMask;
            const auto before = world.sound->fieldGroups;
            EXPECT_FALSE(validateFieldGroupProject(world.project).ok());
            const auto edit = world.edit(Group::Send, SetFieldGroupMode{Mode::Local});
            EXPECT_FALSE(edit.ok());
            EXPECT_TRUE(hasCode(edit.diagnostics, Code::InvalidGroups));
            const auto restore = prepareFieldGroupRestore(
                world.project, world.sound->guid, DefinitionFieldGroups{});
            EXPECT_FALSE(restore.ok());
            EXPECT_EQ(world.sound->fieldGroups, before);
        }
    }
}

TEST(FieldGroupEditing, LegacyBusAndPlaybackMixSlotsRemainIndependent)
{
    World world;
    world.master->playablePropertyPresence.bits = kFieldGroupPropertyMask;
    world.master->platformOverrides.ensure(HarkenTest::PlatformIds::Linux)
        ->properties.presence.bits = kFieldGroupPropertyMask;
    world.sound->playablePropertyPresence.Set(Property::Volume);
    world.sound->platformOverrides.ensure(HarkenTest::PlatformIds::Windows)
        ->properties.presence.Set(Property::Pitch);
    EXPECT_TRUE(validateFieldGroupProject(world.project).ok());
    EXPECT_TRUE(world.edit(Group::MainOutput, SetFieldGroupMode{Mode::Local}).ok());
}

TEST(FieldGroupEditing, NonDefinitionObjectsCannotHideAuthoredFieldGroups)
{
    World world;
    for (const auto& target : {world.master, world.attenuation}) {
        target->fieldGroups.send.shared = local(SendFieldGroup{});
        const auto result = validateFieldGroupProject(world.project);
        EXPECT_FALSE(result.ok());
        EXPECT_TRUE(hasCode(result.diagnostics, Code::UnsupportedDefinition));
        target->fieldGroups = DefinitionFieldGroups{};
    }
    EXPECT_TRUE(validateFieldGroupProject(world.project).ok());
}

TEST(FieldGroupEditing, ClipGroupSlotsAreRejectedOnBothStorageScopes)
{
    for (const bool platform : {false, true}) {
        World world;
        auto track = item(3, ProjectItem::Type::Track);
        own(world.layer, track);
        auto* timeline = track->compOrCreate<VTrack>();
        timeline->clips.items.push_back({QUuid::createUuid(), world.sound->guid});
        auto& clip = timeline->clips.items.front();
        if (platform)
            clip.platformOverrides.ensure(HarkenTest::PlatformIds::iOS)
                ->properties.presence.bits = kFieldGroupPropertyMask;
        else
            clip.playableProperties.presence.bits = kFieldGroupPropertyMask;
        const auto result = validateFieldGroupProject(world.project);
        EXPECT_FALSE(result.ok());
        ASSERT_EQ(result.diagnostics.size(), 1u);
        EXPECT_EQ(result.diagnostics.front().definition, track->guid);
        EXPECT_EQ(result.diagnostics.front().row, 0u);
    }
}

}
}
