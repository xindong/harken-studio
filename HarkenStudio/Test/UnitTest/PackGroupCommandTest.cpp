#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "PackGroupEditing.h"
#include "ProjectPackExporter.h"
#include "ProjectTypes.h"
#include "Components/VAction.h"
#include "Components/VAudio.h"
#include "Components/VSound.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

using VNX::PackGroup;
using VNX::PackGroupEditResult;
namespace Edit = VNX::PackGroupEditing;

namespace {

PackGroup makeGroup(const QString& name, std::vector<harkenstudio::ObjectId> ids = {})
{
    PackGroup g;
    g.name = name;
    g.actionIds = std::move(ids);
    return g;
}

const PackGroup* find(const std::vector<PackGroup>& groups, const QString& name)
{
    for (const auto& g : groups)
        if (g.name == name) return &g;
    return nullptr;
}

} // namespace

// --- create_pack_group ---

TEST(PackGroupCommand, CreateAppendsEmptyGroup)
{
    std::vector<PackGroup> groups;
    const auto r = Edit::createGroup(groups, QStringLiteral("Combat"));
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].name, QStringLiteral("Combat"));
    EXPECT_TRUE(groups[0].actionIds.empty());
}

TEST(PackGroupCommand, CreateRejectsEmptyName)
{
    std::vector<PackGroup> groups;
    const auto r = Edit::createGroup(groups, QString());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(groups.empty());
}

TEST(PackGroupCommand, CreateRejectsDuplicateName)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("Combat"))};
    const auto r = Edit::createGroup(groups, QStringLiteral("Combat"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(groups.size(), 1u);
}

TEST(PackGroupCommand, CreateIsCaseSensitive)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("Combat"))};
    const auto r = Edit::createGroup(groups, QStringLiteral("combat"));
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(groups.size(), 2u);
}

// --- delete_pack_group ---

TEST(PackGroupCommand, DeleteRemovesGroup)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A")), makeGroup(QStringLiteral("B"))};
    const auto r = Edit::deleteGroup(groups, QStringLiteral("A"));
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].name, QStringLiteral("B"));
}

TEST(PackGroupCommand, DeleteRejectsMissingGroup)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    const auto r = Edit::deleteGroup(groups, QStringLiteral("Missing"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(groups.size(), 1u);
}

// --- rename_pack_group ---

TEST(PackGroupCommand, RenameChangesName)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"), {harkentest::StudioObjectId(1u), harkentest::StudioObjectId(2u)})};
    const auto r = Edit::renameGroup(groups, QStringLiteral("A"), QStringLiteral("B"));
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(groups[0].name, QStringLiteral("B"));
    EXPECT_EQ(groups[0].actionIds, (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(1u), harkentest::StudioObjectId(2u)}));
}

TEST(PackGroupCommand, RenameRejectsMissingGroup)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    const auto r = Edit::renameGroup(groups, QStringLiteral("X"), QStringLiteral("Y"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(groups[0].name, QStringLiteral("A"));
}

TEST(PackGroupCommand, RenameRejectsEmptyNewName)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    const auto r = Edit::renameGroup(groups, QStringLiteral("A"), QString());
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(groups[0].name, QStringLiteral("A"));
}

TEST(PackGroupCommand, RenameRejectsDuplicateNewName)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A")), makeGroup(QStringLiteral("B"))};
    const auto r = Edit::renameGroup(groups, QStringLiteral("A"), QStringLiteral("B"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(groups[0].name, QStringLiteral("A"));
}

TEST(PackGroupCommand, RenameToSameNameSucceeds)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    const auto r = Edit::renameGroup(groups, QStringLiteral("A"), QStringLiteral("A"));
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(groups[0].name, QStringLiteral("A"));
}

// --- add_actions_to_pack_group ---

