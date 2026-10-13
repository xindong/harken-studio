#include "ActionManager.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "Timeline/TimelineTypes.h"
#include "UndoCommands.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QAbstractSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTcpSocket>

using namespace VNX;

namespace {

class TrackPieceIpcScenarioTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();

        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();

        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        UndoRedoManager::instance().clear();

        ASSERT_TRUE(tempDir_.isValid());
        server_.setProjectManager(&manager);
        server_.setAppState(IPCServer::AppState::Ready);
        ASSERT_TRUE(server_.start(0));
        ASSERT_GT(server_.port(), 0);

        socket_.connectToHost(QHostAddress::LocalHost,
                              static_cast<quint16>(server_.port()));
        ASSERT_TRUE(TestSupport::spinUntil([this] {
            return socket_.state() == QAbstractSocket::ConnectedState;
        }));
    }

    void TearDown() override
    {
        socket_.disconnectFromHost();
        TestSupport::spinUntil([this] {
            return socket_.state() == QAbstractSocket::UnconnectedState;
        });
        server_.stop();
        QCoreApplication::processEvents();

        UndoRedoManager::instance().clear();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QJsonObject call(const QString& command, const QJsonObject& args = {})
    {
        const QString requestId = QStringLiteral("phase3-%1").arg(++requestId_);
        const QJsonObject request{
            {QStringLiteral("cmd"), command},
            {QStringLiteral("args"), args},
            {QStringLiteral("request_id"), requestId},
        };
        const QByteArray payload =
            QJsonDocument(request).toJson(QJsonDocument::Compact)
            + QByteArrayLiteral("\n");
        if (socket_.write(payload) != payload.size()) {
            ADD_FAILURE() << "Failed to write complete IPC request";
            return {};
        }
        socket_.flush();

        if (!TestSupport::spinUntil([this] { return socket_.canReadLine(); })) {
            ADD_FAILURE() << "Timed out waiting for IPC response";
            return {};
        }

        QJsonParseError error;
        const QJsonDocument response = QJsonDocument::fromJson(
            socket_.readLine().trimmed(), &error);
        if (error.error != QJsonParseError::NoError || !response.isObject()) {
            ADD_FAILURE() << "IPC response was not a JSON object";
            return {};
        }

        const QJsonObject object = response.object();
        EXPECT_EQ(object.value(QStringLiteral("request_id")).toString(), requestId);
        return object;
    }

    static QJsonArray clipsFromInfo(const QJsonObject& response)
    {
        return response.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("components")).toObject()
            .value(QStringLiteral("tracktimeline")).toObject()
            .value(QStringLiteral("clips")).toArray();
    }

    QTemporaryDir tempDir_;
    IPCServer server_{nullptr};
    QTcpSocket socket_;
    int requestId_ = 0;
};

}

