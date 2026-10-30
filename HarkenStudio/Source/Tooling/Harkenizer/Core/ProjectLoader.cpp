#include "ProjectLoader.h"
#include "ProjectLoaderPrivate.h"

#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/BucketFolderStore.h"
#include "Core/Components/VBus.h"
#include "Core/Components/VSound.h"
#include "Core/FieldGroupDerivedView.h"
#include "Core/FieldGroupEditing.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/FolderSaveState.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/PlaybackReferenceValidation.h"
#include "Core/ProjectContract.h"
#include "Core/ProjectDocument.h"
#include "Core/ProjectIndex.h"
#include "Core/ProjectUpgradeManager.h"
#include "Core/SoundSourceSemantics.h"
#include "Core/VariableSemantics.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <utility>

namespace VNX {
namespace ProjectLoaderInternal {

constexpr const char* kActionsFolder = ItemTypeCatalog::kActionsFolderName;

QString actionsRootDir(const QString& projectRoot)
{
    return QDir(projectRoot).filePath(kActionsFolder);
}

std::shared_ptr<ProjectItem> loadOneActionFile(const QFileInfo& fileInfo,
                                              ProjectSerializer& serializer)
{
    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[ActionLoader] Failed to open" << fileInfo.absoluteFilePath();
        return nullptr;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[ActionLoader] JSON parse failed for"
                   << fileInfo.absoluteFilePath() << ":" << parseErr.errorString();
        return nullptr;
    }
    ProjectDocumentData document;
    QString documentError;
    if (!ProjectDocument::parse(doc.object(), QStringLiteral("project.action"),
                                &document, &documentError)) {
        qWarning() << "[ActionLoader]" << documentError << "in"
                   << fileInfo.absoluteFilePath();
        return nullptr;
    }
    const QJsonObject obj = document.content;

    auto ev = std::make_shared<ProjectItem>(QString(), ProjectItem::Type::Action);
    if (!serializer.actionPackFromJson(obj, *ev)) {
        qWarning() << "[ActionLoader] actionPackFromJson rejected payload in"
                   << fileInfo.absoluteFilePath();
        return nullptr;
    }

    ev->documentHeader = document.header;
    serializer.recordLoadedDocument(fileInfo.absoluteFilePath(), data);

    return ev;
}

bool loadActions(const QString& projectRoot, ProjectData& project,
                 ProjectSerializer& serializer, QString* errOut)
{
    const QString rootDir = actionsRootDir(projectRoot);
    if (!QDir(rootDir).exists()) {
        return true;
    }

    auto folders = BucketFolderStore::scanFolderTree(
        rootDir, QString::fromLatin1(kActionsFolder));
    project.folders.insert(project.folders.end(),
                           std::make_move_iterator(folders.begin()),
                           std::make_move_iterator(folders.end()));

    QDirIterator it(rootDir,
                    QStringList() << QStringLiteral("*.vp"),
                    QDir::Files,
                    QDirIterator::Subdirectories);
    int loaded = 0;
    int failed = 0;
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        auto ev = loadOneActionFile(fi, serializer);
        if (!ev) {
            ++failed;
            continue;
        }
        ev->parent = nullptr;
        ev->parentRef = ParentRef{};

        ProjectFileBinding binding;
        binding.parentFolder = QString::fromLatin1(kActionsFolder);
        binding.rootItem     = std::move(ev);
        PhysicalFolder* folder = project.findFolderByPath(
            binding.parentFolder, QDir(rootDir).relativeFilePath(fi.absolutePath()));
        project.setObjectFileLocation(binding.rootItem.get(), folder, fi.absoluteFilePath());
        project.actions.push_back(std::move(binding));
        ++loaded;
    }

    qDebug() << "[ActionLoader] Loaded" << loaded << "actions as bindings under" << rootDir;
    if (failed > 0) {
        if (errOut)
            *errOut = QStringLiteral("Actions contains %1 unreadable or invalid Pack file(s)")
                          .arg(failed);
        return false;
    }
    return true;
}

const std::vector<SingleObjHierarchySpec>& s3Specs()
{
    static const std::vector<SingleObjHierarchySpec> specs = {
        { QStringLiteral("Attenuations"),    ProjectItem::Type::Attenuation },
        { QLatin1String(ItemTypeCatalog::kVariablesFolderName),
                                             ProjectItem::Type::Variable },
    };
    return specs;
}

QString s3RootDir(const QString& projectRoot, const SingleObjHierarchySpec& spec)
{
    return QDir(projectRoot).filePath(spec.folderName);
}

QString s3DocumentType(const SingleObjHierarchySpec& spec)
{
    return spec.topType == ProjectItem::Type::Attenuation
               ? QStringLiteral("project.attenuation")
               : QStringLiteral("project.variable");
}

