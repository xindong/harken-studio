#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Components/VAction.h"
#include "Components/VSound.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "SoundSourceSemantics.h"
#include "UndoCommands.h"
#include "HarkenMath.h"

#include "RuntimeBridge/ProjectSyncBridge.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

#include <memory>

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


class RuntimeSyncPresenceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();

        VnConfig config{};
        config.structSize = sizeof(VnConfig);
        config.deviceMode = VN_DEVICE_NULL;
        ASSERT_EQ(VN_OK, Vn_Init(&config));
        HarkenEditor::Bridge::clearRuntimeObjects();

        server_ = std::make_unique<IPCServer>(nullptr);
        server_->setProjectManager(&ProjectManager::instance());

        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig projectConfig;
        projectConfig.name = QStringLiteral("RuntimePresenceProject");
        projectConfig.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(projectConfig));
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        server_.reset();
        HarkenEditor::Bridge::clearRuntimeObjects();
        Vn_Term();
    }

    static bool bridgeHas(const ProjectItem* item)
    {
        return HarkenEditor::Bridge::runtimeObjectExists(item->runtimeId());
    }

    QTemporaryDir tempDir_;
    std::unique_ptr<IPCServer> server_;
};

} // namespace

TEST_F(RuntimeSyncPresenceTest, PresenceMatchesTypeContractAcrossAllTypes)
{
    auto& pm = ProjectManager::instance();

    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("Pres_Layer"),
                                       ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    EXPECT_TRUE(bridgeHas(layer));

    ProjectItem* sound = pm.createAudioItem(layer, QStringLiteral("Pres_Sound"),
                                            QStringLiteral("pres.wav"), 4242);
    ASSERT_NE(sound, nullptr);
    EXPECT_TRUE(bridgeHas(sound));

    ProjectItem* container = pm.createItem(layer, QStringLiteral("Pres_Container"),
                                           ProjectItem::Type::Container);
    ProjectItem* track = pm.createItem(layer, QStringLiteral("Pres_Track"),
                                       ProjectItem::Type::Track);
    ProjectItem* piece = pm.createItem(layer, QStringLiteral("Pres_Piece"),
                                       ProjectItem::Type::Piece);
    ProjectItem* variable = pm.createItem(nullptr, QStringLiteral("Pres_Variable"),
                                          ProjectItem::Type::Variable);
    ProjectItem* attenuation = pm.createItem(nullptr, QStringLiteral("Pres_Attenuation"),
                                             ProjectItem::Type::Attenuation);
    auto* folder = pm.createFolder(nullptr, QStringLiteral("Pres_Folder"), QStringLiteral("Actions"));
    ASSERT_NE(container, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(piece, nullptr);
    ASSERT_NE(variable, nullptr);
    ASSERT_NE(attenuation, nullptr);
    ASSERT_NE(folder, nullptr);
    EXPECT_FALSE(bridgeHas(container));
    EXPECT_FALSE(bridgeHas(track));
    EXPECT_FALSE(bridgeHas(piece));
    EXPECT_FALSE(bridgeHas(variable));
    EXPECT_FALSE(bridgeHas(attenuation));

    ProjectItem* master = pm.getMasterBusItem();
    ASSERT_NE(master, nullptr);
    ProjectItem* bus = pm.createItem(master, QStringLiteral("Pres_Bus"),
                                     ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    EXPECT_FALSE(bridgeHas(bus));

    ProjectItem* action = pm.createActionTargeting(
        sound, VActionStep::Type::Play, QStringLiteral("Pres_Action"));
    ASSERT_NE(action, nullptr);
    EXPECT_FALSE(bridgeHas(action));

    ASSERT_TRUE(pm.deleteItem(action));
}

TEST_F(RuntimeSyncPresenceTest, DeepSoundSourceSemanticsPreserveSoundBinding)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("Deep_Layer"),
                                       ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* sound = pm.createAudioItem(layer, QStringLiteral("Deep_Sound"),
                                            QStringLiteral("deep.wav"), 4400);
    ASSERT_NE(sound, nullptr);
    EXPECT_TRUE(bridgeHas(sound));

    auto* vs = sound->comp<VSound>();
    ASSERT_NE(vs, nullptr);
    ASSERT_EQ(vs->sources.plain.entries.size(), 1u);

    vs->scheduleOffsetSec = 0.25;
    ProjectManager::resyncItemToRuntime(sound);
    EXPECT_TRUE(bridgeHas(sound));

    vs->scheduleOffsetSec = 0.0;
    ProjectManager::resyncItemToRuntime(sound);
    EXPECT_TRUE(bridgeHas(sound));

    vs->sources.plain.entries.front().loopRegion = SoundLoopRegion{24000, 72000};
    ProjectManager::resyncItemToRuntime(sound);
    EXPECT_TRUE(bridgeHas(sound));

    vs->sources.plain.entries.front().loopRegion.reset();
    ProjectManager::resyncItemToRuntime(sound);
    EXPECT_TRUE(bridgeHas(sound));

    SoundSourceDef second;
    second.entryId = QUuid::createUuid();
    second.sourceId = 4401;
    second.trimStartFrame = 24000;
    vs->sources.plain.entries.push_back(second);
    ProjectManager::resyncItemToRuntime(sound);
    EXPECT_TRUE(bridgeHas(sound));

    ASSERT_FALSE(vs->sources.plain.active.isNull());
    vs->sources.plain.active = second.entryId;
    ProjectManager::resyncItemToRuntime(sound);
    EXPECT_TRUE(bridgeHas(sound));

    vs->sources.plain.active = vs->sources.plain.entries.front().entryId;
    SoundSourceSemantics::removeEntry(vs->sources, vs->sources.plain.entries[1].entryId);
    ProjectManager::resyncItemToRuntime(sound);
    EXPECT_TRUE(bridgeHas(sound));
}

TEST_F(RuntimeSyncPresenceTest, NewTypesNeverMaterializeAcrossLifecycle)
{
    auto& pm = ProjectManager::instance();

    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("Mat_Layer"),
                                       ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    const uint32_t baseline = HarkenEditor::Bridge::runtimeObjectCount();

    ProjectItem* container = pm.createItem(layer, QStringLiteral("Mat_Container"),
                                           ProjectItem::Type::Container);
    ProjectItem* track = pm.createItem(layer, QStringLiteral("Mat_Track"),
                                       ProjectItem::Type::Track);
    ProjectItem* piece = pm.createItem(layer, QStringLiteral("Mat_Piece"),
                                       ProjectItem::Type::Piece);
    ProjectItem* variable = pm.createItem(nullptr, QStringLiteral("Mat_Variable"),
                                          ProjectItem::Type::Variable);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(piece, nullptr);
    ASSERT_NE(variable, nullptr);
    EXPECT_EQ(baseline, HarkenEditor::Bridge::runtimeObjectCount());

    EXPECT_TRUE(pm.applyItemPropertyLive(container, QStringLiteral("volume"), -6.0f));
    EXPECT_TRUE(pm.applyItemPropertyLive(track, QStringLiteral("volume"), -3.0f));
    EXPECT_TRUE(pm.applyItemPropertyLive(piece, QStringLiteral("volume"), -1.5f));
    EXPECT_TRUE(pm.setItemColor(variable, 0x00FF00FF));
    EXPECT_EQ(baseline, HarkenEditor::Bridge::runtimeObjectCount());

    ASSERT_TRUE(pm.moveItem(container, track));
    ASSERT_TRUE(pm.moveItem(piece, container));
    EXPECT_FALSE(bridgeHas(container));
    EXPECT_FALSE(bridgeHas(track));
    EXPECT_FALSE(bridgeHas(piece));
    EXPECT_EQ(baseline, HarkenEditor::Bridge::runtimeObjectCount());

    ASSERT_TRUE(pm.deleteItem(piece));
    ASSERT_TRUE(pm.deleteItem(container));
    ASSERT_TRUE(pm.deleteItem(track));
    ASSERT_TRUE(pm.deleteItem(variable));
    EXPECT_FALSE(bridgeHas(variable));
    EXPECT_EQ(baseline, HarkenEditor::Bridge::runtimeObjectCount());
}

TEST_F(RuntimeSyncPresenceTest, DeletePathUnregistersByRuntimeStableId)
{
    auto& pm = ProjectManager::instance();

    ProjectItem* layer = pm.createItem(nullptr, QStringLiteral("Del_Layer"),
                                       ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* sound = pm.createAudioItem(layer, QStringLiteral("Del_Sound"),
                                            QStringLiteral("del.wav"), 4243);
    ASSERT_NE(sound, nullptr);

    const harkenstudio::ObjectId stableId = sound->runtimeId();
    const uint64_t nameHashId =
        HarkenEditor::hashString(sound->name.toUtf8().constData());
    ASSERT_NE(stableId, harkentest::StudioObjectId(nameHashId));
    EXPECT_TRUE(HarkenEditor::Bridge::runtimeObjectExists(stableId));
    EXPECT_FALSE(HarkenEditor::Bridge::runtimeObjectExists(harkentest::StudioObjectId(nameHashId)));

    const uint32_t countBefore = HarkenEditor::Bridge::runtimeObjectCount();
    ASSERT_TRUE(pm.deleteItem(sound));
    EXPECT_FALSE(HarkenEditor::Bridge::runtimeObjectExists(stableId));
    EXPECT_EQ(countBefore - 1, HarkenEditor::Bridge::runtimeObjectCount());
}
