#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/Components/VSelection.h"
#include "Core/PackBuildPlan.h"
#include "Core/Components/VAction.h"
#include "Core/Components/VSound.h"
#include "Core/FieldGroupProjectAdapter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <gtest/gtest.h>

#include <set>
#include "Support/ProjectPlatformTestUtils.h"

using namespace VNX;

namespace {
std::shared_ptr<ProjectItem> item(ProjectItem::Type type, uint32_t objectId, harkenstudio::ObjectId stableId)
{
    auto result = std::make_shared<ProjectItem>(QStringLiteral("Item"), type);
    result->objectID = objectId;
    result->guid = VNX::authoringUuid(harkentest::StudioObjectId(stableId));
    result->attachDefaultComponents();
    return result;
}
}

TEST(PackBuildPlanTest, CreatesStableActionVariantsAndArtifactNames)
{
    auto sound = item(ProjectItem::Type::Sound, 1, harkentest::StudioObjectId(101));
    auto* soundComponent = sound->comp<VSound>();
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = 10;
    soundComponent->sources.plain.entries.push_back(source);
    soundComponent->sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = source.entryId;
    auto action = item(ProjectItem::Type::Action, 2, harkentest::StudioObjectId(102));
    VActionStep step;
    step.type = VActionStep::Type::Play;
    step.targetObjectID = sound->objectID;
    action->comp<VAction>()->steps.push_back(step);
    auto project = HarkenTest::projectWithPlatforms();
    ProjectFileBinding soundBinding;
    soundBinding.rootItem = sound;
    project.layers.push_back(std::move(soundBinding));
    ProjectFileBinding actionBinding;
    actionBinding.rootItem = action;
    project.actions.push_back(std::move(actionBinding));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows")});

    const auto plan = PackBuildPlanning::build(project);
    ASSERT_EQ(plan.variants.size(), 1u);
    EXPECT_EQ(plan.variants.front().artifactName, QStringLiteral("Item"));
    EXPECT_EQ(plan.variants.front().relativePackPath,
              QStringLiteral("Windows/Item"));
    EXPECT_TRUE(plan.variants.front().variant.language.isEmpty());
    EXPECT_EQ(plan.variants.front().closure.media.size(), 1u);
}

TEST(PackBuildPlanTest, PostActionDependenciesAreOrderedBeforeRoot)
{
    auto project = HarkenTest::projectWithPlatforms();
    auto dependency = item(ProjectItem::Type::Action, 1, harkentest::StudioObjectId(11));
    auto root = item(ProjectItem::Type::Action, 2, harkentest::StudioObjectId(12));
    VActionStep post;
    post.type = VActionStep::Type::PostAction;
    post.targetObjectID = dependency->objectID;
    root->comp<VAction>()->steps.push_back(post);
    for (const auto& action : {dependency, root}) {
        ProjectFileBinding binding;
        binding.rootItem = action;
        project.actions.push_back(std::move(binding));
    }
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows")});

    const auto plan = PackBuildPlanning::build(project);
    const auto variant = std::find_if(plan.variants.begin(), plan.variants.end(), [](const auto& value) {
        return value.actionStableId == harkentest::StudioObjectId(12);
    });
    ASSERT_NE(variant, plan.variants.end());
    EXPECT_EQ(variant->dependencyOrder, (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(11), harkentest::StudioObjectId(12)}));
}

TEST(PackBuildPlanTest, LegacyGroupMembersBecomeIndependentBuildUnits)
{
    auto project = HarkenTest::projectWithPlatforms();
    auto action = item(ProjectItem::Type::Action, 1, harkentest::StudioObjectId(11));
    ProjectFileBinding binding;
    binding.rootItem = action;
    project.actions.push_back(std::move(binding));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
    const auto first = PackBuildPlanning::build(project);
    auto secondAction = item(ProjectItem::Type::Action, 2, harkentest::StudioObjectId(12));
    project.actions.push_back({secondAction, QStringLiteral("Second")});
    PackGroup group;
    group.name = QStringLiteral("Bundle");
    group.actionIds = {harkentest::StudioObjectId(11), harkentest::StudioObjectId(12)};
    project.packGroups.push_back(group);
    const auto grouped = PackBuildPlanning::build(project);
    ASSERT_EQ(first.units.size(), 1u);
    EXPECT_FALSE(first.units.front().grouped);
    ASSERT_EQ(grouped.units.size(), 2u);
    ASSERT_EQ(grouped.variants.size(), 2u);
    for (const auto& unit : grouped.units) {
        EXPECT_FALSE(unit.grouped);
        ASSERT_EQ(unit.actionStableIds.size(), 1u);
        EXPECT_EQ(unit.artifactKey, PackBuildPlanning::stableArtifactKey(unit.actionStableIds.front()));
    }

}

