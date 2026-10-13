#include "Security/TrustAnchors.h"

#include <QRegularExpression>
#include <QString>

namespace VNH {

QByteArray TrustAnchors::manifestPublicKey()
{
#if !defined(Q_OS_WIN) || defined(HARKEN_HUB_TEST_OVERRIDES)
    const QString overrideHex =
        qEnvironmentVariable("HARKEN_HUB_MANIFEST_PUBKEY");
    if (!overrideHex.isEmpty()) {
        static const QRegularExpression hexPattern(
            QStringLiteral("^[0-9a-fA-F]{64}$"));
        if (!hexPattern.match(overrideHex).hasMatch())
            return {};
        return QByteArray::fromHex(overrideHex.toLatin1());
    }
#endif
    return QByteArray::fromHex(QByteArrayLiteral(
        "bc2fea5095f03d2f9512407325a5c61c68b3e08901fd10db355891f052ac6f0f"));
}

}
