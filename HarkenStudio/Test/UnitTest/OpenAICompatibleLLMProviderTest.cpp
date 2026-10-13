#include "AI/OpenAICompatibleLLMProvider.h"
#include "AI/AICommandCatalog.h"
#include "Support/AITestSupport.h"
#include "Support/SyntheticHttpServer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <tuple>
#include <QCoreApplication>
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
    request.tools = AICommandCatalog::enabled();
    request.maxTokens = 777;
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

TEST_F(OpenAICompatibleLLMProviderTest, SendsCredentialOnlyInAuthorizationAndCatalogTools)
{
    server.enqueueResponse({200, responseWithMessage(
        QJsonObject{{QStringLiteral("content"), QStringLiteral("done")}})});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl()));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 1; }));

    EXPECT_EQ(server.lastMethod(), QByteArrayLiteral("POST"));
    EXPECT_EQ(server.lastPath(), QByteArrayLiteral("/v1/chat/completions"));
    EXPECT_EQ(server.lastHeader("Authorization"), QByteArrayLiteral("Bearer secret-token"));
    EXPECT_EQ(server.lastHeader("Content-Type"), QByteArrayLiteral("application/json"));
    EXPECT_FALSE(server.lastBody().contains("secret-token"));

    const QJsonObject body = QJsonDocument::fromJson(server.lastBody()).object();
    EXPECT_EQ(body.value(QStringLiteral("model")).toString(), QStringLiteral("test-model"));
    EXPECT_FALSE(body.value(QStringLiteral("stream")).toBool(true));
    EXPECT_EQ(body.value(QStringLiteral("max_tokens")).toInt(), 777);
    const QJsonArray messages = body.value(QStringLiteral("messages")).toArray();
    ASSERT_EQ(messages.size(), 2);
    EXPECT_EQ(messages.first().toObject().value(QStringLiteral("role")).toString(),
              QStringLiteral("system"));
    const QJsonArray tools = body.value(QStringLiteral("tools")).toArray();
    EXPECT_EQ(tools.size(), AICommandCatalog::enabled().size());
    EXPECT_EQ(body.value(QStringLiteral("tool_choice")).toString(), QStringLiteral("auto"));
    EXPECT_FALSE(body.contains(QStringLiteral("strict")));
    EXPECT_FALSE(body.contains(QStringLiteral("parallel_tool_calls")));
    for (const AICommandSpec& spec : AICommandCatalog::enabled())
    {
        const auto match = std::find_if(tools.begin(), tools.end(), [&spec](const QJsonValue& value) {
            return value.toObject().value(QStringLiteral("function")).toObject()
                .value(QStringLiteral("name")).toString() == spec.cmd;
        });
        ASSERT_NE(match, tools.end());
        const QJsonObject function = match->toObject()
            .value(QStringLiteral("function")).toObject();
        const QJsonObject parameters = function.value(QStringLiteral("parameters")).toObject();
        EXPECT_EQ(parameters.value(QStringLiteral("type")).toString(), QStringLiteral("object"));
        for (const QString& forbidden : {QStringLiteral("oneOf"), QStringLiteral("anyOf"),
                                         QStringLiteral("allOf"), QStringLiteral("enum"),
                                         QStringLiteral("const"), QStringLiteral("not")})
            EXPECT_FALSE(parameters.contains(forbidden));
        EXPECT_EQ(function.value(QStringLiteral("description")).toString(), spec.description);
        EXPECT_EQ(parameters.value(QStringLiteral("properties")),
                  spec.argumentSchema.value(QStringLiteral("properties")));
        EXPECT_EQ(parameters.value(QStringLiteral("required")),
                  spec.argumentSchema.value(QStringLiteral("required")));
        EXPECT_EQ(parameters.value(QStringLiteral("additionalProperties")),
                  spec.argumentSchema.value(QStringLiteral("additionalProperties")));
        if (spec.cmd == QStringLiteral("set_item_property"))
        {
            EXPECT_EQ(parameters.value(QStringLiteral("properties")).toObject()
                          .value(QStringLiteral("property")).toObject()
                          .value(QStringLiteral("enum")),
                      spec.argumentSchema.value(QStringLiteral("properties")).toObject()
                          .value(QStringLiteral("property")).toObject()
                          .value(QStringLiteral("enum")));
        }
        EXPECT_EQ(spec.argumentSchema.value(QStringLiteral("anyOf")).isArray(),
                  spec.cmd == QStringLiteral("get_item_info")
                      || spec.cmd == QStringLiteral("set_item_property"));
    }
}

