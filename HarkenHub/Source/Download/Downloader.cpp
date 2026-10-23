#include "Download/Downloader.h"

#include "Update/ServerEndpoints.h"

#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStorageInfo>
#include <QTimer>

#include <algorithm>

namespace VNH {

namespace {

constexpr qint64 kDiskReserveBytes = 64ll * 1024 * 1024;

qint64 contentRangeStart(const QByteArray& header)
{
    if (!header.startsWith("bytes "))
        return -1;
    const QByteArray range = header.mid(6).trimmed();
    const qsizetype dash = range.indexOf('-');
    if (dash <= 0)
        return -1;
    bool parsed = false;
    const qint64 start = range.left(dash).toLongLong(&parsed);
    return parsed ? start : -1;
}

}

Downloader::Downloader(QObject* parent)
    : QObject(parent)
{
    retryTimer_.setSingleShot(true);
    connect(&retryTimer_, &QTimer::timeout, this, &Downloader::beginAttempt);
}

void Downloader::setBackoffPolicy(const BackoffPolicy& policy)
{
    backoff_ = policy;
}

bool Downloader::isActive() const
{
    return active_;
}

QString Downloader::partPath(const QString& targetPath)
{
    return targetPath + QStringLiteral(".part");
}

QString Downloader::metaPath(const QString& targetPath)
{
    return targetPath + QStringLiteral(".part.meta");
}

bool Downloader::start(const DownloadRequest& request, QString* error)
{
    if (error)
        error->clear();
    if (active_)
        return false;
    if (!request.url.isValid() || request.url.isEmpty() ||
        request.targetPath.isEmpty() || request.maxAttempts < 1)
        return false;
    for (const QUrl& fallback : request.fallbackUrls) {
        if (!fallback.isValid() || fallback.isEmpty())
            return false;
    }

    auto lease = request.lease;
    if (lease && !lease->covers(request.targetPath))
        return false;
    if (!lease)
        lease = DownloadLease::acquire(request.targetPath, error);
    if (!lease)
        return false;
    request_ = request;
    request_.lease = std::move(lease);
    attemptsStarted_ = 0;
    sourceIndex_ = 0;
    resultHttpStatus_ = 0;
    cancelRequested_ = false;
    active_ = true;

    const QDir targetDir = QFileInfo(request_.targetPath).dir();
    if (!targetDir.mkpath(QStringLiteral("."))) {
        QTimer::singleShot(0, this, [this]() {
            failPermanently(QStringLiteral("Cannot create the download directory"));
        });
        return true;
    }

    const QStorageInfo storage(targetDir.path());
    const qint64 partBytes = QFileInfo(partPath(request_.targetPath)).size();
    const qint64 remaining =
        request_.expectedSizeBytes >= 0
            ? std::max<qint64>(request_.expectedSizeBytes - partBytes, 0)
            : 0;
    if (storage.isValid() &&
        storage.bytesAvailable() < remaining + kDiskReserveBytes) {
        QTimer::singleShot(0, this, [this]() {
            failPermanently(QStringLiteral("Insufficient disk space for the download"));
        });
        return true;
    }

    beginAttempt();
    return true;
}

void Downloader::cancel()
{
    if (!active_)
        return;
    cancelRequested_ = true;
    if (reply_) {
        reply_->abort();
        return;
    }
    if (retryTimer_.isActive()) {
        retryTimer_.stop();
        failPermanently(QStringLiteral("Download canceled"));
    }
}

void Downloader::beginAttempt()
{
    if (cancelRequested_) {
        failPermanently(QStringLiteral("Download canceled"));
        return;
    }

    ++attemptsStarted_;
    headerHandled_ = false;
    responseUsable_ = false;
    restartRequested_ = false;
    writeFailed_ = false;
    httpStatus_ = 0;
    redirectCount_ = 0;
    requestUrl_ = currentUrl();
    qInfo().noquote() << QStringLiteral("Update package request source:")
                      << currentSourceName();

    const qint64 partBytes =
        QFileInfo(partPath(request_.targetPath)).size();
    QByteArray lastModified;
    qint64 totalSize = -1;
    if (readMeta(&lastModified, &totalSize) && partBytes > 0 &&
        partBytes < totalSize) {
        metaLastModified_ = lastModified;
        metaTotalSize_ = totalSize;
        startHead();
        return;
    }

    discardPartialFiles();
    startGet(false);
}

void Downloader::startHead()
{
    QNetworkRequest netRequest(requestUrl_);
    netRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                            QNetworkRequest::ManualRedirectPolicy);
    netRequest.setTransferTimeout(std::max(request_.transferTimeoutMs, 1));
    reply_ = manager_.head(netRequest);
    connect(reply_, &QNetworkReply::finished, this,
            &Downloader::handleHeadFinished);
}

