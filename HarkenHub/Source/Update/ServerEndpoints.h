#pragma once

#include <QString>
#include <QUrl>
#include <QList>

namespace VNH {

class ServerEndpoints
{
public:
    static QUrl defaultBaseUrl();
    static QUrl resolvedBaseUrl();
    static QString hostChannel();
    static QUrl latestUrl(const QUrl& baseUrl, const QString& product,
                          const QString& channel);
    static QUrl githubLatestUrl(const QString& product);
    static QList<QUrl> manifestUrls(const QUrl& fallbackBaseUrl,
                                    const QString& product,
                                    const QString& channel);
    static QUrl versionsUrl(const QUrl& baseUrl, const QString& product,
                            const QString& channel);
    static bool isReleaseObjectPath(const QString& objectPath,
                                    const QString& product,
                                    const QString& channel,
                                    const QString& version);
    static QUrl downloadUrl(const QUrl& baseUrl, const QString& objectPath);
    static QList<QUrl> downloadUrls(const QUrl& fallbackBaseUrl,
                                    const QString& objectPath,
                                    const QString& releaseTag);
    static bool isSafeReleaseTag(const QString& releaseTag);
    static bool isApprovedGithubRedirect(const QUrl& url);
};

}
