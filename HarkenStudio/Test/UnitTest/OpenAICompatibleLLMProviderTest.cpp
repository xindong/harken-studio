#include "AI/OpenAICompatibleLLMProvider.h"
#include "Support/AITestSupport.h"
#include "Support/SyntheticHttpServer.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>

using namespace VNX;

namespace {

QByteArray responseWithMessage(const QJsonObject& message)
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("choices"), QJsonArray{
            QJsonObject{{QStringLiteral("message"), message}}
        }}
    }).toJson(QJsonDocument::Compact);
}

ILLMProvider::Request requestFor(const QUrl& endpoint,
                                 const QString& id = QStringLiteral("request-1"))
{
    ILLMProvider::Request request;
    request.requestId = id;
    request.model = QStringLiteral("test-model");
    request.systemPrompt = QStringLiteral("System instructions.");
    request.messages = QJsonArray{
        QJsonObject{
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("content"), QStringLiteral("Hello")}
        }
    };
    request.apiKey = QStringLiteral("secret-token");
    request.endpoint = endpoint;
    return request;
}

class ProviderObserver
{
public:
    explicit ProviderObserver(OpenAICompatibleLLMProvider& provider)
    {
        QObject::connect(&provider, &ILLMProvider::completed, &provider,
                         [this](const ILLMProvider::Response& value) {
            response = value;
            ++completedCount;
        });
        QObject::connect(&provider, &ILLMProvider::failed, &provider,
                         [this](const QString&, const QString& code,
                                const QString& message) {
            failedCode = code;
            failedMessage = message;
            ++failedCount;
        });
    }

    ILLMProvider::Response response;
    QString failedCode;
    QString failedMessage;
    int completedCount = 0;
    int failedCount = 0;
};

class OpenAICompatibleLLMProviderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ASSERT_TRUE(server.listen(true));
    }

    TestSupport::SyntheticHttpServer server;
};

}

TEST_F(OpenAICompatibleLLMProviderTest, KeepsCredentialsOutOfRequestBody)
{
    server.enqueueResponse({200, responseWithMessage(
        QJsonObject{{QStringLiteral("content"), QStringLiteral("done")}})});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl()));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 1; }));

    EXPECT_EQ(server.lastHeader("Authorization"), QByteArrayLiteral("Bearer secret-token"));
    EXPECT_FALSE(server.lastBody().contains("secret-token"));
}

TEST_F(OpenAICompatibleLLMProviderTest, CancelAndReplacementSuppressStaleDelivery)
{
    server.enqueueResponse({200, responseWithMessage(
        QJsonObject{{QStringLiteral("content"), QStringLiteral("cancelled")}}), {}, 200});
    server.enqueueResponse({200, responseWithMessage(
        QJsonObject{{QStringLiteral("content"), QStringLiteral("stale")}}), {}, 200});
    server.enqueueResponse({200, responseWithMessage(
        QJsonObject{{QStringLiteral("content"), QStringLiteral("replacement")}})});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("cancelled")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return server.requestCount() == 1; }));
    provider.cancel(QStringLiteral("cancelled"));
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return server.delayedResponseAttemptCount() == 1;
    }));
    EXPECT_EQ(observer.completedCount, 0);
    EXPECT_EQ(observer.failedCount, 0);

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("stale")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return server.requestCount() == 2; }));
    provider.submit(requestFor(server.baseUrl(), QStringLiteral("replacement")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return server.requestCount() == 3; }));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 1; }));
    EXPECT_EQ(observer.response.requestId, QStringLiteral("replacement"));
    EXPECT_EQ(observer.response.text, QStringLiteral("replacement"));
    EXPECT_EQ(observer.failedCount, 0);
}

TEST_F(OpenAICompatibleLLMProviderTest, RedactsCredentialsFromProtocolErrors)
{
    QString message = QStringLiteral("secret-token");
    message.append(QChar(1));
    message += QStringLiteral(" secret-token ");
    message += QString(700, QLatin1Char('x'));
    const QByteArray errorBody = QJsonDocument(QJsonObject{
        {QStringLiteral("error"), QJsonObject{
            {QStringLiteral("message"), message}
        }}
    }).toJson(QJsonDocument::Compact);
    server.enqueueResponse({400, errorBody});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl()));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }));
    EXPECT_FALSE(observer.failedMessage.contains(QStringLiteral("secret-token")));
    EXPECT_FALSE(observer.failedMessage.contains(QChar(1)));
}
