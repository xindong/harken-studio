
#include "AssetRegistry.h"
#include "AssetImporter.h"
#include "PlatformProfile.h"
#include "PathUtils.h"
#include "../ProjectDataPolicy.h"
#include "../ProjectLocator.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QDebug>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSaveFile>

namespace VNX {

AssetRegistry& AssetRegistry::instance()
{
    static AssetRegistry instance;
    return instance;
}

AssetRegistry::AssetRegistry(QObject* parent)
    : QObject(parent)
{
}


void AssetRegistry::setProjectRoot(const QString& root)
{
    if (root.isEmpty()) {
        qCritical() << "[AssetRegistry] ERROR: projectRoot is empty!";
        qCritical() << "[AssetRegistry] Ensure ProjectManager::loadProject() is called first.";
        return;
    }

    if (!QFileInfo(root).isAbsolute()) {
        qCritical() << "[AssetRegistry] ERROR: projectRoot must be absolute path!";
        qCritical() << "[AssetRegistry] Received:" << root;
        return;
    }

    if (!QDir(root).exists()) {
        qWarning() << "[AssetRegistry] WARNING: projectRoot does not exist, will create:" << root;
    }

    qDebug() << "[AssetRegistry] Setting project root:" << root;
    QMutexLocker lock(&mutex_);
    session_ = QUuid::createUuid();
    platforms_.clear();
    platformRevisions_.clear();
    projectRoot_ = root;

    QString AudioSourcesDir = PathUtils::getAudioSourcesDir(projectRoot_);
    QString cacheDir = PathUtils::getCacheDir(projectRoot_);
    QString assetRegistryDir = PathUtils::getAssetRegistryCacheDir(projectRoot_);
    if (!PathUtils::ensureDirectory(AudioSourcesDir)
        || !PathUtils::ensureDirectory(cacheDir)
        || !PathUtils::ensureDirectory(assetRegistryDir)) {
        qCritical() << "[AssetRegistry] Failed to initialize project directories";
        projectRoot_.clear();
        return;
    }

    qDebug() << "[AssetRegistry] Directories initialized:";
    qDebug() << "  AudioSources:" << AudioSourcesDir;
    qDebug() << "  Cache:" << cacheDir;
    qDebug() << "  AssetRegistry:" << assetRegistryDir;
}

void AssetRegistry::clear()
{
    QMutexLocker lock(&mutex_);
    session_ = QUuid::createUuid();
    platforms_.clear();
    platformRevisions_.clear();
    assets_.clear();
    pathToID_.clear();
    uncommittedIDs_.clear();
}


uint32_t AssetRegistry::allocateStableID()
{
    QMutexLocker lock(&mutex_);
    uint32_t id = 0;
    do {
        id = QRandomGenerator::global()->generate();
    } while (id == 0 || assets_.contains(id));
    return id;
}


bool AssetRegistry::registerAsset(const AssetInfo& info, bool uncommitted)
{
    QMutexLocker lock(&mutex_);
    if (info.sourceID == 0) {
        qWarning() << "Cannot register asset with sourceID 0";
        return false;
    }

    if (assets_.contains(info.sourceID)) {
        const AssetInfo& existing = assets_[info.sourceID];
        if (existing.relativePath != info.relativePath) {
            qWarning() << "Cannot register: ID collision detected";
            return false;
        }
    }

    const QString path = PathUtils::normalizeToUnix(info.relativePath);
    for (auto it = assets_.cbegin(); it != assets_.cend(); ++it) {
        if (it.key() != info.sourceID && PathUtils::normalizeToUnix(it->relativePath).compare(path,
#ifdef Q_OS_WIN
                Qt::CaseInsensitive
#else
                Qt::CaseSensitive
#endif
            ) == 0) {
            qWarning() << "Cannot register: source path already belongs to another asset";
            return false;
        }
    }

    assets_[info.sourceID] = info;
    updatePathIndex(info);
    if (uncommitted)
        uncommittedIDs_.insert(info.sourceID);

    lock.unlock();
    emit assetRegistered(info.sourceID);
    return true;
}

void AssetRegistry::unregisterAsset(uint32_t sourceID)
{
    QMutexLocker lock(&mutex_);
    if (!assets_.contains(sourceID)) {
        return;
    }

    const AssetInfo info = assets_[sourceID];
    pathToID_.remove(info.relativePath);
    assets_.remove(sourceID);
    uncommittedIDs_.remove(sourceID);

    lock.unlock();
    emit assetUnregistered(sourceID);
}

void AssetRegistry::markAsOrphaned(uint32_t sourceID)
{
    QMutexLocker lock(&mutex_);
    if (!assets_.contains(sourceID)) {
        return;
    }

    assets_[sourceID].isOrphaned = true;
}

void AssetRegistry::restoreOrphaned(uint32_t sourceID)
{
    QMutexLocker lock(&mutex_);
    if (!assets_.contains(sourceID)) {
        return;
    }

    assets_[sourceID].isOrphaned = false;
}


AssetInfo* AssetRegistry::getAsset(uint32_t sourceID)
{
    QMutexLocker lock(&mutex_);
    auto it = assets_.find(sourceID);
    return it != assets_.end() ? &it.value() : nullptr;
}

const AssetInfo* AssetRegistry::getAsset(uint32_t sourceID) const
{
    QMutexLocker lock(&mutex_);
    auto it = assets_.find(sourceID);
    return it != assets_.end() ? &it.value() : nullptr;
}

AssetInfo* AssetRegistry::getAssetByPath(const QString& relativePath)
{
    QMutexLocker lock(&mutex_);
    QString normalized = PathUtils::normalizeToUnix(relativePath);
    auto it = pathToID_.find(normalized);
    if (it != pathToID_.end()) {
        auto assetIt = assets_.find(it.value());
        return assetIt != assets_.end() ? &assetIt.value() : nullptr;
    }
    return nullptr;
}

const AssetInfo* AssetRegistry::getAssetByPath(const QString& relativePath) const
{
    QMutexLocker lock(&mutex_);
    QString normalized = PathUtils::normalizeToUnix(relativePath);
    auto it = pathToID_.find(normalized);
    if (it != pathToID_.end()) {
        auto assetIt = assets_.find(it.value());
        return assetIt != assets_.end() ? &assetIt.value() : nullptr;
    }
    return nullptr;
}

const AssetInfo* AssetRegistry::findAssetByContentHash(const QString& contentHash) const
{
    QMutexLocker lock(&mutex_);
    if (contentHash.isEmpty()) {
        return nullptr;
    }

    for (auto it = assets_.constBegin(); it != assets_.constEnd(); ++it) {
        const AssetInfo& info = it.value();
        if (info.isOrphaned) {
            continue;
        }
        if (info.contentHash == contentHash) {
            return &info;
        }
    }
    return nullptr;
}

const AssetInfo* AssetRegistry::findAssetByPath(const QString& absolutePath) const
{
    QMutexLocker lock(&mutex_);

    if (!projectRoot_.isEmpty()) {
        QString AudioSourcesDir = PathUtils::getAudioSourcesDir(projectRoot_);
        if (absolutePath.startsWith(AudioSourcesDir, Qt::CaseInsensitive)) {
            QString relativePath = PathUtils::toRelativePath(absolutePath, AudioSourcesDir);
            QString normalized = PathUtils::normalizeToUnix(relativePath);
            auto pathIt = pathToID_.find(normalized);
            if (pathIt != pathToID_.end()) {
                auto assetIt = assets_.find(pathIt.value());
                if (assetIt != assets_.end()) return &assetIt.value();
            }
        }
    }

    for (auto it = assets_.constBegin(); it != assets_.constEnd(); ++it) {
        if (it.value().absolutePath.compare(absolutePath,
#ifdef Q_OS_WIN
                Qt::CaseInsensitive
#else
                Qt::CaseSensitive
#endif
            ) == 0) {
            return &it.value();
        }
    }
    return nullptr;
}

bool AssetRegistry::contains(uint32_t sourceID) const
{
    QMutexLocker lock(&mutex_);
    return assets_.contains(sourceID);
}

QList<uint32_t> AssetRegistry::getAllSourceIDs() const
{
    QMutexLocker lock(&mutex_);
    return assets_.keys();
}

QList<const AssetInfo*> AssetRegistry::getAllAssets() const
{
    QMutexLocker lock(&mutex_);
    QList<const AssetInfo*> result;
    result.reserve(assets_.size());
    for (auto it = assets_.begin(); it != assets_.end(); ++it) {
        result.append(&it.value());
    }
    return result;
}

bool AssetRegistry::sourceFileExists(uint32_t sourceID) const
{
    QMutexLocker lock(&mutex_);
    const auto asset = assets_.constFind(sourceID);
    if (asset == assets_.cend()) return false;
    const QString path = asset->absolutePath.isEmpty()
        ? PathUtils::toAbsolutePath(asset->relativePath, PathUtils::getAudioSourcesDir(projectRoot_))
        : asset->absolutePath;
    return QFileInfo(path).isFile();
}

QList<uint32_t> AssetRegistry::getMissingAssets() const
{
    QMutexLocker lock(&mutex_);
    QList<uint32_t> missing;

    for (auto it = assets_.begin(); it != assets_.end(); ++it) {
        const AssetInfo& info = it.value();
        QString absPath = PathUtils::toAbsolutePath(info.relativePath,
                                                     PathUtils::getAudioSourcesDir(projectRoot_));
        if (!QFileInfo::exists(absPath)) {
            missing.append(it.key());
        }
    }

    return missing;
}


bool AssetRegistry::updateSourceFile(uint32_t sourceID, const AudioMetadata& metadata,
                                     const QString& absolutePath,
                                     const QString& contentHash,
                                     uint64_t lastModified)
{
    QMutexLocker lock(&mutex_);
    auto it = assets_.find(sourceID);
    if (it == assets_.end())
        return false;
    AssetInfo& asset = it.value();
    const bool changed = !audioMetadataEqual(asset.metadata, metadata)
        || asset.contentHash != contentHash;
    asset.metadata = metadata;
    asset.absolutePath = absolutePath;
    asset.contentHash = contentHash;
    asset.lastModified = lastModified;
    if (changed) {
        lock.unlock();
        emit assetModified(sourceID);
    }
    return changed;
}


void AssetRegistry::relocateSourcePaths(const QString& oldPath, const QString& newPath)
{
    QMutexLocker lock(&mutex_);
    QList<uint32_t> changed;
    const QString prefix = oldPath + QLatin1Char('/');
    for (auto& asset : assets_) {
        if (asset.relativePath != oldPath && !asset.relativePath.startsWith(prefix))
            continue;
        pathToID_.remove(asset.relativePath);
        asset.relativePath = newPath + asset.relativePath.mid(oldPath.size());
        asset.absolutePath = QDir(PathUtils::getAudioSourcesDir(projectRoot_)).filePath(asset.relativePath);
        updatePathIndex(asset);
        changed.append(asset.sourceID);
    }
    lock.unlock();
    for (uint32_t id : changed)
        emit assetModified(id);
}

QString AssetRegistry::projectRoot() const
{
    QMutexLocker lock(&mutex_);
    return projectRoot_;
}

void AssetRegistry::setPlatforms(const QVector<ProjectPlatform>& platforms)
{
    QMutexLocker lock(&mutex_);
    QHash<ProjectPlatformId, QUuid> revisions;
    for (const auto& platform : platforms) {
        const auto* previous = ProjectPlatformSemantics::find(platforms_, platform.id);
        const bool unchanged = previous && previous->basePlatform == platform.basePlatform
            && previous->profile.toJson() == platform.profile.toJson();
        revisions.insert(platform.id, unchanged ? platformRevisions_.value(platform.id)
                                               : QUuid::createUuid());
    }
    platforms_ = platforms;
    platformRevisions_ = revisions;
    for (auto& asset : assets_) {
        for (auto it = asset.platformCaches.begin(); it != asset.platformCaches.end();) {
            if (!revisions.contains(it.key())) it = asset.platformCaches.erase(it);
            else ++it;
        }
    }
}

MediaBuildSnapshot AssetRegistry::buildSnapshot() const
{
    QString root;
    QVector<ProjectPlatform> platforms;
    {
        QMutexLocker lock(&mutex_);
        root = projectRoot_;
        platforms = platforms_;
    }
    return buildSnapshot(root, platforms);
}

MediaBuildSnapshot AssetRegistry::buildSnapshot(
    const QString& root, const QVector<ProjectPlatform>& platforms) const
{
    MediaBuildSnapshot result;
    result.projectRoot = root;
    result.platforms = platforms;
    {
        QMutexLocker lock(&mutex_);
        if (QDir::cleanPath(root) == QDir::cleanPath(projectRoot_)) {
            result.registrySession = session_;
            result.platformRevisions = platformRevisions_;
            result.assets = assets_;
            result.fileOverrides = PlatformProfileManager::instance().fileOverrides();
        }
    }
    if (result.registrySession.isNull()) {
        QList<AssetInfo> assets;
        loadSnapshotFromFile(root, assets);
        const auto location = ProjectLocator::locate(root);
        QFile projectFile(location.projectFilePath);
        if (location.ok() && projectFile.open(QIODevice::ReadOnly)) {
            const QJsonObject content = QJsonDocument::fromJson(projectFile.readAll()).object()
                                            .value(QStringLiteral("content")).toObject();
            QMap<uint32_t, QString> references;
            if (sourceAssetsFromJson(content.value(QStringLiteral("sourceAssets")), references))
                reconcileSnapshot(root, references, assets);
            else
                assets.clear();
        }
        for (const auto& asset : assets) result.assets.insert(asset.sourceID, asset);
    }
    for (auto& asset : result.assets) {
        if (asset.contentHash.isEmpty())
            asset.contentHash = AssetImporter::computeContentHash(asset.absolutePath);
    }
    return result;
}

std::optional<MediaConversionRequest> AssetRegistry::conversionRequest(
    uint32_t sourceID, ProjectPlatformId platform, QString* error) const
{
    QMutexLocker lock(&mutex_);
    const auto* definition = ProjectPlatformSemantics::find(platforms_, platform);
    if (!definition) {
        if (error) *error = QStringLiteral("Platform ID does not belong to the current project");
        return std::nullopt;
    }
    const auto asset = assets_.constFind(sourceID);
    if (asset == assets_.cend()) {
        if (error) *error = QStringLiteral("Asset not found in the current project");
        return std::nullopt;
    }
    MediaConversionRequest request{projectRoot_, session_, platformRevisions_.value(platform),
        *definition, *asset, PlatformProfileManager::effectiveSettingsForAsset(
            *asset, *definition, PlatformProfileManager::instance().getFileOverride(sourceID))};
    lock.unlock();
    if (request.asset.contentHash.isEmpty())
        request.asset.contentHash = AssetImporter::computeContentHash(request.asset.absolutePath);
    return request;
}

bool AssetRegistry::accepts(const MediaConversionRequest& request) const
{
    const auto current = conversionRequest(request.asset.sourceID, request.platform.id);
    return current && !request.registrySession.isNull() && !request.platformRevision.isNull()
        && current->registrySession == request.registrySession
        && current->platformRevision == request.platformRevision
        && current->asset.absolutePath == request.asset.absolutePath
        && current->fingerprint() == request.fingerprint();
}

bool AssetRegistry::isCacheValid(uint32_t sourceID, ProjectPlatformId platform) const
{
    const auto request = conversionRequest(sourceID, platform);
    return request && request->cacheValid();
}

bool AssetRegistry::updateCache(const MediaConversionRequest& request, const CacheEntry& entry)
{
    if (!accepts(request)) return false;
    QMutexLocker lock(&mutex_);
    auto it = assets_.find(request.asset.sourceID);
    if (it == assets_.end()) return false;
    it->platformCaches[request.platform.id] = entry;
    lock.unlock();
    emit assetModified(request.asset.sourceID);
    return true;
}

void AssetRegistry::touchCache(uint32_t sourceID, ProjectPlatformId platform)
{
    QMutexLocker lock(&mutex_);
    auto it = assets_.find(sourceID);
    if (it == assets_.end()) {
        return;
    }

    auto cacheIt = it.value().platformCaches.find(platform);
    if (cacheIt != it.value().platformCaches.end()) {
        cacheIt->lastAccessTime = QDateTime::currentSecsSinceEpoch();
    }
}

void AssetRegistry::invalidateCache(const QString& cachePath)
{
    QMutexLocker lock(&mutex_);
    const QString normalized = PathUtils::normalizeToUnix(QDir::isAbsolutePath(cachePath)
        ? QDir(PathUtils::getCacheDir(projectRoot_)).relativeFilePath(cachePath) : cachePath);
    QList<QPair<uint32_t, ProjectPlatformId>> invalidated;
    for (auto& info : assets_) {
        for (auto it = info.platformCaches.begin(); it != info.platformCaches.end(); ++it) {
            if (it->cachePath == normalized) {
                it->valid = false;
                invalidated.append({info.sourceID, it.key()});
            }
        }
    }
    lock.unlock();
    for (const auto& [sourceID, platform] : invalidated)
        emit cacheInvalidated(sourceID, platform);
}

void AssetRegistry::invalidateAllCaches(uint32_t sourceID)
{
    QMutexLocker lock(&mutex_);
    auto it = assets_.find(sourceID);
    if (it == assets_.end()) {
        return;
    }

    for (auto cacheIt = it->platformCaches.begin();
         cacheIt != it->platformCaches.end(); ++cacheIt) {
        cacheIt->valid = false;
        emit cacheInvalidated(sourceID, cacheIt.key());
    }
}


QString AssetRegistry::assetsFilePath() const
{
    return QDir(PathUtils::getAssetRegistryCacheDir(projectRoot_)).absoluteFilePath("assets.json");
}

bool AssetRegistry::saveToFile(const QString& path)
{
    QMutexLocker lock(&mutex_);
    QString filePath = path.isEmpty() ? assetsFilePath() : path;
    if (!PathUtils::ensureDirectory(QFileInfo(filePath).absolutePath()))
        return false;

    QJsonObject root;
    root["version"] = kFormatVersion;

    QJsonObject assetsObj;
    for (auto it = assets_.begin(); it != assets_.end(); ++it) {
        const AssetInfo& info = it.value();
        assetsObj[QString::number(it.key())] = info.toJson();
    }
    root["assets"] = assetsObj;

    QJsonDocument doc(root);
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Failed to save asset registry:" << file.errorString();
        return false;
    }

