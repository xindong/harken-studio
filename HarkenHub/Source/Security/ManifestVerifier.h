#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace VNH {

struct ReleaseManifest
{
    QString product;
    QString version;
    QString buildId;
    QString channel;
    QString platform;
    QString architecture;
    QString sha256Hex;
    qint64 packageSize = -1;
    qint64 releaseSequence = -1;
    QDateTime publishedAt;
    QDateTime expiresAt;
};

struct ManifestVerifyResult
{
    bool ok = false;
    QString error;
    ReleaseManifest manifest;
};

class ManifestVerifier
{
public:
    static ManifestVerifyResult verify(const QByteArray& envelopeJson,
                                       const QByteArray& publicKey,
                                       const QDateTime& now,
                                       bool allowExpired = false);
    static QString hostPlatform();
    static QString hostArchitecture();
    static bool isSafeFileToken(const QString& value);
};

}