TEST(PackBuildPlanTest, LegacyGroupPlatformSelectionDoesNotRestrictBuildUnits)
{
    auto project = HarkenTest::projectWithPlatforms();
    auto action = item(ProjectItem::Type::Action, 1, harkentest::StudioObjectId(11));
    ProjectFileBinding binding;
    binding.rootItem = action;
    project.actions.push_back(std::move(binding));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("macOS")});
    PackGroup group;
    group.name = QStringLiteral("Bundle");
    group.actionIds = {harkentest::StudioObjectId(11)};
    group.allPlatforms = false;
    group.platformIds = {HarkenTest::platformIdString("Windows")};
    project.packGroups.push_back(group);

    const auto plan = PackBuildPlanning::build(project);
    EXPECT_TRUE(plan.groupMetadata.value(QStringLiteral("groups")).toArray().isEmpty());
    EXPECT_EQ(plan.variants.size(), 2u);
    ASSERT_EQ(plan.units.size(), 2u);
    EXPECT_EQ(plan.units.front().name, action->name);
    EXPECT_EQ(plan.units.front().variant.platform, HarkenTest::platformIdString("Windows"));
    EXPECT_EQ(plan.units.back().variant.platform, HarkenTest::platformIdString("macOS"));

}

TEST(PackBuildPlanTest, UnlocalizedActionStaysOnPlatformAxisEvenWithProjectLanguages)
{
    auto sound = item(ProjectItem::Type::Sound, 1, harkentest::StudioObjectId(101));
    auto* soundComponent = sound->comp<VSound>();
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = 10;
    soundComponent->sources.plain.entries.push_back(source);
    soundComponent->sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = source.entryId;
    auto action = item(ProjectItem::Type::Action, 2, harkentest::StudioObjectId(102));
    VActionStep step;
    step.type = VActionStep::Type::Play;
    step.targetObjectID = sound->objectID;
    action->comp<VAction>()->steps.push_back(step);
    auto project = HarkenTest::projectWithPlatforms();
    ProjectFileBinding soundBinding;
    soundBinding.rootItem = sound;
    project.layers.push_back(std::move(soundBinding));
    ProjectFileBinding actionBinding;
    actionBinding.rootItem = action;
    project.actions.push_back(std::move(actionBinding));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("macOS")});
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};

    const auto plan = PackBuildPlanning::build(project);
    ASSERT_EQ(plan.variants.size(), 2u);
    EXPECT_EQ(plan.variants[0].relativePackPath, QStringLiteral("Windows/Item"));
    EXPECT_EQ(plan.variants[1].relativePackPath, QStringLiteral("macOS/Item"));
    EXPECT_TRUE(plan.variants[0].variant.language.isEmpty());
    EXPECT_TRUE(plan.variants[1].variant.language.isEmpty());
    EXPECT_EQ(PackBuildPlanning::actionLanguages(project).value(action->runtimeId()),
              QStringList{QString()});

    const auto requested = PackBuildPlanning::build(project, {}, {QStringLiteral("French")});
    EXPECT_EQ(requested.toJson().value(QStringLiteral("variants")),
              plan.toJson().value(QStringLiteral("variants")));
    EXPECT_EQ(requested.toJson().value(QStringLiteral("diagnostics")),
              plan.toJson().value(QStringLiteral("diagnostics")));
}

