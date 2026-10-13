#include "OpenAICompatibleLLMProvider.h"
#include "AIEndpointValidation.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaEnum>
#include <QNetworkReply>
#include <QSet>
#include <QNetworkRequest>
#include <QUrl>

namespace VNX {
namespace {

constexpr qsizetype kMaximumResponseBytes = 1024 * 1024;
constexpr int kMaximumErrorCharacters = 512;
constexpr qsizetype kMaximumToolCallsPerResponse = 8;

QString boundedMessage(const QString& message)
{
    return message.left(kMaximumErrorCharacters);
}

QString printableErrorText(QString text)
{
    QString filtered;
    filtered.reserve(text.size());
    for (const QChar character : text)
    {
        if (character == QLatin1Char('\n') || character == QLatin1Char('\t')
            || (!character.isNull()
                && character.category() != QChar::Other_Control))
        {
            filtered.append(character);
        }
    }
    return filtered;
}

QString protocolErrorMessage(int status, const QByteArray& body, const QString& apiKey)
{
    const QString generic = QStringLiteral("The AI endpoint returned HTTP %1.").arg(status);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return generic;
    const QJsonValue errorValue = document.object().value(QStringLiteral("error"));
    if (!errorValue.isObject())
        return generic;
    const QJsonObject error = errorValue.toObject();
    const QJsonValue messageValue = error.value(QStringLiteral("message"));
    if (!messageValue.isString() || messageValue.toString().isEmpty())
        return generic;
    QStringList details{messageValue.toString()};
    for (const QString& key : {QStringLiteral("code"), QStringLiteral("type")})
    {
        const QJsonValue value = error.value(key);
        if (value.isString() && !value.toString().isEmpty())
            details.append(QStringLiteral("%1: %2").arg(key, value.toString()));
    }
    QString result = generic + QLatin1Char(' ') + details.join(QStringLiteral("; "));
    if (!apiKey.isEmpty())
        result.replace(apiKey, QStringLiteral("***"), Qt::CaseSensitive);
    return boundedMessage(printableErrorText(result));
}

QUrl chatCompletionsUrl(QUrl endpoint)
{
    QString path = endpoint.path();
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
        path.chop(1);
    if (!path.endsWith(QStringLiteral("/chat/completions")))
    {
        if (path.isEmpty() || path == QLatin1String("/"))
            path = QStringLiteral("/chat/completions");
        else
            path += QStringLiteral("/chat/completions");
    }
    endpoint.setPath(path);
    return endpoint;
}

QJsonObject providerParameters(const QJsonObject& schema)
{
    QJsonObject projected = schema;
    projected.insert(QStringLiteral("type"), QStringLiteral("object"));
    for (const QString& key : {QStringLiteral("oneOf"), QStringLiteral("anyOf"),
                               QStringLiteral("allOf"), QStringLiteral("enum"),
                               QStringLiteral("const"), QStringLiteral("not")})
        projected.remove(key);
    return projected;
}

QJsonArray toolsPayload(const QList<AICommandSpec>& specs)
{
    QJsonArray tools;
    for (const AICommandSpec& spec : specs)
    {
        tools.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{
                {QStringLiteral("name"), spec.cmd},
                {QStringLiteral("description"), spec.description},
                {QStringLiteral("parameters"), providerParameters(spec.argumentSchema)}
            }}
        });
    }
    return tools;
}

bool parseTextContent(const QJsonValue& content, QString* text)
{
    if (content.isNull()) { text->clear(); return true; }
    if (content.isString()) { *text = content.toString(); return true; }
    if (!content.isArray()) return false;
    QString combined;
    for (const QJsonValue& partValue : content.toArray())
    {
        if (!partValue.isObject()) return false;
        const QJsonObject part = partValue.toObject();
        if (part.value(QStringLiteral("type")).toString() != QLatin1String("text")
            || !part.value(QStringLiteral("text")).isString()) return false;
        combined += part.value(QStringLiteral("text")).toString();
    }
    *text = combined;
    return true;
}

bool parseArguments(const QJsonValue& value, QJsonObject* arguments)
{
    if (value.isObject()) { *arguments = value.toObject(); return true; }
    if (!value.isString()) return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(value.toString().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    *arguments = document.object();
    return true;
}

QString protocolErrorCode(int status)
{
    if (status == 401 || status == 403) return QStringLiteral("AUTH_MISSING");
    if (status >= 400 && status < 500) return QStringLiteral("HTTP_4XX");
    if (status >= 500 && status < 600) return QStringLiteral("HTTP_5XX");
    return QStringLiteral("NETWORK_ERROR");
}

}

