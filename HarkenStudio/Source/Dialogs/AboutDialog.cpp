#include "AboutDialog.h"

#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "harken_buildinfo.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QSizePolicy>
#include <QStringList>
#include <QVBoxLayout>

#include <string_view>

namespace {

constexpr int kDialogWidth = 500;

QString buildInfoString(std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

QString versionString()
{
    const QString applicationVersion = QCoreApplication::applicationVersion();
    return applicationVersion.isEmpty()
               ? buildInfoString(Harken::BuildInfo::Version)
               : applicationVersion;
}

QString buildDate()
{
    const QString timestamp = buildInfoString(Harken::BuildInfo::Timestamp);
    const QDateTime dateTime = QDateTime::fromString(timestamp, Qt::ISODate);
    return dateTime.isValid() ? dateTime.date().toString(Qt::ISODate)
                              : timestamp.left(10);
}

void addSummaryRow(QGridLayout* layout, int row, const QString& name,
                   const QString& value, const QString& objectName,
                   QWidget* parent)
{
    auto* nameLabel = new QLabel(name, parent);
    nameLabel->setProperty("aboutRole", QStringLiteral("key"));
    layout->addWidget(nameLabel, row, 1);

    auto* valueLabel = new QLabel(value, parent);
    valueLabel->setObjectName(objectName);
    valueLabel->setProperty("aboutRole", QStringLiteral("value"));
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    valueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(valueLabel, row, 2);
}

}

namespace VNX {

AboutDialog::AboutDialog(QWidget* parent, bool showDeveloperInformation)
    : BaseDialog(parent)
    , showDeveloperInformation_(showDeveloperInformation)
{
    setObjectName(QStringLiteral("aboutDialog"));
    setWindowTitle(tr("About Harken Studio"));
    setModal(true);
    setMinimumWidth(kDialogWidth);
    resize(kDialogWidth, showDeveloperInformation ? 560 : 330);

    const QString currentVersion = versionString();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 22, 28, 18);
    root->setSpacing(0);

    auto* logo = new QLabel(this);
    logo->setObjectName(QStringLiteral("aboutLogo"));
    logo->setAccessibleName(QStringLiteral("Harken Studio"));
    const QSize logoSize(400, 32);
    logo->setFixedSize(logoSize);
    logo->setAlignment(Qt::AlignCenter);
    const auto resolvedLogo = ThemeManager::instance().resolveImage(
        QString::fromLatin1(ThemeAssetIds::AppLogo));
    if (resolvedLogo.resource) {
        QPixmap pixmap = QPixmap::fromImage(resolvedLogo.resource->image).scaled(
            logoSize * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        logo->setPixmap(pixmap);
    }
    root->addWidget(logo);
    root->addSpacing(12);

    auto* tagline = new QLabel(
        tr("Professional Audio Middleware Editor"), this);
    tagline->setObjectName(QStringLiteral("aboutTagline"));
    tagline->setWordWrap(true);
    root->addWidget(tagline);
    root->addSpacing(20);

    auto* summary = new QFrame(this);
    summary->setObjectName(QStringLiteral("aboutSummary"));
    auto* summaryLayout = new QGridLayout(summary);
    summaryLayout->setContentsMargins(16, 12, 16, 12);
    summaryLayout->setHorizontalSpacing(24);
    summaryLayout->setVerticalSpacing(8);
    summaryLayout->setColumnStretch(0, 1);
    summaryLayout->setColumnMinimumWidth(1, 48);
    summaryLayout->setColumnStretch(3, 1);
    addSummaryRow(summaryLayout, 0, tr("Version"), currentVersion,
                  QStringLiteral("aboutVersionValue"), summary);
    addSummaryRow(summaryLayout, 1, tr("Date"), buildDate(),
                  QStringLiteral("aboutDateValue"), summary);
    root->addWidget(summary);

    if (showDeveloperInformation) {
        root->addSpacing(20);

        auto* sectionTitle = new QLabel(tr("Developer Information"), this);
        sectionTitle->setObjectName(QStringLiteral("aboutDeveloperTitle"));
        root->addWidget(sectionTitle);
        root->addSpacing(8);

        QStringList diagnostics{
            tr("Build ID: %1").arg(buildInfoString(Harken::BuildInfo::BuildId)),
            tr("Build Type: %1 (%2)")
                .arg(buildInfoString(Harken::BuildInfo::BuildType),
                     buildInfoString(Harken::BuildInfo::BuildTypeValues)),
            tr("Build Channel: %1 (%2)")
                .arg(buildInfoString(Harken::BuildInfo::BuildChannel),
                     buildInfoString(Harken::BuildInfo::BuildChannelValues)),
            tr("Commit: %1").arg(buildInfoString(Harken::BuildInfo::GitCommit)),
            tr("Source State: %1").arg(
                Harken::BuildInfo::GitDirty ? tr("Modified") : tr("Clean")),
            tr("Platform: %1").arg(
                buildInfoString(Harken::BuildInfo::TargetPlatform)),
            tr("Timestamp (UTC): %1").arg(
                buildInfoString(Harken::BuildInfo::Timestamp)),
            tr("Qt Version: %1").arg(QString::fromLatin1(qVersion())),
        };
        const QString ciRun = buildInfoString(Harken::BuildInfo::CIRunNumber);
        if (!ciRun.isEmpty()) {
            diagnostics.append(tr("CI Run: %1").arg(ciRun));
            diagnostics.append(tr("CI Attempt: %1").arg(
                buildInfoString(Harken::BuildInfo::CIRunAttempt)));
        }
        diagnosticsText_ = diagnostics.join(QLatin1Char('\n'));

        auto* developerInfo = new QPlainTextEdit(diagnosticsText_, this);
        developerInfo->setObjectName(QStringLiteral("aboutDeveloperInformation"));
        developerInfo->setReadOnly(true);
        developerInfo->setLineWrapMode(QPlainTextEdit::NoWrap);
        developerInfo->setMinimumHeight(210);
        root->addWidget(developerInfo, 1);
    }

    root->addSpacing(20);

    auto* copyright = new QLabel(
        tr("Copyright %1 %2 XD.")
            .arg(QChar(0x00A9))
            .arg(buildInfoString(Harken::BuildInfo::BuildYear)),
        this);
    copyright->setObjectName(QStringLiteral("aboutCopyright"));
    copyright->setAlignment(Qt::AlignCenter);
    root->addWidget(copyright);
}

void AboutDialog::showEvent(QShowEvent* event)
{
    BaseDialog::showEvent(event);
    if (showDeveloperInformation_ || compactGeometryApplied_)
        return;

    compactGeometryApplied_ = true;
    layout()->activate();
    const int compactHeight = layout()->sizeHint().height();
    setFixedHeight(compactHeight);
    resize(kDialogWidth, compactHeight);
}

}