TEST(PackBuildPlanTest, LocalizedActionFansOutLanguageDirectoriesWithSharedFilename)
{
    auto sound = item(ProjectItem::Type::Sound, 1, harkentest::StudioObjectId(101));
    auto* soundComponent = sound->comp<VSound>();
    soundComponent->distinguishLanguage = true;
    SoundSourceDef english;
    english.entryId = QUuid::createUuid();
    english.sourceId = 10;
    SoundSourceDef japanese;
    japanese.entryId = QUuid::createUuid();
    japanese.sourceId = 11;
    soundComponent->sources.localized["English"].entries = {english};
    soundComponent->sources.localized["Japanese"].entries = {japanese};
    soundComponent->sources.localized[QStringLiteral("English")].platforms[HarkenTest::platformIdString("Windows")].active = english.entryId;
    soundComponent->sources.localized[QStringLiteral("Japanese")].platforms[HarkenTest::platformIdString("Windows")].active = japanese.entryId;
    auto action = item(ProjectItem::Type::Action, 2, harkentest::StudioObjectId(102));
    VActionStep step;
    step.type = VActionStep::Type::Play;
    step.targetObjectID = sound->objectID;
    action->comp<VAction>()->steps.push_back(step);
    auto project = HarkenTest::projectWithPlatforms();
    auto master = item(ProjectItem::Type::Bus, 3, harkentest::StudioObjectId(0x4d4153544552));
    project.busses.push_back({master, QStringLiteral("Busses")});
    sound->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    ProjectFileBinding soundBinding;
    soundBinding.rootItem = sound;
    project.layers.push_back(std::move(soundBinding));
    ProjectFileBinding actionBinding;
    actionBinding.rootItem = action;
    project.actions.push_back(std::move(actionBinding));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};

    const auto plan = PackBuildPlanning::build(project);
    ASSERT_EQ(plan.variants.size(), 2u);
    EXPECT_EQ(plan.variants[0].artifactName, QStringLiteral("Item"));
    EXPECT_EQ(plan.variants[1].artifactName, QStringLiteral("Item"));
    EXPECT_EQ(plan.variants[0].relativePackPath,
              QStringLiteral("Windows/English/Item"));
    EXPECT_EQ(plan.variants[1].relativePackPath,
              QStringLiteral("Windows/Japanese/Item"));
    EXPECT_NE(plan.variants[0].relativePackPath, plan.variants[1].relativePackPath);

    const auto selected = PackBuildPlanning::build(project, {}, {QStringLiteral("Japanese")});
    ASSERT_EQ(selected.variants.size(), 1u);
    EXPECT_EQ(selected.variants.front().variant.language, QStringLiteral("Japanese"));
    EXPECT_TRUE(selected.diagnostics.empty());
    for (const QStringList& requested : std::vector<QStringList>{
             {QStringLiteral("French")},
             {QStringLiteral("english")},
             {QStringLiteral("English"), QStringLiteral("French")}}) {
        SCOPED_TRACE(requested.join(QLatin1Char(',')).toStdString());
        const auto rejected = PackBuildPlanning::build(project, {}, requested);
        EXPECT_TRUE(rejected.variants.empty());
        ASSERT_EQ(rejected.diagnostics.size(), 1u);
        EXPECT_EQ(rejected.diagnostics.front().code,
                  ActionClosureDiagnosticCode::InvalidBuildLanguage);
        EXPECT_EQ(rejected.diagnostics.front().toJson().value(QStringLiteral("code")).toString(),
                  QStringLiteral("InvalidBuildLanguage"));
        EXPECT_FALSE(rejected.ready());
    }

    auto commonSound = item(ProjectItem::Type::Sound, 4, harkentest::StudioObjectId(104));
    commonSound->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    commonSound->comp<VSound>()->sources.plain.entries = {english};
    commonSound->comp<VSound>()->sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = english.entryId;
    auto commonAction = item(ProjectItem::Type::Action, 5, harkentest::StudioObjectId(105));
    commonAction->name = QStringLiteral("Common");
    step.targetObjectID = commonSound->objectID;
    commonAction->comp<VAction>()->steps = {step};
    project.layers.push_back({commonSound, QStringLiteral("CommonSound")});
    project.actions.push_back({commonAction, QStringLiteral("CommonAction")});
    PackGroup group;
    group.name = QStringLiteral("Dialogue");
    group.actionIds = {action->runtimeId(), commonAction->runtimeId()};
    project.packGroups.push_back(group);
    const auto grouped = PackBuildPlanning::build(project);
    ASSERT_EQ(grouped.units.size(), 3u);
    ASSERT_EQ(grouped.variants.size(), 3u);
    for (const auto& unit : grouped.units) {
        EXPECT_FALSE(unit.grouped);
        ASSERT_EQ(unit.actionStableIds.size(), 1u);
        EXPECT_EQ(unit.variant.language.isEmpty(), unit.actionStableIds.front() == commonAction->runtimeId());
    }
    const auto selectedUnits = PackBuildPlanning::build(project, {}, {QStringLiteral("Japanese")});
    ASSERT_EQ(selectedUnits.units.size(), 2u);
    PackExportSelection selection;
    selection.actionTargeting.push_back({action->runtimeId(), false, {}, QStringLiteral("English")});
    project.packGroups.clear();
    const auto targeted = PackBuildPlanning::build(project, {}, {}, false, selection);
    ASSERT_EQ(targeted.units.size(), 2u);
    for (const auto& unit : targeted.units) EXPECT_NE(unit.variant.language, QStringLiteral("English"));
    const auto visible = PackBuildPlanning::build(project, {}, {}, true, selection);
    EXPECT_EQ(visible.units.size(), 3u);

}