    const QByteArray bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        qWarning() << "Failed to commit asset registry:" << file.errorString();
        return false;
    }

    if (path.isEmpty()) {
        uncommittedIDs_.clear();
    }
    return true;
}

bool AssetRegistry::loadFromFile(const QString& path)
{
    QList<AssetInfo> assets;
    if (!loadSnapshotFromFile(projectRoot_, assets, path))
        return false;
    replaceSnapshot(projectRoot_, assets);
    qDebug() << "Loaded" << assets_.size() << "assets from registry";
    return true;
}

bool AssetRegistry::loadSnapshotFromFile(const QString& projectRoot,
                                         QList<AssetInfo>& assets,
                                         const QString& path)
{
    const QString filePath = path.isEmpty()
        ? QDir(PathUtils::getAssetRegistryCacheDir(projectRoot)).filePath("assets.json")
        : path;

    QFile file(filePath);
    if (!file.exists()) {
        return false;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to load asset registry:" << file.errorString();
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        qWarning() << "Invalid asset registry format";
        return false;
    }

    QJsonObject root = doc.object();
    if (!root.value(QStringLiteral("assets")).isObject()) {
        qWarning() << "Invalid asset registry entries:" << filePath;
        return false;
    }
    int version = root["version"].toInt();
    if (version != kFormatVersion) {
        qWarning() << "Unsupported asset registry version:" << version;
        return false;
    }

    QList<AssetInfo> loadedAssets;
    QSet<uint32_t> loadedIDs;
    QSet<QString> loadedPaths;
    QJsonObject assetsObj = root["assets"].toObject();
    for (auto it = assetsObj.begin(); it != assetsObj.end(); ++it) {
        bool validID = false;
        const uint32_t sourceID = it.key().toUInt(&validID);
        if (!validID || sourceID == 0 || !it.value().isObject()
            || loadedIDs.contains(sourceID))
            return false;
        const QJsonObject object = it.value().toObject();
        AssetInfo info = AssetInfo::fromJson(object);
        const QString relativePath = ProjectDataPolicy::normalizeRelativePath(info.relativePath);
        if (object.value(QStringLiteral("sourceID")).toInteger(-1) != sourceID
            || !ProjectDataPolicy::isSafeRelativePath(info.relativePath)
            || loadedPaths.contains(relativePath))
            return false;

        info.absolutePath = PathUtils::toAbsolutePath(
            info.relativePath,
            PathUtils::getAudioSourcesDir(projectRoot)
        );

        loadedAssets.append(info);
        loadedIDs.insert(sourceID);
        loadedPaths.insert(relativePath);
    }

    assets = std::move(loadedAssets);
    return true;
}


