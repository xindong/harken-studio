
#include "MediaPool.h"
#include "AssetRegistry.h"
#include "AssetConverter.h"
#include "../ProjectPlatformActivity.h"
#include "AssetImporter.h"
#include "SourceFileSystem.h"
#include "PathUtils.h"
#include "PlatformProfile.h"
#include "WaveformCache.h"
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
    connect(&WaveformCache::instance(), &WaveformCache::cacheReady, this, [this](const QString& path) {
        const auto* asset = AssetRegistry::instance().findAssetByPath(path);
        if (asset && WaveformCache::isValid(path))
            emit waveformReady(asset->sourceID);
    });
    auto& pm = ProjectManager::instance();
    connect(&pm, &ProjectManager::externalFilesAdded, this, &MediaPool::synchronizeSourceFiles);
    connect(&pm, &ProjectManager::externalFilesRemoved, this, &MediaPool::synchronizeSourceFiles);
    connect(&pm, &ProjectManager::externalFilesModified, this, &MediaPool::synchronizeSourceFiles);
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
        synchronizeSourceFilesAsync(ProjectManager::instance().fileWatcher()->sourceFiles());
        refreshStatusCacheAsync();
    });
    connect(&pm, &ProjectManager::projectClosed, this, [this]() {
        if (sourceRefreshWatcher_) sourceRefreshWatcher_->cancel();
        sourceRefreshWatcher_ = nullptr;
        ++sourceRefreshGeneration_;
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
    connect(&AssetRegistry::instance(), &AssetRegistry::assetRegistered, this, invalidatePlatforms);
    connect(&AssetRegistry::instance(), &AssetRegistry::assetUnregistered, this, invalidatePlatforms);

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
    return AssetRegistry::instance().contains(sourceID) ? MediaStatus::Imported : MediaStatus::Missing;
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

    if (!SourceFileSystem::copyFile(newFilePath, destPath)) {
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

        if (!AssetRegistry::instance().updateSourceFile(
            pr.sourceID, pr.newMeta, pr.destPath, pr.contentHash,
            static_cast<uint64_t>(pr.lastModified)))
            emit AssetRegistry::instance().assetModified(pr.sourceID);
        AssetRegistry::instance().invalidateAllCaches(pr.sourceID);
        WaveformCache::remove(pr.destPath);
        refreshWaveform(pr.sourceID);
        HarkenEditor::Bridge::mediaRegistryRegister(pr.sourceID, pr.destPath);

        emit mediaStatusChanged(pr.sourceID, MediaStatus::Imported);
        emit mediaHotSwapped(pr.sourceID, pr.destPath);
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
    if (sourceRefreshWatcher_) {
        const auto generation = sourceRefreshGeneration_;
        const auto revision = AssetRegistry::instance().buildSnapshot(false).platformRevisions.value(platform);
        connect(sourceRefreshWatcher_, &QFutureWatcherBase::finished, this,
            [this, sourceIDs, platform, generation, revision, activity]() {
                if (generation != sourceRefreshGeneration_ || revision.isNull()) return;
                const auto current = AssetRegistry::instance().buildSnapshot(false);
                if (current.platformRevisions.value(platform) == revision) batchConvert(sourceIDs, platform);
            });
        return;
    }
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

    return !QFileInfo(absPath).isFile();
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

    auto mediaSnapshot = AssetRegistry::instance().buildSnapshot(false);

    uint64_t startGeneration = cacheGeneration_;

    QtConcurrent::run([this, platform, ids, refCacheSnapshot,
                       mediaSnapshot = std::move(mediaSnapshot), projRoot, startGeneration]() mutable {
        try {
        QHash<uint32_t, MediaStatus> newCache;
        newCache.reserve(ids.size());

        for (uint32_t id : ids) {
            auto snapIt = mediaSnapshot.assets.find(id);
            if (snapIt == mediaSnapshot.assets.end()) {
                newCache[id] = MediaStatus::Missing;
                continue;
            }
            AssetInfo& snap = snapIt.value();

            QString absPath = snap.absolutePath;
            if (absPath.isEmpty() && !projRoot.isEmpty()) {
                absPath = PathUtils::toAbsolutePath(
                    snap.relativePath, PathUtils::getAudioSourcesDir(projRoot));
            }
            if (!QFileInfo(absPath).isFile()) {
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

            if (snap.contentHash.isEmpty())
                snap.contentHash = AssetImporter::computeContentHash(absPath);
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


void MediaPool::synchronizeSourceFiles(const QStringList& paths)
{
    auto& registry = AssetRegistry::instance();
    AssetImporter importer;
    const QDir audioRoot(PathUtils::getAudioSourcesDir(projectRoot()));
    for (const QString& path : paths) {
        if (!AssetImporter::isFormatSupported(path)) continue;
        const AssetInfo* asset = registry.findAssetByPath(path);
        if (asset) {
            const uint32_t sourceID = asset->sourceID;
            const QString hash = asset->contentHash;
            if (registry.sourceFileExists(sourceID)) {
                if (!refreshSourceFile(sourceID, path)) continue;
                if (registry.getAsset(sourceID)->contentHash == hash)
                    emit registry.assetModified(sourceID);
            } else {
                registry.invalidateAllCaches(sourceID);
                emit registry.assetModified(sourceID);
            }
        } else if (QFileInfo(path).isFile()) {
            AssetInfo info;
            info.metadata = importer.analyzeFile(path);
            if (info.metadata.sampleRate == 0 || info.metadata.totalSamples == 0) continue;
            info.contentHash = AssetImporter::computeContentHash(path);
            if (info.contentHash.isEmpty()) continue;
            info.sourceID = registry.allocateStableID();
            info.relativePath = audioRoot.relativeFilePath(path);
            info.absolutePath = path;
            info.lastModified = QFileInfo(path).lastModified().toSecsSinceEpoch();
            if (registry.registerAsset(info, false)) {
                HarkenEditor::Bridge::mediaRegistryRegister(info.sourceID, path);
                refreshWaveform(info.sourceID);
            }
        }
    }
}

void MediaPool::synchronizeSourceFilesAsync(const QStringList& paths)
{
    if (sourceRefreshWatcher_) sourceRefreshWatcher_->cancel();
    const auto generation = ++sourceRefreshGeneration_;
    struct SourceRefresh {
        QString path;
        std::optional<AssetInfo> original;
        AudioMetadata metadata;
        QString contentHash;
        qint64 size = 0;
        QDateTime modified;
    };
    QVector<SourceRefresh> sources;
    const auto& registry = AssetRegistry::instance();
    for (const auto& path : paths) {
        if (!AssetImporter::isFormatSupported(path)) continue;
        SourceRefresh source;
        source.path = path;
        if (const auto* asset = registry.findAssetByPath(path))
            source.original = *asset;
        sources.append(std::move(source));
    }
    auto* watcher = new QFutureWatcher<SourceRefresh>(this);
    sourceRefreshWatcher_ = watcher;
    connect(watcher, &QFutureWatcher<SourceRefresh>::finished, this, [this, watcher, generation]() {
        if (generation == sourceRefreshGeneration_) sourceRefreshWatcher_ = nullptr;
        watcher->deleteLater();
    });
    connect(watcher, &QFutureWatcher<SourceRefresh>::resultReadyAt, this, [this, watcher, generation](int index) {
        if (watcher->isCanceled() || generation != sourceRefreshGeneration_) return;
        const auto source = watcher->resultAt(index);
        const QFileInfo file(source.path);
        if (!file.isFile() || file.size() != source.size || file.lastModified() != source.modified)
            return;
        auto& registry = AssetRegistry::instance();
        const auto* current = registry.findAssetByPath(source.path);
        if (source.original) {
            const auto& original = *source.original;
            if (!current || current->sourceID != original.sourceID
                || current->relativePath != original.relativePath
                || current->absolutePath != original.absolutePath
                || current->contentHash != original.contentHash
                || current->lastModified != original.lastModified
                || !audioMetadataEqual(current->metadata, original.metadata))
                return;
            commitSourceFile(original.sourceID, source.path, source.metadata, source.contentHash,
                static_cast<uint64_t>(source.modified.toSecsSinceEpoch()));
        } else if (!current) {
            AssetInfo info;
            info.sourceID = registry.allocateStableID();
            info.relativePath = QDir(PathUtils::getAudioSourcesDir(projectRoot())).relativeFilePath(source.path);
            info.absolutePath = source.path;
            info.metadata = source.metadata;
            info.contentHash = source.contentHash;
            info.lastModified = static_cast<uint64_t>(source.modified.toSecsSinceEpoch());
            if (registry.registerAsset(info, false)) {
                HarkenEditor::Bridge::mediaRegistryRegister(info.sourceID, source.path);
                refreshWaveform(info.sourceID);
            }
        }
    });
    const auto future = QtConcurrent::run([sources = std::move(sources)](QPromise<SourceRefresh>& promise) {
        AssetImporter importer;
        for (auto source : sources) {
            if (promise.isCanceled()) return;
            const QFileInfo before(source.path);
            if (!before.isFile()) continue;
            source.size = before.size();
            source.modified = before.lastModified();
            source.metadata = importer.analyzeFile(source.path);
            if (source.metadata.sampleRate == 0 || source.metadata.totalSamples == 0) continue;
            if (promise.isCanceled()) return;
            source.contentHash = AssetImporter::computeContentHash(source.path);
            if (source.contentHash.isEmpty()) continue;
            const QFileInfo after(source.path);
            if (!after.isFile() || after.size() != source.size || after.lastModified() != source.modified)
                continue;
            promise.addResult(std::move(source));
        }
    });
    watcher->setFuture(future);
}

void MediaPool::refreshWaveform(uint32_t sourceID)
{
    const auto* asset = AssetRegistry::instance().getAsset(sourceID);
    if (!asset || WaveformCache::isValid(asset->absolutePath)) return;
    WaveformCache::generateAsync(asset->absolutePath);
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
    commitSourceFile(sourceID, absolutePath, metadata, contentHash, lastModified);
    return true;
}

void MediaPool::commitSourceFile(uint32_t sourceID, const QString& absolutePath,
                                 const AudioMetadata& metadata, const QString& contentHash,
                                 uint64_t lastModified)
{
    const bool changed = AssetRegistry::instance().updateSourceFile(
        sourceID, metadata, absolutePath, contentHash, lastModified);
    if (changed) {
        AssetRegistry::instance().invalidateAllCaches(sourceID);
        WaveformCache::remove(absolutePath);
    }
    refreshWaveform(sourceID);
    HarkenEditor::Bridge::mediaRegistryRegister(sourceID, absolutePath);
    if (changed)
        emit mediaStatusChanged(sourceID, MediaStatus::Imported);
    emit mediaHotSwapped(sourceID, absolutePath);
}

}
