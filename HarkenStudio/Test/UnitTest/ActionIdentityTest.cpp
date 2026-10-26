#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "ProjectObjectIndex.h"
#include "Components/VMixNode.h"
#include "Components/VTrack.h"
#include "Components/VPiece.h"
#include "UndoCommands.h"

#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QTemporaryDir>

using namespace VNX;

namespace {

using TestSupport::ensureApp;

class ActionIdentityTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("ActionIdentityProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        projectPath_ = manager.currentProject()->projectPath;
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    ProjectItem* createAction(const QString& name, const QString& path = QString())
    {
        return ProjectManager::instance().createItem(
            nullptr, name, ProjectItem::Type::Action, path);
    }

    void reopen()
    {
        auto& manager = ProjectManager::instance();
        ASSERT_TRUE(manager.closeProject());
        ASSERT_TRUE(manager.loadProject(projectPath_));
    }

    QTemporaryDir tempDir_;
    QString projectPath_;
};

}

TEST_F(ActionIdentityTest, ActionDuplicateStableIdsRefuse)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* first = createAction(QStringLiteral("Play_One"));
    ProjectItem* second = createAction(QStringLiteral("Play_Two"));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const harkenstudio::ObjectId firstId = first->runtimeId();
    const harkenstudio::ObjectId secondId = second->runtimeId();
    ASSERT_NE(firstId, secondId);
    second->guid = VNX::authoringUuid(harkentest::StudioObjectId(firstId));
    QString error;
    EXPECT_FALSE(validateProjectObjectIdentities(*manager.currentProject(), &error));
    EXPECT_EQ(first->runtimeId(), firstId);
    EXPECT_EQ(second->runtimeId(), firstId);
    second->guid = VNX::authoringUuid(harkentest::StudioObjectId(secondId));
}

TEST_F(ActionIdentityTest, MissingPublishedActionIdRefuses)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* action = createAction(QStringLiteral("Play_Published"));
    ASSERT_NE(action, nullptr);
    const harkenstudio::ObjectId publishedId = action->runtimeId();
    ASSERT_NE(publishedId, harkenstudio::ObjectId{});
    action->guid = VNX::authoringUuid(harkentest::StudioObjectId(0));
    QString error;
    EXPECT_FALSE(validateProjectObjectIdentities(*manager.currentProject(), &error));
    EXPECT_EQ(action->runtimeId(), harkenstudio::ObjectId{});
    action->guid = VNX::authoringUuid(harkentest::StudioObjectId(publishedId));
}