TEST_F(OpenAICompatibleLLMProviderTest, RejectsHttpBeforeOpeningAConnection)
{
    TestSupport::SyntheticHttpServer httpServer;
    ASSERT_TRUE(httpServer.listen());
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(httpServer.baseUrl()));

    ASSERT_EQ(observer.failedCount, 1);
    EXPECT_EQ(observer.failedCode, QStringLiteral("INVALID_ARGUMENTS"));
    QCoreApplication::processEvents();
    EXPECT_EQ(httpServer.connectionCount(), 0);
    EXPECT_EQ(httpServer.requestCount(), 0);
}

TEST_F(OpenAICompatibleLLMProviderTest, PreservesCompletedEndpointPath)
{
    server.enqueueResponse({200, responseWithMessage(QJsonObject{{QStringLiteral("content"), QJsonValue::Null}})});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);
    QUrl endpoint = server.baseUrl();
    endpoint.setPath(QStringLiteral("/chat/completions"));

    provider.submit(requestFor(endpoint));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 1; }));
    EXPECT_EQ(server.lastPath(), QByteArrayLiteral("/chat/completions"));
}

TEST_F(OpenAICompatibleLLMProviderTest, ParsesTypedToolCallAndLegacyCommand)
{
    server.enqueueResponse({200, responseWithMessage(QJsonObject{
        {QStringLiteral("content"), QJsonValue::Null},
        {QStringLiteral("tool_calls"), QJsonArray{
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("call-1")},
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"), QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("ping")},
                    {QStringLiteral("arguments"), QStringLiteral("{\"value\":1}")}
                }}
            }
        }}
    })});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("typed")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 1; }));
    ASSERT_EQ(observer.response.toolCalls.size(), 1);
    EXPECT_EQ(observer.response.toolCalls.first().callId, QStringLiteral("call-1"));
    EXPECT_EQ(observer.response.toolCalls.first().name, QStringLiteral("ping"));
    EXPECT_EQ(observer.response.toolCalls.first().arguments.value(QStringLiteral("value")).toInt(), 1);
    EXPECT_TRUE(observer.response.hasCommand);
    EXPECT_EQ(observer.response.structuredCommand.value(QStringLiteral("request_id")).toString(),
              QStringLiteral("typed"));
}

TEST_F(OpenAICompatibleLLMProviderTest, RejectsInvalidToolCallIdentityAndShape)
{
    auto responseForCall = [](const QJsonObject& call) {
        return responseWithMessage(QJsonObject{
            {QStringLiteral("content"), QJsonValue::Null},
            {QStringLiteral("tool_calls"), QJsonArray{call}}
        });
    };
    const QJsonObject function{
        {QStringLiteral("name"), QStringLiteral("ping")},
        {QStringLiteral("arguments"), QStringLiteral("{}")}
    };
    server.enqueueResponse({200, responseForCall(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), function}
    })});
    server.enqueueResponse({200, responseForCall(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("call-2")},
        {QStringLiteral("type"), QStringLiteral("not-function")},
        {QStringLiteral("function"), function}
    })});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("missing-id")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }));
    provider.submit(requestFor(server.baseUrl(), QStringLiteral("invalid-function")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 2; }));
    EXPECT_EQ(observer.failedCode, QStringLiteral("INVALID_RESPONSE"));
    EXPECT_EQ(observer.completedCount, 0);
}

