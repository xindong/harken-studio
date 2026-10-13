
#include "MediaPool.h"
#include "AssetRegistry.h"
#include "AssetConverter.h"
#include "../ProjectPlatformActivity.h"
#include "AssetImporter.h"
#include "PathUtils.h"
#include "PlatformProfile.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "../ProjectManager.h"
#include "../PlatformContextService.h"
#include <QFutureWatcher>

#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QDebug>
#include <QDateTime>
#include <QtConcurrent>

namespace VNX {


MediaPool& MediaPool::instance()
{
    static MediaPool inst;
    return inst;
}

MediaPool::MediaPool(QObject* parent)
    : QObject(parent)
{
    auto& pm = ProjectManager::instance();
    connect(&pm, &ProjectManager::itemCreated, this, [this](ProjectItem*) {
        invalidateReferenceCache();
    });
    connect(&pm, &ProjectManager::itemDeleted, this, [this](ProjectItem*) {
        invalidateReferenceCache();
    });
    connect(&pm, &ProjectManager::itemMoved, this, [this](ProjectItem*, ProjectItem*, ProjectItem*) {
        invalidateReferenceCache();
    });
    connect(&pm, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem*, const QString& propertyName) {
        if (propertyName == QStringLiteral("sources"))
            invalidateReferenceCache();
    });
    connect(&pm, &ProjectManager::projectOpened, this, [this](ProjectData*) {
        referenceCache_.clear();
        referenceCacheValid_ = false;
        statusCache_.clear();
        statusCacheValid_ = false;
        ++cacheGeneration_;
        refreshStatusCacheAsync();
    });
    connect(&pm, &ProjectManager::projectClosed, this, [this]() {
        referenceCache_.clear();
        referenceCacheValid_ = false;
        statusCache_.clear();
        statusCacheValid_ = false;
        ++cacheGeneration_;
    });

    const auto invalidatePlatforms = [this]() {
        statusCache_.clear();
        statusCacheValid_ = false;
        ++cacheGeneration_;
        refreshStatusCacheAsync();
    };
    connect(&pm, &ProjectManager::targetPlatformsChanged, this, invalidatePlatforms);
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, invalidatePlatforms);
    connect(&AssetRegistry::instance(), &AssetRegistry::assetModified, this, invalidatePlatforms);

    connect(this, &MediaPool::mediaStatusChanged, this, [this](uint32_t sourceID, MediaStatus) {
        statusCache_.remove(sourceID);
    });
    connect(this, &MediaPool::mediaHotSwapped, this, [this](uint32_t sourceID, const QString&) {
        statusCache_.remove(sourceID);
    });
}


QString MediaPool::projectRoot() const
{
    return AssetRegistry::instance().projectRoot();
}


MediaInfo MediaPool::getMediaInfo(uint32_t sourceID) const
{
    MediaInfo info;
    info.sourceID = sourceID;

    const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceID);
    if (!asset) {
        info.status = MediaStatus::Missing;
        return info;
    }

    info.relativePath   = asset->relativePath;
    info.absolutePath   = asset->absolutePath;
    info.metadata       = asset->metadata;
    info.settings       = asset->settings;
    info.platformCaches = asset->platformCaches;
    info.isOrphaned     = asset->isOrphaned;

    const QString runtimePath = HarkenEditor::Bridge::mediaRegistryFindPath(sourceID);
    if (!runtimePath.isEmpty()) {
        info.absolutePath = runtimePath;
    }

    info.status = computeStatus(sourceID);
    return info;
}

QVector<MediaInfo> MediaPool::getAllMedia() const
{
    QVector<MediaInfo> result;
    QList<uint32_t> ids = AssetRegistry::instance().getAllSourceIDs();
    result.reserve(ids.size());

    for (uint32_t id : ids) {
        result.append(getMediaInfo(id));
    }

    return result;
}

QVector<MediaInfo> MediaPool::getMediaByStatus(MediaStatus status) const
{
    QVector<MediaInfo> result;
    QList<uint32_t> ids = AssetRegistry::instance().getAllSourceIDs();

    for (uint32_t id : ids) {
        MediaInfo mi = getMediaInfo(id);
        if (mi.status == status) {
            result.append(std::move(mi));
        }
    }

    return result;
}


