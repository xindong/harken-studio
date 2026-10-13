#include "Transactions/HandoffArea.h"

#include "Platform/PlatformPaths.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QRegularExpression>

namespace VNH {

namespace {

bool copyFileTo(const QString& sourcePath, const QString& destPath, QString* error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read package: %1").arg(sourcePath);
        return false;
    }

    QFile dest(destPath);
    if (!dest.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        *error = QStringLiteral("Handoff target already exists or is not writable: %1")
                     .arg(destPath);
        return false;
    }

    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    qint64 read = 0;
    while ((read = source.read(buffer.data(), buffer.size())) > 0) {
        if (dest.write(buffer.constData(), read) != read) {
            dest.close();
            QFile::remove(destPath);
            *error = QStringLiteral("Write failed: %1").arg(destPath);
            return false;
        }
    }
    if (read < 0) {
        dest.close();
        QFile::remove(destPath);
        *error = QStringLiteral("Read failed: %1").arg(sourcePath);
        return false;
    }
    return true;
}

}

QString HandoffArea::rootDir()
{
    return machineDataDir() + QStringLiteral("/handoff");
}

QString HandoffArea::packagePath(const QString& transactionId)
{
    return rootDir() + QStringLiteral("/") + transactionId + QStringLiteral(".pkg");
}

QString HandoffArea::manifestPath(const QString& transactionId)
{
    return rootDir() + QStringLiteral("/") + transactionId +
           QStringLiteral(".manifest");
}

QString HandoffArea::queuePath(const QString& transactionId)
{
    return rootDir() + QStringLiteral("/") + transactionId +
           QStringLiteral(".queue");
}

QString HandoffArea::newTransactionId()
{
    QByteArray bytes(16, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32*>(bytes.data()),
        bytes.size() / static_cast<int>(sizeof(quint32)));
    return QString::fromLatin1(bytes.toHex());
}

bool HandoffArea::isValidTransactionId(const QString& transactionId)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(transactionId).hasMatch();
}

HandoffPlacement HandoffArea::place(const QString& packageFilePath,
                                    const QByteArray& releaseManifestJson)
{
    return placeWithId(newTransactionId(), packageFilePath, releaseManifestJson);
}

HandoffPlacement HandoffArea::placeWithId(const QString& transactionId,
                                          const QString& packageFilePath,
                                          const QByteArray& releaseManifestJson)
{
    HandoffPlacement placement;

    if (!isValidTransactionId(transactionId)) {
        placement.error = QStringLiteral("Invalid transaction id");
        return placement;
    }
    if (!QDir().mkpath(rootDir())) {
        placement.error =
            QStringLiteral("Cannot create handoff directory: %1").arg(rootDir());
        return placement;
    }

    const QString packagePath = HandoffArea::packagePath(transactionId);
    const QString manifestPath = HandoffArea::manifestPath(transactionId);

    if (!copyFileTo(packageFilePath, packagePath, &placement.error))
        return placement;

    QFile manifest(manifestPath);
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        QFile::remove(packagePath);
        placement.error =
            QStringLiteral("Handoff target already exists or is not writable: %1")
                .arg(manifestPath);
        return placement;
    }
    if (manifest.write(releaseManifestJson) != releaseManifestJson.size()) {
        manifest.close();
        QFile::remove(manifestPath);
        QFile::remove(packagePath);
        placement.error = QStringLiteral("Write failed: %1").arg(manifestPath);
        return placement;
    }
    manifest.close();

    placement.ok = true;
    placement.transactionId = transactionId;
    placement.packagePath = packagePath;
    placement.manifestPath = manifestPath;
    return placement;
}

bool HandoffArea::discard(const QString& transactionId)
{
    if (!isValidTransactionId(transactionId))
        return false;

    auto removeIfExists = [](const QString& path) {
        return !QFile::exists(path) || QFile::remove(path);
    };
    const bool packageGone = removeIfExists(packagePath(transactionId));
    const bool manifestGone = removeIfExists(manifestPath(transactionId));
    const bool queueGone = removeIfExists(queuePath(transactionId));
    return packageGone && manifestGone && queueGone;
}

}