bool AssetRegistry::sourceAssetsFromJson(const QJsonValue& value,
                                         QMap<uint32_t, QString>& references)
{
    if (!value.isObject() && !value.isUndefined()) return false;
    QMap<uint32_t, QString> parsed;
    QSet<QString> paths;
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool validId = false;
        const uint32_t id = it.key().toUInt(&validId);
        if (!validId || id == 0 || QString::number(id) != it.key()
            || !it->isString() || !ProjectDataPolicy::isSafeRelativePath(it->toString()))
            return false;
        const QString path = ProjectDataPolicy::normalizeRelativePath(it->toString());
#if defined(Q_OS_WIN)
        const QString key = path.toCaseFolded();
#else
        const QString key = path;
#endif
        if (paths.contains(key)) return false;
        parsed.insert(id, path);
        paths.insert(key);
    }
    references = std::move(parsed);
    return true;
}

bool AssetRegistry::reconcileSnapshot(const QString& projectRoot,
                                      const QMap<uint32_t, QString>& references,
                                      QList<AssetInfo>& assets)
{
    QHash<uint32_t, AssetInfo> cached;
    for (const auto& asset : assets) cached.insert(asset.sourceID, asset);
    bool rebuilt = cached.size() != references.size();
    QList<AssetInfo> resolved;
    AssetImporter importer;
    const QDir audioRoot(PathUtils::getAudioSourcesDir(projectRoot));
    for (auto it = references.cbegin(); it != references.cend(); ++it) {
        const auto found = cached.constFind(it.key());
        AssetInfo info;
        if (found != cached.cend() && found->relativePath == it.value()) {
            info = *found;
        } else {
            rebuilt = true;
            info.sourceID = it.key();
            info.relativePath = it.value();
            info.absolutePath = audioRoot.filePath(info.relativePath);
            const QFileInfo file(info.absolutePath);
            if (file.isFile()) {
                info.lastModified = file.lastModified().toSecsSinceEpoch();
                info.metadata = importer.analyzeFile(info.absolutePath);
                info.contentHash = AssetImporter::computeContentHash(info.absolutePath);
            }
        }
        resolved.append(info);
    }
    assets = std::move(resolved);
    return rebuilt;
}