OpenAICompatibleLLMProvider::OpenAICompatibleLLMProvider(QObject* parent) : ILLMProvider(parent)
{
    timeoutTimer_.setSingleShot(true);
    connect(&timeoutTimer_, &QTimer::timeout, this, [this]() {
        if (!activeReply_) return;
        timedOut_ = true;
        activeReply_->abort();
    });
}

OpenAICompatibleLLMProvider::~OpenAICompatibleLLMProvider()
{
    if (activeReply_)
    {
        activeReply_->disconnect(this);
        activeReply_->abort();
        activeReply_->deleteLater();
    }
}

QString OpenAICompatibleLLMProvider::providerId() const { return QStringLiteral("openai_compatible"); }

void OpenAICompatibleLLMProvider::submit(const Request& request)
{
    abandonActiveReply();
    if (request.apiKey.isEmpty()) { fail(request.requestId, QStringLiteral("AUTH_MISSING"), QStringLiteral("The configured LLM provider requires an API key.")); return; }
    if (request.requestId.isEmpty() || !isValidAIEndpoint(request.endpoint.toString(), request.model) || request.timeoutMs <= 0 || request.maxTokens <= 0)
    { fail(request.requestId, QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("The AI provider request was invalid.")); return; }
    QJsonArray messages;
    if (!request.systemPrompt.isEmpty()) messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), request.systemPrompt}});
    for (const QJsonValue& message : request.messages) messages.append(message);
    QJsonObject payload{{QStringLiteral("model"), request.model}, {QStringLiteral("messages"), messages}, {QStringLiteral("stream"), false}, {QStringLiteral("max_tokens"), request.maxTokens}};
    const QJsonArray tools = toolsPayload(request.tools);
    if (!tools.isEmpty()) { payload.insert(QStringLiteral("tools"), tools); payload.insert(QStringLiteral("tool_choice"), QStringLiteral("auto")); }
    QNetworkRequest networkRequest(chatCompletionsUrl(request.endpoint));
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    networkRequest.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + request.apiKey.toUtf8());
    QNetworkReply* reply = networkManager_.post(networkRequest, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    activeReply_ = reply; activeRequestId_ = request.requestId; timedOut_ = false; responseOversized_ = false;
    const QString requestId = request.requestId; const QString apiKey = request.apiKey;
    connect(reply, &QIODevice::readyRead, this, [this, reply]() {
        if (reply->bytesAvailable() > kMaximumResponseBytes) { responseOversized_ = true; reply->abort(); }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId, apiKey]() { handleFinished(reply, requestId, apiKey); });
    timeoutTimer_.start(request.timeoutMs);
}

void OpenAICompatibleLLMProvider::cancel(const QString& requestId)
{
    if (!activeReply_ || requestId != activeRequestId_) return;
    activeReply_->disconnect(this); activeReply_->abort(); activeReply_->deleteLater(); activeReply_ = nullptr; activeRequestId_.clear(); timeoutTimer_.stop();
}

void OpenAICompatibleLLMProvider::abandonActiveReply()
{
    if (!activeReply_) return;
    activeReply_->disconnect(this); activeReply_->abort(); activeReply_->deleteLater(); activeReply_ = nullptr; activeRequestId_.clear(); timeoutTimer_.stop();
}

