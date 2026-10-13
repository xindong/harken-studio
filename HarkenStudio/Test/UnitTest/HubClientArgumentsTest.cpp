#include "Core/HubClient/HubClient.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>

#include <functional>

namespace {

void ensureCoreApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QCoreApplication app(argc, argv);
}

bool spinUntil(const std::function<bool()>& predicate, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

}

TEST(HubClientArgumentsTest, ParsesProjectCreationRequest)
{
    ensureCoreApp();
    const QStringList args = {
        QStringLiteral("HarkenStudio"),
        QStringLiteral("--create-project"),
        QStringLiteral("--project-name"),
        QStringLiteral("My Project"),
        QStringLiteral("--project-location"),
        QStringLiteral("D:/Projects"),
        QStringLiteral("--project-template-id"),
        QStringLiteral("mmorpg"),
        QStringLiteral("--project-template-file"),
        QStringLiteral("D:/Studio/mmorpg.json"),
        QStringLiteral("--hub-session"),
        QStringLiteral("session"),
        QStringLiteral("--hub-token"),
        QStringLiteral("token"),
        QStringLiteral("--launch-intent"),
        QStringLiteral("DirectOpenOnly"),
    };

    const VNX::HubClient::Arguments parsed =
        VNX::HubClient::parseArguments(args);
    EXPECT_TRUE(parsed.present);
    EXPECT_TRUE(parsed.createProject);
    EXPECT_EQ(parsed.projectName, QStringLiteral("My Project"));
    EXPECT_EQ(parsed.projectLocation, QStringLiteral("D:/Projects"));
    EXPECT_EQ(parsed.projectTemplateId, QStringLiteral("mmorpg"));
    EXPECT_EQ(parsed.projectTemplatePath,
              QStringLiteral("D:/Studio/mmorpg.json"));
}

TEST(HubClientArgumentsTest, ParsesStandaloneProjectCreationPath)
{
    ensureCoreApp();
    const QStringList args = {
        QStringLiteral("HarkenStudio"),
        QStringLiteral("--create-project"),
        QStringLiteral("D:/Projects/My Project"),
    };

    const VNX::HubClient::Arguments parsed =
        VNX::HubClient::parseArguments(args);
    EXPECT_FALSE(parsed.present);
    EXPECT_TRUE(parsed.createProject);
    EXPECT_EQ(parsed.projectName, QStringLiteral("My Project"));
    EXPECT_EQ(parsed.projectLocation, QStringLiteral("D:/Projects"));
}

TEST(HubClientArgumentsTest, ParsesStandaloneProjectCreationPathWithTrailingSeparator)
{
    ensureCoreApp();
    const QStringList args = {
        QStringLiteral("HarkenStudio"),
        QStringLiteral("--create-project"),
        QStringLiteral("D:/Projects/My Project/"),
    };

    const VNX::HubClient::Arguments parsed =
        VNX::HubClient::parseArguments(args);
    EXPECT_TRUE(parsed.createProject);
    EXPECT_EQ(parsed.projectName, QStringLiteral("My Project"));
    EXPECT_EQ(parsed.projectLocation, QStringLiteral("D:/Projects"));
}
TEST(HubClientArgumentsTest, ConnectAndHelloReportsOpenOperation)
{
    ensureCoreApp();
    QLocalServer server;
    const QString serverName =
        QStringLiteral("hub-client-test-%1").arg(QCoreApplication::applicationPid());
    QLocalServer::removeServer(serverName);
    ASSERT_TRUE(server.listen(serverName));

    VNX::HubClient::Arguments arguments;
    arguments.present = true;
    arguments.serverName = serverName;
    arguments.sessionToken = QStringLiteral("token");
    arguments.projectPath = QStringLiteral("D:/Projects/Test.hsp");
    arguments.launchIntent = QStringLiteral("DirectOpenOnly");

    VNX::HubClient client(arguments);
    bool completed = false;
    bool connected = false;
    QObject::connect(&client, &VNX::HubClient::helloCompleted,
                     [&](bool result) {
        completed = true;
        connected = result;
    });
    client.connectAndHelloAsync();
    ASSERT_TRUE(spinUntil([&]() { return completed; }));
    ASSERT_TRUE(connected);
    ASSERT_TRUE(spinUntil([&]() { return server.hasPendingConnections(); }));
    QLocalSocket* socket = server.nextPendingConnection();
    ASSERT_NE(socket, nullptr);
    ASSERT_TRUE(spinUntil([&]() { return socket->canReadLine(); }));

    const QJsonObject hello =
        QJsonDocument::fromJson(socket->readLine()).object();
    EXPECT_EQ(hello.value(QStringLiteral("type")).toString(),
              QStringLiteral("hello"));
    EXPECT_EQ(hello.value(QStringLiteral("operation")).toString(),
              QStringLiteral("openProject"));
    EXPECT_EQ(hello.value(QStringLiteral("token")).toString(),
              QStringLiteral("token"));
    EXPECT_EQ(hello.value(QStringLiteral("launchIntent")).toString(),
              QStringLiteral("DirectOpenOnly"));
    EXPECT_EQ(hello.value(QStringLiteral("protocolVersion")).toInt(), 2);
    EXPECT_TRUE(client.isConnected());
}