TEST_F(OpenAICompatibleLLMProviderTest, PassesAssistantAndToolMessagesUnchanged)
{
    server.enqueueResponse({200, responseWithMessage(
        QJsonObject{{QStringLiteral("content"), QStringLiteral("done")}})});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);
    ILLMProvider::Request request = requestFor(server.baseUrl());
    const QJsonObject assistant{
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), QJsonValue::Null},
        {QStringLiteral("tool_calls"), QJsonArray{
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("call-1")},
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"), QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("ping")},
                    {QStringLiteral("arguments"), QStringLiteral("{}")}
                }}
            }
        }}
    };
    const QJsonObject tool{
        {QStringLiteral("role"), QStringLiteral("tool")},
        {QStringLiteral("tool_call_id"), QStringLiteral("call-1")},
        {QStringLiteral("content"), QStringLiteral("{\"status\":\"ok\"}")}
    };
    request.messages = QJsonArray{assistant, tool};

    provider.submit(request);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 1; }));
    const QJsonArray messages = QJsonDocument::fromJson(server.lastBody()).object()
                                    .value(QStringLiteral("messages")).toArray();
    ASSERT_EQ(messages.size(), 3);
    EXPECT_EQ(messages.at(1).toObject(), assistant);
    EXPECT_EQ(messages.at(2).toObject(), tool);
}

TEST_F(OpenAICompatibleLLMProviderTest, ParsesStringAndObjectToolArguments)
{
    auto toolResponse = [](const QJsonValue& arguments) {
        return responseWithMessage(QJsonObject{
            {QStringLiteral("content"), QStringLiteral("ignored")},
            {QStringLiteral("tool_calls"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("call-legacy")},
                    {QStringLiteral("type"), QStringLiteral("function")},
                    {QStringLiteral("function"), QJsonObject{
                        {QStringLiteral("name"), QStringLiteral("ping")},
                        {QStringLiteral("arguments"), arguments}
                    }}
                }
            }}
        });
    };
    server.enqueueResponse({200, toolResponse(QStringLiteral("{\"value\":1}"))});
    server.enqueueResponse({200, toolResponse(QJsonObject{{QStringLiteral("value"), 2}})});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("first")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 1; }));
    EXPECT_TRUE(observer.response.hasCommand);
    EXPECT_EQ(observer.response.structuredCommand.value(QStringLiteral("cmd")).toString(),
              QStringLiteral("ping"));
    EXPECT_EQ(observer.response.structuredCommand.value(QStringLiteral("args")).toObject()
                  .value(QStringLiteral("value")).toInt(), 1);
    EXPECT_EQ(observer.response.structuredCommand.value(QStringLiteral("request_id")).toString(),
              QStringLiteral("first"));

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("second")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 2; }));
    EXPECT_EQ(observer.response.structuredCommand.value(QStringLiteral("args")).toObject()
                  .value(QStringLiteral("value")).toInt(), 2);
}

TEST_F(OpenAICompatibleLLMProviderTest, RejectsMultipleOrMalformedToolCalls)
{
    const QJsonObject firstCall{
        {QStringLiteral("id"), QStringLiteral("call-1")},
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("ping")},
            {QStringLiteral("arguments"), QStringLiteral("{}")}
        }}
    };
    QJsonObject secondCall = firstCall;
    secondCall.insert(QStringLiteral("id"), QStringLiteral("call-2"));
    server.enqueueResponse({200, responseWithMessage(QJsonObject{
        {QStringLiteral("tool_calls"), QJsonArray{firstCall, secondCall}}
    })});
    server.enqueueResponse({200, responseWithMessage(QJsonObject{
        {QStringLiteral("tool_calls"), QJsonArray{
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("call-malformed")},
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"), QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("ping")},
                    {QStringLiteral("arguments"), QStringLiteral("[]")}
                }}
            }
        }}
    })});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("multiple")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 1; }));
    ASSERT_EQ(observer.response.toolCalls.size(), 2);
    EXPECT_EQ(observer.response.toolCalls.at(0).callId, QStringLiteral("call-1"));
    EXPECT_EQ(observer.response.toolCalls.at(1).callId, QStringLiteral("call-2"));
    EXPECT_FALSE(observer.response.hasCommand);
    provider.submit(requestFor(server.baseUrl(), QStringLiteral("malformed")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }));
    EXPECT_EQ(observer.failedCode, QStringLiteral("INVALID_RESPONSE"));
    EXPECT_EQ(observer.completedCount, 1);
}

