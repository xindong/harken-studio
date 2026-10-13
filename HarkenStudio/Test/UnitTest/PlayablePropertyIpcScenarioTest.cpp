#include "ActionManager.h"
#include "Components/VSound.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "Support/AITestSupport.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTcpSocket>

using namespace VNX;

namespace {

class PlayablePropertyIpcScenarioTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        UndoRedoManager::instance().clear();
        ASSERT_TRUE(temp_.isValid());
        server_.setProjectManager(&manager);
        server_.setAppState(IPCServer::AppState::Ready);
        ASSERT_TRUE(server_.start(0));
        socket_.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(server_.port()));
        ASSERT_TRUE(TestSupport::spinUntil([this] {
            return socket_.state() == QAbstractSocket::ConnectedState;
        }));
        ASSERT_EQ(call(QStringLiteral("create_project"),
                       {{QStringLiteral("name"), QStringLiteral("PlayablePropertyIpcProject")},
                        {QStringLiteral("location"), temp_.path()}})
                      .value(QStringLiteral("status")).toString(),
                  QStringLiteral("ok"));
    }

    void TearDown() override
    {
        socket_.disconnectFromHost();
        TestSupport::spinUntil([this] {
            return socket_.state() == QAbstractSocket::UnconnectedState;
        });
        server_.stop();
        UndoRedoManager::instance().clear();
        if (ProjectManager::instance().isProjectOpen())
            ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QJsonObject call(const QString& command, const QJsonObject& args = {})
    {
        const QString id = QStringLiteral("phase5-%1").arg(++request_);
        const QByteArray payload = QJsonDocument(QJsonObject{
            {QStringLiteral("cmd"), command},
            {QStringLiteral("args"), args},
            {QStringLiteral("request_id"), id}}).toJson(QJsonDocument::Compact)
            + QByteArrayLiteral("\n");
        EXPECT_EQ(socket_.write(payload), payload.size());
        socket_.flush();
        if (!TestSupport::spinUntil([this] { return socket_.canReadLine(); })) return {};
        const QJsonObject response = QJsonDocument::fromJson(socket_.readLine().trimmed()).object();
        EXPECT_EQ(response.value(QStringLiteral("request_id")).toString(), id);
        return response;
    }

    QJsonObject create(const QString& name, const QString& type, const QString& parent = {})
    {
        const QJsonObject response = call(QStringLiteral("create_item"),
            {{QStringLiteral("parent"), parent}, {QStringLiteral("name"), name},
             {QStringLiteral("type"), type}});
        EXPECT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        return response.value(QStringLiteral("result")).toObject();
    }

    QTemporaryDir temp_;
    IPCServer server_{nullptr};
    QTcpSocket socket_;
    int request_ = 0;
};

}

TEST_F(PlayablePropertyIpcScenarioTest, FourCanonicalPropertiesRoundTripAndRejectAtomicallyOverTcp)
{
    const QJsonObject container = create(QStringLiteral("Container"), QStringLiteral("Container"));
    const QString containerPath = container.value(QStringLiteral("path")).toString();
    const QJsonObject child = create(QStringLiteral("Child"), QStringLiteral("Sound"), containerPath);
    const QString childPath = child.value(QStringLiteral("path")).toString();

    const QJsonObject behavior{
        {QStringLiteral("discipline"), QStringLiteral("shuffle")},
        {QStringLiteral("avoidRepeat"), 0}
    };
    ASSERT_EQ(call(QStringLiteral("set_item_property"),
                   {{QStringLiteral("path"), containerPath},
                    {QStringLiteral("property"), QStringLiteral("behavior")},
                    {QStringLiteral("value"), behavior}})
                  .value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const auto containerInfo = call(QStringLiteral("get_item_info"), {{QStringLiteral("path"), containerPath}});
    auto selections = containerInfo.value("result").toObject().value("components").toObject()
        .value("selection").toObject().value("selections").toObject();
    auto shared = selections.value("shared").toObject();
    auto entries = shared.value("entries").toArray();
    ASSERT_EQ(entries.size(), 1);
    auto entry = entries[0].toObject();
    auto rows = entry.value("occurrences").toArray();
    ASSERT_EQ(rows.size(), 1);
    auto row = rows[0].toObject();
    row["weight"] = 70;
    rows[0] = row;
    entry["occurrences"] = rows;
    entries[0] = entry;
    shared["entries"] = entries;
    selections["shared"] = shared;
    ASSERT_EQ(call(QStringLiteral("set_item_property"),
                   {{QStringLiteral("path"), containerPath},
                    {QStringLiteral("property"), QStringLiteral("selections")},
                    {QStringLiteral("value"), selections}})
                  .value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    ASSERT_EQ(call(QStringLiteral("set_item_property"),
                   {{QStringLiteral("path"), childPath},
                    {QStringLiteral("property"), QStringLiteral("loopCount")},
                    {QStringLiteral("value"), 4}})
                  .value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    ASSERT_EQ(call(QStringLiteral("set_item_property"),
                   {{QStringLiteral("path"), childPath},
                    {QStringLiteral("property"), QStringLiteral("scheduleOffsetSec")},
                    {QStringLiteral("value"), 0.25}})
                  .value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    const QJsonObject emptySources = VSound{}.toJson().value(QStringLiteral("sources")).toObject();
    ASSERT_EQ(call(QStringLiteral("set_item_property"),
                   {{QStringLiteral("path"), childPath},
                    {QStringLiteral("property"), QStringLiteral("sources")},
                    {QStringLiteral("value"), emptySources}})
                  .value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

    const QJsonObject before = call(QStringLiteral("get_item_info"),
                                    {{QStringLiteral("path"), childPath}});
    const QJsonObject invalid = call(QStringLiteral("set_item_property"),
        {{QStringLiteral("path"), childPath},
         {QStringLiteral("property"), QStringLiteral("scheduleOffsetSec")},
         {QStringLiteral("value"), -1.0}});
    EXPECT_EQ(invalid.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    const QJsonObject after = call(QStringLiteral("get_item_info"),
                                   {{QStringLiteral("path"), childPath}});
    EXPECT_EQ(after.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("components")).toObject(),
              before.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("components")).toObject());

    ASSERT_EQ(call(QStringLiteral("undo")).value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    ASSERT_EQ(call(QStringLiteral("redo")).value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
}
