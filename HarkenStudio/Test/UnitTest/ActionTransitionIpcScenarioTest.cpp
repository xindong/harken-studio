#include "ActionManager.h"
#include "AuthoringPolicy.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "Support/AITestSupport.h"
#include "TransitionSemantics.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QAbstractSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTcpSocket>

using namespace VNX;

namespace {

struct CreatedItem
{
    QString path;
    QUuid guid;
    qint64 authoringHandle = 0;
};

TransitionRuleDef ruleFor(std::optional<QUuid> source, std::optional<QUuid> target)
{
    TransitionRuleDef rule = AuthoringPolicy::defaultTransitionRule();
    rule.source = source;
    rule.target = target;
    return rule;
}

QJsonObject quantizeJson(qint64 referenceObjectID, const QString& referenceName,
                        const QString& timing, const QString& ifSilent)
{
    return QJsonObject{
        {QStringLiteral("referenceObjectID"), referenceObjectID},
        {QStringLiteral("referenceName"), referenceName},
        {QStringLiteral("timing"), timing},
        {QStringLiteral("ifSilent"), ifSilent}
    };
}

QJsonObject stepJson(const QString& type, qint64 targetAuthoringHandle,
                    const QString& targetName)
{
    return QJsonObject{
        {QStringLiteral("type"), type},
        {QStringLiteral("targetObjectID"), targetAuthoringHandle},
        {QStringLiteral("targetName"), targetName},
        {QStringLiteral("delay"), 0.0}
    };
}

class ActionTransitionIpcScenarioTest : public ::testing::Test
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

        const QJsonObject createdProject = call(
            QStringLiteral("create_project"),
            {{QStringLiteral("name"), QStringLiteral("ActionTransitionIpcProject")},
             {QStringLiteral("location"), tempDir_.path()}},
            5000);
        ASSERT_EQ(createdProject.value(QStringLiteral("status")).toString(),
                  QStringLiteral("ok"));
        projectPath_ = createdProject.value(QStringLiteral("result")).toObject()
                          .value(QStringLiteral("path")).toString();
        ASSERT_FALSE(projectPath_.isEmpty());
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

    QJsonObject call(const QString& command, const QJsonObject& args = {},
                     int timeoutMs = 1000)
    {
        const QString requestId = QStringLiteral("phase4-%1").arg(++requestId_);
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

        if (!TestSupport::spinUntil([this] { return socket_.canReadLine(); }, timeoutMs)) {
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

    CreatedItem createItem(const QString& parentPath, const QString& name,
                          const QString& type)
    {
        const QJsonObject created = call(
            QStringLiteral("create_item"),
            {{QStringLiteral("parent"), parentPath},
             {QStringLiteral("name"), name},
             {QStringLiteral("type"), type}});
        EXPECT_EQ(created.value(QStringLiteral("status")).toString(),
                  QStringLiteral("ok"));
        const QJsonObject result = created.value(QStringLiteral("result")).toObject();
        return CreatedItem{
            result.value(QStringLiteral("path")).toString(),
            QUuid(result.value(QStringLiteral("guid")).toString()),
            result.value(QStringLiteral("authoring_handle")).toInteger()
        };
    }

    static QJsonObject componentsOf(const QJsonObject& info)
    {
        return info.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("components")).toObject();
    }

    QTemporaryDir tempDir_;
    IPCServer server_{nullptr};
    QTcpSocket socket_;
    int requestId_ = 0;
    QString projectPath_;
};

}