TEST_F(OpenAICompatibleLLMProviderTest, ParsesEightToolCallsAndRejectsNine)
{
    auto responseWithCallCount = [](int count) {
        QJsonArray calls;
        for (int i = 0; i < count; ++i)
        {
            calls.append(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("call-%1").arg(i + 1)},
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"), QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("ping")},
                    {QStringLiteral("arguments"), QStringLiteral("{}")}
                }}
            });
        }
        return responseWithMessage(QJsonObject{
            {QStringLiteral("content"), QJsonValue::Null},
            {QStringLiteral("tool_calls"), calls}
        });
    };
    server.enqueueResponse({200, responseWithCallCount(8)});
    server.enqueueResponse({200, responseWithCallCount(9)});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("eight")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 1; }));
    ASSERT_EQ(observer.response.toolCalls.size(), 8);
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(observer.response.toolCalls.at(i).callId,
                  QStringLiteral("call-%1").arg(i + 1));
    EXPECT_FALSE(observer.response.hasCommand);

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("nine")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }));
    EXPECT_EQ(observer.failedCode, QStringLiteral("INVALID_RESPONSE"));
    EXPECT_EQ(observer.failedMessage,
              QStringLiteral("The AI provider returned invalid tool calls."));
    EXPECT_EQ(observer.completedCount, 1);
}

TEST_F(OpenAICompatibleLLMProviderTest, ParsesStringNullAndTextParts)
{
    server.enqueueResponse({200, responseWithMessage(
        QJsonObject{{QStringLiteral("content"), QStringLiteral("plain")}})});
    server.enqueueResponse({200, responseWithMessage(
        QJsonObject{{QStringLiteral("content"), QJsonValue::Null}})});
    server.enqueueResponse({200, responseWithMessage(QJsonObject{
        {QStringLiteral("content"), QJsonArray{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("part one ")}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("part two")}}
        }}
    })});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("string")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 1; }));
    EXPECT_EQ(observer.response.text, QStringLiteral("plain"));
    provider.submit(requestFor(server.baseUrl(), QStringLiteral("null")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 2; }));
    EXPECT_TRUE(observer.response.text.isEmpty());
    provider.submit(requestFor(server.baseUrl(), QStringLiteral("parts")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 3; }));
    EXPECT_EQ(observer.response.text, QStringLiteral("part one part two"));
}

TEST_F(OpenAICompatibleLLMProviderTest, RejectsNonTextContentPart)
{
    server.enqueueResponse({200, responseWithMessage(QJsonObject{
        {QStringLiteral("content"), QJsonArray{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")}}
        }}
    })});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl()));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }));
    EXPECT_EQ(observer.failedCode, QStringLiteral("INVALID_RESPONSE"));
}

class OpenAIStatusMappingTest : public OpenAICompatibleLLMProviderTest,
                                public ::testing::WithParamInterface<std::tuple<int, QString>>
{
};

TEST_P(OpenAIStatusMappingTest, MapsProtocolStatus)
{
    const auto [status, expected] = GetParam();
    server.enqueueResponse({status, QByteArrayLiteral("synthetic error")});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl()));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }));
    EXPECT_EQ(observer.failedCode, expected);
    EXPECT_LE(observer.failedMessage.size(), 512);
    EXPECT_FALSE(observer.failedMessage.contains(QStringLiteral("secret-token")));
}

INSTANTIATE_TEST_SUITE_P(StatusMappings, OpenAIStatusMappingTest,
    ::testing::Values(
        std::make_tuple(401, QStringLiteral("AUTH_MISSING")),
        std::make_tuple(403, QStringLiteral("AUTH_MISSING")),
        std::make_tuple(404, QStringLiteral("HTTP_4XX")),
        std::make_tuple(500, QStringLiteral("HTTP_5XX")),
        std::make_tuple(302, QStringLiteral("NETWORK_ERROR"))));

