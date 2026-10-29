#pragma once

#include "Download/Downloader.h"
#include "Security/ManifestVerifier.h"
#include "Update/ServerVersionCatalog.h"

#include <QDialog>
#include <QSet>

#include <vector>

class QLabel;
class QProgressBar;
class QTableWidget;

namespace VNH {

class DialogTextView;
class StudioReleaseNotes;

class ServerVersionsDialog : public QDialog
{
    Q_OBJECT

public:
    ServerVersionsDialog(const QByteArray& manifestPublicKey,
                         const QSet<QString>& installedBuildIds,
                         const QSet<QString>& installedVersions,
                         StudioReleaseNotes* releaseNotes,
                         QWidget* parent = nullptr);

    QString packagePath() const { return packagePath_; }
    QString manifestPath() const { return manifestPath_; }
    std::shared_ptr<DownloadLease> takeDownloadLease() { return std::move(downloadLease_); }

private:
    struct DisplayRow
    {
        ServerVersionEntry entry;
        ReleaseManifest manifest;
        bool installable = false;
        bool installed = false;
    };

    void populate(const ServerVersionList& result);
    void updateReleaseNotes();
    void beginInstall(int rowIndex);
    void startDownload();
    void handleDownloadFinished(const DownloadResult& result);
    void setBusy(bool busy);

    QByteArray manifestPublicKey_;
    QSet<QString> installedBuildIds_;
    QSet<QString> installedVersions_;
    ServerVersionCatalog catalog_;
    StudioReleaseNotes* releaseNotes_ = nullptr;
    Downloader downloader_;
    std::shared_ptr<DownloadLease> downloadLease_;
    std::vector<DisplayRow> rows_;
    QTableWidget* table_ = nullptr;
    DialogTextView* statusLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QString packagePath_;
    QString manifestPath_;
    QString downloadTargetPath_;
    int activeRow_ = -1;
    bool downloadRestarted_ = false;
};

}
