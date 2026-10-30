
#include "FileWatcher.h"
#include "BucketFolderStore.h"
#include "ItemTypeCatalog.h"
#include "AssetPipeline/AudioFormatCatalog.h"

#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QCoreApplication>
#include <QEvent>
#include <utility>

namespace VNX {

namespace {

QString pathKey(const QString& path)
{
    return BucketFolderStore::pathComparisonKey(QDir::fromNativeSeparators(path));
}

bool matchesInstalledFile(const QString& path, const QByteArray& expectedHash)
{
    const QFileInfo info(path);
    if (expectedHash.isEmpty()) return !info.exists() && !info.isSymLink();
    if (!info.isFile() || info.isSymLink()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) && hash.result() == expectedHash;
}

}

FileWatcher::FileWatcher(QObject* parent)
    : QObject(parent)
{
    debounceTimer_.setSingleShot(true);
    connect(&debounceTimer_, &QTimer::timeout, this, &FileWatcher::flushPendingChanges);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, &FileWatcher::onDirectoryChanged);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &FileWatcher::onFileChanged);

    suppressTimer_.setSingleShot(true);
    connect(&suppressTimer_, &QTimer::timeout, this, [this]() {
        suppressed_ = false;
        if (changesDeferred_) return;
        pendingWUModified_.clear();
        if (!watching_) return;
        projectSnapshot_ = scanProjectEntries();
        flushPendingChanges();
    });
}

FileWatcher::~FileWatcher()
{
    stopWatching();
}

void FileWatcher::startWatching(const QString& projectRoot)
{
    stopWatching();

    projectRoot_ = QDir::cleanPath(QDir::fromNativeSeparators(projectRoot));
    watching_ = true;

    sourceSnapshot_ = scanSourceEntries();
    projectSnapshot_ = scanProjectEntries();
    refreshWatches();

    qDebug() << "[FileWatcher] Started watching" << projectRoot_
             << "| dirs:" << watcher_.directories().size()
             << "| files:" << watcher_.files().size();
}

void FileWatcher::stopWatching()
{
    const bool wasWatching = watching_;

    projectWriteActive_ = false;
    changesDeferred_ = false;
    debounceTimer_.stop();
    suppressTimer_.stop();
    suppressed_ = false;
    pendingModified_.clear();
    pendingWUModified_.clear();
    projectWritePaths_.clear();
    projectWriteTargets_.clear();
    selfWriteStates_.clear();

    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());
    if (!watcher_.files().isEmpty())
        watcher_.removePaths(watcher_.files());

    sourceSnapshot_.clear();
    projectSnapshot_.clear();
    watching_ = false;

    if (wasWatching) qDebug() << "[FileWatcher] Stopped watching";
}

bool FileWatcher::beginProjectWrite(const QString& projectRoot, const QStringList& changedPaths)
{
    if (!watching_ || projectWriteActive_) return false;
    if (pathKey(projectRoot) != pathKey(projectRoot_)) {
        stopWatching();
        projectRoot_ = QDir::cleanPath(QDir::fromNativeSeparators(projectRoot));
        watching_ = true;
        sourceSnapshot_ = scanSourceEntries();
        projectSnapshot_ = scanProjectEntries();
    }
    projectWriteActive_ = true;
    debounceTimer_.stop();
    suppressTimer_.stop();
    suppressed_ = false;
    QSet<QString> changed;
    projectWritePaths_.clear();
    projectWriteTargets_ = changedPaths;
    const QString rootPrefix = pathKey(projectRoot_) + QLatin1Char('/');
    for (const auto& path : changedPaths) {
        changed.insert(pathKey(path));
        QString ancestor = QDir::cleanPath(QDir::fromNativeSeparators(path));
        while (pathKey(ancestor).startsWith(rootPrefix)) {
            projectWritePaths_.insert(pathKey(ancestor));
            ancestor = QFileInfo(ancestor).absolutePath();
        }
    }
    QStringList removed;
    QStringList changedDirectories;
    for (const auto& path : watcher_.directories()) {
        if (!changed.contains(pathKey(path))) continue;
        removed.append(path);
        changedDirectories.append(pathKey(path) + QLatin1Char('/'));
    }
    for (const auto& path : watcher_.files() + watcher_.directories()) {
        if (removed.contains(path)) continue;
        for (const auto& prefix : changedDirectories)
            if (pathKey(path).startsWith(prefix)) {
                removed.append(path);
                break;
            }
    }
    if (!removed.isEmpty() && !watcher_.removePaths(removed).isEmpty()) {
        const auto watched = watcher_.files() + watcher_.directories();
        if (!watched.isEmpty()) watcher_.removePaths(watched);
    }
    return true;
}

