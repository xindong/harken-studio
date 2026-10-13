#include "Transactions/InstallRequest.h"

#include "Packaging/PackageVerifier.h"
#include "Security/ManifestVerifier.h"
#include "Transactions/HandoffArea.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace VNH {

namespace {

constexpr qint64 kManifestMaxBytes = 1024 * 1024;

}

QString InstallRequest::manifestPathForPackage(const QString& packagePath)
{
    const QString sibling = packagePath + QStringLiteral(".manifest");
    if (QFileInfo::exists(sibling))
        return sibling;
    const QFileInfo info(packagePath);
    const QString swapped = info.dir().filePath(info.completeBaseName() +
                                                QStringLiteral(".manifest"));
    if (QFileInfo::exists(swapped))
        return swapped;
    return QString();
}

InstallPreparation InstallRequest::prepare(const QString& packagePath,
                                           const QString& manifestEnvelopePath,
                                           const QByteArray& manifestPublicKey,
                                           const QDateTime& now,
                                           bool allowExpired)
{
    InstallPreparation preparation;

    const QFileInfo manifestInfo(manifestEnvelopePath);
    if (!manifestInfo.isFile() || manifestInfo.size() <= 0 ||
        manifestInfo.size() > kManifestMaxBytes) {
        preparation.error =
            QStringLiteral("Release manifest not found next to the package: %1")
                .arg(manifestEnvelopePath);
        return preparation;
    }

    QFile manifestFile(manifestEnvelopePath);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        preparation.error = QStringLiteral("Cannot read the release manifest: %1")
                                .arg(manifestEnvelopePath);
        return preparation;
    }
    const QByteArray envelope = manifestFile.readAll();

    const ManifestVerifyResult manifestResult =
        ManifestVerifier::verify(envelope, manifestPublicKey, now,
                                 allowExpired);
    if (!manifestResult.ok) {
        preparation.error = manifestResult.error;
        return preparation;
    }
    const ReleaseManifest manifest = manifestResult.manifest;

    if (manifest.product != QStringLiteral("HarkenStudio")) {
        preparation.error =
            QStringLiteral("Manifest is for product: %1").arg(manifest.product);
        return preparation;
    }

    ExpectedPackage expected;
    expected.buildId = manifest.buildId;
    expected.semanticVersion = manifest.version;
    expected.sha256Hex = manifest.sha256Hex;
    expected.sizeBytes = manifest.packageSize;
    const PackageVerifyResult packageResult =
        PackageVerifier::verify(packagePath, expected);
    if (!packageResult.ok) {
        preparation.error = packageResult.error;
        return preparation;
    }
    if (packageResult.channel != manifest.channel) {
        preparation.error =
            QStringLiteral("Package channel %1 disagrees with the manifest "
                           "channel %2")
                .arg(packageResult.channel, manifest.channel);
        return preparation;
    }

    const HandoffPlacement placement = HandoffArea::place(packagePath, envelope);
    if (!placement.ok) {
        preparation.error = placement.error;
        return preparation;
    }

    preparation.ok = true;
    preparation.transactionId = placement.transactionId;
    preparation.version = manifest.version;
    preparation.buildId = manifest.buildId;
    return preparation;
}

}