void Downloader::handleHeadFinished()
{
    const auto networkError = reply_->error();
    const QString networkErrorText = reply_->errorString();
    const int status =
        reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QUrl redirect = reply_->attribute(
        QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const QByteArray lastModified =
        reply_->rawHeader(QByteArrayLiteral("Last-Modified"));
    bool sizeParsed = false;
    const qint64 totalSize =
        reply_->rawHeader(QByteArrayLiteral("Content-Length"))
            .toLongLong(&sizeParsed);
    reply_->deleteLater();
    reply_ = nullptr;
    if (status > 0)
        resultHttpStatus_ = status;

    if (cancelRequested_) {
        failPermanently(QStringLiteral("Download canceled"));
        return;
    }

    if (status >= 300 && status < 400) {
        if (followRedirect(redirect, true))
            return;
        failPermanently(QStringLiteral("Download redirect is not approved"));
        return;
    }

    if (networkError != QNetworkReply::NoError || status != 200) {
        if (status >= 400) {
            const QString error = QStringLiteral("HTTP status %1").arg(status);
            if ((status == 403 || status == 404 || status == 408 ||
                 status == 429 || status >= 500) &&
                switchToFallback(error))
                return;
            if (status >= 500 || status == 408 || status == 429)
                retryOrFail(error);
            else
                failPermanently(error);
            return;
        }
        if (!switchToFallback(networkErrorText))
            retryOrFail(networkErrorText);
        return;
    }

    if (lastModified.isEmpty() || lastModified != metaLastModified_ ||
        !sizeParsed || totalSize != metaTotalSize_) {
        discardPartialFiles();
        startGet(false);
        return;
    }

    startGet(true);
}

void Downloader::startGet(bool resume)
{
    headerHandled_ = false;
    responseUsable_ = false;
    restartRequested_ = false;
    httpStatus_ = 0;
    const QString part = partPath(request_.targetPath);
    file_.setFileName(part);
    const auto mode = resume ? (QIODevice::WriteOnly | QIODevice::Append)
                             : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!file_.open(mode)) {
        failPermanently(QStringLiteral("Cannot open %1 for writing").arg(part));
        return;
    }
    resumeOffset_ = resume ? file_.size() : 0;

    QNetworkRequest netRequest(requestUrl_);
    netRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                            QNetworkRequest::ManualRedirectPolicy);
    netRequest.setTransferTimeout(std::max(request_.transferTimeoutMs, 1));
    if (resume)
        netRequest.setRawHeader(QByteArrayLiteral("Range"),
                                QByteArrayLiteral("bytes=") +
                                    QByteArray::number(resumeOffset_) +
                                    QByteArrayLiteral("-"));

    reply_ = manager_.get(netRequest);
    connect(reply_, &QIODevice::readyRead, this, &Downloader::handleReadyRead);
    connect(reply_, &QNetworkReply::finished, this, &Downloader::handleReplyFinished);
    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                emit progress(resumeOffset_ + received,
                              total >= 0 ? resumeOffset_ + total : total);
            });
}

