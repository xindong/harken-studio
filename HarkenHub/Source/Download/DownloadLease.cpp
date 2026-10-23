#include "Download/DownloadLease.h"

#include "Platform/PlatformPaths.h"

#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>

namespace VNH {

DownloadLease::DownloadLease(const QString& lockPath)
    : lockPath_(lockPath), lock_(lockPath)
{
    lock_.setStaleLockTime(0);
}

QString DownloadLease::lockPathFor(const QString& path)
{
    QString artifact = path;
    for (const auto& suffix : {QStringLiteral(".part.meta"),
                                QStringLiteral(".part"),
                                QStringLiteral(".stale")}) {
        if (artifact.endsWith(suffix)) {
            artifact.chop(suffix.size());
            break;
        }
    }
    const QFileInfo info(artifact);
    QString stem = info.completeBaseName();
    QString suffix = info.suffix();
    if (info.suffix() == QStringLiteral("manifest") &&
        (stem.endsWith(QStringLiteral(".zip")) ||
         stem.endsWith(QStringLiteral(".exe")) ||
         stem.endsWith(QStringLiteral(".msi")) ||
         stem.endsWith(QStringLiteral(".pkg")))) {
        const QFileInfo package(stem);
        suffix = package.suffix();
        stem = package.completeBaseName();
    }
    if ((suffix == QStringLiteral("exe") || suffix == QStringLiteral("msi") ||
         suffix == QStringLiteral("pkg")) &&
        stem.startsWith(QStringLiteral("HarkenHubSetup-")))
        stem.remove(0, QStringLiteral("HarkenHubSetup-").size());
    const QString directory = info.dir().canonicalPath();
    return QDir(directory.isEmpty() ? info.absolutePath() : directory)
        .filePath(stem + QStringLiteral(".download.lock"));
}

std::shared_ptr<DownloadLease> DownloadLease::acquire(const QString& path,
                                                     QString* error)
{
    if (error)
        error->clear();
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = QCoreApplication::translate("HarkenHub", "Cannot create the download directory");
        return {};
    }
    auto lease = std::shared_ptr<DownloadLease>(new DownloadLease(lockPathFor(path)));
    if (!lease->lock_.tryLock(0)) {
        if (error)
            *error = lease->lock_.error() == QLockFile::LockFailedError
                         ? QCoreApplication::translate("HarkenHub", "This package is in use by another operation")
                         : QCoreApplication::translate("HarkenHub", "Cannot lock the download cache");
        return {};
    }
    return lease;
}

bool DownloadLease::covers(const QString& path) const
{
    return lockPath_ == lockPathFor(path);
}

bool DownloadLease::isCachePath(const QString& path)
{
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (const auto& directory : {QStringLiteral("/downloads/"),
                                  QStringLiteral("/hub-updates/")}) {
        const QString root = QDir::cleanPath(userDataDir() + directory) + QLatin1Char('/');
#if defined(Q_OS_WIN)
        if (absolute.startsWith(root, Qt::CaseInsensitive))
#else
        if (absolute.startsWith(root))
#endif
            return true;
    }
    return false;
}

}
