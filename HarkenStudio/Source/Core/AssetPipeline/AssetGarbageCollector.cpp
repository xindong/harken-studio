
#include "AssetGarbageCollector.h"
#include "AudioFormatCatalog.h"
#include "AssetRegistry.h"
#include "../ProjectPlatformActivity.h"
#include "PathUtils.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QDateTime>
#include <QDebug>
#include <algorithm>

namespace VNX {

namespace {

bool isCurrentMediaCacheFile(const QFileInfo& info)
{
    if (info.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) != 0)
        return false;
    if (!ProjectPlatformSemantics::parseId(info.dir().dirName())) return false;
    if (QFileInfo(info.absolutePath()).dir().dirName() == QStringLiteral("pack-derivatives"))
        return info.completeBaseName().size() == 64;
    const QStringList parts = info.completeBaseName().split(QLatin1Char('_'));
    bool ok = false;
    const uint32_t sourceID = parts.value(0).toUInt(&ok);
    return ok && sourceID != 0 && parts.size() == 2 && parts[1].size() == 64;
}

QPair<int, uint64_t> directoryFileStats(const QString& path)
{
    int count = 0;
    uint64_t size = 0;
    QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++count;
        size += static_cast<uint64_t>(it.fileInfo().size());
    }
    return qMakePair(count, size);
}

} // namespace


AssetGarbageCollector::AssetGarbageCollector(QObject* parent)
    : QObject(parent)
{
}

AssetGarbageCollector::~AssetGarbageCollector() = default;

void AssetGarbageCollector::setProjectRoot(const QString& root)
{
    projectRoot_ = root;
}


CleanupResult AssetGarbageCollector::cleanupLRU(int maxAgeDays, uint64_t maxCacheSizeBytes)
{
    CleanupResult result;
    if (ProjectPlatformActivity::instance().isBusy()) {
        result.errors.append(QStringLiteral("Media cache cleanup is unavailable during an active build or conversion"));
        return result;
    }


    emit cleanupProgress(0, tr("Cleaning legacy cache directory..."));

    const QString legacyCachePath = QDir(projectRoot_).absoluteFilePath(QStringLiteral("Cache"));
    QDir legacyCacheDir(legacyCachePath);
    if (legacyCacheDir.exists()) {
        const QPair<int, uint64_t> stats = directoryFileStats(legacyCachePath);
        if (legacyCacheDir.removeRecursively()) {
            result.deletedCount += stats.first;
            result.freedBytes += stats.second;
            result.deletedFiles.append(legacyCachePath);
            qDebug() << "Deleted legacy cache directory:" << legacyCachePath;
        } else {
            result.errors.append(tr("Failed to delete legacy cache directory: %1")
                                     .arg(legacyCachePath));
        }
    }

    emit cleanupProgress(5, tr("Cleaning stale cache files..."));

    const QList<QString> staleFiles = scanForStaleCaches();
    for (const QString& stalePath : staleFiles) {
        const uint64_t fileSize = deleteFile(stalePath);
        if (fileSize > 0) {
            result.deletedCount++;
            result.freedBytes += fileSize;
            result.deletedFiles.append(stalePath);
            qDebug() << "Deleted stale cache file:" << stalePath;
        }
    }

    emit cleanupProgress(10, tr("Collecting cache files..."));

    auto cacheFiles = collectCacheFiles();

    if (cacheFiles.isEmpty()) {
        emit cleanupComplete(result.deletedCount, result.freedBytes);
        return result;
    }

    emit cleanupProgress(20, tr("Sorting by access time..."));

    std::sort(cacheFiles.begin(), cacheFiles.end(),
              [](const QPair<QString, uint64_t>& a, const QPair<QString, uint64_t>& b) {
                  return a.second < b.second;
              });

    uint64_t totalSize = 0;
    for (const auto& file : cacheFiles) {
        totalSize += QFileInfo(file.first).size();
    }

    emit cleanupProgress(40, tr("Cleaning up old files..."));

    uint64_t now = QDateTime::currentSecsSinceEpoch();
    uint64_t maxAgeSeconds = static_cast<uint64_t>(maxAgeDays) * 86400;
    int processed = 0;

    for (const auto& file : cacheFiles) {
        bool shouldDelete = false;
        QString reason;

        if (now - file.second > maxAgeSeconds) {
            shouldDelete = true;
            reason = "exceeded max age";
        }

        if (maxCacheSizeBytes > 0 && totalSize > maxCacheSizeBytes) {
            shouldDelete = true;
            reason = "reducing cache size";
        }

        if (shouldDelete) {
            uint64_t fileSize = deleteFile(file.first);
            if (fileSize > 0) {
                result.deletedCount++;
                result.freedBytes += fileSize;
                result.deletedFiles.append(file.first);
                totalSize -= fileSize;
                qDebug() << "Deleted cache file (" << reason << "):" << file.first;
            }
        }

        processed++;
        int progress = 40 + (processed * 50 / cacheFiles.size());
        emit cleanupProgress(progress, tr("Processing %1 of %2...").arg(processed).arg(cacheFiles.size()));

        if (maxCacheSizeBytes > 0 && totalSize <= maxCacheSizeBytes &&
            (now - file.second <= maxAgeSeconds)) {
            break;
        }
    }

    emit cleanupProgress(100, tr("Cleanup complete"));
    emit cleanupComplete(result.deletedCount, result.freedBytes);

    return result;
}

