
#include "FileSystemWatcher.h"
#include <QDateTime>
#include <QDirIterator>
#include <QFileInfo>
#include <QMetaObject>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#endif

namespace VNX {

FileSystemWatcher::FileSystemWatcher(QObject* parent)
    : QObject(parent)
{
    debounceTimer_.setSingleShot(true);
    connect(&debounceTimer_, &QTimer::timeout, this, &FileSystemWatcher::flushPendingEvents);
}

FileSystemWatcher::~FileSystemWatcher()
{
    stop();
}

void FileSystemWatcher::addWatch(const QString& dirPath)
{
    std::lock_guard<std::mutex> lock(watchMutex_);
    for (const auto& d : watchDirs_)
        if (d == dirPath) return;
    watchDirs_.push_back(dirPath);
}

void FileSystemWatcher::removeWatch(const QString& dirPath)
{
    std::lock_guard<std::mutex> lock(watchMutex_);
    watchDirs_.erase(
        std::remove(watchDirs_.begin(), watchDirs_.end(), dirPath),
        watchDirs_.end());
}

void FileSystemWatcher::start()
{
    if (running_.load(std::memory_order_relaxed))
        return;

#ifdef _WIN32
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return;
#elif defined(__linux__)
    stopPipe_[0] = -1;
    stopPipe_[1] = -1;
#endif

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&FileSystemWatcher::watchThread, this);
}

void FileSystemWatcher::stop()
{
    if (!running_.load(std::memory_order_relaxed))
        return;

    running_.store(false, std::memory_order_release);

#ifdef _WIN32
    if (stopEvent_) {
        SetEvent(static_cast<HANDLE>(stopEvent_));
    }
#elif defined(__linux__)
    if (stopPipe_[1] >= 0) {
        char c = 'q';
        (void)write(stopPipe_[1], &c, 1);
    }
#endif

    if (thread_.joinable())
        thread_.join();

#ifdef _WIN32
    if (stopEvent_) {
        CloseHandle(static_cast<HANDLE>(stopEvent_));
        stopEvent_ = nullptr;
    }
#elif defined(__linux__)
    if (stopPipe_[0] >= 0) { close(stopPipe_[0]); stopPipe_[0] = -1; }
    if (stopPipe_[1] >= 0) { close(stopPipe_[1]); stopPipe_[1] = -1; }
#endif
}

bool FileSystemWatcher::isMediaFile(const QString& path) const
{
    QFileInfo fi(path);
    QString ext = fi.suffix().toLower();
    for (const auto& me : mediaExtensions_)
        if (ext == me) return true;
    return false;
}

void FileSystemWatcher::flushPendingEvents()
{
    std::vector<PendingEvent> events;
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        events.swap(pendingEvents_);
    }

    QSet<QString> seen;
    for (const auto& evt : events)
    {
        if (seen.contains(evt.path))
            continue;
        seen.insert(evt.path);
        emit mediaSourceChanged(evt.path, evt.type);
    }
}

#ifdef _WIN32