void rebindOwner(ProjectItem& node, ProjectItem* parent)
{
    node.parent = parent;
    for (const auto& c : node.children) {
        if (c) rebindOwner(*c, &node);
    }
}

bool loadS3PayloadByType(const SingleObjHierarchySpec& spec,
                         const QJsonObject& obj,
                         ProjectItem& item,
                         ProjectSerializer& serializer)
{
    switch (spec.topType) {
    case ProjectItem::Type::Attenuation:
        return serializer.attenuationPackFromJson(obj, item);
    case ProjectItem::Type::Variable:
        return serializer.variablePackFromJson(obj, item);
    default:
        qWarning() << "[S3Loader] Unhandled topType in dispatch:"
                   << static_cast<int>(spec.topType);
        return false;
    }
}

std::shared_ptr<ProjectItem> loadOneS3File(const SingleObjHierarchySpec& spec,
                                           const QFileInfo& fileInfo,
                                           ProjectSerializer& serializer)
{
    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[S3Loader]" << spec.folderName << "open failed:" << fileInfo.absoluteFilePath();
        return nullptr;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[S3Loader]" << spec.folderName << "JSON parse failed for"
                   << fileInfo.absoluteFilePath() << ":" << parseErr.errorString();
        return nullptr;
    }
    ProjectDocumentData document;
    QString documentError;
    if (!ProjectDocument::parse(doc.object(), s3DocumentType(spec),
                                &document, &documentError)) {
        qWarning() << "[S3Loader]" << documentError << "in"
                   << fileInfo.absoluteFilePath();
        return nullptr;
    }
    const QJsonObject obj = document.content;

    const QString storedType = obj.value("type").toString();
    if (storedType != ItemTypeCatalog::canonicalName(spec.topType)) {
        qWarning() << "[S3Loader] Skipping wrong-typed vp under" << spec.folderName << ":"
                   << fileInfo.absoluteFilePath() << "type=" << storedType;
        return nullptr;
    }

    auto item = std::make_shared<ProjectItem>(QString(), spec.topType);
    if (!loadS3PayloadByType(spec, obj, *item, serializer)) {
        qWarning() << "[S3Loader] serializer rejected payload:" << fileInfo.absoluteFilePath();
        return nullptr;
    }

    item->documentHeader = document.header;

    rebindOwner(*item, nullptr);
    serializer.recordLoadedDocument(fileInfo.absoluteFilePath(), data);
    return item;
}

bool loadS3Hierarchy(const QString& projectRoot,
                     const SingleObjHierarchySpec& spec,
                     ProjectData& project,
                     ProjectSerializer& serializer,
                     QString* errOut)
{
    const QString rootDir = s3RootDir(projectRoot, spec);
    if (!QDir(rootDir).exists()) {
        return true;
    }

    auto folders = BucketFolderStore::scanFolderTree(rootDir, spec.folderName);
    project.folders.insert(project.folders.end(),
                           std::make_move_iterator(folders.begin()),
                           std::make_move_iterator(folders.end()));

    auto* bucket = project.getBucketForFolder(spec.folderName);
    if (!bucket) {
        qCritical() << "[S3Loader] No bucket for folder" << spec.folderName
                    << "-- bindings would be dropped on the floor.";
        return false;
    }

    QDirIterator it(rootDir,
                    QStringList() << QStringLiteral("*.vp"),
                    QDir::Files,
                    QDirIterator::Subdirectories);
    int loaded = 0;
    int failed = 0;
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        auto top = loadOneS3File(spec, fi, serializer);
        if (!top) {
            ++failed;
            continue;
        }
        top->parent = nullptr;
        top->parentRef = ParentRef{};

        ProjectFileBinding binding;
        binding.parentFolder = spec.folderName;
        binding.rootItem     = std::move(top);
        PhysicalFolder* folder = project.findFolderByPath(
            binding.parentFolder, QDir(rootDir).relativeFilePath(fi.absolutePath()));
        project.setObjectFileLocation(binding.rootItem.get(), folder, fi.absoluteFilePath());
        bucket->push_back(std::move(binding));
        ++loaded;
    }
    qDebug() << "[S3Loader]" << spec.folderName << "loaded" << loaded << "top-level objects";
    if (failed > 0) {
        if (errOut)
            *errOut = QStringLiteral("%1 contains %2 unreadable or invalid Pack file(s)")
                          .arg(spec.folderName).arg(failed);
        return false;
    }
    return true;
}

constexpr const char* kBusFolder = ItemTypeCatalog::kBussesFolderName;

QString busFolderAbs(const QString& projectRoot)
{
    return QDir(projectRoot).filePath(kBusFolder);
}

enum class LoadOneBusPackResult { Ok, Skip, Fail };

