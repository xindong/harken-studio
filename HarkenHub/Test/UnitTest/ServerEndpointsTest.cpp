#include "Update/ServerEndpoints.h"

#include "Security/ManifestVerifier.h"

#include <gtest/gtest.h>

using VNH::ManifestVerifier;
using VNH::ServerEndpoints;

TEST(ServerEndpointsTest, DefaultSourcesMatchPublishedProducts)
{
    const QUrl base = ServerEndpoints::defaultBaseUrl();
    for (const QString& product : {QStringLiteral("hub"), QStringLiteral("harken")}) {
        const auto urls = ServerEndpoints::manifestUrls(
            base, product, QStringLiteral("windows-x64"));
        ASSERT_EQ(urls.size(), 2);
        EXPECT_EQ(urls[0].toString(),
                  QStringLiteral("https://github.com/xindong/harken-studio/"
                                 "releases/latest/download/%1-manifest.json").arg(product));
        EXPECT_TRUE(ServerEndpoints::isApprovedGithubRedirect(urls[0]));
        EXPECT_EQ(urls[1].toString(),
                  QStringLiteral("https://venus-1459151234.cos.ap-shanghai.myqcloud.com/"
                                 "release/%1/manifest.json").arg(product));
    }
}

TEST(ServerEndpointsTest, ResolvedBaseUrlHonorsEnvironmentOverride)
{
    qunsetenv("HARKEN_CONTENT_BASE_URL");
    EXPECT_EQ(ServerEndpoints::resolvedBaseUrl(),
              ServerEndpoints::defaultBaseUrl());

    qputenv("HARKEN_CONTENT_BASE_URL", "http://127.0.0.1:9099");
    EXPECT_EQ(ServerEndpoints::resolvedBaseUrl(),
              QUrl(QStringLiteral("http://127.0.0.1:9099")));
    qunsetenv("HARKEN_CONTENT_BASE_URL");
}

TEST(ServerEndpointsTest, HostChannelMatchesPlatformAndArchitecture)
{
    const QString channel = ServerEndpoints::hostChannel();
#if defined(Q_OS_WIN)
    EXPECT_TRUE(channel.startsWith(QStringLiteral("windows-")));
#elif defined(Q_OS_MACOS)
    EXPECT_TRUE(channel.startsWith(QStringLiteral("macos-")));
#endif
    EXPECT_TRUE(channel.endsWith(ManifestVerifier::hostArchitecture()));
}

TEST(ServerEndpointsTest, UrlsComposeUnderBasePath)
{
    const QUrl base(QStringLiteral("https://updates.example/harken/"));
    EXPECT_EQ(ServerEndpoints::latestUrl(base, QStringLiteral("harken"),
                                         QStringLiteral("windows-x64"))
                  .toString(),
              QStringLiteral("https://updates.example/harken/release/harken/"
                             "manifest.json"));
    EXPECT_EQ(ServerEndpoints::versionsUrl(base, QStringLiteral("harken"),
                                           QStringLiteral("windows-x64"))
                  .toString(),
              QStringLiteral("https://updates.example/harken/release/harken/"
                             "manifest.json"));
    EXPECT_EQ(ServerEndpoints::downloadUrl(
                  base, QStringLiteral(
                            "release/hub/macos-arm64/1.2.3/run12.abcd1234/Hub.pkg"))
                  .toString(),
              QStringLiteral("https://updates.example/harken/release/hub/"
                             "macos-arm64/1.2.3/run12.abcd1234/Hub.pkg"));
    EXPECT_TRUE(ServerEndpoints::downloadUrl(
                    base, QStringLiteral("../outside.exe"))
                    .isEmpty());
}

TEST(ServerEndpointsTest, GitHubIsPrimaryAndCosIsFallback)
{
    const QUrl cos(QStringLiteral("https://updates.example/harken/"));
    const QList<QUrl> manifests = ServerEndpoints::manifestUrls(
        cos, QStringLiteral("hub"), QStringLiteral("windows-x64"));
    ASSERT_EQ(manifests.size(), 2);
    EXPECT_EQ(manifests[0].toString(),
              QStringLiteral("https://github.com/xindong/harken-studio/"
                             "releases/latest/download/hub-manifest.json"));
    EXPECT_EQ(manifests[1].toString(),
              QStringLiteral("https://updates.example/harken/release/hub/"
                             "manifest.json"));

    const QList<QUrl> downloads = ServerEndpoints::downloadUrls(
        cos,
        QStringLiteral("release/hub/windows-x64/1.2.3/run12.abcd1234/Hub.exe"),
        QStringLiteral("release-42-abcd1234"));
    ASSERT_EQ(downloads.size(), 2);
    EXPECT_EQ(downloads[0].toString(),
              QStringLiteral("https://github.com/xindong/harken-studio/"
                             "releases/download/release-42-abcd1234/Hub.exe"));
    EXPECT_EQ(downloads[1].toString(),
              QStringLiteral("https://updates.example/harken/release/hub/"
                             "windows-x64/1.2.3/run12.abcd1234/Hub.exe"));
}

