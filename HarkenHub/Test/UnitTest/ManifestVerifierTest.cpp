#include "Security/ManifestVerifier.h"

#include "Support/ManifestTestUtils.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

using VNH::ManifestVerifier;
using HarkenHubTest::baseManifestPayload;
using HarkenHubTest::makeKeypair;
using HarkenHubTest::signEnvelope;
using HarkenHubTest::TestKeypair;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-23T12:00:00Z"), Qt::ISODate);

QByteArray payloadBytes(const QJsonObject& payload)
{
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

}

TEST(ManifestVerifier, SignedManifestRoundTripVerifies)
{
    const TestKeypair keys = makeKeypair();
    const auto envelope = signEnvelope(payloadBytes(baseManifestPayload()), keys);

    const auto result = ManifestVerifier::verify(envelope, keys.publicKey, kNow);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.manifest.product, QStringLiteral("HarkenStudio"));
    EXPECT_EQ(result.manifest.version, QStringLiteral("1.4.0"));
    EXPECT_EQ(result.manifest.buildId, QStringLiteral("run12.abcd1234"));
    EXPECT_EQ(result.manifest.channel, QStringLiteral("stable"));
    EXPECT_EQ(result.manifest.packageSize, 13000);
    EXPECT_EQ(result.manifest.releaseSequence, 7);
    EXPECT_EQ(result.manifest.sha256Hex, QString(64, QLatin1Char('a')));
    EXPECT_TRUE(result.manifest.expiresAt > kNow);
}

TEST(ManifestVerifier, TamperedPayloadFails)
{
    const TestKeypair keys = makeKeypair();
    const auto envelope = signEnvelope(payloadBytes(baseManifestPayload()), keys);

    QJsonObject parsed = QJsonDocument::fromJson(envelope).object();
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("version")] = QStringLiteral("9.9.9");
    parsed[QStringLiteral("payloadBase64")] =
        QString::fromLatin1(payloadBytes(payload).toBase64());
    const auto tampered = QJsonDocument(parsed).toJson(QJsonDocument::Compact);

    const auto result = ManifestVerifier::verify(tampered, keys.publicKey, kNow);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("signature")));
}

TEST(ManifestVerifier, WrongPublicKeyFails)
{
    const auto envelope =
        signEnvelope(payloadBytes(baseManifestPayload()), makeKeypair(0x42));
    const TestKeypair other = makeKeypair(0x24);

    const auto result = ManifestVerifier::verify(envelope, other.publicKey, kNow);
    EXPECT_FALSE(result.ok);
}

TEST(ManifestVerifier, ExpiredManifestFails)
{
    const TestKeypair keys = makeKeypair();
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("expiresAt")] = QStringLiteral("2026-07-23T11:59:00Z");
    const auto envelope = signEnvelope(payloadBytes(payload), keys);

    const auto result = ManifestVerifier::verify(envelope, keys.publicKey, kNow);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("expired")));
}

TEST(ManifestVerifier, AllowExpiredAcceptsExpiredManifest)
{
    const TestKeypair keys = makeKeypair();
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("expiresAt")] = QStringLiteral("2026-07-23T11:59:00Z");
    const auto envelope = signEnvelope(payloadBytes(payload), keys);

    const auto result =
        ManifestVerifier::verify(envelope, keys.publicKey, kNow, true);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.manifest.buildId, QStringLiteral("run12.abcd1234"));
}

TEST(ManifestVerifier, AllowExpiredStillRejectsBadSignature)
{
    const auto envelope =
        signEnvelope(payloadBytes(baseManifestPayload()), makeKeypair(0x42));
    const TestKeypair other = makeKeypair(0x24);

    EXPECT_FALSE(
        ManifestVerifier::verify(envelope, other.publicKey, kNow, true).ok);
}

TEST(ManifestVerifier, MissingRequiredFieldFails)
{
    const TestKeypair keys = makeKeypair();
    QJsonObject payload = baseManifestPayload();
    payload.remove(QStringLiteral("sha256"));
    const auto envelope = signEnvelope(payloadBytes(payload), keys);

    const auto result = ManifestVerifier::verify(envelope, keys.publicKey, kNow);
    EXPECT_FALSE(result.ok);
}

TEST(ManifestVerifier, RejectsUnsafeInstallIdentity)
{
    const TestKeypair keys = makeKeypair();
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("version")] = QStringLiteral("../outside");
    const auto envelope = signEnvelope(payloadBytes(payload), keys);

    const auto result = ManifestVerifier::verify(envelope, keys.publicKey, kNow);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("unsafe identity")));
}

TEST(ManifestVerifier, MalformedEnvelopeFails)
{
    const TestKeypair keys = makeKeypair();

    EXPECT_FALSE(
        ManifestVerifier::verify(QByteArrayLiteral("not json"), keys.publicKey, kNow)
            .ok);

    QJsonObject envelope;
    envelope[QStringLiteral("algorithm")] = QStringLiteral("ed25519");
    envelope[QStringLiteral("payloadBase64")] = QStringLiteral("@@@");
    envelope[QStringLiteral("signatureBase64")] = QStringLiteral("@@@");
    EXPECT_FALSE(ManifestVerifier::verify(
                     QJsonDocument(envelope).toJson(QJsonDocument::Compact),
                     keys.publicKey, kNow)
                     .ok);

    envelope[QStringLiteral("algorithm")] = QStringLiteral("none");
    EXPECT_FALSE(ManifestVerifier::verify(
                     QJsonDocument(envelope).toJson(QJsonDocument::Compact),
                     keys.publicKey, kNow)
                     .ok);
}

