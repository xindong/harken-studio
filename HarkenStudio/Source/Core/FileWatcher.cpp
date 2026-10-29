
#include "FileWatcher.h"
#include "ItemTypeCatalog.h"
#include "AssetPipeline/AudioFormatCatalog.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QDebug>
#include <QCoreApplication>
#include <QEvent>
#include <utility>

namespace VNX {

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

    projectRoot_ = projectRoot;
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
    restartAfterWriteRoot_.clear();
    debounceTimer_.stop();
    suppressTimer_.stop();
    suppressed_ = false;
    pendingModified_.clear();
    pendingWUModified_.clear();

    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());
    if (!watcher_.files().isEmpty())
        watcher_.removePaths(watcher_.files());

    sourceSnapshot_.clear();
    projectSnapshot_.clear();
    watching_ = false;

    if (wasWatching) qDebug() << "[FileWatcher] Stopped watching";
}

bool FileWatcher::beginProjectWrite(const QStringList& changedPaths)
{
    if (!watching_ || projectWriteActive_) return false;
#ifdef Q_OS_WIN
    projectWriteActive_ = true;
    debounceTimer_.stop();
    suppressTimer_.stop();
    suppressed_ = false;
    pendingModified_.clear();
    pendingWUModified_.clear();
    sourceSnapshot_.clear();
    projectSnapshot_.clear();
    QSet<QString> changed;
    for (const auto& path : changedPaths)
        changed.insert(QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded());
    QStringList removed;
    for (const auto& path : watcher_.files() + watcher_.directories())
        if (changed.contains(QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded()))
            removed.append(path);
    if (removed.isEmpty() || watcher_.removePaths(removed).isEmpty()) return true;
#else
    Q_UNUSED(changedPaths)
#endif
    const QString root = projectRoot_;
    stopWatching();
    restartAfterWriteRoot_ = root;
    projectWriteActive_ = true;
    return true;
}

void FileWatcher::endProjectWrite()
{
    if (!projectWriteActive_) return;
    if (!restartAfterWriteRoot_.isEmpty()) {
        const QString root = std::exchange(restartAfterWriteRoot_, QString{});
        startWatching(root);
        return;
    }
    for (QObject* receiver : watcher_.findChildren<QObject*>())
        QCoreApplication::sendPostedEvents(receiver, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(&watcher_, QEvent::MetaCall);
    sourceSnapshot_ = scanSourceEntries();
    projectSnapshot_ = scanProjectEntries();
    projectWriteActive_ = false;
    refreshWatches();
}

void FileWatcher::setDebounceInterval(int ms)
{
    debounceMs_ = qMax(50, ms);
}

QStringList FileWatcher::sourceFiles() const
{
    QStringList files;
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
    if (!watching_ || projectWriteActive_) return;
    debounceTimer_.start(debounceMs_);
}

void FileWatcher::onFileChanged(const QString& path)
{
    if (!watching_ || projectWriteActive_) return;

    if (sourceSnapshot_.contains(path)) {
        pendingModified_.insert(path);
    } else if (!suppressed_) {
        pendingWUModified_.insert(path);
    }

    if (QFileInfo::exists(path) && !watcher_.files().contains(path))
        watcher_.addPath(path);

    debounceTimer_.start(debounceMs_);
}

void FileWatcher::refreshSources()
{
    if (!watching_ || projectWriteActive_) return;
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
    if (!watching_ || projectWriteActive_) return;
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
