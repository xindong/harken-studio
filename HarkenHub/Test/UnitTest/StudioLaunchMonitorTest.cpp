#include "Launch/StudioLaunchMonitor.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

#include <functional>

using VNH::StudioLaunchMonitor;

namespace {

bool spinUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

struct Capture
{
    QStringList stages;
    int readyCount = 0;
    int forwardedCount = 0;
    int failedCount = 0;
    QString failure;
};

Capture* wire(StudioLaunchMonitor& monitor, Capture& capture)
{
    QObject::connect(&monitor, &StudioLaunchMonitor::stageChanged,
                     [&capture](const QString& stage) {
                         capture.stages.append(stage);
                     });
    QObject::connect(&monitor, &StudioLaunchMonitor::ready,
                     [&capture]() { ++capture.readyCount; });
    QObject::connect(&monitor, &StudioLaunchMonitor::forwarded,
                     [&capture]() { ++capture.forwardedCount; });
    QObject::connect(&monitor, &StudioLaunchMonitor::failed,
                     [&capture](const QString& error) {
                         ++capture.failedCount;
                         capture.failure = error;
                     });
    return &capture;
}

void sendMessage(QLocalSocket& socket, const QJsonObject& message)
{
    socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact) +
                 QByteArrayLiteral("\n"));
    socket.flush();
}

QJsonObject helloFor(const StudioLaunchMonitor& monitor,
                     const QString& buildId)
{
    QJsonObject hello;
    hello[QStringLiteral("type")] = QStringLiteral("hello");
    hello[QStringLiteral("token")] = monitor.sessionToken();
    hello[QStringLiteral("protocolVersion")] = 2;
    hello[QStringLiteral("pid")] =
        static_cast<double>(QCoreApplication::applicationPid());
    hello[QStringLiteral("buildId")] = buildId;
    hello[QStringLiteral("launchIntent")] = QStringLiteral("DirectOpenOnly");
    return hello;
}

QJsonObject createHelloFor(const StudioLaunchMonitor& monitor,
                           const QString& buildId)
{
    QJsonObject hello = helloFor(monitor, buildId);
    hello[QStringLiteral("operation")] = QStringLiteral("createProject");
    return hello;
}

QJsonObject legacyHelloFor(const StudioLaunchMonitor& monitor,
                           const QString& buildId,
                           const QString& operation)
{
    QJsonObject hello;
    hello[QStringLiteral("type")] = QStringLiteral("hello");
    hello[QStringLiteral("token")] = monitor.sessionToken();
    hello[QStringLiteral("protocolVersion")] = 1;
    hello[QStringLiteral("pid")] =
        static_cast<double>(QCoreApplication::applicationPid());
    hello[QStringLiteral("buildId")] = buildId;
    hello[QStringLiteral("operation")] = operation;
    return hello;
}

QJsonObject stageFor(const StudioLaunchMonitor& monitor, const QString& stage)
{
    QJsonObject message;
    message[QStringLiteral("type")] = QStringLiteral("stage");
    message[QStringLiteral("token")] = monitor.sessionToken();
    message[QStringLiteral("stage")] = stage;
    return message;
}

TEST(StudioLaunchMonitor, AcceptsHelloStagesAndReady)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    monitor.setExpectedBuildId(QStringLiteral("run1.aaaa1111"));
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));

    sendMessage(socket, helloFor(monitor, QStringLiteral("run1.aaaa1111")));
    sendMessage(socket, stageFor(monitor, QStringLiteral("processStarted")));
    sendMessage(socket, stageFor(monitor, QStringLiteral("projectRead")));
    sendMessage(socket, stageFor(monitor, QStringLiteral("projectCreated")));
    sendMessage(socket, stageFor(monitor, QStringLiteral("ready")));

    ASSERT_TRUE(spinUntil([&]() { return capture.readyCount == 1; }));
    EXPECT_EQ(capture.failedCount, 0);
    EXPECT_EQ(capture.stages,
              (QStringList{QStringLiteral("processStarted"),
                           QStringLiteral("projectRead"),
                           QStringLiteral("projectCreated"),
                           QStringLiteral("ready")}));
    EXPECT_TRUE(monitor.helloAccepted());
}

TEST(StudioLaunchMonitor, ProcessStartedIsNotTerminal)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));

    sendMessage(socket, helloFor(monitor, QStringLiteral("run1")));
    sendMessage(socket, stageFor(monitor, QStringLiteral("processStarted")));

    ASSERT_TRUE(spinUntil([&]() { return capture.stages.size() == 1; }));
    EXPECT_EQ(capture.readyCount, 0);
    EXPECT_EQ(capture.failedCount, 0);
    EXPECT_EQ(monitor.lastStage(), QStringLiteral("processStarted"));

    sendMessage(socket, stageFor(monitor, QStringLiteral("ready")));
    ASSERT_TRUE(spinUntil([&]() { return capture.readyCount == 1; }));
    EXPECT_EQ(capture.failedCount, 0);
}