TEST_F(OpenAICompatibleLLMProviderTest, MapsTransportFailureWithoutHttpStatus)
{
    TestSupport::SyntheticHttpServer::Response response;
    response.abortWithoutResponse = true;
    server.enqueueResponse(response);
    server.enqueueResponse(response);
    server.enqueueResponse(response);
    server.enqueueResponse(response);
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl()));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }, 3000));
    EXPECT_EQ(observer.failedCode, QStringLiteral("NETWORK_ERROR"));
}

TEST_F(OpenAICompatibleLLMProviderTest, NetworkFailureMessageCarriesEnumDetailWithoutEndpointData)
{
    QTcpServer probe;
    ASSERT_TRUE(probe.listen(QHostAddress::LocalHost, 0));
    const quint16 closedPort = probe.serverPort();
    probe.close();
    QUrl endpoint;
    endpoint.setScheme(QStringLiteral("https"));
    endpoint.setHost(QStringLiteral("127.0.0.1"));
    endpoint.setPort(closedPort);
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(endpoint));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }, 10000));
    EXPECT_EQ(observer.failedCode, QStringLiteral("NETWORK_ERROR"));
    EXPECT_EQ(observer.failedMessage,
              QStringLiteral("The AI provider request failed (ConnectionRefusedError, 1)."));
    EXPECT_FALSE(observer.failedMessage.contains(QStringLiteral("127.0.0.1")));
    EXPECT_FALSE(observer.failedMessage.contains(QStringLiteral("http")));
    EXPECT_FALSE(observer.failedMessage.contains(QString::number(closedPort)));
    EXPECT_FALSE(observer.failedMessage.contains(QStringLiteral("secret-token")));
    EXPECT_FALSE(observer.failedMessage.contains(QStringLiteral("Hello")));
    EXPECT_EQ(observer.completedCount, 0);
}

TEST_F(OpenAICompatibleLLMProviderTest, TimeoutWinsOverAbortError)
{
    server.enqueueResponse({200, responseWithMessage(
        QJsonObject{{QStringLiteral("content"), QStringLiteral("late")}}), {}, 1500});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);
    auto request = requestFor(server.baseUrl());
    request.timeoutMs = 1000;

    provider.submit(request);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }, 1800));
    EXPECT_EQ(observer.failedCode, QStringLiteral("TIMEOUT"));
    EXPECT_EQ(observer.completedCount, 0);
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

TEST_F(OpenAICompatibleLLMProviderTest, RejectsEmptyOrMalformedChoices)
{
    server.enqueueResponse({200, QJsonDocument(QJsonObject{
        {QStringLiteral("choices"), QJsonArray{}}
    }).toJson(QJsonDocument::Compact)});
    server.enqueueResponse({200, QJsonDocument(QJsonObject{
        {QStringLiteral("choices"), QJsonArray{QStringLiteral("not-an-object")}}
    }).toJson(QJsonDocument::Compact)});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("empty-choices")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }));
    EXPECT_EQ(observer.failedCode, QStringLiteral("INVALID_RESPONSE"));

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("malformed-choice")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 2; }));
    EXPECT_EQ(observer.failedCode, QStringLiteral("INVALID_RESPONSE"));
}

TEST_F(OpenAICompatibleLLMProviderTest, RejectsMalformedMissingAndOversizedResponses)
{
    server.enqueueResponse({200, QByteArrayLiteral("not-json")});
    server.enqueueResponse({200, QByteArrayLiteral("{}")});
    server.enqueueResponse({200, QByteArray(1024 * 1024 + 1, 'x'), {}, 10});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl(), QStringLiteral("malformed")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }));
    EXPECT_EQ(observer.failedCode, QStringLiteral("INVALID_RESPONSE"))
        << observer.failedMessage.toStdString();
    provider.submit(requestFor(server.baseUrl(), QStringLiteral("missing")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 2; }));
    EXPECT_EQ(observer.failedCode, QStringLiteral("INVALID_RESPONSE"))
        << observer.failedMessage.toStdString();
    provider.submit(requestFor(server.baseUrl(), QStringLiteral("oversized")));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 3; }));
    EXPECT_EQ(observer.failedCode, QStringLiteral("INVALID_RESPONSE"))
        << observer.failedMessage.toStdString();
    EXPECT_EQ(observer.failedMessage,
              QStringLiteral("The AI provider response exceeded the size limit."));
}