TEST(PackGroupCommand, AddActionsAddsValidIds)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    const std::set<harkenstudio::ObjectId> valid{harkentest::StudioObjectId(10u), harkentest::StudioObjectId(20u), harkentest::StudioObjectId(30u)};
    const auto r = Edit::addActions(groups, QStringLiteral("A"), {harkentest::StudioObjectId(10u), harkentest::StudioObjectId(20u)}, valid);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(groups[0].actionIds, (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(10u), harkentest::StudioObjectId(20u)}));
}

TEST(PackGroupCommand, AddActionsRejectsMissingGroup)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    const auto r = Edit::addActions(groups, QStringLiteral("X"), {harkentest::StudioObjectId(10u)}, {harkentest::StudioObjectId(10u)});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(groups[0].actionIds.empty());
}

TEST(PackGroupCommand, AddActionsFiltersInvalidIds)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    const std::set<harkenstudio::ObjectId> valid{harkentest::StudioObjectId(10u)};
    const auto r = Edit::addActions(groups, QStringLiteral("A"), {harkentest::StudioObjectId(10u), harkentest::StudioObjectId(999u)}, valid);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(groups[0].actionIds, (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(10u)}));
}

TEST(PackGroupCommand, AddActionsIsIdempotentWithinGroup)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"), {harkentest::StudioObjectId(10u)})};
    const auto r = Edit::addActions(groups, QStringLiteral("A"), {harkentest::StudioObjectId(10u)}, {harkentest::StudioObjectId(10u)});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(groups[0].actionIds, (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(10u)}));
}

TEST(PackGroupCommand, AddActionsExclusiveMovesFromOtherGroup)
{
    std::vector<PackGroup> groups{
        makeGroup(QStringLiteral("A"), {harkentest::StudioObjectId(10u), harkentest::StudioObjectId(11u), harkentest::StudioObjectId(12u)}),
        makeGroup(QStringLiteral("B"))};
    const std::set<harkenstudio::ObjectId> valid{harkentest::StudioObjectId(10u), harkentest::StudioObjectId(11u), harkentest::StudioObjectId(12u)};

    const auto r = Edit::addActions(groups, QStringLiteral("B"), {harkentest::StudioObjectId(10u)}, valid);
    ASSERT_TRUE(r.ok);

    EXPECT_EQ(find(groups, QStringLiteral("A"))->actionIds, (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(11u), harkentest::StudioObjectId(12u)}));
    EXPECT_EQ(find(groups, QStringLiteral("B"))->actionIds, (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(10u)}));
}

TEST(PackGroupCommand, AddActionsMoveDissolvesDonorLeftWithOneMember)
{
    std::vector<PackGroup> groups{
        makeGroup(QStringLiteral("A"), {harkentest::StudioObjectId(10u), harkentest::StudioObjectId(11u)}),
        makeGroup(QStringLiteral("B"), {harkentest::StudioObjectId(20u), harkentest::StudioObjectId(21u)})};
    const std::set<harkenstudio::ObjectId> valid{harkentest::StudioObjectId(10u), harkentest::StudioObjectId(11u), harkentest::StudioObjectId(20u), harkentest::StudioObjectId(21u)};

    const auto r = Edit::addActions(groups, QStringLiteral("B"), {harkentest::StudioObjectId(10u)}, valid);
    ASSERT_TRUE(r.ok);

    EXPECT_EQ(find(groups, QStringLiteral("A")), nullptr);
    EXPECT_EQ(find(groups, QStringLiteral("B"))->actionIds,
              (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(20u), harkentest::StudioObjectId(21u), harkentest::StudioObjectId(10u)}));
    ASSERT_EQ(groups.size(), 1u);
}

TEST(PackGroupCommand, AddActionsKeepsSingleMemberTarget)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    const auto r = Edit::addActions(groups, QStringLiteral("A"), {harkentest::StudioObjectId(10u)}, {harkentest::StudioObjectId(10u)});
    ASSERT_TRUE(r.ok);
    ASSERT_NE(find(groups, QStringLiteral("A")), nullptr);
    EXPECT_EQ(find(groups, QStringLiteral("A"))->actionIds, (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(10u)}));
}