TEST(StudioLaunchMonitor, ForwardingIsAcknowledgedAndTerminalWithoutClaimingReadiness)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    monitor.setExpectedOperation(QStringLiteral("openProject"));
    monitor.setExpectedBuildId(QStringLiteral("run1"));
    monitor.setExpectedProcess(QCoreApplication::applicationPid(),
                               QCoreApplication::applicationFilePath());
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));
    QJsonObject hello = helloFor(monitor, QStringLiteral("run1"));
    hello[QStringLiteral("operation")] = QStringLiteral("openProject");
    sendMessage(socket, hello);
    sendMessage(socket, stageFor(monitor, QStringLiteral("forwarded")));
    sendMessage(socket, stageFor(monitor, QStringLiteral("ready")));
    sendMessage(socket, stageFor(monitor, QStringLiteral("forwarded")));

    ASSERT_TRUE(spinUntil([&]() { return capture.forwardedCount == 1 && socket.canReadLine(); }));
    const auto acknowledgement = QJsonDocument::fromJson(socket.readLine()).object();
    EXPECT_EQ(acknowledgement.value(QStringLiteral("type")).toString(), QStringLiteral("forwardedAccepted"));
    EXPECT_EQ(acknowledgement.value(QStringLiteral("token")).toString(), monitor.sessionToken());
    EXPECT_EQ(capture.stages, QStringList{QStringLiteral("forwarded")});
    EXPECT_EQ(capture.readyCount, 0);
    EXPECT_EQ(capture.failedCount, 0);

    socket.disconnectFromServer();
    QCoreApplication::processEvents();
    EXPECT_EQ(capture.forwardedCount, 1);
    EXPECT_EQ(capture.failedCount, 0);
}

TEST(StudioLaunchMonitor, ForwardingRequiresAnOpenOperation)
{
    for (const auto& operation : {QString(), QStringLiteral("createProject")}) {
        StudioLaunchMonitor monitor;
        ASSERT_TRUE(monitor.listen());
        Capture capture;
        wire(monitor, capture);

        QLocalSocket socket;
        socket.connectToServer(monitor.serverName());
        ASSERT_TRUE(socket.waitForConnected(3000));
        QJsonObject hello = helloFor(monitor, QStringLiteral("run1"));
        hello[QStringLiteral("operation")] = operation;
        sendMessage(socket, hello);
        sendMessage(socket, stageFor(monitor, QStringLiteral("forwarded")));

        ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
        EXPECT_EQ(capture.forwardedCount, 0);
        EXPECT_EQ(capture.readyCount, 0);
        EXPECT_FALSE(socket.canReadLine());
    }
}

TEST(StudioLaunchMonitor, ReportsStudioFailure)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));

    sendMessage(socket, helloFor(monitor, QStringLiteral("run1.aaaa1111")));
    QJsonObject failed = stageFor(monitor, QStringLiteral("failed"));
    failed[QStringLiteral("error")] = QStringLiteral("Project is corrupt");
    failed[QStringLiteral("errorCode")] = QStringLiteral("PROJECT_FILE_DAMAGED");
    sendMessage(socket, failed);

    ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
    EXPECT_EQ(capture.failure, QStringLiteral("Project is corrupt"));
    EXPECT_EQ(capture.readyCount, 0);
}

TEST(StudioLaunchMonitor, AcceptsExpectedProjectCreationOperation)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    monitor.setExpectedOperation(QStringLiteral("createProject"));
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));
    sendMessage(socket, createHelloFor(monitor, QStringLiteral("run1")));

    ASSERT_TRUE(spinUntil([&]() { return monitor.helloAccepted(); }));
    EXPECT_EQ(capture.failedCount, 0);
}

TEST(StudioLaunchMonitor, RejectsProjectCreationClientWithoutOperation)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    monitor.setExpectedOperation(QStringLiteral("createProject"));
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));
    sendMessage(socket, helloFor(monitor, QStringLiteral("run1")));

    ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
    EXPECT_TRUE(capture.failure.contains(QStringLiteral("Update HarkenStudio")));
    EXPECT_FALSE(monitor.helloAccepted());
}

TEST(StudioLaunchMonitor, RejectsLegacyProjectCreationClient)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    monitor.setExpectedOperation(QStringLiteral("createProject"));
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));
    sendMessage(socket, legacyHelloFor(monitor, QStringLiteral("run1"),
                                       QStringLiteral("createProject")));

    ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
    EXPECT_FALSE(monitor.helloAccepted());
}

TEST(StudioLaunchMonitor, RejectsLegacyOpenClient)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    monitor.setExpectedOperation(QStringLiteral("openProject"));
    monitor.setExpectedLaunchIntent(QStringLiteral("DirectOpenOnly"));
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));
    sendMessage(socket, legacyHelloFor(monitor, QStringLiteral("run1"),
                                       QStringLiteral("openProject")));

    ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
    EXPECT_FALSE(monitor.helloAccepted());
}

