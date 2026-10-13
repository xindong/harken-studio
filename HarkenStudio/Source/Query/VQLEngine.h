
#pragma once

#include "VQLTypes.h"
#include "VQLObserver.h"
#include "MemoryDataSource.h"
#include <list>
#include <unordered_map>
#include <memory>
#include <functional>
#include <vector>

#ifdef HARKEN_HAS_LMDB
#include "LMDBService.h"
#include "LMDBIndexer.h"
#include "LMDBQueryTask.h"
#endif

namespace VNX {

struct ProjectData;
struct ProjectItem;
struct PhysicalFolder;
#ifndef HARKEN_HAS_LMDB
struct SearchResult {
    uint32_t objectID = 0;
    QString  name;
    QString  type;
    float    score = 0.0f;
};
#endif

class VQLEngine
{
public:
    static constexpr size_t DEFAULT_CACHE_CAPACITY = 64;

    VQLEngine();

    void setProject(ProjectData* project);
    ProjectData* project() const { return project_; }

    IVQLDataSource* dataSource() { return &memoryDataSource_; }

    VQLCompiledQuery compile(const QString& vql, VQLError* outError = nullptr);

    VQLResultSet execute(const VQLCompiledQuery& query, VQLError* outError = nullptr);

    VQLResultSet query(const QString& vql, VQLError* outError = nullptr);

    QJsonArray queryJson(const QString& vql,
                         const std::vector<QString>& returnFields = {},
                         VQLError* outError = nullptr);

    void clearCache();
    size_t cacheSize() const { return cache_.size(); }
    void setCacheCapacity(size_t cap) { cacheCapacity_ = cap; }

    VQLObserver* observer() { return observer_.get(); }
    ObserverId observe(const QString& vql, ObserverCallback callback);
    void unobserve(ObserverId id);
    void notifyProjectChanged();
    void notifyFolderChanged(const PhysicalFolder* folder);

    void searchAsync(const QString& query,
                     std::function<void(const std::vector<SearchResult>&)> callback,
                     int maxResults = 100);
    void cancelSearch();
    bool isSearching() const;

    void rebuildSearchIndex();
    void indexObjectChanged(const ProjectItem* item);
    void indexObjectRemoved(uint32_t objectID);

    LMDBService* lmdbService() const;
    LMDBIndexer* lmdbIndexer() const;

    LMDBService* ensureLmdbOpen();

private:
    void evictLRU();

    ProjectData* project_ = nullptr;
    MemoryDataSource memoryDataSource_;
    size_t       cacheCapacity_ = DEFAULT_CACHE_CAPACITY;

    using LRUList = std::list<uint32_t>;
    struct CacheEntry {
        VQLCompiledQuery query;
        LRUList::iterator lruIter;
    };

    std::unordered_map<uint32_t, CacheEntry> cache_;
    LRUList                                  lruOrder_;

    std::unique_ptr<VQLObserver> observer_;

#ifdef HARKEN_HAS_LMDB
    std::unique_ptr<LMDBService> lmdbService_;
    std::unique_ptr<LMDBIndexer> lmdbIndexer_;
    LMDBQueryTask* activeSearch_ = nullptr;
    bool lmdbInitialized_ = false;
    QString lmdbPath_;

    void initLMDB();
#endif
};

}
