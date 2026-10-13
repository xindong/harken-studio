#include "Monitor/MonitorCatalogPanel.h"
#include "Monitor/MonitorTypes.h"
#include "Common/TableWidget.h"
#include "Common/ListFooter.h"
#include "Common/ElidedLabel.h"
#include "Core/ThemeManager.h"
#include "Core/ThemeAssetCatalog.h"

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableView>
#include <QToolButton>
#include <QTextBrowser>
#include <QSortFilterProxyModel>
#include <QShortcut>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {

class MonitorCatalogFilter final : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    int category = 0;
    int level = -1;
    QString search;
    void refresh() { invalidateFilter(); }

protected:
    bool filterAcceptsRow(int row, const QModelIndex&) const override
    {
        const auto* model = static_cast<const MonitorCatalogModel*>(sourceModel());
        const auto* event = model->eventAt(row);
        if (!event || (category && event->detail.category != category) ||
            (level >= 0 && event->detail.level != level)) return false;
        if (search.isEmpty()) return true;
        for (int column = 0; column < MonitorCatalogModel::ColumnCount; ++column)
            if (model->data(model->index(row, column)).toString().contains(search, Qt::CaseInsensitive)) return true;
        return objectIdString(event->header.stableId).contains(search) ||
            QString::number(event->header.instanceHandle).contains(search) ||
            QString::number(event->detail.code).contains(search);
    }
};

MonitorCatalogPanel::MonitorCatalogPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorCatalogPanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    gapLabel_ = new QLabel(this);
    gapLabel_->setObjectName(QStringLiteral("MonitorCatalogGap"));
    gapLabel_->setWordWrap(true);
    root->addWidget(gapLabel_);

    auto* filters = new QHBoxLayout();
    levelFilter_ = new QComboBox(this);
    levelFilter_->setObjectName(QStringLiteral("MonitorCatalogLevelFilter"));
    levelFilter_->addItem(tr("All levels"), -1);
    levelFilter_->addItem(tr("Info"), VNX::Monitor::kLogInfo);
    levelFilter_->addItem(tr("Warning"), VNX::Monitor::kLogWarn);
    levelFilter_->addItem(tr("Error"), VNX::Monitor::kLogError);
    filters->addWidget(levelFilter_);
    categoryFilter_ = new QComboBox(this);
    categoryFilter_->setObjectName(QStringLiteral("MonitorCatalogCategoryFilter"));
    categoryFilter_->addItem(tr("All categories"), 0);
    categoryFilter_->addItem(tr("Operation"), VNX::Monitor::kMonitorEventCategoryOperation);
    categoryFilter_->addItem(tr("Playback"), VNX::Monitor::kMonitorEventCategoryLifecycle);
    categoryFilter_->addItem(tr("Diagnostic"), VNX::Monitor::kMonitorEventCategoryDiagnostic);
    filters->addWidget(categoryFilter_);

    textFilter_ = new QLineEdit(this);
    textFilter_->setObjectName(QStringLiteral("MonitorCatalogTextFilter"));
    textFilter_->setPlaceholderText(tr("Search"));
    textFilter_->setMinimumWidth(0);
    textFilter_->setClearButtonEnabled(true);
    filters->addWidget(textFilter_);
    jumpButton_ = new QPushButton(QStringLiteral("Jump"), this);
    jumpButton_->setObjectName(QStringLiteral("MonitorCatalogJumpButton"));
    jumpButton_->setEnabled(false);
    filters->addWidget(jumpButton_);
    root->addLayout(filters);

    tableFrame_ = new TableWidget(TableOptions::display(), this);
    tableFrame_->setColumnSizing(TableColumnSizing::Scrollable);
    table_ = tableFrame_->createModelView(QStringLiteral("MonitorCatalogTable"), nullptr);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(tableFrame_, 1);

    details_ = new QTextBrowser(this);
    details_->setObjectName(QStringLiteral("MonitorCatalogDetails"));
    details_->setMaximumHeight(170);
    details_->setMinimumHeight(70);
    details_->setOpenLinks(false);
    details_->hide();
    root->addWidget(details_);
    auto* footer = tableFrame_->footer(false);
    status_ = new ElidedLabel(footer);
    status_->setObjectName(QStringLiteral("MonitorCatalogStatus"));
    footer->addStatusWidget(status_);
    const auto tool = [footer](const QString& name, const QString& tip, const char* asset) {
        auto* action = new QAction(tip, footer);
        action->setToolTip(tip);
        auto* button = footer->addActionButton(action, true, ListFooter::Position::Right,
                                               QString::fromLatin1(asset));
        button->setObjectName(name);
        button->hide();
        return button;
    };
    autoScroll_ = tool(QStringLiteral("MonitorCatalogAutoScroll"), tr("Auto-scroll"), ThemeAssetIds::LogScrollToBottom);
    autoScroll_->defaultAction()->setCheckable(true);
    autoScroll_->defaultAction()->setChecked(true);
    copy_ = tool(QStringLiteral("MonitorCatalogCopy"), tr("Copy selected records"), ThemeAssetIds::LogCopy);
    clear_ = tool(QStringLiteral("MonitorCatalogClear"), tr("Clear records"), ThemeAssetIds::LogClear);
    connect(levelFilter_, &QComboBox::currentIndexChanged, this, [this] { rebuildVisibleRows(); });
    connect(copy_, &QToolButton::clicked, this, &MonitorCatalogPanel::copySelected);
    connect(clear_, &QToolButton::clicked, this, [this] { if (capture_) capture_->clear(); });
    connect(autoScroll_, &QToolButton::toggled, this, [this](bool checked) {
        if (checked) table_->scrollToBottom();
    });
    auto* copyShortcut = new QShortcut(QKeySequence::Copy, table_);
    copyShortcut->setContext(Qt::WidgetShortcut);
    connect(copyShortcut, &QShortcut::activated, this, &MonitorCatalogPanel::copySelected);

    connect(categoryFilter_, &QComboBox::currentIndexChanged, this, [this] { rebuildVisibleRows(); });
    connect(textFilter_, &QLineEdit::textChanged, this, [this] { rebuildVisibleRows(); });
    connect(jumpButton_, &QPushButton::clicked, this, &MonitorCatalogPanel::onJumpClicked);

    gapLabel_->hide();
    refreshJumpButton();
}