TEST(ServerEndpointsTest, GithubRedirectsAreStrictlyAllowlisted)
{
    EXPECT_TRUE(ServerEndpoints::isApprovedGithubRedirect(QUrl(
        QStringLiteral("https://github.com/xindong/harken-studio/releases/"
                       "download/release-42-abcd/Hub.exe"))));
    EXPECT_TRUE(ServerEndpoints::isApprovedGithubRedirect(QUrl(
        QStringLiteral("https://release-assets.githubusercontent.com/"
                       "github-production-release-asset/123?sp=r&sig=value"))));
    EXPECT_FALSE(ServerEndpoints::isApprovedGithubRedirect(QUrl(
        QStringLiteral("https://example.invalid/Hub.exe"))));
    EXPECT_FALSE(ServerEndpoints::isApprovedGithubRedirect(QUrl(
        QStringLiteral("https://github.com/xindong/harken-studio/releases/"
                       "download/release-42-abcd/Hub.exe?redirect=1"))));
    EXPECT_FALSE(ServerEndpoints::isSafeReleaseTag(QStringLiteral("../bad")));
}

TEST(ServerEndpointsTest, ReleaseObjectPathIsBoundToManifestIdentity)
{
    EXPECT_FALSE(ServerEndpoints::isReleaseObjectPath(
        QStringLiteral("releases/harken/windows-x64/1.4.0/run12-abcd1234-20261008T100000Z/Harken.zip"),
        QStringLiteral("harken"), QStringLiteral("windows-x64"),
        QStringLiteral("1.4.0")));
    EXPECT_TRUE(ServerEndpoints::isReleaseObjectPath(
        QStringLiteral("release/harken/windows-x64/1.4.0/run12-abcd1234-20261008T100000Z/Harken.zip"),
        QStringLiteral("harken"), QStringLiteral("windows-x64"),
        QStringLiteral("1.4.0")));
    EXPECT_FALSE(ServerEndpoints::isReleaseObjectPath(
        QStringLiteral("release/hub/windows-x64/1.4.0/run12-abcd1234-20261008T100000Z/Harken.zip"),
        QStringLiteral("harken"), QStringLiteral("windows-x64"),
        QStringLiteral("1.4.0")));
    EXPECT_FALSE(ServerEndpoints::isReleaseObjectPath(
        QStringLiteral("release/harken/windows-x64/1.4.0/run12-abcd1234-20261008T100000Z/%2e%2e.zip"),
        QStringLiteral("harken"), QStringLiteral("windows-x64"),
        QStringLiteral("1.4.0")));
    for (const QString& tail : {
             QStringLiteral("Harken.zip"),
             QStringLiteral("/Harken.zip"),
             QStringLiteral("./Harken.zip"),
             QStringLiteral("../Harken.zip"),
             QStringLiteral("%2e%2e/Harken.zip"),
             QStringLiteral("run12.abcd1234/.."),
             QStringLiteral("run12.abcd1234/"),
             QStringLiteral("run12.abcd1234/extra/Harken.zip"),
             QStringLiteral("run12.abcd1234/Harken.zip?redirect=1"),
             QStringLiteral("run12.abcd1234/Harken.zip#fragment"),
             QStringLiteral("run12.abcd1234\n/Harken.zip"),
             QStringLiteral("run12.abcd1234/Harken.zip\n"),
             QStringLiteral("run12.abcd1234\\outside/Harken.zip")}) {
        const QString path = QStringLiteral("release/harken/windows-x64/1.4.0/") + tail;
        EXPECT_FALSE(ServerEndpoints::isReleaseObjectPath(
            path, QStringLiteral("harken"), QStringLiteral("windows-x64"),
            QStringLiteral("1.4.0"))) << tail.toStdString();
        EXPECT_TRUE(ServerEndpoints::downloadUrls(
            ServerEndpoints::defaultBaseUrl(), path,
            QStringLiteral("release-42-abcd1234")).isEmpty()) << tail.toStdString();
    }
}