MediaStatus MediaPool::computeStatus(uint32_t sourceID, ProjectPlatformId platform) const
{
    if (platform.isNull()) platform = PlatformContextService::instance().context().primary;
    if (statusCacheValid_ && statusCachePlatform_ == platform) {
        auto it = statusCache_.find(sourceID);
        if (it != statusCache_.end())
            return it.value();
    }

    return computeStatusInternal(sourceID, platform);
}

MediaStatus MediaPool::computeStatusInternal(uint32_t sourceID, ProjectPlatformId platform) const
{
    const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceID);
    if (!asset)
        return MediaStatus::Missing;

    if (isSourceFileMissing(*asset))
        return MediaStatus::Missing;

    if (asset->isOrphaned || !isReferencedByAnyObject(sourceID))
        return MediaStatus::Orphaned;

    auto cacheIt = asset->platformCaches.find(platform);
    if (cacheIt == asset->platformCaches.end() || !cacheIt->valid) {
        return MediaStatus::Imported;
    }

    if (!AssetRegistry::instance().isCacheValid(sourceID, platform))
        return MediaStatus::Outdated;

    return MediaStatus::Converted;
}

MediaStatus MediaPool::cachedStatus(uint32_t sourceID) const
{
    auto it = statusCache_.find(sourceID);
    if (statusCachePlatform_ == PlatformContextService::instance().context().primary
        && it != statusCache_.end())
        return it.value();
    return MediaStatus::Imported;
}

QMap<uint32_t, MediaStatus> MediaPool::batchGetStatus(
    const QVector<uint32_t>& sourceIDs, ProjectPlatformId platform) const
{
    QMap<uint32_t, MediaStatus> result;
    for (uint32_t id : sourceIDs) {
        result[id] = computeStatus(id, platform);
    }
    return result;
}

int MediaPool::rescan(ProjectPlatformId platform)
{
    if (platform.isNull()) platform = PlatformContextService::instance().context().primary;
    invalidateReferenceCache();
    const int queued = AssetRegistry::instance().getAllSourceIDs().size();
    refreshStatusCacheAsync(platform);
    return queued;
}


bool MediaPool::stageReplacement(AssetImporter& importer,
                                 const QString& projectRoot,
                                 uint32_t sourceID,
                                 const QString& newFilePath,
                                 PendingReplace& pending,
                                 BatchReplaceResult& result)
{
    result.sourceID = sourceID;

    QString assetRelPath;
    {
        const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceID);
        if (!asset) {
            result.errorMessage = QStringLiteral("Asset not found: ") + QString::number(sourceID);
            return false;
        }
        assetRelPath = asset->relativePath;
    }

    if (!QFileInfo::exists(newFilePath)) {
        result.errorMessage = QStringLiteral("Replacement file not found: ") + newFilePath;
        return false;
    }

    AudioMetadata newMeta = importer.analyzeFile(newFilePath);
    if (newMeta.sampleRate == 0) {
        result.errorMessage = QStringLiteral("Cannot read metadata: ") + newFilePath;
        return false;
    }

    const QString audioSourcesDir = PathUtils::getAudioSourcesDir(projectRoot);
    const QString destPath = PathUtils::toAbsolutePath(assetRelPath, audioSourcesDir);

    QDir().mkpath(QFileInfo(destPath).absolutePath());
    if (QFile::exists(destPath))
        QFile::remove(destPath);

    if (!QFile::copy(newFilePath, destPath)) {
        result.errorMessage = QStringLiteral("Failed to copy file to AudioSources/");
        return false;
    }

    pending.sourceID = sourceID;
    pending.newMeta = newMeta;
    pending.destPath = destPath;
    pending.contentHash = AssetImporter::computeContentHash(destPath);
    pending.lastModified = QFileInfo(destPath).lastModified().toSecsSinceEpoch();
    result.success = true;
    return true;
}

