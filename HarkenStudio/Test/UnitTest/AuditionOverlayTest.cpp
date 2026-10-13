#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ActionManager.h"
#include "AudioEngine.h"
#include "AuditionOverlay.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "RuntimeSyncCoordinator.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonObject>
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

class AuditionOverlayTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ActionManager::instance().clearAllForTesting();
        registerMixerActions(ActionManager::instance(), nullptr,
                             &ProjectManager::instance(), nullptr);

        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("AuditionOverlayProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));

        ProjectItem* master = ProjectManager::instance().getMasterBusItem();
        ASSERT_NE(master, nullptr);
        busStableId_ = master->runtimeId();
        busName_ = master->name;
    }

    void TearDown() override
    {
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
    harkenstudio::ObjectId busStableId_ = harkentest::StudioObjectId(0);
    QString busName_;
};

QString projectFilesText(const QString& root)
{
    QString all;
    QDirIterator it(root,
                    QStringList() << QStringLiteral("*.hsp") << QStringLiteral("*.vp"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile f(it.next());
        if (f.open(QIODevice::ReadOnly))
            all += QString::fromUtf8(f.readAll());
    }
    return all;
}

} // namespace

TEST(AuditionOverlayUnit, SetClearSemanticsAndChangeSignal)
{
    ensureApp();
    AuditionOverlay overlay;
    int changes = 0;
    QObject::connect(&overlay, &AuditionOverlay::auditionChanged,
                     [&](harkenstudio::ObjectId) { ++changes; });

    constexpr harkenstudio::ObjectId kLargeBusId{0x1122334455664788ull, 0x100000007ull};
    EXPECT_TRUE(overlay.setMuted(kLargeBusId, true));
    EXPECT_FALSE(overlay.setMuted(kLargeBusId, true));
    EXPECT_TRUE(overlay.isMuted(kLargeBusId));
    EXPECT_FALSE(overlay.isMuted(harkentest::StudioObjectId(8)));

    EXPECT_TRUE(overlay.setSoloed(harkentest::StudioObjectId(9), true));
    EXPECT_TRUE(overlay.isSoloed(harkentest::StudioObjectId(9)));
    EXPECT_TRUE(overlay.anySolo());
    EXPECT_EQ(changes, 2);

    overlay.clearAll();
    EXPECT_FALSE(overlay.isMuted(kLargeBusId));
    EXPECT_FALSE(overlay.isSoloed(harkentest::StudioObjectId(9)));
    EXPECT_FALSE(overlay.anySolo());
    EXPECT_EQ(changes, 4);
}

