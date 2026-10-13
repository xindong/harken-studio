#include "Update/UpdateChecker.h"

#include "Update/ServerEndpoints.h"

#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>
#include <climits>
#include <limits>

namespace VNH {

namespace {

constexpr qint64 kResponseMaxBytes = 1024 * 1024;

QString sourceName(int sourceIndex)
{
    return sourceIndex == 0 ? QStringLiteral("GitHub")
                            : QStringLiteral("OSS");
}

bool isAcceptableLatestUrl(const QUrl& url, const QUrl& trustedHttpOrigin)
{
    if (!url.isValid() || url.isEmpty())
        return false;
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("https"))
        return true;
    if (scheme != QStringLiteral("http"))
        return false;
    const QString host = url.host().toLower();
    if (host == QStringLiteral("localhost") || QHostAddress(host).isLoopback())
        return true;
    return trustedHttpOrigin.scheme().toLower() == QStringLiteral("http") &&
           !trustedHttpOrigin.host().isEmpty() &&
           host == trustedHttpOrigin.host().toLower() &&
           url.port(80) == trustedHttpOrigin.port(80);
}

}

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent)
{
    backoff_.baseDelayMs = 30 * 1000;
    backoff_.maxDelayMs = 30 * 60 * 1000;
    nextTimer_.setSingleShot(true);
    connect(&nextTimer_, &QTimer::timeout, this, &UpdateChecker::beginRound);
}

void UpdateChecker::setBackoffPolicy(const BackoffPolicy& policy)
{
    backoff_ = policy;
}

bool UpdateChecker::start(const Options& options)
{
    if (started_ || options.product.isEmpty() || options.channel.isEmpty() ||
        !isAcceptableLatestUrl(options.latestUrl, options.trustedHttpOrigin))
        return false;
    for (const QUrl& fallback : options.fallbackUrls) {
        if (!isAcceptableLatestUrl(fallback, options.trustedHttpOrigin))
            return false;
    }
    options_ = options;
    options_.intervalMs = std::max(options_.intervalMs, kMinIntervalMs);
    started_ = true;
    failureStreak_ = 0;
    beginRound();
    return true;
}

void UpdateChecker::stop()
{
    if (!started_)
        return;
    started_ = false;
    forcedRoundPending_ = false;
    nextTimer_.stop();
    if (reply_) {
        stopping_ = true;
        reply_->abort();
        stopping_ = false;
    }
}

bool UpdateChecker::checkNow()
{
    if (!started_ || reply_)
        return false;
    nextTimer_.stop();
    beginRound();
    return true;
}

void UpdateChecker::invalidateCache()
{
    etags_.clear();
    if (reply_)
        forcedRoundPending_ = true;
}

bool UpdateChecker::isChecking() const
{
    return reply_ != nullptr;
}

qint64 UpdateChecker::effectiveIntervalMs() const
{
    return options_.intervalMs;
}

void UpdateChecker::beginRound()
{
    if (!started_ || reply_)
        return;

    roundUrls_.clear();
    roundUrls_.append(options_.latestUrl);
    for (const QUrl& fallback : options_.fallbackUrls) {
        if (!roundUrls_.contains(fallback))
            roundUrls_.append(fallback);
    }
    sourceIndex_ = 0;
    redirectCount_ = 0;
    requestUrl_ = roundUrls_.front();
    beginRequest();
}

void UpdateChecker::beginRequest()
{
    if (!started_ || reply_ || requestUrl_.isEmpty())
        return;

    QNetworkRequest request(requestUrl_);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(std::max(options_.transferTimeoutMs, 1));
    const QByteArray etag = etags_.value(roundUrls_[sourceIndex_].toString());
    sentConditional_ = !etag.isEmpty();
    roundOversized_ = false;
    if (sentConditional_)
        request.setRawHeader(QByteArrayLiteral("If-None-Match"), etag);

    qInfo().noquote() << QStringLiteral("Update manifest request source:")
                      << sourceName(sourceIndex_);
    reply_ = manager_.get(request);
    connect(reply_, &QNetworkReply::finished, this,
            &UpdateChecker::handleReplyFinished);
    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64) {
                if (received > kResponseMaxBytes && reply_ && !roundOversized_) {
                    roundOversized_ = true;
                    reply_->abort();
                }
            });
}