// --- remove_actions_from_pack_group ---

TEST(PackGroupCommand, RemoveActionsDropsListed)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"), {harkentest::StudioObjectId(10u), harkentest::StudioObjectId(20u), harkentest::StudioObjectId(30u)})};
    const auto r = Edit::removeActions(groups, QStringLiteral("A"), {harkentest::StudioObjectId(20u)});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(groups[0].actionIds, (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(10u), harkentest::StudioObjectId(30u)}));
}

TEST(PackGroupCommand, RemoveActionsRejectsMissingGroup)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    const auto r = Edit::removeActions(groups, QStringLiteral("X"), {harkentest::StudioObjectId(10u)});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(groups[0].actionIds.empty());
}

TEST(PackGroupCommand, RemoveActionsDissolvesGroupLeftWithOneMember)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"), {harkentest::StudioObjectId(10u), harkentest::StudioObjectId(20u)})};
    const auto r = Edit::removeActions(groups, QStringLiteral("A"), {harkentest::StudioObjectId(20u)});
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(groups.empty());
}

TEST(PackGroupCommand, RemoveActionsDissolvesGroupLeftEmpty)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"), {harkentest::StudioObjectId(10u), harkentest::StudioObjectId(20u)})};
    const auto r = Edit::removeActions(groups, QStringLiteral("A"), {harkentest::StudioObjectId(10u), harkentest::StudioObjectId(20u)});
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(groups.empty());
}

TEST(PackGroupCommand, RemoveActionsNoOpKeepsSingleMemberGroup)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"), {harkentest::StudioObjectId(10u)})};
    const auto r = Edit::removeActions(groups, QStringLiteral("A"), {harkentest::StudioObjectId(999u)});
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].actionIds, (std::vector<harkenstudio::ObjectId>{harkentest::StudioObjectId(10u)}));
}

// --- set_pack_group_platforms ---

TEST(PackGroupCommand, SetPlatformsOverwrites)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    groups[0].allPlatforms = false;
    groups[0].platformIds = QStringList{HarkenTest::platformIdString(QStringLiteral("Windows"))};
    const auto r = Edit::setPlatforms(groups, QStringLiteral("A"), false,
                                      QStringList{HarkenTest::platformIdString(QStringLiteral("Android")), HarkenTest::platformIdString(QStringLiteral("iOS"))});
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(groups[0].allPlatforms);
    EXPECT_EQ(groups[0].platformIds,
              (QStringList{HarkenTest::platformIdString(QStringLiteral("Android")), HarkenTest::platformIdString(QStringLiteral("iOS"))}));
}

TEST(PackGroupCommand, SetPlatformsAllClearsList)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    groups[0].allPlatforms = false;
    groups[0].platformIds = QStringList{HarkenTest::platformIdString(QStringLiteral("Windows"))};
    const auto r = Edit::setPlatforms(groups, QStringLiteral("A"), true,
                                      QStringList{HarkenTest::platformIdString(QStringLiteral("Windows"))});
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(groups[0].allPlatforms);
    EXPECT_TRUE(groups[0].platformIds.isEmpty());
}

TEST(PackGroupCommand, SetPlatformsEmptySubsetTargetsNothing)
{
    std::vector<PackGroup> groups{makeGroup(QStringLiteral("A"))};
    const auto r = Edit::setPlatforms(groups, QStringLiteral("A"), false, QStringList{});
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(groups[0].allPlatforms);
    EXPECT_TRUE(groups[0].platformIds.isEmpty());
    EXPECT_FALSE(groups[0].includesPlatform(HarkenTest::platformIdString(QStringLiteral("Windows"))));
}

// --- readiness duplicate-member guard (S0.3) ---