void MediaPool::commitReplacements(const QVector<PendingReplace>& pending)
{
    for (const PendingReplace& pr : pending) {
        if (!AssetRegistry::instance().getAsset(pr.sourceID))
            continue;

        AssetRegistry::instance().updateSourceFile(
            pr.sourceID, pr.newMeta, pr.destPath, pr.contentHash,
            static_cast<uint64_t>(pr.lastModified));
        AssetRegistry::instance().invalidateAllCaches(pr.sourceID);
        HarkenEditor::Bridge::mediaRegistryRegister(pr.sourceID, pr.destPath);

        emit mediaStatusChanged(pr.sourceID, MediaStatus::Imported);
    }
}

QVector<BatchReplaceResult> MediaPool::batchReplace(
    const QMap<uint32_t, QString>& replacements)
{
    QVector<BatchReplaceResult> results;
    results.reserve(replacements.size());
    QVector<PendingReplace> pending;
    pending.reserve(replacements.size());

    AssetImporter importer;
    importer.setProjectRoot(projectRoot());

    int current = 0;
    const int total = replacements.size();

    for (auto it = replacements.constBegin(); it != replacements.constEnd(); ++it) {
        PendingReplace pr;
        BatchReplaceResult res;
        const bool ok = stageReplacement(importer, projectRoot(), it.key(), it.value(), pr, res);
        results.append(res);
        if (ok)
            pending.append(pr);

        ++current;
        emit batchOperationProgress(current, total,
            ok ? QFileInfo(it.value()).fileName() : it.value());
    }

    commitReplacements(pending);

    int succeeded = 0, failed = 0;
    for (const auto& r : results) {
        if (r.success) ++succeeded;
        else ++failed;
    }

    emit batchOperationComplete(succeeded, failed);
    return results;
}


void MediaPool::batchConvert(const QVector<uint32_t>& sourceIDs, ProjectPlatformId platform)
{
    const auto activity = ProjectPlatformActivity::acquire();
    const auto snapshot = AssetRegistry::instance().buildSnapshot();
    auto converter = std::make_shared<AssetConverter>();
    converter->setBuildSnapshot(snapshot);
    if (platform == PlatformContextService::instance().context().primary) {
        for (uint32_t id : sourceIDs) emit mediaStatusChanged(id, MediaStatus::Converting);
    }
    auto* watcher = new QFutureWatcher<QList<ConversionResult>>(this);
    connect(watcher, &QFutureWatcher<QList<ConversionResult>>::finished, this,
            [this, watcher, converter, snapshot, platform, activity]() {
        const auto results = watcher->result();
        watcher->deleteLater();
        const auto current = AssetRegistry::instance().buildSnapshot();
        if (current.registrySession != snapshot.registrySession
            || current.platformRevisions.value(platform).isNull()
            || current.platformRevisions.value(platform) != snapshot.platformRevisions.value(platform)) return;
        int succeeded = 0, failed = 0;
        for (const auto& result : results) {
            const auto request = snapshot.request(result.sourceID, platform);
            if (!request || !AssetRegistry::instance().accepts(*request)) continue;
            if (result.success) ++succeeded;
            else ++failed;
            if (platform == PlatformContextService::instance().context().primary)
                emit mediaStatusChanged(result.sourceID,
                    result.success ? MediaStatus::Converted : MediaStatus::Error);
        }
        refreshStatusCacheAsync();
        emit batchOperationComplete(succeeded, failed);
    });
    watcher->setFuture(converter->convertBatchAsync(sourceIDs, platform));
}


PoolStats MediaPool::getStats(ProjectPlatformId platform) const
{
    if (platform.isNull()) platform = PlatformContextService::instance().context().primary;
    ensureStatusCacheValid(platform);

    PoolStats stats;
    QList<uint32_t> ids = AssetRegistry::instance().getAllSourceIDs();
    stats.totalMedia = static_cast<uint32_t>(ids.size());

    for (uint32_t id : ids) {
        const AssetInfo* asset = AssetRegistry::instance().getAsset(id);
        if (!asset) continue;

        stats.totalOriginalBytes += asset->metadata.fileSize;

        MediaStatus s = statusCache_.value(id, MediaStatus::Imported);
        switch (s) {
            case MediaStatus::Imported:   ++stats.importedCount; break;
            case MediaStatus::Converted:  ++stats.convertedCount; break;
            case MediaStatus::Missing:    ++stats.missingCount; break;
            case MediaStatus::Outdated:   ++stats.outdatedCount; break;
            case MediaStatus::Orphaned:   ++stats.orphanedCount; break;
            default: break;
        }
    }

    return stats;
}


