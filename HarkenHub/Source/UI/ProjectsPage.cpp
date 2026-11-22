#include "UI/ProjectsPage.h"
#include "UI/MessageDialog.h"

#include "UI/NoHoverItemDelegate.h"
#include "UI/BoundedHeaderView.h"
#include "UI/TableSorting.h"
#include "UI/HubIcons.h"
#include "Launch/ProjectCreateFlow.h"
#include "Launch/ProjectOpenFlow.h"
#include "Localization/HubLocalization.h"
#include "Platform/PlatformPaths.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectVersionSelection.h"
#include "Projects/ProjectStore.h"
#include "Settings/HubSettings.h"
#include "Transactions/InstallIndex.h"
#include "Versions/StudioLaunchTarget.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QColor>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QPointer>
#include <QSet>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <QThreadPool>
#include <QToolButton>
#include <QUrl>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QVersionNumber>

#include <algorithm>
#include <unordered_map>

namespace harken::hub {

namespace {

constexpr int kColumnProject = 0;
constexpr int kColumnLastOpened = 1;
constexpr int kColumnVersion = 2;
constexpr int kColumnMenu = 3;
constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kBuildIdRole = Qt::UserRole + 2;

QPixmap paintVersionMenuCheck(const QColor& color, qreal devicePixelRatio)
{
    const qreal dpr = qMax(qreal(1), devicePixelRatio);
    const int pixel = qMax(1, qRound(16 * dpr));
    QPixmap pixmap(pixel, pixel);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(dpr);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    QPolygonF mark;
    mark << QPointF(3.0, 8.5) << QPointF(6.5, 12.0) << QPointF(13.0, 4.0);
    painter.drawPolyline(mark);
    return pixmap;
}

QColor contrastingMenuMark(const QColor& preferred, const QColor& background)
{
    const int contrast = qAbs(preferred.lightness() - background.lightness());
    if (contrast >= 80)
        return preferred;
    return background.lightness() < 140 ? QColor(0xE0, 0xE0, 0xE0)
                                        : QColor(0x11, 0x11, 0x11);
}

void addVersionMenuIconStates(QIcon& icon, const QPixmap& pixmap)
{
    icon.addPixmap(pixmap, QIcon::Normal, QIcon::Off);
    icon.addPixmap(pixmap, QIcon::Normal, QIcon::On);
    icon.addPixmap(pixmap, QIcon::Selected, QIcon::Off);
    icon.addPixmap(pixmap, QIcon::Selected, QIcon::On);
    icon.addPixmap(pixmap, QIcon::Active, QIcon::Off);
    icon.addPixmap(pixmap, QIcon::Active, QIcon::On);
}

QIcon versionMenuCheckIcon(const QWidget* menu)
{
    const QPalette palette = menu ? menu->palette() : QPalette();
    const QPalette::ColorGroup group =
        palette.currentColorGroup() == QPalette::Disabled ? QPalette::Active
                                                          : palette.currentColorGroup();
    const QColor menuBackground = palette.color(group, QPalette::Window);
    const QColor highlightBackground = palette.color(group, QPalette::Highlight);
    const QColor text = palette.color(group, QPalette::WindowText);
    const QColor highlightedText = palette.color(group, QPalette::HighlightedText);
    const qreal dpr = menu ? menu->devicePixelRatioF() : qreal(1);
    const QPixmap normal = paintVersionMenuCheck(
        contrastingMenuMark(text, menuBackground), dpr);
    const QPixmap highlighted = paintVersionMenuCheck(
        contrastingMenuMark(highlightedText, highlightBackground), dpr);
    QIcon icon;
    icon.addPixmap(normal, QIcon::Normal, QIcon::Off);
    icon.addPixmap(normal, QIcon::Normal, QIcon::On);
    icon.addPixmap(highlighted, QIcon::Selected, QIcon::Off);
    icon.addPixmap(highlighted, QIcon::Selected, QIcon::On);
    icon.addPixmap(highlighted, QIcon::Active, QIcon::Off);
    icon.addPixmap(highlighted, QIcon::Active, QIcon::On);
    return icon;
}

QIcon versionMenuEmptyIcon(const QWidget* menu)
{
    const qreal dpr = qMax(qreal(1), menu ? menu->devicePixelRatioF() : qreal(1));
    const int pixel = qMax(1, qRound(16 * dpr));
    QPixmap pixmap(pixel, pixel);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(dpr);
    QIcon icon;
    addVersionMenuIconStates(icon, pixmap);
    return icon;
}

class ProjectCellDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();

        if (option.state & QStyle::State_Selected)
            painter->fillRect(option.rect, option.palette.color(QPalette::Button));
        else if (option.state & QStyle::State_MouseOver)
            painter->fillRect(option.rect, option.palette.color(QPalette::Midlight));

        painter->setPen(option.palette.color(QPalette::Shadow));
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());

        const QRect content = option.rect.adjusted(10, 9, -8, -9);
        QFont nameFont = option.font;
        nameFont.setWeight(QFont::DemiBold);
        painter->setFont(nameFont);
        painter->setPen(option.palette.color(QPalette::Text));
        painter->drawText(content, Qt::AlignLeft | Qt::AlignTop,
                          index.data(Qt::DisplayRole).toString());

