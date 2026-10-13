#include "AIResultNormalizer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace VNX {
namespace {

constexpr int kDefaultMaxBytes = 16384;

bool isSensitiveKey(const QString& key)
{
    const QString normalized = key.toLower();
    return normalized.contains(QStringLiteral("api_key"))
        || normalized.contains(QStringLiteral("apikey"))
        || normalized.contains(QStringLiteral("authorization"))
        || normalized.contains(QStringLiteral("credential"))
        || normalized.contains(QStringLiteral("grant"))
        || normalized.contains(QStringLiteral("token"));
}

QString sanitizedText(const QString& text)
{
    if (text.contains(QStringLiteral("api_key"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("authorization"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("bearer "), Qt::CaseInsensitive))
        return QStringLiteral("Sensitive diagnostic content was removed.");
    return text.left(512);
}

QJsonValue sanitized(const QJsonValue& value)
{
    if (value.isObject())
    {
        QJsonObject object;
        const QJsonObject input = value.toObject();
        for (auto it = input.constBegin(); it != input.constEnd(); ++it)
        {
            if (!isSensitiveKey(it.key()))
                object.insert(it.key(), sanitized(it.value()));
        }
        return object;
    }
    if (value.isArray())
    {
        QJsonArray array;
        for (const QJsonValue& item : value.toArray())
            array.append(sanitized(item));
        return array;
    }
    return value;
}

QString categoryFor(const QString& status, const QString& code)
{
    if (status == QLatin1String("ok")) return QStringLiteral("success");
    if (status != QLatin1String("error")) return QStringLiteral("protocol");
    if (code.contains(QStringLiteral("CONFIRM"), Qt::CaseInsensitive)
        || code.contains(QStringLiteral("GRANT"), Qt::CaseInsensitive))
        return QStringLiteral("confirmation");
    if (code.contains(QStringLiteral("POLICY"), Qt::CaseInsensitive)
        || code.startsWith(QStringLiteral("AI_")))
        return QStringLiteral("policy");
    if (code.contains(QStringLiteral("TIMEOUT"), Qt::CaseInsensitive))
        return QStringLiteral("timeout");
    if (code.contains(QStringLiteral("INVALID"), Qt::CaseInsensitive)
        || code.contains(QStringLiteral("RESPONSE"), Qt::CaseInsensitive))
        return QStringLiteral("protocol");
    if (code.contains(QStringLiteral("LIMIT"), Qt::CaseInsensitive))
        return QStringLiteral("limit");
    return QStringLiteral("command");
}

bool retryableFor(const QString& category, const QString& code)
{
    return category == QLatin1String("timeout")
        || code == QLatin1String("NETWORK_ERROR")
        || code == QLatin1String("HTTP_5XX");
}

int updateReturnedBytes(QJsonObject* result)
{
    int previous = -1;
    int current = 0;
    while (current != previous)
    {
        previous = current;
        result->insert(QStringLiteral("returned_bytes"), previous);
        current = QJsonDocument(*result).toJson(QJsonDocument::Compact).size();
    }
    result->insert(QStringLiteral("returned_bytes"), current);
    return QJsonDocument(*result).toJson(QJsonDocument::Compact).size();
}

}

QJsonObject AIResultNormalizer::normalize(const QJsonObject& envelope,
                                          const QString& requestId,
                                          const QString& toolCallId,
                                          int maxBytes)
{
    const int budget = maxBytes > 0 ? maxBytes : kDefaultMaxBytes;
    const QByteArray original = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    const QString sourceStatus = envelope.value(QStringLiteral("status")).toString();
    const QString code = envelope.value(QStringLiteral("code")).toString();
    const QString category = categoryFor(sourceStatus, code);
    const bool success = sourceStatus == QLatin1String("ok");
    QJsonObject result{
        {QStringLiteral("version"), 1},
        {QStringLiteral("status"), success ? QStringLiteral("ok") : QStringLiteral("error")},
        {QStringLiteral("request_id"), requestId.isEmpty()
            ? envelope.value(QStringLiteral("request_id")).toString() : requestId},
        {QStringLiteral("category"), category},
        {QStringLiteral("retryable"), retryableFor(category, code)},
        {QStringLiteral("summary"), success
            ? QStringLiteral("Command completed successfully.")
            : sanitizedText(envelope.value(QStringLiteral("message")).toString())},
        {QStringLiteral("truncated"), false},
        {QStringLiteral("original_bytes"), original.size()},
        {QStringLiteral("returned_bytes"), 0}
    };
    if (!toolCallId.isEmpty()) result.insert(QStringLiteral("tool_call_id"), toolCallId);
    if (envelope.value(QStringLiteral("cmd")).isString())
        result.insert(QStringLiteral("cmd"), envelope.value(QStringLiteral("cmd")));
    if (envelope.contains(QStringLiteral("result")))
        result.insert(QStringLiteral("result"), sanitized(envelope.value(QStringLiteral("result"))));
    if (!code.isEmpty()) result.insert(QStringLiteral("code"), code);
    if (envelope.value(QStringLiteral("message")).isString())
        result.insert(QStringLiteral("message"), sanitizedText(envelope.value(QStringLiteral("message")).toString()));
    if (envelope.value(QStringLiteral("suggestion")).isString())
        result.insert(QStringLiteral("suggestion"), sanitizedText(envelope.value(QStringLiteral("suggestion")).toString()));

    if (updateReturnedBytes(&result) > budget)
    {
        result.remove(QStringLiteral("result"));
        result.insert(QStringLiteral("truncated"), true);
        result.insert(QStringLiteral("summary"), QStringLiteral("Result exceeded the output limit."));
        result.remove(QStringLiteral("message"));
        result.remove(QStringLiteral("suggestion"));
        updateReturnedBytes(&result);
    }
    return result;
}

}
