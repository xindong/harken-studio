#include "Monitor/MonitorPanel.h"
#include "Monitor/MonitorTypes.h"
#include "Common/TableWidget.h"
#include "Common/TableItemDelegate.h"
#include "Common/ListFooter.h"
#include "Common/ElidedLabel.h"
#include "Core/ThemeManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/QtControlSkin.h"
#include "Selector/ObjectRefNavigation.h"
#include "Selector/RefDomainAdapter.h"

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
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

namespace {

class MonitorObjectDelegate final : public TableItemDelegate
{
public:
    explicit MonitorObjectDelegate(QTableView* view)
        : TableItemDelegate(view), view_(view)
    {
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override
    {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const auto id = index.data(MonitorCatalogModel::ProjectObjectIdRole).toUInt();
            if (mouse->button() == Qt::LeftButton && id && refDomainAdapter(RefDomain::ProjectObject).exists(id)) {
                if (event->type() == QEvent::MouseButtonPress) {
                    ObjectRefNavigation::locate(view_, RefDomain::ProjectObject, id);
                } else {
                    QMetaObject::invokeMethod(this, [this, reference = QPersistentModelIndex(index), id] {
                        if (reference.isValid() && reference.data(MonitorCatalogModel::ProjectObjectIdRole).toUInt() == id
                            && refDomainAdapter(RefDomain::ProjectObject).exists(id))
                            ObjectRefNavigation::navigate(view_, RefDomain::ProjectObject, id);
                    }, Qt::QueuedConnection);
                }
            }
        }
        return TableItemDelegate::editorEvent(event, model, option, index);
    }

protected:
    void paintContents(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const auto id = index.data(MonitorCatalogModel::ProjectObjectIdRole).toUInt();
        if (!id) {
            TableItemDelegate::paintContents(painter, option, index);
            return;
        }
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        const auto icon = background.icon;
        background.icon = {};
        background.features &= ~QStyleOptionViewItem::HasDecoration;
        background.text.clear();
        view_->style()->drawControl(QStyle::CE_ItemViewItem, &background, painter, view_);
        const auto& theme = ThemeManager::instance();
        const auto& metrics = theme.metrics();
        const auto& colors = theme.colors();
        QRect rect = option.rect.adjusted(metrics.table.cellPaddingX, metrics.table.cellPaddingY,
                                         -metrics.table.cellPaddingX, -metrics.table.cellPaddingY);
        if (rect.isEmpty()) return;
        const int height = std::min(rect.height(), QtControlSkin(theme.snapshot()).fieldHeight());
        rect.setTop(rect.top() + (rect.height() - height) / 2);
        rect.setHeight(height);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        painter->save();
        painter->setClipRect(option.rect, Qt::IntersectClip);
        painter->fillRect(rect, selected ? colors.bgSelected : colors.bgPrimary);
        painter->setPen(selected ? colors.accent : colors.borderPrimary);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(rect.adjusted(0, 0, -1, -1));
        rect.adjust(1, 1, -1, -1);
        const int padding = std::clamp(rect.width(), 0, 12);
        const int left = std::min(5, padding);
        rect.adjust(left, 0, -(padding - left), 0);
        if (rect.width() >= metrics.objectIconExtent) {
            const QRect iconRect(rect.x(), rect.y() + (rect.height() - metrics.objectIconExtent) / 2,
                                 metrics.objectIconExtent, metrics.objectIconExtent);
            icon.paint(painter, iconRect, Qt::AlignCenter, QIcon::Disabled);
            rect.setLeft(iconRect.right() + 1 + 5);
        }
        if (!rect.isEmpty()) {
            painter->setFont(option.font);
            painter->setPen(colors.textSecondary);
            painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter,
                              option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight, rect.width()));
        }
        painter->restore();
    }

private:
    QTableView* view_;
};

}

class MonitorCatalogFilter final : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    Monitor::DesignerEventType type = Monitor::DesignerEventType::None;
    int level = -1;
    QString search;
    void refresh() { invalidateFilter(); }

protected:
    bool filterAcceptsRow(int row, const QModelIndex&) const override
    {
        const auto* model = static_cast<const MonitorCatalogModel*>(sourceModel());
        const auto* event = model->eventAt(row);
        if (!event || !event->isDesignerEvent() || (type != Monitor::DesignerEventType::None && event->type() != type) ||
            (level >= 0 && event->level() != level)) return false;
        if (search.isEmpty()) return true;
        for (int column = 0; column < MonitorCatalogModel::ColumnCount; ++column)
            if (model->data(model->index(row, column)).toString().contains(search, Qt::CaseInsensitive)) return true;
        return false;
    }
};

