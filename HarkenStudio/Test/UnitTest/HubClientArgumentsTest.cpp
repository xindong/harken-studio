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

TEST(HubClientArgumentsTest, ForwardingRequiresMatchingAcknowledgementBeforeExit)
{
    ensureCoreApp();
    enum class Response { Acknowledged, WrongToken, Disconnected, Timeout };
    for (const auto response : {Response::Acknowledged, Response::WrongToken,
                                Response::Disconnected, Response::Timeout}) {
        QLocalServer server;
        const QString serverName = QStringLiteral("hub-forwarded-test-%1-%2")
            .arg(QCoreApplication::applicationPid()).arg(static_cast<int>(response));
        QLocalServer::removeServer(serverName);
        ASSERT_TRUE(server.listen(serverName));
        VNX::HubClient::Arguments arguments;
        arguments.present = true;
        arguments.serverName = serverName;
        arguments.sessionToken = QStringLiteral("token");
        arguments.launchIntent = QStringLiteral("DirectOpenOnly");

        VNX::HubClient client(arguments);
        int completionCount = 0;
        bool acknowledged = false;
        QObject::connect(&client, &VNX::HubClient::forwardedCompleted, [&](bool result) {
            ++completionCount;
            acknowledged = result;
        });
        client.connectAndHelloAsync();
        ASSERT_TRUE(spinUntil([&]() { return client.isConnected() && server.hasPendingConnections(); }));
        QLocalSocket* socket = server.nextPendingConnection();
        ASSERT_NE(socket, nullptr);
        ASSERT_TRUE(spinUntil([&]() { return socket->canReadLine(); }));
        socket->readLine();

        client.reportForwarded(response == Response::Timeout ? 50 : 1000);
        EXPECT_EQ(completionCount, 0);
        ASSERT_TRUE(spinUntil([&]() { return socket->canReadLine(); }));
        socket->readLine();

        if (response == Response::Acknowledged || response == Response::WrongToken) {
            const auto reply = QJsonDocument(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("forwardedAccepted")},
                {QStringLiteral("token"), response == Response::Acknowledged
                    ? arguments.sessionToken : QStringLiteral("wrong-token")},
            }).toJson(QJsonDocument::Compact);
            socket->write(reply);
            socket->flush();
            QCoreApplication::processEvents();
            EXPECT_EQ(completionCount, 0);
            socket->write("\n");
            socket->flush();
        }
        if (response != Response::Timeout)
            socket->disconnectFromServer();

        ASSERT_TRUE(spinUntil([&]() { return completionCount > 0; }));
        QCoreApplication::processEvents();
        EXPECT_EQ(completionCount, 1);
        EXPECT_EQ(acknowledged, response == Response::Acknowledged);
    }
}