QString MonitorCatalogPanel::gapMessage() const
{
    return gapLabel_ ? gapLabel_->text() : QString();
}

QVariantMap MonitorCatalogPanel::snapshotParams() const
{
    QVariantMap params;
    params.insert(QStringLiteral("filterText"), textFilter_->text());
    params.insert(QStringLiteral("filterLevel"), levelFilter_->currentData());
    params.insert(QStringLiteral("filterCategory"), categoryFilter_->currentData());
    params.insert(QStringLiteral("autoScroll"), autoScroll_->isChecked());
    return params;
}

void MonitorCatalogPanel::restoreParams(const QVariantMap& params)
{
    const QSignalBlocker categoryBlocker(categoryFilter_);
    const QSignalBlocker levelBlocker(levelFilter_);
    const QSignalBlocker textBlocker(textFilter_);
    categoryFilter_->setCurrentIndex(std::max(0, categoryFilter_->findData(params.value(QStringLiteral("filterCategory"), 0))));
    levelFilter_->setCurrentIndex(std::max(0, levelFilter_->findData(params.value(QStringLiteral("filterLevel"), -1))));
    textFilter_->setText(params.value(QStringLiteral("filterText")).toString());
    autoScroll_->setChecked(params.value(QStringLiteral("autoScroll"), true).toBool());
    rebuildVisibleRows();
}

int MonitorCatalogPanel::visibleRowCount() const
{
    return filter_ ? filter_->rowCount() : 0;
}

void MonitorCatalogPanel::rebuildVisibleRows()
{
    if (!filter_) return;
    filter_->category = categoryFilter_->currentData().toInt();
    filter_->level = levelFilter_->currentData().toInt();
    filter_->search = textFilter_->text().trimmed();
    filter_->refresh();
    refreshCaptureView();
}

void MonitorCatalogPanel::setJumpLocator(MonitorJumpLocator locator)
{
    locator_ = std::move(locator);
    refreshJumpButton();
}

void MonitorCatalogPanel::refreshJumpButton()
{
    if (!jumpButton_ || !capture_ || !filter_) return;
    const auto index = filter_->mapToSource(table_->currentIndex());
    const auto* event = model_->eventAt(index.row());
    jumpButton_->setEnabled(locator_ && event && table_->selectionModel()->hasSelection() &&
                             capture_->matchingProjectObject(*event) != 0);
    jumpButton_->setToolTip(jumpButton_->isEnabled() ? tr("Show object in project")
        : tr("No matching object in the current project"));
}

bool MonitorCatalogPanel::jumpCurrentRow()
{
    if (!capture_ || !filter_ || !table_->currentIndex().isValid()) return false;
    const auto index = filter_->mapToSource(table_->currentIndex());
    const auto* event = model_->eventAt(index.row());
    if (!event || !locator_ || capture_->matchingProjectObject(*event) == 0) return false;
    locator_(event->header.stableId);
    return true;
}