MonitorPanel::MonitorPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorPanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    gapLabel_ = new QLabel(this);
    gapLabel_->setObjectName(QStringLiteral("MonitorGap"));
    gapLabel_->setWordWrap(true);
    root->addWidget(gapLabel_);

    auto* filters = new QHBoxLayout();
    levelFilter_ = new QComboBox(this);
    levelFilter_->setObjectName(QStringLiteral("MonitorLevelFilter"));
    levelFilter_->addItem(tr("All levels"), -1);
    levelFilter_->addItem(tr("Info"), VNX::Monitor::kLogInfo);
    levelFilter_->addItem(tr("Warning"), VNX::Monitor::kLogWarn);
    levelFilter_->addItem(tr("Error"), VNX::Monitor::kLogError);
    filters->addWidget(levelFilter_);
    typeFilter_ = new QComboBox(this);
    typeFilter_->setObjectName(QStringLiteral("MonitorTypeFilter"));
    typeFilter_->addItem(tr("All types"), 0);
    typeFilter_->addItem(tr("Action"), static_cast<int>(Monitor::DesignerEventType::Action));
    typeFilter_->addItem(tr("Step"), static_cast<int>(Monitor::DesignerEventType::Step));
    typeFilter_->addItem(tr("Playable"), static_cast<int>(Monitor::DesignerEventType::Playable));
    typeFilter_->addItem(tr("Variable"), static_cast<int>(Monitor::DesignerEventType::Variable));
    typeFilter_->addItem(tr("Game Object"), static_cast<int>(Monitor::DesignerEventType::GameObject));
    filters->addWidget(typeFilter_);

    textFilter_ = new QLineEdit(this);
    textFilter_->setObjectName(QStringLiteral("MonitorTextFilter"));
    textFilter_->setPlaceholderText(tr("Search"));
    textFilter_->setMinimumWidth(0);
    textFilter_->setClearButtonEnabled(true);
    filters->addWidget(textFilter_);
    jumpButton_ = new QPushButton(QStringLiteral("Jump"), this);
    jumpButton_->setObjectName(QStringLiteral("MonitorJumpButton"));
    jumpButton_->setEnabled(false);
    filters->addWidget(jumpButton_);
    root->addLayout(filters);

    tableFrame_ = new TableWidget(TableOptions::display(), this);
    table_ = tableFrame_->createModelView(QStringLiteral("MonitorTable"), nullptr);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setItemDelegateForColumn(MonitorCatalogModel::Object, new MonitorObjectDelegate(table_));
    root->addWidget(tableFrame_, 1);

    details_ = new QTextBrowser(this);
    details_->setObjectName(QStringLiteral("MonitorDetails"));
    details_->setMaximumHeight(170);
    details_->setMinimumHeight(70);
    details_->setOpenLinks(false);
    details_->hide();
    root->addWidget(details_);
    auto* footer = tableFrame_->footer(false);
    status_ = new ElidedLabel(footer);
    status_->setObjectName(QStringLiteral("MonitorStatus"));
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
    autoScroll_ = tool(QStringLiteral("MonitorAutoScroll"), tr("Auto-scroll"), ThemeAssetIds::LogScrollToBottom);
    autoScroll_->defaultAction()->setCheckable(true);
    autoScroll_->defaultAction()->setChecked(true);
    copy_ = tool(QStringLiteral("MonitorCopy"), tr("Copy selected records"), ThemeAssetIds::LogCopy);
    clear_ = tool(QStringLiteral("MonitorClear"), tr("Clear records"), ThemeAssetIds::LogClear);
    connect(levelFilter_, &QComboBox::currentIndexChanged, this, [this] { rebuildVisibleRows(); });
    connect(copy_, &QToolButton::clicked, this, &MonitorPanel::copySelected);
    connect(clear_, &QToolButton::clicked, this, [this] { if (capture_) capture_->clear(); });
    connect(autoScroll_, &QToolButton::toggled, this, [this](bool checked) {
        if (checked) table_->scrollToBottom();
    });
    auto* copyShortcut = new QShortcut(QKeySequence::Copy, table_);
    copyShortcut->setContext(Qt::WidgetShortcut);
    connect(copyShortcut, &QShortcut::activated, this, &MonitorPanel::copySelected);

    connect(typeFilter_, &QComboBox::currentIndexChanged, this, [this] { rebuildVisibleRows(); });
    connect(textFilter_, &QLineEdit::textChanged, this, [this] { rebuildVisibleRows(); });
    connect(jumpButton_, &QPushButton::clicked, this, &MonitorPanel::onJumpClicked);

    gapLabel_->hide();
    refreshJumpButton();
}

QString MonitorPanel::gapMessage() const
{
    return gapLabel_ ? gapLabel_->text() : QString();
}

QVariantMap MonitorPanel::snapshotParams() const
{
    QVariantMap params;
    params.insert(QStringLiteral("filterText"), textFilter_->text());
    params.insert(QStringLiteral("filterLevel"), levelFilter_->currentData());
    params.insert(QStringLiteral("filterType"), typeFilter_->currentData());
    params.insert(QStringLiteral("autoScroll"), autoScroll_->isChecked());
    return params;
}