        QFont pathFont = option.font;
        pathFont.setPixelSize(pathFont.pixelSize() - 1);
        painter->setFont(pathFont);
        painter->setPen(option.palette.color(QPalette::PlaceholderText));
        const QString path = index.data(kPathRole).toString();
        painter->drawText(content, Qt::AlignLeft | Qt::AlignBottom,
                          QFontMetrics(pathFont).elidedText(path, Qt::ElideMiddle,
                                                            content.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        return {QStyledItemDelegate::sizeHint(option, index).width(), 58};
    }
};

QString relativeTime(const QString& isoTimestamp)
{
    if (isoTimestamp.isEmpty())
        return QStringLiteral("-");

    const QDateTime then = QDateTime::fromString(isoTimestamp, Qt::ISODate);
    if (!then.isValid())
        return isoTimestamp;

    const qint64 totalMinutes = qMax<qint64>(0, then.secsTo(QDateTime::currentDateTimeUtc()) / 60);
    const qint64 days = totalMinutes / (24 * 60);
    const qint64 hours = totalMinutes / 60 % 24;
    const qint64 minutes = totalMinutes % 60;
    if (totalMinutes < 60)
        return QCoreApplication::translate("HarkenHub", "%1m ago").arg(minutes);
    if (days > 0)
        return QCoreApplication::translate("HarkenHub", "%1d %2h %3m ago")
            .arg(days).arg(hours).arg(minutes);
    return QCoreApplication::translate("HarkenHub", "%1h %2m ago")
        .arg(hours).arg(minutes);
}

QWidget* makeActionCell(
    QTableWidget* table, const QString& buildId,
    const std::function<void()>& onLaunch,
    const std::function<void(QToolButton*)>& onMenu)
{
    auto* holder = new QWidget(table);
    holder->setObjectName(QStringLiteral("projectActionCell"));
    holder->setStyleSheet(QStringLiteral(
        "#projectActionCell { background: transparent; }"));
    auto* layout = new QHBoxLayout(holder);
    layout->setContentsMargins(0, 0, 12, 0);
    layout->addStretch();

    auto* launchButton =
        new QPushButton(QCoreApplication::translate("HarkenHub", "Open Project"), holder);
    launchButton->setObjectName(QStringLiteral("rowLaunchButton"));
    launchButton->setProperty("studioBuildId", buildId);
    launchButton->setFixedSize(110, 26);
    QObject::connect(launchButton, &QPushButton::clicked, holder, onLaunch);
    layout->addWidget(launchButton);
    layout->addSpacing(14);

    auto* menuButton = new QToolButton(holder);
    menuButton->setObjectName(QStringLiteral("rowMenuButton"));
    menuButton->setIcon(HubIcons::moreIcon(holder->devicePixelRatioF()));
    menuButton->setIconSize(QSize(16, 16));
    menuButton->setFixedSize(34, 26);
    QObject::connect(menuButton, &QToolButton::clicked, holder,
                     [menuButton, onMenu]() { onMenu(menuButton); });
    layout->addWidget(menuButton);
    return holder;
}

void configureHeader(QTableWidget* table)
{
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

QSet<QString> needsRepairBuildIds()
{
    QSet<QString> result;
    const InstallIndexSnapshot index = InstallIndex::snapshot();
    if (!index.ok)
        return result;
    for (const auto& record : index.records) {
        if (record.healthState == QStringLiteral("NeedsRepair"))
            result.insert(record.buildId);
    }
    return result;
}

bool isValidProjectName(const QString& name)
{
    if (name.isEmpty() || name == QStringLiteral(".") ||
        name == QStringLiteral("..") || name.endsWith(QLatin1Char('.')) ||
        name.endsWith(QLatin1Char(' ')))
        return false;
    const QString invalid = QStringLiteral("<>:\"/\\|?*");
    for (const QChar character : name) {
        if (character.unicode() < 0x20 || invalid.contains(character))
            return false;
    }
    return true;
}

}

ProjectsPage::ProjectsPage(ProjectStore* store, QWidget* parent)
    : QWidget(parent)
    , store_(store)
{
    projectsView_ = new QWidget(this);
    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QCoreApplication::translate("HarkenHub", "Projects"), projectsView_);
    title->setObjectName(QStringLiteral("pageTitle"));
    header->addWidget(title, 0, Qt::AlignBottom);
    countLabel_ = new QLabel(projectsView_);
    countLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    header->addSpacing(10);
    header->addWidget(countLabel_, 0, Qt::AlignBottom);
    header->addStretch();
    auto* addButton =
        new QPushButton(QCoreApplication::translate("HarkenHub", "Add Project"), projectsView_);
    connect(addButton, &QPushButton::clicked, this, &ProjectsPage::onAddProjectClicked);
    header->addWidget(addButton);
    header->addSpacing(8);
    newProjectButton_ =
        new QPushButton(QCoreApplication::translate("HarkenHub", "New Project"), projectsView_);
    newProjectButton_->setObjectName(QStringLiteral("newProjectButton"));
    connect(newProjectButton_, &QPushButton::clicked, this,
            &ProjectsPage::onNewProjectClicked);
    header->addWidget(newProjectButton_);

    launchTargetPollTimer_ = new QTimer(this);
    launchTargetPollTimer_->setInterval(1000);
    connect(launchTargetPollTimer_, &QTimer::timeout, this,
            &ProjectsPage::updateLaunchTarget);