void Downloader::handleResponseHeader()
{
    headerHandled_ = true;
    httpStatus_ =
        reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (httpStatus_ == 206) {
        const qint64 start =
            contentRangeStart(reply_->rawHeader(QByteArrayLiteral("Content-Range")));
        if (start != resumeOffset_) {
            restartRequested_ = true;
            reply_->abort();
            return;
        }
        const QByteArray lastModified =
            reply_->rawHeader(QByteArrayLiteral("Last-Modified"));
        if (!lastModified.isEmpty() && lastModified != metaLastModified_) {
            restartRequested_ = true;
            reply_->abort();
            return;
        }
        responseUsable_ = true;
        return;
    }

    if (httpStatus_ == 200) {
        file_.close();
        if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            restartRequested_ = true;
            reply_->abort();
            return;
        }
        resumeOffset_ = 0;
        const QByteArray lastModified =
            reply_->rawHeader(QByteArrayLiteral("Last-Modified"));
        bool sizeParsed = false;
        const qint64 totalSize =
            reply_->rawHeader(QByteArrayLiteral("Content-Length"))
                .toLongLong(&sizeParsed);
        if (!lastModified.isEmpty() && sizeParsed && totalSize > 0)
            writeMeta(lastModified, totalSize);
        else
            QFile::remove(metaPath(request_.targetPath));
        responseUsable_ = true;
    }
}

void Downloader::handleReadyRead()
{
    if (!headerHandled_)
        handleResponseHeader();
    if (!reply_)
        return;
    const QByteArray chunk = reply_->readAll();
    if (responseUsable_ && !writeFailed_ && file_.write(chunk) != chunk.size()) {
        writeFailed_ = true;
        reply_->abort();
    }
}

