#pragma once

#include "Download/Downloader.h"
#include "Update/UpdateChecker.h"
#include "Update/UpdateStateStore.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QUrl>

namespace VNH {

class UpdateCoordinator : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        QUrl serverBaseUrl;
        QByteArray manifestPublicKey;
        qint64 checkIntervalMs = UpdateChecker::kDefaultIntervalMs;
        QString downloadDir;
        QString quarantineDir;
        qint64 bridgePollIntervalMs = 1000;
        qint64 bridgePollTimeoutMs = 15ll * 60 * 1000;
        bool automaticDownloadEnabled = true;
    };

    explicit UpdateCoordinator(QObject* parent = nullptr);

    void configure(const Options& options);
    bool start(const Options& options);
    void stop();
    bool checkNow();
    void setAutomaticDownloadEnabled(bool enabled);
    bool isDownloading() const;
    UpdateStateRecord currentState() const;

    void handleCheckResult(const UpdateCheckResult& result,
                           const QDateTime& now);
    void beginAutoInstall(const QDateTime& now);

signals:
    void stateChanged(const VNH::UpdateStateRecord& record);

private:
    bool persist(UpdateStateRecord record, const QDateTime& now);
    void restoreState(const UpdateStateRecord& record,
                      const UpdateStateRecord& pending);
    void beginDownload(const ReleaseManifest& manifest, const QDateTime& now);
    void handleDownloadFinished(const DownloadResult& result);
    void verifyDownloaded(const QString& packagePath,
                          const ReleaseManifest& manifest, const QDateTime& now);
    void startBridgePoll();
    void pollBridgeOutcome();
    bool isBuildInstalled(const QString& buildId) const;

    UpdateChecker checker_;
    Downloader downloader_;
    Options options_;
    ReleaseManifest activeManifest_;
    QString activeObjectPath_;
    QString activeReleaseTag_;
    QTimer bridgePollTimer_;
    QElapsedTimer progressPersistTimer_;
    qint64 bridgePollElapsedMs_ = 0;
    bool downloading_ = false;
    bool automaticDownloadPauseRequested_ = false;
    bool started_ = false;
};

}