TEST(ManifestVerifier, MissingTrustAnchorFails)
{
    const auto envelope =
        signEnvelope(payloadBytes(baseManifestPayload()), makeKeypair());

    const auto result = ManifestVerifier::verify(envelope, QByteArray(), kNow);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("trust anchor")));
}

TEST(ManifestVerifier, RejectsForeignPlatform)
{
    const TestKeypair keys = makeKeypair();
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("platform")] =
        ManifestVerifier::hostPlatform() == QStringLiteral("windows")
            ? QStringLiteral("mac")
            : QStringLiteral("windows");
    const auto envelope = signEnvelope(payloadBytes(payload), keys);

    const auto result = ManifestVerifier::verify(envelope, keys.publicKey, kNow);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("platform mismatch")));
}

TEST(ManifestVerifier, RejectsForeignArchitecture)
{
    const TestKeypair keys = makeKeypair();
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("architecture")] =
        ManifestVerifier::hostArchitecture() == QStringLiteral("x64")
            ? QStringLiteral("arm64")
            : QStringLiteral("x64");
    const auto envelope = signEnvelope(payloadBytes(payload), keys);

    const auto result = ManifestVerifier::verify(envelope, keys.publicKey, kNow);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(
        result.error.contains(QStringLiteral("architecture mismatch")));
}

TEST(ManifestVerifier, AcceptsUniversalArchitecture)
{
    const TestKeypair keys = makeKeypair();
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("architecture")] = QStringLiteral("universal");
    const auto envelope = signEnvelope(payloadBytes(payload), keys);

    EXPECT_TRUE(
        ManifestVerifier::verify(envelope, keys.publicKey, kNow).ok);
}

TEST(ManifestVerifier, AcceptsSynonymousArchitectureToken)
{
    const TestKeypair keys = makeKeypair();
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("architecture")] =
        ManifestVerifier::hostArchitecture() == QStringLiteral("x64")
            ? QStringLiteral("x86_64")
            : QStringLiteral("aarch64");
    const auto envelope = signEnvelope(payloadBytes(payload), keys);

    EXPECT_TRUE(
        ManifestVerifier::verify(envelope, keys.publicKey, kNow).ok);
}

TEST(ManifestVerifier, RejectsMissingPlatformIdentity)
{
    const TestKeypair keys = makeKeypair();
    QJsonObject payload = baseManifestPayload();
    payload.remove(QStringLiteral("platform"));
    const auto envelope = signEnvelope(payloadBytes(payload), keys);

    const auto result = ManifestVerifier::verify(envelope, keys.publicKey, kNow);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(
        result.error.contains(QStringLiteral("platform identity")));
}

TEST(ManifestVerifier, PythonSignedVectorVerifies)
{
    const QByteArray publicKey = QByteArray::fromHex(
        "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8");
    QJsonObject envelope;
    envelope[QStringLiteral("algorithm")] = QStringLiteral("ed25519");
    envelope[QStringLiteral("payloadBase64")] = QStringLiteral(
        "ewogICJhcmNoaXRlY3R1cmUiOiAieDY0IiwKICAiYnVpbGRJZCI6ICJydW4xMi5hYmNkMTIzNCIsCiAgImNoYW5uZWwiOiAic3RhYmxlIiwKICAiZXhwaXJlc0F0IjogIjIwMzAtMDEtMDFUMDA6MDA6MDBaIiwKICAicGFja2FnZVNpemUiOiAxMzAwMCwKICAicGFja2FnZVVybCI6ICJodHRwczovL3JlbGVhc2VzLmV4YW1wbGUvc3R1ZGlvLnppcCIsCiAgInBsYXRmb3JtIjogIndpbmRvd3MiLAogICJwcm9kdWN0IjogIlZlbnVzU3R1ZGlvIiwKICAicHVibGlzaGVkQXQiOiAiMjAyNi0wNy0yM1QwMDowMDowMFoiLAogICJyZWxlYXNlU2VxdWVuY2UiOiA3LAogICJzaGEyNTYiOiAiYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYSIsCiAgInZlcnNpb24iOiAiMS40LjAiCn0=");
    envelope[QStringLiteral("signatureBase64")] = QStringLiteral(
        "gqwD1xyGQ/GZ3YDdUyPjEmDrVryGoM+2sGk/emtauwXwgOYmDJAHXsf+2vno1mmrpR6uK8GHVFnSgTcLxfi+Dw==");

    const auto result = ManifestVerifier::verify(
        QJsonDocument(envelope).toJson(QJsonDocument::Compact), publicKey, kNow);
#if defined(Q_OS_WIN)
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.manifest.buildId, QStringLiteral("run12.abcd1234"));
    EXPECT_EQ(result.manifest.releaseSequence, 7);
#else
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("platform mismatch")))
        << result.error.toStdString();
#endif
}
