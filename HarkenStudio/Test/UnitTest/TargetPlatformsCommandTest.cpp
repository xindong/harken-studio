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

TEST_F(TargetPlatformsCommandTest, InstanceSelectionRoundTripsThroughUndoRedo)
{
    const auto before = scope();
    const QStringList selected{HarkenTest::platformIdString(QStringLiteral("Android"))};
    ASSERT_TRUE(setSelectedPlatforms(selected).success);
    EXPECT_EQ(scope(), selected);
    stack()->undo();
    EXPECT_EQ(scope(), before);
    stack()->redo();
    EXPECT_EQ(scope(), selected);
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
    EXPECT_EQ(stack()->count(), 1);
}

TEST_F(TargetPlatformsCommandTest, InvalidActionTargetingDoesNotResetExistingFilter)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* action = manager.createItem(nullptr, QStringLiteral("Action"), VNX::ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    manager.currentProject()->actionPackTargeting = {{action->runtimeId(), false, {targets().first()}}};
    const auto before = manager.currentProject()->actionPackTargeting;
    stack()->clear();
    for (const auto& invalid : {QJsonObject{{"platforms", QJsonArray{QStringLiteral("Android")}}},
                               QJsonObject{{"platform_ids", QStringLiteral("Android")}},
                               QJsonObject{{"platform_ids", QJsonArray{QUuid::createUuid().toString(QUuid::WithoutBraces)}}}}) {
        auto args = invalid;
        args[QStringLiteral("stableId")] = VNX::objectIdString(action->runtimeId());
        const auto result = VNX::ActionManager::instance().execute(QStringLiteral("set_action_pack_targeting"), args);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(manager.currentProject()->actionPackTargeting, before);
        EXPECT_EQ(stack()->count(), 0);
    }
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
