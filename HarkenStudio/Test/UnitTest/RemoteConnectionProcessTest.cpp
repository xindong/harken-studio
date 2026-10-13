#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "RuntimeBridge/RemoteConnection.h"
#include "Monitor/EventCaptureController.h"
#include "ProjectTypes.h"
#include "Core/Components/VMixNode.h"
#include "Widgets/Monitor/MonitorCatalogPanel.h"
#include "UnitTest/RemoteGameFixture.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QLineEdit>
#include <QTableView>
#include <QToolButton>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

#include <algorithm>
#include <functional>

namespace {

using HarkenEditor::Bridge::RemoteConnection;
using HarkenEditor::Bridge::RemoteConnectionState;

bool waitForRemote(const std::function<bool()>& predicate, int timeout = 7000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return predicate();
}

class GameProcess {
  public:
    QProcess process;
    QJsonObject ready;
    QString stopPath;

    bool start(const QString& directory, const QString& stop, quint16 discoveryPort) {
        stopPath = stop;
        process.start(qEnvironmentVariable("HARKEN_REMOTE_TEST_GAME", QStringLiteral(HARKEN_REMOTE_GAME_PATH)),
                      {directory, stop, QString::number(discoveryPort)});
        if (!process.waitForStarted(5000)) {
            return false;
        }
        QByteArray output;
        const bool received = waitForRemote([&] {
            output += process.readAllStandardOutput();
            return output.contains('\n') || process.state() == QProcess::NotRunning;
        });
        ready = QJsonDocument::fromJson(output.trimmed()).object();
        return received && ready.value(QStringLiteral("port")).toInt() > 0;
    }

    bool stop() {
        QFile signal(stopPath);
        if (!signal.open(QIODevice::WriteOnly)) {
            return false;
        }
        signal.close();
        return process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit &&
               process.exitCode() == 0;
    }

