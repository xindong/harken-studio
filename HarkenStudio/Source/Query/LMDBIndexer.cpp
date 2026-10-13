
#include "LMDBIndexer.h"
#include "LMDBService.h"
#include "../Core/ItemTypeCatalog.h"
#include "../Core/ProjectTypes.h"

#include <QElapsedTimer>
#include <QDataStream>
#include <QIODevice>
#include <algorithm>
#include <cstring>

namespace VNX {


static constexpr uint64_t kFNV64Offset = 14695981039346656037ULL;
static constexpr uint64_t kFNV64Prime  = 1099511628211ULL;

uint64_t LMDBIndexer::fnv1aHash(const QByteArray& data)
{
    uint64_t hash = kFNV64Offset;
    for (int i = 0; i < data.size(); ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= kFNV64Prime;
    }
    return hash;
}

uint64_t LMDBIndexer::fnv1aHash(const QString& str)
{
    return fnv1aHash(str.toUtf8());
}


QStringList LMDBIndexer::tokenize(const QString& text)
{
    QStringList tokens;
    if (text.isEmpty()) return tokens;

    QString current;
    current.reserve(32);

    auto flushToken = [&]() {
        if (!current.isEmpty()) {
            QString lower = current.toLower();
            if (!tokens.contains(lower))
                tokens.append(lower);
            current.clear();
        }
    };

    for (int i = 0; i < text.size(); ++i) {
        QChar ch = text[i];

        if (ch == QLatin1Char('_') || ch == QLatin1Char('.') ||
            ch == QLatin1Char('/') || ch == QLatin1Char('\\') ||
            ch == QLatin1Char('-') || ch == QLatin1Char(' ')) {
            flushToken();
            continue;
        }

        if (ch.isUpper() && !current.isEmpty() && current.back().isLower()) {
            flushToken();
        }

        current.append(ch);
    }
    flushToken();

    return tokens;
}


QStringList LMDBIndexer::generateTrigrams(const QString& word)
{
    QStringList trigrams;
    QString lower = word.toLower();
    if (lower.size() < 3) return trigrams;

    for (int i = 0; i <= lower.size() - 3; ++i) {
        QString tri = lower.mid(i, 3);
        if (!trigrams.contains(tri))
            trigrams.append(tri);
    }
    return trigrams;
}


QByteArray LMDBIndexer::packObjectMeta(const ProjectItem* item, const QString& searchText)
{
    QByteArray packed;
    QByteArray nameUtf8 = item->name.toUtf8();

    packed.resize(4 + 2 + 2 + nameUtf8.size());
    char* p = packed.data();

    uint32_t oid = item->objectID;
    std::memcpy(p, &oid, 4);       p += 4;

    uint16_t ty = static_cast<uint16_t>(item->type);
    std::memcpy(p, &ty, 2);        p += 2;

    uint16_t nlen = static_cast<uint16_t>(nameUtf8.size());
    std::memcpy(p, &nlen, 2);      p += 2;

    std::memcpy(p, nameUtf8.constData(), nameUtf8.size());
    packed.append(searchText.toUtf8());

    return packed;
}


QByteArray LMDBIndexer::typeKeyForItem(const ProjectItem* item)
{
    return ItemTypeCatalog::canonicalName(item->type).toLatin1();
}


QString LMDBIndexer::buildSearchText(const ProjectItem* item, const ProjectData* project)
{
    if (!item) return {};

    QString searchText = item->name;
    const QString path = item->type == ProjectItem::Type::Sound
        ? item->sourcePath
        : project && !item->parent ? project->itemFolderPath(item) : QString{};
    if (!path.isEmpty())
        searchText += QLatin1Char(' ') + path;

    searchText += QLatin1Char(' ') + QString::fromUtf8(typeKeyForItem(item));

    item->components.forEach([&](const IComponent& c) {
        const QString ct = c.searchableText();
        if (!ct.isEmpty())
            searchText += QLatin1Char(' ') + ct;
    });

    return searchText;
}


std::vector<uint32_t> LMDBIndexer::unpackIDList(const void* data, size_t size)
{
    size_t count = size / sizeof(uint32_t);
    std::vector<uint32_t> ids(count);
    if (count > 0)
        std::memcpy(ids.data(), data, count * sizeof(uint32_t));
    return ids;
}

QByteArray LMDBIndexer::packIDList(const std::vector<uint32_t>& ids)
{
    QByteArray packed;
    packed.resize(static_cast<int>(ids.size() * sizeof(uint32_t)));
    if (!ids.empty())
        std::memcpy(packed.data(), ids.data(), ids.size() * sizeof(uint32_t));
    return packed;
}

void LMDBIndexer::insertSorted(std::vector<uint32_t>& ids, uint32_t id)
{
    auto it = std::lower_bound(ids.begin(), ids.end(), id);
    if (it == ids.end() || *it != id)
        ids.insert(it, id);
}

void LMDBIndexer::removeSorted(std::vector<uint32_t>& ids, uint32_t id)
{
    auto it = std::lower_bound(ids.begin(), ids.end(), id);
    if (it != ids.end() && *it == id)
        ids.erase(it);
}


#ifdef HARKEN_HAS_LMDB

static void addToPostingList(LMDBService::WriteTxn& txn, LMDBService::DBI dbi,
                             uint64_t hashKey, uint32_t objectID)
{
    void*  existData = nullptr;
    size_t existSize = 0;
    txn.get(dbi, &hashKey, sizeof(hashKey), &existData, &existSize);

    auto ids = LMDBIndexer::unpackIDList(existData, existSize);
    LMDBIndexer::insertSorted(ids, objectID);
    QByteArray packed = LMDBIndexer::packIDList(ids);
    txn.put(dbi, &hashKey, sizeof(hashKey), packed.constData(), packed.size());
}

static void removeFromPostingList(LMDBService::WriteTxn& txn, LMDBService::DBI dbi,
                                  uint64_t hashKey, uint32_t objectID)
{
    void*  existData = nullptr;
    size_t existSize = 0;
    txn.get(dbi, &hashKey, sizeof(hashKey), &existData, &existSize);

    auto ids = LMDBIndexer::unpackIDList(existData, existSize);
    LMDBIndexer::removeSorted(ids, objectID);

    if (ids.empty()) {
        txn.del(dbi, &hashKey, sizeof(hashKey));
    } else {
        QByteArray packed = LMDBIndexer::packIDList(ids);
        txn.put(dbi, &hashKey, sizeof(hashKey), packed.constData(), packed.size());
    }
}

#endif


LMDBIndexer::LMDBIndexer(LMDBService* service)
    : service_(service)
{
}


void LMDBIndexer::collectAllItems(const ProjectItem* root,
                                  std::vector<const ProjectItem*>& out)
{
    if (!root) return;
    out.push_back(root);
    for (const auto& child : root->children)
        collectAllItems(child.get(), out);
}


void LMDBIndexer::indexObjectInternal(const ProjectItem* item, void* txnOpaque,
                                      const ProjectData* project)
{
#ifdef HARKEN_HAS_LMDB
    if (!item || !txnOpaque) return;
    auto& txn = *static_cast<LMDBService::WriteTxn*>(txnOpaque);
    uint32_t oid = item->objectID;
    if (oid == 0) return;

    const QString searchText = buildSearchText(item, project);
    QByteArray meta = packObjectMeta(item, searchText);
    txn.put(LMDBService::Object, &oid, sizeof(oid),
            meta.constData(), meta.size());

    QStringList tokens = tokenize(searchText);
    for (const QString& tok : tokens) {
        uint64_t h = fnv1aHash(tok);
        addToPostingList(txn, LMDBService::Inverted, h, oid);
    }

    for (const QString& tok : tokens) {
        QStringList tris = generateTrigrams(tok);
        for (const QString& tri : tris) {
            uint64_t h = fnv1aHash(tri);
            addToPostingList(txn, LMDBService::Trigram, h, oid);
        }
    }

    QByteArray typeKey = typeKeyForItem(item);
    uint64_t typeHash = fnv1aHash(typeKey);
    addToPostingList(txn, LMDBService::Type, typeHash, oid);

#else
    Q_UNUSED(item);
    Q_UNUSED(txnOpaque);
    Q_UNUSED(project);
#endif
}

void LMDBIndexer::removeObjectInternal(uint32_t objectID, void* txnOpaque)
{
#ifdef HARKEN_HAS_LMDB
    if (!txnOpaque || objectID == 0) return;
    auto& txn = *static_cast<LMDBService::WriteTxn*>(txnOpaque);

    void*  metaData = nullptr;
    size_t metaSize = 0;
    int rc = txn.get(LMDBService::Object, &objectID, sizeof(objectID),
                     &metaData, &metaSize);
    if (rc != 0 || !metaData || metaSize < 8) {
        return;
    }

    const char* p = static_cast<const char*>(metaData);
    p += 4;
    uint16_t typeVal = 0;
    std::memcpy(&typeVal, p, 2);  p += 2;
    uint16_t nameLen = 0;
    std::memcpy(&nameLen, p, 2);  p += 2;

    if (static_cast<size_t>(8 + nameLen) > metaSize) return;
    const QString searchText = QString::fromUtf8(p + nameLen, metaSize - 8 - nameLen);
    QStringList tokens = tokenize(searchText);

    for (const QString& tok : tokens) {
        uint64_t h = fnv1aHash(tok);
        removeFromPostingList(txn, LMDBService::Inverted, h, objectID);
    }

    for (const QString& tok : tokens) {
        QStringList tris = generateTrigrams(tok);
        for (const QString& tri : tris) {
            uint64_t h = fnv1aHash(tri);
            removeFromPostingList(txn, LMDBService::Trigram, h, objectID);
        }
    }

    auto itemType = static_cast<ProjectItem::Type>(typeVal);
    ProjectItem tmpItem;
    tmpItem.type = itemType;
    QByteArray typeKey = typeKeyForItem(&tmpItem);
    uint64_t typeHash = fnv1aHash(typeKey);
    removeFromPostingList(txn, LMDBService::Type, typeHash, objectID);

    txn.del(LMDBService::Object, &objectID, sizeof(objectID));

#else
    Q_UNUSED(objectID);
    Q_UNUSED(txnOpaque);
#endif
}


bool LMDBIndexer::rebuild(ProjectData* project)
{
#ifdef HARKEN_HAS_LMDB
    if (!service_ || !service_->isOpen() || !project)
        return false;

    QElapsedTimer timer;
    timer.start();

    std::vector<const ProjectItem*> allItems;
    allItems.reserve(4096);
    if (project->root)
        collectAllItems(project->root.get(), allItems);

    for (const auto* binding : project->allFileBindings()) {
        if (binding && binding->rootItem)
            collectAllItems(binding->rootItem.get(), allItems);
    }

    std::sort(allItems.begin(), allItems.end(),
              [](const ProjectItem* a, const ProjectItem* b) {
                  return a->objectID < b->objectID;
              });
    allItems.erase(
        std::unique(allItems.begin(), allItems.end(),
                    [](const ProjectItem* a, const ProjectItem* b) {
                        return a->objectID == b->objectID;
                    }),
        allItems.end());

    auto txn = service_->beginWrite();
    if (!txn.valid())
        return false;

    for (int i = 0; i < LMDBService::DBI_COUNT; ++i) {
        if (txn.clear(static_cast<LMDBService::DBI>(i)) != 0)
            return false;
    }

    uint32_t count = 0;
    for (const ProjectItem* item : allItems) {
        if (item->objectID == 0) continue;
        indexObjectInternal(item, &txn, project);
        ++count;
    }

    if (!txn.commit())
        return false;

    indexedCount_ = count;
    lastRebuildMs_ = timer.elapsed();
    return true;

#else
    Q_UNUSED(project);
    return false;
#endif
}

bool LMDBIndexer::indexObject(const ProjectItem* item, const ProjectData* project)
{
#ifdef HARKEN_HAS_LMDB
    if (!service_ || !service_->isOpen() || !item || item->objectID == 0)
        return false;

    auto txn = service_->beginWrite();
    if (!txn.valid()) return false;

    indexObjectInternal(item, &txn, project);

    if (!txn.commit()) return false;
    ++indexedCount_;
    return true;

#else
    Q_UNUSED(item);
    Q_UNUSED(project);
    return false;
#endif
}

bool LMDBIndexer::removeObject(uint32_t objectID)
{
#ifdef HARKEN_HAS_LMDB
    if (!service_ || !service_->isOpen() || objectID == 0)
        return false;

    auto txn = service_->beginWrite();
    if (!txn.valid()) return false;

    removeObjectInternal(objectID, &txn);

    if (!txn.commit()) return false;
    if (indexedCount_ > 0) --indexedCount_;
    return true;

#else
    Q_UNUSED(objectID);
    return false;
#endif
}

bool LMDBIndexer::updateObject(const ProjectItem* item, const ProjectData* project)
{
#ifdef HARKEN_HAS_LMDB
    if (!service_ || !service_->isOpen() || !item || item->objectID == 0)
        return false;

    auto txn = service_->beginWrite();
    if (!txn.valid()) return false;

    removeObjectInternal(item->objectID, &txn);
    indexObjectInternal(item, &txn, project);

    return txn.commit();

#else
    Q_UNUSED(item);
    Q_UNUSED(project);
    return false;
#endif
}

bool LMDBIndexer::updateFolder(const PhysicalFolder* folder, const ProjectData* project)
{
#ifdef HARKEN_HAS_LMDB
    if (!service_ || !service_->isOpen() || !folder || !project)
        return false;

    auto txn = service_->beginWrite();
    if (!txn.valid()) return false;

    for (const auto* binding : project->fileBindingsUnderFolder(folder)) {
        const auto* item = binding->rootItem.get();
        removeObjectInternal(item->objectID, &txn);
        indexObjectInternal(item, &txn, project);
    }

    return txn.commit();
#else
    Q_UNUSED(folder);
    Q_UNUSED(project);
    return false;
#endif
}

uint64_t LMDBIndexer::fnv1aHashPublic(const QString& str)
{
    return fnv1aHash(str);
}

}