TEST_F(ActionTransitionIpcScenarioTest, TransitionsSetAndGetRoundTripOverRealSocket)
{
    const CreatedItem container = createItem(QString(), QStringLiteral("IpcContainer"),
                                             QStringLiteral("Container"));
    const CreatedItem childA = createItem(container.path, QStringLiteral("IpcChildA"),
                                          QStringLiteral("Sound"));
    const CreatedItem childB = createItem(container.path, QStringLiteral("IpcChildB"),
                                          QStringLiteral("Sound"));
    ASSERT_FALSE(container.path.isEmpty());
    ASSERT_FALSE(childA.guid.isNull());
    ASSERT_FALSE(childB.guid.isNull());

    TransitionTableDef table = AuthoringPolicy::defaultTransitionTable();
    ASSERT_TRUE(table.addRule(ruleFor(childA.guid, childB.guid)));
    const QJsonObject tableJson = table.toJson();

    const QJsonObject written = call(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("path"), container.path},
         {QStringLiteral("property"), QStringLiteral("transitions")},
         {QStringLiteral("value"), tableJson}});
    ASSERT_EQ(written.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(written.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("value")).toObject(),
              tableJson);

    const QJsonObject info = call(
        QStringLiteral("get_item_info"),
        {{QStringLiteral("path"), container.path}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(componentsOf(info).value(QStringLiteral("selection")).toObject()
                  .value(QStringLiteral("transitions")).toObject(),
              tableJson);
}

TEST_F(ActionTransitionIpcScenarioTest, InvalidTransitionsTableIsRejectedAtomicallyOverRealSocket)
{
    const CreatedItem container = createItem(QString(), QStringLiteral("IpcInvalidContainer"),
                                             QStringLiteral("Container"));
    const CreatedItem childA = createItem(container.path, QStringLiteral("IpcInvalidChildA"),
                                          QStringLiteral("Sound"));
    const CreatedItem childB = createItem(container.path, QStringLiteral("IpcInvalidChildB"),
                                          QStringLiteral("Sound"));

    TransitionTableDef table = AuthoringPolicy::defaultTransitionTable();
    ASSERT_TRUE(table.addRule(ruleFor(childA.guid, childB.guid)));
    const QJsonObject validJson = table.toJson();
    ASSERT_EQ(call(QStringLiteral("set_item_property"),
                   {{QStringLiteral("path"), container.path},
                    {QStringLiteral("property"), QStringLiteral("transitions")},
                    {QStringLiteral("value"), validJson}})
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));

    const QJsonObject canUndoBefore = call(QStringLiteral("can_undo"));
    ASSERT_EQ(canUndoBefore.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));

    QJsonObject missingDefault = validJson;
    QJsonArray rules = missingDefault.value(QStringLiteral("rules")).toArray();
    rules.removeFirst();
    missingDefault[QStringLiteral("rules")] = rules;
    const QJsonObject rejected = call(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("path"), container.path},
         {QStringLiteral("property"), QStringLiteral("transitions")},
         {QStringLiteral("value"), missingDefault}});
    EXPECT_EQ(rejected.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(rejected.value(QStringLiteral("code")).toString(),
              QStringLiteral("INVALID_ARGS"));

    const QJsonObject info = call(
        QStringLiteral("get_item_info"),
        {{QStringLiteral("path"), container.path}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(componentsOf(info).value(QStringLiteral("selection")).toObject()
                  .value(QStringLiteral("transitions")).toObject(),
              validJson);

    const QJsonObject canUndoAfter = call(QStringLiteral("can_undo"));
    EXPECT_EQ(canUndoAfter.value(QStringLiteral("result")).toObject(),
              canUndoBefore.value(QStringLiteral("result")).toObject());
}

TEST_F(ActionTransitionIpcScenarioTest, ActionStepsSetAndGetRoundTripOverRealSocket)
{
    const CreatedItem action = createItem(QString(), QStringLiteral("IpcAction"),
                                          QStringLiteral("Action"));
    const CreatedItem playTarget = createItem(QString(), QStringLiteral("IpcPlayTarget"),
                                              QStringLiteral("Sound"));
    const CreatedItem clockRef = createItem(QString(), QStringLiteral("IpcClockRef"),
                                            QStringLiteral("Container"));
    const CreatedItem speedVar = createItem(QString(), QStringLiteral("IpcSpeedVar"),
                                            QStringLiteral("Variable"));

    QJsonObject play = stepJson(QStringLiteral("Play"), playTarget.authoringHandle,
                                QStringLiteral("StalePlayName"));
    play[QStringLiteral("quantize")] = quantizeJson(
        clockRef.authoringHandle, QStringLiteral("StaleClockName"),
        QStringLiteral("NextMarker"), QStringLiteral("Drop"));
    QJsonObject setValue = stepJson(QStringLiteral("SetVariable"), speedVar.authoringHandle,
                                    QStringLiteral("StaleVarName"));
    setValue[QStringLiteral("value")] = 50.0;

    const QJsonArray ledger{play, setValue};
    const QJsonObject written = call(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("path"), action.path},
         {QStringLiteral("property"), QStringLiteral("steps")},
         {QStringLiteral("value"), ledger}});
    ASSERT_EQ(written.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonArray canonical = written.value(QStringLiteral("result")).toObject()
                                    .value(QStringLiteral("value")).toArray();
    ASSERT_EQ(canonical.size(), 2);
    EXPECT_EQ(canonical[0].toObject().value(QStringLiteral("targetName")).toString(),
              QStringLiteral("IpcPlayTarget"));
    EXPECT_EQ(canonical[0].toObject().value(QStringLiteral("quantize")).toObject()
                  .value(QStringLiteral("referenceName")).toString(),
              QStringLiteral("IpcClockRef"));
    EXPECT_DOUBLE_EQ(canonical[1].toObject().value(QStringLiteral("value")).toDouble(),
                     50.0);

    const QJsonObject info = call(
        QStringLiteral("get_item_info"),
        {{QStringLiteral("path"), action.path}});
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(componentsOf(info).value(QStringLiteral("action")).toObject()
                  .value(QStringLiteral("steps")).toArray(),
              canonical);
}

TEST_F(ActionTransitionIpcScenarioTest, UndoAndRedoRevertAndReapplyTransitionsOverRealSocket)
{
    const CreatedItem container = createItem(QString(), QStringLiteral("IpcUndoContainer"),
                                             QStringLiteral("Container"));
    const CreatedItem childA = createItem(container.path, QStringLiteral("IpcUndoChildA"),
                                          QStringLiteral("Sound"));
    const CreatedItem childB = createItem(container.path, QStringLiteral("IpcUndoChildB"),
                                          QStringLiteral("Sound"));

    TransitionTableDef table = AuthoringPolicy::defaultTransitionTable();
    ASSERT_TRUE(table.addRule(ruleFor(childA.guid, childB.guid)));
    const QJsonObject tableJson = table.toJson();
    ASSERT_EQ(call(QStringLiteral("set_item_property"),
                   {{QStringLiteral("path"), container.path},
                    {QStringLiteral("property"), QStringLiteral("transitions")},
                    {QStringLiteral("value"), tableJson}})
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));

    const QJsonObject undone = call(QStringLiteral("undo"));
    ASSERT_EQ(undone.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    QJsonObject info = call(QStringLiteral("get_item_info"),
                            {{QStringLiteral("path"), container.path}});
    EXPECT_EQ(componentsOf(info).value(QStringLiteral("selection")).toObject()
                  .value(QStringLiteral("transitions")).toObject(),
              AuthoringPolicy::defaultTransitionTable().toJson());

    const QJsonObject redone = call(QStringLiteral("redo"));
    ASSERT_EQ(redone.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    info = call(QStringLiteral("get_item_info"),
               {{QStringLiteral("path"), container.path}});
    EXPECT_EQ(componentsOf(info).value(QStringLiteral("selection")).toObject()
                  .value(QStringLiteral("transitions")).toObject(),
              tableJson);
}

TEST_F(ActionTransitionIpcScenarioTest,
       SaveAndReloadPreservesStepsAndTransitionsWithoutAddingUndoCommandsOverRealSocket)
{
    const CreatedItem action = createItem(QString(), QStringLiteral("IpcPersistAction"),
                                          QStringLiteral("Action"));
    const CreatedItem playTarget = createItem(QString(), QStringLiteral("IpcPersistPlayTarget"),
                                              QStringLiteral("Sound"));
    const CreatedItem container = createItem(QString(), QStringLiteral("IpcPersistContainer"),
                                             QStringLiteral("Container"));
    const CreatedItem childA = createItem(container.path, QStringLiteral("IpcPersistChildA"),
                                          QStringLiteral("Sound"));
    const CreatedItem childB = createItem(container.path, QStringLiteral("IpcPersistChildB"),
                                          QStringLiteral("Sound"));

    const QJsonArray steps{stepJson(QStringLiteral("Play"), playTarget.authoringHandle,
                                    QStringLiteral("IpcPersistPlayTarget"))};
    ASSERT_EQ(call(QStringLiteral("set_item_property"),
                   {{QStringLiteral("path"), action.path},
                    {QStringLiteral("property"), QStringLiteral("steps")},
                    {QStringLiteral("value"), steps}})
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));

    TransitionTableDef table = AuthoringPolicy::defaultTransitionTable();
    ASSERT_TRUE(table.addRule(ruleFor(childA.guid, childB.guid)));
    const QJsonObject tableJson = table.toJson();
    ASSERT_EQ(call(QStringLiteral("set_item_property"),
                   {{QStringLiteral("path"), container.path},
                    {QStringLiteral("property"), QStringLiteral("transitions")},
                    {QStringLiteral("value"), tableJson}})
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));

    const QJsonObject stepsBefore = componentsOf(
        call(QStringLiteral("get_item_info"), {{QStringLiteral("path"), action.path}}))
            .value(QStringLiteral("action")).toObject();
    const QJsonObject transitionsBefore = componentsOf(
        call(QStringLiteral("get_item_info"), {{QStringLiteral("path"), container.path}}))
            .value(QStringLiteral("selection")).toObject();

    const QJsonObject saved = call(QStringLiteral("save_project"));
    ASSERT_EQ(saved.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_TRUE(saved.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("success")).toBool());

    const QJsonObject opened = call(
        QStringLiteral("open_project"),
        {{QStringLiteral("path"), projectPath_}});
    ASSERT_EQ(opened.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));

    const QJsonObject stepsAfter = componentsOf(
        call(QStringLiteral("get_item_info"), {{QStringLiteral("path"), action.path}}))
            .value(QStringLiteral("action")).toObject();
    const QJsonObject transitionsAfter = componentsOf(
        call(QStringLiteral("get_item_info"), {{QStringLiteral("path"), container.path}}))
            .value(QStringLiteral("selection")).toObject();
    const QJsonObject reloadedTargetResponse = call(
        QStringLiteral("get_item_info"),
        {{QStringLiteral("path"), playTarget.path}});
    ASSERT_EQ(reloadedTargetResponse.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    const QJsonObject reloadedTarget =
        reloadedTargetResponse.value(QStringLiteral("result")).toObject();
    ASSERT_GT(reloadedTarget.value(QStringLiteral("authoring_handle")).toInteger(), 0);
    QJsonObject expectedStepsAfter = stepsBefore;
    QJsonArray expectedRows = expectedStepsAfter.value(QStringLiteral("steps")).toArray();
    QJsonObject expectedRow = expectedRows[0].toObject();
    expectedRow[QStringLiteral("targetObjectID")] =
        reloadedTarget.value(QStringLiteral("authoring_handle"));
    expectedRows[0] = expectedRow;
    expectedStepsAfter[QStringLiteral("steps")] = expectedRows;
    EXPECT_EQ(stepsAfter, expectedStepsAfter);
    EXPECT_EQ(transitionsAfter, transitionsBefore);

    const QJsonObject canUndoReloaded = call(QStringLiteral("can_undo"));
    ASSERT_EQ(canUndoReloaded.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_FALSE(canUndoReloaded.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("can_undo")).toBool());
}