TEST(StudioLaunchMonitor, RejectsUnsupportedProtocol)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));
    QJsonObject hello = helloFor(monitor, QStringLiteral("run1"));
    hello[QStringLiteral("protocolVersion")] = 3;
    sendMessage(socket, hello);

    ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
    EXPECT_TRUE(capture.failure.contains(QStringLiteral("protocol version")));
    EXPECT_FALSE(monitor.helloAccepted());
}

TEST(StudioLaunchMonitor, RejectsWrongToken)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));

    QJsonObject hello = helloFor(monitor, QStringLiteral("run1.aaaa1111"));
    hello[QStringLiteral("token")] = QStringLiteral("forged");
    sendMessage(socket, hello);

    ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
    EXPECT_FALSE(monitor.helloAccepted());
}

TEST(StudioLaunchMonitor, RejectsBuildIdMismatch)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    monitor.setExpectedBuildId(QStringLiteral("run1.aaaa1111"));
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));

    sendMessage(socket, helloFor(monitor, QStringLiteral("run9.ffff9999")));

    ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
    EXPECT_FALSE(monitor.helloAccepted());
}

TEST(StudioLaunchMonitor, ValidatesActualLaunchedProcessAndExecutable)
{
    for (int mismatch = 0; mismatch < 3; ++mismatch) {
        StudioLaunchMonitor monitor;
        ASSERT_TRUE(monitor.listen());
        monitor.setExpectedProcess(QCoreApplication::applicationPid() + (mismatch == 1 ? 1 : 0),
            mismatch == 2 ? QStringLiteral("missing-studio") : QCoreApplication::applicationFilePath());
        Capture capture;
        wire(monitor, capture);
        QLocalSocket socket;
        socket.connectToServer(monitor.serverName());
        ASSERT_TRUE(socket.waitForConnected(3000));
        sendMessage(socket, helloFor(monitor, QStringLiteral("run1")));
        ASSERT_TRUE(spinUntil([&]() { return monitor.helloAccepted() || capture.failedCount > 0; }));
        EXPECT_EQ(monitor.helloAccepted(), mismatch == 0);
        monitor.close();
    }
}

TEST(StudioLaunchMonitor, RejectsHelloWithoutLaunchIntent)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));

    QJsonObject hello = helloFor(monitor, QStringLiteral("run1"));
    hello.remove(QStringLiteral("launchIntent"));
    sendMessage(socket, hello);

    ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
    EXPECT_FALSE(monitor.helloAccepted());
}

TEST(StudioLaunchMonitor, RejectsStageBeforeHello)
{
    for (const auto& stage : {QStringLiteral("ready"), QStringLiteral("forwarded")}) {
        StudioLaunchMonitor monitor;
        ASSERT_TRUE(monitor.listen());
        Capture capture;
        wire(monitor, capture);

        QLocalSocket socket;
        socket.connectToServer(monitor.serverName());
        ASSERT_TRUE(socket.waitForConnected(3000));

        sendMessage(socket, stageFor(monitor, stage));

        ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
        EXPECT_EQ(capture.readyCount, 0);
        EXPECT_EQ(capture.forwardedCount, 0);
    }
}

TEST(StudioLaunchMonitor, RejectsUnknownStage)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));

    sendMessage(socket, helloFor(monitor, QString()));
    sendMessage(socket, stageFor(monitor, QStringLiteral("mystery")));

    ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
}

TEST(StudioLaunchMonitor, RejectsFailureWithoutErrorCode)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));

    sendMessage(socket, helloFor(monitor, QStringLiteral("run1")));
    QJsonObject failed = stageFor(monitor, QStringLiteral("failed"));
    failed[QStringLiteral("error")] = QStringLiteral("Project is corrupt");
    sendMessage(socket, failed);

    ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
    EXPECT_TRUE(capture.failure.contains(QStringLiteral("error code")));
    EXPECT_EQ(capture.readyCount, 0);
}


TEST(StudioLaunchMonitor, ReportsDisconnectBeforeReady)
{
    StudioLaunchMonitor monitor;
    ASSERT_TRUE(monitor.listen());
    Capture capture;
    wire(monitor, capture);

    QLocalSocket socket;
    socket.connectToServer(monitor.serverName());
    ASSERT_TRUE(socket.waitForConnected(3000));
    sendMessage(socket, helloFor(monitor, QStringLiteral("run1")));
    ASSERT_TRUE(spinUntil([&]() { return monitor.helloAccepted(); }));

    socket.disconnectFromServer();
    ASSERT_TRUE(spinUntil([&]() { return capture.failedCount == 1; }));
    EXPECT_TRUE(capture.failure.contains(QStringLiteral("was lost")));
    EXPECT_EQ(capture.readyCount, 0);
}

}
