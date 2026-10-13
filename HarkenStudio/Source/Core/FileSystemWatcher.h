
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include "AssetPipeline/AudioFormatCatalog.h"
#include <QTimer>
#include <QSet>
#include <QHash>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <unordered_map>

namespace VNX {

class FileSystemWatcher : public QObject
{
    Q_OBJECT

public:
    enum class ChangeType : uint8_t { Modified, Created, Deleted };

    explicit FileSystemWatcher(QObject* parent = nullptr);
    ~FileSystemWatcher() override;

    void addWatch(const QString& dirPath);
    void removeWatch(const QString& dirPath);

    void start();
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_relaxed); }

    void setDebounceMs(int ms) { debounceMs_ = ms; }
    int debounceMs() const { return debounceMs_; }

    void setMediaExtensions(const QStringList& exts) { mediaExtensions_ = exts; }

signals:
    void mediaSourceChanged(const QString& path, VNX::FileSystemWatcher::ChangeType type);
    void watchError(const QString& message);

private slots:
    void flushPendingEvents();

private:
    struct PendingEvent {
        QString path;
        ChangeType type;
    };

    void watchThread();
    bool isMediaFile(const QString& path) const;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex watchMutex_;
    std::vector<QString> watchDirs_;

    std::mutex eventMutex_;
    std::vector<PendingEvent> pendingEvents_;

    QTimer debounceTimer_;
    int debounceMs_ = 300;

    QStringList mediaExtensions_ = AudioFormatCatalog::extensions();

#ifdef _WIN32
    void* stopEvent_ = nullptr;
#elif defined(__linux__)
    int inotifyFd_ = -1;
    int stopPipe_[2] = {-1, -1};
    std::unordered_map<int, QString> wdToDirMap_;
#else
    QHash<QString, qint64> fileSnapshot_;
#endif
};

}
