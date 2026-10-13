#include "Transactions/InstallPackageCache.h"

#include "Platform/PlatformPaths.h"
#include "Security/ManifestVerifier.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>

namespace VNH {

namespace {

QString cacheDir(const QString& buildId)
{
    return machineDataDir() + QStringLiteral("/package-cache/") + buildId;
}

bool copyAtomically(const QString& sourcePath, const QString& targetPath,
                    QString* error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read verified package cache source");
        return false;
    }
    QSaveFile target(targetPath);
    if (!target.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Cannot create verified package cache");
        return false;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    qint64 count = 0;
    while ((count = source.read(buffer.data(), buffer.size())) > 0) {
        if (target.write(buffer.constData(), count) != count) {
            *error = QStringLiteral("Cannot write verified package cache");
            return false;
        }
    }
    if (count < 0 || !target.commit()) {
        *error = QStringLiteral("Cannot commit verified package cache");
        return false;
    }
    return true;
}

}

QString InstallPackageCache::packagePath(const QString& buildId)
{
    return cacheDir(buildId) + QStringLiteral("/package.pkg");
}

QString InstallPackageCache::manifestPath(const QString& buildId)
{
    return cacheDir(buildId) + QStringLiteral("/release.manifest");
}

bool InstallPackageCache::store(const QString& buildId,
                                const QString& packageSource,
                                const QString& manifestSource, QString* error)
{
    if (!ManifestVerifier::isSafeFileToken(buildId)) {
        *error = QStringLiteral("Cannot cache package with an invalid build id");
        return false;
    }
    const QString dir = cacheDir(buildId);
    if (!QDir().mkpath(dir)) {
        *error = QStringLiteral("Cannot create verified package cache directory");
        return false;
    }
    if (!copyAtomically(packageSource, packagePath(buildId), error) ||
        !copyAtomically(manifestSource, manifestPath(buildId), error)) {
        QDir(dir).removeRecursively();
        return false;
    }
    return true;
}

bool InstallPackageCache::remove(const QString& buildId)
{
    if (!ManifestVerifier::isSafeFileToken(buildId))
        return false;
    const QDir dir(cacheDir(buildId));
    return !dir.exists() || QDir(dir).removeRecursively();
}

}
