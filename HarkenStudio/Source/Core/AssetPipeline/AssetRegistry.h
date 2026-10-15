
#pragma once

#include "AssetTypes.h"
#include "MediaBuildSnapshot.h"
#include "PathUtils.h"
#include <QObject>
#include <QHash>
#include <QString>
#include <QList>
#include <QMutex>
#include <QSet>
#include <memory>

namespace VNX {

class AssetRegistry : public QObject
{
    Q_OBJECT

public:
    static constexpr int kFormatVersion = 1;

    static AssetRegistry& instance();


    void setProjectRoot(const QString& root);

    QString projectRoot() const;
    void setPlatforms(const QVector<ProjectPlatform>& platforms);
    MediaBuildSnapshot buildSnapshot(bool resolveMissingHashes = true) const;
    MediaBuildSnapshot buildSnapshot(const QString& projectRoot,
                                     const QVector<ProjectPlatform>& platforms,
                                     bool resolveMissingHashes = true) const;
    bool accepts(const MediaConversionRequest& request) const;

    void clear();


    uint32_t allocateStableID();


    bool registerAsset(const AssetInfo& info, bool uncommitted = true);

    void unregisterAsset(uint32_t sourceID);

    void markAsOrphaned(uint32_t sourceID);

    void restoreOrphaned(uint32_t sourceID);


    AssetInfo* getAsset(uint32_t sourceID);
    const AssetInfo* getAsset(uint32_t sourceID) const;

    AssetInfo* getAssetByPath(const QString& relativePath);
    const AssetInfo* getAssetByPath(const QString& relativePath) const;

    const AssetInfo* findAssetByContentHash(const QString& contentHash) const;

    const AssetInfo* findAssetByPath(const QString& absolutePath) const;

    bool contains(uint32_t sourceID) const;
    bool sourceFileExists(uint32_t sourceID) const;

    QList<uint32_t> getAllSourceIDs() const;

    QList<const AssetInfo*> getAllAssets() const;

    QList<uint32_t> getMissingAssets() const;

    int count() const { return assets_.size(); }
    bool updateSourceFile(uint32_t sourceID, const AudioMetadata& metadata,
                          const QString& absolutePath, const QString& contentHash,
                          uint64_t lastModified);
    void relocateSourcePaths(const QString& oldPath, const QString& newPath);


    std::optional<MediaConversionRequest> conversionRequest(
        uint32_t sourceID, ProjectPlatformId platform, QString* error = nullptr) const;
    bool isCacheValid(uint32_t sourceID, ProjectPlatformId platform) const;

    bool updateCache(const MediaConversionRequest& request, const CacheEntry& entry);

    void touchCache(uint32_t sourceID, ProjectPlatformId platform);

    void invalidateCache(const QString& cachePath);

    void invalidateAllCaches(uint32_t sourceID);


    bool saveToFile(const QString& path = QString());

    bool loadFromFile(const QString& path = QString());
    static bool loadSnapshotFromFile(const QString& projectRoot,
                                     QList<AssetInfo>& assets,
                                     const QString& path = QString());
    static bool sourceAssetsFromJson(const QJsonValue& value,
                                     QMap<uint32_t, QString>& references);
    static bool reconcileSnapshot(const QString& projectRoot,
                                   const QMap<uint32_t, QString>& references,
                                   QList<AssetInfo>& assets);
    QList<AssetInfo> snapshot() const;
    void replaceSnapshot(const QString& projectRoot, const QList<AssetInfo>& assets);
    static bool saveSnapshotToFile(const QString& projectRoot,
                                   const QList<AssetInfo>& assets);


    // Assets registered since the last load/save that have not yet been
    // persisted to assets.json. saveToFile() commits them (clears the set);
    // discardUncommittedFiles() drops them — deletes their physical files and
    // records — so closing an unsaved session leaves no import residue behind.
    int discardUncommittedFiles();

    bool isUncommitted(uint32_t sourceID) const;

    void markAllCommitted();

    void cleanupOrphaned(const QList<QString>& files);

signals:
    void sourceFoldersChanged();

    void assetRegistered(uint32_t sourceID);

    void assetUnregistered(uint32_t sourceID);

    void assetModified(uint32_t sourceID);

    void cacheInvalidated(uint32_t sourceID, VNX::ProjectPlatformId platform);

private:
    explicit AssetRegistry(QObject* parent = nullptr);
    ~AssetRegistry() override = default;

    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;


    QString assetsFilePath() const;

    void updatePathIndex(const AssetInfo& info);

    QString projectRoot_;
    QUuid session_ = QUuid::createUuid();
    QVector<ProjectPlatform> platforms_;
    QHash<ProjectPlatformId, QUuid> platformRevisions_;
    QHash<uint32_t, AssetInfo> assets_;
    QHash<QString, uint32_t> pathToID_;
    QSet<uint32_t> uncommittedIDs_;
    mutable QMutex mutex_;
};

}