LoadOneBusPackResult loadOneBusPack(const QFileInfo& fileInfo,
                   ProjectItem& scratchRoot,
                   ProjectData& project,
                   ProjectSerializer& serializer,
                   std::unordered_map<uint32_t, uint32_t>& outParentEdges)
{
    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[BussesPack] open failed:" << fileInfo.absoluteFilePath();
        return LoadOneBusPackResult::Fail;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[BussesPack] JSON parse failed for"
                   << fileInfo.absoluteFilePath() << ":" << perr.errorString();
        return LoadOneBusPackResult::Fail;
    }
    ProjectDocumentData document;
    QString documentError;
    if (!ProjectDocument::parse(doc.object(), QStringLiteral("project.bus"),
                                &document, &documentError)) {
        qWarning() << "[BussesPack]" << documentError << "in"
                   << fileInfo.absoluteFilePath();
        return LoadOneBusPackResult::Fail;
    }
    const QJsonObject root = document.content;
    if (root.value(QStringLiteral("type")).toString() != QStringLiteral("Bus"))
        return LoadOneBusPackResult::Skip;

    ProjectItem* item = serializer.busPackFromJson(
        root, scratchRoot, outParentEdges);
    if (!item) {
        qWarning() << "[BussesPack] Serializer rejected payload:"
                   << fileInfo.absoluteFilePath();
        return LoadOneBusPackResult::Fail;
    }
    item->documentHeader = document.header;
    project.setObjectFileLocation(item, nullptr, fileInfo.absoluteFilePath());
    serializer.recordLoadedDocument(fileInfo.absoluteFilePath(), bytes);
    return LoadOneBusPackResult::Ok;
}

bool loadBussesPacksImpl(const QString& projectRoot,
                              ProjectData& project,
                              ProjectSerializer& serializer,
                              QString* errOut)
{
    project.busses.clear();

    QDir dir(busFolderAbs(projectRoot));
    if (!dir.exists()) {
        return true;
    }

    const QFileInfoList files =
        dir.entryInfoList(QStringList() << QStringLiteral("*.vp"), QDir::Files);

    auto scratchRoot = std::make_shared<ProjectItem>(
        QStringLiteral("__bus_scratch__"), ProjectItem::Type::None);

    std::unordered_map<uint32_t, uint32_t> parentEdges;

    int loaded = 0;
    int failed = 0;
    for (const QFileInfo& fi : files) {
        const LoadOneBusPackResult result =
            loadOneBusPack(fi, *scratchRoot, project, serializer, parentEdges);
        if (result == LoadOneBusPackResult::Fail) {
            if (errOut) {
                *errOut = QStringLiteral("Rejected bus Pack file: %1")
                              .arg(fi.absoluteFilePath());
            }
            return false;
        }
        if (result == LoadOneBusPackResult::Ok)
            ++loaded;
        else
            ++failed;
    }

    auto isEdgeRoot = [&parentEdges](const ProjectItem* p) -> bool {
        return p && parentEdges.find(p->objectID) == parentEdges.end();
    };

    std::unordered_map<uint32_t, std::shared_ptr<ProjectItem>> byObjectID;
    QSet<QString> seenGuids;
    std::shared_ptr<ProjectItem> masterPtr;
    int rootCount = 0;
    for (const auto& child : scratchRoot->children) {
        if (!child) continue;
        if (!child->comp<VNX::VBus>()) continue;
        if (child->objectID == 0 || !byObjectID.emplace(child->objectID, child).second) {
            ++failed;
            if (errOut)
                *errOut = QStringLiteral("Busses contains a missing or duplicate object ID");
            continue;
        }
        const QString guid = child->guid.toString(QUuid::WithoutBraces);
        if (child->guid.isNull() || seenGuids.contains(guid)) {
            ++failed;
            if (errOut)
                *errOut = QStringLiteral("Busses contains a missing or duplicate guid");
            continue;
        }
        seenGuids.insert(guid);
        if (isEdgeRoot(child.get())) {
            ++rootCount;
            if (!masterPtr) masterPtr = child;
        }
    }

    std::function<void(const ProjectItem*)> walkMaxObjId =
        [&](const ProjectItem* node)
    {
        if (!node) return;
        if (node->objectID >= project.nextObjectID)
            project.nextObjectID = node->objectID + 1;
        for (const auto& child : node->children) walkMaxObjId(child.get());
    };
    walkMaxObjId(scratchRoot.get());
    if (project.nextObjectID == 0) project.nextObjectID = 1;

    qDebug() << "[BussesPack] loadBussesPacks scanned" << files.size()
              << "files, loaded" << loaded << "Bus Packs;"
              << "rootCount=" << rootCount
              << "; nextObjectID=" << project.nextObjectID;

    if (loaded != files.size() || failed > 0) {
        if (errOut)
            *errOut = QStringLiteral("Busses contains %1 unreadable or invalid Pack file(s)")
                          .arg(files.size() - loaded + failed);
        return false;
    }

    if (rootCount != 1) {
        if (errOut)
            *errOut = QStringLiteral(
                "Busses has %1 root Bus Packs; exactly 1 expected.")
                .arg(rootCount);
        return false;
    }

    if (masterPtr) {
        std::vector<std::shared_ptr<ProjectItem>> packs(
            scratchRoot->children.begin(), scratchRoot->children.end());
        scratchRoot->children.clear();
        for (auto& pack : packs) {
            if (pack.get() == masterPtr.get()) continue;
            auto eit = parentEdges.find(pack->objectID);
            const uint32_t parentObjId = (eit != parentEdges.end()) ? eit->second : 0u;
            auto pit = byObjectID.find(parentObjId);
            const bool resolved =
                (pit != byObjectID.end() && pit->second.get() != pack.get());
            if (!resolved) {
                qWarning() << "[BussesPack] Bus" << pack->name
                           << "has unresolved parent objectID" << parentObjId;
                if (errOut)
                    *errOut = QStringLiteral("Busses contains an unresolved parent object ID");
                return false;
            }
            ProjectItem* parentItem = pit->second.get();
            pack->parent = parentItem;
            parentItem->children.push_back(pack);
        }
        masterPtr->parent = nullptr;

        ProjectFileBinding binding;
        binding.parentFolder = QStringLiteral("Busses");
        binding.rootItem     = masterPtr;
        project.busses.push_back(std::move(binding));
    }

    return true;
}

