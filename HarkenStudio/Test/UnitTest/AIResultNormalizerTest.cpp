#include "AI/AIResultNormalizer.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>

using namespace VNX;

TEST(AIResultNormalizerTest, PreservesSuccessAndCorrelationWithoutInventedFields)
{
    const QJsonObject normalized = AIResultNormalizer::normalize(
        QJsonObject{
            {QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("result"), QJsonObject{
                {QStringLiteral("items"), QJsonArray{1, 2, 3}}
            }}
        }, QStringLiteral("request-1"), QStringLiteral("call-1"));

    EXPECT_EQ(normalized.value(QStringLiteral("version")).toInt(), 1);
    EXPECT_EQ(normalized.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(normalized.value(QStringLiteral("category")).toString(), QStringLiteral("success"));
    EXPECT_EQ(normalized.value(QStringLiteral("request_id")).toString(), QStringLiteral("request-1"));
    EXPECT_EQ(normalized.value(QStringLiteral("tool_call_id")).toString(), QStringLiteral("call-1"));
    EXPECT_EQ(normalized.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("items")).toArray().size(), 3);
    EXPECT_FALSE(normalized.contains(QStringLiteral("undo")));
    EXPECT_FALSE(normalized.contains(QStringLiteral("cursor")));
}

TEST(AIResultNormalizerTest, PreservesBoundedErrorAndClassifiesConservatively)
{
    const QJsonObject normalized = AIResultNormalizer::normalize(QJsonObject{
        {QStringLiteral("status"), QStringLiteral("error")},
        {QStringLiteral("code"), QStringLiteral("HTTP_5XX")},
        {QStringLiteral("message"), QStringLiteral("Unavailable")},
        {QStringLiteral("suggestion"), QStringLiteral("Retry later")}
    });

    EXPECT_EQ(normalized.value(QStringLiteral("category")).toString(), QStringLiteral("command"));
    EXPECT_TRUE(normalized.value(QStringLiteral("retryable")).toBool());
    EXPECT_EQ(normalized.value(QStringLiteral("code")).toString(), QStringLiteral("HTTP_5XX"));
    EXPECT_EQ(normalized.value(QStringLiteral("message")).toString(), QStringLiteral("Unavailable"));
    EXPECT_EQ(normalized.value(QStringLiteral("suggestion")).toString(), QStringLiteral("Retry later"));
}

TEST(AIResultNormalizerTest, MalformedEnvelopeBecomesProtocolError)
{
    const QJsonObject normalized = AIResultNormalizer::normalize(
        QJsonObject{{QStringLiteral("unexpected"), true}});
    EXPECT_EQ(normalized.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(normalized.value(QStringLiteral("category")).toString(), QStringLiteral("protocol"));
    EXPECT_FALSE(normalized.value(QStringLiteral("retryable")).toBool());
}

TEST(AIResultNormalizerTest, OutputIsBoundedUtf8AndSensitiveFieldsAreRemoved)
{
    const QString large = QStringLiteral("中文😀").repeated(10000);
    const QJsonObject normalized = AIResultNormalizer::normalize(QJsonObject{
        {QStringLiteral("status"), QStringLiteral("ok")},
        {QStringLiteral("result"), QJsonObject{
            {QStringLiteral("text"), large},
            {QStringLiteral("api_key"), QStringLiteral("secret")},
            {QStringLiteral("nested"), QJsonObject{
                {QStringLiteral("authorization"), QStringLiteral("Bearer secret")},
                {QStringLiteral("safe"), QStringLiteral("value")}
            }}
        }}
    });
    const QByteArray bytes = QJsonDocument(normalized).toJson(QJsonDocument::Compact);

    EXPECT_LE(bytes.size(), 16384);
    EXPECT_TRUE(normalized.value(QStringLiteral("truncated")).toBool());
    EXPECT_FALSE(bytes.contains("secret"));
    EXPECT_FALSE(bytes.contains("api_key"));
    EXPECT_FALSE(bytes.contains("authorization"));
    EXPECT_GT(normalized.value(QStringLiteral("original_bytes")).toInt(),
              normalized.value(QStringLiteral("returned_bytes")).toInt());
    QJsonParseError error;
    EXPECT_TRUE(QJsonDocument::fromJson(bytes, &error).isObject());
    EXPECT_EQ(error.error, QJsonParseError::NoError);
}