    projectLaunchGuidanceAnimation_ = new QVariantAnimation(this);
    projectLaunchGuidanceAnimation_->setDuration(1000);
    projectLaunchGuidanceAnimation_->setLoopCount(3);
    projectLaunchGuidanceAnimation_->setEasingCurve(QEasingCurve::InOutSine);
    connect(projectLaunchGuidanceAnimation_, &QVariantAnimation::valueChanged,
            this, [this](const QVariant& value) {
                applyProjectLaunchGuidanceColor(value.value<QColor>());
            });
    connect(projectLaunchGuidanceAnimation_, &QVariantAnimation::finished, this,
            &ProjectsPage::clearProjectLaunchGuidance);

    table_ = new QTableWidget(projectsView_);
    auto* tableHeader = new BoundedHeaderView(Qt::Horizontal, table_);
    tableHeader->setFillSection(kColumnVersion);
    table_->setHorizontalHeader(tableHeader);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({QCoreApplication::translate("HarkenHub", "PROJECT"),
                                       QCoreApplication::translate("HarkenHub", "LAST OPENED"),
                                       QCoreApplication::translate("HarkenHub", "VERSION"), QString()});
    configureHeader(table_);
    sorting_ = new TableSorting(table_, QStringLiteral("projects"), kColumnMenu);
    table_->horizontalHeader()->setSectionResizeMode(kColumnProject, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(kColumnLastOpened, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(kColumnVersion, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(kColumnMenu, QHeaderView::Fixed);
    table_->horizontalHeader()->setMinimumSectionSize(70);
    table_->setColumnWidth(kColumnProject, 380);
    table_->setColumnWidth(kColumnLastOpened, 160);
    table_->setColumnWidth(kColumnVersion, 280);
    table_->setColumnWidth(kColumnMenu, 190);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(58);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setItemDelegateForColumn(kColumnProject, new ProjectCellDelegate(table_));
    table_->setItemDelegateForColumn(kColumnLastOpened,
                                     new NoHoverItemDelegate(table_));
    table_->setItemDelegateForColumn(kColumnVersion,
                                     new NoHoverItemDelegate(table_));
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                const int row = table_->rowAt(pos.y());
                if (row >= 0)
                    showRowMenu(row, table_->viewport()->mapToGlobal(pos));
            });

    auto* panel = new QFrame(projectsView_);
    panel->setObjectName(QStringLiteral("tablePanel"));
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(12, 4, 12, 8);
    panelLayout->addWidget(table_);

    auto* layout = new QVBoxLayout(projectsView_);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->addLayout(header);
    layout->addSpacing(14);
    layout->addWidget(panel);

    newProjectView_ = createNewProjectPage();
    viewStack_ = new QStackedWidget(this);
    viewStack_->addWidget(projectsView_);
    viewStack_->addWidget(newProjectView_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    launchError_ = new QLabel(this);
    launchError_->setObjectName(QStringLiteral("projectLaunchError"));
    launchError_->setWordWrap(true);
    launchError_->setTextFormat(Qt::PlainText);
    launchError_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    launchError_->setContentsMargins(24, 12, 24, 0);
    launchError_->hide();
    root->addWidget(launchError_);
    root->addWidget(viewStack_);

    refresh();
}

void ProjectsPage::showProjectLaunchGuidance(const QString& buildId)
{
    showProjectsList();
    projectLaunchGuidanceAnimation_->stop();
    clearProjectLaunchGuidance();
    projectLaunchGuidanceBuildId_ = buildId;
    pendingCreateBuildId_ = buildId;
    const QColor accent = palette().color(QPalette::Highlight);
    projectLaunchGuidanceAnimation_->setKeyValueAt(0.0, accent);
    projectLaunchGuidanceAnimation_->setKeyValueAt(0.5, accent.darker(120));
    projectLaunchGuidanceAnimation_->setKeyValueAt(1.0, accent);
    projectLaunchGuidanceAnimation_->start();
}

void ProjectsPage::applyProjectLaunchGuidanceColor(const QColor& color)
{
    const QString style = QStringLiteral(
        "QPushButton { background-color: %1; border: 1px solid %1; "
        "color: palette(highlighted-text); font-weight: 600; }")
                              .arg(color.name());
    auto updateButton = [&style](QPushButton* button, bool highlighted) {
        if (!button || !highlighted)
            return;
        button->setStyleSheet(style);
    };

    updateButton(newProjectButton_, true);
    const auto openButtons =
        table_->findChildren<QPushButton*>(QStringLiteral("rowLaunchButton"));
    for (QPushButton* button : openButtons) {
        const bool matches =
            button->property("studioBuildId").toString() ==
            projectLaunchGuidanceBuildId_;
        updateButton(button, matches);
    }
}

void ProjectsPage::clearProjectLaunchGuidance()
{
    newProjectButton_->setStyleSheet(QString());
    const auto openButtons =
        table_->findChildren<QPushButton*>(QStringLiteral("rowLaunchButton"));
    for (QPushButton* button : openButtons)
        button->setStyleSheet(QString());
    projectLaunchGuidanceBuildId_.clear();
}

QWidget* ProjectsPage::createNewProjectPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(14);

    auto* header = new QHBoxLayout();
    auto* backButton = new QPushButton(QCoreApplication::translate("HarkenHub", "Back to Projects"), page);
    connect(backButton, &QPushButton::clicked, this, [this]() {
        viewStack_->setCurrentWidget(projectsView_);
        refresh();
    });
    header->addWidget(backButton);
    header->addSpacing(12);
    auto* title = new QLabel(QCoreApplication::translate("HarkenHub", "New Project"), page);
    title->setObjectName(QStringLiteral("pageTitle"));
    header->addWidget(title);
    header->addStretch();
    layout->addLayout(header);

    auto* panel = new QFrame(page);
    panel->setObjectName(QStringLiteral("newProjectPanel"));
    auto* form = new QVBoxLayout(panel);
    form->setContentsMargins(18, 14, 18, 14);
    form->setSpacing(6);

    auto* nameRowWidget = new QWidget(panel);
    nameRowWidget->setFixedHeight(30);
    auto* nameRow = new QHBoxLayout(nameRowWidget);
    nameRow->setContentsMargins(0, 0, 0, 0);
    nameRow->setSpacing(8);
    auto* nameLabel =
        new QLabel(QCoreApplication::translate("HarkenHub", "Project Name"), nameRowWidget);
    nameLabel->setFixedWidth(110);
    nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    nameRow->addWidget(nameLabel);
    projectNameEdit_ = new QLineEdit(nameRowWidget);
    projectNameEdit_->setObjectName(QStringLiteral("newProjectField"));
    projectNameEdit_->setPlaceholderText(QStringLiteral("MyGameAudio"));
    nameRow->addWidget(projectNameEdit_, 1);
    projectVersionCombo_ = new QComboBox(nameRowWidget);
    projectVersionCombo_->setObjectName(
        QStringLiteral("newProjectVersion"));
    projectVersionCombo_->setMinimumWidth(190);
    projectVersionCombo_->setMaximumWidth(240);
    nameRow->addWidget(projectVersionCombo_);
    form->addWidget(nameRowWidget);

    auto* locationRowWidget = new QWidget(panel);
    locationRowWidget->setFixedHeight(30);
    auto* locationRow = new QHBoxLayout(locationRowWidget);
    locationRow->setContentsMargins(0, 0, 0, 0);
    locationRow->setSpacing(8);
    auto* locationLabel =
        new QLabel(QCoreApplication::translate("HarkenHub", "Location"), locationRowWidget);
    locationLabel->setFixedWidth(110);
    locationLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    locationRow->addWidget(locationLabel);
    projectLocationEdit_ = new QLineEdit(locationRowWidget);
    projectLocationEdit_->setObjectName(QStringLiteral("newProjectField"));
    const QString savedProjectLocation = HubSettings::projectLocation();
    projectLocationEdit_->setText(
        savedProjectLocation.isEmpty()
            ? QStandardPaths::writableLocation(
                  QStandardPaths::DocumentsLocation)
            : savedProjectLocation);
    auto* browseButton = new QPushButton(
        QCoreApplication::translate("HarkenHub", "Browse..."),
        locationRowWidget);
    browseButton->setObjectName(QStringLiteral("newProjectBrowse"));
    browseButton->setFixedHeight(30);
    browseButton->setMinimumWidth(110);
    connect(browseButton, &QPushButton::clicked, this, [this]() {
        projectLocationSaveTimer_->stop();
        const QString path = QFileDialog::getExistingDirectory(
            this, QCoreApplication::translate("HarkenHub", "Select Project Location"),
            projectLocationEdit_->text());
        if (!path.isEmpty()) {
            projectLocationEdit_->setText(path);
            persistProjectLocation(path);
        }
    });
    locationRow->addWidget(projectLocationEdit_, 1);
    locationRow->addWidget(browseButton);
    form->addWidget(locationRowWidget);

    form->addStretch(1);

    auto* actions = new QHBoxLayout();
    actions->addStretch();
    createProjectButton_ =
        new QPushButton(QCoreApplication::translate("HarkenHub", "Create Project"), panel);
    createProjectButton_->setObjectName(QStringLiteral("createProjectButton"));
    createProjectButton_->setMinimumWidth(150);
    connect(createProjectButton_, &QPushButton::clicked, this,
            &ProjectsPage::onCreateProjectClicked);
    actions->addWidget(createProjectButton_);
    form->addLayout(actions);

    layout->addWidget(panel, 1);

    connect(projectNameEdit_, &QLineEdit::textChanged, this,
            [this]() { updateNewProjectCreateEnabled(); });
    connect(projectLocationEdit_, &QLineEdit::textChanged, this,
            [this]() { updateNewProjectCreateEnabled(); });
    projectLocationSaveTimer_ = new QTimer(this);
    projectLocationSaveTimer_->setSingleShot(true);
    projectLocationSaveTimer_->setInterval(300);
    connect(projectLocationEdit_, &QLineEdit::textEdited, this,
            [this]() { projectLocationSaveTimer_->start(); });
    connect(projectLocationSaveTimer_, &QTimer::timeout, this, [this]() {
        persistProjectLocation(projectLocationEdit_->text());
    });
    connect(projectVersionCombo_,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index < 0 ||
                    index >= static_cast<int>(createVersions_.size())) {
                    createTarget_ = {};
                } else {
                    createTarget_ =
                        createVersions_[static_cast<size_t>(index)];
                }
                updateNewProjectCreateEnabled();
            });
    updateNewProjectCreateEnabled();
    return page;
}

