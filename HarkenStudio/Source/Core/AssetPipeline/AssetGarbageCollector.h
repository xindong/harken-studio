
#pragma once

#include "AssetTypes.h"
#include <QObject>
#include <QString>
#include <QList>
#include <QPair>

namespace VNX {

struct CleanupResult
{
    int deletedCount = 0;
    uint64_t freedBytes = 0;
    QStringList deletedFiles;
    QStringList errors;
};

class AssetGarbageCollector : public QObject
{
    Q_OBJECT

public:
    explicit AssetGarbageCollector(QObject* parent = nullptr);
    ~AssetGarbageCollector() override;

    void setProjectRoot(const QString& root);


    CleanupResult cleanupLRU(int maxAgeDays = 90, uint64_t maxCacheSizeBytes = 0);

    uint64_t getCacheSize() const;

    struct CacheStats {
        int fileCount = 0;
        uint64_t totalSize = 0;
        uint64_t oldestAccessTime = 0;
        uint64_t newestAccessTime = 0;
    };
    CacheStats getCacheStats() const;


    QList<QString> scanForOrphans();

    // Drops .trash/ quarantine entries older than maxAgeDays; quarantined
    // audio stays recoverable until then.
    int purgeTrash(int maxAgeDays = 30);

    QList<QString> scanForStaleCaches();

signals:
    void cleanupProgress(int percent, const QString& message);

    void cleanupComplete(int deletedCount, uint64_t freedBytes);

private:

    QList<QPair<QString, uint64_t>> collectCacheFiles() const;

    uint64_t deleteFile(const QString& path);

    uint64_t getFileAccessTime(const QString& cachePath) const;

    QString projectRoot_;
};

}
