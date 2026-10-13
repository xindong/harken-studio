
#include "FileWatcher.h"
#include "ItemTypeCatalog.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QDebug>
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
        pendingModified_.clear();
        pendingWUModified_.clear();
        if (!watching_) return;
        lastSnapshot_.clear();
        QString AudioSourcesDir = QDir(projectRoot_).filePath("AudioSources");
        if (QDir(AudioSourcesDir).exists())
            scanDirectoryRecursive(AudioSourcesDir, lastSnapshot_);
        projectSnapshot_ = scanProjectEntries();
        refreshWatches();
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

    QString AudioSourcesDir = QDir(projectRoot_).filePath("AudioSources");
    if (QDir(AudioSourcesDir).exists())
        watchDirectoryRecursive(AudioSourcesDir);

    projectSnapshot_ = scanProjectEntries();
    refreshWatches();

    lastSnapshot_.clear();
    if (QDir(AudioSourcesDir).exists())
        scanDirectoryRecursive(AudioSourcesDir, lastSnapshot_);

    qDebug() << "[FileWatcher] Started watching" << projectRoot_
             << "| dirs:" << watcher_.directories().size()
             << "| files:" << watcher_.files().size();
}

void FileWatcher::stopWatching()
{
    const bool wasWatching = watching_;

    debounceTimer_.stop();
    suppressTimer_.stop();
    suppressed_ = false;
    pendingModified_.clear();
    pendingWUModified_.clear();

    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());
    if (!watcher_.files().isEmpty())
        watcher_.removePaths(watcher_.files());

    lastSnapshot_.clear();
    projectSnapshot_.clear();
    watching_ = false;

    if (wasWatching) qDebug() << "[FileWatcher] Stopped watching";
}

void FileWatcher::setDebounceInterval(int ms)
{
    debounceMs_ = qMax(50, ms);
}

void FileWatcher::suppressSelfWrites(int durationMs)
{
    suppressed_ = true;
    pendingModified_.clear();
    pendingWUModified_.clear();
    suppressTimer_.start(qMax(50, durationMs));
}

void FileWatcher::onDirectoryChanged(const QString& path)
{
    if (!watching_) return;
    if (suppressed_) return;

    QSet<QString> currentSnapshot;
    QString AudioSourcesDir = QDir(projectRoot_).filePath("AudioSources");

    if (path == AudioSourcesDir || path.startsWith(AudioSourcesDir + QChar('/'))) {
        scanDirectoryRecursive(AudioSourcesDir, currentSnapshot);

        QSet<QString> added = currentSnapshot - lastSnapshot_;
        QSet<QString> removed = lastSnapshot_ - currentSnapshot;

        if (!added.isEmpty())
            emit filesAdded(added.values());
        if (!removed.isEmpty())
            emit filesRemoved(removed.values());

        lastSnapshot_ = currentSnapshot;

        QFileInfo info(path);
        if (info.isDir() && !watcher_.directories().contains(path))
            watchDirectoryRecursive(path);
    }
    if (!debounceTimer_.isActive())
        debounceTimer_.start(debounceMs_);
}

void FileWatcher::onFileChanged(const QString& path)
{
    if (!watching_) return;

    if (suppressed_) {
        if (QFileInfo::exists(path) && !watcher_.files().contains(path))
            watcher_.addPath(path);
        return;
    }

    if (projectSnapshot_.contains(path)) {
        pendingWUModified_.insert(path);
    } else {
        pendingModified_.insert(path);
    }

    if (QFileInfo::exists(path) && !watcher_.files().contains(path))
        watcher_.addPath(path);

    if (!debounceTimer_.isActive())
        debounceTimer_.start(debounceMs_);
}

void FileWatcher::flushPendingChanges()
{
    if (!watching_ || suppressed_) return;
    const auto current = scanProjectEntries();
    for (auto it = current.cbegin(); it != current.cend(); ++it)
        if (!projectSnapshot_.contains(it.key()) || projectSnapshot_.value(it.key()) != it.value())
            pendingWUModified_.insert(it.key());
    for (auto it = projectSnapshot_.cbegin(); it != projectSnapshot_.cend(); ++it)
        if (!current.contains(it.key())) pendingWUModified_.insert(it.key());
    projectSnapshot_ = current;
    refreshWatches();
    if (!pendingModified_.isEmpty()) {
        emit filesModified(pendingModified_.values());
        pendingModified_.clear();
    }

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
    QSet<QString> watched;
    for (const auto& path : watcher_.directories()) watched.insert(path);
    for (const auto& path : watcher_.files()) watched.insert(path);
    const QString audioSources = QDir(projectRoot_).filePath(QStringLiteral("AudioSources"));
    QStringList removed;
    for (const auto& path : watched) {
        if (path != projectRoot_ && !projectSnapshot_.contains(path)
            && path != audioSources && !path.startsWith(audioSources + QChar('/')))
            removed.append(path);
    }
    if (!removed.isEmpty()) watcher_.removePaths(removed);
    if (!watched.contains(projectRoot_) && QDir(projectRoot_).exists())
        watcher_.addPath(projectRoot_);
    for (auto it = projectSnapshot_.cbegin(); it != projectSnapshot_.cend(); ++it)
        if (!watched.contains(it.key())) watcher_.addPath(it.key());
}

void FileWatcher::scanDirectoryRecursive(const QString& dir, QSet<QString>& snapshot)
{
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        snapshot.insert(it.filePath());
    }
}

void FileWatcher::watchDirectoryRecursive(const QString& dir)
{
    watcher_.addPath(dir);
    QDirIterator it(dir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        watcher_.addPath(it.filePath());
    }
}

}