QString layersFolderAbs(const QString& projectRoot)
{
    return QDir(projectRoot).filePath(ItemTypeCatalog::kLayersFolderName);
}

bool isLayersBucketRootType(ProjectItem::Type t)
{
    return ItemTypeCatalog::bucketOf(t) == ItemTypeCatalog::Bucket::Layers;
}

bool loadOneLayerPack(const QFileInfo& fileInfo,
                          ProjectSerializer& serializer,
                          std::shared_ptr<ProjectItem>& outLayer,
                          QUuid& outParentGuid,
                          bool& outIsRoot,
                          int& outOrder)
{
    outLayer.reset();
    outParentGuid = QUuid{};
    outIsRoot = false;
    outOrder = std::numeric_limits<int>::max();

    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[LayersPack] open failed:" << fileInfo.absoluteFilePath();
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[LayersPack] JSON parse failed for"
                   << fileInfo.absoluteFilePath() << ":" << perr.errorString();
        return false;
    }
    ProjectDocumentData document;
    QString documentError;
    if (!ProjectDocument::parse(doc.object(), QStringLiteral("project.layer"),
                                &document, &documentError)) {
        qWarning() << "[LayersPack]" << documentError << "in"
                   << fileInfo.absoluteFilePath();
        return false;
    }
    const QJsonObject root = document.content;

    const auto headerType = ItemTypeCatalog::parseTypeName(
        root.value(QStringLiteral("type")).toString(), Qt::CaseSensitive, false);
    if (!headerType || !isLayersBucketRootType(*headerType)) {
        qWarning() << "[LayersPack] Skipping non-Layers-bucket Pack (type="
                   << root.value(QStringLiteral("type")).toString() << "):"
                   << fileInfo.absoluteFilePath();
        return false;
    }

    const QJsonObject dataObj = root;
    if (dataObj.isEmpty()) {
        qWarning() << "[LayersPack] Missing or empty `data` payload in"
                   << fileInfo.absoluteFilePath();
        return false;
    }

    auto layer = std::make_shared<ProjectItem>(QString{}, *headerType);
    if (!serializer.audioPackItemFromJson(dataObj, *layer)) {
        qWarning() << "[LayersPack] audioItemFromJson failed for"
                   << fileInfo.absoluteFilePath();
        return false;
    }

    layer->documentHeader = document.header;
    const auto payloadType = ItemTypeCatalog::parseTypeName(
        dataObj.value(QStringLiteral("type")).toString(),
        Qt::CaseSensitive, false);
    if (!payloadType || !isLayersBucketRootType(*payloadType)) {
        qWarning() << "[LayersPack] Bucket root payload type"
                   << dataObj.value(QStringLiteral("type")).toString()
                   << "is not a Layers bucket root in"
                   << fileInfo.absoluteFilePath() << "-- rejecting.";
        return false;
    }
    if (!root.value(QStringLiteral("guid")).isString()) {
        qWarning() << "[LayersPack] Missing Layer guid in"
                   << fileInfo.absoluteFilePath();
        return false;
    }
    layer->guid = QUuid::fromString(root.value(QStringLiteral("guid")).toString());
    if (layer->guid.isNull()) {
        qWarning() << "[LayersPack] Invalid Layer guid in"
                   << fileInfo.absoluteFilePath();
        return false;
    }
    outOrder = root.value(QStringLiteral("order"))
                   .toInt(std::numeric_limits<int>::max());

    const auto ref = ParentRef::fromJson(root.value(QStringLiteral("parentRef")).toObject());
    if (!ref) {
        qWarning() << "[LayersPack] Unknown parentRef.kind in"
                   << fileInfo.absoluteFilePath() << "-- rejecting.";
        return false;
    }
    if (ref->isRoot()) {
        outIsRoot = true;
    } else if (ref->kind == ParentRef::Kind::Item) {
        outParentGuid = ref->guid;
        if (outParentGuid.isNull()) {
            qWarning() << "[LayersPack] item parentRef missing/invalid id in"
                       << fileInfo.absoluteFilePath();
            return false;
        }
    } else {
        qWarning() << "[LayersPack] Unexpected parentRef.kind in"
                   << fileInfo.absoluteFilePath();
        return false;
    }

    if (outIsRoot) {
        layer->parentRef = ParentRef{};
        layer->parentRef.kind = ParentRef::Kind::Root;
    } else {
        layer->parentRef.kind = ParentRef::Kind::Item;
        layer->parentRef.guid = outParentGuid;
    }

    serializer.recordLoadedDocument(fileInfo.absoluteFilePath(), bytes);
    outLayer = layer;
    return true;
}

