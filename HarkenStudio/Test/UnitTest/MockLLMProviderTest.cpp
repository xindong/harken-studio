#include "Support/AITestSupport.h"
#include "Support/MockLLMProvider.h"

#include <gtest/gtest.h>

#include <QCoreApplication>

using namespace VNX;

namespace {

ILLMProvider::Request request(const QString& id)
{
    ILLMProvider::Request value;
    value.requestId = id;
    return value;
}

}

TEST(MockLLMProviderTest, AutoCompletionIsAsynchronousAndDeterministic)
{
    TestSupport::ensureApp();
    MockLLMProvider provider;
    provider.enqueueTextResponse(QStringLiteral("ready"));

    int completionCount = 0;
    ILLMProvider::Response observed;
    QObject::connect(&provider, &ILLMProvider::completed, &provider,
                     [&](const ILLMProvider::Response& response) {
        ++completionCount;
        observed = response;
    });

    provider.submit(request(QStringLiteral("ai-0001")));
    EXPECT_EQ(completionCount, 0);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return completionCount == 1; }));
    EXPECT_EQ(observed.requestId, QStringLiteral("ai-0001"));
    EXPECT_EQ(observed.text, QStringLiteral("ready"));
    EXPECT_TRUE(observed.finished);
}

TEST(MockLLMProviderTest, TypedToolCallsSupportMultiRoundTranscriptCapture)
{
    TestSupport::ensureApp();
    MockLLMProvider provider;
    provider.enqueueToolCallsResponse({
        {QStringLiteral("call-1"), QStringLiteral("ping"), QJsonObject{}},
        {QStringLiteral("call-2"), QStringLiteral("get_app_state"), QJsonObject{}}
    });
    provider.enqueueTextResponse(QStringLiteral("done"));

    QList<ILLMProvider::Response> responses;
    QObject::connect(&provider, &ILLMProvider::completed, &provider,
                     [&](const ILLMProvider::Response& response) {
        responses.append(response);
    });
    auto first = request(QStringLiteral("ai-typed"));
    provider.submit(first);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses.size() == 1; }));
    ASSERT_EQ(responses.first().toolCalls.size(), 2);

    auto second = request(QStringLiteral("ai-typed"));
    second.messages = QJsonArray{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"), QStringLiteral("go")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                    {QStringLiteral("tool_call_id"), QStringLiteral("call-1")},
                    {QStringLiteral("content"), QStringLiteral("{}")}}
    };
    provider.submit(second);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return responses.size() == 2; }));
    EXPECT_EQ(responses.last().text, QStringLiteral("done"));
    ASSERT_EQ(provider.submittedRequests().size(), 2);
    EXPECT_EQ(provider.submittedRequests().last().messages, second.messages);
}

TEST(MockLLMProviderTest, ManualCancellationSuppressesDeliveryAndIsRecorded)
{
    TestSupport::ensureApp();
    MockLLMProvider provider;
    provider.setAutoDelivery(false);
    provider.enqueueTextResponse(QStringLiteral("late"));

    int completionCount = 0;
    QObject::connect(&provider, &ILLMProvider::completed, &provider,
                     [&](const ILLMProvider::Response&) { ++completionCount; });

    provider.submit(request(QStringLiteral("ai-0007")));
    provider.cancel(QStringLiteral("ai-0007"));
    EXPECT_FALSE(provider.deliverNext());
    QCoreApplication::processEvents();
    EXPECT_EQ(completionCount, 0);
    EXPECT_EQ(provider.cancelCallCount(), 1);
    EXPECT_EQ(provider.cancelledRequestIds(), QStringList{QStringLiteral("ai-0007")});
}

TEST(MockLLMProviderTest, ManualDeliveryWorksAndRecordedRequestOmitsTheKey)
{
    TestSupport::ensureApp();
    MockLLMProvider provider;
    provider.setAutoDelivery(false);
    provider.enqueueCommandResponse(QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("ping")},
        {QStringLiteral("args"), QJsonObject{}},
        {QStringLiteral("request_id"), QStringLiteral("ai-0002")}
    });

    int completionCount = 0;
    QObject::connect(&provider, &ILLMProvider::completed, &provider,
                     [&](const ILLMProvider::Response&) { ++completionCount; });

    auto submitted = request(QStringLiteral("ai-0002"));
    submitted.apiKey = QStringLiteral("not-retained");
    provider.submit(submitted);
    EXPECT_TRUE(provider.lastRequest().apiKey.isEmpty());
    EXPECT_TRUE(provider.deliverNext());
    EXPECT_EQ(completionCount, 1);
}
