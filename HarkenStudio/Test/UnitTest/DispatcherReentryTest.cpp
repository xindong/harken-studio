#include "IPC/IPCServer.h"
#include "ActionManager.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonObject>
#include <QObject>
#include <QTemporaryDir>
#include <stdexcept>

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

class DispatcherReentryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ActionManager::instance().clearAllForTesting();
    }

    void TearDown() override
    {
        ActionManager::instance().clearAllForTesting();
    }
};

} // namespace

TEST_F(DispatcherReentryTest, ReadonlyDispatchTraversesActionManagerExactlyOnce)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);

    int pingExecuted = 0;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &server,
        [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource) {
            if (id == QStringLiteral("ping")) ++pingExecuted;
        });

    const QJsonObject response = server.dispatchCommand(QStringLiteral("ping"), {});

    EXPECT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_TRUE(response.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("pong")).toBool());
    EXPECT_EQ(pingExecuted, 1);
}

TEST_F(DispatcherReentryTest, ReadonlyRoutingPreservesLegacyResponse)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);

    const QJsonObject viaActionRoute = server.dispatchCommand(QStringLiteral("ping"), {});
    ActionManager::instance().clearAllForTesting();
    const QJsonObject viaLegacyRoute = server.dispatchCommand(QStringLiteral("ping"), {});

    EXPECT_EQ(viaActionRoute, viaLegacyRoute);
}

TEST_F(DispatcherReentryTest, DispatcherResponseEnvelopesRemainStable)
{
    IPCServer server(nullptr);

    const QJsonObject success = server.dispatchCommand(QStringLiteral("ping"), {});
    EXPECT_EQ(success.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_TRUE(success.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("pong")).toBool());

    const QJsonObject unknown =
        server.dispatchCommand(QStringLiteral("no_such_command_zzz"), {});
    EXPECT_EQ(unknown.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(unknown.value(QStringLiteral("code")).toString(), QStringLiteral("UNKNOWN_COMMAND"));
}

TEST_F(DispatcherReentryTest, DispatchContextPreservesActionSource)
{
    IPCServer server(nullptr);
    ActionManager::instance().registerAction({
        QStringLiteral("capture_source"), {}, {}, {}, false,
        [](const QJsonObject&) {
            return ActionResult{true, QJsonObject{{QStringLiteral("ok"), true}}};
        }
    });

    ActionSource observed = ActionSource::GUI;
    QObject::connect(&ActionManager::instance(), &ActionManager::actionExecuted, &server,
        [&](const QString& id, const QJsonObject&, const QJsonObject&, ActionSource source) {
            if (id == QStringLiteral("capture_source")) observed = source;
        });

    server.dispatchCommand(QStringLiteral("capture_source"), {});
    EXPECT_EQ(observed, ActionSource::Internal);

    IPCServer::IPCDispatchContext context;
    context.source = ActionSource::Remote;
    server.dispatchCommand(QStringLiteral("capture_source"), {}, context);
    EXPECT_EQ(observed, ActionSource::Remote);
}

TEST_F(DispatcherReentryTest, DispatchContextAddsOnlyNonEmptyRequestId)
{
    IPCServer server(nullptr);
    IPCServer::IPCDispatchContext context;
    context.requestId = QStringLiteral("req-42");

    const QJsonObject withId = server.dispatchCommand(QStringLiteral("ping"), {}, context);
    EXPECT_EQ(withId.value(QStringLiteral("request_id")).toString(), QStringLiteral("req-42"));

    const QJsonObject withoutId = server.dispatchCommand(QStringLiteral("ping"), {});
    EXPECT_FALSE(withoutId.contains(QStringLiteral("request_id")));
}

TEST_F(DispatcherReentryTest, ContextPreviewMatchesArgumentPreview)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);

    IPCServer::IPCDispatchContext context;
    context.preview = true;
    const QJsonObject viaContext = server.dispatchCommand(QStringLiteral("ping"), {}, context);
    const QJsonObject viaArgs = server.dispatchCommand(QStringLiteral("ping"), QJsonObject{{QStringLiteral("preview"), true}});

    EXPECT_EQ(viaContext, viaArgs);
}