void FileWatcher::endProjectWrite(const QHash<QString, QByteArray>& expectedInstalledHashes)
{
    if (!projectWriteActive_) return;
    for (QObject* receiver : watcher_.findChildren<QObject*>())
        QCoreApplication::sendPostedEvents(receiver, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(&watcher_, QEvent::MetaCall);
    const auto current = scanProjectEntries();
    for (auto it = current.cbegin(); it != current.cend(); ++it)
        if ((!projectSnapshot_.contains(it.key()) || projectSnapshot_.value(it.key()) != it.value())
            && !projectWritePaths_.contains(pathKey(it.key())))
            pendingWUModified_.insert(it.key());
    for (auto it = projectSnapshot_.cbegin(); it != projectSnapshot_.cend(); ++it)
        if (!current.contains(it.key()) && !projectWritePaths_.contains(pathKey(it.key())))
            pendingWUModified_.insert(it.key());
    for (auto it = pendingWUModified_.begin(); it != pendingWUModified_.end();)
        if (projectWritePaths_.contains(pathKey(*it))) it = pendingWUModified_.erase(it);
        else ++it;
    projectSnapshot_ = current;
    for (const auto& path : projectWriteTargets_) {
        const QString key = pathKey(path);
        selfWriteStates_.remove(key);
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && !info.isFile())) continue;
        SavedFileState state;
        if (info.isFile()) {
            const auto expected = expectedInstalledHashes.constFind(path);
            if (expected != expectedInstalledHashes.cend()) {
                if (expected->isEmpty()) continue;
                state.hash = *expected;
            } else {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) continue;
                QCryptographicHash hash(QCryptographicHash::Sha256);
                if (!hash.addData(&file)) continue;
                state.hash = hash.result();
            }
            state.size = info.size();
            state.modified = info.lastModified();
            state.permissions = info.permissions();
        }
        selfWriteStates_.insert(key, state);
    }
    projectWriteActive_ = false;
    projectWritePaths_.clear();
    projectWriteTargets_.clear();
    refreshWatches();
    for (auto file = expectedInstalledHashes.cbegin(); file != expectedInstalledHashes.cend(); ++file)
        if (!matchesInstalledFile(file.key(), file.value())) pendingWUModified_.insert(file.key());
    debounceTimer_.start(debounceMs_);
}

void FileWatcher::setDebounceInterval(int ms)
{
    debounceMs_ = qMax(50, ms);
}

void FileWatcher::deferChanges(bool deferred)
{
    if (changesDeferred_ == deferred) return;
    changesDeferred_ = deferred;
    if (deferred) debounceTimer_.stop();
    else if (watching_) debounceTimer_.start(0);
}

QStringList FileWatcher::sourceFiles() const
{
    QStringList files;
    if (projectWriteActive_) return files;
    for (auto it = sourceSnapshot_.cbegin(); it != sourceSnapshot_.cend(); ++it)
        if (it.value().first >= 0) files.append(it.key());
    return files;
}

void FileWatcher::suppressSelfWrites(int durationMs)
{
    suppressed_ = true;
    pendingWUModified_.clear();
    suppressTimer_.start(qMax(50, durationMs));
}

void FileWatcher::onDirectoryChanged(const QString&)
{
    if (!watching_ || projectWriteActive_ || changesDeferred_) return;
    debounceTimer_.start(debounceMs_);
}

void FileWatcher::onFileChanged(const QString& path)
{
    if (!watching_) return;

    if (sourceSnapshot_.contains(path)) {
        pendingModified_.insert(path);
    } else if (!suppressed_ && !projectWritePaths_.contains(pathKey(path))
        && !matchesSelfWrite(path)) {
        pendingWUModified_.insert(path);
    }

    if (projectWriteActive_ || changesDeferred_) return;

    if (QFileInfo::exists(path) && !watcher_.files().contains(path))
        watcher_.addPath(path);

    debounceTimer_.start(debounceMs_);
}

bool FileWatcher::matchesSelfWrite(const QString& path)
{
    const auto saved = selfWriteStates_.constFind(pathKey(path));
    if (saved == selfWriteStates_.cend()) return false;
    const QFileInfo info(path);
    const bool metadataMatches = saved->hash.isEmpty()
        ? !info.exists() && !info.isSymLink()
        : info.isFile() && !info.isSymLink() && info.size() == saved->size
            && info.lastModified() == saved->modified && info.permissions() == saved->permissions;
    if (metadataMatches && matchesInstalledFile(path, saved->hash)) return true;
    selfWriteStates_.erase(saved);
    return false;
}

