#pragma once

#include "Download/Downloader.h"
#include "Update/UpdateChecker.h"
#include "Update/UpdateStateStore.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QUrl>

namespace VNH {

class HubSelfUpdate : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        QUrl serverBaseUrl;
        QByteArray manifestPublicKey;
        QString currentVersion;
        qint64 checkIntervalMs = UpdateChecker::kDefaultIntervalMs;
        QString downloadDir;
    };

    explicit HubSelfUpdate(QObject* parent = nullptr);

    void configure(const Options& options);
    bool start(const Options& options);
    void stop();
    UpdateStateRecord currentState() const;

    void handleCheckResult(const UpdateCheckResult& result,
                           const QDateTime& now);
    bool approve(QString* error, const QDateTime& now);
    bool approveViaBridge(const QString& restartMode, QString* error,
                          const QDateTime& now);
    bool checkNow();

    static bool selfUpdateEnabled();
    static bool isNewerVersion(const QString& candidate,
                               const QString& current);
    static QString backupRootDir();
    static bool backupUserData(const QDateTime& now, QString* error);
    static bool verifyInstaller(const QString& installerPath,
                                const QString& expectedSha256,
                                qint64 expectedSize, QString* error);
    static void installerLaunchCommand(const QString& installerPath,
                                       QString* program,
                                       QStringList* arguments);
    static bool finalizeAfterRestart(const QString& currentVersion);
    static bool recoverStaleElevation(const QString& currentVersion,
                                      const QDateTime& now);
    static bool reconcileLandingStateAtStartup(const QDateTime& now);

signals:
    void stateChanged(const VNH::UpdateStateRecord& record);

private:
    bool persist(UpdateStateRecord record, const QDateTime& now);
    void handleDownloadFinished(const DownloadResult& result);

    UpdateChecker checker_;
    Downloader downloader_;
    Options options_;
    ReleaseManifest activeManifest_;
    QString activeObjectPath_;
    QElapsedTimer progressPersistTimer_;
    bool downloading_ = false;
};

}