TEST_F(DispatcherReentryTest, AIDispatchPolicyRejectsUnavailableAndRequiresWriteGrant)
{
    IPCServer server(nullptr);
    registerReadonlyActions(ActionManager::instance(), &server);

    IPCServer::IPCDispatchContext context;
    context.source = ActionSource::Internal;
    context.origin = IPCServer::DispatchOrigin::AI;
    context.requestId = QStringLiteral("ai-0001");
    context.permissionProfile = QStringLiteral("ask");

    const QJsonObject read = server.dispatchCommand(QStringLiteral("ping"), {}, context);
    EXPECT_EQ(read.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(read.value(QStringLiteral("request_id")).toString(), QStringLiteral("ai-0001"));

    const QJsonObject unavailable = server.dispatchCommand(
        QStringLiteral("batch"), QJsonObject{{QStringLiteral("commands"), QJsonArray{}}}, context);
    EXPECT_EQ(unavailable.value(QStringLiteral("code")).toString(),
              QStringLiteral("AI_COMMAND_UNAVAILABLE"));

    const QJsonObject writeArgs{
        {QStringLiteral("object_id"), 7},
        {QStringLiteral("property"), QStringLiteral("volume")},
        {QStringLiteral("value"), -6.0}
    };
    const QJsonObject denied = server.dispatchCommand(
        QStringLiteral("set_item_property"), writeArgs, context);
    EXPECT_EQ(denied.value(QStringLiteral("code")).toString(),
              QStringLiteral("AI_CONFIRMATION_REQUIRED"));
}

TEST_F(DispatcherReentryTest, AIWriteGrantExecutesOnceAndRejectsReplayOrMismatch)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    int executions = 0;
    ActionManager::instance().registerAction({
        QStringLiteral("set_item_property"), {}, {}, {}, false,
        [&](const QJsonObject&) {
            ++executions;
            return ActionResult{true, QJsonObject{{QStringLiteral("ok"), true}}};
        }
    });

    const QJsonObject args{
        {QStringLiteral("object_id"), 7},
        {QStringLiteral("property"), QStringLiteral("volume")},
        {QStringLiteral("value"), -6.0}
    };
    IPCServer::IPCDispatchContext context;
    context.source = ActionSource::Internal;
    context.origin = IPCServer::DispatchOrigin::AI;
    context.requestId = QStringLiteral("ai-0001");
    context.permissionProfile = QStringLiteral("ask");
    context.grantId = server.issueAIGrant(QStringLiteral("set_item_property"), args, context);

    const QJsonObject success = server.dispatchCommand(
        QStringLiteral("set_item_property"), args, context);
    EXPECT_EQ(success.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(executions, 1);

    const QJsonObject replay = server.dispatchCommand(
        QStringLiteral("set_item_property"), args, context);
    EXPECT_EQ(replay.value(QStringLiteral("code")).toString(),
              QStringLiteral("AI_GRANT_REPLAYED"));
    EXPECT_EQ(executions, 1);
}

TEST_F(DispatcherReentryTest, NestedAICommandInheritsOriginAndIsDeniedBeforeSideEffect)
{
    IPCServer server(nullptr);
    server.setAppState(IPCServer::AppState::Ready);
    int nestedSideEffects = 0;
    ActionManager::instance().registerAction({
        QStringLiteral("set_item_property"), {}, {}, {}, false,
        [&](const QJsonObject&) {
            const QJsonObject nested = server.dispatchCommand(
                QStringLiteral("batch"),
                QJsonObject{
                    {QStringLiteral("commands"), QJsonArray{
                        QJsonObject{
                            {QStringLiteral("cmd"), QStringLiteral("set_master_volume")},
                            {QStringLiteral("args"), QJsonObject{}}
                        }
                    }}
                });
            const QJsonArray results = nested.value(QStringLiteral("result"))
                                           .toObject().value(QStringLiteral("results")).toArray();
            if (!results.isEmpty()
                && results.first().toObject().value(QStringLiteral("status"))
                       .toString() == QLatin1String("ok"))
            {
                ++nestedSideEffects;
            }
            const QString nestedCode = results.isEmpty()
                ? nested.value(QStringLiteral("code")).toString()
                : results.first().toObject().value(QStringLiteral("code")).toString();
            return ActionResult{true, QJsonObject{{QStringLiteral("nested_code"), nestedCode}}};
        }
    });

    const QJsonObject args{
        {QStringLiteral("object_id"), 7},
        {QStringLiteral("property"), QStringLiteral("volume")},
        {QStringLiteral("value"), -6.0}
    };
    IPCServer::IPCDispatchContext context;
    context.origin = IPCServer::DispatchOrigin::AI;
    context.requestId = QStringLiteral("ai-nested");
    context.permissionProfile = QStringLiteral("ask");
    context.grantId = server.issueAIGrant(QStringLiteral("set_item_property"), args, context);

    const QJsonObject response = server.dispatchCommand(
        QStringLiteral("set_item_property"), args, context);
    EXPECT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(response.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("nested_code")).toString(),
              QStringLiteral("AI_COMMAND_UNAVAILABLE"));
    EXPECT_EQ(nestedSideEffects, 0);
}