uint64_t AssetGarbageCollector::getCacheSize() const
{
    QString cacheDir = PathUtils::getCacheDir(projectRoot_);
    uint64_t total = 0;

    QDirIterator it(cacheDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }

    return total;
}

AssetGarbageCollector::CacheStats AssetGarbageCollector::getCacheStats() const
{
    CacheStats stats;
    stats.oldestAccessTime = UINT64_MAX;
    stats.newestAccessTime = 0;

    auto files = collectCacheFiles();
    for (const auto& file : files) {
        stats.fileCount++;
        stats.totalSize += QFileInfo(file.first).size();

        if (file.second < stats.oldestAccessTime) {
            stats.oldestAccessTime = file.second;
        }
        if (file.second > stats.newestAccessTime) {
            stats.newestAccessTime = file.second;
        }
    }

    if (stats.fileCount == 0) {
        stats.oldestAccessTime = 0;
    }

    return stats;
}


QList<QString> AssetGarbageCollector::scanForOrphans()
{
    QList<QString> orphans;
    QString AudioSourcesDir = PathUtils::getAudioSourcesDir(projectRoot_);

    QDirIterator it(AudioSourcesDir,
                    AudioFormatCatalog::filePatterns(),
                    QDir::Files,
                    QDirIterator::Subdirectories);

    while (it.hasNext()) {
        it.next();
        QString relativePath = PathUtils::toRelativePath(it.filePath(), AudioSourcesDir);

        if (!AssetRegistry::instance().getAssetByPath(relativePath)) {
            orphans.append(relativePath);
        }
    }

    return orphans;
}

int AssetGarbageCollector::purgeTrash(int maxAgeDays)
{
    const QDir trashDir(PathUtils::getTrashDir(projectRoot_));
    if (!trashDir.exists())
        return 0;

    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-maxAgeDays);
    int purged = 0;
    const QFileInfoList entries =
        trashDir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : entries) {
        if (entry.lastModified() >= cutoff)
            continue;
        const bool removed = entry.isDir()
            ? QDir(entry.absoluteFilePath()).removeRecursively()
            : QFile::remove(entry.absoluteFilePath());
        if (removed)
            ++purged;
    }
    return purged;
}

QList<QString> AssetGarbageCollector::scanForStaleCaches()
{
    QList<QString> stale;
    QString cacheDir = PathUtils::getCacheDir(projectRoot_);

    QDirIterator it(cacheDir, QDir::Files, QDirIterator::Subdirectories);

    while (it.hasNext()) {
        it.next();
        if (!isCurrentMediaCacheFile(it.fileInfo())) {
            stale.append(it.filePath());
        }
    }

    return stale;
}

QList<QPair<QString, uint64_t>> AssetGarbageCollector::collectCacheFiles() const
{
    QList<QPair<QString, uint64_t>> files;
    QString cacheDir = PathUtils::getCacheDir(projectRoot_);

    QDirIterator it(cacheDir, QDir::Files, QDirIterator::Subdirectories);

    while (it.hasNext()) {
        it.next();
        if (!isCurrentMediaCacheFile(it.fileInfo()))
            continue;
        QString filePath = it.filePath();
        uint64_t accessTime = getFileAccessTime(filePath);
        files.append(qMakePair(filePath, accessTime));
    }

    return files;
}

uint64_t AssetGarbageCollector::deleteFile(const QString& path)
{
    QFileInfo info(path);
    if (!info.exists()) {
        return 0;
    }

    uint64_t size = info.size();

    if (!QFile::remove(path)) {
        qWarning() << "Failed to delete file:" << path;
        return 0;
    }

    AssetRegistry::instance().invalidateCache(path);

    return size;
}

uint64_t AssetGarbageCollector::getFileAccessTime(const QString& cachePath) const
{
    QString relativePath = PathUtils::toRelativePath(cachePath, PathUtils::getCacheDir(projectRoot_));

    QFileInfo info(relativePath);
    QString platformName = info.dir().dirName();
    QString fileName = info.completeBaseName().section(QLatin1Char('_'), 0, 0);

    bool ok;
    uint32_t sourceID = fileName.toUInt(&ok);
    if (ok) {
        const auto platform = QUuid::fromString(platformName);
        const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceID);
        if (asset) {
            auto cacheIt = asset->platformCaches.find(platform);
            if (cacheIt != asset->platformCaches.end() && cacheIt->cachePath == relativePath) {
                return cacheIt->lastAccessTime;
            }
        }
    }

    QFileInfo fileInfo(cachePath);
    return fileInfo.lastRead().toSecsSinceEpoch();
}

}