TEST(HubClientArgumentsTest, ConnectAndHelloFailsWithoutServer)
{
    ensureCoreApp();
    const QString serverName = QStringLiteral("missing-hub-client-test-%1")
                                   .arg(QCoreApplication::applicationPid());
    QLocalServer::removeServer(serverName);

    VNX::HubClient::Arguments arguments;
    arguments.present = true;
    arguments.serverName = serverName;
    arguments.sessionToken = QStringLiteral("token");
    arguments.launchIntent = QStringLiteral("DirectOpenOnly");

    VNX::HubClient client(arguments);
    bool completed = false;
    bool connected = true;
    QObject::connect(&client, &VNX::HubClient::helloCompleted,
                     [&](bool result) {
        completed = true;
        connected = result;
    });
    QElapsedTimer callTimer;
    callTimer.start();
    client.connectAndHelloAsync(10);
    EXPECT_LT(callTimer.elapsed(), 10);
    ASSERT_TRUE(spinUntil([&]() { return completed; }));
    EXPECT_FALSE(connected);
    EXPECT_FALSE(client.isConnected());
}

TEST(HubClientArgumentsTest, ParsesHubUpgradeApproval)
{
    ensureCoreApp();
    const QStringList args = {
        QStringLiteral("HarkenStudio"),
        QStringLiteral("--hub-session"), QStringLiteral("session"),
        QStringLiteral("--hub-token"), QStringLiteral("token"),
        QStringLiteral("--launch-intent"),
        QStringLiteral("UpgradeApprovedByHub"),
        QStringLiteral("--project-open-request-id"),
        QStringLiteral("request"),
        QStringLiteral("--project-open-approval"),
        QString(64, QLatin1Char('a')),
    };

    const VNX::HubClient::Arguments parsed =
        VNX::HubClient::parseArguments(args);
    EXPECT_TRUE(parsed.present);
    EXPECT_EQ(parsed.projectOpenRequestId, QStringLiteral("request"));
    EXPECT_EQ(parsed.projectOpenApproval, QString(64, QLatin1Char('a')));
}

TEST(HubClientArgumentsTest, StandaloneProjectAndBoundApprovalDoNotRequireHubArguments)
{
    const auto parsed = VNX::HubClient::parseArguments({
        QStringLiteral("HarkenStudio"),
        QStringLiteral("--project"), QStringLiteral("D:/Projects/Legacy.hsp"),
        QStringLiteral("--launch-intent"), QStringLiteral("UpgradeApprovedByHub"),
        QStringLiteral("--project-open-request-id"), QStringLiteral("request"),
        QStringLiteral("--project-open-approval"), QString(64, QLatin1Char('a')),
    });
    EXPECT_FALSE(parsed.present);
    EXPECT_EQ(parsed.projectPath, QStringLiteral("D:/Projects/Legacy.hsp"));
    EXPECT_EQ(parsed.projectOpenRequestId, QStringLiteral("request"));
    EXPECT_EQ(parsed.projectOpenApproval, QString(64, QLatin1Char('a')));
}