void ProjectsPage::loadProjectVersions(const QString& preferredBuildId)
{
    createVersions_ = versionsProvider_ ? versionsProvider_()
                                        : std::vector<InstalledVersion>{};
    projectVersionCombo_->blockSignals(true);
    projectVersionCombo_->clear();
    for (const InstalledVersion& version : createVersions_) {
        QString label =
            QCoreApplication::translate("HarkenHub", "HarkenStudio %1").arg(version.semanticVersion);
        if (version.isLocalBuild())
            label += QCoreApplication::translate("HarkenHub", " (Standalone)");
        projectVersionCombo_->addItem(label, version.buildId);
        projectVersionCombo_->setItemData(
            projectVersionCombo_->count() - 1,
            QDir::toNativeSeparators(version.executablePath),
            Qt::ToolTipRole);
    }

    const InstalledVersion* target = StudioLaunchTarget::pick(
        createVersions_, preferredBuildId.isEmpty()
            ? StudioLaunchTarget::pinnedBuildId() : preferredBuildId,
        needsRepairBuildIds());
    int selectedIndex = -1;
    if (target) {
        for (int index = 0;
             index < static_cast<int>(createVersions_.size()); ++index) {
            if (createVersions_[static_cast<size_t>(index)].buildId ==
                target->buildId) {
                selectedIndex = index;
                break;
            }
        }
    }
    projectVersionCombo_->setCurrentIndex(selectedIndex);
    createTarget_ = selectedIndex >= 0
                        ? createVersions_[static_cast<size_t>(selectedIndex)]
                        : InstalledVersion{};
    projectVersionCombo_->blockSignals(false);
}

