#include "ActionManager.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include "Support/ProjectPlatformTestUtils.h"
#include "Core/SoundSourceSemantics.h"

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class TargetPlatformsCommandTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(),
                                    &VNX::ProjectManager::instance(), nullptr, {});

        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("TargetPlatformsProject");
        config.location = tempDir_.path();
        config.platforms = HarkenTest::platforms(QStringList{QStringLiteral("Windows"),
                                             QStringLiteral("macOS"),
                                             QStringLiteral("Android")});
        ASSERT_TRUE(VNX::ProjectManager::instance().createProject(config));

        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ProjectManager::instance().closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    VNX::ActionResult setTargetPlatforms(const QStringList& platforms)
    {
        QJsonArray arr;
        for (const auto& p : platforms) arr.append(p);
        return VNX::ActionManager::instance().execute(
            QStringLiteral("set_target_platforms"),
            QJsonObject{{"platforms", arr}});
    }

    VNX::ActionResult setSelectedPlatforms(const QStringList& platforms)
    {
        QJsonArray arr;
        for (const auto& p : platforms) arr.append(p);
        return VNX::ActionManager::instance().execute(
            QStringLiteral("set_pack_export_selected_platforms"),
            QJsonObject{{"platform_ids", arr}});
    }

    QStringList targets() const
    {
        return VNX::ProjectManager::instance().getTargetPlatforms();
    }

    QStringList scope() const
    {
        return VNX::ProjectManager::instance().getPackExportSelectedPlatforms();
    }

    void narrowScope(const QStringList& platforms)
    {
        VNX::ProjectManager::instance().setPackExportSelectedPlatformsInternal(platforms);
    }

    QUndoStack* stack() { return VNX::UndoRedoManager::instance().undoStack(); }

    QTemporaryDir tempDir_;
};

}

TEST_F(TargetPlatformsCommandTest, LegacyTargetListIsRejectedWithoutMutation)
{
    const auto before = targets();
    const auto beforeScope = scope();
    const auto result = setTargetPlatforms({QStringLiteral("Windows"), QStringLiteral("Android")});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("UNSUPPORTED_PLATFORM_FORMAT"));
    EXPECT_EQ(targets(), before);
    EXPECT_EQ(scope(), beforeScope);
    EXPECT_EQ(stack()->count(), 0);
}

TEST_F(TargetPlatformsCommandTest, InstanceSelectionDoesNotEnterProjectUndoHistory)
{
    const QStringList selected{HarkenTest::platformIdString(QStringLiteral("Android"))};
    ASSERT_TRUE(setSelectedPlatforms(selected).success);
    EXPECT_EQ(scope(), selected);
    EXPECT_EQ(stack()->count(), 0);
    EXPECT_TRUE(stack()->isClean());
}

TEST_F(TargetPlatformsCommandTest, NamesUnknownIdsAndDuplicatesAreRejected)
{
    const auto before = scope();
    const auto id = before.first();
    for (const QStringList invalid : {QStringList{QStringLiteral("Android")},
                                     QStringList{VNX::ProjectPlatformSemantics::idString(QUuid::createUuid())},
                                     QStringList{id, id}}) {
        EXPECT_FALSE(setSelectedPlatforms(invalid).success);
        EXPECT_EQ(scope(), before);
        EXPECT_EQ(stack()->count(), 0);
    }
}

TEST_F(TargetPlatformsCommandTest, EmptySelectionIsExplicitAndNoOpAddsNoUndo)
{
    ASSERT_TRUE(setSelectedPlatforms(scope()).success);
    EXPECT_EQ(stack()->count(), 0);
    ASSERT_TRUE(setSelectedPlatforms({}).success);
    EXPECT_TRUE(scope().isEmpty());
    EXPECT_FALSE(targets().isEmpty());
    EXPECT_EQ(stack()->count(), 0);
}

TEST_F(TargetPlatformsCommandTest, InvalidActionTargetingDoesNotResetExistingFilter)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* action = manager.createItem(nullptr, QStringLiteral("Action"), VNX::ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    manager.setActionPackTargetingInternal({{action->runtimeId(), false, {targets().first()}}});
    const auto before = manager.getActionPackTargeting();
    stack()->clear();
    for (const auto& invalid : {QJsonObject{{"platforms", QJsonArray{QStringLiteral("Android")}}},
                               QJsonObject{{"platform_ids", QStringLiteral("Android")}},
                               QJsonObject{{"platform_ids", QJsonArray{QUuid::createUuid().toString(QUuid::WithoutBraces)}}}}) {
        auto args = invalid;
        args[QStringLiteral("stableId")] = VNX::objectIdString(action->runtimeId());
        const auto result = VNX::ActionManager::instance().execute(QStringLiteral("set_action_pack_targeting"), args);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(manager.getActionPackTargeting(), before);
        EXPECT_EQ(stack()->count(), 0);
    }
}

