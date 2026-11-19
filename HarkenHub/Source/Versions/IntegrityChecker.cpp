#include "Versions/IntegrityChecker.h"

#include "Packaging/PackageReader.h"

#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace VNH {

InstallationSnapshot IntegrityChecker::snapshot(const QString& installPath)
{
    InstallationSnapshot result;
    const QFileInfo root(installPath);
    QDirIterator iterator(installPath, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const auto info = iterator.fileInfo();
        QJsonObject entry;
        if (info.isSymbolicLink() || info.isJunction()) entry["link"] = info.symLinkTarget();
        else if (info.isDir()) entry["directory"] = true;
        else {
            const auto hash = PackageReader::fileSha256Hex(info.absoluteFilePath());
            if (hash.isEmpty()) {
                result.error = QStringLiteral("Cannot read installed file: %1").arg(info.absoluteFilePath());
                return result;
            }
            entry["sha256"] = hash;
        }
        result.files[QDir(installPath).relativeFilePath(info.absoluteFilePath())] = entry;
    }
    const QJsonObject identity{{"path", root.absoluteFilePath()}, {"exists", root.exists()},
        {"link", root.symLinkTarget()}, {"files", result.files}};
    result.hash = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    return result;
}

IntegrityReport IntegrityChecker::check(
    const QString& installPath, const QString& expectedPackageManifestHash)
{
    IntegrityReport report;

    const QFileInfo dirInfo(installPath);
    if (!dirInfo.exists() || !dirInfo.isDir()) {
        report.error =
            QStringLiteral("Install directory is missing: %1").arg(installPath);
        return report;
    }

    QFile manifestFile(installPath + QStringLiteral("/package-manifest.json"));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        report.error = QStringLiteral("Cannot read package-manifest.json");
        return report;
    }
    if (!expectedPackageManifestHash.isEmpty() &&
        PackageReader::fileSha256Hex(manifestFile.fileName()).toLower() !=
            expectedPackageManifestHash.toLower()) {
        report.mismatchedFiles.append(QStringLiteral("package-manifest.json"));
        report.error = QStringLiteral("Installed package metadata was modified");
        return report;
    }
    QJsonParseError parseError;
    const QJsonDocument doc =
        QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        report.error = QStringLiteral("package-manifest.json is corrupt: %1")
                           .arg(parseError.errorString());
        return report;
    }

    const QJsonArray files = doc.object()[QStringLiteral("files")].toArray();
    if (files.isEmpty()) {
        report.error =
            QStringLiteral("package-manifest.json lists no payload files");
        return report;
    }

    for (const auto& value : files) {
        const QJsonObject entry = value.toObject();
        const QString relativePath = entry[QStringLiteral("path")].toString();
        const qint64 size = entry[QStringLiteral("size")].toInteger(-1);
        const QString sha256 =
            entry[QStringLiteral("sha256")].toString().toLower();
        if (relativePath.isEmpty() || sha256.isEmpty()) {
            report.error = QStringLiteral(
                "package-manifest.json contains an invalid file record");
            return report;
        }
        QString pathError;
        if (!PackageReader::isSafeEntryPath(relativePath, &pathError)) {
            report.error = pathError;
            return report;
        }
        const QString absolutePath =
            installPath + QStringLiteral("/") + relativePath;
        const QFileInfo info(absolutePath);
        if (!info.exists() || !info.isFile()) {
            report.missingFiles.append(relativePath);
            continue;
        }
        if (info.size() != size ||
            PackageReader::fileSha256Hex(absolutePath).toLower() != sha256)
            report.mismatchedFiles.append(relativePath);
    }

    if (!report.missingFiles.isEmpty() || !report.mismatchedFiles.isEmpty()) {
        report.error = QStringLiteral("%1 missing and %2 mismatched files")
                           .arg(report.missingFiles.size())
                           .arg(report.mismatchedFiles.size());
        return report;
    }

    report.ok = true;
    return report;
}

}