void FileSystemWatcher::watchThread()
{
    std::vector<QString> dirs;
    {
        std::lock_guard<std::mutex> lock(watchMutex_);
        dirs = watchDirs_;
    }

    if (dirs.empty()) {
        running_.store(false);
        return;
    }

    struct DirHandle {
        HANDLE hDir = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped = {};
        alignas(DWORD) char buffer[4096] = {};
        QString dirPath;
    };

    std::vector<std::unique_ptr<DirHandle>> handles;
    std::vector<HANDLE> waitHandles;
    waitHandles.push_back(static_cast<HANDLE>(stopEvent_));

    for (const auto& dir : dirs)
    {
        auto dh = std::make_unique<DirHandle>();
        dh->dirPath = dir;
        std::wstring wdir = dir.toStdWString();

        dh->hDir = CreateFileW(
            wdir.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);

        if (dh->hDir == INVALID_HANDLE_VALUE) {
            QMetaObject::invokeMethod(this, [this, dir]() {
                emit watchError(QString("Cannot open directory: %1").arg(dir));
            }, Qt::QueuedConnection);
            continue;
        }

        dh->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        BOOL ok = ReadDirectoryChangesW(
            dh->hDir, dh->buffer, sizeof(dh->buffer), TRUE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION,
            nullptr, &dh->overlapped, nullptr);

        if (!ok) {
            CloseHandle(dh->overlapped.hEvent);
            CloseHandle(dh->hDir);
            continue;
        }

        waitHandles.push_back(dh->overlapped.hEvent);
        handles.push_back(std::move(dh));
    }

    while (running_.load(std::memory_order_relaxed))
    {
        DWORD result = WaitForMultipleObjects(
            static_cast<DWORD>(waitHandles.size()),
            waitHandles.data(), FALSE, 1000);

        if (result == WAIT_OBJECT_0)
            break;

        if (result == WAIT_TIMEOUT)
            continue;

        DWORD idx = result - WAIT_OBJECT_0 - 1;
        if (idx >= handles.size())
            continue;

        auto& dh = handles[idx];
        DWORD bytesTransferred = 0;
        if (!GetOverlappedResult(dh->hDir, &dh->overlapped, &bytesTransferred, FALSE))
            continue;

        if (bytesTransferred > 0)
        {
            const char* ptr = dh->buffer;
            for (;;)
            {
                auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);
                int nameLen = static_cast<int>(info->FileNameLength / sizeof(WCHAR));
                QString fileName = QString::fromWCharArray(info->FileName, nameLen);
                QString fullPath = dh->dirPath + "/" + fileName;
                fullPath.replace('\\', '/');

                if (isMediaFile(fullPath))
                {
                    ChangeType ct = ChangeType::Modified;
                    if (info->Action == FILE_ACTION_ADDED ||
                        info->Action == FILE_ACTION_RENAMED_NEW_NAME)
                        ct = ChangeType::Created;
                    else if (info->Action == FILE_ACTION_REMOVED ||
                             info->Action == FILE_ACTION_RENAMED_OLD_NAME)
                        ct = ChangeType::Deleted;

                    {
                        std::lock_guard<std::mutex> lock(eventMutex_);
                        pendingEvents_.push_back({fullPath, ct});
                    }
                    QMetaObject::invokeMethod(&debounceTimer_, [this]() {
                        if (!debounceTimer_.isActive())
                            debounceTimer_.start(debounceMs_);
                    }, Qt::QueuedConnection);
                }

                if (info->NextEntryOffset == 0)
                    break;
                ptr += info->NextEntryOffset;
            }
        }

        ResetEvent(dh->overlapped.hEvent);
        ReadDirectoryChangesW(
            dh->hDir, dh->buffer, sizeof(dh->buffer), TRUE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION,
            nullptr, &dh->overlapped, nullptr);
    }

    for (auto& dh : handles)
    {
        CancelIo(dh->hDir);
        CloseHandle(dh->overlapped.hEvent);
        CloseHandle(dh->hDir);
    }
}

#elif defined(__linux__)

