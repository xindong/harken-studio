#include "Update/ServerVersionCatalog.h"

#include "Security/ManifestVerifier.h"
#include "Update/ServerEndpoints.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace VNH {

namespace {

constexpr qint64 kResponseMaxBytes = 16ll * 1024 * 1024;

}

ServerVersionCatalog::ServerVersionCatalog(QObject* parent)
    : QObject(parent)
{
}

bool ServerVersionCatalog::isFetching() const
{
    return reply_ != nullptr;
}

bool ServerVersionCatalog::fetch(const QUrl& versionsUrl,
                                 const QString& product,
                                 const QString& channel)
{
    return fetch(QList<QUrl>{versionsUrl}, product, channel);
}

bool ServerVersionCatalog::fetch(const QList<QUrl>& versionsUrls,
                                 const QString& product,
                                 const QString& channel)
{
    if (reply_ || versionsUrls.isEmpty() || product.isEmpty() ||
        channel.isEmpty())
        return false;
    for (const QUrl& url : versionsUrls) {
        if (!url.isValid() || url.isEmpty())
            return false;
    }
    product_ = product;
    channel_ = channel;
    urls_ = versionsUrls;
    sourceIndex_ = 0;
    redirectCount_ = 0;
    requestUrl_ = urls_.front();
    beginRequest();
    return true;
}

void ServerVersionCatalog::beginRequest()
{
    QNetworkRequest request(requestUrl_);
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    reply_ = manager_.get(request);
    connect(reply_, &QNetworkReply::finished, this,
            &ServerVersionCatalog::handleReplyFinished);
}

void ServerVersionCatalog::handleReplyFinished()
{
    QNetworkReply* reply = reply_;
    reply_ = nullptr;
    reply->deleteLater();

    ServerVersionList result;
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QUrl redirect = reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (status >= 300 && status < 400) {
        const QUrl resolved = requestUrl_.resolved(redirect);
        if (sourceIndex_ == 0 &&
            ServerEndpoints::isApprovedGithubRedirect(urls_.front()) &&
            ++redirectCount_ <= 5 &&
            ServerEndpoints::isApprovedGithubRedirect(resolved)) {
            requestUrl_ = resolved;
            beginRequest();
            return;
        }
        result.error = QStringLiteral("Version list redirect is not approved");
        emit finished(result);
        return;
    }
    const bool availabilityFailure =
        reply->error() != QNetworkReply::NoError || status == 403 ||
        status == 404 || status == 408 || status == 429 || status >= 500;
    if (availabilityFailure && sourceIndex_ + 1 < urls_.size()) {
        ++sourceIndex_;
        redirectCount_ = 0;
        requestUrl_ = urls_[sourceIndex_];
        beginRequest();
        return;
    }
    if (reply->error() != QNetworkReply::NoError && status != 404) {
        result.error = reply->errorString();
        emit finished(result);
        return;
    }
    if (status == 404) {
        result.ok = true;
        emit finished(result);
        return;
    }
    if (reply->bytesAvailable() > kResponseMaxBytes) {
        result.error = QStringLiteral("Version list response is too large");
        emit finished(result);
        return;
    }
    emit finished(parse(reply->readAll(), product_, channel_));
}

ServerVersionList ServerVersionCatalog::parse(const QByteArray& json,
                                              const QString& product,
                                              const QString& channel)
{
    ServerVersionList result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("Invalid version list response: %1")
                           .arg(parseError.errorString());
        return result;
    }
    const QJsonObject root = doc.object();
    const QJsonValue channels = root[QStringLiteral("channels")];
    if (root[QStringLiteral("schema_version")].toInt(-1) != 1 ||
        !channels.isObject()) {
        result.error =
            QStringLiteral("Release manifest schema is invalid");
        return result;
    }
    const QJsonValue versions =
        channels.toObject()[channel];
    if (!versions.isArray()) {
        result.ok = true;
        return result;
    }
    for (const QJsonValue& value : versions.toArray()) {
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();
        ServerVersionEntry entry;
        entry.version = object[QStringLiteral("version")].toString();
        if (entry.version.isEmpty())
            continue;
        const QJsonObject integrity =
            object[QStringLiteral("integrity")].toObject();
        if (integrity.isEmpty())
            continue;
        entry.updateManifest =
            QJsonDocument(integrity).toJson(QJsonDocument::Compact);
        entry.objectPath = object[QStringLiteral("file")].toString();
        entry.releaseTag = object[QStringLiteral("releaseTag")].toString();
        if (!ServerEndpoints::isReleaseObjectPath(
                entry.objectPath, product, channel, entry.version))
            continue;
        if (!entry.releaseTag.isEmpty() &&
            !ServerEndpoints::isSafeReleaseTag(entry.releaseTag))
            continue;
        entry.fileName = entry.objectPath.section(QLatin1Char('/'), -1);
        const QByteArray payloadBytes = QByteArray::fromBase64(
            integrity[QStringLiteral("payloadBase64")].toString().toLatin1(),
            QByteArray::AbortOnBase64DecodingErrors);
        const QJsonObject payload =
            QJsonDocument::fromJson(payloadBytes).object();
        entry.createdAt = payload[QStringLiteral("publishedAt")].toString();
        entry.fileSize = payload[QStringLiteral("packageSize")].toInteger(-1);
        result.entries.push_back(std::move(entry));
    }
    result.ok = true;
    return result;
}

QByteArray ServerVersionCatalog::matchEnvelopeForBuild(
    const std::vector<ServerVersionEntry>& entries,
    const QByteArray& publicKey, const QString& buildId, const QDateTime& now)
{
    for (const ServerVersionEntry& entry : entries) {
        if (entry.updateManifest.isEmpty())
            continue;
        const ManifestVerifyResult verified =
            ManifestVerifier::verify(entry.updateManifest, publicKey, now,
                                     true);
        if (verified.ok && verified.manifest.buildId == buildId)
            return entry.updateManifest;
    }
    return QByteArray();
}

}