QList<AssetInfo> AssetRegistry::snapshot() const
{
    QMutexLocker lock(&mutex_);
    return assets_.values();
}

void AssetRegistry::replaceSnapshot(const QString& projectRoot,
                                    const QList<AssetInfo>& assets)
{
    QMutexLocker lock(&mutex_);
    session_ = QUuid::createUuid();
    projectRoot_ = projectRoot;
    assets_.clear();
    pathToID_.clear();
    uncommittedIDs_.clear();
    for (AssetInfo info : assets) {
        const QString audioSourcesDir =
            QDir(projectRoot_).filePath(QString::fromLatin1(PathUtils::AUDIO_SOURCES_DIR));
        info.absolutePath = PathUtils::toAbsolutePath(info.relativePath, audioSourcesDir);
        assets_.insert(info.sourceID, info);
        updatePathIndex(info);
    }
}

bool AssetRegistry::saveSnapshotToFile(const QString& projectRoot,
                                       const QList<AssetInfo>& assets)
{
    const QString filePath = QDir(PathUtils::getAssetRegistryCacheDir(projectRoot))
                                 .filePath(QStringLiteral("assets.json"));
    if (!PathUtils::ensureDirectory(QFileInfo(filePath).absolutePath()))
        return false;
    QJsonObject root;
    root[QStringLiteral("version")] = kFormatVersion;
    QJsonObject entries;
    for (const AssetInfo& info : assets)
        entries[QString::number(info.sourceID)] = info.toJson();
    root[QStringLiteral("assets")] = entries;

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Failed to save asset registry snapshot:" << file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size() && file.commit();
}