void OpenAICompatibleLLMProvider::handleFinished(QNetworkReply* reply, const QString& requestId, const QString& apiKey)
{
    if (reply != activeReply_ || requestId != activeRequestId_) { reply->deleteLater(); return; }
    timeoutTimer_.stop(); activeReply_ = nullptr; activeRequestId_.clear();
    const QByteArray body = reply->readAll(); const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const bool timedOut = timedOut_; const bool oversized = responseOversized_; reply->deleteLater();
    if (oversized) { fail(requestId, QStringLiteral("INVALID_RESPONSE"), QStringLiteral("The AI provider response exceeded the size limit.")); return; }
    if (timedOut) { fail(requestId, QStringLiteral("TIMEOUT"), QStringLiteral("The AI provider request timed out.")); return; }
    if (status > 0 && (status < 200 || status >= 300)) { fail(requestId, protocolErrorCode(status), protocolErrorMessage(status, body, apiKey)); return; }
    if (networkError != QNetworkReply::NoError)
    {
        const char* errorKey = QMetaEnum::fromType<QNetworkReply::NetworkError>().valueToKey(networkError);
        const QString errorDetail = errorKey
            ? QStringLiteral("%1, %2").arg(QLatin1String(errorKey)).arg(static_cast<int>(networkError))
            : QString::number(static_cast<int>(networkError));
        fail(requestId, QStringLiteral("NETWORK_ERROR"),
             boundedMessage(QStringLiteral("The AI provider request failed (%1).").arg(errorDetail)));
        return;
    }
    QJsonParseError parseError; const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) { fail(requestId, QStringLiteral("INVALID_RESPONSE"), QStringLiteral("The AI provider returned an invalid response.")); return; }
    const QJsonObject root = document.object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty() || !choices.first().isObject())
    {
        fail(requestId, QStringLiteral("INVALID_RESPONSE"),
             QStringLiteral("The AI provider returned an invalid response."));
        return;
    }
    const QJsonObject choice = choices.first().toObject();
    const QJsonValue messageValue = choice.value(QStringLiteral("message"));
    if (!messageValue.isObject())
    {
        fail(requestId, QStringLiteral("INVALID_RESPONSE"),
             QStringLiteral("The AI provider returned an invalid response."));
        return;
    }
    const QJsonObject message = messageValue.toObject();
    QString text;
    if (message.contains(QStringLiteral("content"))
        && !parseTextContent(message.value(QStringLiteral("content")), &text))
    {
        fail(requestId, QStringLiteral("INVALID_RESPONSE"),
             QStringLiteral("The AI provider returned unsupported message content."));
        return;
    }
    ILLMProvider::Response response;
    response.requestId = requestId;
    response.text = text;
    const QJsonValue callsValue = message.value(QStringLiteral("tool_calls"));
    if (!callsValue.isUndefined())
    {
        if (!callsValue.isArray()
            || callsValue.toArray().size() > kMaximumToolCallsPerResponse)
        {
            fail(requestId, QStringLiteral("INVALID_RESPONSE"),
                 QStringLiteral("The AI provider returned invalid tool calls."));
            return;
        }
        const QJsonArray calls = callsValue.toArray();
        QSet<QString> callIds;
        for (const QJsonValue& callValue : calls)
        {
            if (!callValue.isObject())
            {
                fail(requestId, QStringLiteral("INVALID_RESPONSE"),
                     QStringLiteral("The AI provider returned a malformed tool call."));
                return;
            }
            const QJsonObject call = callValue.toObject();
            const QJsonValue idValue = call.value(QStringLiteral("id"));
            const QJsonValue typeValue = call.value(QStringLiteral("type"));
            const QJsonValue functionValue = call.value(QStringLiteral("function"));
            if (!idValue.isString() || idValue.toString().isEmpty()
                || callIds.contains(idValue.toString())
                || typeValue.toString() != QLatin1String("function")
                || !functionValue.isObject())
            {
                fail(requestId, QStringLiteral("INVALID_RESPONSE"),
                     QStringLiteral("The AI provider returned a malformed tool call."));
                return;
            }
            const QJsonObject function = functionValue.toObject();
            const QJsonValue nameValue = function.value(QStringLiteral("name"));
            QJsonObject arguments;
            if (!nameValue.isString() || nameValue.toString().isEmpty()
                || !parseArguments(function.value(QStringLiteral("arguments")), &arguments))
            {
                fail(requestId, QStringLiteral("INVALID_RESPONSE"),
                     QStringLiteral("The AI provider returned a malformed tool call."));
                return;
            }
            callIds.insert(idValue.toString());
            response.toolCalls.append({idValue.toString(), nameValue.toString(), arguments});
        }
        if (response.toolCalls.size() == 1)
        {
            const ILLMProvider::ToolCall& call = response.toolCalls.first();
            response.hasCommand = true;
            response.structuredCommand = QJsonObject{
                {QStringLiteral("cmd"), call.name},
                {QStringLiteral("args"), call.arguments},
                {QStringLiteral("request_id"), requestId}
            };
        }
    }
    emit completed(response);
}

void OpenAICompatibleLLMProvider::fail(const QString& requestId, const QString& code, const QString& message)
{
    emit failed(requestId, code, message);
}

}
