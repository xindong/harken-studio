#pragma once

#include "Security/ManifestVerifier.h"
#include "Update/ServerEndpoints.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

#include <ed25519.h>

namespace HarkenHubTest {

struct TestKeypair
{
    QByteArray publicKey;
    QByteArray privateKey;
};

inline TestKeypair makeKeypair(char seedByte = 0x42)
{
    const QByteArray seed(32, seedByte);
    TestKeypair keys;
    keys.publicKey.resize(32);
    keys.privateKey.resize(64);
    ed25519_create_keypair(
        reinterpret_cast<unsigned char*>(keys.publicKey.data()),
        reinterpret_cast<unsigned char*>(keys.privateKey.data()),
        reinterpret_cast<const unsigned char*>(seed.constData()));
    return keys;
}

inline QByteArray signEnvelope(const QByteArray& payload, const TestKeypair& keys)
{
    QByteArray signature(64, Qt::Uninitialized);
    ed25519_sign(reinterpret_cast<unsigned char*>(signature.data()),
                 reinterpret_cast<const unsigned char*>(payload.constData()),
                 static_cast<size_t>(payload.size()),
                 reinterpret_cast<const unsigned char*>(keys.publicKey.constData()),
                 reinterpret_cast<const unsigned char*>(keys.privateKey.constData()));

    QJsonObject envelope;
    envelope[QStringLiteral("algorithm")] = QStringLiteral("ed25519");
    envelope[QStringLiteral("payloadBase64")] =
        QString::fromLatin1(payload.toBase64());
    envelope[QStringLiteral("signatureBase64")] =
        QString::fromLatin1(signature.toBase64());
    return QJsonDocument(envelope).toJson(QJsonDocument::Compact);
}

inline QJsonObject baseManifestPayload()
{
    QJsonObject payload;
    payload[QStringLiteral("product")] = QStringLiteral("HarkenStudio");
    payload[QStringLiteral("version")] = QStringLiteral("1.4.0");
    payload[QStringLiteral("buildId")] = QStringLiteral("run12.abcd1234");
    payload[QStringLiteral("channel")] = QStringLiteral("stable");
    payload[QStringLiteral("platform")] = VNH::ManifestVerifier::hostPlatform();
    payload[QStringLiteral("architecture")] =
        VNH::ManifestVerifier::hostArchitecture();
    payload[QStringLiteral("packageSize")] = 13000;
    payload[QStringLiteral("sha256")] = QString(64, QLatin1Char('a'));
    payload[QStringLiteral("releaseSequence")] = 7;
    payload[QStringLiteral("publishedAt")] = QStringLiteral("2026-07-23T00:00:00Z");
    payload[QStringLiteral("expiresAt")] = QStringLiteral("2030-01-01T00:00:00Z");
    return payload;
}

inline QByteArray latestResponseJson(const QByteArray& envelope)
{
    QJsonObject integrity;
    if (!envelope.isEmpty())
        integrity = QJsonDocument::fromJson(envelope).object();
    QJsonObject entry;
    entry[QStringLiteral("version")] = QStringLiteral("1.4.0");
    entry[QStringLiteral("file")] =
        QStringLiteral("release/harken/%1/1.4.0/run12-abcd1234-20261008T100000Z/Harken.zip")
            .arg(VNH::ServerEndpoints::hostChannel());
    entry[QStringLiteral("integrity")] = integrity;
    QJsonArray versions;
    if (!envelope.isEmpty())
        versions.append(entry);
    QJsonObject channels;
    channels[VNH::ServerEndpoints::hostChannel()] = versions;
    QJsonObject response;
    response[QStringLiteral("schema_version")] = 1;
    response[QStringLiteral("channels")] = channels;
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

}