bool AssetRegistry::isUncommitted(uint32_t sourceID) const
{
    QMutexLocker lock(&mutex_);
    return uncommittedIDs_.contains(sourceID);
}

void AssetRegistry::markAllCommitted()
{
    QMutexLocker lock(&mutex_);
    uncommittedIDs_.clear();
}

int AssetRegistry::discardUncommittedFiles()
{
    QMutexLocker lock(&mutex_);

    int removed = 0;
    const QList<uint32_t> ids = uncommittedIDs_.values();
    for (uint32_t id : ids) {
        auto it = assets_.find(id);
        if (it == assets_.end()) {
            continue;
        }

        const AssetInfo& info = it.value();
        if (!info.absolutePath.isEmpty() && QFile::exists(info.absolutePath)) {
            if (QFile::remove(info.absolutePath)) {
                ++removed;
            }
        }
        for (auto cacheIt = info.platformCaches.constBegin();
             cacheIt != info.platformCaches.constEnd(); ++cacheIt) {
            QString cachePath = PathUtils::toAbsolutePath(
                cacheIt->cachePath, PathUtils::getCacheDir(projectRoot_));
            if (QFile::exists(cachePath)) {
                QFile::remove(cachePath);
            }
        }

        pathToID_.remove(info.relativePath);
        assets_.remove(id);
    }

    uncommittedIDs_.clear();
    return removed;
}

