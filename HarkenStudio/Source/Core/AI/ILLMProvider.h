#pragma once

#include "AICommandCatalog.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

namespace VNX {

class ILLMProvider : public QObject
{
    Q_OBJECT

public:
    struct Request
    {
        QString requestId;
        QString model;
        QString systemPrompt;
        QJsonArray messages;
        QString apiKey;
        QUrl endpoint;
        QList<AICommandSpec> tools;
        int maxTokens = 1024;
        int timeoutMs = 30000;
    };

    struct ToolCall
    {
        QString callId;
        QString name;
        QJsonObject arguments;
    };

    struct Response
    {
        QString requestId;
        QString text;
        QList<ToolCall> toolCalls;
        QJsonObject structuredCommand;
        bool hasCommand = false;
        bool finished = false;
        QString errorCode;
        QString errorMessage;
    };

    explicit ILLMProvider(QObject* parent = nullptr) : QObject(parent) {}
    ~ILLMProvider() override = default;

    virtual QString providerId() const = 0;
    virtual bool requiresApiKey() const { return true; }
    virtual void submit(const Request& request) = 0;
    virtual void cancel(const QString& requestId) = 0;

signals:
    void delta(const QString& requestId, const QString& text);
    void completed(const VNX::ILLMProvider::Response& response);
    void failed(const QString& requestId, const QString& code, const QString& message);
};

}

Q_DECLARE_METATYPE(VNX::ILLMProvider::Request)
Q_DECLARE_METATYPE(VNX::ILLMProvider::Response)
