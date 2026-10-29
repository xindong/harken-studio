#include "ReleaseNotes/StudioReleaseNotes.h"

#include "Localization/HubLocalization.h"
#include "Update/ServerEndpoints.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

namespace VNH {

namespace {

constexpr qint64 kResponseMaxBytes = 4ll * 1024 * 1024;

QHash<QString, QUrl> parseLinks(const QByteArray& data, const QString& version)
{
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    const QString pageId = root.value(QStringLiteral("releaseNotesPageId"))
                               .toString().trimmed();
    if (root.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
        root.value(QStringLiteral("product")).toString() != QStringLiteral("harken-studio") ||
        root.value(QStringLiteral("version")).toString() != version ||
        pageId.isEmpty())
        return {};

    QHash<QString, QUrl> links;
    const QJsonArray pages = root.value(QStringLiteral("pages")).toArray();
    for (const QJsonValue& value : pages) {
        const QJsonObject page = value.toObject();
        if (page.value(QStringLiteral("id")).toString() != pageId)
            continue;
        const QString locale = page.value(QStringLiteral("locale")).toString();
        if (locale != QStringLiteral("zh") && locale != QStringLiteral("en"))
            continue;
        const QUrl path(page.value(QStringLiteral("path")).toString().trimmed(),
                        QUrl::StrictMode);
        if (!path.isValid() || !path.isRelative() || !path.authority().isEmpty() ||
            path.hasQuery() || path.hasFragment())
            continue;
        const QUrl url = ServerEndpoints::downloadUrl(
            QUrl(QStringLiteral("https://harkenstudio.xd.com/")), path.path());
        if (url.isValid() && !url.isEmpty())
            links.insert(locale, url);
    }
    return links;
}

}

StudioReleaseNotes::StudioReleaseNotes(QObject* parent)
    : QObject(parent)
{
}

void StudioReleaseNotes::fetch(const QString& version)
{
    static const QRegularExpression versionPattern(
        QRegularExpression::anchoredPattern(
            QStringLiteral("[A-Za-z0-9][A-Za-z0-9._-]{0,127}")));
    if (links_.contains(version) || pending_.contains(version) ||
        !versionPattern.match(version).hasMatch())
        return;

    const QUrl manifestUrl = ServerEndpoints::downloadUrl(
        ServerEndpoints::resolvedBaseUrl(),
        QStringLiteral("document/harken-studio/%1/manifest.json").arg(version));
    QNetworkRequest request(manifestUrl);
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    pending_.insert(version);
    QNetworkReply* reply = manager_.get(request);
    connect(reply, &QNetworkReply::downloadProgress, this,
            [reply](qint64 received, qint64 total) {
                if (received > kResponseMaxBytes || total > kResponseMaxBytes)
                    reply->abort();
            });
    connect(reply, &QNetworkReply::finished, this, [this, reply, version]() {
        pending_.remove(version);
        if (reply->error() == QNetworkReply::NoError &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
            reply->bytesAvailable() <= kResponseMaxBytes) {
            const auto links = parseLinks(reply->readAll(), version);
            if (!links.isEmpty())
                links_.insert(version, links);
        }
        reply->deleteLater();
        emit finished(version);
    });
}

QUrl StudioReleaseNotes::url(const QString& version) const
{
    const QString locale = HubLocalization::effectiveLanguage() ==
                                   HubLocalization::simplifiedChinesePreference()
                               ? QStringLiteral("zh")
                               : QStringLiteral("en");
    return links_.value(version).value(locale);
}

}
