
#pragma once

#include <QString>
#include <QStringList>
#include <vector>
#include <cstdint>

namespace VNX {

struct ProjectData;
struct ProjectItem;
struct PhysicalFolder;
class LMDBService;

class LMDBIndexer {
public:
    explicit LMDBIndexer(LMDBService* service);

    bool rebuild(ProjectData* project);

    bool indexObject(const ProjectItem* item, const ProjectData* project = nullptr);
    bool removeObject(uint32_t objectID);
    bool updateObject(const ProjectItem* item, const ProjectData* project = nullptr);
    bool updateFolder(const PhysicalFolder* folder, const ProjectData* project);

    uint32_t indexedCount() const { return indexedCount_; }
    double   lastRebuildMs() const { return lastRebuildMs_; }

    static QStringList tokenize(const QString& text);
    static QStringList generateTrigrams(const QString& word);

    static QString buildSearchText(const ProjectItem* item, const ProjectData* project = nullptr);

    static std::vector<uint32_t> unpackIDList(const void* data, size_t size);
    static QByteArray            packIDList(const std::vector<uint32_t>& ids);
    static void insertSorted(std::vector<uint32_t>& ids, uint32_t id);
    static void removeSorted(std::vector<uint32_t>& ids, uint32_t id);
    static uint64_t fnv1aHashPublic(const QString& str);

private:
    class WriteTxnRef;

    void indexObjectInternal(const ProjectItem* item, void* txnOpaque, const ProjectData* project);
    void removeObjectInternal(uint32_t objectID, void* txnOpaque);
    void collectAllItems(const ProjectItem* root,
                         std::vector<const ProjectItem*>& out);

    static QByteArray packObjectMeta(const ProjectItem* item, const QString& searchText);
    static QByteArray typeKeyForItem(const ProjectItem* item);
    static uint64_t fnv1aHash(const QByteArray& data);
    static uint64_t fnv1aHash(const QString& str);

    LMDBService* service_;
    uint32_t indexedCount_ = 0;
    double   lastRebuildMs_ = 0.0;
};

}