TEST(PackBuildPlanTest, NamedPostActionAndExternalOccurrenceUseTheSameLocalizedClosure)
{
    auto sound = item(ProjectItem::Type::Sound, 1, harkentest::StudioObjectId(101));
    sound->name = QStringLiteral("Voice");
    auto* component = sound->comp<VSound>();
    component->distinguishLanguage = true;
    auto project = HarkenTest::projectWithPlatforms();
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};
    for (const QString& language : project.projectLanguages) {
        SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = language == QStringLiteral("English") ? 10 : 11;
        component->sources.localized[language].entries.push_back(source);
        component->sources.localized[language].platforms[HarkenTest::platformIdString("Windows")].active = source.entryId;
    }
    auto container = item(ProjectItem::Type::Container, 2, harkentest::StudioObjectId(102));
    container->name = QStringLiteral("ExternalVoice");
    container->comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater = 1;
    EXPECT_TRUE(container->comp<VNX::VSelection>()->selections.shared.setOccurrenceRows({{sound->guid, 0}}));
    auto inner = item(ProjectItem::Type::Action, 3, harkentest::StudioObjectId(103));
    inner->name = QStringLiteral("Inner");
    VActionStep play;
    play.type = VActionStep::Type::Play;
    play.targetName = container->name;
    inner->comp<VAction>()->steps.push_back(play);
    auto outer = item(ProjectItem::Type::Action, 4, harkentest::StudioObjectId(104));
    outer->name = QStringLiteral("Outer");
    VActionStep post;
    post.type = VActionStep::Type::PostAction;
    post.targetName = inner->name;
    outer->comp<VAction>()->steps.push_back(post);
    for (const auto& value : {sound, container, inner, outer}) {
        ProjectFileBinding binding;
        binding.rootItem = value;
        if (value->type == ProjectItem::Type::Action)
            project.actions.push_back(std::move(binding));
        else
            project.layers.push_back(std::move(binding));
    }
    EXPECT_TRUE(PackBuildPlanning::actionIsLocalized(project, harkentest::StudioObjectId(104)));
    const auto projected = PackBuildPlanning::actionLanguages(project);
    EXPECT_EQ(projected.value(inner->runtimeId()), project.projectLanguages);
    EXPECT_EQ(projected.value(outer->runtimeId()), project.projectLanguages);
    const auto plan = PackBuildPlanning::buildForAction(project, harkentest::StudioObjectId(104));
    ASSERT_EQ(plan.variants.size(), 2u);
    for (const auto& variant : plan.variants) {
        EXPECT_EQ(variant.dependencyOrder, (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(103), harkentest::StudioObjectId(104)}));
        ASSERT_EQ(variant.closure.media.size(), 1u);
        EXPECT_EQ(variant.closure.media.front().soundStableId, harkentest::StudioObjectId(101u));
        EXPECT_EQ(variant.closure.media.front().language, variant.variant.language);
        EXPECT_EQ(variant.closure.media.front().authoringSourceId,
                  variant.variant.language == QStringLiteral("English") ? 10u : 11u);
        EXPECT_TRUE(std::any_of(variant.closure.edges.begin(), variant.closure.edges.end(),
                               [](const auto& edge) {
            return edge.fromStableId == harkentest::StudioObjectId(102) && edge.toStableId == harkentest::StudioObjectId(101)
                && edge.field == QStringLiteral("selections");
        }));
    }
    project.projectLanguages.clear();
    const auto withoutLanguages = PackBuildPlanning::actionLanguages(project);
    EXPECT_TRUE(withoutLanguages.contains(inner->runtimeId()));
    EXPECT_TRUE(withoutLanguages.value(inner->runtimeId()).isEmpty());
    EXPECT_TRUE(withoutLanguages.contains(outer->runtimeId()));
    EXPECT_TRUE(withoutLanguages.value(outer->runtimeId()).isEmpty());
    const auto empty = PackBuildPlanning::buildForAction(project, harkentest::StudioObjectId(104));
    EXPECT_TRUE(empty.variants.empty());
    ASSERT_EQ(empty.diagnostics.size(), 1u);
    EXPECT_EQ(empty.diagnostics.front().code, ActionClosureDiagnosticCode::LocalizedWithoutLanguages);
}