void AssetRegistry::cleanupOrphaned(const QList<QString>& files)
{
    // Holds mutex_ for the whole pass; look up assets_ / pathToID_ directly
    // because the public accessors lock the same non-recursive mutex.
    QMutexLocker lock(&mutex_);
    for (const QString& relativePath : files) {
        QString absPath = PathUtils::toAbsolutePath(relativePath,
                                                     PathUtils::getAudioSourcesDir(projectRoot_));
        if (PathUtils::moveToTrash(projectRoot_, absPath, relativePath)) {
            qDebug() << "Moved orphaned file to trash:" << absPath;
        }

        const QString normalized = PathUtils::normalizeToUnix(relativePath);
        auto pathIt = pathToID_.find(normalized);
        if (pathIt != pathToID_.end()) {
            const uint32_t sourceID = pathIt.value();
            auto assetIt = assets_.find(sourceID);
            if (assetIt != assets_.end()) {
                const AssetInfo& info = assetIt.value();
                for (auto it = info.platformCaches.begin();
                     it != info.platformCaches.end(); ++it) {
                    QString cachePath = PathUtils::toAbsolutePath(
                        it->cachePath,
                        PathUtils::getCacheDir(projectRoot_)
                    );
                    if (QFile::exists(cachePath)) {
                        QFile::remove(cachePath);
                    }
                }

                assets_.erase(assetIt);
            }
            pathToID_.erase(pathIt);
            uncommittedIDs_.remove(sourceID);
        }
    }
}

void AssetRegistry::updatePathIndex(const AssetInfo& info)
{
    QString normalized = PathUtils::normalizeToUnix(info.relativePath);
    pathToID_[normalized] = info.sourceID;
}

}
