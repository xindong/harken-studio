
#include "ProjectIndex.h"

#include "AssetPipeline/PathUtils.h"
#include "ItemTypeCatalog.h"
#include "ProjectDocument.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLockFile>
#include <QSaveFile>

#include <algorithm>

namespace VNX {

namespace {

const QStringList& standardHierarchyFolders()
{
    static const QStringList folders = [] {
        QStringList out;
        for (ItemTypeCatalog::Bucket bucket : ItemTypeCatalog::allBuckets())
            out << ItemTypeCatalog::folderNameOf(bucket);
        return out;
    }();
    return folders;
}

QString cacheDir(const QString& projectRoot)
{
    return QDir(projectRoot).filePath(QStringLiteral(".cache/project_index"));
}

QString indexFilePath(const QString& projectRoot)
{
    return QDir(cacheDir(projectRoot)).filePath(QStringLiteral("index.json"));
}

QString lockFilePath(const QString& projectRoot)
{
    return QDir(cacheDir(projectRoot)).filePath(QStringLiteral("index.lock"));
}

std::unique_ptr<QLockFile> acquireLock(const QString& projectRoot)
{
    QDir().mkpath(cacheDir(projectRoot));
    auto lock = std::make_unique<QLockFile>(lockFilePath(projectRoot));
    lock->setStaleLockTime(30000);
    if (!lock->tryLock(2000)) {
        qWarning() << "[ProjectIndex] Lock contention on" << lockFilePath(projectRoot);
        return nullptr;
    }
    return lock;
}

QString guidKey(const QUuid& guid)
{
    return guid.toString(QUuid::WithoutBraces);
}

QString nameKey(ProjectItem::Type type, const QString& name)
{
    const QString normalized = name.normalized(QString::NormalizationForm_C);
    return type == ProjectItem::Type::Attenuation
        ? normalized.toCaseFolded() : normalized;
}

QByteArray sha256Hex(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toLower();
}

QString relativeForwardSlash(const QString& projectRoot, const QString& absoluteFile)
{
    const QString rel = QDir(projectRoot).relativeFilePath(absoluteFile);
    return QString(rel).replace(QLatin1Char('\\'), QLatin1Char('/'));
}

QString documentTypeForPath(const QString& relativePath)
{
    const QString folder = relativePath.section(QLatin1Char('/'), 0, 0);
    if (folder == QLatin1String("Layers")) return QStringLiteral("project.layer");
    if (folder == QLatin1String("Busses")) return QStringLiteral("project.bus");
    if (folder == QLatin1String("Actions")) return QStringLiteral("project.action");
    if (folder == QLatin1String("Variables")) return QStringLiteral("project.variable");
    if (folder == QLatin1String("Attenuations")) return QStringLiteral("project.attenuation");
    return {};
}

void indexChildrenRecursive(const QJsonArray& children,
                            const QString& filePath,
                            qint64 mtime,
                            qint64 size,
                            const QByteArray& hash,
                            std::vector<int>& embedPathStack,
                            QHash<QUuid, ProjectIndex::Entry>& outByGuid,
                            QHash<int, QMultiHash<QString, QUuid>>& outByName)
{
    for (int i = 0; i < children.size(); ++i) {
        const QJsonValue cv = children.at(i);
        if (!cv.isObject()) continue;
        const QJsonObject co = cv.toObject();

        const QString guidStr = co.value(QStringLiteral("guid")).toString();
        const QUuid   guid    = QUuid::fromString(guidStr);
        if (guid.isNull()) continue;

        const QString rawType = co.value(QStringLiteral("type")).toString();
        const auto childType = ItemTypeCatalog::parseTypeName(
            rawType, Qt::CaseSensitive, false);
        if (!childType) {
            qWarning() << "[ProjectIndex] Unknown item type" << rawType
                       << "in" << filePath << "-- skipping entry.";
            continue;
        }

        ProjectIndex::Entry entry;
        entry.guid     = guid;
        entry.name     = co.value(QStringLiteral("name")).toString();
        entry.type     = *childType;
        entry.filePath = filePath;
        entry.mtime    = mtime;
        entry.size     = size;
        entry.hash     = hash;

        embedPathStack.push_back(i);
        entry.embedPath = embedPathStack;

        if (co.contains(QStringLiteral("parentRef"))) {
            const QJsonObject pr = co.value(QStringLiteral("parentRef")).toObject();
            const QString prGuid = pr.value(QStringLiteral("id")).toString();
            if (!prGuid.isEmpty()) entry.parentGuid = QUuid::fromString(prGuid);
        }

        outByGuid.insert(guid, entry);
        if (!entry.name.isEmpty()) {
            outByName[static_cast<int>(entry.type)].insert(nameKey(entry.type, entry.name), guid);
        }

        const QJsonArray nested = co.value(QStringLiteral("children")).toArray();
        if (!nested.isEmpty()) {
            indexChildrenRecursive(nested, filePath, mtime, size, hash,
                                   embedPathStack, outByGuid, outByName);
        }

        embedPathStack.pop_back();
    }
}

bool indexOnePackFile(const QString& projectRoot,
                     const QFileInfo& fileInfo,
                     QHash<QUuid, ProjectIndex::Entry>& outByGuid,
                     QHash<int, QMultiHash<QString, QUuid>>& outByName)
{
    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[ProjectIndex] open failed:" << fileInfo.absoluteFilePath();
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[ProjectIndex] JSON parse failed for"
                   << fileInfo.absoluteFilePath() << ":" << parseErr.errorString();
        return false;
    }
    const QString relPath = relativeForwardSlash(projectRoot, fileInfo.absoluteFilePath());
    const QString documentType = documentTypeForPath(relPath);
    ProjectDocumentData document;
    QString documentError;
    if (documentType.isEmpty()
        || !ProjectDocument::parse(doc.object(), documentType,
                                   &document, &documentError)) {
        qWarning() << "[ProjectIndex]" << documentError << "in"
                   << fileInfo.absoluteFilePath();
        return false;
    }
    const QJsonObject obj = document.content;