TEST_F(ActionIdentityTest, NestedObjectIndexRejectsAmbiguousOwners)
{
    auto& manager = ProjectManager::instance();
    auto* sound = manager.createItem(nullptr, QStringLiteral("Tone"), ProjectItem::Type::Sound);
    auto* track = manager.createItem(nullptr, QStringLiteral("Track"), ProjectItem::Type::Track);
    auto* piece = manager.createItem(nullptr, QStringLiteral("Piece"), ProjectItem::Type::Piece);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(piece, nullptr);
    EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.parameters = {0.0f};
    auto* mix = sound->comp<VMixNode>();
    ASSERT_TRUE(mix->effects.replace({effect}));
    auto binding = BindingSemantics::defaultBinding(BindingSemantics::volumeAddress(), QUuid::createUuid());
    binding.guid = QUuid::createUuid();
    ASSERT_TRUE(mix->bindings.upsert(binding));
    Clip clip{QUuid::createUuid(), sound->guid, 0, 1000000000};
    ASSERT_TRUE(track->comp<VTrack>()->clips.mintOccurrenceOrdinal(&clip));
    track->comp<VTrack>()->clips.items.push_back(clip);
    Marker marker{QUuid::createUuid(), MarkerKind::Custom, QStringLiteral("Accent"), 0};
    piece->comp<VPiece>()->markers.items.push_back(marker);
    const auto index = ProjectObjectIndex::fromProject(manager.currentProject());
    for (const auto id : {sound->runtimeId(), effect.runtimeId(), binding.runtimeId(),
                           clip.runtimeId(), marker.runtimeId()})
        ASSERT_NE(index.find(id), nullptr);
    EXPECT_EQ(index.find(effect.runtimeId())->kind, ProjectObjectLocation::Kind::Effect);
    EXPECT_EQ(index.find(binding.runtimeId())->kind, ProjectObjectLocation::Kind::Binding);
    EXPECT_EQ(index.find(clip.runtimeId())->kind, ProjectObjectLocation::Kind::Clip);
    EXPECT_EQ(index.find(marker.runtimeId())->kind, ProjectObjectLocation::Kind::Marker);
    EXPECT_EQ(index.find(effect.runtimeId())->ownerGuid, sound->guid);
    EXPECT_EQ(index.find(clip.runtimeId())->ownerGuid, track->guid);
    EXPECT_EQ(index.find(marker.runtimeId())->ownerGuid, piece->guid);
    const auto excluded = ProjectObjectIndex::fromProject(manager.currentProject(), sound);
    EXPECT_EQ(excluded.find(sound->runtimeId()), nullptr);
    EXPECT_EQ(excluded.find(effect.runtimeId()), nullptr);
    EXPECT_EQ(excluded.find(binding.runtimeId()), nullptr);
    ASSERT_NE(excluded.find(clip.runtimeId()), nullptr);
    auto* duplicate = manager.createItem(nullptr, QStringLiteral("Duplicate"), ProjectItem::Type::Sound);
    ASSERT_NE(duplicate, nullptr);
    const QUuid original = duplicate->guid;
    duplicate->guid = sound->guid;
    const auto ambiguous = ProjectObjectIndex::fromProject(manager.currentProject());
    EXPECT_EQ(ambiguous.find(sound->runtimeId()), nullptr);
    EXPECT_EQ(ambiguous.find(effect.runtimeId()), nullptr);
    EXPECT_EQ(ambiguous.find(binding.runtimeId()), nullptr);
    QString error;
    EXPECT_FALSE(validateProjectObjectIdentities(*manager.currentProject(), &error));
    EXPECT_EQ(duplicate->guid, sound->guid);
    duplicate->guid = original;
}

TEST_F(ActionIdentityTest, DuplicateMintsNestedUuidsAndSaveReloadKeepsThem)
{
    auto& manager = ProjectManager::instance();
    auto* sound = manager.createItem(nullptr, QStringLiteral("Tone"), ProjectItem::Type::Sound);
    auto* variable = manager.createItem(nullptr, QStringLiteral("Intensity"), ProjectItem::Type::Variable);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(variable, nullptr);
    EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.parameters = {-3.0f};
    auto binding = BindingSemantics::defaultBinding(BindingSemantics::volumeAddress(), variable->guid);
    binding.guid = QUuid::createUuid();
    ASSERT_TRUE(sound->comp<VMixNode>()->effects.replace({effect}));
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(binding));
    auto* copy = manager.duplicateItem(sound);
    ASSERT_NE(copy, nullptr);
    auto* copiedMix = copy->comp<VMixNode>();
    ASSERT_EQ(copiedMix->effects.size(), 1u);
    ASSERT_EQ(copiedMix->bindings.size(), 1u);
    const auto copiedEffect = copiedMix->effects.entries().front().runtimeId();
    const auto copiedBinding = copiedMix->bindings.entries().front().runtimeId();
    const auto copiedGuid = copy->guid;
    EXPECT_NE(copy->guid, sound->guid);
    EXPECT_NE(copiedEffect, effect.runtimeId());
    EXPECT_NE(copiedBinding, binding.runtimeId());
    EXPECT_EQ(copiedMix->bindings.entries().front().variable, variable->guid);
    ASSERT_TRUE(manager.saveProject());
    ASSERT_NO_FATAL_FAILURE(reopen());
    copy = manager.findItemByGuid(copiedGuid);
    ASSERT_NE(copy, nullptr);
    copiedMix = copy->comp<VMixNode>();
    ASSERT_NE(copiedMix, nullptr);
    EXPECT_EQ(copiedMix->effects.entries().front().runtimeId(), copiedEffect);
    EXPECT_EQ(copiedMix->bindings.entries().front().runtimeId(), copiedBinding);
}