bool loadLayersPacksImpl(const QString& projectRoot,
                                ProjectData& project,
                                ProjectSerializer& serializer,
                                QString* errOut,
                                int* failedOut)
{
    const QString rootDir = layersFolderAbs(projectRoot);
    if (!QDir(rootDir).exists()) {
        return true;
    }

    auto folders = BucketFolderStore::scanFolderTree(
        rootDir, QString::fromLatin1(ItemTypeCatalog::kLayersFolderName));
    project.folders.insert(project.folders.end(),
                           std::make_move_iterator(folders.begin()),
                           std::make_move_iterator(folders.end()));

    struct LoadedLayer
    {
        std::shared_ptr<ProjectItem> layer;
        QUuid parentGuid;
        bool isRoot = false;
        int order = std::numeric_limits<int>::max();
    };

    QFileInfoList files;
    QDirIterator iterator(rootDir, QStringList() << QStringLiteral("*.vp"),
                          QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        files.push_back(iterator.fileInfo());
    }

    std::vector<LoadedLayer> loaded;
    loaded.reserve(static_cast<size_t>(files.size()));
    QHash<QString, ProjectItem*> guidToItem;
    guidToItem.reserve(files.size() * 2);

    int failed = 0;
    for (const QFileInfo& fi : files) {
        LoadedLayer ll;
        if (!loadOneLayerPack(fi, serializer, ll.layer, ll.parentGuid, ll.isRoot,
                              ll.order)) {
            ++failed;
            continue;
        }
        if (!ll.layer) {
            ++failed;
            continue;
        }
        const QString key = ll.layer->guid.toString(QUuid::WithoutBraces);
        if (guidToItem.contains(key)) {
            qWarning() << "[LayersPack] Duplicate Layer GUID" << key
                       << "in" << fi.absoluteFilePath();
            ++failed;
            continue;
        }
        project.setObjectFileLocation(ll.layer.get(), project.findFolderByPath(
            QString::fromLatin1(ItemTypeCatalog::kLayersFolderName),
            QDir(rootDir).relativeFilePath(fi.absolutePath())), fi.absoluteFilePath());
        guidToItem.insert(key, ll.layer.get());
        loaded.push_back(std::move(ll));
    }

    int rootCount = 0;
    int childCount = 0;
    int orphanCount = 0;
    for (auto& ll : loaded) {
        if (ll.isRoot) {
            ll.layer->parent = nullptr;
            ++rootCount;
            continue;
        }
        const QString key = ll.parentGuid.toString(QUuid::WithoutBraces);
        auto it = guidToItem.find(key);
        if (it != guidToItem.end()) {
            ProjectItem* parent = it.value();
            ll.layer->parent = parent;
            parent->children.push_back(ll.layer);
            ++childCount;
        } else {
            qWarning() << "[LayersPack] Orphan Layer" << ll.layer->name
                       << "guid=" << ll.layer->guid.toString(QUuid::WithoutBraces)
                       << "missing parent_guid=" << key;
            ++orphanCount;
        }
    }

    QHash<const ProjectItem*, int> orderOf;
    orderOf.reserve(static_cast<int>(loaded.size()));
    for (const auto& ll : loaded)
        orderOf.insert(ll.layer.get(), ll.order);
    const auto byOrder = [&orderOf](const std::shared_ptr<ProjectItem>& a,
                                    const std::shared_ptr<ProjectItem>& b) {
        return orderOf.value(a.get(), std::numeric_limits<int>::max())
             < orderOf.value(b.get(), std::numeric_limits<int>::max());
    };
    for (auto& ll : loaded) {
        std::stable_sort(ll.layer->children.begin(), ll.layer->children.end(),
                         byOrder);
    }

    for (auto& ll : loaded) {
        rebindOwner(*ll.layer, ll.layer->parent);
    }

    std::vector<LoadedLayer*> rootPacks;
    rootPacks.reserve(loaded.size());
    for (auto& ll : loaded)
        if (ll.isRoot) rootPacks.push_back(&ll);
    std::stable_sort(rootPacks.begin(), rootPacks.end(),
                     [](const LoadedLayer* a, const LoadedLayer* b) {
                         return a->order < b->order;
                     });

    for (LoadedLayer* ll : rootPacks) {
        ProjectFileBinding binding;
        binding.parentFolder = QString::fromLatin1(ItemTypeCatalog::kLayersFolderName);
        binding.rootItem     = std::move(ll->layer);
        project.layers.push_back(std::move(binding));
    }

    qDebug() << "[LayersPack] loadLayersPacks scanned" << files.size()
             << "files, loaded" << loaded.size() << "Layers"
             << "(roots=" << rootCount << ", children=" << childCount
             << ", orphans=" << orphanCount << ")";

    if (failedOut)
        *failedOut = failed + orphanCount;
    if (failed > 0) {
        if (errOut)
            *errOut = QStringLiteral("Layers contains %1 unreadable or invalid Pack file(s)")
                          .arg(failed);
        return false;
    }
    if (orphanCount > 0) {
        if (errOut)
            *errOut = QStringLiteral("Layers has %1 orphan Layer(s) -- parent_guid not found.")
                          .arg(orphanCount);
        return false;
    }
    return true;
}