void ProjectsPage::updateNewProjectCreateEnabled()
{
    if (!createProjectButton_)
        return;
    createProjectButton_->setEnabled(
        !createTarget_.installPath.isEmpty() &&
        isValidProjectName(projectNameEdit_->text().trimmed()) &&
        !projectLocationEdit_->text().trimmed().isEmpty());
}

void ProjectsPage::persistProjectLocation(const QString& path)
{
    const QString value = path.trimmed();
    if (value.isEmpty() || value == HubSettings::projectLocation())
        return;
    if (!HubSettings::setProjectLocation(value)) {
        MessageDialog::warning(
            this, QCoreApplication::translate("HarkenHub", "New Project"),
            QCoreApplication::translate("HarkenHub", "The project location could not be saved. It will "
                           "not be restored after restarting Harken Hub."));
    }
}

void ProjectsPage::onNewProjectClicked()
{
    showLaunchError({});
    loadProjectVersions(pendingCreateBuildId_);
    if (createTarget_.installPath.isEmpty()) {
        showLaunchError(QCoreApplication::translate("HarkenHub", "Install or repair HarkenStudio before creating a "
                           "project."));
        return;
    }

    pendingCreateBuildId_.clear();
    projectNameEdit_->clear();
    updateNewProjectCreateEnabled();
    viewStack_->setCurrentWidget(newProjectView_);
    projectNameEdit_->setFocus();
}

void ProjectsPage::showNewProjectPage()
{
    onNewProjectClicked();
}

void ProjectsPage::showProjectsList()
{
    showLaunchError({});
    pendingCreateBuildId_.clear();
    viewStack_->setCurrentWidget(projectsView_);
    refresh();
}

void ProjectsPage::onCreateProjectClicked()
{
    showLaunchError({});
    const QString name = projectNameEdit_->text().trimmed();
    const QString location = QDir::cleanPath(
        projectLocationEdit_->text().trimmed());
    if (!isValidProjectName(name)) {
        MessageDialog::warning(
            this, QCoreApplication::translate("HarkenHub", "New Project"),
            QCoreApplication::translate("HarkenHub", "Choose a project name that can be used as a "
                           "folder name."));
        return;
    }
    if (!QDir(location).exists()) {
        MessageDialog::warning(this, QCoreApplication::translate("HarkenHub", "New Project"),
                             QCoreApplication::translate("HarkenHub", "The selected location does not "
                                            "exist."));
        return;
    }
    if (QFileInfo::exists(QDir(location).filePath(name))) {
        MessageDialog::warning(
            this, QCoreApplication::translate("HarkenHub", "New Project"),
            QCoreApplication::translate("HarkenHub", "A folder with this project name already exists."));
        return;
    }

    ProjectCreateRequest request;
    request.version = createTarget_;
    request.name = name;
    request.location = location;

    QString versionName =
        QCoreApplication::translate("HarkenHub", "HarkenStudio %1").arg(createTarget_.semanticVersion);
    if (createTarget_.isLocalBuild())
        versionName += QCoreApplication::translate("HarkenHub", " (Standalone)");
    MessageDialog confirm(
        QMessageBox::Question, QCoreApplication::translate("HarkenHub", "Confirm New Project"),
        QCoreApplication::translate("HarkenHub", "Confirm the HarkenStudio version before creating the project."),
        QMessageBox::Cancel, this);
    confirm.setInformativeText(
        QCoreApplication::translate("HarkenHub", "HarkenStudio: %1").arg(versionName));
    QPushButton* create = confirm.addButton(
        QCoreApplication::translate("HarkenHub", "Create Project"), QMessageBox::AcceptRole);
    confirm.setDefaultButton(create);
    confirm.exec();
    if (confirm.clickedButton() != create)
        return;

    QString error;
    if (!ProjectCreateFlow::create(request, &error)) {
        showLaunchError(error);
        return;
    }
    const QString projectPath = QDir(location).absoluteFilePath(
        name + QLatin1Char('/') + name + QStringLiteral(".hsp"));
    pendingProjectBuilds_.insert(projectPath, request.version.buildId);
    showProjectsList();
}

