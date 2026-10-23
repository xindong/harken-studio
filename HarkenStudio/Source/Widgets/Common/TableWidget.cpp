#include "TableWidget.h"

#include "Core/FontManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "LayoutDebugTint.h"
#include "ListFooter.h"
#include "ProportionalTreeWidget.h"
#include "TableGroupHeader.h"
#include "TableItemDelegate.h"
#include "ElidedLabel.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QListWidget>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QTimer>
#include <QTableView>
#include <QTableWidget>
#include <QToolTip>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <numeric>

namespace VNX {

namespace {

class TableNavigationList : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex& index,
                                                         const QEvent* event) const override
    {
        if (!index.isValid()) return QItemSelectionModel::NoUpdate;
        const auto command = QListWidget::selectionCommand(index, event);
        return command.testFlag(QItemSelectionModel::Deselect) || command.testFlag(QItemSelectionModel::Toggle)
            ? QItemSelectionModel::ClearAndSelect : command;
    }
};

class TableRowsScrollArea : public QScrollArea
{
public:
    using QScrollArea::QScrollArea;
    using QScrollArea::setViewportMargins;
};

class TableRowHandle : public QWidget
{
public:
    TableRowHandle(QWidget* parent, ThemeManager* theme) : QWidget(parent), theme_(theme) {}

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        QtControlSkin(theme_->snapshot()).paintDragHandle(painter, rect(), isEnabled());
    }

private:
    ThemeManager* theme_;
};

QBoxLayout* cellLayout(QLayout* layout, QWidget* cell)
{
    if (layout->indexOf(cell) >= 0) return qobject_cast<QBoxLayout*>(layout);
    for (int i = 0; i < layout->count(); ++i)
        if (auto* nested = layout->itemAt(i)->layout())
            if (auto* owner = cellLayout(nested, cell)) return owner;
    return nullptr;
}

}

TableCell::TableCell(QWidget* parent, ThemeManager* themeManager)
    : QWidget(parent), themeManager_(themeManager), layout_(new QHBoxLayout(this))
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(themeManager_, &ThemeManager::themeChanged, this, &TableCell::applyTheme);
    applyTheme();
}

void TableCell::addWidget(QWidget* widget, int stretch, Qt::Alignment alignment)
{
    if (stretch > 0) {
        widget->setMinimumWidth(0);
        widget->setMaximumWidth(QWIDGETSIZE_MAX);
        auto policy = widget->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::Ignored);
        widget->setSizePolicy(policy);
    }
    layout_->addWidget(widget, stretch, alignment);
}

void TableCell::applyTheme()
{
    const auto& metrics = themeManager_->metrics().table;
    layout_->setContentsMargins(metrics.cellPaddingX, metrics.cellPaddingY,
                               metrics.cellPaddingX, metrics.cellPaddingY);
    layout_->setSpacing(metrics.cellPaddingX * 2);
    positionHandle();
    if (handle_) handle_->update();
}

void TableCell::positionHandle()
{
    if (!handle_) return;
    const auto geometry = QtControlSkin(themeManager_->snapshot()).tableHandleLayout(rect(), this);
    handle_->setGeometry(geometry.handle);
    layout_->setContentsMargins(geometry.content.left(), geometry.content.top(),
        width() - geometry.content.x() - geometry.content.width(),
        height() - geometry.content.y() - geometry.content.height());
}

void TableCell::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    positionHandle();
}

void TableCell::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (handle_ && property("selected").toBool()) {
        QPainter painter(this);
        QtControlSkin(themeManager_->snapshot()).paintTableCellSelection(painter, rect());
    }
}

TableWidget::TableWidget(QWidget* parent, ThemeManager* themeManager)
    : TableWidget(TableOptions{}, parent, themeManager)
{
}

TableWidget::TableWidget(const TableOptions& options, QWidget* parent, ThemeManager* themeManager)
    : QFrame(parent), themeManager_(themeManager), options_(options),
      layout_(new QVBoxLayout(this))
{
    if (!themeManager_)
        themeManager_ = &ThemeManager::instance();
    setObjectName(QStringLiteral("listBox"));
    setProperty("tableKind", options.kind == TableKind::Config ? "config" : "display");
    setAttribute(Qt::WA_StyledBackground, true);
    installEventFilter(this);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    DebugTint::tint(this, DebugTint::Role::Box);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);
    layout_->setAlignment(options_.bodySizing == TableBodySizing::Expanding
        ? Qt::Alignment{} : Qt::AlignTop);
    connect(themeManager_, &ThemeManager::themeChanged, this, [this] { applyTheme(); });
    connect(&FontManager::instance(), &FontManager::fontChanged, this, [this] { applyTheme(); });
}

void TableWidget::ensureRows()
{
    Q_ASSERT(!view_);
    if (rows_) return;
    scroll_ = new TableRowsScrollArea(this);
    scroll_->setProperty("tableBody", true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    headerClip_ = new QWidget(scroll_);
    headerClip_->setObjectName(QStringLiteral("tableHeaderClip"));
    headerClip_->setAttribute(Qt::WA_StyledBackground, true);
    headerClip_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    headerClip_->installEventFilter(this);
    headerClip_->hide();
    header_ = new QWidget(headerClip_);
    header_->setObjectName(QStringLiteral("listHead"));
    header_->setAttribute(Qt::WA_StyledBackground, true);
    header_->setMouseTracking(true);
    header_->installEventFilter(this);
    auto* headerLayout = new QHBoxLayout(header_);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);
    DebugTint::tint(header_, DebugTint::Role::Head);
    connect(scroll_->horizontalScrollBar(), &QScrollBar::valueChanged, this,
        [this](int value) { header_->move(-value, 0); });
    scroll_->viewport()->installEventFilter(this);
    rows_ = new QWidget(scroll_);
    rows_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* rowsLayout = new QVBoxLayout(rows_);
    rowsLayout->setSizeConstraint(QLayout::SetNoConstraint);
    rowsLayout->setContentsMargins(0, 0, 0, 0);
    rowsLayout->setSpacing(0);
    rowsLayout->setAlignment(Qt::AlignTop);
    rows_->installEventFilter(this);
    scroll_->setWidget(rows_);
    layout_->insertWidget(0, scroll_);
    initializeScrollArea(scroll_);
    applySizing();
    applyTheme();
}

void TableWidget::setColumns(const QString& key, const QList<TableColumn>& columns)
{
    ensureRows();
    Q_ASSERT(rowBindings_.isEmpty());
    qDeleteAll(labels_);
    labels_.clear();
    columns_ = columns;
    visualOrder_.clear();
    for (int i = 0; i < columns.size(); ++i) visualOrder_.append(i);
    hiddenColumns_ = QList<bool>(columns.size(), false);
    QList<int> weights;
    QStringList keys;
    for (const auto& column : columns) {
        auto* label = new ElidedLabel(column.title, header_);
        label->setObjectName(column.objectName);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        header_->layout()->addWidget(label);
        labels_.append(label);
        weights.append(column.weight);
        keys.append(column.id);
    }
    columnLayout_ = TableColumnLayout(weights);
    columnLayout_.setKey(key, keys);
    headerClip_->setVisible(!columns.isEmpty());
    applyTheme();
}

QLabel* TableWidget::headerCell(int column) const
{
    return labels_.at(column);
}

void TableWidget::setColumnWeights(const QString& key, const QList<int>& weights)
{
    Q_ASSERT(weights.size() == columns_.size());
    QStringList keys;
    for (int i = 0; i < columns_.size(); ++i) {
        columns_[i].weight = weights[i];
        keys.append(columns_[i].id);
    }
    columnLayout_ = TableColumnLayout(weights);
    columnLayout_.setKey(key, keys);
    scheduleLayout();
}

void TableWidget::setColumnMinimumWidth(int column, int width)
{
    columns_[column].minimumWidth = width;
    scheduleLayout();
}

