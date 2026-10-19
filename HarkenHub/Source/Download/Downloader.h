#pragma once

#include "Download/BackoffPolicy.h"
#include "Download/DownloadLease.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QList>

class QNetworkReply;

namespace VNH {

struct DownloadRequest
{
    QUrl url;
    QList<QUrl> fallbackUrls;
    QString targetPath;
    qint64 expectedSizeBytes = -1;
    int maxAttempts = 5;
    int transferTimeoutMs = 30000;
    std::shared_ptr<DownloadLease> lease;
};

struct DownloadResult
{
    bool ok = false;
    QString source;
    QString error;
    QString filePath;
    int attemptsUsed = 0;
    int httpStatus = 0;
    std::shared_ptr<DownloadLease> lease;
};

class Downloader : public QObject
{
    Q_OBJECT

public:
    explicit Downloader(QObject* parent = nullptr);

    void setBackoffPolicy(const BackoffPolicy& policy);
    bool isActive() const;

    bool start(const DownloadRequest& request, QString* error = nullptr);
    void cancel();

    static QString partPath(const QString& targetPath);
    static QString metaPath(const QString& targetPath);

signals:
    void progress(qint64 receivedBytes, qint64 totalBytes);
    void finished(const VNH::DownloadResult& result);

private:
    void beginAttempt();
    void startHead();
    void handleHeadFinished();
    void startGet(bool resume);
    bool followRedirect(const QUrl& redirect, bool headRequest);
    void handleResponseHeader();
    void handleReadyRead();
    void handleReplyFinished();
    void finishSuccess();
    void failPermanently(const QString& error);
    void retryOrFail(const QString& error);
    bool switchToFallback(const QString& error);
    QString currentSourceName() const;
    QUrl currentUrl() const;
    void discardPartialFiles();
    bool readMeta(QByteArray* lastModified, qint64* totalSize) const;
    void writeMeta(const QByteArray& lastModified, qint64 totalSize) const;

    QNetworkAccessManager manager_;
    BackoffPolicy backoff_;
    DownloadRequest request_;
    QNetworkReply* reply_ = nullptr;
    QFile file_;
    QTimer retryTimer_;
    qint64 resumeOffset_ = 0;
    QByteArray metaLastModified_;
    qint64 metaTotalSize_ = -1;
    int attemptsStarted_ = 0;
    int httpStatus_ = 0;
    int resultHttpStatus_ = 0;
    int sourceIndex_ = 0;
    int redirectCount_ = 0;
    QUrl requestUrl_;
    bool active_ = false;
    bool cancelRequested_ = false;
    bool restartRequested_ = false;
    bool headerHandled_ = false;
    bool responseUsable_ = false;
    bool writeFailed_ = false;
};

}
