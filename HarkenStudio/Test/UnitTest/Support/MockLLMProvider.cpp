#include "MockLLMProvider.h"

#include <QTimer>

namespace VNX {

MockLLMProvider::MockLLMProvider(QObject* parent)
    : ILLMProvider(parent)
{
}

QString MockLLMProvider::providerId() const
{
    return QStringLiteral("mock");
}

void MockLLMProvider::submit(const Request& request)
{
    ++submitCount_;
    lastRequest_ = request;
    lastRequest_.apiKey.clear();
    submittedRequests_.append(lastRequest_);

    Script script;
    if (scripts_.isEmpty())
    {
        script.kind = ScriptKind::Failure;
        script.code = QStringLiteral("INVALID_RESPONSE");
        script.message = QStringLiteral("No scripted mock response is available.");
    }
    else
    {
        script = scripts_.dequeue();
    }

    pending_ = PendingDelivery{request.requestId, script};
    if (autoDelivery_)
    {
        const QString requestId = request.requestId;
        QTimer::singleShot(0, this, [this, requestId]() {
            deliverPending(requestId);
        });
    }
}

void MockLLMProvider::cancel(const QString& requestId)
{
    cancelledRequestIds_.append(requestId);
    if (pending_ && pending_->requestId == requestId)
        pending_.reset();
}

void MockLLMProvider::enqueueCommandResponse(const QJsonObject& command)
{
    Script script;
    script.kind = ScriptKind::Command;
    script.command = command;
    scripts_.enqueue(script);
}

void MockLLMProvider::enqueueToolCallsResponse(
    const QList<ILLMProvider::ToolCall>& toolCalls, const QString& text)
{
    Script script;
    script.kind = ScriptKind::ToolCalls;
    script.toolCalls = toolCalls;
    script.text = text;
    scripts_.enqueue(script);
}

void MockLLMProvider::enqueueTextResponse(const QString& text)
{
    Script script;
    script.kind = ScriptKind::Text;
    script.text = text;
    scripts_.enqueue(script);
}

void MockLLMProvider::enqueueFailure(const QString& code, const QString& message)
{
    Script script;
    script.kind = ScriptKind::Failure;
    script.code = code;
    script.message = message;
    scripts_.enqueue(script);
}

void MockLLMProvider::setAutoDelivery(bool enabled)
{
    autoDelivery_ = enabled;
}

bool MockLLMProvider::deliverNext()
{
    if (!pending_)
        return false;
    return deliverPending(pending_->requestId);
}

bool MockLLMProvider::deliverPending(const QString& requestId)
{
    if (!pending_ || pending_->requestId != requestId)
        return false;

    const PendingDelivery delivery = *pending_;
    pending_.reset();

    if (delivery.script.kind == ScriptKind::Failure)
    {
        emit failed(delivery.requestId, delivery.script.code, delivery.script.message);
        return true;
    }

    Response response;
    response.requestId = delivery.requestId;
    response.finished = true;
    if (delivery.script.kind == ScriptKind::Command)
    {
        response.hasCommand = true;
        response.structuredCommand = delivery.script.command;
    }
    else if (delivery.script.kind == ScriptKind::ToolCalls)
    {
        response.toolCalls = delivery.script.toolCalls;
        response.text = delivery.script.text;
    }
    else
    {
        response.text = delivery.script.text;
    }
    emit completed(response);
    return true;
}

}