TEST_F(AuditionOverlayTest, MuteSoloLeaveUndoStackAndCleanStateUntouched)
{
    IPCServer server(nullptr);
    auto* stack = UndoRedoManager::instance().undoStack();
    const int countBefore = stack->count();
    const bool cleanBefore = stack->isClean();

    const QJsonObject muteResp = server.dispatchCommand(
        QStringLiteral("set_mixer_mute"),
        QJsonObject{{QStringLiteral("bus"), busName_}, {QStringLiteral("mute"), true}});
    ASSERT_EQ(muteResp.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

    const QJsonObject soloResp = server.dispatchCommand(
        QStringLiteral("set_mixer_solo"),
        QJsonObject{{QStringLiteral("bus"), busName_}, {QStringLiteral("solo"), true}});
    ASSERT_EQ(soloResp.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

    auto& pm = ProjectManager::instance();
    EXPECT_TRUE(pm.auditionOverlay().isMuted(busStableId_));
    EXPECT_TRUE(pm.auditionOverlay().isSoloed(busStableId_));
    EXPECT_EQ(stack->count(), countBefore);
    EXPECT_EQ(stack->isClean(), cleanBefore);
}

TEST_F(AuditionOverlayTest, SetBusPropertiesMuteSoloOnlyAddsNoUndoEntry)
{
    IPCServer server(nullptr);
    auto* stack = UndoRedoManager::instance().undoStack();
    const int countBefore = stack->count();

    const QJsonObject muteSolo = server.dispatchCommand(
        QStringLiteral("set_bus_properties"),
        QJsonObject{{QStringLiteral("bus_id"), VNX::authoringUuid(busStableId_).toString(QUuid::WithoutBraces)},
                    {QStringLiteral("mute"), true},
                    {QStringLiteral("solo"), true}});
    ASSERT_EQ(muteSolo.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(stack->count(), countBefore);

    const QJsonObject volume = server.dispatchCommand(
        QStringLiteral("set_bus_properties"),
        QJsonObject{{QStringLiteral("bus_id"), VNX::authoringUuid(busStableId_).toString(QUuid::WithoutBraces)},
                    {QStringLiteral("volume"), -6.0}});
    ASSERT_EQ(volume.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(stack->count(), countBefore + 1);
}

TEST_F(AuditionOverlayTest, SaveProductCarriesNoMuteSoloKeys)
{
    IPCServer server(nullptr);
    server.dispatchCommand(
        QStringLiteral("set_mixer_mute"),
        QJsonObject{{QStringLiteral("bus"), busName_}, {QStringLiteral("mute"), true}});
    server.dispatchCommand(
        QStringLiteral("set_mixer_solo"),
        QJsonObject{{QStringLiteral("bus"), busName_}, {QStringLiteral("solo"), true}});
    ASSERT_TRUE(ProjectManager::instance().saveProject());

    const QString text = projectFilesText(tempDir_.path());
    ASSERT_FALSE(text.isEmpty());
    EXPECT_FALSE(text.contains(QStringLiteral("\"mute\"")));
    EXPECT_FALSE(text.contains(QStringLiteral("\"solo\"")));
}

TEST_F(AuditionOverlayTest, MuteSoloKeepsSessionOverlayWithoutMarkingWorldMutation)
{
    auto& coordinator = AudioEngine::instance().runtimeSyncCoordinator();
    const auto revisionBefore = coordinator.currentAuthoringRevision();
    IPCServer server(nullptr);

    const QJsonObject muteResp = server.dispatchCommand(
        QStringLiteral("set_mixer_mute"),
        QJsonObject{{QStringLiteral("bus"), busName_}, {QStringLiteral("mute"), true}});
    ASSERT_EQ(muteResp.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_TRUE(ProjectManager::instance().auditionOverlay().isMuted(busStableId_));
    EXPECT_EQ(coordinator.currentAuthoringRevision(), revisionBefore);

    const QJsonObject soloResp = server.dispatchCommand(
        QStringLiteral("set_mixer_solo"),
        QJsonObject{{QStringLiteral("bus"), busName_}, {QStringLiteral("solo"), true}});
    ASSERT_EQ(soloResp.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_TRUE(ProjectManager::instance().auditionOverlay().isSoloed(busStableId_));
    EXPECT_EQ(coordinator.currentAuthoringRevision(), revisionBefore);
}

TEST_F(AuditionOverlayTest, CloseProjectClearsOverlay)
{
    IPCServer server(nullptr);
    server.dispatchCommand(
        QStringLiteral("set_mixer_mute"),
        QJsonObject{{QStringLiteral("bus"), busName_}, {QStringLiteral("mute"), true}});
    server.dispatchCommand(
        QStringLiteral("set_mixer_solo"),
        QJsonObject{{QStringLiteral("bus"), busName_}, {QStringLiteral("solo"), true}});

    auto& pm = ProjectManager::instance();
    ASSERT_TRUE(pm.auditionOverlay().isMuted(busStableId_));
    ASSERT_TRUE(pm.auditionOverlay().anySolo());

    pm.closeProject();

    EXPECT_FALSE(pm.auditionOverlay().isMuted(busStableId_));
    EXPECT_FALSE(pm.auditionOverlay().isSoloed(busStableId_));
    EXPECT_FALSE(pm.auditionOverlay().anySolo());
}