bool validateLoadedProjectIdentities(const ProjectData& project, QString* error)
{
    if (!validateProjectObjectIdentities(project, error))
        return false;
    QSet<QString> guids;
    QSet<uint32_t> objectIds;
    QSet<const ProjectItem*> visited;
    uint32_t maxObjectId = 0;
    QSet<harkenstudio::ObjectId> actionRuntimeIds;

    std::function<bool(const ProjectItem*, const ProjectItem*)> visit =
        [&](const ProjectItem* item, const ProjectItem* expectedParent) {
            if (!item || visited.contains(item)) {
                if (error) *error = QStringLiteral("Project contains a null or cyclic item");
                return false;
            }
            visited.insert(item);
            if (item->parent != expectedParent) {
                if (error) *error = QStringLiteral("Project item parent links are inconsistent");
                return false;
            }
            const QString guid = item->guid.toString(QUuid::WithoutBraces);
            if (item->guid.isNull() || guids.contains(guid)) {
                if (error) *error = QStringLiteral("Project contains a missing or duplicate guid");
                return false;
            }
            guids.insert(guid);
            if (item->objectID == 0 || objectIds.contains(item->objectID)) {
                if (error) *error = QStringLiteral("Project contains a missing or duplicate object ID");
                return false;
            }
            objectIds.insert(item->objectID);
            maxObjectId = std::max(maxObjectId, item->objectID);
            if (item->type == ProjectItem::Type::Action)
                if (!harkenstudio::IsNull(item->runtimeId()))
                    actionRuntimeIds.insert(item->runtimeId());
            for (const auto& child : item->children) {
                if (!visit(child.get(), item)) return false;
            }
            return true;
        };

    if (project.root && !visit(project.root.get(), nullptr)) return false;
    for (const ProjectFileBinding* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem) {
            if (error) *error = QStringLiteral("Project contains an empty file binding");
            return false;
        }
        if (!visit(binding->rootItem.get(), nullptr)) return false;
    }

    if (project.nextObjectID == 0 || project.nextObjectID <= maxObjectId) {
        if (error) *error = QStringLiteral("Project identity counters are behind loaded data");
        return false;
    }
    for (const PackGroup& group : project.packGroups) {
        for (const harkenstudio::ObjectId id : group.actionIds) {
            if (harkenstudio::IsNull(id) || !actionRuntimeIds.contains(id)) {
                if (error) *error = QStringLiteral("Project pack group references an unknown action");
                return false;
            }
        }
    }
    return true;
}

}

PreparedProject::PreparedProject() = default;

PreparedProject::PreparedProject(PreparedProject&& other) noexcept
    : instance(std::move(other.instance)),
      project(std::move(other.project)),
      folderSaveState(std::move(other.folderSaveState)),
      index(std::move(other.index)),
      integrityScanner(std::move(other.integrityScanner)),
      integrityReport(std::move(other.integrityReport)),
      assets(std::move(other.assets)),
      loadedDocuments(std::move(other.loadedDocuments)),
      diagnostics(std::move(other.diagnostics)),
      error(std::move(other.error)),
      rebuildAssetRegistry(other.rebuildAssetRegistry),
      audioLoadFailures(other.audioLoadFailures),
      upgrade(std::move(other.upgrade)),
      upgradePending(other.upgradePending)
{
    other.upgradePending = false;
}