void MonitorPanel::restoreParams(const QVariantMap& params)
{
    const QSignalBlocker typeBlocker(typeFilter_);
    const QSignalBlocker levelBlocker(levelFilter_);
    const QSignalBlocker textBlocker(textFilter_);
    typeFilter_->setCurrentIndex(std::max(0, typeFilter_->findData(params.value(QStringLiteral("filterType"), 0))));
    levelFilter_->setCurrentIndex(std::max(0, levelFilter_->findData(params.value(QStringLiteral("filterLevel"), -1))));
    textFilter_->setText(params.value(QStringLiteral("filterText")).toString());
    autoScroll_->setChecked(params.value(QStringLiteral("autoScroll"), true).toBool());
    rebuildVisibleRows();
}

int MonitorPanel::visibleRowCount() const
{
    return filter_ ? filter_->rowCount() : 0;
}

void MonitorPanel::rebuildVisibleRows()
{
    if (!filter_) return;
    filter_->type = static_cast<Monitor::DesignerEventType>(typeFilter_->currentData().toInt());
    filter_->level = levelFilter_->currentData().toInt();
    filter_->search = textFilter_->text().trimmed();
    filter_->refresh();
    refreshCaptureView();
}

void MonitorPanel::setJumpLocator(MonitorJumpLocator locator)
{
    locator_ = std::move(locator);
    refreshJumpButton();
}

void MonitorPanel::refreshJumpButton()
{
    if (!jumpButton_ || !capture_ || !filter_) return;
    const auto index = filter_->mapToSource(table_->currentIndex());
    const auto* event = model_->eventAt(index.row());
    jumpButton_->setEnabled(locator_ && event && table_->selectionModel()->hasSelection() &&
                             capture_->matchingProjectObject(*event) != 0);
    jumpButton_->setToolTip(jumpButton_->isEnabled() ? tr("Show object in project")
        : tr("No matching object in the current project"));
}

bool MonitorPanel::jumpCurrentRow()
{
    if (!capture_ || !filter_ || !table_->currentIndex().isValid()) return false;
    const auto index = filter_->mapToSource(table_->currentIndex());
    const auto* event = model_->eventAt(index.row());
    if (!event || !locator_ || capture_->matchingProjectObject(*event) == 0) return false;
    locator_(event->objectId());
    return true;
}

void MonitorPanel::onJumpClicked()
{
    jumpCurrentRow();
}

void MonitorPanel::setCaptureController(Monitor::EventCaptureController* capture)
{
    if (!capture || capture_ == capture) return;
    Q_ASSERT(!capture_);
    capture_ = capture;
    model_ = new MonitorCatalogModel(capture, this);
    filter_ = new MonitorCatalogFilter(this);
    filter_->setSourceModel(model_);
    tableFrame_->setModel(filter_);
    table_->setWordWrap(false);
    tableFrame_->setViewColumns(QStringLiteral("MonitorTable"), {
        {QStringLiteral("time"), {}, 10},
        {QStringLiteral("level"), {}, 7},
        {QStringLiteral("type"), {}, 9},
        {QStringLiteral("description"), {}, 18},
        {QStringLiteral("requestId"), {}, 17},
        {QStringLiteral("object"), {}, 15},
        {QStringLiteral("objectId"), {}, 28},
        {QStringLiteral("gameObject"), {}, 12},
        {QStringLiteral("gameObjectId"), {}, 17}
    });
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
    connect(capture, &Monitor::EventCaptureController::stateChanged, this, &MonitorPanel::refreshCaptureView);
    connect(capture, &Monitor::EventCaptureController::projectObjectsChanged, this, &MonitorPanel::refreshJumpButton);
    connect(filter_, &QAbstractItemModel::rowsInserted, this, [this] {
        refreshCaptureView();
        if (autoScroll_->isChecked()) table_->scrollToBottom();
    });
    connect(filter_, &QAbstractItemModel::rowsRemoved, this, &MonitorPanel::refreshCaptureView);
    connect(filter_, &QAbstractItemModel::modelReset, this, &MonitorPanel::refreshCaptureView);
    connect(filter_, &QAbstractItemModel::dataChanged, this, &MonitorPanel::refreshDetails);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MonitorPanel::refreshDetails);
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, &MonitorPanel::refreshDetails);
    rebuildVisibleRows();
}

void MonitorPanel::refreshCaptureView()
{
    if (!capture_) return;
    QStringList status{capture_->statusText(), tr("%1 records").arg(visibleRowCount())};
    if (capture_->incomplete()) status << tr("Incomplete");
    if (capture_->sourceDropped() || capture_->deliveryDropped() || capture_->transportDropped())
        status << tr("Some records were lost");
    if (capture_->trimmedRecords()) status << tr("History limit reached");
    status_->setText(status.join(QStringLiteral(" | ")));
    QString empty;
    if (!visibleRowCount()) {
        using Monitor::EventCaptureState;
        switch (capture_->state()) {
        case EventCaptureState::Idle:
        case EventCaptureState::Capturing:
        case EventCaptureState::Stopped: empty = tr("No matching log entries"); break;
        default: empty = capture_->statusText(); break;
        }
    }
    gapLabel_->setText(empty);
    gapLabel_->setVisible(!empty.isEmpty());
    table_->show();
    clear_->setEnabled(capture_->rowCount() > 0);
    refreshDetails();
}

void MonitorPanel::refreshDetails()
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

void MonitorPanel::copySelected()
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