TEST_F(OpenAICompatibleLLMProviderTest, EmptyToolsOmitToolFields)
{
    server.enqueueResponse({200, responseWithMessage(
        QJsonObject{{QStringLiteral("content"), QStringLiteral("done")}})});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);
    auto request = requestFor(server.baseUrl());
    request.tools.clear();

    provider.submit(request);
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.completedCount == 1; }));

    const QJsonObject body = QJsonDocument::fromJson(server.lastBody()).object();
    EXPECT_FALSE(body.contains(QStringLiteral("tools")));
    EXPECT_FALSE(body.contains(QStringLiteral("tool_choice")));
    EXPECT_EQ(body.value(QStringLiteral("max_tokens")).toInt(), 777);
}

TEST_F(OpenAICompatibleLLMProviderTest, SurfacesStructuredProtocolErrorSafely)
{
    const QByteArray errorBody = QJsonDocument(QJsonObject{
        {QStringLiteral("error"), QJsonObject{
            {QStringLiteral("message"), QStringLiteral("Unsupported parameter")},
            {QStringLiteral("code"), QStringLiteral("unsupported_parameter")},
            {QStringLiteral("type"), QStringLiteral("invalid_request_error")}
        }}
    }).toJson(QJsonDocument::Compact);
    server.enqueueResponse({400, errorBody});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl()));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }));
    EXPECT_EQ(observer.failedCode, QStringLiteral("HTTP_4XX"));
    EXPECT_TRUE(observer.failedMessage.contains(QStringLiteral("HTTP 400")));
    EXPECT_TRUE(observer.failedMessage.contains(QStringLiteral("Unsupported parameter")));
    EXPECT_TRUE(observer.failedMessage.contains(QStringLiteral("unsupported_parameter")));
    EXPECT_TRUE(observer.failedMessage.contains(QStringLiteral("invalid_request_error")));
}

TEST_F(OpenAICompatibleLLMProviderTest, HidesUnstructuredProtocolErrorBody)
{
    server.enqueueResponse({400, QByteArrayLiteral("<html><b>proxy failure</b></html>")});
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);

    provider.submit(requestFor(server.baseUrl()));
    ASSERT_TRUE(TestSupport::spinUntil([&]() { return observer.failedCount == 1; }));
    EXPECT_EQ(observer.failedMessage, QStringLiteral("The AI endpoint returned HTTP 400."));
    EXPECT_FALSE(observer.failedMessage.contains(QLatin1Char('<')));
}

TEST_F(OpenAICompatibleLLMProviderTest, BoundsAndRedactsStructuredProtocolError)
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
    EXPECT_LE(observer.failedMessage.size(), 512);
    EXPECT_TRUE(observer.failedMessage.contains(QStringLiteral("***")));
    EXPECT_FALSE(observer.failedMessage.contains(QStringLiteral("secret-token")));
    EXPECT_FALSE(observer.failedMessage.contains(QChar(1)));
}

TEST_F(OpenAICompatibleLLMProviderTest, EmptyKeyFailsWithoutConnection)
{
    OpenAICompatibleLLMProvider provider;
    ProviderObserver observer(provider);
    auto request = requestFor(server.baseUrl());
    request.apiKey.clear();

    provider.submit(request);
    EXPECT_EQ(observer.failedCount, 1);
    EXPECT_EQ(observer.failedCode, QStringLiteral("AUTH_MISSING"));
    QCoreApplication::processEvents();
    EXPECT_EQ(server.connectionCount(), 0);
}