void ProjectsPage::refresh()
{
    const auto projects = store_->listProjects();
    for (auto it = pendingProjectBuilds_.begin(); it != pendingProjectBuilds_.end();) {
        const QString path = QFileInfo(it.key()).canonicalFilePath();
        const auto project = std::find_if(projects.begin(), projects.end(),
            [&path](const ProjectRecord& value) { return value.canonicalPath == path; });
        if (project == projects.end()) {
            ++it;
            continue;
        }
        if (project->preferredStudioBuildId.isEmpty()
            && !store_->setPreferredBuild(path, it.value())) {
            ++it;
            continue;
        }
        it = pendingProjectBuilds_.erase(it);
    }
    renderProjects();
    refreshProjectMetadata();
    emit projectsChanged();
}

void ProjectsPage::refreshProjectMetadata()
{
    if (metadataRefreshRunning_) {
        metadataRefreshPending_ = true;
        return;
    }
    metadataRefreshRunning_ = true;
    metadataRefreshPending_ = false;
    const auto records = store_->listProjects();
    const auto cached = projectMetadata_;
    const QPointer<ProjectsPage> self(this);
    QThreadPool::globalInstance()->start([self, records, cached]() {
        QHash<QString, ProjectMetadata> metadata;
        for (const auto& record : records) {
            const QFileInfo file(record.canonicalPath);
            auto entry = cached.value(record.canonicalPath);
            if (!cached.contains(record.canonicalPath) || !entry.probe.ok
                || !entry.probe.metadataWarning.isEmpty()
                || entry.size != file.size()
                || entry.modifiedMs != file.lastModified().toMSecsSinceEpoch()) {
                entry.probe = ProjectProbe::probe(record.canonicalPath);
                entry.size = file.size();
                entry.modifiedMs = file.lastModified().toMSecsSinceEpoch();
            }
            metadata.insert(record.canonicalPath, std::move(entry));
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, metadata]() {
            if (!self)
                return;
            self->projectMetadata_ = metadata;
            self->metadataRefreshRunning_ = false;
            self->renderProjects();
            if (self->metadataRefreshPending_)
                self->refreshProjectMetadata();
        }, Qt::QueuedConnection);
    });
}

void ProjectsPage::renderProjects()
{
    sorting_->beginRefresh();
    QSet<QString> selectedPaths;
    for (const auto& index : table_->selectionModel()->selectedRows(kColumnProject))
        selectedPaths.insert(index.data(kPathRole).toString());
    const auto* currentItem = table_->item(table_->currentRow(), kColumnProject);
    const QString currentPath = currentItem ? currentItem->data(kPathRole).toString() : QString();
    const auto records = store_->listProjects();
    const auto versions = versionsProvider_ ? versionsProvider_() : std::vector<InstalledVersion>{};

    std::unordered_map<QString, QString> versionNames;
    std::unordered_map<QString, QString> versionPaths;
    QHash<QString, QVersionNumber> versionNumbers;
    if (versionsProvider_) {
        for (const auto& version : versions) {
            versionNumbers.insert(version.buildId, QVersionNumber::fromString(version.semanticVersion).normalized());
            QString name = version.semanticVersion;
            if (version.isLocalBuild())
                name += QCoreApplication::translate("HarkenHub", " (Standalone)");
            versionNames.emplace(version.buildId, name);
            versionPaths.emplace(
                version.buildId,
                QDir::toNativeSeparators(version.executablePath));
        }
    }
    auto versionLabel = [&versionNames](const QString& buildId) {
        if (buildId.isEmpty())
            return QStringLiteral("-");
        const auto it = versionNames.find(buildId);
        if (it != versionNames.end())
            return QCoreApplication::translate("HarkenHub", "HarkenStudio %1").arg(it->second);
        return buildId;
    };

    countLabel_->setText(records.size() == 1
                             ? QCoreApplication::translate("HarkenHub", "1 project")
                             : QCoreApplication::translate("HarkenHub", "%1 projects").arg(records.size()));

    table_->clearSelection();
    table_->setCurrentCell(-1, -1);
    table_->setRowCount(static_cast<int>(records.size()));
    QItemSelection rowSelection;
    for (int row = 0; row < static_cast<int>(records.size()); ++row) {
        const auto& record = records[static_cast<size_t>(row)];
        const auto probe = projectMetadata_.value(record.canonicalPath).probe;
        auto selection = ProjectVersionSelection::resolve(record, probe, versions);
        if (!projectMetadata_.contains(record.canonicalPath) && record.preferredStudioBuildId.isEmpty())
            selection = {};

        auto* projectItem = new SortableTableItem(probe.ok ? probe.displayName
            : QFileInfo(record.canonicalPath).completeBaseName());
        projectItem->setData(kPathRole, record.canonicalPath);
        projectItem->setToolTip(
            QDir::toNativeSeparators(record.canonicalPath));
        table_->setItem(row, kColumnProject, projectItem);

        const auto openedAt = QDateTime::fromString(record.lastOpenedAt, Qt::ISODate);
        table_->setItem(row, kColumnLastOpened,
                       new SortableTableItem(relativeTime(record.lastOpenedAt),
                           openedAt.isValid() ? QVariant(openedAt) : QVariant()));
        const auto versionNumber = versionNumbers.value(selection.buildId);
        auto* versionItem = new SortableTableItem(
            selection.buildId.isEmpty()
                ? QCoreApplication::translate("HarkenHub", "Select Version")
                : versionLabel(selection.buildId),
            versionNumber.isNull() ? QVariant() : QVariant::fromValue(versionNumber));
        versionItem->setData(kBuildIdRole, selection.buildId);
        QStringList versionTooltip{selection.source, selection.reason, probe.metadataWarning,
            QCoreApplication::translate("HarkenHub", "Build: %1")
                .arg(selection.buildId)};
        const auto versionPath =
            versionPaths.find(selection.buildId);
        if (versionPath != versionPaths.end())
            versionTooltip.append(
                QCoreApplication::translate("HarkenHub", "Path: %1").arg(versionPath->second));
        versionItem->setToolTip(versionTooltip.join(QLatin1Char('\n')));
        table_->setItem(row, kColumnVersion, versionItem);
        table_->setItem(row, kColumnMenu, new QTableWidgetItem());

        const QString canonicalPath = record.canonicalPath;
        if (selectedPaths.contains(canonicalPath))
            rowSelection.select(table_->model()->index(row, kColumnProject),
                                table_->model()->index(row, kColumnMenu));
        if (canonicalPath == currentPath)
            table_->setCurrentCell(row, kColumnProject, QItemSelectionModel::NoUpdate);
        table_->setCellWidget(
            row, kColumnMenu,
            makeActionCell(
                table_, selection.buildId,
                [this, canonicalPath]() { openProject(canonicalPath); },
                [this, canonicalPath](QToolButton* button) {
                    for (int row = 0; row < table_->rowCount(); ++row) {
                        if (table_->item(row, kColumnProject)->data(kPathRole).toString() == canonicalPath) {
                            showRowMenu(row, button->mapToGlobal(button->rect().bottomLeft()));
                            break;
                        }
                    }
                }));
    }

    table_->selectionModel()->select(rowSelection, QItemSelectionModel::ClearAndSelect);
    sorting_->endRefresh();
    updateLaunchTarget();
}