bool MediaPool::isSourceFileMissing(const AssetInfo& info) const
{
    if (projectRoot().isEmpty())
        return true;

    QString absPath = info.absolutePath;
    if (absPath.isEmpty()) {
        absPath = PathUtils::toAbsolutePath(
            info.relativePath, PathUtils::getAudioSourcesDir(projectRoot()));
    }

    return !QFileInfo::exists(absPath);
}

bool MediaPool::hasAnyValidCache(const AssetInfo& info) const
{
    for (auto it = info.platformCaches.constBegin();
         it != info.platformCaches.constEnd(); ++it) {
        if (it->valid)
            return true;
    }
    return false;
}

bool MediaPool::isReferencedByAnyObject(uint32_t sourceID) const
{
    if (!referenceCacheValid_)
        rebuildReferenceCache();

    return referenceCache_.value(sourceID, false);
}


void MediaPool::rebuildReferenceCache() const
{
    referenceCache_.clear();

    ProjectData* project = ProjectManager::instance().currentProject();
    if (!project) {
        referenceCacheValid_ = true;
        return;
    }

    for (const auto* binding : project->allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        for (uint32_t sourceID : getOwnedMedia(binding->rootItem.get()))
            referenceCache_[sourceID] = true;
    }

    referenceCacheValid_ = true;
    qDebug() << "[MediaPool] Reference cache rebuilt:" << referenceCache_.size() << "referenced sourceIDs";
}

void MediaPool::invalidateReferenceCache()
{
    referenceCacheValid_ = false;
    statusCacheValid_ = false;
    ++cacheGeneration_;
}


void MediaPool::rebuildStatusCache(ProjectPlatformId platform) const
{
    statusCache_.clear();
    QList<uint32_t> ids = AssetRegistry::instance().getAllSourceIDs();
    statusCache_.reserve(ids.size());

    for (uint32_t id : ids) {
        statusCache_[id] = computeStatusInternal(id, platform);
    }

    statusCachePlatform_ = platform;
    statusCacheValid_ = true;
}

void MediaPool::ensureStatusCacheValid(ProjectPlatformId platform) const
{
    if (!statusCacheValid_ || statusCachePlatform_ != platform) {
        rebuildStatusCache(platform);
    }
}


void MediaPool::refreshStatusCacheAsync(ProjectPlatformId platform)
{
    if (platform.isNull()) platform = PlatformContextService::instance().context().primary;
    bool expected = false;
    if (!asyncRefreshRunning_.compare_exchange_strong(expected, true))
        return;

    if (!referenceCacheValid_)
        rebuildReferenceCache();

    QHash<uint32_t, bool> refCacheSnapshot = referenceCache_;
    QList<uint32_t> ids = AssetRegistry::instance().getAllSourceIDs();
    QString projRoot = projectRoot();

    const auto mediaSnapshot = AssetRegistry::instance().buildSnapshot();

    uint64_t startGeneration = cacheGeneration_;

    QtConcurrent::run([this, platform, ids, refCacheSnapshot, mediaSnapshot, projRoot, startGeneration]() {
        try {
        QHash<uint32_t, MediaStatus> newCache;
        newCache.reserve(ids.size());

        for (uint32_t id : ids) {
            auto snapIt = mediaSnapshot.assets.constFind(id);
            if (snapIt == mediaSnapshot.assets.cend()) {
                newCache[id] = MediaStatus::Missing;
                continue;
            }
            const AssetInfo& snap = snapIt.value();

            QString absPath = snap.absolutePath;
            if (absPath.isEmpty() && !projRoot.isEmpty()) {
                absPath = PathUtils::toAbsolutePath(
                    snap.relativePath, PathUtils::getAudioSourcesDir(projRoot));
            }
            if (!QFileInfo::exists(absPath)) {
                newCache[id] = MediaStatus::Missing;
                continue;
            }

            if (snap.isOrphaned || !refCacheSnapshot.value(id, false)) {
                newCache[id] = MediaStatus::Orphaned;
                continue;
            }

            const auto cache = snap.platformCaches.constFind(platform);
            if (cache == snap.platformCaches.cend() || !cache->valid) {
                newCache[id] = MediaStatus::Imported;
                continue;
            }

            const auto request = mediaSnapshot.request(id, platform);
            if (!request || !request->cacheValid()) {
                newCache[id] = MediaStatus::Outdated;
                continue;
            }

            newCache[id] = MediaStatus::Converted;
        }

        QMetaObject::invokeMethod(this, [this, platform, startGeneration, cache = std::move(newCache)]() {
            if (cacheGeneration_ != startGeneration) {
                asyncRefreshRunning_.store(false);
                refreshStatusCacheAsync();
                return;
            }
            statusCache_ = std::move(cache);
            statusCachePlatform_ = platform;
            statusCacheValid_ = true;
            asyncRefreshRunning_.store(false);
            emit statusCacheRefreshed();
        }, Qt::QueuedConnection);

        } catch (...) {
            asyncRefreshRunning_.store(false);
            qWarning() << "[MediaPool] Async status refresh failed with exception";
        }
    });
}


