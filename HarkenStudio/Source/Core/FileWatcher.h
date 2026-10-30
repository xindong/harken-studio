
#pragma once

#include <QObject>
#include <QFileSystemWatcher>
#include <QFileDevice>
#include <QTimer>
#include <QSet>
#include <QMap>
#include <QDateTime>
#include <QHash>
#include <QByteArray>
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
    bool isWatching() const { return watching_ && !projectWriteActive_; }
    QStringList sourceFiles() const;
    void refreshSources();
    bool beginProjectWrite(const QString& projectRoot, const QStringList& changedPaths);
    void endProjectWrite(const QHash<QString, QByteArray>& expectedInstalledHashes = {});

    void setDebounceInterval(int ms);
    int debounceInterval() const { return debounceMs_; }

    void suppressSelfWrites(int durationMs = 2000);

signals:
    void filesAdded(const QStringList& paths);
    void filesRemoved(const QStringList& paths);
    void filesModified(const QStringList& paths);
    void sourcesChanged();
    void packExternallyModified(const QString& wuPath);

private slots:
    void onDirectoryChanged(const QString& path);
    void onFileChanged(const QString& path);
    void flushPendingChanges();

private:
    QMap<QString, QPair<qint64, QDateTime>> scanSourceEntries() const;
    QMap<QString, QPair<qint64, QDateTime>> scanProjectEntries() const;
    void refreshWatches();
    bool matchesSelfWrite(const QString& path);

    struct SavedFileState {
        QByteArray hash;
        qint64 size = 0;
        QDateTime modified;
        QFileDevice::Permissions permissions{};
    };

    QFileSystemWatcher watcher_;
    QTimer debounceTimer_;
    QTimer suppressTimer_;

    QString projectRoot_;
    bool watching_ = false;
    bool suppressed_ = false;
    bool projectWriteActive_ = false;
    int debounceMs_ = 500;

    QMap<QString, QPair<qint64, QDateTime>> sourceSnapshot_;
    QMap<QString, QPair<qint64, QDateTime>> projectSnapshot_;
    QSet<QString> pendingModified_;
    QSet<QString> pendingWUModified_;
    QSet<QString> projectWritePaths_;
    QStringList projectWriteTargets_;
    QHash<QString, SavedFileState> selfWriteStates_;
};

}