TEST(PackGroupReadiness, DuplicateMemberAcrossGroupsIsError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::ProjectData project;
    project.name = QStringLiteral("dup");
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("dup.vproj"));
    project.packGroups.push_back(makeGroup(QStringLiteral("GroupA"), {harkentest::StudioObjectId(12345u)}));
    project.packGroups.push_back(makeGroup(QStringLiteral("GroupB"), {harkentest::StudioObjectId(12345u)}));

    const auto result =
        VNX::ProjectPackExporter::ValidatePackBuildReadiness(project, dir.path());

    EXPECT_FALSE(result.valid);
    EXPECT_GE(result.errorCount, 1);

    bool sawDuplicateError = false;
    for (const auto& issue : result.issues) {
        if (issue.severity == VNX::ProjectPackExporter::ValidationIssue::Error
            && issue.message.contains(QStringLiteral("GroupA"))
            && issue.message.contains(QStringLiteral("GroupB"))
            && issue.message.contains(QStringLiteral("3da91a64-b947-4560-0000-000000003039"))) {
            sawDuplicateError = true;
        }
    }
    EXPECT_TRUE(sawDuplicateError);
}

TEST(PackGroupReadiness, DisjointMembersHaveNoDuplicateError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::ProjectData project;
    project.name = QStringLiteral("disjoint");
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("disjoint.vproj"));
    project.packGroups.push_back(makeGroup(QStringLiteral("GroupA"), {harkentest::StudioObjectId(1u), harkentest::StudioObjectId(2u)}));
    project.packGroups.push_back(makeGroup(QStringLiteral("GroupB"), {harkentest::StudioObjectId(3u), harkentest::StudioObjectId(4u)}));

    const auto result =
        VNX::ProjectPackExporter::ValidatePackBuildReadiness(project, dir.path());

    for (const auto& issue : result.issues) {
        EXPECT_FALSE(issue.message.contains(QStringLiteral("multiple pack groups")));
    }
}

TEST(PackGroupReadiness, UnselectedSourcesDoNotBlockReadiness)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::ProjectData project;
    project.name = QStringLiteral("sources");
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("sources.vproj"));
    VNX::ProjectFileBinding binding;
    binding.rootItem = std::make_shared<VNX::ProjectItem>(
        QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    binding.rootItem->guid = QUuid::createUuid();
    binding.rootItem->attachDefaultComponents();
    auto sound = std::make_shared<VNX::ProjectItem>(
        QStringLiteral("Multi"), VNX::ProjectItem::Type::Sound);
    sound->attachDefaultComponents();
    sound->objectID = 77;
    sound->guid = QUuid::createUuid();
    auto* ledger = sound->comp<VNX::VSound>();
    ASSERT_NE(ledger, nullptr);
    VNX::SoundSourceDef first;
    first.entryId = QUuid::createUuid();
    first.sourceId = 1;
    VNX::SoundSourceDef second;
    second.entryId = QUuid::createUuid();
    second.sourceId = 2;
    ledger->sources.plain.entries = {first, second};
    sound->parent = binding.rootItem.get();
    binding.rootItem->children.push_back(sound);
    project.layers.push_back(std::move(binding));

    for (const auto& active : {QUuid{}, first.entryId, second.entryId}) {
        ledger->sources.plain.active = active;
        const auto result = VNX::ProjectPackExporter::ValidatePackBuildReadiness(
            project, dir.path());
        EXPECT_TRUE(result.valid);
        EXPECT_EQ(result.errorCount, 0);
    }
}

