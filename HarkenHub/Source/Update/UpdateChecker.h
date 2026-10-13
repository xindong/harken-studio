#pragma once

#include "Download/BackoffPolicy.h"
#include "Security/ManifestVerifier.h"

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QHash>
#include <QList>

class QNetworkReply;

namespace VNH {

struct UpdateCheckResult
{
    bool ok = false;
    bool notModified = false;
    bool manifestAbsent = false;
    QString source;
    QString error;
    ReleaseManifest manifest;
    QByteArray envelope;
    QString objectPath;
    QString releaseTag;
};

class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 kDefaultIntervalMs = 60ll * 60 * 1000;

    struct Options
    {
        QUrl latestUrl;
        QList<QUrl> fallbackUrls;
        QUrl trustedHttpOrigin;
        QString product;
        QString channel;
        QByteArray manifestPublicKey;
        qint64 intervalMs = kDefaultIntervalMs;
        int transferTimeoutMs = 15000;
    };

    static constexpr qint64 kMinIntervalMs = 15ll * 60 * 1000;

    explicit UpdateChecker(QObject* parent = nullptr);

    void setBackoffPolicy(const BackoffPolicy& policy);
    bool start(const Options& options);
    void stop();
    bool checkNow();
    void invalidateCache();
    bool isChecking() const;
    qint64 effectiveIntervalMs() const;

signals:
    void checkFinished(const VNH::UpdateCheckResult& result);

private:
    void beginRound();
    void beginRequest();
    void handleReplyFinished();
    bool tryFallback(int status, int networkError, const QString& error);
    void finishRound(UpdateCheckResult result, bool failed);
    void scheduleNext(bool failed);

    QNetworkAccessManager manager_;
    BackoffPolicy backoff_;
    Options options_;
    QTimer nextTimer_;
    QNetworkReply* reply_ = nullptr;
    QHash<QString, QByteArray> etags_;
    QList<QUrl> roundUrls_;
    QUrl requestUrl_;
    int sourceIndex_ = 0;
    int redirectCount_ = 0;
    int failureStreak_ = 0;
    bool started_ = false;
    bool forcedRoundPending_ = false;
    bool stopping_ = false;
    bool sentConditional_ = false;
    bool roundOversized_ = false;
};

}