bool UpdateChecker::tryFallback(int status, int networkError,
                                const QString& error)
{
    const bool availabilityFailure =
        networkError != QNetworkReply::NoError || status == 403 ||
        status == 404 || status == 408 || status == 429 || status >= 500;
    if (!availabilityFailure || sourceIndex_ + 1 >= roundUrls_.size())
        return false;
    const QString previousSource = sourceName(sourceIndex_);
    ++sourceIndex_;
    qWarning().noquote()
        << QStringLiteral("Update manifest source fallback:")
        << previousSource << QStringLiteral("->") << sourceName(sourceIndex_)
        << QStringLiteral("reason:") << error;
    redirectCount_ = 0;
    requestUrl_ = roundUrls_[sourceIndex_];
    beginRequest();
    return true;
}

void UpdateChecker::handleReplyFinished()
{
    const auto networkError = reply_->error();
    const QString networkErrorText = reply_->errorString();
    const int status =
        reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseEtag =
        reply_->rawHeader(QByteArrayLiteral("ETag"));
    const QUrl redirect = reply_->attribute(
        QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const QByteArray body =
        reply_->bytesAvailable() > kResponseMaxBytes
            ? QByteArray()
            : reply_->readAll();
    const bool oversized = roundOversized_ || reply_->bytesAvailable() > 0;
    reply_->deleteLater();
    reply_ = nullptr;

    if (stopping_)
        return;

    UpdateCheckResult result;
    result.source = sourceName(sourceIndex_);

    if (oversized) {
        result.error = QStringLiteral("Release feed response exceeds %1 bytes")
                           .arg(kResponseMaxBytes);
        ++failureStreak_;
        finishRound(result, true);
        return;
    }

    if (status >= 300 && status < 400 && status != 304) {
        const QUrl resolved = requestUrl_.resolved(redirect);
        if (sourceIndex_ != 0 ||
            !ServerEndpoints::isApprovedGithubRedirect(roundUrls_.front()) ||
            ++redirectCount_ > 5 ||
            !ServerEndpoints::isApprovedGithubRedirect(resolved)) {
            result.error =
                QStringLiteral("HTTP redirect %1 is not approved").arg(status);
            ++failureStreak_;
            finishRound(result, true);
            return;
        }
        requestUrl_ = resolved;
        beginRequest();
        return;
    }

    if (status == 304) {
        if (!sentConditional_) {
            result.error = QStringLiteral(
                "Server answered 304 to an unconditional release feed request");
            ++failureStreak_;
            finishRound(result, true);
            return;
        }
        result.ok = true;
        result.notModified = true;
        failureStreak_ = 0;
        finishRound(result, false);
        return;
    }

    if (status == 404) {
        if (tryFallback(status, networkError, QStringLiteral("HTTP status 404")))
            return;
        result.ok = true;
        result.manifestAbsent = true;
        failureStreak_ = 0;
        finishRound(result, false);
        return;
    }

    if (networkError != QNetworkReply::NoError || status != 200) {
        const QString error = status > 0 && status != 200
                                  ? QStringLiteral("HTTP status %1").arg(status)
                                  : networkErrorText;
        if (tryFallback(status, networkError, error))
            return;
        result.error = error;
        ++failureStreak_;
        finishRound(result, true);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument releaseDoc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !releaseDoc.isObject()) {
        result.error =
            QStringLiteral("Release feed response is not a JSON object: %1")
                .arg(parseError.errorString());
        ++failureStreak_;
        finishRound(result, true);
        return;
    }

    const QJsonObject releaseRoot = releaseDoc.object();
    const QJsonValue channelsValue = releaseRoot[QStringLiteral("channels")];
    if (releaseRoot[QStringLiteral("schema_version")].toInt(-1) != 1 ||
        !channelsValue.isObject()) {
        result.error = QStringLiteral("Release feed schema is invalid");
        ++failureStreak_;
        finishRound(result, true);
        return;
    }
    const QJsonObject channels = channelsValue.toObject();
    const QJsonArray versions = channels[options_.channel].toArray();
    if (versions.isEmpty()) {
        etags_[roundUrls_[sourceIndex_].toString()] = responseEtag;
        failureStreak_ = 0;
        result.ok = true;
        result.manifestAbsent = true;
        finishRound(result, false);
        return;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject latest;
    QByteArray envelope;
    qint64 previousSequence = std::numeric_limits<qint64>::max();
    QString feedError;
    for (qsizetype index = 0; index < versions.size(); ++index) {
        if (!versions[index].isObject()) {
            if (index == 0) {
                feedError =
                    QStringLiteral("Release feed entry is not an object");
                break;
            }
            continue;
        }
        const QJsonObject entry = versions[index].toObject();
        const QJsonObject integrity =
            entry[QStringLiteral("integrity")].toObject();
        const QByteArray entryEnvelope =
            QJsonDocument(integrity).toJson(QJsonDocument::Compact);
        const ManifestVerifyResult entryVerified = ManifestVerifier::verify(
            entryEnvelope, options_.manifestPublicKey, now, true);
        if (!entryVerified.ok) {
            if (index == 0) {
                feedError = entryVerified.error;
                break;
            }
            continue;
        }
        if (entry[QStringLiteral("version")].toString() !=
            entryVerified.manifest.version) {
            if (index == 0) {
                feedError = QStringLiteral(
                    "Release entry version does not match signed manifest");
                break;
            }
            continue;
        }
        if (!ServerEndpoints::isReleaseObjectPath(
                entry[QStringLiteral("file")].toString(), options_.product,
                options_.channel, entryVerified.manifest.version)) {
            if (index == 0) {
                feedError = QStringLiteral(
                    "Release entry object path does not match product, channel, and version");
                break;
            }
            continue;
        }
        if (entryVerified.manifest.releaseSequence >= previousSequence) {
            feedError = QStringLiteral(
                "Release feed entries are not ordered newest first");
            break;
        }
        previousSequence = entryVerified.manifest.releaseSequence;
        if (index == 0) {
            latest = entry;
            envelope = entryEnvelope;
        }
    }
    if (!feedError.isEmpty()) {
        result.error = feedError;
        ++failureStreak_;
        finishRound(result, true);
        return;
    }

    const ManifestVerifyResult verified = ManifestVerifier::verify(
        envelope, options_.manifestPublicKey, now);
    if (!verified.ok) {
        result.error = verified.error;
        ++failureStreak_;
        finishRound(result, true);
        return;
    }

    etags_[roundUrls_[sourceIndex_].toString()] = responseEtag;
    failureStreak_ = 0;
    result.ok = true;
    result.manifest = verified.manifest;
    result.envelope = envelope;
    result.objectPath = latest[QStringLiteral("file")].toString();
    result.releaseTag = latest[QStringLiteral("releaseTag")].toString();
    const QString listedVersion = latest[QStringLiteral("version")].toString();
    if (listedVersion != result.manifest.version) {
        result.ok = false;
        result.error = QStringLiteral(
            "Release entry version does not match signed manifest");
        ++failureStreak_;
        finishRound(result, true);
        return;
    }
    if (!ServerEndpoints::isReleaseObjectPath(
            result.objectPath, options_.product, options_.channel,
            result.manifest.version)) {
        result.ok = false;
        result.error = QStringLiteral(
            "Release entry object path does not match product, channel, and version");
        ++failureStreak_;
        finishRound(result, true);
        return;
    }
    if (!result.releaseTag.isEmpty() &&
        !ServerEndpoints::isSafeReleaseTag(result.releaseTag)) {
        result.ok = false;
        result.error = QStringLiteral("Release entry tag is invalid");
        ++failureStreak_;
        finishRound(result, true);
        return;
    }
    finishRound(result, false);
}

void UpdateChecker::finishRound(UpdateCheckResult result, bool failed)
{
    result.source = sourceName(sourceIndex_);
    if (result.ok) {
        qInfo().noquote() << QStringLiteral("Update manifest source:")
                          << result.source << QStringLiteral("result: success");
    } else {
        qWarning().noquote()
            << QStringLiteral("Update manifest source:") << result.source
            << QStringLiteral("result: failed; reason:") << result.error;
    }
    scheduleNext(failed);
    emit checkFinished(result);
}

void UpdateChecker::scheduleNext(bool failed)
{
    if (!started_)
        return;
    if (forcedRoundPending_) {
        forcedRoundPending_ = false;
        nextTimer_.start(0);
        return;
    }
    const qint64 delay =
        failed ? std::min<qint64>(backoff_.sampleDelayMs(failureStreak_ - 1),
                                  options_.intervalMs)
               : options_.intervalMs;
    nextTimer_.start(static_cast<int>(std::min<qint64>(delay, INT_MAX)));
}

}
