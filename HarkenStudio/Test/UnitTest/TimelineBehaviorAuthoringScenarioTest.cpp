#include "ActionManager.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetTypes.h"
#include "Components/VAudio.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "ContainerBehaviorSemantics.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectManager.h"
#include "SoundSourceEditing.h"
#include "SoundSourceSemantics.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>
#include "Support/ProjectPlatformTestUtils.h"

#include <QApplication>
#include <QTemporaryDir>

using namespace VNX;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

ActionResult setProperty(ProjectItem* item, const QString& property,
                         const QJsonValue& value)
{
    return ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
         {QStringLiteral("property"), property},
         {QStringLiteral("value"), value}}, ActionSource::GUI);
}

class TimelineBehaviorAuthoringScenarioTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        ASSERT_TRUE(temp_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("TimelineBehaviorAuthoringProject");
        config.location = temp_.path();
        config.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
        ASSERT_TRUE(manager.createProject(config));
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        if (ProjectManager::instance().isProjectOpen())
            ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir temp_;
};

}

TEST_F(TimelineBehaviorAuthoringScenarioTest, BehaviorLoopSourcesTimelineUndoAndReloadStayCanonical)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* layer = manager.createItem(nullptr, QStringLiteral("Layer"), ProjectItem::Type::Layer);
    ProjectItem* container = manager.createItem(layer, QStringLiteral("Container"), ProjectItem::Type::Container);
    ProjectItem* first = manager.createItem(container, QStringLiteral("First"), ProjectItem::Type::Sound);
    ProjectItem* second = manager.createItem(container, QStringLiteral("Second"), ProjectItem::Type::Sound);
    ASSERT_NE(second, nullptr);
    UndoRedoManager::instance().clear();

    const QJsonObject behavior{
        {QStringLiteral("discipline"), QStringLiteral("shuffle")},
        {QStringLiteral("avoidRepeat"), 1}
    };
    ASSERT_TRUE(setProperty(container, QStringLiteral("behavior"), behavior).success);
    EXPECT_EQ(container->comp<VNX::VSelection>()->behavior.discipline, RandomDiscipline::Shuffle);
    auto selections = container->comp<VNX::VSelection>()->selections;
    selections.shared.find(selections.shared.occurrenceRows().front().stableOccurrenceOrdinal)->weight = 80;
    ASSERT_TRUE(setProperty(container, QStringLiteral("selections"), selections.toJson()).success);

    ASSERT_TRUE(setProperty(first, QStringLiteral("loopCount"), 3).success);
    EXPECT_EQ(first->comp<VAudio>()->loopCount, 3u);

    AssetInfo asset;
    asset.sourceID = 9201;
    asset.relativePath = QStringLiteral("phase5.wav");
    asset.absolutePath = temp_.path() + QStringLiteral("/phase5.wav");
    asset.metadata.sampleRate = 48000;
    asset.metadata.totalSamples = 480000;
    asset.metadata.durationSec = 10.0;
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 96000;
    source.trimEndFrame = 384000;
    source.loopRegion = SoundLoopRegion{144000, 288000};
    SoundSources sources;
    sources.plain.entries = {source};
    sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = source.entryId;
    const QJsonObject ledger = SoundSourceSemantics::toJson(sources);
    ASSERT_TRUE(setProperty(first, QStringLiteral("sources"), ledger).success);
    ASSERT_TRUE(setProperty(first, QStringLiteral("scheduleOffsetSec"), 0.5).success);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(
                  *first, HarkenTest::PlatformIds::Windows, QString()),
              asset.sourceID);
    EXPECT_DOUBLE_EQ(*SoundSourceEditing::activeTrimmedDuration(
                         *first, HarkenTest::PlatformIds::Windows, QString()),
                     6.0);

    const int beforeUndo = UndoRedoManager::instance().undoStack()->count();
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_DOUBLE_EQ(first->comp<VSound>()->scheduleOffsetSec, 0.0);
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_DOUBLE_EQ(first->comp<VSound>()->scheduleOffsetSec, 0.5);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), beforeUndo);

    const QUuid containerGuid = container->guid;
    const QUuid soundGuid = first->guid;
    const QJsonObject expectedBehavior = ContainerBehaviorSemantics::toJson(
        container->comp<VNX::VSelection>()->behavior);
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    ProjectItem* reloadedContainer = manager.findItemByGuid(containerGuid);
    ProjectItem* reloadedSound = manager.findItemByGuid(soundGuid);
    ASSERT_NE(reloadedContainer, nullptr);
    ASSERT_NE(reloadedSound, nullptr);
    EXPECT_EQ(ContainerBehaviorSemantics::toJson(reloadedContainer->comp<VNX::VSelection>()->behavior),
              expectedBehavior);
    EXPECT_EQ(reloadedSound->comp<VAudio>()->loopCount, 3u);
    EXPECT_DOUBLE_EQ(reloadedSound->comp<VSound>()->scheduleOffsetSec, 0.5);
    EXPECT_EQ(SoundSourceSemantics::toJson(reloadedSound->comp<VSound>()->sources),
              ledger);
}