void TableWidget::setColumnVisible(int column, bool visible)
{
    hiddenColumns_[column] = !visible;
    labels_[column]->setVisible(visible);
    for (const auto& row : rowBindings_)
        if (row.cells[column]) row.cells[column]->setVisible(visible);
    scheduleLayout();
}

QWidget* TableWidget::rowsWidget()
{
    ensureRows();
    return rows_;
}

void TableWidget::setHeaderVisible(bool visible)
{
    ensureRows();
    headerClip_->setVisible(visible);
    scheduleLayout();
}

QVBoxLayout* TableWidget::rowsLayout()
{
    return static_cast<QVBoxLayout*>(rowsWidget()->layout());
}

QLabel* TableWidget::bodyMessage()
{
    if (!message_) {
        message_ = new QLabel(this);
        message_->setWordWrap(true);
        message_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        message_->hide();
        layout_->insertWidget(footer_ ? layout_->indexOf(footer_) : layout_->count(), message_);
        applyTheme();
    }
    return message_;
}

ListFooter* TableWidget::footer(bool standardButtons)
{
    if (!footer_) {
        footer_ = new ListFooter(this, standardButtons, themeManager_);
        footer_->installEventFilter(this);
        DebugTint::tint(footer_, DebugTint::Role::Foot);
        layout_->addWidget(footer_);
    }
    return footer_;
}

ListFooter* TableWidget::createGroupFooter(bool standardButtons)
{
    auto* footer = new ListFooter(rowsWidget(), standardButtons, themeManager_);
    QtControlSkin::setSemanticProperty(footer, "tableGroupFooter", true);
    footer->installEventFilter(this);
    DebugTint::tint(footer, DebugTint::Role::Foot);
    rowsLayout()->addWidget(footer);
    scheduleLayout();
    return footer;
}

QTableView* TableWidget::createModelView(const QString& name, QAbstractItemModel* model)
{
    Q_ASSERT(!rows_ && !view_);
    auto* view = new QTableView(this);
    view->setObjectName(name);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setModel(model);
    attachView(view);
    return view;
}

QListWidget* TableWidget::createNavigationView(const QString& name)
{
    Q_ASSERT(!rows_ && !view_);
    navigation_ = true;
    auto* view = new TableNavigationList(this);
    view->setObjectName(name);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    attachView(view);
    return view;
}

ProportionalTreeWidget* TableWidget::createItemView(const QString& name, const QStringList& headers,
                                                  const QList<int>& columnWeights)
{
    Q_ASSERT(!rows_ && !view_);
    Q_ASSERT(headers.size() == columnWeights.size());
    auto* view = new ProportionalTreeWidget(columnWeights, this, themeManager_);
    view->setObjectName(name);
    view->setHeaderLabels(headers);
    attachView(view);
    return view;
}

TableWidget::~TableWidget()
{
    qApp->removeEventFilter(this);
    for (const auto& connection : modelConnections_) disconnect(connection);
}

void TableWidget::setModel(QAbstractItemModel* model)
{
    Q_ASSERT(view_);
    Q_ASSERT(!qobject_cast<QTreeWidget*>(view_) && !qobject_cast<QTableWidget*>(view_));
    if (view_->model() == model) return;
    cancelRowMove();
    ++selectionGeneration_;
    viewColumns_.clear();
    viewColumnLayoutKey_.clear();
    {
        const QScopedValueRollback guard(applyingLayout_, true);
        view_->setModel(model);
        bindViewModel();
    }
    applyTheme();
}

void TableWidget::setViewColumns(const QString& key, const QList<TableColumn>& columns)
{
    Q_ASSERT(view_ && view_->model() && columns.size() == view_->model()->columnCount());
    viewColumnLayoutKey_ = key;
    viewColumns_ = columns;
    if (auto* table = qobject_cast<QTableWidget*>(view_)) {
        QStringList titles;
        for (const auto& column : columns) titles.append(column.title);
        table->setHorizontalHeaderLabels(titles);
    } else {
        for (int column = 0; column < columns.size(); ++column)
            view_->model()->setHeaderData(column, Qt::Horizontal, columns[column].title);
    }
    widths_.clear();
    scrollableViewWidths_.clear();
    refreshLayout();
}

void TableWidget::setRowNumberColumn(int column)
{
    Q_ASSERT(view_ && view_->model() && column >= 0 && column < view_->model()->columnCount());
    view_->setProperty("rowNumberColumn", column);
    view_->model()->setHeaderData(column, Qt::Horizontal, Qt::AlignCenter, Qt::TextAlignmentRole);
    view_->viewport()->update();
}

void TableWidget::attachView(QAbstractItemView* view)
{
    Q_ASSERT(!rows_ && !view_);
    view_ = view;
    view->setParent(this);
    QtControlSkin::setSemanticProperty(view, "tableRole",
        navigation_ ? QStringLiteral("navigation") : QStringLiteral("data"));
    view->setAlternatingRowColors(false);
    view->setMouseTracking(true);
    view->viewport()->installEventFilter(this);
    view->installEventFilter(this);
    if (auto* table = qobject_cast<QTableView*>(view)) {
        table->setShowGrid(false);
        table->verticalHeader()->hide();
    }
    if (auto* tree = qobject_cast<ProportionalTreeWidget*>(view)) {
        tree->setColumnLayoutKey(view->objectName());
    } else if (!navigation_)
        view->setItemDelegate(new TableItemDelegate(view, themeManager_));
    if (auto* header = viewHeader()) {
        header->installEventFilter(this);
        header->setStretchLastSection(false);
        header->setSectionResizeMode(QHeaderView::Interactive);
        header->setSectionsMovable(options_.columnsMovable);
        header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        connect(header, &QHeaderView::sectionResized, this, [this](int column, int, int width) {
            if (!applyingLayout_ && columnsFitToWidth())
                resizeViewColumn(column, width);
        });
        connect(header, &QHeaderView::sectionMoved, this, [this] { scheduleLayout(); });
        connect(header, &QHeaderView::sectionCountChanged, this, [this] {
            scrollableViewWidths_.clear();
            scheduleLayout();
        });
    }
    if (auto* tree = qobject_cast<QTreeView*>(view)) {
        connect(tree, &QTreeView::expanded, this, [this] { scheduleLayout(); });
        connect(tree, &QTreeView::collapsed, this, [this] { scheduleLayout(); });
    }
    bindViewModel();
    layout_->insertWidget(0, view);
    initializeScrollArea(view);
    applySizing();
    applyTheme();
}

void TableWidget::initializeScrollArea(QAbstractScrollArea* area)
{
    for (auto* bar : {area->verticalScrollBar(), area->horizontalScrollBar()}) {
        QtControlSkin::setSemanticProperty(bar, "scrollRole", QStringLiteral("table"));
        connect(bar, &QScrollBar::rangeChanged, bar, [bar](int minimum, int maximum) {
            bar->setEnabled(minimum < maximum);
        });
        bar->setEnabled(bar->minimum() < bar->maximum());
    }
}

bool TableWidget::columnsFitToWidth() const
{
    return options_.bodySizing == TableBodySizing::Content
        || options_.columnSizing == TableColumnSizing::Fit;
}