    const QString guidStr = obj.value(QStringLiteral("guid")).toString();
    const QUuid   guid    = QUuid::fromString(guidStr);
    if (guid.isNull()) {
        qWarning() << "[ProjectIndex] missing/invalid top-level guid in"
                   << fileInfo.absoluteFilePath();
        return false;
    }

    const qint64  mtime   = fileInfo.lastModified().toSecsSinceEpoch();
    const qint64  size    = fileInfo.size();
    const QByteArray hash = sha256Hex(bytes);

    const QJsonValue typeValue = obj.value(QStringLiteral("type"));
    const std::optional<ProjectItem::Type> parsedType =
        ItemTypeCatalog::parseTypeName(
            typeValue.toString(), Qt::CaseSensitive, false);
    if (!parsedType) {
        qWarning() << "[ProjectIndex] Unknown pack type" << typeValue.toVariant()
                   << "in" << fileInfo.absoluteFilePath() << "-- skipping file.";
        return false;
    }

    ProjectIndex::Entry top;
    top.guid     = guid;
    top.name     = obj.value(QStringLiteral("name")).toString();
    top.type     = *parsedType;
    top.filePath = relPath;
    top.mtime    = mtime;
    top.size     = size;
    top.hash     = hash;

    if (obj.contains(QStringLiteral("parentRef"))) {
        const QJsonObject pr = obj.value(QStringLiteral("parentRef")).toObject();
        const QString prGuidStr = pr.value(QStringLiteral("id")).toString();
        if (!prGuidStr.isEmpty()) {
            top.parentGuid = QUuid::fromString(prGuidStr);
        }
    }

    outByGuid.insert(guid, top);
    if (!top.name.isEmpty()) {
        outByName[static_cast<int>(top.type)].insert(nameKey(top.type, top.name), guid);
    }

