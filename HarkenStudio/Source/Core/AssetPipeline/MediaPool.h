
#pragma once

#include "../ProjectPlatform.h"
#include <QObject>
#include <QVector>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QFutureWatcher>
#include <cstdint>
#include <atomic>
#include <memory>

namespace VNX {

class AssetRegistry;
class AssetConverter;
class AssetImporter;

struct MediaInfo {
    uint32_t    sourceID = 0;
    QString     relativePath;
    QString     absolutePath;
    MediaStatus status = MediaStatus::Imported;
    AudioMetadata metadata;
    ConversionSettings settings;
    QHash<ProjectPlatformId, CacheEntry> platformCaches;
    bool        isOrphaned = false;
};

struct PoolStats {
    uint32_t totalMedia = 0;
    uint32_t importedCount = 0;
    uint32_t convertedCount = 0;
    uint32_t missingCount = 0;
    uint32_t outdatedCount = 0;
    uint32_t orphanedCount = 0;
    uint64_t totalOriginalBytes = 0;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["totalMedia"]        = static_cast<qint64>(totalMedia);
        obj["importedCount"]     = static_cast<qint64>(importedCount);
        obj["convertedCount"]    = static_cast<qint64>(convertedCount);
        obj["missingCount"]      = static_cast<qint64>(missingCount);
        obj["outdatedCount"]     = static_cast<qint64>(outdatedCount);
        obj["orphanedCount"]     = static_cast<qint64>(orphanedCount);
        obj["totalOriginalBytes"] = static_cast<qint64>(totalOriginalBytes);
        return obj;
    }
};

struct BatchReplaceResult {
    uint32_t sourceID = 0;
    bool success = false;
    QString errorMessage;
};

class MediaPool : public QObject
{
    Q_OBJECT

public:
    static MediaPool& instance();

    void beginSourceFileChange();
    void endSourceFileChange();

    // Project root is owned by AssetRegistry; MediaPool reads through it so the
    // two never drift out of sync.
    QString projectRoot() const;


    MediaInfo getMediaInfo(uint32_t sourceID) const;
    QVector<MediaInfo> getAllMedia() const;
    QVector<MediaInfo> getMediaByStatus(MediaStatus status) const;


    MediaStatus computeStatus(uint32_t sourceID, ProjectPlatformId platform = {}) const;
    QMap<uint32_t, MediaStatus> batchGetStatus(const QVector<uint32_t>& sourceIDs,
                                                ProjectPlatformId platform = {}) const;

    int rescan(ProjectPlatformId platform = {});


    QVector<BatchReplaceResult> batchReplace(const QMap<uint32_t, QString>& replacements);


    void batchConvert(const QVector<uint32_t>& sourceIDs, ProjectPlatformId platform);


    PoolStats getStats(ProjectPlatformId platform = {}) const;


    MediaStatus cachedStatus(uint32_t sourceID) const;

    void refreshStatusCacheAsync(ProjectPlatformId platform = {});


    void batchReplaceAsync(const QMap<uint32_t, QString>& replacements);


    bool refreshSourceFile(uint32_t sourceID, const QString& absolutePath);
    void refreshWaveform(uint32_t sourceID);

signals:
    void mediaStatusChanged(uint32_t sourceID, VNX::MediaStatus newStatus);
    void batchOperationProgress(int current, int total, const QString& currentFile);
    void batchOperationComplete(int succeeded, int failed);
    void mediaHotSwapped(uint32_t sourceID, const QString& path);
    void statusCacheRefreshed();
    void waveformReady(uint32_t sourceID);
    void batchReplaceFinished(QVector<VNX::BatchReplaceResult> results);

private:
    explicit MediaPool(QObject* parent = nullptr);
    ~MediaPool() override = default;

    MediaPool(const MediaPool&) = delete;
    MediaPool& operator=(const MediaPool&) = delete;

    // Shared core for batchReplace (sync) and batchReplaceAsync (worker thread).
    // Each replacement is first *staged* — validate the new file, read its
    // metadata and copy it over the asset's AudioSources/ path — which is pure
    // filesystem work, safe on any thread. The resulting PendingReplace list is
    // then *committed* on the owning thread: registry fields are updated, caches
    // invalidated and the runtime bridge / status signals fired.
    struct PendingReplace {
        uint32_t      sourceID = 0;
        AudioMetadata newMeta;
        QString       destPath;
        QString       contentHash;
        qint64        lastModified = 0;
    };

    struct ReplacementBatchResult {
        QVector<PendingReplace> pending;
        QVector<BatchReplaceResult> results;
    };

    struct ReplacementBatch {
        QFuture<ReplacementBatchResult> future;
        uint64_t generation = 0;
        bool published = false;
    };

    struct ConversionBatch {
        std::shared_ptr<AssetConverter> converter;
        QFuture<void> future;
        QVector<uint32_t> sourceIDs;
        ProjectPlatformId platform;
    };

    static bool stageReplacement(AssetImporter& importer,
                                 const QString& projectRoot,
                                 uint32_t sourceID,
                                 const QString& newFilePath,
                                 PendingReplace& pending,
                                 BatchReplaceResult& result);
    void commitReplacements(const QVector<PendingReplace>& pending);
    void finishReplacementBatch(const std::shared_ptr<ReplacementBatch>& batch);
    void synchronizeSourceFiles(const QStringList& paths);
    void synchronizeSourceFilesAsync(const QStringList& paths);
    void trackSourceFileTask(const QFuture<void>& future);
    void commitSourceFile(uint32_t sourceID, const QString& absolutePath,
                          const AudioMetadata& metadata, const QString& contentHash,
                          uint64_t lastModified);

    bool isSourceFileMissing(const AssetInfo& info) const;
    bool hasAnyValidCache(const AssetInfo& info) const;
    bool isReferencedByAnyObject(uint32_t sourceID) const;

    void rebuildReferenceCache() const;
    void invalidateReferenceCache();
    mutable QHash<uint32_t, bool> referenceCache_;
    mutable bool referenceCacheValid_ = false;

    MediaStatus computeStatusInternal(uint32_t sourceID, ProjectPlatformId platform) const;
    void rebuildStatusCache(ProjectPlatformId platform = {}) const;
    void ensureStatusCacheValid(ProjectPlatformId platform = {}) const;
    mutable QHash<uint32_t, MediaStatus> statusCache_;
    mutable bool statusCacheValid_ = false;
    mutable ProjectPlatformId statusCachePlatform_ = {};

    std::atomic<bool> asyncRefreshRunning_{false};
    QFutureWatcherBase* sourceRefreshWatcher_ = nullptr;
    QList<QFuture<void>> sourceFileFutures_;
    QList<std::shared_ptr<ReplacementBatch>> replacementBatches_;
    QList<std::shared_ptr<ConversionBatch>> conversionBatches_;
    QStringList sourceRefreshPaths_;
    QSet<QString> deferredSourceRefreshPaths_;
    QSet<uint32_t> deferredWaveformIds_;
    QMap<uint32_t, QString> deferredReplacements_;
    QMap<ProjectPlatformId, QSet<uint32_t>> deferredConversions_;
    ProjectPlatformId deferredStatusPlatform_;
    bool deferredStatusRefresh_ = false;
    bool projectClosedDuringFileChange_ = false;
    int sourceFileChangeDepth_ = 0;
    uint64_t sourceFileGeneration_ = 0;
    uint64_t sourceRefreshGeneration_ = 0;
    uint64_t cacheGeneration_ = 0;
};

}