TEST_F(TrackPieceIpcScenarioTest,
       CreatesWritesRejectsAtomicallyReloadsAndUndoesOverTcp)
{
    const QJsonObject createdProject = call(
        QStringLiteral("create_project"),
        {{QStringLiteral("name"), QStringLiteral("TrackPieceIpcProject")},
         {QStringLiteral("location"), tempDir_.path()}});
    ASSERT_EQ(createdProject.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QString projectPath = createdProject.value(QStringLiteral("result"))
                                    .toObject()
                                    .value(QStringLiteral("path"))
                                    .toString();
    ASSERT_FALSE(projectPath.isEmpty());

    const QJsonObject createdTrack = call(
        QStringLiteral("create_item"),
        {{QStringLiteral("parent"), QString()},
         {QStringLiteral("name"), QStringLiteral("IpcTrack")},
         {QStringLiteral("type"), QStringLiteral("Track")}});
    ASSERT_EQ(createdTrack.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QString trackPath = createdTrack.value(QStringLiteral("result"))
                                  .toObject().value(QStringLiteral("path")).toString();
    ASSERT_FALSE(trackPath.isEmpty());

    const QJsonObject createdSound = call(
        QStringLiteral("create_item"),
        {{QStringLiteral("parent"), trackPath},
         {QStringLiteral("name"), QStringLiteral("IpcSound")},
         {QStringLiteral("type"), QStringLiteral("Sound")}});
    ASSERT_EQ(createdSound.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonObject soundResult =
        createdSound.value(QStringLiteral("result")).toObject();
    const QUuid soundGuid(soundResult.value(QStringLiteral("guid")).toString());
    ASSERT_FALSE(soundGuid.isNull());

    const QJsonObject createdPiece = call(
        QStringLiteral("create_item"),
        {{QStringLiteral("parent"), QString()},
         {QStringLiteral("name"), QStringLiteral("IpcPiece")},
         {QStringLiteral("type"), QStringLiteral("Piece")}});
    ASSERT_EQ(createdPiece.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QString piecePath = createdPiece.value(QStringLiteral("result"))
                                  .toObject().value(QStringLiteral("path")).toString();
    ASSERT_FALSE(piecePath.isEmpty());

    const QJsonObject createdQueue = call(
        QStringLiteral("create_item"),
        {{QStringLiteral("parent"), QString()},
         {QStringLiteral("name"), QStringLiteral("IpcQueue")},
         {QStringLiteral("type"), QStringLiteral("Container")}});
    ASSERT_EQ(createdQueue.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QString queuePath = createdQueue.value(QStringLiteral("result"))
                                  .toObject()
                                  .value(QStringLiteral("path"))
                                  .toString();
    ASSERT_FALSE(queuePath.isEmpty());

    VPiece pieceTimeline;
    QJsonObject written = call(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("path"), piecePath},
         {QStringLiteral("property"), QStringLiteral("pieceTime")},
         {QStringLiteral("value"), PieceTimeSpec::musical(600000, 7, 8).toJson()}});
    ASSERT_EQ(written.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(written.value(QStringLiteral("code")).toString(),
              QStringLiteral("AUTHORING_RESTRICTED"));
    QJsonObject info = call(
        QStringLiteral("get_item_info"),
        {{QStringLiteral("path"), piecePath}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    ASSERT_TRUE(pieceTimeline.fromJson(
        info.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("components")).toObject()
            .value(QStringLiteral("piecetimeline")).toObject()));
    EXPECT_EQ(pieceTimeline.pieceTime, PieceTimeSpec{});

    written = call(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("path"), queuePath},
         {QStringLiteral("property"), QStringLiteral("mode")},
         {QStringLiteral("value"),
          static_cast<int>(SelectionMode::Sequence)}});
    ASSERT_EQ(written.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    written = call(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("path"), queuePath},
         {QStringLiteral("property"), QStringLiteral("playType")},
         {QStringLiteral("value"), static_cast<int>(SelectionPlayType::Queue)}});
    ASSERT_EQ(written.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    written = call(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("path"), queuePath},
         {QStringLiteral("property"), QStringLiteral("spliceByMarker")},
         {QStringLiteral("value"), true}});
    ASSERT_EQ(written.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    info = call(QStringLiteral("get_item_info"),
                {{QStringLiteral("path"), queuePath}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonObject selection = info.value(QStringLiteral("result")).toObject()
                                      .value(QStringLiteral("components")).toObject()
                                      .value(QStringLiteral("selection")).toObject();
    EXPECT_EQ(selection.value(QStringLiteral("mode")).toInt(),
              static_cast<int>(SelectionMode::Sequence));
    EXPECT_EQ(selection.value(QStringLiteral("playType")).toInt(),
              static_cast<int>(SelectionPlayType::Queue));
    EXPECT_TRUE(selection.value(QStringLiteral("spliceByMarker")).toBool());

    info = call(QStringLiteral("get_item_info"),
                {{QStringLiteral("path"), trackPath}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonArray defaultClips = clipsFromInfo(info);
    ASSERT_EQ(defaultClips.size(), 1);

    Clip clip{
        QUuid(QStringLiteral("11111111-1111-4111-8111-111111111111")),
        soundGuid,
        -125,
        2000,
    };
    ClipLedger clipLedger;
    ASSERT_TRUE(clipLedger.mintOccurrenceOrdinal(&clip));
    clipLedger.items.push_back(clip);
    const QJsonObject clipLedgerJson = clipLedger.toJsonObject();
    written = call(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("path"), trackPath},
         {QStringLiteral("property"), QStringLiteral("clips")},
         {QStringLiteral("value"), clipLedgerJson}});
    ASSERT_EQ(written.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonObject canonicalClipLedger =
        written.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("value")).toObject();
    const QJsonArray clips = canonicalClipLedger.value(QStringLiteral("rows")).toArray();
    ASSERT_EQ(clips.size(), 1);
    const QUuid clipGuid(clips.at(0).toObject()
                            .value(QStringLiteral("clipGuid")).toString());
    EXPECT_EQ(clipGuid, clip.clipGuid);

    info = call(
        QStringLiteral("get_item_info"),
        {{QStringLiteral("path"), trackPath}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(clipsFromInfo(info), clips);

    const QJsonObject canUndoBefore = call(QStringLiteral("can_undo"));
    ASSERT_EQ(canUndoBefore.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_TRUE(canUndoBefore.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("can_undo")).toBool());

    QJsonObject changedClip = clip.toJson();
    changedClip[QStringLiteral("start")] = 9000;
    QJsonObject invalidClip = clip.toJson();
    invalidClip[QStringLiteral("clipGuid")] =
        QStringLiteral("22222222-2222-4222-8222-222222222222");
    invalidClip[QStringLiteral("stableOccurrenceOrdinal")] = 1;
    invalidClip[QStringLiteral("duration")] = 0;
    const QJsonObject invalidLedger{
        {QStringLiteral("occurrenceOrdinalHighWater"), 2},
        {QStringLiteral("rows"), QJsonArray{changedClip, invalidClip}}};
    const QJsonObject rejected = call(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("path"), trackPath},
         {QStringLiteral("property"), QStringLiteral("clips")},
         {QStringLiteral("value"), invalidLedger}});
    EXPECT_EQ(rejected.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(rejected.value(QStringLiteral("code")).toString(),
              QStringLiteral("INVALID_ARGS"));

    info = call(QStringLiteral("get_item_info"),
                {{QStringLiteral("path"), trackPath}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(clipsFromInfo(info), clips);
    const QJsonObject canUndoAfter = call(QStringLiteral("can_undo"));
    EXPECT_EQ(canUndoAfter.value(QStringLiteral("result")).toObject(),
              canUndoBefore.value(QStringLiteral("result")).toObject());

    const QJsonObject undone = call(QStringLiteral("undo"));
    ASSERT_EQ(undone.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    info = call(QStringLiteral("get_item_info"),
                {{QStringLiteral("path"), trackPath}});
    EXPECT_EQ(clipsFromInfo(info), defaultClips);

    const QJsonObject redone = call(QStringLiteral("redo"));
    ASSERT_EQ(redone.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    info = call(QStringLiteral("get_item_info"),
                {{QStringLiteral("path"), trackPath}});
    EXPECT_EQ(clipsFromInfo(info), clips);

    const QJsonObject saved = call(QStringLiteral("save_project"));
    ASSERT_EQ(saved.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_TRUE(saved.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("success")).toBool());

    const QJsonObject opened = call(
        QStringLiteral("open_project"),
        {{QStringLiteral("path"), projectPath}});
    ASSERT_EQ(opened.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    info = call(QStringLiteral("get_item_info"),
                {{QStringLiteral("path"), trackPath}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonArray reopenedClips = clipsFromInfo(info);
    ASSERT_EQ(reopenedClips.size(), 1);
    EXPECT_EQ(reopenedClips, clips);
    EXPECT_EQ(QUuid(reopenedClips.at(0).toObject()
                       .value(QStringLiteral("clipGuid")).toString()),
              clipGuid);

    info = call(QStringLiteral("get_item_info"),
                {{QStringLiteral("path"), piecePath}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    ASSERT_TRUE(pieceTimeline.fromJson(
        info.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("components")).toObject()
            .value(QStringLiteral("piecetimeline")).toObject()));
    EXPECT_EQ(pieceTimeline.pieceTime, PieceTimeSpec{});

    info = call(QStringLiteral("get_item_info"),
                {{QStringLiteral("path"), queuePath}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonObject reloadedSelection =
        info.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("components")).toObject()
            .value(QStringLiteral("selection")).toObject();
    EXPECT_EQ(reloadedSelection.value(QStringLiteral("mode")).toInt(),
              static_cast<int>(SelectionMode::Sequence));
    EXPECT_EQ(reloadedSelection.value(QStringLiteral("playType")).toInt(),
              static_cast<int>(SelectionPlayType::Queue));
    EXPECT_TRUE(reloadedSelection.value(QStringLiteral("spliceByMarker")).toBool());

    const QJsonObject canUndoReloaded = call(QStringLiteral("can_undo"));
    ASSERT_EQ(canUndoReloaded.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_FALSE(canUndoReloaded.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("can_undo")).toBool());
}