    const QJsonArray children = obj.value(QStringLiteral("children")).toArray();
    if (!children.isEmpty()) {
        std::vector<int> embedPathStack;
        indexChildrenRecursive(children, relPath, mtime, size, hash,
                               embedPathStack, outByGuid, outByName);
    }
    return true;
}

}

ProjectIndex::ProjectIndex()  = default;
ProjectIndex::~ProjectIndex() = default;

bool ProjectIndex::loadFromCache(const QString& projectRoot)
{
    clear();

    const QString path = indexFilePath(projectRoot);
    QFile file(path);
    if (!file.exists()) {
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[ProjectIndex] open-for-read failed:" << path;
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[ProjectIndex] JSON parse failed for cache:" << parseErr.errorString();
        return false;
    }
    const QJsonObject root = doc.object();

    const int storedVersion = root.value(QStringLiteral("version")).toInt(-1);
    if (storedVersion != kIndexFormatVersion) {
        qDebug() << "[ProjectIndex] version mismatch (have"
                 << storedVersion << "want" << kIndexFormatVersion << ")";
        return false;
    }
    if (!root.value(QStringLiteral("by_guid")).isObject()
        || !root.value(QStringLiteral("by_name")).isObject()) {
        qWarning() << "[ProjectIndex] cache entries are invalid:" << path;
        return false;
    }

    const QJsonObject byGuidObj = root.value(QStringLiteral("by_guid")).toObject();
    for (auto it = byGuidObj.constBegin(); it != byGuidObj.constEnd(); ++it) {
        const QUuid guid = QUuid::fromString(it.key());
        const QJsonObject eo = it.value().toObject();
        if (guid.isNull() || !it.value().isObject()
            || !eo.value(QStringLiteral("name")).isString()
            || !eo.value(QStringLiteral("mtime")).isDouble()
            || !eo.value(QStringLiteral("size")).isDouble()
            || !eo.value(QStringLiteral("hash")).isString()) {
            clear();
            return false;
        }

        const int rawType = eo.value(QStringLiteral("type")).toInt(-1);
        const auto cachedType = static_cast<ProjectItem::Type>(rawType);
        if (!ItemTypeCatalog::factsOrNull(cachedType)) {
            qWarning() << "[ProjectIndex] Unknown item type" << rawType
                       << "in cache -- discarding cache.";
            clear();
            return false;
        }

        Entry e;
        e.guid     = guid;
        e.name     = eo.value(QStringLiteral("name")).toString();
        e.type     = cachedType;
        if (eo.contains(QStringLiteral("parent_guid"))) {
            e.parentGuid = QUuid::fromString(eo.value(QStringLiteral("parent_guid")).toString());
        }
        if (eo.contains(QStringLiteral("file_path"))) {
            e.filePath = eo.value(QStringLiteral("file_path")).toString();
        }
        if (eo.contains(QStringLiteral("embed_path"))) {
            if (!eo.value(QStringLiteral("embed_path")).isArray()) {
                clear();
                return false;
            }
            const QJsonArray ep = eo.value(QStringLiteral("embed_path")).toArray();
            e.embedPath.reserve(static_cast<size_t>(ep.size()));
            for (const QJsonValue& v : ep) {
                const int index = v.toInt(-1);
                if (index < 0) {
                    clear();
                    return false;
                }
                e.embedPath.push_back(index);
            }
        }
        e.mtime = static_cast<qint64>(eo.value(QStringLiteral("mtime")).toDouble(0.0));
        e.size  = static_cast<qint64>(eo.value(QStringLiteral("size")).toDouble(0.0));
        e.hash  = eo.value(QStringLiteral("hash")).toString().toUtf8();

        m_byGuid.insert(guid, e);
    }

    const QJsonObject byNameObj = root.value(QStringLiteral("by_name")).toObject();
    for (auto tit = byNameObj.constBegin(); tit != byNameObj.constEnd(); ++tit) {
        bool ok = false;
        const int typeKey = tit.key().toInt(&ok);
        if (!ok) continue;
        const QJsonObject nameMap = tit.value().toObject();
        QMultiHash<QString, QUuid>& bucket = m_byName[typeKey];
        for (auto nit = nameMap.constBegin(); nit != nameMap.constEnd(); ++nit) {
            for (const auto& value : nit.value().toArray()) {
                const QUuid g = QUuid::fromString(value.toString());
                if (!g.isNull()) bucket.insert(nit.key(), g);
            }
        }
    }

    return true;
}