void ProjectsPage::refreshVersions()
{
    refresh();
    if (viewStack_->currentWidget() != newProjectView_)
        return;
    loadProjectVersions(createTarget_.buildId);
    updateNewProjectCreateEnabled();
}

void ProjectsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
    launchTargetPollTimer_->start();
}

void ProjectsPage::hideEvent(QHideEvent* event)
{
    pendingCreateBuildId_.clear();
    projectLaunchGuidanceAnimation_->stop();
    clearProjectLaunchGuidance();
    QWidget::hideEvent(event);
    launchTargetPollTimer_->stop();
}

void ProjectsPage::updateLaunchTarget()
{
    const std::vector<InstalledVersion> versions =
        versionsProvider_ ? versionsProvider_()
                          : std::vector<InstalledVersion>{};
    const InstalledVersion* target = StudioLaunchTarget::pick(
        versions, StudioLaunchTarget::pinnedBuildId(), needsRepairBuildIds());
    if (target) {
        newProjectButton_->setEnabled(true);
        return;
    }

    newProjectButton_->setEnabled(false);
}

void ProjectsPage::addProjectFromPath(const QString& path)
{
    const ProjectProbeResult probe = ProjectProbe::probe(path);
    if (!probe.ok) {
        MessageDialog::warning(
            this, QCoreApplication::translate("HarkenHub", "Add Project"),
            QCoreApplication::translate(
                "HarkenHub", "The project could not be added.\n\n%1")
                .arg(probe.error));
        return;
    }
    if (store_->contains(probe.canonicalPath)) {
        projectMetadata_.remove(probe.canonicalPath);
        refresh();
        MessageDialog::information(this, QCoreApplication::translate("HarkenHub", "Add Project"),
                                 QCoreApplication::translate("HarkenHub", "This project is already in the list."));
        return;
    }

    ProjectRecord record;
    record.canonicalPath = probe.canonicalPath;
    record.displayName = probe.displayName;
    if (!store_->addProject(record)) {
        MessageDialog::warning(
            this, QCoreApplication::translate("HarkenHub", "Add Project"),
            QCoreApplication::translate(
                "HarkenHub", "The project could not be added.\n\n%1")
                .arg(store_->lastError()));
        return;
    }
    refresh();
}

void ProjectsPage::onAddProjectClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QCoreApplication::translate("HarkenHub", "Add Project"), QString(),
        QCoreApplication::translate("HarkenHub", "Harken Project (*.hsp)"));
    if (path.isEmpty())
        return;
    addProjectFromPath(path);
}

void ProjectsPage::revealProject(const QString& canonicalPath)
{
    for (int row = 0; row < table_->rowCount(); ++row) {
        QTableWidgetItem* item = table_->item(row, kColumnProject);
        if (!item || item->data(kPathRole).toString() != canonicalPath)
            continue;
        table_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        table_->setCurrentCell(row, kColumnProject);
        return;
    }
}

void ProjectsPage::showLaunchError(const QString& error)
{
    launchError_->setText(error);
    launchError_->setVisible(!error.isEmpty());
}

void ProjectsPage::openProject(const QString& canonicalPath)
{
    if (canonicalPath.isEmpty())
        return;
    const std::vector<InstalledVersion> versions =
        versionsProvider_ ? versionsProvider_() : std::vector<InstalledVersion>{};
    showLaunchError({});
    const auto result = ProjectOpenFlow::open(store_, canonicalPath, versions, this);
    showLaunchError(result.error);
    refresh();
}

