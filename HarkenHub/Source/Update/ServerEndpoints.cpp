#include "Update/ServerEndpoints.h"

#include "Security/ManifestVerifier.h"

#include <QRegularExpression>

namespace VNH {

namespace {

QUrl withClientPath(const QUrl& baseUrl, const QString& relativePath)
{
    QUrl url = baseUrl;
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    url.setPath(path + relativePath);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

}

QUrl ServerEndpoints::defaultBaseUrl()
{
    return QUrl(QStringLiteral(
        "https://venus-1459151234.cos.ap-shanghai.myqcloud.com/"));
}

QUrl ServerEndpoints::resolvedBaseUrl()
{
    const QString override =
        qEnvironmentVariable("HARKEN_CONTENT_BASE_URL");
    return override.isEmpty() ? defaultBaseUrl() : QUrl(override);
}

QString ServerEndpoints::hostChannel()
{
    const QString platform = ManifestVerifier::hostPlatform();
    const QString prefix = platform == QStringLiteral("mac")
                               ? QStringLiteral("macos")
                               : platform;
    return prefix + QStringLiteral("-") + ManifestVerifier::hostArchitecture();
}

QUrl ServerEndpoints::latestUrl(const QUrl& baseUrl, const QString& product,
                                const QString& channel)
{
    Q_UNUSED(channel)
    return withClientPath(baseUrl,
                          QStringLiteral("/release/%1/manifest.json")
                              .arg(product));
}

QUrl ServerEndpoints::githubLatestUrl(const QString& product)
{
    if (product != QStringLiteral("hub") &&
        product != QStringLiteral("harken"))
        return QUrl();
    return withClientPath(
        QUrl(QStringLiteral("https://github.com/xindong/harken-studio")),
        QStringLiteral("/releases/latest/download/%1-manifest.json")
            .arg(product));
}

QList<QUrl> ServerEndpoints::manifestUrls(const QUrl& fallbackBaseUrl,
                                          const QString& product,
                                          const QString& channel)
{
    QList<QUrl> result;
    const QUrl github = githubLatestUrl(product);
    if (!github.isEmpty())
        result.append(github);
    result.append(latestUrl(fallbackBaseUrl, product, channel));
    return result;
}

QUrl ServerEndpoints::versionsUrl(const QUrl& baseUrl, const QString& product,
                                  const QString& channel)
{
    return latestUrl(baseUrl, product, channel);
}

bool ServerEndpoints::isReleaseObjectPath(const QString& objectPath,
                                          const QString& product,
                                          const QString& channel,
                                          const QString& version)
{
    if (objectPath.contains(QLatin1Char('%')) ||
        objectPath.contains(QLatin1Char('\\')) ||
        objectPath.contains(QLatin1Char('?')) ||
        objectPath.contains(QLatin1Char('#')))
        return false;
    const QStringList parts = objectPath.split(QLatin1Char('/'));
    static const QRegularExpression token(
        QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9][A-Za-z0-9._-]*")));
    return parts.size() == 6 && parts[0] == QStringLiteral("release") &&
           parts[1] == product && parts[2] == channel &&
           parts[3] == version && token.match(parts[4]).hasMatch() &&
           token.match(parts[5]).hasMatch();
}

QUrl ServerEndpoints::downloadUrl(const QUrl& baseUrl,
                                  const QString& objectPath)
{
    if (objectPath.isEmpty() || objectPath.startsWith(QLatin1Char('/')) ||
        objectPath.contains(QLatin1Char('\\')) ||
        objectPath.contains(QLatin1Char('?')) ||
        objectPath.contains(QLatin1Char('#')))
        return QUrl();
    const QStringList parts = objectPath.split(QLatin1Char('/'));
    if (parts.contains(QString()) || parts.contains(QStringLiteral(".")) ||
        parts.contains(QStringLiteral("..")))
        return QUrl();
    return withClientPath(baseUrl, QStringLiteral("/") + objectPath);
}

QList<QUrl> ServerEndpoints::downloadUrls(const QUrl& fallbackBaseUrl,
                                          const QString& objectPath,
                                          const QString& releaseTag)
{
    QList<QUrl> result;
    const QStringList parts = objectPath.split(QLatin1Char('/'));
    if (parts.size() != 6 ||
        !isReleaseObjectPath(objectPath, parts[1], parts[2], parts[3]))
        return result;
    if (isSafeReleaseTag(releaseTag)) {
        result.append(withClientPath(
            QUrl(QStringLiteral("https://github.com/xindong/harken-studio")),
            QStringLiteral("/releases/download/%1/%2")
                .arg(releaseTag, parts.last())));
    }
    const QUrl fallback = downloadUrl(fallbackBaseUrl, objectPath);
    if (!fallback.isEmpty())
        result.append(fallback);
    return result;
}

bool ServerEndpoints::isSafeReleaseTag(const QString& releaseTag)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return pattern.match(releaseTag).hasMatch();
}

bool ServerEndpoints::isApprovedGithubRedirect(const QUrl& url)
{
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        !url.userInfo().isEmpty() || url.hasFragment() ||
        (url.port() != -1 && url.port() != 443))
        return false;
    const QString host = url.host().toLower();
    const QStringList parts = url.path().split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    if (host == QStringLiteral("release-assets.githubusercontent.com") ||
        host == QStringLiteral("objects.githubusercontent.com"))
        return !parts.isEmpty() && !parts.contains(QStringLiteral(".."));
    if (url.hasQuery())
        return false;
    if (host != QStringLiteral("github.com") || parts.size() < 6 ||
        parts[0] != QStringLiteral("xindong") ||
        parts[1] != QStringLiteral("harken-studio") ||
        parts[2] != QStringLiteral("releases"))
        return false;
    if (parts[3] == QStringLiteral("latest"))
        return parts.size() == 6 && parts[4] == QStringLiteral("download") &&
               (parts[5] == QStringLiteral("hub-manifest.json") ||
                parts[5] == QStringLiteral("harken-manifest.json"));
    return parts[3] == QStringLiteral("download") && parts.size() == 6 &&
           isSafeReleaseTag(parts[4]) && !parts[5].isEmpty();
}

}