ProjectIndex::ScanResult ProjectIndex::scanProjectRoot(const QString& projectRoot)
{
    m_lock = acquireLock(projectRoot);
    if (!m_lock) return ScanResult::LockBusy;

    struct PriorFileSnapshot {
        QHash<QUuid, Entry> entries;
        qint64              mtime = 0;
        qint64              size  = 0;
    };
    QHash<QString, PriorFileSnapshot> priorByFile;
    for (auto it = m_byGuid.constBegin(); it != m_byGuid.constEnd(); ++it) {
        const Entry& e = it.value();
        if (e.filePath.isEmpty()) continue;
        PriorFileSnapshot& snap = priorByFile[e.filePath];
        snap.entries.insert(e.guid, e);
        if (e.embedPath.empty()) {
            snap.mtime = e.mtime;
            snap.size  = e.size;
        }
    }

    QHash<QUuid, Entry>               nextByGuid;
    QHash<int, QMultiHash<QString, QUuid>> nextByName;

    const QDir projectDir(projectRoot);
    QStringList seenFiles;

    for (const QString& folder : standardHierarchyFolders()) {
        const QString folderAbs = projectDir.filePath(folder);
        if (!QDir(folderAbs).exists()) continue;

        QDirIterator it(folderAbs,
                        QStringList() << QStringLiteral("*.vp"),
                        QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            const QString relPath = relativeForwardSlash(projectRoot, fi.absoluteFilePath());
            seenFiles.push_back(relPath);

            const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
            const qint64 size  = fi.size();

            auto pit = priorByFile.constFind(relPath);
            if (pit != priorByFile.constEnd()
                && pit->mtime == mtime
                && pit->size  == size
                && !pit->entries.isEmpty())
            {
                for (auto eit = pit->entries.constBegin();
                     eit != pit->entries.constEnd(); ++eit)
                {
                    const Entry& e = eit.value();
                    nextByGuid.insert(e.guid, e);
                    if (!e.name.isEmpty()) {
                        nextByName[static_cast<int>(e.type)]
                            .insert(nameKey(e.type, e.name), e.guid);
                    }
                }
                continue;
            }

            indexOnePackFile(projectRoot, fi, nextByGuid, nextByName);
        }
    }


    m_byGuid.swap(nextByGuid);
    m_byName.swap(nextByName);

    m_lock.reset();
    return ScanResult::Ok;
}