void Downloader::handleReplyFinished()
{
    if (!headerHandled_ && !restartRequested_)
        handleResponseHeader();
    if (responseUsable_ && !writeFailed_ && reply_->bytesAvailable() > 0) {
        const QByteArray chunk = reply_->readAll();
        if (file_.write(chunk) != chunk.size())
            writeFailed_ = true;
    }

    const auto networkError = reply_->error();
    const QString networkErrorText = reply_->errorString();
    const int status = httpStatus_;
    const QUrl redirect = reply_->attribute(
        QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (status > 0)
        resultHttpStatus_ = status;

    if (responseUsable_ && !file_.flush())
        writeFailed_ = true;
    file_.close();
    reply_->deleteLater();
    reply_ = nullptr;

    if (writeFailed_) {
        discardPartialFiles();
        failPermanently(QStringLiteral("Failed writing the download to disk"));
        return;
    }

    if (cancelRequested_) {
        failPermanently(QStringLiteral("Download canceled"));
        return;
    }

    if (status >= 300 && status < 400) {
        if (followRedirect(redirect, false))
            return;
        failPermanently(QStringLiteral("Download redirect is not approved"));
        return;
    }

    if (restartRequested_) {
        discardPartialFiles();
        retryOrFail(QStringLiteral("Server range response did not match the resume offset"));
        return;
    }

    if (networkError == QNetworkReply::NoError && (status == 200 || status == 206)) {
        finishSuccess();
        return;
    }

    if (status == 416) {
        discardPartialFiles();
        retryOrFail(QStringLiteral("HTTP status 416"));
        return;
    }

    if (status >= 400) {
        const QString error = QStringLiteral("HTTP status %1").arg(status);
        if ((status == 403 || status == 404 || status == 408 ||
             status == 429 || status >= 500) && switchToFallback(error))
            return;
        if (status >= 500 || status == 408 || status == 429)
            retryOrFail(error);
        else
            failPermanently(error);
        return;
    }

    if (!switchToFallback(networkErrorText))
        retryOrFail(networkErrorText);
}

void Downloader::finishSuccess()
{
    const QString part = partPath(request_.targetPath);
    const qint64 finalSize = QFileInfo(part).size();
    if (request_.expectedSizeBytes >= 0 && finalSize != request_.expectedSizeBytes) {
        discardPartialFiles();
        failPermanently(QStringLiteral("Downloaded size mismatch: expected %1, got %2")
                            .arg(request_.expectedSizeBytes)
                            .arg(finalSize));
        return;
    }

    const QString stale = request_.targetPath + QStringLiteral(".stale");
    const bool hadTarget = QFile::exists(request_.targetPath);
    if (hadTarget) {
        QFile::remove(stale);
        if (!QFile::rename(request_.targetPath, stale)) {
            failPermanently(
                QStringLiteral("Cannot replace %1").arg(request_.targetPath));
            return;
        }
    }
    if (!QFile::rename(part, request_.targetPath)) {
        if (hadTarget)
            QFile::rename(stale, request_.targetPath);
        failPermanently(QStringLiteral("Cannot move the download into place"));
        return;
    }
    if (hadTarget)
        QFile::remove(stale);
    QFile::remove(metaPath(request_.targetPath));

    active_ = false;
    DownloadResult result;
    result.ok = true;
    result.source = currentSourceName();
    result.filePath = request_.targetPath;
    result.attemptsUsed = attemptsStarted_;
    result.httpStatus = resultHttpStatus_;
    result.lease = std::move(request_.lease);
    qInfo().noquote() << QStringLiteral("Update package source:")
                      << result.source << QStringLiteral("result: success");
    emit finished(result);
}

void Downloader::failPermanently(const QString& error)
{
    active_ = false;
    request_.lease.reset();
    DownloadResult result;
    if (attemptsStarted_ > 0)
        result.source = currentSourceName();
    result.error = error;
    result.attemptsUsed = attemptsStarted_;
    result.httpStatus = resultHttpStatus_;
    if (!result.source.isEmpty()) {
        qWarning().noquote()
            << QStringLiteral("Update package source:") << result.source
            << QStringLiteral("result: failed; reason:") << result.error;
    }
    emit finished(result);
}

void Downloader::retryOrFail(const QString& error)
{
    if (attemptsStarted_ >= request_.maxAttempts) {
        failPermanently(error);
        return;
    }
    retryTimer_.start(backoff_.sampleDelayMs(attemptsStarted_ - 1));
}

bool Downloader::switchToFallback(const QString& error)
{
    if (sourceIndex_ >= request_.fallbackUrls.size() ||
        attemptsStarted_ >= request_.maxAttempts)
        return false;
    const QString previousSource = currentSourceName();
    ++sourceIndex_;
    qWarning().noquote()
        << QStringLiteral("Update package source fallback:") << previousSource
        << QStringLiteral("->") << currentSourceName()
        << QStringLiteral("reason:") << error;
    discardPartialFiles();
    retryTimer_.start(0);
    return true;
}

bool Downloader::followRedirect(const QUrl& redirect, bool headRequest)
{
    const QUrl resolved = requestUrl_.resolved(redirect);
    if (sourceIndex_ != 0 ||
        !ServerEndpoints::isApprovedGithubRedirect(request_.url) ||
        ++redirectCount_ > 5 ||
        !ServerEndpoints::isApprovedGithubRedirect(resolved))
        return false;
    requestUrl_ = resolved;
    if (headRequest)
        startHead();
    else
        startGet(resumeOffset_ > 0);
    return true;
}

QString Downloader::currentSourceName() const
{
    return sourceIndex_ == 0 ? QStringLiteral("GitHub")
                             : QStringLiteral("OSS");
}

QUrl Downloader::currentUrl() const
{
    return sourceIndex_ == 0
               ? request_.url
               : request_.fallbackUrls.value(sourceIndex_ - 1);
}

void Downloader::discardPartialFiles()
{
    QFile::remove(partPath(request_.targetPath));
    QFile::remove(metaPath(request_.targetPath));
}

bool Downloader::readMeta(QByteArray* lastModified, qint64* totalSize) const
{
    QFile meta(metaPath(request_.targetPath));
    if (!meta.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(meta.readAll());
    if (!doc.isObject())
        return false;
    const QJsonObject object = doc.object();
    if (object[QStringLiteral("url")].toString() != currentUrl().toString())
        return false;
    const QByteArray modifiedValue =
        object[QStringLiteral("lastModified")].toString().toUtf8();
    const qint64 sizeValue =
        object[QStringLiteral("totalSize")].toInteger(-1);
    if (modifiedValue.isEmpty() || sizeValue <= 0)
        return false;
    *lastModified = modifiedValue;
    *totalSize = sizeValue;
    return true;
}

void Downloader::writeMeta(const QByteArray& lastModified,
                           qint64 totalSize) const
{
    QFile meta(metaPath(request_.targetPath));
    if (!meta.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    meta.write(QJsonDocument(
                   QJsonObject{
                       {QStringLiteral("url"), currentUrl().toString()},
                       {QStringLiteral("lastModified"),
                        QString::fromUtf8(lastModified)},
                       {QStringLiteral("totalSize"), totalSize},
                   })
                   .toJson(QJsonDocument::Compact));
}

}