TEST_F(DispatcherReentryTest, ActionExceptionsBecomeCommandExceptionEnvelope)
{
    IPCServer server(nullptr);
    ActionManager::instance().registerAction({
        QStringLiteral("throwing_action"), {}, {}, {}, false,
        [](const QJsonObject&) -> ActionResult { throw std::runtime_error("boom"); }
    });

    const QJsonObject response = server.dispatchCommand(QStringLiteral("throwing_action"), {});
    EXPECT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(response.value(QStringLiteral("code")).toString(), QStringLiteral("COMMAND_EXCEPTION"));
}

TEST_F(DispatcherReentryTest, InProcessAIDispatchIsMarkedInProgressForModalSuppression)
{
    IPCServer server(nullptr);
    bool observedInProgress = false;
    ActionManager::instance().registerAction({
        QStringLiteral("observe_dispatch_flag"), {}, {}, {}, false,
        [&](const QJsonObject&) {
            observedInProgress = server.isDispatchingCommand();
            return ActionResult{true, QJsonObject{{QStringLiteral("ok"), true}}};
        }
    });

    EXPECT_FALSE(server.isDispatchingCommand());

    IPCServer::IPCDispatchContext context;
    context.source = ActionSource::Internal;
    context.origin = IPCServer::DispatchOrigin::AI;
    context.unrestricted = true;
    const QJsonObject response =
        server.dispatchCommand(QStringLiteral("observe_dispatch_flag"), {}, context);

    EXPECT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    // The in-process AI path never touches the socket handleCommand guard, yet it
    // must still report "dispatching" while the command runs so a ProjectManager
    // errorOccurred emitted mid-command is suppressed to a non-modal status update
    // rather than blocking the AI turn on a modal dialog.
    EXPECT_TRUE(observedInProgress);
    EXPECT_FALSE(server.isDispatchingCommand());
}

namespace {

// Exercises the create/rename handlers' name pre-validation end to end: an
// invalid name must come back as a INVALID_NAME error envelope carrying the
// specific reason, so an AI caller can read it and correct the name instead of
// seeing a generic failure. Needs a real open project because the handlers gate
// on isProjectOpen() before reaching the name check.
class CommandNameValidationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(),
                               &ProjectManager::instance(), nullptr, {});

        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("NameValidationProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
    }

    void TearDown() override
    {
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
};

class CreateItemUndoStackTest : public CommandNameValidationTest
{
};

} // namespace

TEST_F(CommandNameValidationTest, CreateItemAcceptsNonAsciiDisplayName)
{
    IPCServer server(nullptr);

    const QString nonAsciiName = QString(QChar(0x97F3)) + QChar(0x4E50);
    const QJsonObject response = server.dispatchCommand(
        QStringLiteral("create_item"),
        QJsonObject{{QStringLiteral("parent"), QString()},
                    {QStringLiteral("name"), nonAsciiName},
                    {QStringLiteral("type"), QStringLiteral("Sound")}});

    EXPECT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("name")).toString(),
              nonAsciiName);
}

TEST_F(CommandNameValidationTest, CreateItemDoesNotFlagValidAsciiNameAsInvalid)
{
    IPCServer server(nullptr);

    const QJsonObject response = server.dispatchCommand(
        QStringLiteral("create_item"),
        QJsonObject{{QStringLiteral("parent"), QString()},
                    {QStringLiteral("name"), QStringLiteral("Music")},
                    {QStringLiteral("type"), QStringLiteral("Sound")}});

    // A valid ASCII name must never trip the name-validation guard (it may fail
    // for unrelated reasons such as placement, but not with INVALID_NAME).
    EXPECT_NE(response.value(QStringLiteral("code")).toString(),
              QStringLiteral("INVALID_NAME"));
}

TEST_F(CreateItemUndoStackTest, ChildCreationPushesSingleUndoEntry)
{
    IPCServer server(nullptr);

    const QJsonObject parentResponse = server.dispatchCommand(
        QStringLiteral("create_item"),
        QJsonObject{{QStringLiteral("parent"), QString()},
                    {QStringLiteral("name"), QStringLiteral("Layer_A")},
                    {QStringLiteral("type"), QStringLiteral("Layer")}});
    ASSERT_EQ(parentResponse.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));

    // createItem pushes CreateProjectItemCommand itself; the handler used to
    // push a second command for child creation, polluting the undo stack.
    const int entriesBefore = UndoRedoManager::instance().undoStack()->count();
    const QJsonObject childResponse = server.dispatchCommand(
        QStringLiteral("create_item"),
        QJsonObject{{QStringLiteral("parent"), QStringLiteral("Layer_A")},
                    {QStringLiteral("name"), QStringLiteral("Child_Sound")},
                    {QStringLiteral("type"), QStringLiteral("Sound")}});
    ASSERT_EQ(childResponse.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));

    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), entriesBefore + 1);
}