PreparedProject& PreparedProject::operator=(PreparedProject&& other) noexcept
{
    if (this == &other)
        return *this;
    rollbackUpgrade();
    project = std::move(other.project);
    folderSaveState = std::move(other.folderSaveState);
    index = std::move(other.index);
    integrityScanner = std::move(other.integrityScanner);
    integrityReport = std::move(other.integrityReport);
    assets = std::move(other.assets);
    loadedDocuments = std::move(other.loadedDocuments);
    diagnostics = std::move(other.diagnostics);
    error = std::move(other.error);
    rebuildAssetRegistry = other.rebuildAssetRegistry;
    audioLoadFailures = other.audioLoadFailures;
    upgrade = std::move(other.upgrade);
    upgradePending = other.upgradePending;
    other.upgradePending = false;
    instance = std::move(other.instance);
    return *this;
}

PreparedProject::~PreparedProject()
{
    rollbackUpgrade();
}

void PreparedProject::rollbackUpgrade()
{
    if (!upgradePending)
        return;
    QString rollbackError;
    if (!ProjectUpgradeManager().rollback(upgrade, &rollbackError))
        qCritical() << "[ProjectManager] Failed to roll back uncommitted project upgrade:"
                    << rollbackError;
    upgradePending = false;
}

PreparedProject ProjectLoader::prepareProject(const QString& filePath,
                                               bool allowUpgrade,
                                               ProjectUpgradeProgressCallback upgradeProgress,
                                               ProjectUpgradeApprovalCallback upgradeApproval,
                                               std::function<bool()> cancelled,
                                               ProjectContractReadContext* context,
                                               ProjectSerializer::LoadMode mode)
{
    if (cancelled && cancelled())
        return {};
    PreparedProject prepared;
    prepared.project = std::make_unique<ProjectData>();
    ProjectSerializer serializer;
    if (!serializer.loadProject(filePath, *prepared.project, allowUpgrade,
                                std::move(upgradeProgress),
                                std::move(upgradeApproval), context, mode)) {
        prepared.error = serializer.lastError();
        prepared.project.reset();
        return prepared;
    }
    prepared.upgrade = serializer.upgradeResult();
    prepared.upgradePending = prepared.upgrade.ok && prepared.upgrade.changed;
    if (cancelled && cancelled())
        return {};

    const QString projectRoot = QFileInfo(prepared.project->projectPath).absolutePath();
    const bool registryLoaded =
        AssetRegistry::loadSnapshotFromFile(projectRoot, prepared.assets);
    prepared.rebuildAssetRegistry = AssetRegistry::reconcileSnapshot(
        projectRoot, prepared.project->sourceAssets, prepared.assets,
        mode == ProjectSerializer::LoadMode::RecoverTransactions) || !registryLoaded;
    if (cancelled && cancelled())
        return {};

    if (mode == ProjectSerializer::LoadMode::RecoverTransactions) {
        prepared.index = std::make_unique<ProjectIndex>();
        prepared.index->loadFromCache(projectRoot);
        const auto scanResult = prepared.index->scanProjectRoot(projectRoot);
        if (scanResult != ProjectIndex::ScanResult::Ok) {
            qWarning() << "[ProjectManager] ProjectIndex scan returned non-Ok:"
                       << static_cast<int>(scanResult);
        }
    }
    if (cancelled && cancelled())
        return {};

    QString busError;
    if (!ProjectLoaderInternal::loadBussesPacksImpl(projectRoot, *prepared.project, serializer, &busError)) {
        prepared.error = busError.isEmpty()
                             ? QStringLiteral("Failed to load Busses")
                             : busError;
        prepared.project.reset();
        return prepared;
    }
    if (cancelled && cancelled())
        return {};
    QString actionError;
    if (!ProjectLoaderInternal::loadActions(projectRoot, *prepared.project, serializer, &actionError)) {
        prepared.error = actionError.isEmpty()
                             ? QStringLiteral("Failed to load Actions")
                             : actionError;
        prepared.project.reset();
        return prepared;
    }
    for (const auto& spec : ProjectLoaderInternal::s3Specs()) {
        if (cancelled && cancelled())
            return {};
        QString hierarchyError;
        if (!ProjectLoaderInternal::loadS3Hierarchy(projectRoot, spec, *prepared.project, serializer,
                             &hierarchyError)) {
            prepared.error = hierarchyError.isEmpty()
                                 ? QStringLiteral("Failed to load %1").arg(spec.folderName)
                                 : hierarchyError;
            prepared.project.reset();
            return prepared;
        }
    }
    if (cancelled && cancelled())
        return {};

    QString audioError;
    ProjectLoaderInternal::loadLayersPacksImpl(projectRoot, *prepared.project, serializer,
                        &audioError, &prepared.audioLoadFailures);
    if (cancelled && cancelled())
        return {};
    prepared.diagnostics.append(serializer.warnings());
    if (!serializer.lastError().isEmpty() || !audioError.isEmpty()) {
        prepared.error = !serializer.lastError().isEmpty()
            ? serializer.lastError() : audioError;
        prepared.project.reset();
        return prepared;
    }

    QString identityError;
    if (!validateProjectObjectIdentities(*prepared.project, &prepared.error)) {
        prepared.project.reset();
        return prepared;
    }
    if (!ProjectLoaderInternal::validateLoadedProjectIdentities(*prepared.project, &identityError)
        || !ProjectPlatformSemantics::validateProject(*prepared.project, &identityError)) {
        prepared.error = identityError;
        prepared.project.reset();
        return prepared;
    }
    prepared.diagnostics.append(VariableSemantics::restoreLoadedLabelReferences(*prepared.project));
    if (!validatePlaybackReferences(*prepared.project, &prepared.error)) {
        prepared.error = projectIssueCode(ProjectIssue::InvalidPlaybackReference)
            + QStringLiteral(": ") + prepared.error;
        prepared.project.reset();
        return prepared;
    }
    const auto groupValidation = validateFieldGroupProject(*prepared.project);
    if (!groupValidation.ok()) {
        prepared.error = groupValidation.diagnostics.front().message;
        prepared.project.reset();
        return prepared;
    }
    if (cancelled && cancelled())
        return {};
    if (mode == ProjectSerializer::LoadMode::ReadOnly) {
        refreshFieldGroupDerivedViews(*prepared.project);
        return prepared;
    }
    prepared.loadedDocuments = serializer.loadedDocuments();
    prepared.folderSaveState = FolderSaveState::capture(*prepared.project, &prepared.error);
    if (!prepared.folderSaveState) {
        prepared.project.reset();
        return prepared;
    }
    refreshFieldGroupDerivedViews(*prepared.project);
    for (AssetInfo& asset : prepared.assets) {
        if (asset.metadata.fileSize == 0 && !asset.absolutePath.isEmpty())
            asset.metadata.fileSize = static_cast<uint64_t>(QFileInfo(asset.absolutePath).size());
    }


    {
        QHash<uint32_t, AssetInfo*> assets;
        for (auto& asset : prepared.assets) assets.insert(asset.sourceID, &asset);
        QSet<uint32_t> referenced;
        std::function<void(const ProjectItem*, const QString&)> collect =
            [&](const ProjectItem* item, const QString& ownerGuid) {
                if (!item) return;
                const auto* sound = item->type == ProjectItem::Type::Sound
                    ? item->comp<VSound>() : nullptr;
                if (sound) {
                    for (const auto* set : SoundSourceSemantics::sourceSets(sound->sources))
                    for (const SoundSourceDef& source : set->entries) {
                        if (source.sourceId == 0) continue;
                        referenced.insert(source.sourceId);
                        auto* asset = assets.value(source.sourceId);
                        if (asset && asset->ownerObjectGuid.isEmpty())
                            asset->ownerObjectGuid = ownerGuid;
                    }
                }
                for (const auto& child : item->children)
                    collect(child.get(), ownerGuid);
            };
        for (const auto* binding : prepared.project->allFileBindings()) {
            if (!binding || !binding->rootItem)
                continue;
            collect(binding->rootItem.get(),
                    binding->rootItem->guid.toString(QUuid::WithoutBraces));
        }
        QList<uint32_t> sourceIds = referenced.values();
        std::sort(sourceIds.begin(), sourceIds.end());
        for (const uint32_t sourceId : sourceIds) {
            if (!assets.contains(sourceId))
                prepared.diagnostics.append(QStringLiteral(
                    "No persisted file reference for audio source %1. "
                    "Its reference and audio files were preserved.").arg(sourceId));
        }
    }

    if (cancelled && cancelled())
        return {};
    prepared.integrityScanner = std::make_unique<IntegrityScanner>();
    IntegrityScanner::Report cached;
    const bool hasCached = mode == ProjectSerializer::LoadMode::RecoverTransactions
        && prepared.integrityScanner->loadFromCache(projectRoot, cached);
    const QByteArray indexHash = prepared.index->contentHash();
    if (!prepared.rebuildAssetRegistry && hasCached
        && !indexHash.isEmpty() && cached.indexHash == indexHash) {
        prepared.integrityReport = std::move(cached);
    } else {
        prepared.integrityReport =
            prepared.integrityScanner->scan(*prepared.project, prepared.assets);
        prepared.integrityReport.indexHash = indexHash;
    }
    if (cancelled && cancelled())
        return {};

    return prepared;
}

}