    ~GameProcess() {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
    }
};

TEST(RemoteConnectionProcess, SharedDllDiscoveryAudioControlMonitorReconnectAndTerm) {
    ASSERT_NE(qApp, nullptr);
    QTemporaryDir directory(QStringLiteral(HARKEN_TEMP_DIR "/remote-game-XXXXXX"));
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(harkentest::remote::WriteGamePacks(
        std::filesystem::path(directory.path().toStdU16String())));
    QUdpSocket portReservation;
    ASSERT_TRUE(portReservation.bind(QHostAddress::AnyIPv4, 0));
    const auto discoveryPort = portReservation.localPort();
    portReservation.close();
    GameProcess game;
    ASSERT_TRUE(
        game.start(directory.path(), directory.filePath(QStringLiteral("stop1")), discoveryPort))
        << game.process.readAllStandardError().toStdString();
    GameProcess second;
    ASSERT_TRUE(
        second.start(directory.path(), directory.filePath(QStringLiteral("stop2")), discoveryPort))
        << second.process.readAllStandardError().toStdString();
    RemoteConnection connection;
    connection.discover(discoveryPort);
    ASSERT_TRUE(waitForRemote([&] { return connection.discoveredTargets().size() == 2; }))
        << "Discovered " << connection.discoveredTargets().size() << " targets; games "
        << QJsonDocument(game.ready).toJson(QJsonDocument::Compact).toStdString() << " / "
        << QJsonDocument(second.ready).toJson(QJsonDocument::Compact).toStdString();
    const auto targets = connection.discoveredTargets();
    EXPECT_NE(targets[0].instanceId, targets[1].instanceId);
    const auto discovered = std::find_if(targets.cbegin(), targets.cend(), [&](const auto& target) {
        return target.instanceId == game.ready.value(QStringLiteral("instance")).toString();
    });
    ASSERT_NE(discovered, targets.cend());
    const auto port = static_cast<quint16>(game.ready.value(QStringLiteral("port")).toInt());
    EXPECT_EQ(discovered->port, port);
    EXPECT_EQ(discovered->processId, game.ready.value(QStringLiteral("pid")).toInt());
    connection.connectTarget(discovered->address, discovered->port);
    ASSERT_TRUE(waitForRemote([&] { return connection.catalog().size() >= 2; }))
        << connection.statusText().toStdString();
    EXPECT_EQ(connection.target().processId, game.ready.value(QStringLiteral("pid")).toInt());
    EXPECT_EQ(connection.target().engineVersion,
              game.ready.value(QStringLiteral("version")).toString());
    EXPECT_FALSE(connection.captureEnabled());
    quint64 recordCount = 0;
    QObject::connect(&connection, &RemoteConnection::monitorRecordsReceived, &connection,
                     [&](const QByteArray& bytes, quint32 count) {
                         EXPECT_FALSE(bytes.isEmpty());
                         recordCount += count;
                     });
    int commandCount = 0;
    QString lastCommandResult;
    QObject::connect(&connection, &RemoteConnection::commandFinished, &connection,
                     [&](const QString& command, bool success, const QString& result) {
                         lastCommandResult = command + QStringLiteral(": ") + result;
                         EXPECT_TRUE(success) << result.toStdString();
                         // Catalog and capture also emit commandFinished. Count Play/Stop/Set only,
                         // without requiring the English word "remote" (translated CI locales).
                         const bool ignored =
                             command.contains(QLatin1String("objects"), Qt::CaseInsensitive) ||
                             command.contains(QStringLiteral("对象")) ||
                             command.contains(QLatin1String("capture"), Qt::CaseInsensitive) ||
                             command.contains(QStringLiteral("捕获"));
                         if (!ignored)
                             ++commandCount;
                     });
    connection.setCaptureEnabled(true);
    ASSERT_TRUE(waitForRemote([&] { return connection.snapshot().valid; }))
        << connection.statusText().toStdString();
    const auto initialBlock = connection.snapshot().blockIndex;
    connection.postAction(harkentest::ToStudio(harkentest::remote::kPlayAction));
    ASSERT_TRUE(
        waitForRemote([&] { return commandCount == 1 && connection.snapshot().activeVoices > 0; }))
        << "Commands " << commandCount << ", voices " << connection.snapshot().activeVoices
        << ", last result " << lastCommandResult.toStdString()
        << ", initial block " << initialBlock << ", block " << connection.snapshot().blockIndex
        << ", valid snapshot " << connection.snapshot().valid
        << ", status " << connection.statusText().toStdString()
        << ", game state " << game.process.state()
        << ", game stderr " << game.process.readAllStandardError().toStdString();
    EXPECT_GT(connection.lastPlaybackHandle(), 0u);
    ASSERT_TRUE(waitForRemote([&] { return connection.snapshot().peak > 0; }));
    ASSERT_TRUE(waitForRemote(
        [&] { return recordCount > 0 && connection.snapshot().blockIndex > initialBlock; }));
    connection.setParameter(harkentest::ToStudio(harkentest::remote::kVariable), 0.6f);
    ASSERT_TRUE(waitForRemote([&] { return commandCount == 2; }));
    connection.stopPlayback(connection.lastPlaybackHandle());
    ASSERT_TRUE(waitForRemote(
        [&] { return commandCount == 3 && connection.snapshot().activeVoices == 0; }));
    connection.postAction(harkentest::ToStudio(harkentest::remote::kPlayAction));
    ASSERT_TRUE(waitForRemote(
        [&] { return commandCount == 4 && connection.snapshot().activeVoices == 1; }))
        << "Commands " << commandCount << ", pending " << connection.commandPending()
        << ", valid snapshot " << connection.snapshot().valid
        << ", voices " << connection.snapshot().activeVoices
        << ", block " << connection.snapshot().blockIndex
        << ", status " << connection.statusText().toStdString()
        << ", last result " << lastCommandResult.toStdString()
        << ", game state " << game.process.state()
        << ", game stderr " << game.process.readAllStandardError().toStdString();
    connection.disconnectTarget();
    EXPECT_FALSE(connection.snapshot().valid);
    connection.connectTarget(QStringLiteral("127.0.0.1"), port);
    ASSERT_TRUE(waitForRemote([&] { return connection.catalog().size() >= 2; }));
    EXPECT_EQ(commandCount, 4);
    EXPECT_FALSE(connection.captureEnabled());
    connection.setCaptureEnabled(true);
    ASSERT_TRUE(waitForRemote([&] { return connection.snapshot().valid; }));
    EXPECT_EQ(connection.snapshot().activeVoices, 1u);
    connection.stopAll();
    ASSERT_TRUE(waitForRemote(
        [&] { return commandCount == 5 && connection.snapshot().activeVoices == 0; }))
        << "Commands " << commandCount << ", pending " << connection.commandPending()
        << ", valid snapshot " << connection.snapshot().valid
        << ", voices " << connection.snapshot().activeVoices
        << ", block " << connection.snapshot().blockIndex
        << ", status " << connection.statusText().toStdString()
        << ", last result " << lastCommandResult.toStdString();
    EXPECT_TRUE(game.stop()) << game.process.readAllStandardError().toStdString();
    ASSERT_TRUE(waitForRemote([&] { return connection.state() == RemoteConnectionState::Failed; }));
    EXPECT_FALSE(connection.snapshot().valid);
    EXPECT_TRUE(second.stop()) << second.process.readAllStandardError().toStdString();
}

TEST(MonitorCatalog, RemoteGameCallsFromSharedDllAppearInPanel) {
    using namespace VNX::Monitor;
    ASSERT_NE(qApp, nullptr);
    QTemporaryDir directory(QStringLiteral(HARKEN_TEMP_DIR "/event-game-XXXXXX"));
    ASSERT_TRUE(directory.isValid());
    harkentest::remote::GameFingerprints fingerprints;
    ASSERT_TRUE(harkentest::remote::WriteGamePacks(
        std::filesystem::path(directory.path().toStdU16String()), &fingerprints));
    QUdpSocket reservation;
    ASSERT_TRUE(reservation.bind(QHostAddress::AnyIPv4, 0));
    const auto discoveryPort = reservation.localPort();
    reservation.close();
    GameProcess game;
    ASSERT_TRUE(game.start(directory.path(), directory.filePath(QStringLiteral("stop")), discoveryPort));
    RemoteConnection connection;
    EventCaptureController capture(&connection);
    const auto projectGuid = QUuid::fromRfc4122(QByteArrayView(
        reinterpret_cast<const char*>(harkentest::remote::kProjectGuid.data()), 16));
    VNX::ProjectData project;
    project.projectId = projectGuid.toString(QUuid::WithoutBraces);
    const auto addObject = [&](harkenstudio::ObjectId id, quint32 objectId,
                               VNX::ProjectItem::Type type, const QString& name) {
        auto item = std::make_shared<VNX::ProjectItem>(name, type);
        item->objectID = objectId;
        item->guid = VNX::authoringUuid(id);
        if (!project.root) project.root = item;
        else project.root->children.push_back(item);
    };
    addObject(harkentest::ToStudio(harkentest::remote::kSound), 31, VNX::ProjectItem::Type::Sound, QStringLiteral("Remote Tone"));
    addObject(harkentest::ToStudio(harkentest::remote::kPlayAction), 41, VNX::ProjectItem::Type::Action, QStringLiteral("Play Remote Probe"));
    addObject(harkentest::ToStudio(harkentest::remote::kVariable), 7, VNX::ProjectItem::Type::Variable, QStringLiteral("Intensity"));
    project.root->attachDefaultComponents();
    VNX::EffectEntry effect;
    effect.guid = VNX::authoringUuid(harkentest::ToStudio(harkentest::remote::kEffect));
    effect.type = VNX::EffectType::Gain;
    effect.parameters = {0.0f};
    ASSERT_TRUE(project.root->comp<VNX::VMixNode>()->effects.replace({effect}));
    capture.setProjectObjects(VNX::ProjectObjectIndex::fromProject(&project));
    VNX::MonitorCatalogPanel panel;
    panel.setCaptureController(&capture);
    connection.connectTarget(QStringLiteral("127.0.0.1"),
        static_cast<quint16>(game.ready.value(QStringLiteral("port")).toInt()));
    ASSERT_TRUE(waitForRemote([&] { return connection.canCaptureEvents(); }));
    capture.start();
    ASSERT_TRUE(waitForRemote([&] { return capture.state() == EventCaptureState::Capturing; }));
    QSaveFile probe(directory.filePath(QStringLiteral("event-probe.json")));
    ASSERT_TRUE(probe.open(QIODevice::WriteOnly));
    const auto probeRequest = QJsonDocument(QJsonObject{
        {QStringLiteral("variable"), VNX::objectIdString(harkentest::ToStudio(harkentest::remote::kVariable))},
        {QStringLiteral("action"), VNX::objectIdString(harkentest::ToStudio(harkentest::remote::kPlayAction))}
    }).toJson(QJsonDocument::Compact);
    ASSERT_EQ(probe.write(probeRequest), probeRequest.size());
    ASSERT_TRUE(probe.commit());
    bool sawVariable = false, sawAction = false, sawRefusal = false, sawLifecycle = false, sawEffect = false;
    ASSERT_TRUE(waitForRemote([&] {
        for (int row = 0; row < capture.rowCount(); ++row) {
            const auto& event = capture.eventAt(row);
            if (event.detail.category == VN_MONITOR_EVENT_CATEGORY_OPERATION) {
                sawVariable |= event.detail.code == VN_MONITOR_OPERATION_SET_VALUE &&
                    event.header.stableId == harkentest::remote::kVariable && event.detail.result == VN_OK;
                sawAction |= event.detail.code == VN_MONITOR_OPERATION_POST_ACTION &&
                    event.header.stableId == harkentest::remote::kPlayAction && event.detail.result == VN_OK;
                sawRefusal |= event.detail.code == VN_MONITOR_OPERATION_POST_ACTION &&
                    event.header.stableId == harkentest::StudioObjectId(999000111) && event.detail.result != VN_OK;
            } else if (event.detail.category == VN_MONITOR_EVENT_CATEGORY_LIFECYCLE) {
                sawLifecycle |= event.detail.code == VN_MONITOR_LIFECYCLE_PLAYABLE_START &&
                    event.header.stableId == harkentest::remote::kSound;
                sawEffect |= event.detail.code == VN_MONITOR_LIFECYCLE_EFFECT_INSTALLED &&
                    event.header.stableId == harkentest::remote::kEffect &&
                    event.objectName == QStringLiteral("Remote Tone / gain");
            }
        }
        return sawVariable && sawAction && sawRefusal && sawLifecycle && sawEffect;
    })) << capture.statusText().toStdString()
        << ", process=" << game.process.exitCode()
        << ", stderr=" << game.process.readAllStandardError().toStdString()
        << ", observed=" << sawVariable << sawAction << sawRefusal << sawLifecycle << sawEffect;
    capture.stop();
    ASSERT_TRUE(waitForRemote([&] { return capture.state() == EventCaptureState::Stopped; }));
    EXPECT_FALSE(capture.incomplete());
    const auto retained = capture.rowCount();
    EXPECT_GT(retained, 3);
    bool namedVariable = false, namedAction = false, namedSound = false, namedLifecycleAction = false;
    for (int row = 0; row < retained; ++row) {
        const auto& event = capture.eventAt(row);
        SCOPED_TRACE(::testing::Message() << "row " << row << " code " << event.detail.code
            << " world " << event.header.worldGeneration
            << " project " << event.projectGuid.toString().toStdString());
        if (event.header.stableId == harkentest::remote::kVariable) {
            EXPECT_EQ(event.objectName, QStringLiteral("Intensity"));
            namedVariable = true;
        }
        if (event.header.stableId == harkentest::remote::kPlayAction) {
            EXPECT_EQ(event.objectName, QStringLiteral("Play Remote Probe"));
            namedAction = true;
            namedLifecycleAction |= event.detail.category == VN_MONITOR_EVENT_CATEGORY_LIFECYCLE;
        }
        if (event.header.stableId == harkentest::remote::kSound) {
            EXPECT_EQ(event.objectName, QStringLiteral("Remote Tone"));
            namedSound = true;
        }
    }
    EXPECT_TRUE(namedVariable && namedAction && namedSound && namedLifecycleAction);
    ASSERT_EQ(panel.visibleRowCount(), retained);
    auto* level = panel.findChild<QComboBox*>(QStringLiteral("MonitorCatalogLevelFilter"));
    auto* search = panel.findChild<QLineEdit*>(QStringLiteral("MonitorCatalogTextFilter"));
    ASSERT_NE(level, nullptr);
    ASSERT_NE(search, nullptr);
    level->setCurrentIndex(level->findData(VN_LOG_ERROR));
    search->setText(VNX::objectIdString(harkentest::StudioObjectId(999000111)));
    ASSERT_EQ(panel.visibleRowCount(), 1);
    EXPECT_EQ(panel.table()->model()->index(0, 1).data().toString(), QStringLiteral("Error"));
    EXPECT_TRUE(panel.table()->model()->index(0, 4).data().toString().contains(QStringLiteral("Unknown object")));
    panel.table()->selectRow(0);
    panel.copySelected();
    EXPECT_TRUE(QApplication::clipboard()->text().contains(VNX::objectIdString(harkentest::StudioObjectId(999000111))));
    search->setText(QStringLiteral("no-such-event"));
    EXPECT_EQ(panel.visibleRowCount(), 0);
    EXPECT_TRUE(panel.gapMessage().isEmpty());
    search->clear();
    level->setCurrentIndex(0);
    EXPECT_EQ(panel.visibleRowCount(), retained);
    EXPECT_EQ(RemoteConnectionState::Connected, connection.state());
    capture.start();
    ASSERT_TRUE(waitForRemote([&] { return capture.state() == EventCaptureState::Capturing; }))
        << capture.statusText().toStdString()
        << ", connection=" << connection.statusText().toStdString()
        << ", result=" << connection.eventCaptureStatus().result;
    EXPECT_EQ(0, capture.rowCount());
    EXPECT_EQ(panel.visibleRowCount(), 0);
    EXPECT_TRUE(panel.gapMessage().isEmpty());
    ASSERT_TRUE(game.stop());
    ASSERT_TRUE(waitForRemote([&] { return capture.state() == EventCaptureState::Failed; }));
    EXPECT_TRUE(capture.incomplete());
}

}