TEST(PackBuildPlanTest, LocalizedActionWithoutProjectLanguagesEmitsError)
{
    auto sound = item(ProjectItem::Type::Sound, 1, harkentest::StudioObjectId(101));
    sound->comp<VSound>()->distinguishLanguage = true;
    auto action = item(ProjectItem::Type::Action, 2, harkentest::StudioObjectId(102));
    VActionStep step;
    step.type = VActionStep::Type::Play;
    step.targetObjectID = sound->objectID;
    action->comp<VAction>()->steps.push_back(step);
    auto project = HarkenTest::projectWithPlatforms();
    ProjectFileBinding soundBinding;
    soundBinding.rootItem = sound;
    project.layers.push_back(std::move(soundBinding));
    ProjectFileBinding actionBinding;
    actionBinding.rootItem = action;
    project.actions.push_back(std::move(actionBinding));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows")});

    const auto plan = PackBuildPlanning::build(project);
    EXPECT_TRUE(plan.variants.empty());
    ASSERT_FALSE(plan.diagnostics.empty());
    EXPECT_EQ(plan.diagnostics.front().code,
              ActionClosureDiagnosticCode::LocalizedWithoutLanguages);
    EXPECT_FALSE(plan.ready());

    const auto requested = PackBuildPlanning::build(project, {}, {QStringLiteral("English")});
    EXPECT_TRUE(requested.variants.empty());
    ASSERT_EQ(requested.diagnostics.size(), 1u);
    EXPECT_EQ(requested.diagnostics.front().code,
              ActionClosureDiagnosticCode::InvalidBuildLanguage);
    EXPECT_FALSE(requested.ready());

    PackExportSelection selection;
    selection.actionTargeting.push_back({action->runtimeId(), false, {}});
    const auto excluded = PackBuildPlanning::build(project, {}, {}, false, selection);
    EXPECT_TRUE(excluded.variants.empty());
    EXPECT_TRUE(excluded.ready());
    EXPECT_FALSE(PackBuildPlanning::build(project, {}, {}, true, selection).ready());
}

TEST(PackBuildPlanTest, MixedClosureCopiesUnlocalizedMediaIntoEveryLanguagePack)
{
    auto voice = item(ProjectItem::Type::Sound, 1, harkentest::StudioObjectId(101));
    auto* voiceComponent = voice->comp<VSound>();
    voiceComponent->distinguishLanguage = true;
    SoundSourceDef english;
    english.entryId = QUuid::createUuid();
    english.sourceId = 10;
    SoundSourceDef japanese;
    japanese.entryId = QUuid::createUuid();
    japanese.sourceId = 11;
    voiceComponent->sources.localized["English"].entries = {english};
    voiceComponent->sources.localized["Japanese"].entries = {japanese};
    voiceComponent->sources.localized[QStringLiteral("English")].platforms[HarkenTest::platformIdString("Windows")].active = english.entryId;
    voiceComponent->sources.localized[QStringLiteral("Japanese")].platforms[HarkenTest::platformIdString("Windows")].active = japanese.entryId;
    auto sfx = item(ProjectItem::Type::Sound, 2, harkentest::StudioObjectId(201));
    auto* sfxComponent = sfx->comp<VSound>();
    SoundSourceDef shared;
    shared.entryId = QUuid::createUuid();
    shared.sourceId = 20;
    sfxComponent->sources.plain.entries.push_back(shared);
    sfxComponent->sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = shared.entryId;
    auto action = item(ProjectItem::Type::Action, 3, harkentest::StudioObjectId(102));
    VActionStep playVoice;
    playVoice.type = VActionStep::Type::Play;
    playVoice.targetObjectID = voice->objectID;
    VActionStep playSfx;
    playSfx.type = VActionStep::Type::Play;
    playSfx.targetObjectID = sfx->objectID;
    action->comp<VAction>()->steps = {playVoice, playSfx};
    auto project = HarkenTest::projectWithPlatforms();
    ProjectFileBinding voiceBinding;
    voiceBinding.rootItem = voice;
    project.layers.push_back(std::move(voiceBinding));
    ProjectFileBinding sfxBinding;
    sfxBinding.rootItem = sfx;
    project.layers.push_back(std::move(sfxBinding));
    ProjectFileBinding actionBinding;
    actionBinding.rootItem = action;
    project.actions.push_back(std::move(actionBinding));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};

    const auto plan = PackBuildPlanning::build(project);
    ASSERT_EQ(plan.variants.size(), 2u);
    for (const auto& variant : plan.variants) {
        std::set<uint32_t> sourceIds;
        for (const auto& media : variant.closure.media)
            sourceIds.insert(media.authoringSourceId);
        EXPECT_TRUE(sourceIds.contains(20));
        if (variant.variant.language == QStringLiteral("English"))
            EXPECT_TRUE(sourceIds.contains(10));
        else
            EXPECT_TRUE(sourceIds.contains(11));
    }
}