TEST_F(TargetPlatformsCommandTest, LanguageTargetingPersistsLocallyAcrossPlatformRemovalAndProjectReopen)
{
    auto& manager = VNX::ProjectManager::instance();
    manager.setProjectLanguagesInternal({"English", "Japanese"});
    auto* action = manager.createItem(nullptr, "Voice", VNX::ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    const auto id = action->runtimeId();
    const auto guid = action->guid;
    const auto windows = HarkenTest::platformIdString("Windows");
    const auto mac = HarkenTest::platformIdString("macOS");
    const auto android = HarkenTest::platformIdString("Android");
    stack()->clear();
    const QJsonObject args{{"stableId", VNX::objectIdString(id)}, {"language", "English"},
                           {"allPlatforms", false}, {"platform_ids", QJsonArray{mac, android}}};
    auto& actions = VNX::ActionManager::instance();
    ASSERT_TRUE(actions.execute("set_action_pack_targeting", args).success);
    EXPECT_FALSE(manager.packExportSelection().includesActionPack(id, windows, "English"));
    EXPECT_TRUE(manager.packExportSelection().includesActionPack(id, mac, "English"));
    EXPECT_TRUE(manager.packExportSelection().includesActionPack(id, windows, "Japanese"));
    const auto selected = manager.getActionPackTargeting();
    EXPECT_EQ(stack()->count(), 0);
    ASSERT_TRUE(actions.execute("set_action_pack_targeting", args).success);
    EXPECT_EQ(stack()->count(), 0);
    auto invalid = args;
    invalid["language"] = "Unknown";
    EXPECT_FALSE(actions.execute("set_action_pack_targeting", invalid).success);
    EXPECT_EQ(manager.getActionPackTargeting(), selected);
    EXPECT_TRUE(stack()->isClean());
    ASSERT_TRUE(actions.execute("delete_project_platform", {{"platform_id", mac}}).success);
    EXPECT_FALSE(manager.getActionPackTargeting().front().platformIds.contains(mac));
    stack()->undo();
    const auto restored = manager.packExportSelection();
    EXPECT_FALSE(restored.includesActionPack(id, windows, "English"));
    EXPECT_TRUE(restored.includesActionPack(id, mac, "English"));
    EXPECT_TRUE(restored.includesActionPack(id, android, "English"));
    EXPECT_TRUE(restored.includesActionPack(id, windows, "Japanese"));
    ASSERT_TRUE(manager.saveProject());
    const auto path = manager.currentProject()->projectPath;
    manager.closeProject();
    auto reopened = VNX::ProjectManager::prepareProject(path);
    ASSERT_NE(reopened.project, nullptr) << reopened.error.toStdString();
    ASSERT_TRUE(manager.commitPreparedProject(std::move(reopened)));
    EXPECT_EQ(manager.getActionPackTargeting(), restored.actionTargeting);
    ASSERT_TRUE(manager.deleteItem(manager.findItemByGuid(guid)));
    EXPECT_TRUE(manager.getActionPackTargeting().empty());
    stack()->undo();
    EXPECT_EQ(manager.getActionPackTargeting(), restored.actionTargeting);
}

TEST_F(TargetPlatformsCommandTest, UnknownSoundPlatformIsRejectedBeforeMutation)
{
    auto* item = VNX::ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(item, nullptr);
    ASSERT_TRUE(VNX::ProjectManager::instance().saveProject());
    stack()->clear();
    VNX::SoundSources sources;
    sources.plain.platforms[QUuid::createUuid().toString(QUuid::WithoutBraces)].active = QUuid{};
    const QJsonObject ledger = VNX::SoundSourceSemantics::toJson(sources);
    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{{"authoring_handle", static_cast<qint64>(item->objectID)},
                    {"property", QStringLiteral("sources")}, {"value", ledger}}, VNX::ActionSource::Internal);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.contains(QStringLiteral("platform ID")));
    EXPECT_TRUE(item->comp<VNX::VSound>()->sources.plain.platforms.isEmpty());
    EXPECT_FALSE(item->isDirtyObject());
    EXPECT_EQ(stack()->count(), 0);
}