void FileWatcher::refreshSources()
{
    if (!watching_ || projectWriteActive_ || changesDeferred_) return;
    const auto sources = scanSourceEntries();
    QStringList added;
    QStringList removed;
    QStringList modified;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
        if (it.value().first < 0) continue;
        if (!sourceSnapshot_.contains(it.key()))
            added.append(it.key());
        else if (sourceSnapshot_.value(it.key()) != it.value() || pendingModified_.contains(it.key()))
            modified.append(it.key());
    }
    for (auto it = sourceSnapshot_.cbegin(); it != sourceSnapshot_.cend(); ++it)
        if (it.value().first >= 0 && !sources.contains(it.key())) removed.append(it.key());
    const bool sourceChanges = sources != sourceSnapshot_ || !modified.isEmpty();
    sourceSnapshot_ = sources;
    pendingModified_.clear();
    refreshWatches();
    if (!removed.isEmpty()) emit filesRemoved(removed);
    if (!added.isEmpty()) emit filesAdded(added);
    if (!modified.isEmpty()) emit filesModified(modified);
    if (sourceChanges) emit sourcesChanged();
}

void FileWatcher::flushPendingChanges()
{
    if (!watching_ || projectWriteActive_ || changesDeferred_) return;
    if (!suppressed_) {
        const auto current = scanProjectEntries();
        for (auto it = current.cbegin(); it != current.cend(); ++it)
            if (!projectSnapshot_.contains(it.key()) || projectSnapshot_.value(it.key()) != it.value())
                pendingWUModified_.insert(it.key());
        for (auto it = projectSnapshot_.cbegin(); it != projectSnapshot_.cend(); ++it)
            if (!current.contains(it.key())) pendingWUModified_.insert(it.key());
        projectSnapshot_ = current;
    }
    refreshSources();

    const auto changed = std::exchange(pendingWUModified_, QSet<QString>{});
    for (const QString& wuPath : changed)
        emit packExternallyModified(wuPath);
}

QMap<QString, QPair<qint64, QDateTime>> FileWatcher::scanProjectEntries() const
{
    QMap<QString, QPair<qint64, QDateTime>> entries;
    const QDir root(projectRoot_);
    for (const auto& file : root.entryInfoList(
             {QStringLiteral("*.hsp"), QStringLiteral("*.vp")}, QDir::Files | QDir::NoSymLinks))
        entries.insert(file.absoluteFilePath(), {file.size(), file.lastModified()});
    for (const auto bucket : ItemTypeCatalog::allBuckets()) {
        const QString directory = root.filePath(ItemTypeCatalog::folderNameOf(bucket));
        if (!QDir(directory).exists()) continue;
        entries.insert(directory, {0, QFileInfo(directory).lastModified()});
        QDirIterator it(directory,
            QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot | QDir::NoSymLinks,
            QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QFileInfo info = it.fileInfo();
            entries.insert(path, {info.isFile() ? info.size() : 0, info.lastModified()});
        }
    }
    return entries;
}

void FileWatcher::refreshWatches()
{
    if (projectWriteActive_) return;
    QSet<QString> watched;
    for (const auto& path : watcher_.directories()) watched.insert(path);
    for (const auto& path : watcher_.files()) watched.insert(path);
    QSet<QString> desired{projectRoot_};
    for (auto it = projectSnapshot_.cbegin(); it != projectSnapshot_.cend(); ++it)
        desired.insert(it.key());
    for (auto it = sourceSnapshot_.cbegin(); it != sourceSnapshot_.cend(); ++it)
        desired.insert(it.key());
    QStringList removed;
    for (const auto& path : watched) {
        if (!desired.contains(path))
            removed.append(path);
    }
    if (!removed.isEmpty()) watcher_.removePaths(removed);
    QStringList added;
    for (const auto& path : desired)
        if (!watched.contains(path)) added.append(path);
    if (!added.isEmpty()) watcher_.addPaths(added);
}

QMap<QString, QPair<qint64, QDateTime>> FileWatcher::scanSourceEntries() const
{
    QMap<QString, QPair<qint64, QDateTime>> entries;
    const QString root = QDir(projectRoot_).filePath(QStringLiteral("AudioSources"));
    if (!QDir(root).exists()) return entries;
    entries.insert(root, {-1, QFileInfo(root).lastModified()});
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo file = it.fileInfo();
        if (file.isDir() || AudioFormatCatalog::containsExtension(file.suffix()))
            entries.insert(path, {file.isDir() ? -1 : file.size(), file.lastModified()});
    }
    return entries;
}

}
