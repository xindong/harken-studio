#include "Update/ServerVersionCatalog.h"

#include "Support/ManifestTestUtils.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using VNH::ServerVersionCatalog;
using HarkenHubTest::baseManifestPayload;
using HarkenHubTest::makeKeypair;
using HarkenHubTest::signEnvelope;
using HarkenHubTest::TestKeypair;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-23T12:00:00Z"), Qt::ISODate);

QByteArray versionsResponse(const std::vector<QJsonObject>& versions)
{
    QJsonArray array;
    for (const QJsonObject& version : versions)
        array.append(version);
    QJsonObject root;
    QJsonObject channels;
    channels[VNH::ServerEndpoints::hostChannel()] = array;
    root[QStringLiteral("schema_version")] = 1;
    root[QStringLiteral("channels")] = channels;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QJsonObject versionObject(const QString& version, const QString& envelope)
{
    QJsonObject object;
    object[QStringLiteral("version")] = version;
    object[QStringLiteral("file")] =
        QStringLiteral("release/harken/%1/%2/run12-abcd1234-20261008T100000Z/Harken-%2.zip")
            .arg(VNH::ServerEndpoints::hostChannel(), version);
    if (!envelope.isEmpty()) {
        QJsonObject integrity =
            QJsonDocument::fromJson(envelope.toUtf8()).object();
        if (integrity.isEmpty()) {
            QJsonObject payload;
            payload[QStringLiteral("packageSize")] = 13000;
            payload[QStringLiteral("publishedAt")] =
                QStringLiteral("2026-07-20T03:00:00Z");
            integrity[QStringLiteral("algorithm")] =
                QStringLiteral("ed25519");
            integrity[QStringLiteral("payloadBase64")] = QString::fromLatin1(
                QJsonDocument(payload).toJson(QJsonDocument::Compact)
                    .toBase64());
            integrity[QStringLiteral("signatureBase64")] = QString();
        }
        object[QStringLiteral("integrity")] = integrity;
    }
    return object;
}

}

TEST(ServerVersionCatalogTest, ParsesVersionListResponse)
{
    QJsonObject release =
        versionObject(QStringLiteral("1.4.0"), QStringLiteral("envelope"));
    release[QStringLiteral("releaseTag")] =
        QStringLiteral("release-42-abcd1234");
    const auto result = ServerVersionCatalog::parse(versionsResponse(
        {release,
         versionObject(QStringLiteral("1.3.0"), QString())}),
        QStringLiteral("harken"),
        VNH::ServerEndpoints::hostChannel());

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_EQ(result.entries.size(), 1u);
    EXPECT_EQ(result.entries[0].version, QStringLiteral("1.4.0"));
    EXPECT_FALSE(result.entries[0].updateManifest.isEmpty());
    EXPECT_EQ(result.entries[0].fileName, QStringLiteral("Harken-1.4.0.zip"));
    EXPECT_EQ(result.entries[0].fileSize, 13000);
    EXPECT_EQ(result.entries[0].createdAt,
              QStringLiteral("2026-07-20T03:00:00Z"));
    EXPECT_EQ(result.entries[0].releaseTag,
              QStringLiteral("release-42-abcd1234"));
}

TEST(ServerVersionCatalogTest, SkipsEntriesWithUnsafeReleaseTag)
{
    QJsonObject release =
        versionObject(QStringLiteral("1.4.0"), QStringLiteral("envelope"));
    release[QStringLiteral("releaseTag")] = QStringLiteral("../outside");

    const auto result = ServerVersionCatalog::parse(
        versionsResponse({release}), QStringLiteral("harken"),
        VNH::ServerEndpoints::hostChannel());

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.entries.empty());
}

TEST(ServerVersionCatalogTest, RejectsMalformedResponses)
{
    EXPECT_FALSE(ServerVersionCatalog::parse(
                     QByteArrayLiteral("not json"),
                     QStringLiteral("harken"),
                     VNH::ServerEndpoints::hostChannel()).ok);
    EXPECT_FALSE(ServerVersionCatalog::parse(
                     QByteArrayLiteral("{\"product\":\"harken\"}"),
                     QStringLiteral("harken"),
                     VNH::ServerEndpoints::hostChannel()).ok);
}

TEST(ServerVersionCatalogTest, SkipsEntriesWithoutVersion)
{
    QJsonObject noVersion;
    noVersion[QStringLiteral("changelog")] = QStringLiteral("orphan");
    const auto result = ServerVersionCatalog::parse(versionsResponse(
        {noVersion,
         versionObject(QStringLiteral("1.2.0"),
                       QStringLiteral("envelope"))}),
        QStringLiteral("harken"),
        VNH::ServerEndpoints::hostChannel());
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.entries.size(), 1u);
    EXPECT_EQ(result.entries[0].version, QStringLiteral("1.2.0"));
}

TEST(ServerVersionCatalogTest, SkipsStorageProbeWithoutReleaseManifest)
{
    QJsonObject probe = versionObject(
        QStringLiteral("anon-upload-check-f74bfa62aaf64cc9aa0a29652f4ad748"),
        QString());
    probe[QStringLiteral("created_at")] = QString();
    probe[QStringLiteral("changelog")] = QString();

    const auto result = ServerVersionCatalog::parse(versionsResponse(
        {versionObject(QStringLiteral("1.4.0"), QStringLiteral("envelope")),
         probe}), QStringLiteral("harken"),
        VNH::ServerEndpoints::hostChannel());

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_EQ(result.entries.size(), 1u);
    EXPECT_EQ(result.entries.front().version, QStringLiteral("1.4.0"));
}

TEST(ServerVersionCatalogTest, MatchesEnvelopeByBuildId)
{
    const TestKeypair keys = makeKeypair();
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("expiresAt")] = QStringLiteral("2026-01-01T00:00:00Z");
    const QByteArray envelope = signEnvelope(
        QJsonDocument(payload).toJson(QJsonDocument::Compact), keys);

    const auto parsed = ServerVersionCatalog::parse(versionsResponse(
        {versionObject(QStringLiteral("1.4.0"),
                       QString::fromUtf8(envelope))}),
        QStringLiteral("harken"),
        VNH::ServerEndpoints::hostChannel());
    ASSERT_TRUE(parsed.ok);

    EXPECT_EQ(ServerVersionCatalog::matchEnvelopeForBuild(
                  parsed.entries, keys.publicKey,
                  QStringLiteral("run12.abcd1234"), kNow),
              envelope);
    EXPECT_TRUE(ServerVersionCatalog::matchEnvelopeForBuild(
                    parsed.entries, keys.publicKey,
                    QStringLiteral("run99.ffffffff"), kNow)
                    .isEmpty());
    EXPECT_TRUE(ServerVersionCatalog::matchEnvelopeForBuild(
                    parsed.entries, makeKeypair(0x24).publicKey,
                    QStringLiteral("run12.abcd1234"), kNow)
                    .isEmpty());
}
