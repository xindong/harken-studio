#include "Security/ManifestVerifier.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSysInfo>

#include <ed25519.h>

namespace VNH {

namespace {

bool decodeBase64Field(const QJsonObject& envelope, const QString& key,
                       QByteArray* out)
{
    const QJsonValue value = envelope[key];
    if (!value.isString())
        return false;
    const auto decoded =
        QByteArray::fromBase64Encoding(value.toString().toLatin1(),
                                       QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return false;
    *out = *decoded;
    return !out->isEmpty();
}

bool requireString(const QJsonObject& object, const QString& key, QString* out)
{
    const QJsonValue value = object[key];
    if (!value.isString() || value.toString().isEmpty())
        return false;
    *out = value.toString();
    return true;
}

bool requireUtcTime(const QJsonObject& object, const QString& key, QDateTime* out)
{
    QString text;
    if (!requireString(object, key, &text))
        return false;
    const QDateTime parsed = QDateTime::fromString(text, Qt::ISODate);
    if (!parsed.isValid())
        return false;
    *out = parsed.toUTC();
    return true;
}

QString canonicalArchitecture(const QString& value)
{
    const QString lowered = value.toLower();
    if (lowered == QStringLiteral("x86_64") ||
        lowered == QStringLiteral("amd64"))
        return QStringLiteral("x64");
    if (lowered == QStringLiteral("aarch64"))
        return QStringLiteral("arm64");
    return lowered;
}

}

QString ManifestVerifier::hostPlatform()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("mac");
#else
    return QStringLiteral("unsupported");
#endif
}

QString ManifestVerifier::hostArchitecture()
{
    return canonicalArchitecture(QSysInfo::currentCpuArchitecture());
}

bool ManifestVerifier::isSafeFileToken(const QString& value)
{
    if (value.isEmpty() || value.size() > 128)
        return false;
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    return pattern.match(value).hasMatch() && value != QStringLiteral(".") &&
           value != QStringLiteral("..") &&
           !value.contains(QStringLiteral(".."));
}

ManifestVerifyResult ManifestVerifier::verify(const QByteArray& envelopeJson,
                                              const QByteArray& publicKey,
                                              const QDateTime& now,
                                              bool allowExpired)
{
    ManifestVerifyResult result;

    if (publicKey.size() != 32) {
        result.error = QStringLiteral("Manifest trust anchor is not configured");
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument envelopeDoc =
        QJsonDocument::fromJson(envelopeJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !envelopeDoc.isObject()) {
        result.error = QStringLiteral("Invalid manifest envelope: %1")
                           .arg(parseError.errorString());
        return result;
    }
    const QJsonObject envelope = envelopeDoc.object();

    if (envelope[QStringLiteral("algorithm")].toString() !=
        QStringLiteral("ed25519")) {
        result.error = QStringLiteral("Unsupported manifest signature algorithm");
        return result;
    }

    QByteArray payload;
    QByteArray signature;
    if (!decodeBase64Field(envelope, QStringLiteral("payloadBase64"), &payload) ||
        !decodeBase64Field(envelope, QStringLiteral("signatureBase64"),
                           &signature) ||
        signature.size() != 64) {
        result.error = QStringLiteral("Malformed manifest envelope");
        return result;
    }

    if (ed25519_verify(
            reinterpret_cast<const unsigned char*>(signature.constData()),
            reinterpret_cast<const unsigned char*>(payload.constData()),
            static_cast<size_t>(payload.size()),
            reinterpret_cast<const unsigned char*>(publicKey.constData())) != 1) {
        result.error = QStringLiteral("Manifest signature verification failed");
        return result;
    }

    const QJsonDocument payloadDoc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !payloadDoc.isObject()) {
        result.error = QStringLiteral("Invalid manifest payload: %1")
                           .arg(parseError.errorString());
        return result;
    }
    const QJsonObject payloadObject = payloadDoc.object();

    ReleaseManifest manifest;
    if (!requireString(payloadObject, QStringLiteral("product"),
                       &manifest.product) ||
        !requireString(payloadObject, QStringLiteral("version"),
                       &manifest.version) ||
        !requireString(payloadObject, QStringLiteral("buildId"),
                       &manifest.buildId) ||
        !requireString(payloadObject, QStringLiteral("channel"),
                       &manifest.channel) ||
        !requireString(payloadObject, QStringLiteral("sha256"),
                       &manifest.sha256Hex)) {
        result.error = QStringLiteral("Manifest is missing required fields");
        return result;
    }

    static const QRegularExpression sha256Pattern(
        QStringLiteral("^[0-9a-fA-F]{64}$"));
    if (!sha256Pattern.match(manifest.sha256Hex).hasMatch()) {
        result.error = QStringLiteral("Manifest sha256 is malformed");
        return result;
    }
    if (!isSafeFileToken(manifest.version) ||
        !isSafeFileToken(manifest.buildId) ||
        !isSafeFileToken(manifest.channel)) {
        result.error = QStringLiteral("Manifest contains an unsafe identity field");
        return result;
    }

    manifest.packageSize =
        payloadObject[QStringLiteral("packageSize")].toInteger(-1);
    if (manifest.packageSize <= 0) {
        result.error = QStringLiteral("Manifest packageSize is invalid");
        return result;
    }

    manifest.releaseSequence =
        payloadObject[QStringLiteral("releaseSequence")].toInteger(-1);
    if (manifest.releaseSequence < 0) {
        result.error = QStringLiteral("Manifest releaseSequence is invalid");
        return result;
    }

    if (!requireUtcTime(payloadObject, QStringLiteral("publishedAt"),
                        &manifest.publishedAt) ||
        !requireUtcTime(payloadObject, QStringLiteral("expiresAt"),
                        &manifest.expiresAt)) {
        result.error = QStringLiteral("Manifest timestamps are invalid");
        return result;
    }
    if (!allowExpired && manifest.expiresAt <= now.toUTC()) {
        result.error = QStringLiteral("Manifest has expired");
        return result;
    }

    manifest.platform = payloadObject[QStringLiteral("platform")].toString();
    manifest.architecture =
        payloadObject[QStringLiteral("architecture")].toString();
    if (manifest.platform.isEmpty() || manifest.architecture.isEmpty()) {
        result.error =
            QStringLiteral("Manifest platform identity is missing");
        return result;
    }
    if (manifest.platform.toLower() != hostPlatform()) {
        result.error =
            QStringLiteral("Manifest platform mismatch: expected %1, got %2")
                .arg(hostPlatform(), manifest.platform);
        return result;
    }
    const QString architecture = canonicalArchitecture(manifest.architecture);
    if (architecture != QStringLiteral("universal") &&
        architecture != hostArchitecture()) {
        result.error =
            QStringLiteral(
                "Manifest architecture mismatch: expected %1, got %2")
                .arg(hostArchitecture(), manifest.architecture);
        return result;
    }
    result.manifest = manifest;
    result.ok = true;
    return result;
}

}