void MonitorCatalogPanel::onJumpClicked()
{
    jumpCurrentRow();
}

void MonitorCatalogPanel::setCaptureController(Monitor::EventCaptureController* capture)
{
    if (!capture || capture_ == capture) return;
    Q_ASSERT(!capture_);
    capture_ = capture;
    model_ = new MonitorCatalogModel(capture, this);
    filter_ = new MonitorCatalogFilter(this);
    filter_->setSourceModel(model_);
    tableFrame_->setModel(filter_);
    table_->setWordWrap(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->setColumnWidth(MonitorCatalogModel::Time, 100);
    table_->setColumnWidth(MonitorCatalogModel::Level, 70);
    table_->setColumnWidth(MonitorCatalogModel::Category, 90);
    table_->setColumnWidth(MonitorCatalogModel::Object, 160);
    table_->horizontalHeader()->setStretchLastSection(true);
    jumpButton_->setText({});
    jumpButton_->setFixedSize(26, 22);
    jumpButton_->setAccessibleName(tr("Show object in project"));
    const auto refreshJumpIcon = [this] {
        jumpButton_->setIcon(ThemeManager::instance().resolveIcon(QString::fromLatin1(ThemeAssetIds::SelectorTarget)).icon);
    };
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, jumpButton_, refreshJumpIcon);
    refreshJumpIcon();
    autoScroll_->show();
    copy_->show();
    clear_->show();
    connect(capture, &Monitor::EventCaptureController::stateChanged, this, &MonitorCatalogPanel::refreshCaptureView);
    connect(capture, &Monitor::EventCaptureController::projectObjectsChanged, this, &MonitorCatalogPanel::refreshJumpButton);
    connect(filter_, &QAbstractItemModel::rowsInserted, this, [this] {
        refreshCaptureView();
        if (autoScroll_->isChecked()) table_->scrollToBottom();
    });
    connect(filter_, &QAbstractItemModel::rowsRemoved, this, &MonitorCatalogPanel::refreshCaptureView);
    connect(filter_, &QAbstractItemModel::modelReset, this, &MonitorCatalogPanel::refreshCaptureView);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MonitorCatalogPanel::refreshDetails);
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, &MonitorCatalogPanel::refreshDetails);
    rebuildVisibleRows();
}

void MonitorCatalogPanel::refreshCaptureView()
{
    if (!capture_) return;
    QStringList status{capture_->statusText(), tr("%1 / %2 records").arg(visibleRowCount()).arg(capture_->rowCount())};
    if (capture_->incomplete()) status << tr("Incomplete");
    if (capture_->sourceDropped()) status << tr("Source lost: %1").arg(capture_->sourceDropped());
    if (capture_->deliveryDropped()) status << tr("Delivery lost: %1").arg(capture_->deliveryDropped());
    if (capture_->transportDropped()) status << tr("Transport lost: %1").arg(capture_->transportDropped());
    if (capture_->trimmedRecords()) status << tr("History trimmed: %1").arg(capture_->trimmedRecords());
    status_->setText(status.join(QStringLiteral(" | ")));
    QString empty;
    if (!capture_->rowCount()) {
        using Monitor::EventCaptureState;
        switch (capture_->state()) {
        case EventCaptureState::Idle:
        case EventCaptureState::Capturing:
        case EventCaptureState::Stopped: break;
        default: empty = capture_->statusText(); break;
        }
    }
    gapLabel_->setText(empty);
    gapLabel_->setVisible(!empty.isEmpty());
    table_->show();
    clear_->setEnabled(capture_->rowCount() > 0);
    refreshDetails();
}

void MonitorCatalogPanel::refreshDetails()
{
    if (!filter_) return;
    const auto index = filter_->mapToSource(table_->currentIndex());
    const bool selected = table_->selectionModel()->hasSelection();
    copy_->setEnabled(selected);
    details_->setVisible(selected && index.isValid());
    const auto text = selected && index.isValid() ? model_->details(index.row()) : QString{};
    if (details_->toPlainText() != text) details_->setPlainText(text);
    refreshJumpButton();
}

void MonitorCatalogPanel::copySelected()
{
    if (!filter_) return;
    auto selected = table_->selectionModel()->selectedRows();
    std::sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) { return a.row() < b.row(); });
    QStringList lines;
    for (const auto& index : selected) {
        QStringList cells;
        for (int column = 0; column < filter_->columnCount(); ++column)
            cells << filter_->index(index.row(), column).data().toString().replace(QLatin1Char('\n'), QLatin1Char(' '));
        lines << cells.join(QLatin1Char('\t'));
    }
    if (!lines.isEmpty()) QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}

} // namespace VNX
