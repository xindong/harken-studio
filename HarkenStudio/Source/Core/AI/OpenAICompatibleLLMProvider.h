#pragma once

#include "ILLMProvider.h"

#include <QNetworkAccessManager>
#include <QTimer>

class QNetworkReply;

namespace VNX {

class OpenAICompatibleLLMProvider : public ILLMProvider
{
    Q_OBJECT

public:
    explicit OpenAICompatibleLLMProvider(QObject* parent = nullptr);
    ~OpenAICompatibleLLMProvider() override;
    QString providerId() const override;
    void submit(const Request& request) override;
    void cancel(const QString& requestId) override;

private:
    void abandonActiveReply();
    void handleFinished(QNetworkReply* reply, const QString& requestId,
                        const QString& apiKey);
    void fail(const QString& requestId, const QString& code, const QString& message);

    QNetworkAccessManager networkManager_;
    QNetworkReply* activeReply_ = nullptr;
    QString activeRequestId_;
    QTimer timeoutTimer_;
    bool timedOut_ = false;
    bool responseOversized_ = false;
};

}