void TableWidget::applySizing()
{
    const bool expanding = options_.bodySizing == TableBodySizing::Expanding;
    layout_->setAlignment(expanding ? Qt::Alignment{} : Qt::AlignTop);
    auto* area = scroll_ ? static_cast<QAbstractScrollArea*>(scroll_) : view_;
    if (!area) return;
    if (auto* tree = qobject_cast<ProportionalTreeWidget*>(view_))
        tree->setColumnsFitToWidth(columnsFitToWidth());
    area->setSizePolicy(QSizePolicy::Ignored, expanding ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    area->setVerticalScrollBarPolicy(expanding ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
    area->setHorizontalScrollBarPolicy(columnsFitToWidth() ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAlwaysOn);
    if (columnsFitToWidth()) {
        area->horizontalScrollBar()->setValue(0);
        auto* corner = area->cornerWidget();
        area->setCornerWidget(nullptr);
        delete corner;
    } else if (!area->cornerWidget()) {
        auto* corner = new QWidget(area);
        corner->setObjectName(QStringLiteral("tableScrollCorner"));
        corner->setAttribute(Qt::WA_StyledBackground, true);
        area->setCornerWidget(corner);
    }
    area->setMaximumHeight(QWIDGETSIZE_MAX);
    area->setMinimumHeight(bodyHeightForContent(0));
    layout_->invalidate();
    layout_->activate();
}

int TableWidget::bodyHeightForContent(int contentHeight) const
{
    auto* area = scroll_ ? static_cast<QAbstractScrollArea*>(scroll_) : view_;
    int height = std::max(4, contentHeight) + area->frameWidth() * 2;
    auto* header = scroll_ ? headerClip_ : viewHeader();
    if (header && !header->isHidden()) height += header->height();
    if (area->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOn)
        height += area->horizontalScrollBar()->sizeHint().height();
    return height;
}

void TableWidget::bindViewModel()
{
    for (const auto& connection : modelConnections_) disconnect(connection);
    modelConnections_.clear();
    widths_.clear();
    scrollableViewWidths_.clear();
    if (auto* model = view_->model()) {
        modelConnections_.append(connect(model, &QAbstractItemModel::rowsInserted,
                                         this, [this] { cancelRowMove(); scheduleLayout(); }));
        modelConnections_.append(connect(model, &QAbstractItemModel::rowsRemoved,
                                         this, [this] { cancelRowMove(); scheduleLayout(); }));
        modelConnections_.append(connect(model, &QAbstractItemModel::modelReset,
                                         this, [this] { cancelRowMove(); scheduleLayout(); }));
        modelConnections_.append(connect(model, &QAbstractItemModel::layoutChanged,
                                         this, [this] { cancelRowMove(); scheduleLayout(); }));
        modelConnections_.append(connect(model, &QAbstractItemModel::dataChanged,
                                         this, [this] { scheduleLayout(); }));
    }
}

TableCell* TableWidget::createCell(QWidget* parent)
{
    return new TableCell(parent, themeManager_);
}

void TableWidget::registerRow(QWidget* row, const QList<QWidget*>& cells, QWidget* details,
                              const QString& groupId)
{
    Q_ASSERT(cells.size() == columns_.size());
    Row binding;
    binding.widget = row;
    binding.groupId = groupId;
    row->setAttribute(Qt::WA_StyledBackground, true);
    if (row->objectName() != QLatin1String("listRow")) row->setProperty("listRow", true);
    row->installEventFilter(this);
    if (row->focusPolicy() == Qt::NoFocus) row->setFocusPolicy(Qt::ClickFocus);
    for (int i = 0; i < cells.size(); ++i) {
        auto* cell = cells[i];
        binding.cells.append(cell);
        if (options_.kind == TableKind::Config && cell->property("tableRowNumber").toBool()) {
            binding.number = cell;
            binding.handle = cell;
            binding.selectionCell = cell;
        } else if (auto* tableCell = qobject_cast<TableCell*>(cell); tableCell && tableCell->handle_) {
            binding.handle = tableCell->handle_;
            binding.selectionCell = cell;
        }
        updateCellMetrics(cell);
        if (hiddenColumns_[i]) cell->hide();
    }
    if (binding.selectionCell) QtControlSkin::setSemanticProperty(this, "cellSelection", true);
    if (details) {
        Q_ASSERT(details->parentWidget() == rows_ && details->layout());
        binding.details = details;
        binding.detailsIndent = details->layout()->contentsMargins().left();
        details->setAttribute(Qt::WA_StyledBackground, true);
        QtControlSkin::setSemanticProperty(details, "listRow", true);
        details->installEventFilter(this);
        if (binding.number) {
            binding.detailsNumber = new QFrame(details);
            binding.detailsNumber->setAttribute(Qt::WA_TransparentForMouseEvents);
            binding.detailsNumber->setProperty("tableRowNumber", true);
            binding.detailsNumber->lower();
        }
        connect(row, &QObject::destroyed, details, &QObject::deleteLater);
    }
    rowBindings_.append(binding);
    refreshRowState(binding);
    if (options_.columnsMovable) applyColumnOrder();
    scheduleLayout();
}

QWidget* TableWidget::rowForWidget(QWidget* widget) const
{
    if (!rows_ || !widget || widget == rows_ || !rows_->isAncestorOf(widget)) return nullptr;
    while (widget->parentWidget() != rows_) widget = widget->parentWidget();
    for (const auto& row : rowBindings_)
        if (row.widget == widget || row.details == widget) return row.widget;
    return nullptr;
}

void TableWidget::refreshRowState(const Row& row)
{
    if (!row.widget) return;
    const bool selected = row.widget->property("selected").toBool();
    QtControlSkin::setSemanticProperty(row.selectionCell, "selected", selected);
    QtControlSkin::setSemanticProperty(row.widget, "rowDetailsVisible",
        row.details && !row.details->isHidden());
    if (row.details) {
        QtControlSkin::setSemanticProperty(row.details, "selected", selected);
        QtControlSkin::setSemanticProperty(row.details, "lastRow", row.widget->property("lastRow"));
        QtControlSkin::setSemanticProperty(row.detailsNumber, "selected", selected);
    }
}

void TableWidget::refreshRowDetails(const Row& row)
{
    if (!row.details || !row.detailsNumber || !row.number) return;
    const bool visible = !row.number->isHidden();
    const int left = visible ? row.number->mapTo(rows_, QPoint()).x() - row.details->x() : 0;
    const int width = visible ? row.number->width() : 0;
    row.detailsNumber->setGeometry(left, 0, width, row.details->height());
    row.detailsNumber->setVisible(visible);
    auto* layout = row.details->layout();
    auto margins = layout->contentsMargins();
    margins.setLeft(left + width + themeManager_->metrics().table.cellPaddingX + row.detailsIndent);
    layout->setContentsMargins(margins);
    layout->activate();
}

QLabel* TableWidget::createRowNumber(QWidget* row, int index, std::optional<int> displayNumber)
{
    auto* number = new QLabel(QString::number(displayNumber.value_or(index + 1)), row);
    number->setObjectName(QStringLiteral("tableRowNumber"));
    number->setProperty("tableRowNumber", true);
    number->setProperty("dragHandleRow", index);
    number->setCursor(Qt::SizeVerCursor);
    number->setFocusPolicy(Qt::StrongFocus);
    number->setToolTip(tr("Move Row"));
    updateCellMetrics(number);
    return number;
}

QWidget* TableWidget::createRowHandle(TableCell* cell, int index)
{
    Q_ASSERT(!cell->handle_);
    auto* handle = new TableRowHandle(cell, themeManager_);
    handle->setObjectName(QStringLiteral("tableRowHandle"));
    handle->setProperty("dragHandleRow", index);
    handle->setCursor(Qt::SizeVerCursor);
    handle->setFocusPolicy(Qt::StrongFocus);
    handle->setToolTip(tr("Move Row"));
    cell->handle_ = handle;
    cell->applyTheme();
    return handle;
}

TableGroupHeader* TableWidget::createGroupHeader(const QString& text, const TableGroupOptions& options)
{
    auto* header = new TableGroupHeader(text, rowsWidget(), themeManager_, options);
    rowsLayout()->addWidget(header);
    scheduleLayout();
    return header;
}

void TableWidget::setRowMoveHandler(std::function<void(int, int)> handler,
                                    std::function<bool(int, int)> guard)
{
    cancelRowMove();
    rowMoveHandler_ = std::move(handler);
    rowMoveGuard_ = std::move(guard);
    qApp->installEventFilter(this);
}

void TableWidget::setDropHandler(std::function<bool(const QMimeData*)> handler,
                                 std::function<bool(const QMimeData*)> guard)
{
    dropHandler_ = std::move(handler);
    dropGuard_ = std::move(guard);
    setAcceptDrops(dropHandler_ && dropGuard_);
    QtControlSkin::setSemanticProperty(this, "dropActive", false);
}

bool TableWidget::canAcceptDrop(const QDropEvent* event) const
{
    if (!isEnabled() || !dropHandler_ || !dropGuard_
        || !(event->possibleActions() & Qt::CopyAction))
        return false;
    for (auto* child = childAt(event->position().toPoint()); child && child != this;
         child = child->parentWidget()) {
        if (child->acceptDrops() && child != view_ && child != scroll_
            && (!view_ || child != view_->viewport())
            && (!scroll_ || child != scroll_->viewport()))
            return false;
    }
    return dropGuard_(event->mimeData());
}

void TableWidget::dragEnterEvent(QDragEnterEvent* event)
{
    dragMoveEvent(event);
}

void TableWidget::dragMoveEvent(QDragMoveEvent* event)
{
    const bool accepted = canAcceptDrop(event);
    QtControlSkin::setSemanticProperty(this, "dropActive", accepted);
    if (accepted) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void TableWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    QtControlSkin::setSemanticProperty(this, "dropActive", false);
    QFrame::dragLeaveEvent(event);
}

void TableWidget::dropEvent(QDropEvent* event)
{
    QtControlSkin::setSemanticProperty(this, "dropActive", false);
    if (canAcceptDrop(event) && dropHandler_(event->mimeData())) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void TableWidget::setSelectionClearHandler(std::function<void()> handler)
{
    selectionClearHandler_ = std::move(handler);
}

void TableWidget::setRowSelectionHandler(std::function<void(QWidget*)> handler)
{
    rowSelectionHandler_ = std::move(handler);
}

QList<QWidget*> TableWidget::selectableWidgetRows(const std::optional<QString>& groupId) const
{
    QList<QWidget*> rows;
    if (!rows_) return rows;
    const auto* layout = rows_->layout();
    for (int i = 0; i < layout->count(); ++i) {
        auto* widget = layout->itemAt(i)->widget();
        if (!widget || !widget->isEnabled()
            || (widget->isHidden() && widget->testAttribute(Qt::WA_WState_ExplicitShowHide)))
            continue;
        if (std::any_of(rowBindings_.begin(), rowBindings_.end(),
                       [widget, &groupId](const Row& row) {
                           return row.widget == widget && (!groupId || row.groupId == *groupId);
                       }))
            rows.append(widget);
    }
    return rows;
}

QModelIndexList TableWidget::selectableViewRows() const
{
    QModelIndexList rows;
    if (!view_ || !view_->model() || view_->selectionMode() == QAbstractItemView::NoSelection)
        return rows;
    const auto* model = view_->model();
    const auto* tree = qobject_cast<QTreeView*>(view_);
    const auto* table = qobject_cast<QTableView*>(view_);
    const auto* header = viewHeader();
    int column = 0;
    while (header && column < header->count() && header->isSectionHidden(column)) ++column;
    const auto append = [&](auto&& self, const QModelIndex& parent) -> void {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            if ((table && table->isRowHidden(row)) || (tree && tree->isRowHidden(row, parent)))
                continue;
            const auto index = model->index(row, column, parent);
            if (!index.isValid() || !(index.flags() & Qt::ItemIsEnabled)) continue;
            if (index.flags() & Qt::ItemIsSelectable) rows.append(index);
            const auto branch = model->index(row, 0, parent);
            if (tree && tree->isExpanded(branch)) self(self, branch);
        }
    };
    append(append, view_->rootIndex());
    return rows;
}

bool TableWidget::removeSelectedRow(const std::function<bool()>& remove, const QModelIndex& removedIndex)
{
    if (!remove) return false;
    int position = -1;
    QString groupId;
    const QPointer<QAbstractItemModel> model = view_ ? view_->model() : nullptr;
    if (view_) {
        const auto rows = selectableViewRows();
        for (int i = 0; i < rows.size(); ++i)
            if (removedIndex.isValid()
                    ? rows[i] == removedIndex.siblingAtColumn(rows[i].column())
                    : view_->selectionModel()->isSelected(rows[i])) {
                position = i;
                break;
            }
    } else {
        const auto rows = selectableWidgetRows();
        for (int i = 0; i < rows.size(); ++i)
            if (rows[i]->property("selected").toBool()) {
                const auto row = std::find_if(rowBindings_.begin(), rowBindings_.end(),
                    [&](const Row& row) { return row.widget == rows[i]; });
                groupId = row->groupId;
                position = selectableWidgetRows(groupId).indexOf(rows[i]);
                break;
            }
    }
    const auto generation = ++selectionGeneration_;
    const QPointer<TableWidget> self(this);
    const bool removed = remove();
    if (!self || !removed || position < 0) return removed;
    QTimer::singleShot(0, this, [this, generation, position, model, groupId] {
        if (generation != selectionGeneration_) return;
        if (view_) {
            if (!model || view_->model() != model) return;
            const auto rows = selectableViewRows();
            QPersistentModelIndex target;
            auto* selection = view_->selectionModel();
            QItemSelectionModel::SelectionFlags flags =
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows;
            if (view_->selectionMode() != QAbstractItemView::SingleSelection
                && selection->hasSelection()) {
                const auto current = selection->currentIndex().siblingAtColumn(
                    rows.isEmpty() ? 0 : rows.front().column());
                const auto selected = std::find_if(rows.begin(), rows.end(),
                    [selection](const QModelIndex& row) { return selection->isSelected(row); });
                if (rows.contains(current) && selection->isSelected(current))
                    target = current;
                else if (selected != rows.end())
                    target = *selected;
                else if (!rows.isEmpty())
                    target = rows[std::min(position, int(rows.size()) - 1)];
                flags = QItemSelectionModel::NoUpdate;
            } else if (rows.isEmpty()) {
                clearSelection();
                return;
            } else
                target = rows[std::min(position, int(rows.size()) - 1)];
            const QPointer<QAbstractItemView> view(view_);
            selection->setCurrentIndex(target, flags);
            if (view) {
                if (target.isValid()) view->scrollTo(target);
                view->setFocus(Qt::OtherFocusReason);
            }
        } else {
            const auto rows = selectableWidgetRows(groupId);
            if (rows.isEmpty()) {
                clearSelection();
                return;
            }
            const QPointer<QWidget> target = rows[std::min(position, int(rows.size()) - 1)];
            const QPointer<TableWidget> self(this);
            if (rowSelectionHandler_) rowSelectionHandler_(target);
            if (!self || !target) return;
            target->setFocus(Qt::OtherFocusReason);
            QTimer::singleShot(0, this, [this, generation, target] {
                if (generation != selectionGeneration_ || !target) return;
                refreshLayout();
                scroll_->ensureWidgetVisible(target);
            });
        }
    });
    return removed;
}

bool TableWidget::hasSelection() const
{
    if (view_)
        return view_->currentIndex().isValid() || view_->selectionModel()->hasSelection();
    return selectionClearHandler_ && std::any_of(rowBindings_.begin(), rowBindings_.end(),
        [](const Row& row) { return row.widget && row.widget->property("selected").toBool(); });
}

void TableWidget::clearSelection()
{
    cancelRowMove();
    ++selectionGeneration_;
    if (view_) {
        view_->setCurrentIndex({});
        view_->clearSelection();
    } else if (selectionClearHandler_) {
        selectionClearHandler_();
    }
}

QList<QWidget*> TableWidget::movableRows() const
{
    QList<QWidget*> rows;
    for (const auto& binding : rowBindings_)
        if (binding.widget && binding.widget->parentWidget() == rows_
            && !binding.widget->isHidden() && binding.handle
            && binding.handle->isVisibleTo(binding.widget))
            rows.append(binding.handle);
    return rows;
}

QModelIndexList TableWidget::movableViewRows() const
{
    if (!view_ || !view_->property("rowNumberColumn").isValid()) return {};
    const int column = view_->property("rowNumberColumn").toInt();
    if (viewHeader()->isSectionHidden(column)) return {};
    auto rows = selectableViewRows();
    rows.removeIf([this](const QModelIndex& index) { return index.parent() != view_->rootIndex(); });
    for (auto& index : rows) index = index.siblingAtColumn(column);
    rows.removeIf([](const QModelIndex& index) {
        return !(index.flags() & Qt::ItemIsEnabled) || !(index.flags() & Qt::ItemIsSelectable);
    });
    return rows;
}

bool TableWidget::canMoveRow(int from, int to) const
{
    if (!rowMoveHandler_ || !isEnabled() || from == to) return false;
    if (view_) {
        const auto rows = movableViewRows();
        const auto available = [&rows](int row) {
            return std::any_of(rows.begin(), rows.end(),
                [row](const QModelIndex& index) { return index.row() == row; });
        };
        return available(from) && available(to) && (!rowMoveGuard_ || rowMoveGuard_(from, to));
    }
    const auto rows = movableRows();
    const auto available = [&rows](int index) {
        return std::any_of(rows.begin(), rows.end(), [index](const QWidget* number) {
            return number->isEnabled() && number->property("dragHandleRow").toInt() == index;
        });
    };
    return available(from) && available(to) && (!rowMoveGuard_ || rowMoveGuard_(from, to));
}

void TableWidget::moveRow(int from, int to)
{
    if (!canMoveRow(from, to)) return;
    if (view_) {
        const QPointer<QAbstractItemModel> model(view_->model());
        const auto generation = selectionGeneration_;
        const QPointer<TableWidget> self(this);
        const auto handler = rowMoveHandler_;
        handler(from, to);
        if (self)
            QTimer::singleShot(0, this, [this, model, generation, to] {
                if (model != view_->model() || generation != selectionGeneration_) return;
                for (const auto& index : movableViewRows()) {
                    if (index.row() != to) continue;
                    view_->selectionModel()->setCurrentIndex(index,
                        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                    view_->scrollTo(index);
                    view_->setFocus(Qt::OtherFocusReason);
                    break;
                }
            });
        return;
    }
    const bool restoreFocus = focusWidget() && focusWidget()->property("dragHandleRow").isValid();
    const QPointer<TableWidget> self(this);
    const auto handler = rowMoveHandler_;
    handler(from, to);
    if (self && restoreFocus)
        QTimer::singleShot(0, this, [this, to] {
            for (auto* number : movableRows())
                if (number->property("dragHandleRow").toInt() == to) {
                    number->setFocus(Qt::OtherFocusReason);
                    break;
                }
        });
}

int TableWidget::rowMoveTarget(const QPoint& globalPosition, int* indicatorY) const
{
    if (view_) {
        if (!draggedIndex_.isValid() || !view_->isVisible()
            || !view_->viewport()->rect().contains(view_->viewport()->mapFromGlobal(globalPosition)))
            return -1;
        const auto rows = movableViewRows();
        const int fromPosition = rows.indexOf(draggedIndex_);
        if (fromPosition < 0) return -1;
        const int y = view_->viewport()->mapFromGlobal(globalPosition).y();
        int insertion = 0;
        for (const auto& index : rows)
            if (y >= view_->visualRect(index).center().y()) ++insertion;
        const int toPosition = std::clamp(insertion - (insertion > fromPosition ? 1 : 0),
                                          0, static_cast<int>(rows.size()) - 1);
        const int to = rows[toPosition].row();
        if (!canMoveRow(draggedIndex_.row(), to)) return -1;
        if (indicatorY)
            *indicatorY = insertion < rows.size() ? view_->visualRect(rows[insertion]).top()
                                                  : view_->visualRect(rows.back()).bottom() - 1;
        return to;
    }
    if (!draggedNumber_
        || !scroll_->viewport()->rect().contains(scroll_->viewport()->mapFromGlobal(globalPosition)))
        return -1;
    const auto rows = movableRows();
    const int fromPosition = rows.indexOf(draggedNumber_);
    if (fromPosition < 0) return -1;
    const int y = rows_->mapFromGlobal(globalPosition).y();
    int insertion = 0;
    for (auto* number : rows)
        if (y >= rowForWidget(number)->geometry().center().y()) ++insertion;
    const int toPosition = std::clamp(insertion - (insertion > fromPosition ? 1 : 0),
                                      0, static_cast<int>(rows.size()) - 1);
    const int from = draggedNumber_->property("dragHandleRow").toInt();
    const int to = rows[toPosition]->property("dragHandleRow").toInt();
    if (!canMoveRow(from, to)) return -1;
    if (indicatorY)
        *indicatorY = insertion < rows.size() ? rowForWidget(rows[insertion])->geometry().top()
                                               : rowForWidget(rows.back())->geometry().bottom() - 1;
    return to;
}

void TableWidget::cancelRowMove()
{
    draggedNumber_.clear();
    draggedIndex_ = QPersistentModelIndex();
    if (rowMoveIndicator_) rowMoveIndicator_->hide();
}

void TableWidget::setBodyExpanding(bool expanding)
{
    setBodySizing(expanding ? TableBodySizing::Expanding : TableBodySizing::Content);
}

void TableWidget::setBodySizing(TableBodySizing sizing)
{
    options_.bodySizing = sizing;
    applySizing();
    scheduleLayout();
}

void TableWidget::setColumnSizing(TableColumnSizing sizing)
{
    options_.columnSizing = sizing;
    if (sizing == TableColumnSizing::Fit) scrollableViewWidths_.clear();
    applySizing();
    scheduleLayout();
}

QHeaderView* TableWidget::viewHeader() const
{
    if (auto* tree = qobject_cast<QTreeView*>(view_)) return tree->header();
    if (auto* table = qobject_cast<QTableView*>(view_)) return table->horizontalHeader();
    return nullptr;
}

void TableWidget::setColumnsMovable(bool movable)
{
    options_.columnsMovable = movable;
    movingColumn_ = -1;
    if (auto* header = viewHeader()) header->setSectionsMovable(movable);
}

QList<int> TableWidget::columnOrder() const
{
    if (auto* header = viewHeader()) {
        QList<int> order;
        for (int i = 0; i < header->count(); ++i) order.append(header->logicalIndex(i));
        return order;
    }
    return visualOrder_;
}

void TableWidget::moveColumn(int logicalColumn, int visualIndex)
{
    if (!options_.columnsMovable) return;
    const auto order = columnOrder();
    if (!order.contains(logicalColumn) || visualIndex < 0 || visualIndex >= order.size()) return;
    if (auto* header = viewHeader())
        header->moveSection(header->visualIndex(logicalColumn), visualIndex);
    else {
        visualOrder_.move(visualOrder_.indexOf(logicalColumn), visualIndex);
        applyColumnOrder();
        refreshLayout();
    }
}

void TableWidget::applyColumnOrder()
{
    auto* headerLayout = qobject_cast<QHBoxLayout*>(header_->layout());
    for (int i : visualOrder_) {
        headerLayout->removeWidget(labels_[i]);
        headerLayout->addWidget(labels_[i]);
    }
    for (const auto& row : rowBindings_) {
        if (!row.widget) continue;
        auto* cells = row.cells.isEmpty() || !row.cells.front()
            ? nullptr : cellLayout(row.widget->layout(), row.cells.front());
        if (!cells) continue;
        int start = cells->count();
        for (const auto& cell : row.cells)
            if (cell) start = std::min(start, cells->indexOf(cell));
        for (const auto& cell : row.cells) if (cell) cells->removeWidget(cell);
        for (int i : visualOrder_)
            if (row.cells[i]) cells->insertWidget(start++, row.cells[i]);
    }
}

void TableWidget::resizeViewColumn(int column, int width)
{
    if (qobject_cast<ProportionalTreeWidget*>(view_) || !view_->model()
        || widths_.size() != view_->model()->columnCount()) return;
    columnLayout_.resizeColumn(column, width, widths_, minimums_, columnOrder());
    refreshViewLayout();
}

void TableWidget::refreshViewColumns()
{
    const auto& metrics = themeManager_->metrics().table;
    auto* header = viewHeader();
    if (header && !columnsFitToWidth()) {
        for (auto it = scrollableViewWidths_.cbegin(); it != scrollableViewWidths_.cend(); ++it)
            header->resizeSection(it.key(), it.value());
        scrollableViewWidths_.clear();
    }
    if (header && columnsFitToWidth()
        && !qobject_cast<ProportionalTreeWidget*>(view_)) {
        const int count = view_->model()->columnCount();
        if (widths_.size() != count) {
            Q_ASSERT(viewColumns_.isEmpty() || viewColumns_.size() == count);
            QList<int> weights;
            QStringList keys;
            for (int i = 0; i < count; ++i) {
                weights.append(viewColumns_.isEmpty() ? 1 : viewColumns_[i].weight);
                if (viewColumns_.isEmpty())
                    keys.append(view_->model()->headerData(i, Qt::Horizontal).toString());
                else
                    keys.append(viewColumns_[i].id);
            }
            columnLayout_ = TableColumnLayout(weights);
            columnLayout_.setKey(viewColumnLayoutKey_.isEmpty() ? view_->objectName() : viewColumnLayoutKey_, keys);
        }
        QList<bool> hidden;
        for (int i = 0; i < count; ++i) hidden.append(header->isSectionHidden(i));
        minimums_ = QList<int>(count, metrics.minimumColumnWidth);
        for (int i = 0; i < viewColumns_.size(); ++i)
            minimums_[i] = std::max(viewColumns_[i].minimumWidth, viewColumns_[i].weight == 0
                ? metrics.compactColumnWidth : metrics.minimumColumnWidth);
        widths_ = columnLayout_.widths(view_->viewport()->width(), minimums_, hidden, true);
        header->setMinimumSectionSize(0);
        for (int i = 0; i < count; ++i) {
            if (hidden[i]) continue;
            if (options_.columnSizing == TableColumnSizing::Scrollable
                && !scrollableViewWidths_.contains(i))
                scrollableViewWidths_.insert(i, header->sectionSize(i));
            header->resizeSection(i, widths_[i]);
        }
    }
}

void TableWidget::refreshViewLayout()
{
    if (!view_ || !view_->model() || applyingLayout_) return;
    const QScopedValueRollback guard(applyingLayout_, true);
    refreshViewColumns();
    int height = 0;
    if (options_.bodySizing == TableBodySizing::Content) {
        auto* tree = qobject_cast<QTreeView*>(view_);
        auto* table = qobject_cast<QTableView*>(view_);
        std::function<int(const QModelIndex&)> contentHeight = [&](const QModelIndex& parent) {
            int height = 0;
            for (int row = 0; row < view_->model()->rowCount(parent); ++row) {
                if ((tree && tree->isRowHidden(row, parent)) || (table && table->isRowHidden(row))) continue;
                const auto index = view_->model()->index(row, 0, parent);
                QStyleOptionViewItem option;
                option.initFrom(view_);
                option.widget = view_;
                height += table ? table->rowHeight(row)
                    : view_->itemDelegateForIndex(index)->sizeHint(option, index).height();
                if (tree && tree->isExpanded(index)) height += contentHeight(index);
            }
            return height;
        };
        height = contentHeight(view_->rootIndex());
    }
    if (options_.bodySizing == TableBodySizing::Expanding)
        view_->setMinimumHeight(bodyHeightForContent(0));
    else
        view_->setFixedHeight(bodyHeightForContent(height));
    updateGeometry();
}

QSize TableWidget::minimumSizeHint() const
{
    return QSize(0, QFrame::minimumSizeHint().height());
}

void TableWidget::updateCellMetrics(QWidget* cell)
{
    const auto& metrics = themeManager_->metrics().table;
    if (auto* label = qobject_cast<QLabel*>(cell)) {
        cell->setContentsMargins(metrics.cellPaddingX, metrics.cellPaddingY,
                                metrics.cellPaddingX, metrics.cellPaddingY);
        if (cell->property("tableRowNumber").toBool()) label->setAlignment(Qt::AlignCenter);
    }
}

void TableWidget::applyTheme()
{
    if (message_) {
        const auto& metrics = themeManager_->metrics().table;
        message_->setFont(FontManager::instance().getFont(themeManager_->snapshot(), FontRole::Body));
        message_->setContentsMargins(metrics.cellPaddingX, metrics.cellPaddingY,
                                     metrics.cellPaddingX, metrics.cellPaddingY);
        message_->setMinimumHeight(metrics.rowHeight);
    }
    if (view_) {
        const auto& metrics = themeManager_->metrics().table;
        const auto font = FontManager::instance().getFont(themeManager_->snapshot(), FontRole::Body);
        view_->setFont(font);
        const int iconExtent = themeManager_->metrics().objectIconExtent;
        view_->setIconSize(QSize(iconExtent, iconExtent));
        if (auto* table = qobject_cast<QTableView*>(view_); table && !table->property("compositeTable").toBool()) {
            table->verticalHeader()->setMinimumSectionSize(0);
            table->verticalHeader()->setDefaultSectionSize(std::max(metrics.rowHeight,
                std::max({QFontMetrics(font).height(), iconExtent,
                    QtControlSkin(themeManager_->snapshot()).fieldHeight()}) + metrics.cellPaddingY * 2));
        }
        if (auto* header = viewHeader()) {
            header->setFont(font);
            header->setFixedHeight(std::max(metrics.headerHeight, QFontMetrics(font).height() + metrics.cellPaddingY * 2));
        }
        view_->doItemsLayout();
        if (options_.bodySizing == TableBodySizing::Expanding)
            view_->setMinimumHeight(bodyHeightForContent(0));
        scheduleLayout();
    }
    if (!rows_) return;
    const auto& metrics = themeManager_->metrics().table;
    const auto headerFont = FontManager::instance().getFont(themeManager_->snapshot(), FontRole::Body);
    for (auto* label : labels_) {
        label->setFont(headerFont);
        updateCellMetrics(label);
    }
    const int height = std::max(metrics.headerHeight,
        QFontMetrics(headerFont).height() + metrics.cellPaddingY * 2);
    headerClip_->setFixedHeight(height);
    if (options_.bodySizing == TableBodySizing::Expanding)
        scroll_->setMinimumHeight(bodyHeightForContent(0));
    for (const auto& row : rowBindings_)
        for (const auto& cell : row.cells)
            if (cell) updateCellMetrics(cell);
    scheduleLayout();
}

void TableWidget::scheduleLayout()
{
    if (layoutPending_ || applyingLayout_) return;
    layoutPending_ = true;
    QTimer::singleShot(0, this, [this] {
        layoutPending_ = false;
        refreshLayout();
    });
}

void TableWidget::refreshLayout()
{
    if (view_) { refreshViewLayout(); return; }
    if (!rows_ || applyingLayout_) return;
    const QScopedValueRollback guard(applyingLayout_, true);
    ensurePolished();
    bool lastWidget = true;
    for (int i = rows_->layout()->count() - 1; i >= 0; --i) {
        auto* widget = rows_->layout()->itemAt(i)->widget();
        if (!widget || (widget->isHidden() && widget->testAttribute(Qt::WA_WState_ExplicitShowHide)))
            continue;
        if (widget->property("tableGroupFooter").toBool())
            QtControlSkin::setSemanticProperty(widget, "lastRow", lastWidget);
        lastWidget = false;
    }
    layout_->activate();
    const int headerHeight = headerClip_->isHidden() ? 0 : headerClip_->height();
    static_cast<TableRowsScrollArea*>(scroll_)->setViewportMargins(0, headerHeight, 0, 0);
    const auto& metrics = themeManager_->metrics().table;
    rowBindings_.erase(std::remove_if(rowBindings_.begin(), rowBindings_.end(),
        [](const Row& row) { return row.widget.isNull(); }), rowBindings_.end());
    if (!draggedNumber_ || !movableRows().contains(draggedNumber_)) {
        draggedNumber_.clear();
        if (rowMoveIndicator_) rowMoveIndicator_->hide();
    }
    minimums_.clear();
    for (int i = 0; i < columns_.size(); ++i) {
        const auto& column = columns_[i];
        int minimum = std::max(column.minimumWidth,
            column.weight > 0 ? metrics.minimumColumnWidth : metrics.compactColumnWidth);
        if (column.weight == 0)
            minimum = std::max(minimum, labels_[i]->fontMetrics().horizontalAdvance(column.title)
                                          + metrics.cellPaddingX * 2);
        minimums_.append(minimum);
    }
    for (const auto& row : rowBindings_) {
        int height = std::max(metrics.rowHeight,
            row.widget->fontMetrics().height() + metrics.cellPaddingY * 2);
        for (int i = 0; i < row.cells.size(); ++i) {
            auto* cell = row.cells[i].data();
            if (!cell) continue;
            QSize hint = cell->minimumSizeHint();
            if (auto* number = qobject_cast<QLabel*>(cell); number && cell == row.number) {
                const auto margins = number->contentsMargins();
                hint.setWidth(number->fontMetrics().horizontalAdvance(number->text())
                    + margins.left() + margins.right());
            }
            height = std::max(height, hint.height());
            if (!row.widget->isHidden() && columns_[i].weight == 0)
                minimums_[i] = std::max(minimums_[i], hint.width());
        }
        height = std::max(height, row.widget->minimumSizeHint().height());
        row.widget->setFixedHeight(height);
    }
    for (int i = 0; i < minimums_.size(); ++i)
        if (hiddenColumns_[i]) minimums_[i] = 0;
    rows_->layout()->activate();
    const int rowHeight = rows_->layout()->totalSizeHint().height();
    rows_->setMinimumHeight(rowHeight);
    const int viewportWidth = std::max(0, scroll_->maximumViewportSize().width());
    const int minimumWidth = std::accumulate(minimums_.begin(), minimums_.end(), 0);
    if (columnsFitToWidth() && minimumWidth > viewportWidth) {
        int compactWidth = 0;
        for (int i = 0; i < columns_.size(); ++i)
            if (columns_[i].weight == 0) compactWidth += minimums_[i];
        const bool keepCompact = compactWidth <= viewportWidth;
        int remaining = viewportWidth - (keepCompact ? compactWidth : 0);
        int remainingMinimum = minimumWidth - (keepCompact ? compactWidth : 0);
        for (int i = 0; i < minimums_.size(); ++i) {
            if (minimums_[i] == 0 || (keepCompact && columns_[i].weight == 0)) continue;
            const int minimum = minimums_[i];
            minimums_[i] = static_cast<int>(qint64(remaining) * minimum / remainingMinimum);
            remaining -= minimums_[i];
            remainingMinimum -= minimum;
        }
    }
    widths_ = columnLayout_.widths(viewportWidth, minimums_, hiddenColumns_);
    const int contentWidth = std::accumulate(widths_.begin(), widths_.end(), 0);
    rows_->setMinimumWidth(columnsFitToWidth() ? 0 : contentWidth);
    headerClip_->setGeometry(scroll_->viewport()->x(), scroll_->contentsRect().top(), viewportWidth, headerHeight);
    header_->setGeometry(-scroll_->horizontalScrollBar()->value(), 0, contentWidth, headerClip_->height());
    int lastVisibleColumn = -1;
    for (int i : visualOrder_) if (!hiddenColumns_[i]) lastVisibleColumn = i;
    for (int i = 0; i < widths_.size(); ++i) {
        QtControlSkin::setSemanticProperty(labels_[i], "lastColumn", i == lastVisibleColumn);
        labels_[i]->setAlignment(labels_[i]->text() == QLatin1String("#")
            ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter);
        labels_[i]->setFixedWidth(widths_[i]);
    }
    header_->layout()->activate();
    for (const auto& row : rowBindings_) {
        for (int i = 0; i < row.cells.size(); ++i)
            if (row.cells[i]) row.cells[i]->setFixedWidth(widths_[i]);
        if (auto* layout = row.widget->layout()) layout->activate();
        refreshRowDetails(row);
    }
    if (options_.bodySizing == TableBodySizing::Expanding)
        scroll_->setMinimumHeight(bodyHeightForContent(0));
    else
        scroll_->setFixedHeight(bodyHeightForContent(rowHeight));
    updateGeometry();
}

int TableWidget::resizeColumnAt(int x) const
{
    const int radius = themeManager_->metrics().table.resizeHandleRadius;
    int lastWeighted = -1;
    for (int i : visualOrder_)
        if (columns_[i].weight > 0 && !hiddenColumns_[i]) lastWeighted = i;
    for (int i : visualOrder_) {
        if (i == lastWeighted) break;
        if (columns_[i].weight <= 0 || hiddenColumns_[i]) continue;
        if (std::abs(x - labels_[i]->geometry().right() - 1) <= radius) return i;
    }
    return -1;
}

bool TableWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel && options_.bodySizing == TableBodySizing::Content
        && ((scroll_ && watched == scroll_->viewport()) || (view_ && watched == view_->viewport()))) {
        event->ignore();
        return true;
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget && widget->parentWidget() == rows_) {
        if (event->type() == QEvent::DynamicPropertyChange) {
            const auto name = static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName();
            if (name == "selected" || name == "lastRow") {
                for (const auto& row : rowBindings_)
                    if (row.widget == widget) {
                        refreshRowState(row);
                        break;
                    }
            }
        } else if (event->type() == QEvent::Show || event->type() == QEvent::Hide
                   || event->type() == QEvent::Resize) {
            for (const auto& row : rowBindings_)
                if (row.details == widget) {
                    refreshRowState(row);
                    refreshRowDetails(row);
                    break;
                }
        }
    }
    const bool isNumber = widget && widget->property("dragHandleRow").isValid()
        && rowMoveHandler_ && movableRows().contains(widget);
    QModelIndex viewNumber;
    if (view_ && rowMoveHandler_ && view_->property("rowNumberColumn").isValid()) {
        if (watched == view_->viewport()) {
            if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove)
                viewNumber = view_->indexAt(static_cast<QMouseEvent*>(event)->position().toPoint());
            else if (event->type() == QEvent::ContextMenu)
                viewNumber = view_->indexAt(static_cast<QContextMenuEvent*>(event)->pos());
            else if (event->type() == QEvent::ToolTip)
                viewNumber = view_->indexAt(static_cast<QHelpEvent*>(event)->pos());
        } else if (watched == view_ && event->type() == QEvent::KeyPress)
            viewNumber = view_->currentIndex();
        if (viewNumber.column() != view_->property("rowNumberColumn").toInt()
            || viewNumber.parent() != view_->rootIndex()
            || !(viewNumber.flags() & Qt::ItemIsEnabled)
            || !(viewNumber.flags() & Qt::ItemIsSelectable))
            viewNumber = {};
        if (viewNumber.isValid() && event->type() == QEvent::ToolTip) {
            QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(), tr("Move Row"), view_);
            return true;
        }
    }
    if (view_ && watched == view_->viewport() && event->type() == QEvent::MouseMove) {
        const auto index = view_->indexAt(static_cast<QMouseEvent*>(event)->position().toPoint());
        const auto cellCursor = index.data(TableCursorRole);
        const auto cursor = viewNumber.isValid() ? Qt::SizeVerCursor
            : (index.flags() & Qt::ItemIsEnabled) && cellCursor.isValid()
                ? static_cast<Qt::CursorShape>(cellCursor.toInt()) : Qt::ArrowCursor;
        if (view_->viewport()->cursor().shape() != cursor)
            view_->viewport()->setCursor(cursor);
    }
    if (!navigation_ && view_ && watched == view_ && event->type() == QEvent::FocusIn
        && !view_->currentIndex().isValid()) {
        const auto mode = view_->selectionMode();
        view_->setSelectionMode(QAbstractItemView::NoSelection);
        view_->setCurrentIndex({});
        view_->setSelectionMode(mode);
    }
    if (!navigation_ && event->type() == QEvent::KeyPress
        && (watched == this || watched == view_ || isNumber)) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
            if (draggedNumber_ || draggedIndex_.isValid()) {
                cancelRowMove();
                return true;
            }
            if (hasSelection()) {
                clearSelection();
                return true;
            }
        }
    }
    if (!navigation_ && event->type() == QEvent::MouseButtonPress && hasSelection()) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            const QPoint global = mouse->globalPosition().toPoint();
            const auto* footer = qobject_cast<ListFooter*>(watched);
            bool blank = footer && (footer == footer_ || footer->parentWidget() == rows_)
                && !footer->childAt(footer->mapFromGlobal(global));
            if (view_ && watched == view_->viewport()) {
                const auto index = view_->indexAt(view_->viewport()->mapFromGlobal(global));
                blank = !index.isValid();
                if (index.isValid() && mouse->modifiers() == Qt::ControlModifier
                    && view_->selectionMode() == QAbstractItemView::SingleSelection
                    && view_->selectionModel()->isSelected(index)) {
                    clearSelection();
                    return true;
                }
            } else if (rows_ && (watched == rows_ || watched == scroll_->viewport())) {
                blank = !rows_->childAt(rows_->mapFromGlobal(global));
            }
            if (blank) clearSelection();
        }
    }
    if ((isNumber || viewNumber.isValid()) && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && widget->isEnabled()) {
            rowDragStart_ = mouse->globalPosition().toPoint();
            if (isNumber) {
                draggedNumber_ = widget;
                widget->setFocus(Qt::MouseFocusReason);
            } else {
                draggedIndex_ = viewNumber;
                view_->selectionModel()->setCurrentIndex(viewNumber,
                    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                view_->setFocus(Qt::MouseFocusReason);
            }
            return true;
        }
    }
    if ((draggedNumber_ || draggedIndex_.isValid())
        && (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease)) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint position = mouse->globalPosition().toPoint();
        const bool moved = (position - rowDragStart_).manhattanLength() >= QApplication::startDragDistance();
        int indicatorY = 0;
        const int target = moved ? rowMoveTarget(position, &indicatorY) : -1;
        if (event->type() == QEvent::MouseMove) {
            if (target >= 0) {
                auto* body = view_ ? view_->viewport() : rows_;
                if (!rowMoveIndicator_) {
                    rowMoveIndicator_ = new QFrame(body);
                    rowMoveIndicator_->setObjectName(QStringLiteral("tableRowInsertion"));
                    rowMoveIndicator_->setAttribute(Qt::WA_TransparentForMouseEvents);
                }
                rowMoveIndicator_->setGeometry(0, std::clamp(indicatorY, 0, body->height() - 2),
                                               body->width(), 2);
                rowMoveIndicator_->show();
                rowMoveIndicator_->raise();
            } else if (rowMoveIndicator_) {
                rowMoveIndicator_->hide();
            }
        } else if (mouse->button() == Qt::LeftButton) {
            const QPointer<QWidget> number = draggedNumber_;
            const QPersistentModelIndex index = draggedIndex_;
            const int from = number ? number->property("dragHandleRow").toInt() : index.row();
            const auto generation = selectionGeneration_;
            cancelRowMove();
            if (target >= 0)
                QTimer::singleShot(0, this, [this, number, index, generation, from, target] {
                    if (generation != selectionGeneration_) return;
                    if ((number && movableRows().contains(number))
                        || (index.isValid() && index.row() == from))
                        moveRow(from, target);
                });
        }
        if (view_) return true;
    }
    if ((isNumber || viewNumber.isValid())
        && (event->type() == QEvent::ContextMenu || event->type() == QEvent::KeyPress)) {
        QList<int> rows;
        if (isNumber) {
            for (auto* number : movableRows()) rows.append(number->property("dragHandleRow").toInt());
        } else {
            for (const auto& index : movableViewRows()) rows.append(index.row());
        }
        const int from = isNumber ? widget->property("dragHandleRow").toInt() : viewNumber.row();
        const int position = rows.indexOf(from);
        if (position < 0) return false;
        const int up = position > 0 ? rows[position - 1] : -1;
        const int down = position + 1 < rows.size() ? rows[position + 1] : -1;
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->modifiers() == Qt::AltModifier && (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
                moveRow(from, key->key() == Qt::Key_Up ? up : down);
                return true;
            }
        } else {
            QMenu menu;
            auto* moveUp = menu.addAction(tr("Move Up"));
            auto* moveDown = menu.addAction(tr("Move Down"));
            moveUp->setEnabled(canMoveRow(from, up));
            moveDown->setEnabled(canMoveRow(from, down));
            const QPointer<TableWidget> self(this);
            const QPointer<QWidget> number(isNumber ? widget : nullptr);
            const QPersistentModelIndex index(viewNumber);
            const auto* chosen = menu.exec(static_cast<QContextMenuEvent*>(event)->globalPos());
            if (self && chosen && ((number && self->movableRows().contains(number))
                                   || (index.isValid() && index.row() == from)))
                self->moveRow(from, chosen == moveUp ? up : down);
            return true;
        }
    }
    if (watched == header_) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                dragColumn_ = resizeColumnAt(mouse->position().toPoint().x());
                if (dragColumn_ >= 0) {
                    dragX_ = mouse->position().toPoint().x();
                    dragWidths_ = widths_;
                    return true;
                }
                if (options_.columnsMovable) {
                    for (int i : visualOrder_)
                        if (!hiddenColumns_[i] && labels_[i]->geometry().contains(mouse->position().toPoint())) {
                            movingColumn_ = i;
                            dragX_ = mouse->position().toPoint().x();
                            return true;
                        }
                }
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const int x = mouse->position().toPoint().x();
            if (dragColumn_ >= 0) {
                columnLayout_.resizeColumn(dragColumn_, dragWidths_[dragColumn_] + x - dragX_,
                                            dragWidths_, minimums_, visualOrder_);
                refreshLayout();
                return true;
            }
            header_->setCursor(resizeColumnAt(x) >= 0 ? Qt::SplitHCursor : Qt::ArrowCursor);
        } else if (event->type() == QEvent::MouseButtonRelease && movingColumn_ >= 0) {
            const int moving = movingColumn_;
            movingColumn_ = -1;
            const int x = static_cast<QMouseEvent*>(event)->position().toPoint().x();
            if (std::abs(x - dragX_) >= QApplication::startDragDistance()) {
                auto order = visualOrder_;
                order.removeOne(moving);
                int target = order.size();
                for (int i = 0; i < order.size(); ++i)
                    if (!hiddenColumns_[order[i]] && x < labels_[order[i]]->geometry().center().x()) {
                        target = i;
                        break;
                    }
                moveColumn(moving, target);
            }
            return true;
        } else if (event->type() == QEvent::MouseButtonRelease && dragColumn_ >= 0) {
            dragColumn_ = -1;
            return true;
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            if (resizeColumnAt(static_cast<QMouseEvent*>(event)->position().toPoint().x()) >= 0) {
                dragColumn_ = -1;
                columnLayout_.reset();
                refreshLayout();
                return true;
            }
        }
    }
    const bool isHeader = watched == headerClip_
        || (qobject_cast<QHeaderView*>(watched) && widget->parentWidget() == view_);
    if ((watched == rows_ || (view_ && (watched == view_ || watched == view_->viewport()))
         || (scroll_ && watched == scroll_->viewport())
         || watched == header_ || isHeader || (widget && widget->parentWidget() == rows_))
        && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize
        || event->type() == QEvent::Show || event->type() == QEvent::Hide
        || event->type() == QEvent::FontChange))
        scheduleLayout();
    return QFrame::eventFilter(watched, event);
}

void TableWidget::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::EnabledChange && !isEnabled())
        QtControlSkin::setSemanticProperty(this, "dropActive", false);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) applyTheme();
}

}
