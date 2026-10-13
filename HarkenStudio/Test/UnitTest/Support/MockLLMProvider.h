#pragma once

#include "AI/ILLMProvider.h"

#include <QQueue>
#include <QStringList>
#include <optional>

namespace VNX {

class MockLLMProvider : public ILLMProvider
{
    Q_OBJECT

public:
    explicit MockLLMProvider(QObject* parent = nullptr);

    QString providerId() const override;
    bool requiresApiKey() const override { return false; }
    void submit(const Request& request) override;
    void cancel(const QString& requestId) override;

    void enqueueCommandResponse(const QJsonObject& command);
    void enqueueToolCallsResponse(const QList<ILLMProvider::ToolCall>& toolCalls,
                                  const QString& text = {});
    void enqueueTextResponse(const QString& text);
    void enqueueFailure(const QString& code, const QString& message);
    void setAutoDelivery(bool enabled);
    bool deliverNext();

    int submitCount() const { return submitCount_; }
    int cancelCallCount() const { return cancelledRequestIds_.size(); }
    QStringList cancelledRequestIds() const { return cancelledRequestIds_; }
    QList<Request> submittedRequests() const { return submittedRequests_; }
    Request lastRequest() const { return lastRequest_; }

private:
    enum class ScriptKind { Command, ToolCalls, Text, Failure };

    struct Script
    {
        ScriptKind kind = ScriptKind::Text;
        QJsonObject command;
        QList<ILLMProvider::ToolCall> toolCalls;
        QString text;
        QString code;
        QString message;
    };

    struct PendingDelivery
    {
        QString requestId;
        Script script;
    };

    bool deliverPending(const QString& requestId);

    QQueue<Script> scripts_;
    std::optional<PendingDelivery> pending_;
    bool autoDelivery_ = true;
    int submitCount_ = 0;
    QStringList cancelledRequestIds_;
    QList<Request> submittedRequests_;
    Request lastRequest_;
};

}