void MediaPool::batchReplaceAsync(const QMap<uint32_t, QString>& replacements)
{
    const QString projRoot = projectRoot();

    QtConcurrent::run([this, replacements, projRoot]() {
        QVector<BatchReplaceResult> results;
        results.reserve(replacements.size());
        QVector<PendingReplace> pending;
        pending.reserve(replacements.size());

        AssetImporter importer;
        importer.setProjectRoot(projRoot);

        int current = 0;
        const int total = replacements.size();

        for (auto it = replacements.constBegin(); it != replacements.constEnd(); ++it) {
            PendingReplace pr;
            BatchReplaceResult res;
            const bool ok = stageReplacement(importer, projRoot, it.key(), it.value(), pr, res);
            results.append(res);
            if (ok)
                pending.append(pr);

            ++current;
            const QString label = ok ? QFileInfo(it.value()).fileName() : it.value();
            QMetaObject::invokeMethod(this, [this, current, total, label]() {
                emit batchOperationProgress(current, total, label);
            }, Qt::QueuedConnection);
        }

        QMetaObject::invokeMethod(this, [this, pending, results]() {
            commitReplacements(pending);

            int succeeded = 0, failed = 0;
            for (const auto& r : results) {
                if (r.success) ++succeeded;
                else ++failed;
            }
            emit batchOperationComplete(succeeded, failed);
            emit batchReplaceFinished(results);
        }, Qt::QueuedConnection);
    });
}


bool MediaPool::refreshSourceFile(uint32_t sourceID, const QString& absolutePath)
{
    if (!AssetRegistry::instance().getAsset(sourceID)
        || !QFileInfo::exists(absolutePath))
        return false;

    AssetImporter importer;
    importer.setProjectRoot(projectRoot());
    const AudioMetadata metadata = importer.analyzeFile(absolutePath);
    if (metadata.sampleRate == 0 || metadata.totalSamples == 0)
        return false;
    const QString contentHash = AssetImporter::computeContentHash(absolutePath);
    if (contentHash.isEmpty())
        return false;

    const uint64_t lastModified = static_cast<uint64_t>(
        QFileInfo(absolutePath).lastModified().toSecsSinceEpoch());
    const bool changed = AssetRegistry::instance().updateSourceFile(
        sourceID, metadata, absolutePath, contentHash, lastModified);
    if (changed)
        AssetRegistry::instance().invalidateAllCaches(sourceID);
    HarkenEditor::Bridge::mediaRegistryRegister(sourceID, absolutePath);
    if (changed)
        emit mediaStatusChanged(sourceID, MediaStatus::Imported);
    emit mediaHotSwapped(sourceID, absolutePath);
    return true;
}

}