bool ProjectIndex::saveToCache(const QString& projectRoot) const
{
    m_lock = acquireLock(projectRoot);
    if (!m_lock) return false;

    QJsonObject root;
    root.insert(QStringLiteral("version"), kIndexFormatVersion);

    QJsonObject byGuidObj;
    for (auto it = m_byGuid.constBegin(); it != m_byGuid.constEnd(); ++it) {
        const Entry& e = it.value();
        QJsonObject eo;
        eo.insert(QStringLiteral("type"), static_cast<int>(e.type));
        eo.insert(QStringLiteral("name"), e.name);
        if (!e.parentGuid.isNull()) {
            eo.insert(QStringLiteral("parent_guid"), guidKey(e.parentGuid));
        }
        if (!e.filePath.isEmpty()) {
            eo.insert(QStringLiteral("file_path"), e.filePath);
        }
        if (!e.embedPath.empty()) {
            QJsonArray ep;
            for (int idx : e.embedPath) ep.append(idx);
            eo.insert(QStringLiteral("embed_path"), ep);
        }
        eo.insert(QStringLiteral("mtime"), static_cast<double>(e.mtime));
        eo.insert(QStringLiteral("size"),  static_cast<double>(e.size));
        eo.insert(QStringLiteral("hash"),  QString::fromUtf8(e.hash));
        byGuidObj.insert(guidKey(e.guid), eo);
    }
    root.insert(QStringLiteral("by_guid"), byGuidObj);

    QJsonObject byNameObj;
    for (auto tit = m_byName.constBegin(); tit != m_byName.constEnd(); ++tit) {
        QJsonObject nameMap;
        const QMultiHash<QString, QUuid>& bucket = tit.value();
        for (auto nit = bucket.constBegin(); nit != bucket.constEnd(); ++nit) {
            QJsonArray matches = nameMap.value(nit.key()).toArray();
            matches.append(guidKey(nit.value()));
            nameMap.insert(nit.key(), matches);
        }
        byNameObj.insert(QString::number(tit.key()), nameMap);
    }
    root.insert(QStringLiteral("by_name"), byNameObj);

    const QString dir     = cacheDir(projectRoot);
    if (!QDir().mkpath(dir)) {
        qWarning() << "[ProjectIndex] mkpath failed:" << dir;
        m_lock.reset();
        return false;
    }
    const QString finalPath = indexFilePath(projectRoot);
    QSaveFile output(finalPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[ProjectIndex] open-for-write failed:" << finalPath;
        m_lock.reset();
        return false;
    }
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (output.write(bytes) != bytes.size() || !output.commit()) {
        qWarning() << "[ProjectIndex] atomic commit failed:" << finalPath;
        m_lock.reset();
        return false;
    }

    m_lock.reset();
    return true;
}

const ProjectIndex::Entry* ProjectIndex::findByGuid(const QUuid& guid) const
{
    auto it = m_byGuid.constFind(guid);
    if (it == m_byGuid.constEnd()) return nullptr;
    return &it.value();
}

const ProjectIndex::Entry* ProjectIndex::findByName(ProjectItem::Type type,
                                                    const QString& name) const
{
    const auto matches = findAllByName(type, name);
    return matches.size() == 1 ? matches.front() : nullptr;
}

std::vector<const ProjectIndex::Entry*> ProjectIndex::findAllByName(
    ProjectItem::Type type, const QString& name) const
{
    std::vector<const Entry*> matches;
    auto tit = m_byName.constFind(static_cast<int>(type));
    if (tit == m_byName.constEnd()) return matches;
    for (const auto& guid : tit->values(nameKey(type, name)))
        if (const auto* entry = findByGuid(guid)) matches.push_back(entry);
    std::sort(matches.begin(), matches.end(), [](const auto* a, const auto* b) {
        return a->guid < b->guid;
    });
    return matches;
}

std::vector<QUuid> ProjectIndex::guidsWithMissingParent() const
{
    std::vector<QUuid> result;
    for (auto it = m_byGuid.constBegin(); it != m_byGuid.constEnd(); ++it) {
        const Entry& e = it.value();
        if (e.parentGuid.isNull()) continue;
        if (!m_byGuid.contains(e.parentGuid)) {
            result.push_back(e.guid);
        }
    }
    return result;
}

int ProjectIndex::entryCount() const
{
    return m_byGuid.size();
}

QByteArray ProjectIndex::contentHash() const
{
    std::vector<QUuid> keys;
    keys.reserve(static_cast<size_t>(m_byGuid.size()));
    for (auto it = m_byGuid.constBegin(); it != m_byGuid.constEnd(); ++it) {
        keys.push_back(it.key());
    }
    std::sort(keys.begin(), keys.end());

    QCryptographicHash h(QCryptographicHash::Sha256);
    for (const auto& g : keys) {
        h.addData(g.toRfc4122());
        const Entry& e = m_byGuid.value(g);
        h.addData(e.hash);
    }
    return h.result().toHex();
}

void ProjectIndex::clear()
{
    m_byGuid.clear();
    m_byName.clear();
}

}
