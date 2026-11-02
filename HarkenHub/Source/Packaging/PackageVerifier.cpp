#include "Packaging/PackageVerifier.h"

#include "Packaging/PackageReader.h"
#include "Packaging/PackageInventory.h"
#include "Platform/PlatformPaths.h"
#include "Versions/StudioCapabilities.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace VNH {

namespace {

const QString kPackageManifestName = QStringLiteral("package-manifest.json");
const QString kCapabilitiesName = QStringLiteral("capabilities.json");

QString hostPlatformError(const QSet<QString>& filePaths)
{
    if (filePaths.contains(studioBinaryRelativePath()))
        return {};
#ifdef Q_OS_WIN
    if (filePaths.contains(QStringLiteral("HarkenStudio.app/Contents/MacOS/HarkenStudio")))
        return QCoreApplication::translate("HarkenHub",
            "This package is for macOS and cannot be installed on Windows. Select a Windows package.");
#else
    if (filePaths.contains(QStringLiteral("HarkenStudio.exe")))
        return QCoreApplication::translate("HarkenHub",
            "This package is for Windows and cannot be installed on macOS. Select a macOS package.");
#endif
    return QCoreApplication::translate("HarkenHub", "The package is missing the Studio program required for this system:\n%1")
        .arg(studioBinaryRelativePath());
}

bool parseJsonObject(const QByteArray& data, QJsonObject* out, QString* error,
                     const QString& label)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("Invalid %1: %2").arg(label, parseError.errorString());
        return false;
    }
    *out = doc.object();
    return true;
}

}

bool PackageVerifier::checkHostPlatform(const QString& archivePath, QString* error)
{
    const auto listing = PackageReader::listEntries(archivePath);
    if (!listing.ok) {
        *error = listing.error;
        return false;
    }
    QSet<QString> filePaths;
    for (const auto& entry : listing.entries) {
        if (!entry.isDirectory)
            filePaths.insert(entry.path);
    }
    *error = hostPlatformError(filePaths);
    return error->isEmpty();
}

PackageVerifyResult PackageVerifier::verify(const QString& archivePath,
                                            const ExpectedPackage& expected)
{
    PackageVerifyResult result;

    if (expected.buildId.isEmpty() || expected.semanticVersion.isEmpty() ||
        expected.sha256Hex.isEmpty() || expected.sizeBytes < 0) {
        result.error = QStringLiteral("Expected package identity is incomplete");
        return result;
    }

    const QFileInfo info(archivePath);
    if (!info.isFile()) {
        result.error = QStringLiteral("Package file not found: %1").arg(archivePath);
        return result;
    }
    if (info.size() != expected.sizeBytes) {
        result.error = QStringLiteral("Package size mismatch: expected %1, got %2")
                           .arg(expected.sizeBytes)
                           .arg(info.size());
        return result;
    }

    const QString actual = PackageReader::fileSha256Hex(archivePath);
    if (actual.compare(expected.sha256Hex, Qt::CaseInsensitive) != 0) {
        result.error = QStringLiteral("Package SHA-256 mismatch");
        return result;
    }

    const PackageListResult listing = PackageReader::listEntries(archivePath);
    if (!listing.ok) {
        result.error = listing.error;
        return result;
    }

    QSet<QString> filePaths;
    QHash<QString, qint64> entrySizeByPath;
    for (const auto& entry : listing.entries) {
        if (!entry.isDirectory) {
            filePaths.insert(entry.path);
            entrySizeByPath.insert(entry.path, entry.uncompressedSize);
        }
    }

    if (!filePaths.contains(kPackageManifestName)) {
        result.error = QStringLiteral("Package is missing %1").arg(kPackageManifestName);
        return result;
    }
    if (!filePaths.contains(kCapabilitiesName)) {
        result.error = QStringLiteral("Package is missing %1").arg(kCapabilitiesName);
        return result;
    }
    result.error = hostPlatformError(filePaths);
    if (!result.error.isEmpty())
        return result;

    QByteArray manifestData;
    QString readError;
    if (!PackageReader::readEntry(archivePath, kPackageManifestName, &manifestData,
                                  &readError)) {
        result.error = readError;
        return result;
    }
    const auto inventory = PackageInventory::parse(manifestData, expected.semanticVersion, expected.buildId);
    if (!inventory.ok) {
        result.error = inventory.error;
        return result;
    }
    QSet<QString> manifestPaths;
    QHash<QString, QString> fileSha256ByPath;
    for (auto entry = inventory.files.cbegin(); entry != inventory.files.cend(); ++entry) {
        if (entrySizeByPath.contains(entry.key()) && entrySizeByPath.value(entry.key()) != entry->size) {
            result.error = QStringLiteral("Manifest size disagrees with the archive for: %1").arg(entry.key());
            return result;
        }
        manifestPaths.insert(entry.key());
        fileSha256ByPath.insert(entry.key(), entry->sha256);
    }

    QSet<QString> payloadPaths = filePaths;
    payloadPaths.remove(kPackageManifestName);
    payloadPaths.remove(kCapabilitiesName);
    if (payloadPaths != manifestPaths) {
        const QSet<QString> missing = manifestPaths - payloadPaths;
        const QSet<QString> extra = payloadPaths - manifestPaths;
        result.error = QStringLiteral(
                           "Manifest file set mismatch: %1 missing, %2 unlisted")
                           .arg(missing.size())
                           .arg(extra.size());
        return result;
    }

    QByteArray capabilitiesData;
    if (!PackageReader::readEntry(archivePath, kCapabilitiesName, &capabilitiesData,
                                  &readError)) {
        result.error = readError;
        return result;
    }
    QJsonObject capabilities;
    if (!parseJsonObject(capabilitiesData, &capabilities, &result.error,
                         kCapabilitiesName))
        return result;

    if (!validateStudioCapabilities(capabilities, expected.semanticVersion,
                                    expected.buildId, &result.error,
                                    &result.capabilitiesVersion))
        return result;
    result.channel = inventory.channel;
    result.fileSha256ByPath = fileSha256ByPath;
    result.ok = true;
    return result;
}

}
