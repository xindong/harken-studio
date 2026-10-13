#include "Support/AITestSupport.h"
#include "Support/SyntheticHttpServer.h"

#include <gtest/gtest.h>

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopedPointer>
#include <QSslSocket>

using namespace VNX;

TEST(SyntheticHttpServerTest, DeliversEntireLargeResponse)
{
    TestSupport::ensureApp();
    const QByteArray body(1024 * 1024 + 1, 'x');
    for (const bool tls : {false, true})
    {
        SCOPED_TRACE(tls ? "HTTPS" : "HTTP");
        TestSupport::SyntheticHttpServer server;
        ASSERT_TRUE(server.listen(tls));
        server.enqueueResponse({200, body});
        QNetworkAccessManager network;
        QScopedPointer<QNetworkReply> reply(network.get(QNetworkRequest(server.baseUrl())));

        ASSERT_TRUE(TestSupport::spinUntil([&]() { return reply->isFinished(); }));
        EXPECT_EQ(reply->error(), QNetworkReply::NoError) << reply->errorString().toStdString();
        const QByteArray received = reply->readAll();
        EXPECT_EQ(received.size(), body.size());
        EXPECT_TRUE(received == body);
    }
}

TEST(SyntheticHttpServerTest, HandlesSequentialRequestsOnSameConnection)
{
    TestSupport::ensureApp();
    for (const bool tls : {false, true})
    {
        SCOPED_TRACE(tls ? "HTTPS" : "HTTP");
        TestSupport::SyntheticHttpServer server;
        ASSERT_TRUE(server.listen(tls));
        QSslSocket socket;
        const QUrl endpoint = server.baseUrl();
        if (tls)
            socket.connectToHostEncrypted(endpoint.host(), endpoint.port());
        else
            socket.connectToHost(endpoint.host(), endpoint.port());
        ASSERT_TRUE(TestSupport::spinUntil([&]() {
            return tls ? socket.isEncrypted() : socket.state() == QAbstractSocket::ConnectedState;
        })) << socket.errorString().toStdString();

        for (const QByteArray& body : {QByteArrayLiteral("first"), QByteArrayLiteral("second")})
        {
            server.enqueueResponse({200, body});
            const QByteArray request = QByteArrayLiteral("POST /v1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: ")
                + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
            ASSERT_EQ(socket.write(request), request.size());

            QByteArray received;
            ASSERT_TRUE(TestSupport::spinUntil([&]() {
                received += socket.readAll();
                const qsizetype headerEnd = received.indexOf("\r\n\r\n");
                return headerEnd >= 0 && received.size() == headerEnd + 4 + body.size();
            })) << socket.errorString().toStdString();
            EXPECT_TRUE(received.endsWith(QByteArrayLiteral("\r\n\r\n") + body));
            EXPECT_EQ(server.lastBody(), body);
        }
        EXPECT_EQ(server.requestCount(), 2);
        EXPECT_EQ(server.connectionCount(), 1);
    }
}