void FileSystemWatcher::watchThread()
{
    inotifyFd_ = inotify_init1(IN_NONBLOCK);
    if (inotifyFd_ < 0) {
        running_.store(false);
        return;
    }

    if (pipe(stopPipe_) != 0) {
        close(inotifyFd_);
        inotifyFd_ = -1;
        running_.store(false);
        return;
    }

    std::vector<QString> dirs;
    {
        std::lock_guard<std::mutex> lock(watchMutex_);
        dirs = watchDirs_;
    }

    for (const auto& dir : dirs)
    {
        QByteArray pathBytes = dir.toUtf8();
        int wd = inotify_add_watch(inotifyFd_, pathBytes.constData(),
                                    IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO);
        if (wd >= 0)
            wdToDirMap_[wd] = dir;
    }

    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (running_.load(std::memory_order_relaxed))
    {
        struct pollfd fds[2];
        fds[0].fd = inotifyFd_;
        fds[0].events = POLLIN;
        fds[1].fd = stopPipe_[0];
        fds[1].events = POLLIN;

        int ret = poll(fds, 2, 1000);
        if (ret <= 0) continue;

        if (fds[1].revents & POLLIN)
            break;

        if (fds[0].revents & POLLIN)
        {
            ssize_t len = read(inotifyFd_, buf, sizeof(buf));
            if (len <= 0) continue;

            const char* ptr = buf;
            while (ptr < buf + len)
            {
                auto* event = reinterpret_cast<const struct inotify_event*>(ptr);
                if (event->len > 0)
                {
                    auto it = wdToDirMap_.find(event->wd);
                    if (it != wdToDirMap_.end())
                    {
                        QString fileName = QString::fromUtf8(event->name);
                        QString fullPath = it->second + "/" + fileName;

                        if (isMediaFile(fullPath))
                        {
                            ChangeType ct = ChangeType::Modified;
                            if (event->mask & IN_CREATE || event->mask & IN_MOVED_TO)
                                ct = ChangeType::Created;
                            else if (event->mask & IN_DELETE)
                                ct = ChangeType::Deleted;

                            {
                                std::lock_guard<std::mutex> lock(eventMutex_);
                                pendingEvents_.push_back({fullPath, ct});
                            }
                            QMetaObject::invokeMethod(&debounceTimer_, [this]() {
                                if (!debounceTimer_.isActive())
                                    debounceTimer_.start(debounceMs_);
                            }, Qt::QueuedConnection);
                        }
                    }
                }
                ptr += sizeof(struct inotify_event) + event->len;
            }
        }
    }

    for (auto& [wd, _] : wdToDirMap_)
        inotify_rm_watch(inotifyFd_, wd);
    wdToDirMap_.clear();
    close(inotifyFd_);
    inotifyFd_ = -1;
}

#else

void FileSystemWatcher::watchThread()
{
    std::vector<QString> dirs;
    {
        std::lock_guard<std::mutex> lock(watchMutex_);
        dirs = watchDirs_;
    }

    auto scan = [this, &dirs]() {
        QHash<QString, qint64> snapshot;
        for (const auto& dir : dirs)
        {
            QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext())
            {
                const QString path = it.next();
                if (!isMediaFile(path))
                    continue;
                const QFileInfo fi(path);
                snapshot.insert(path, fi.lastModified().toMSecsSinceEpoch());
            }
        }
        return snapshot;
    };

    fileSnapshot_ = scan();

    while (running_.load(std::memory_order_relaxed))
    {
        for (int i = 0; i < 20 && running_.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!running_.load(std::memory_order_relaxed))
            break;

        QHash<QString, qint64> nextSnapshot = scan();
        bool changed = false;

        for (auto it = nextSnapshot.constBegin(); it != nextSnapshot.constEnd(); ++it)
        {
            auto old = fileSnapshot_.constFind(it.key());
            if (old == fileSnapshot_.constEnd() || old.value() != it.value())
            {
                std::lock_guard<std::mutex> lock(eventMutex_);
                pendingEvents_.push_back({
                    it.key(),
                    old == fileSnapshot_.constEnd() ? ChangeType::Created : ChangeType::Modified
                });
                changed = true;
            }
        }

        for (auto it = fileSnapshot_.constBegin(); it != fileSnapshot_.constEnd(); ++it)
        {
            if (!nextSnapshot.contains(it.key()))
            {
                std::lock_guard<std::mutex> lock(eventMutex_);
                pendingEvents_.push_back({it.key(), ChangeType::Deleted});
                changed = true;
            }
        }

        fileSnapshot_ = std::move(nextSnapshot);
        if (changed)
        {
            QMetaObject::invokeMethod(&debounceTimer_, [this]() {
                if (!debounceTimer_.isActive())
                    debounceTimer_.start(debounceMs_);
            }, Qt::QueuedConnection);
        }
    }
}

#endif

}
