
#pragma once

#include <QObject>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QSet>
#include <QMap>
#include <QDateTime>
#include <QString>
#include <QStringList>

namespace VNX {

class FileWatcher : public QObject
{
    Q_OBJECT

public:
    explicit FileWatcher(QObject* parent = nullptr);
    ~FileWatcher() override;

    void startWatching(const QString& projectRoot);
    void stopWatching();
    bool isWatching() const { return watching_; }

    void setDebounceInterval(int ms);
    int debounceInterval() const { return debounceMs_; }

    void suppressSelfWrites(int durationMs = 2000);

signals:
    void filesAdded(const QStringList& paths);
    void filesRemoved(const QStringList& paths);
    void filesModified(const QStringList& paths);
    void packExternallyModified(const QString& wuPath);

private slots:
    void onDirectoryChanged(const QString& path);
    void onFileChanged(const QString& path);
    void flushPendingChanges();

private:
    void scanDirectoryRecursive(const QString& dir, QSet<QString>& snapshot);
    void watchDirectoryRecursive(const QString& dir);
    QMap<QString, QPair<qint64, QDateTime>> scanProjectEntries() const;
    void refreshWatches();

    QFileSystemWatcher watcher_;
    QTimer debounceTimer_;
    QTimer suppressTimer_;

    QString projectRoot_;
    bool watching_ = false;
    bool suppressed_ = false;
    int debounceMs_ = 500;

    QSet<QString> lastSnapshot_;
    QMap<QString, QPair<qint64, QDateTime>> projectSnapshot_;
    QSet<QString> pendingModified_;
    QSet<QString> pendingWUModified_;
};

}
