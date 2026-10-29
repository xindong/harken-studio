#include "UI/ServerVersionsDialog.h"
#include "UI/DialogLayout.h"

#include "Localization/HubLocalization.h"
#include "Platform/PlatformPaths.h"
#include "ReleaseNotes/VersionReleaseNotes.h"
#include "UI/BoundedHeaderView.h"
#include "Update/ServerEndpoints.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTableWidget>
#include <QVBoxLayout>

namespace VNH {

namespace {

constexpr int kColumnVersion = 0;
constexpr int kColumnReleased = 1;
constexpr int kColumnSize = 2;
constexpr int kColumnChangelog = 3;
constexpr int kColumnAction = 4;

const QString kServerProduct = QStringLiteral("harken");

bool isSafeFileToken(const QString& token)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return pattern.match(token).hasMatch();
}

QString humanSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("-");
    return QLocale::c().formattedDataSize(bytes, 1,
                                          QLocale::DataSizeSIFormat);
}

QString releasedDate(const QString& createdAt)
{
    const QDateTime parsed = QDateTime::fromString(createdAt, Qt::ISODate);
    if (!parsed.isValid())
        return QStringLiteral("-");
    return parsed.date().toString(QStringLiteral("yyyy-MM-dd"));
}

}

ServerVersionsDialog::ServerVersionsDialog(
    const QByteArray& manifestPublicKey,
    const QSet<QString>& installedBuildIds,
    const QSet<QString>& installedVersions,
    VersionReleaseNotes* releaseNotes, QWidget* parent)
    : QDialog(parent)
    , manifestPublicKey_(manifestPublicKey)
    , installedBuildIds_(installedBuildIds)
    , installedVersions_(installedVersions)
    , releaseNotes_(releaseNotes)
{
    setWindowTitle(QCoreApplication::translate("HarkenHub", "Get from Server"));
    setModal(true);
    configureDialog(this, QSize(860, 480), QSize(480, 300));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);

    table_ = new QTableWidget(this);
    auto* tableHeader = new BoundedHeaderView(Qt::Horizontal, table_);
    tableHeader->setFillSection(kColumnChangelog);
    table_->setHorizontalHeader(tableHeader);
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels(
        {QCoreApplication::translate("HarkenHub", "VERSION"), QCoreApplication::translate("HarkenHub", "RELEASED"),
         QCoreApplication::translate("HarkenHub", "SIZE"), QCoreApplication::translate("HarkenHub", "CHANGELOG"), QString()});
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setSectionsClickable(false);
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft |
                                                    Qt::AlignVCenter);
    table_->horizontalHeader()->setSectionResizeMode(kColumnVersion,
                                                     QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(kColumnReleased,
                                                     QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(kColumnSize,
                                                     QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(kColumnChangelog,
                                                     QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(kColumnAction,
                                                     QHeaderView::Fixed);
    table_->horizontalHeader()->setMinimumSectionSize(70);
    table_->setColumnWidth(kColumnVersion, 150);
    table_->setColumnWidth(kColumnReleased, 100);
    table_->setColumnWidth(kColumnSize, 90);
    table_->setColumnWidth(kColumnAction, 130);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(68);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setFocusPolicy(Qt::NoFocus);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    connect(tableHeader, &QHeaderView::sectionResized, this,
            [this](int column, int, int) {
                if (column == kColumnChangelog)
                    resizeReleaseNotesRows();
            }, Qt::QueuedConnection);
    layout->addWidget(table_, 1);

    statusLabel_ = new DialogTextView(this);
    statusLabel_->setText(
        QCoreApplication::translate("HarkenHub", "Loading versions from the server..."));
    layout->addWidget(statusLabel_);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(false);
    progressBar_->hide();
    layout->addWidget(progressBar_);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    auto* closeButton = new QPushButton(QCoreApplication::translate("HarkenHub", "Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    connect(this, &QDialog::rejected, &downloader_, &Downloader::cancel);
    connect(&downloader_, &Downloader::sourceSelected, this,
            [this](const QString&, bool restarted) { downloadRestarted_ = restarted; });
    connect(&catalog_, &ServerVersionCatalog::finished, this,
            &ServerVersionsDialog::populate);
    connect(releaseNotes_, &VersionReleaseNotes::finished, this,
            &ServerVersionsDialog::updateReleaseNotes);
    connect(&downloader_, &Downloader::progress, this,
            [this](qint64 receivedBytes, qint64 totalBytes) {
                if (totalBytes > 0) {
                    progressBar_->setRange(0, 100);
                    progressBar_->setValue(static_cast<int>(
                        receivedBytes * 100 / totalBytes));
                    statusLabel_->setText(
                        QCoreApplication::translate("HarkenHub", "Downloading HarkenStudio %1 - %2 of %3")
                            .arg(rows_[static_cast<size_t>(activeRow_)]
                                     .entry.version,
                                 humanSize(receivedBytes),
                                 humanSize(totalBytes)));
                } else {
                    progressBar_->setRange(0, 0);
                    statusLabel_->setText(QCoreApplication::translate(
                        "HarkenHub", "Downloading HarkenStudio %1...")
                        .arg(rows_[static_cast<size_t>(activeRow_)].entry.version));
                }
                if (downloadRestarted_)
                    statusLabel_->setText(statusLabel_->text() + QCoreApplication::translate(
                        "HarkenHub", " (restarted after switching download source)"));
            });
    connect(&downloader_, &Downloader::finished, this,
            &ServerVersionsDialog::handleDownloadFinished);

    if (!catalog_.fetch(ServerEndpoints::manifestUrls(
            ServerEndpoints::resolvedBaseUrl(), kServerProduct,
            ServerEndpoints::hostChannel()), kServerProduct,
            ServerEndpoints::hostChannel()))
        statusLabel_->setText(
            QCoreApplication::translate("HarkenHub", "The update server is not configured."));
}

void ServerVersionsDialog::populate(const ServerVersionList& result)
{
    if (!result.ok) {
        statusLabel_->setText(
            QCoreApplication::translate("HarkenHub", "Cannot load the version list: %1")
                .arg(result.error));
        return;
    }

    rows_.clear();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const ServerVersionEntry& entry : result.entries) {
        DisplayRow row;
        row.entry = entry;
        if (!entry.updateManifest.isEmpty()) {
            const ManifestVerifyResult verified = ManifestVerifier::verify(
                entry.updateManifest, manifestPublicKey_, now, true);
            if (verified.ok &&
                verified.manifest.product == QStringLiteral("HarkenStudio") &&
                verified.manifest.version == entry.version &&
                isSafeFileToken(verified.manifest.buildId) &&
                isSafeFileToken(verified.manifest.version)) {
                row.manifest = verified.manifest;
                row.installable = true;
            }
        }
        row.installed = row.installable
                            ? installedBuildIds_.contains(row.manifest.buildId)
                            : installedVersions_.contains(entry.version);
        rows_.push_back(std::move(row));
    }

    table_->setRowCount(static_cast<int>(rows_.size()));
    for (int index = 0; index < static_cast<int>(rows_.size()); ++index) {
        DisplayRow& row = rows_[static_cast<size_t>(index)];

        auto* versionItem = new QTableWidgetItem(
            QCoreApplication::translate("HarkenHub", "HarkenStudio %1").arg(row.entry.version));
        QFont nameFont = versionItem->font();
        nameFont.setWeight(QFont::DemiBold);
        versionItem->setFont(nameFont);
        table_->setItem(index, kColumnVersion, versionItem);

        table_->setItem(index, kColumnReleased,
                        new QTableWidgetItem(
                            releasedDate(row.entry.createdAt)));
        table_->setItem(index, kColumnSize,
                        new QTableWidgetItem(humanSize(row.entry.fileSize)));

        auto* notesCell = new QWidget(table_);
        notesCell->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* notesLayout = new QVBoxLayout(notesCell);
        notesLayout->setContentsMargins(12, 8, 12, 8);
        notesLayout->setSpacing(6);
        const auto addNotesLink = [&](VersionReleaseNotes::Product product) {
            auto* link = new QLabel(notesCell);
            link->setWordWrap(true);
            link->setOpenExternalLinks(true);
            link->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                           Qt::LinksAccessibleByKeyboard);
            link->setStyleSheet(QStringLiteral(
                "QLabel { background: transparent; } "
                "QLabel:disabled { color: #6B7488; }"));
            link->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(link, &QWidget::customContextMenuRequested, this,
                    [this, link, version = row.entry.version, product](const QPoint& pos) {
                        const QUrl url = releaseNotes_->url(version, product);
                        if (url.isEmpty())
                            return;
                        QMenu menu(link);
                        QAction* copy = menu.addAction(
                            QCoreApplication::translate("HarkenHub", "Copy Link"));
                        if (menu.exec(link->mapToGlobal(pos)) == copy)
                            QApplication::clipboard()->setText(
                                url.toString(QUrl::FullyEncoded));
                    });
            notesLayout->addWidget(link);
            return link;
        };
        row.studioNotes = addNotesLink(VersionReleaseNotes::Product::Studio);
        row.unityNotes = addNotesLink(VersionReleaseNotes::Product::UnityIntegration);
        table_->setCellWidget(index, kColumnChangelog, notesCell);
        releaseNotes_->fetch(row.entry.version);
        table_->setItem(index, kColumnAction, new QTableWidgetItem());

        auto* holder = new QWidget(table_);
        holder->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* cellLayout = new QHBoxLayout(holder);
        cellLayout->setContentsMargins(0, 4, 12, 4);
        cellLayout->addStretch();
        if (row.installed) {
            auto* label = new QLabel(QCoreApplication::translate("HarkenHub", "Installed"), holder);
            label->setStyleSheet(QStringLiteral(
                "background: transparent; color: #6B7488;"));
            cellLayout->addWidget(label);
        } else if (row.installable) {
            auto* button = new QPushButton(QCoreApplication::translate("HarkenHub", "Install"), holder);
            connect(button, &QPushButton::clicked, this,
                    [this, index]() { beginInstall(index); });
            cellLayout->addWidget(button);
        } else {
            auto* label = new QLabel(QCoreApplication::translate("HarkenHub", "Not installable"),
                                     holder);
            label->setStyleSheet(QStringLiteral(
                "background: transparent; color: #6B7488;"));
            label->setToolTip(QCoreApplication::translate("HarkenHub",
                "The server has no valid signed release manifest for this "
                "version, so it cannot be installed."));
            cellLayout->addWidget(label);
        }
        table_->setCellWidget(index, kColumnAction, holder);
    }
    updateReleaseNotes();

    statusLabel_->setText(
        rows_.empty()
            ? QCoreApplication::translate("HarkenHub", "The server has no versions for this platform.")
            : QCoreApplication::translate("HarkenHub", "%1 versions available").arg(rows_.size()));
}

void ServerVersionsDialog::updateReleaseNotes()
{
    const auto updateLink = [this](QLabel* link, const QString& version,
                                   VersionReleaseNotes::Product product, const QString& text) {
        const QUrl url = releaseNotes_->url(version, product);
        link->setEnabled(!url.isEmpty());
        link->setText(url.isEmpty() ? text : QStringLiteral(
            "<a href=\"%1\" style=\"color: #00F0FF;\">%2</a>")
                .arg(url.toString(QUrl::FullyEncoded).toHtmlEscaped(), text.toHtmlEscaped()));
        link->setToolTip(url.isEmpty()
            ? QCoreApplication::translate("HarkenHub", "No valid web link was found.")
            : url.toDisplayString());
    };
    for (const DisplayRow& row : rows_) {
        updateLink(row.studioNotes, row.entry.version, VersionReleaseNotes::Product::Studio,
                   QCoreApplication::translate("HarkenHub", "Click to view details: Studio"));
        updateLink(row.unityNotes, row.entry.version, VersionReleaseNotes::Product::UnityIntegration,
                   QCoreApplication::translate("HarkenHub", "Click to view details: Unity Integration Package"));
    }
    QMetaObject::invokeMethod(this, &ServerVersionsDialog::resizeReleaseNotesRows,
                              Qt::QueuedConnection);
}

void ServerVersionsDialog::resizeReleaseNotesRows()
{
    for (int index = 0; index < static_cast<int>(rows_.size()); ++index) {
        const DisplayRow& row = rows_[static_cast<size_t>(index)];
        if (!row.studioNotes || !row.unityNotes)
            continue;
        QWidget* cell = row.studioNotes->parentWidget();
        QLayout* layout = cell->layout();
        layout->activate();
        const QMargins margins = layout->contentsMargins();
        row.studioNotes->setMinimumHeight(0);
        row.unityNotes->setMinimumHeight(0);
        const int studioHeight = row.studioNotes->heightForWidth(row.studioNotes->width());
        const int unityHeight = row.unityNotes->heightForWidth(row.unityNotes->width());
        row.studioNotes->setMinimumHeight(studioHeight);
        row.unityNotes->setMinimumHeight(unityHeight);
        const int height = studioHeight + unityHeight + layout->spacing() +
                           margins.top() + margins.bottom() + table_->rowHeight(index) - cell->height();
        table_->setRowHeight(index, qMax(table_->verticalHeader()->defaultSectionSize(), height));
    }
}

void ServerVersionsDialog::setBusy(bool busy)
{
    for (int index = 0; index < table_->rowCount(); ++index) {
        if (QWidget* holder = table_->cellWidget(index, kColumnAction))
            holder->setEnabled(!busy);
    }
    progressBar_->setVisible(busy);
}

void ServerVersionsDialog::beginInstall(int rowIndex)
{
    if (downloader_.isActive() || rowIndex < 0 ||
        rowIndex >= static_cast<int>(rows_.size()))
        return;
    const DisplayRow& row = rows_[static_cast<size_t>(rowIndex)];
    if (!row.installable || row.installed)
        return;

    const QString downloadDir = userDataDir() + QStringLiteral("/downloads/manual");
    if (!QDir().mkpath(downloadDir)) {
        statusLabel_->setText(
            QCoreApplication::translate("HarkenHub", "Cannot create the download directory."));
        return;
    }

    const QString envelopePath = downloadDir + QStringLiteral("/") +
                                 row.manifest.buildId +
                                 QStringLiteral(".manifest");
    downloadLease_.reset();
    QString leaseError;
    downloadLease_ = DownloadLease::acquire(envelopePath, &leaseError);
    if (!downloadLease_) {
        statusLabel_->setText(leaseError);
        return;
    }
    QSaveFile envelopeFile(envelopePath);
    if (!envelopeFile.open(QIODevice::WriteOnly) ||
        envelopeFile.write(row.entry.updateManifest) !=
            row.entry.updateManifest.size() ||
        !envelopeFile.commit()) {
        statusLabel_->setText(
            QCoreApplication::translate("HarkenHub", "Cannot store the release manifest."));
        downloadLease_.reset();
        return;
    }

    activeRow_ = rowIndex;
    manifestPath_ = envelopePath;
    downloadTargetPath_ = downloadDir + QStringLiteral("/") +
                          row.manifest.buildId + QStringLiteral(".zip");
    startDownload();
}

void ServerVersionsDialog::startDownload()
{
    const DisplayRow& row = rows_[static_cast<size_t>(activeRow_)];
    DownloadRequest request;
    QList<QUrl> downloadUrls = ServerEndpoints::downloadUrls(
        ServerEndpoints::resolvedBaseUrl(), row.entry.objectPath,
        row.entry.releaseTag);
    if (downloadUrls.isEmpty()) {
        downloadLease_.reset();
        statusLabel_->setText(QCoreApplication::translate(
            "HarkenHub", "The download did not start."));
        return;
    }
    request.url = downloadUrls.takeFirst();
    request.fallbackUrls = downloadUrls;
    request.targetPath = downloadTargetPath_;
    request.expectedSizeBytes = row.manifest.packageSize;
    request.expectedSha256 = row.manifest.sha256Hex;
    request.lease = downloadLease_;
    QString error;
    if (!downloader_.start(request, &error)) {
        downloadLease_.reset();
        statusLabel_->setText(error.isEmpty()
                                 ? QCoreApplication::translate("HarkenHub", "The download did not start.")
                                 : error);
        return;
    }
    setBusy(true);
    progressBar_->setRange(0, 0);
    statusLabel_->setText(QCoreApplication::translate("HarkenHub", "Downloading HarkenStudio %1...")
                              .arg(row.entry.version));
}

void ServerVersionsDialog::handleDownloadFinished(const DownloadResult& result)
{
    if (result.ok) {
        downloadLease_ = result.lease;
        packagePath_ = result.filePath;
        accept();
        return;
    }

    setBusy(false);
    downloadLease_.reset();
    statusLabel_->setText(
        QCoreApplication::translate("HarkenHub", "Download failed: %1").arg(result.error));
}

}