TEST(PackGroupReadiness, PassCountShipsThroughCurrentWorldForContentObjects)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const auto build = [&dir](VNX::ProjectItem::Type type) {
        VNX::ProjectData project;
        project.name = QStringLiteral("passes");
        project.projectPath =
            QDir(dir.path()).filePath(QStringLiteral("passes.vproj"));
        VNX::ProjectFileBinding binding;
        binding.rootItem = std::make_shared<VNX::ProjectItem>(
            QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
        binding.rootItem->attachDefaultComponents();
        auto item = std::make_shared<VNX::ProjectItem>(
            QStringLiteral("Looping"), type);
        item->attachDefaultComponents();
        item->objectID = 91;
        auto* audio = item->comp<VNX::VAudio>();
        EXPECT_NE(audio, nullptr);
        if (audio)
            audio->loopCount = 3;
        item->parent = binding.rootItem.get();
        binding.rootItem->children.push_back(item);
        project.layers.push_back(std::move(binding));
        return VNX::ProjectPackExporter::ValidatePackBuildReadiness(
            project, dir.path());
    };

    const auto lostPassCount = [](const auto& result) {
        return std::any_of(
            result.issues.begin(), result.issues.end(), [](const auto& issue) {
                return issue.severity
                           == VNX::ProjectPackExporter::ValidationIssue::Error
                    && issue.message.contains(QStringLiteral("loopCount"));
            });
    };

    for (const auto type : {VNX::ProjectItem::Type::Sound,
                            VNX::ProjectItem::Type::Container,
                            VNX::ProjectItem::Type::Track,
                            VNX::ProjectItem::Type::Piece}) {
        EXPECT_FALSE(lostPassCount(build(type)));
    }
}

// --- CollectActionPackUnits (panel identity source: stableId / objectID / name) ---

TEST(PackGroupUnits, CollectActionPackUnitsMapsIdentityAndKeepsEmptyActions)
{
    VNX::ProjectData project;
    project.name = QStringLiteral("units");

    auto actionsRoot = std::make_shared<VNX::ProjectItem>();
    actionsRoot->name = QStringLiteral("ActionsRoot");
    actionsRoot->type = VNX::ProjectItem::Type::None;
    actionsRoot->objectID = 10;
    actionsRoot->attachDefaultComponents();

    auto withSteps = std::make_shared<VNX::ProjectItem>();
    withSteps->name = QStringLiteral("Fire");
    withSteps->type = VNX::ProjectItem::Type::Action;
    withSteps->objectID = 21;
    withSteps->guid = VNX::authoringUuid(harkentest::StudioObjectId(990021u));
    withSteps->attachDefaultComponents();
    {
        VNX::VActionStep play;
        play.type = VNX::VActionStep::Type::Play;
        withSteps->comp<VNX::VAction>()->steps.push_back(play);
    }
    withSteps->parent = actionsRoot.get();
    actionsRoot->children.push_back(withSteps);

    auto emptyAction = std::make_shared<VNX::ProjectItem>();
    emptyAction->name = QStringLiteral("EmptyAction");
    emptyAction->type = VNX::ProjectItem::Type::Action;
    emptyAction->objectID = 22;
    emptyAction->guid = VNX::authoringUuid(harkentest::StudioObjectId(990022u));
    emptyAction->attachDefaultComponents();
    emptyAction->parent = actionsRoot.get();
    actionsRoot->children.push_back(emptyAction);

    VNX::ProjectFileBinding binding;
    binding.rootItem = actionsRoot;
    project.actions.push_back(std::move(binding));

    const auto units = VNX::ProjectPackExporter::CollectActionPackUnits(project);
    ASSERT_EQ(units.size(), 2u);
    EXPECT_EQ(units[0].name, QStringLiteral("Fire"));
    EXPECT_EQ(units[0].objectID, 21u);
    EXPECT_EQ(units[0].stableId, harkentest::StudioObjectId(990021ull));
    EXPECT_EQ(units[1].name, QStringLiteral("EmptyAction"));
    EXPECT_EQ(units[1].objectID, 22u);
    EXPECT_EQ(units[1].stableId, harkentest::StudioObjectId(990022ull));

    const std::set<harkenstudio::ObjectId> ids =
        VNX::ProjectPackExporter::CollectActionStableIds(project);
    EXPECT_EQ(ids.count(harkentest::StudioObjectId(990021ull)), 1u);
    EXPECT_EQ(ids.count(harkentest::StudioObjectId(990022ull)), 1u);
}
