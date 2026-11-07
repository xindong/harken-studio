#include "AboutDialog.h"
#include "Core/ThemeLayout.h"

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
#include <algorithm>

namespace {

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
    ThemeManager::instance().bindMetrics(this, [this](const ThemeMetrics& metrics) {
        setMinimumWidth(metrics.dialog.minimumWidth);
        if (compactGeometryApplied_) {
            QMetaObject::invokeMethod(this, [this] {
                layout()->activate();
                setMinimumWidth(std::max(ThemeManager::instance().metrics().dialog.minimumWidth,
                                         layout()->totalMinimumSize().width()));
                setFixedHeight(layout()->totalSizeHint().height());
            }, Qt::QueuedConnection);
        }
    });
    const auto& metrics = ThemeManager::instance().metrics();
    resize(metrics.dialog.preferredWidth, showDeveloperInformation ? metrics.dialog.wideHeight : metrics.dialog.compactHeight);

    const QString currentVersion = versionString();

    auto* root = new QVBoxLayout(this);
    ThemeLayout::margins(root, ThemeLayout::Space::XLarge);
    root->setSpacing(0);

    auto* logo = new QLabel(this);
    logo->setObjectName(QStringLiteral("aboutLogo"));
    logo->setAccessibleName(QStringLiteral("Harken Studio"));
    ThemeManager::instance().bindMetrics(logo, [logo](const ThemeMetrics& metrics) {
        const QSize logoSize(metrics.dialog.compactWidth, metrics.control.largeHeight);
        logo->setFixedSize(logoSize);
        logo->setAlignment(Qt::AlignCenter);
        const auto resolvedLogo = ThemeManager::instance().resolveImage(
            QString::fromLatin1(ThemeAssetIds::AppLogo));
        if (resolvedLogo.resource) {
            QPixmap pixmap = QPixmap::fromImage(resolvedLogo.resource->image).scaled(
                logoSize * logo->devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            pixmap.setDevicePixelRatio(logo->devicePixelRatioF());
            logo->setPixmap(pixmap);
        }
    });
    root->addWidget(logo);
    ThemeLayout::addSpacing(root, ThemeLayout::Space::Medium);

    auto* tagline = new QLabel(
        tr("Professional Audio Middleware Editor"), this);
    tagline->setObjectName(QStringLiteral("aboutTagline"));
    tagline->setWordWrap(true);
    root->addWidget(tagline);
    ThemeLayout::addSpacing(root, ThemeLayout::Space::XLarge);

    auto* summary = new QFrame(this);
    summary->setObjectName(QStringLiteral("aboutSummary"));
    auto* summaryLayout = new QGridLayout(summary);
    ThemeLayout::margins(summaryLayout, ThemeLayout::Space::Large, ThemeLayout::Space::Medium, ThemeLayout::Space::Large, ThemeLayout::Space::Medium);
    ThemeLayout::horizontalSpacing(summaryLayout, ThemeLayout::Space::XLarge);
    ThemeLayout::verticalSpacing(summaryLayout, ThemeLayout::Space::Small);
    summaryLayout->setColumnStretch(0, 1);
    summaryLayout->setColumnStretch(3, 1);
    addSummaryRow(summaryLayout, 0, tr("Version"), currentVersion,
                  QStringLiteral("aboutVersionValue"), summary);
    addSummaryRow(summaryLayout, 1, tr("Date"), buildDate(),
                  QStringLiteral("aboutDateValue"), summary);
    root->addWidget(summary);

    if (showDeveloperInformation) {
        ThemeLayout::addSpacing(root, ThemeLayout::Space::XLarge);

        auto* sectionTitle = new QLabel(tr("Developer Information"), this);
        sectionTitle->setObjectName(QStringLiteral("aboutDeveloperTitle"));
        root->addWidget(sectionTitle);
        ThemeLayout::addSpacing(root, ThemeLayout::Space::Small);

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
        ThemeManager::instance().bindMetrics(developerInfo, [developerInfo](const ThemeMetrics& metrics) {
            developerInfo->setMinimumHeight(metrics.panel.previewHeight);
        });
        root->addWidget(developerInfo, 1);
    }
}

void AboutDialog::showEvent(QShowEvent* event)
{
    BaseDialog::showEvent(event);
    if (showDeveloperInformation_ || compactGeometryApplied_)
        return;

    compactGeometryApplied_ = true;
    layout()->activate();
    setMinimumWidth(std::max(ThemeManager::instance().metrics().dialog.minimumWidth,
                             layout()->totalMinimumSize().width()));
    const int compactHeight = layout()->totalSizeHint().height();
    setFixedHeight(compactHeight);
    resize(ThemeManager::instance().metrics().dialog.preferredWidth, compactHeight);
}

}