void ProjectsPage::showRowMenu(int row, const QPoint& globalPos)
{
    const auto* item = table_->item(row, kColumnProject);
    if (!item)
        return;
    const QString canonicalPath = item->data(kPathRole).toString();
    if (!item->isSelected())
        table_->setCurrentCell(row, kColumnProject,
                              QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QStringList selectedPaths;
    for (const auto& index : table_->selectionModel()->selectedRows(kColumnProject))
        selectedPaths.append(index.data(kPathRole).toString());
    const auto removeSelected = [this, selectedPaths]() {
        QStringList failures;
        for (const auto& path : selectedPaths) {
            if (!store_->removeProject(path))
                failures.append(QDir::toNativeSeparators(path) + QStringLiteral(": ") + store_->lastError());
        }
        refresh();
        if (!failures.isEmpty())
            MessageDialog::warning(this, QCoreApplication::translate("HarkenHub", "Harken Hub"),
                QCoreApplication::translate("HarkenHub", "These projects could not be removed from the list:\n%1")
                    .arg(failures.join(QLatin1Char('\n'))));
    };

    QMenu menu(this);
    menu.setAttribute(Qt::WA_TranslucentBackground);
    auto applyMenuPalette = [](QMenu* target) {
        QPalette palette = target->palette();
        const QColor window = QApplication::palette().color(QPalette::Window);
        const QColor text = QApplication::palette().color(QPalette::WindowText);
        const QColor highlight = QApplication::palette().color(QPalette::Highlight);
        const QColor highlightedText = QApplication::palette().color(QPalette::HighlightedText);
        for (const auto group : {QPalette::Active, QPalette::Inactive}) {
            palette.setColor(group, QPalette::Window, window);
            palette.setColor(group, QPalette::WindowText, text);
            palette.setColor(group, QPalette::Text, text);
            palette.setColor(group, QPalette::ButtonText, text);
            palette.setColor(group, QPalette::Highlight, highlight);
            palette.setColor(group, QPalette::HighlightedText, highlightedText);
        }
        target->setPalette(palette);
    };
    applyMenuPalette(&menu);

    if (selectedPaths.size() > 1) {
        QAction* remove = menu.addAction(
            QCoreApplication::translate("HarkenHub", "Remove Selected from List (%1)").arg(selectedPaths.size()));
        connect(remove, &QAction::triggered, this, removeSelected);
        menu.exec(globalPos);
        return;
    }

    QMenu* selectVersion = menu.addMenu(QCoreApplication::translate("HarkenHub", "Select Version"));
    selectVersion->setObjectName(QStringLiteral("versionSelectMenu"));
    selectVersion->setAttribute(Qt::WA_TranslucentBackground);
    selectVersion->setToolTipsVisible(true);
    applyMenuPalette(selectVersion);
    std::vector<InstalledVersion> versions =
        versionsProvider_ ? versionsProvider_() : std::vector<InstalledVersion>{};
    std::stable_sort(versions.begin(), versions.end(), [](const auto& left, const auto& right) {
        const int comparison = QVersionNumber::compare(
            QVersionNumber::fromString(left.semanticVersion),
            QVersionNumber::fromString(right.semanticVersion));
        return comparison != 0 ? comparison > 0 : left.releaseSequence > right.releaseSequence;
    });
    const auto* versionItem = table_->item(row, kColumnVersion);
    const QString selectedBuildId =
        versionItem ? versionItem->data(kBuildIdRole).toString() : QString();
    if (versions.empty()) {
        QAction* none = selectVersion->addAction(QCoreApplication::translate("HarkenHub", "No versions installed"));
        none->setEnabled(false);
    } else {
        for (const auto& version : versions) {
            QString label =
                QCoreApplication::translate("HarkenHub", "HarkenStudio %1").arg(version.semanticVersion);
            if (version.isLocalBuild())
                label += QCoreApplication::translate("HarkenHub", " (Standalone)");
            const bool selected = !selectedBuildId.isEmpty() &&
                                  version.buildId == selectedBuildId;
            QAction* action = selectVersion->addAction(label);
            action->setIcon(selected ? versionMenuCheckIcon(selectVersion)
                                     : versionMenuEmptyIcon(selectVersion));
            action->setToolTip(
                QDir::toNativeSeparators(version.executablePath));
            connect(action, &QAction::triggered, this, [this, canonicalPath, version]() {
                store_->setPreferredBuild(canonicalPath, version.buildId);
                refresh();
            });
        }
    }

    QAction* useRecommendation = menu.addAction(
        QCoreApplication::translate("HarkenHub", "Use Project Recommendation"));
    useRecommendation->setObjectName(QStringLiteral("useProjectRecommendation"));
    connect(useRecommendation, &QAction::triggered, this, [this, canonicalPath]() {
        store_->setPreferredBuild(canonicalPath, {});
        refresh();
    });

    QAction* openFolder =
        menu.addAction(QCoreApplication::translate("HarkenHub", "Open Project Folder"));
    connect(openFolder, &QAction::triggered, this, [canonicalPath]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QFileInfo(canonicalPath).absolutePath()));
    });

    QAction* remove = menu.addAction(QCoreApplication::translate("HarkenHub", "Remove from List"));
    connect(remove, &QAction::triggered, this, removeSelected);

    menu.exec(globalPos);
}

}
