#include "ProportionalTreeWidget.h"

#include "Core/QtControlSkin.h"
#include "Core/FontManager.h"
#include "Core/ThemeManager.h"
#include "ReorderHeaderView.h"
#include "TableGroupHeader.h"
#include "TableItemDelegate.h"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>

#include <algorithm>
#include <QScopedValueRollback>

namespace VNX {

ProportionalTreeWidget::ProportionalTreeWidget(const QList<int>& columnWeights, QWidget* parent,
                                               ThemeManager* themeManager)
    : QTreeWidget(parent), columnLayout_(columnWeights),
      themeManager_(themeManager ? themeManager : &ThemeManager::instance())
{
    Q_ASSERT(!columnWeights.isEmpty());
    for (int weight : columnWeights) Q_ASSERT(weight > 0);
    setColumnCount(columnWeights.size());
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setAlternatingRowColors(false);
    setRootIsDecorated(false);
    setUniformRowHeights(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHeader(new ReorderHeaderView(Qt::Horizontal, this));
    header()->setSectionsMovable(false);
    header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(QHeaderView::Interactive);
    setItemDelegate(new TableItemDelegate(this, themeManager_));
    header()->viewport()->installEventFilter(this);
    QtControlSkin::setSemanticProperty(this, "tableRole", QStringLiteral("data"));
    viewport()->installEventFilter(this);
    connect(header(), &QHeaderView::sectionResized, this, [this](int column, int, int newSize) {
        if (!applyingColumnWidths_ && newSize > 0) rebalanceColumnWeight(column, newSize);
    });
    connect(header(), &QHeaderView::sectionMoved, this, [this]() { applyColumnWidths(); });
    connect(themeManager_, &ThemeManager::themeChanged, this, [this] { applyTheme(); });
    connect(&FontManager::instance(), &FontManager::fontChanged, this, [this] { applyTheme(); });
    applyTheme();
}

void ProportionalTreeWidget::setColumnsHidden(const QList<int>& columns, bool hidden)
{
    {
        const QScopedValueRollback guard(applyingColumnWidths_, true);
        for (int column : columns) setColumnHidden(column, hidden);
    }
    applyColumnWidths();
}

void ProportionalTreeWidget::setHierarchyIndentation(int indentation)
{
    if (!hierarchyIndentation_) {
        setIndentation(std::max(0, indentation));
        return;
    }
    hierarchyIndentation_ = std::max(0, indentation);
    doItemsLayout();
    viewport()->update();
}

QRect ProportionalTreeWidget::cellContentsRect(const QRect& rect, const QModelIndex& index) const
{
    if (!hierarchyIndentation_) return rect;
    const int column = treePosition() < 0 ? header()->logicalIndex(0) : treePosition();
    if (index.column() != column || index.siblingAtColumn(0).data(TableGroupRole).toBool()) return rect;
    int depth = 0;
    for (auto parent = index.parent(); parent.isValid() && parent != rootIndex(); parent = parent.parent())
        if (!parent.data(TableGroupRole).toBool()) ++depth;
    const int offset = std::min(rect.width(), depth * *hierarchyIndentation_);
    return isRightToLeft() ? rect.adjusted(0, 0, -offset, 0) : rect.adjusted(offset, 0, 0, 0);
}

void ProportionalTreeWidget::drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const
{
    const auto first = index.siblingAtColumn(0);
    if (!first.data(TableGroupRole).toBool()) {
        QTreeWidget::drawRow(painter, option, index);
        return;
    }
    QRect rect = option.rect;
    rect.setLeft(0);
    rect.setRight(viewport()->width() - 1);
    const auto check = first.data(Qt::CheckStateRole);
    const TableGroupOptions controls{check.isValid(), first.data(TableGroupCollapsibleRole).toBool()};
    TableGroupHeader::paint(*painter, rect, first.data().toString(), themeManager_, controls, this);
    if (controls.checkable) {
        QStyleOptionViewItem indicator = option;
        indicator.rect = TableGroupHeader::checkBoxRect(rect, themeManager_, this);
        indicator.state = QStyle::State_None;
        if (isEnabled() && first.flags().testFlag(Qt::ItemIsEnabled)
            && first.flags().testFlag(Qt::ItemIsUserCheckable))
            indicator.state |= QStyle::State_Enabled;
        indicator.state |= check.toInt() == Qt::Checked ? QStyle::State_On
            : check.toInt() == Qt::PartiallyChecked ? QStyle::State_NoChange : QStyle::State_Off;
        style()->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &indicator, painter, this);
    }
    if (controls.collapsible)
        TableGroupHeader::chevronIcon(isExpanded(first), themeManager_).paint(painter,
            TableGroupHeader::chevronRect(rect, themeManager_), Qt::AlignCenter,
            isEnabled() && first.flags().testFlag(Qt::ItemIsEnabled) ? QIcon::Normal : QIcon::Disabled);
}

QTreeWidgetItem* ProportionalTreeWidget::createGroup(const QString& text, const TableGroupOptions& options)
{
    if (!hierarchyIndentation_) {
        hierarchyIndentation_ = indentation();
        setIndentation(0);
    }
    setUniformRowHeights(false);
    auto* group = new QTreeWidgetItem(this, {text});
    group->setFlags(Qt::ItemIsEnabled);
    group->setData(0, TableGroupRole, true);
    group->setData(0, TableGroupCollapsibleRole, options.collapsible);
    if (options.checkable) setGroupCheckState(group, Qt::Unchecked);
    group->setExpanded(true);
    return group;
}

void ProportionalTreeWidget::setGroupCheckState(QTreeWidgetItem* group, Qt::CheckState state, bool enabled)
{
    Q_ASSERT(group && group->treeWidget() == this && group->data(0, TableGroupRole).toBool());
    auto flags = group->flags();
    flags.setFlag(Qt::ItemIsUserCheckable, enabled);
    group->setFlags(flags);
    group->setCheckState(0, state);
}

QModelIndex ProportionalTreeWidget::indexAt(const QPoint& point) const
{
    const auto index = QTreeWidget::indexAt(point);
    if (index.isValid() || !viewport()->rect().contains(point)) return index;
    const auto row = QTreeWidget::indexAt(QPoint(0, point.y()));
    return row.siblingAtColumn(0).data(TableGroupRole).toBool() ? row : index;
}

QRect ProportionalTreeWidget::visualRect(const QModelIndex& index) const
{
    if (!index.siblingAtColumn(0).data(TableGroupRole).toBool()) return QTreeWidget::visualRect(index);
    QRect rect = QTreeWidget::visualRect(index.siblingAtColumn(header()->logicalIndexAt(0)));
    rect.setLeft(0);
    rect.setRight(viewport()->width() - 1);
    return rect;
}

ProportionalTreeWidget::GroupControl
ProportionalTreeWidget::groupControlAt(const QModelIndex& index, const QPoint& position) const
{
    if (!isEnabled() || !index.flags().testFlag(Qt::ItemIsEnabled)) return GroupControl::None;
    const QRect rect = visualRect(index);
    if (index.data(Qt::CheckStateRole).isValid() && index.flags().testFlag(Qt::ItemIsUserCheckable)
        && TableGroupHeader::checkBoxRect(rect, themeManager_, this).contains(position)) return GroupControl::CheckBox;
    if (index.data(TableGroupCollapsibleRole).toBool()
        && TableGroupHeader::chevronRect(rect, themeManager_).contains(position)) return GroupControl::Chevron;
    return GroupControl::None;
}

void ProportionalTreeWidget::requestGroupCheckState(QTreeWidgetItem* group)
{
    const auto state = group->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked;
    const QPersistentModelIndex index = indexFromItem(group);
    group->setCheckState(0, state);
    if (index.isValid()) emit groupCheckStateRequested(itemFromIndex(index), state);
}

void ProportionalTreeWidget::keyPressEvent(QKeyEvent* event)
{
    auto* group = currentItem();
    if (group && group->data(0, TableGroupRole).toBool()) {
        const bool enabled = isEnabled() && group->flags().testFlag(Qt::ItemIsEnabled);
        if (event->key() == Qt::Key_Space) {
            if (enabled && group->flags().testFlag(Qt::ItemIsUserCheckable)) requestGroupCheckState(group);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            if (enabled && group->data(0, TableGroupCollapsibleRole).toBool())
                group->setExpanded(event->key() == Qt::Key_Right);
            event->accept();
            return;
        }
    }
    QTreeWidget::keyPressEvent(event);
}

void ProportionalTreeWidget::setColumnLayoutKey(const QString& key)
{
    QStringList columns;
    for (int i = 0; i < columnCount(); ++i) columns.append(QString::number(i));
    columnLayout_.setKey(key, columns);
    applyColumnWidths();
}

void ProportionalTreeWidget::setColumnsFitToWidth(bool fit)
{
    columnsFitToWidth_ = fit;
    setHorizontalScrollBarPolicy(fit ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    applyColumnWidths();
}

void ProportionalTreeWidget::applyTheme()
{
    const auto& metrics = themeManager_->metrics().table;
    const auto font = FontManager::instance().getFont(themeManager_->snapshot(), FontRole::Body);
    setFont(font);
    const int iconExtent = themeManager_->metrics().objectIconExtent;
    setIconSize(QSize(iconExtent, iconExtent));
    header()->setFont(font);
    header()->setFixedHeight(std::max(metrics.headerHeight,
        QFontMetrics(font).height() + metrics.cellPaddingY * 2));
    header()->setMinimumSectionSize(0);
    doItemsLayout();
    applyColumnWidths();
}

bool ProportionalTreeWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == viewport() && (event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::MouseButtonDblClick || event->type() == QEvent::MouseButtonRelease)) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            const auto index = indexAt(mouse->position().toPoint()).siblingAtColumn(0);
            const bool group = index.data(TableGroupRole).toBool();
            if (event->type() == QEvent::MouseButtonRelease) {
                const auto pressed = pressedGroup_;
                const auto control = pressedGroupControl_;
                pressedGroup_ = QPersistentModelIndex();
                pressedGroupControl_ = GroupControl::None;
                if (pressed.isValid()) {
                    if (pressed == index && control != GroupControl::None
                        && control == groupControlAt(index, mouse->position().toPoint())) {
                        if (control == GroupControl::CheckBox) requestGroupCheckState(itemFromIndex(index));
                        else setExpanded(index, !isExpanded(index));
                    }
                    return true;
                }
            } else {
                pressedGroup_ = QPersistentModelIndex();
                pressedGroupControl_ = GroupControl::None;
                if (group) {
                    pressedGroup_ = index;
                    pressedGroupControl_ = groupControlAt(index, mouse->position().toPoint());
                    if (pressedGroupControl_ != GroupControl::None)
                        selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
                }
            }
            if (group && event->type() != QEvent::MouseButtonRelease) return true;
        }
    }
    if (watched == viewport() && event->type() == QEvent::Resize) applyColumnWidths();
    if (watched == header()->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        const int x = static_cast<QMouseEvent*>(event)->position().toPoint().x();
        const int radius = themeManager_->metrics().table.resizeHandleRadius;
        for (int i = 0; i < columnCount(); ++i) {
            if (isColumnHidden(i)) continue;
            const int edge = header()->sectionViewportPosition(i) + header()->sectionSize(i);
            if (std::abs(x - edge) <= radius) {
                columnLayout_.reset();
                applyColumnWidths();
                return true;
            }
        }
    }
    return QTreeWidget::eventFilter(watched, event);
}

void ProportionalTreeWidget::applyColumnWidths()
{
    if (applyingColumnWidths_ || viewport()->width() <= 0) return;
    QList<int> minimums(columnCount(), themeManager_->metrics().table.minimumColumnWidth);
    QList<bool> hidden;
    for (int i = 0; i < columnCount(); ++i) hidden.append(isColumnHidden(i));
    columnWidths_ = columnLayout_.widths(viewport()->width(), minimums, hidden, columnsFitToWidth_);
    const QScopedValueRollback guard(applyingColumnWidths_, true);
    for (int i = 0; i < columnCount(); ++i)
        if (!isColumnHidden(i)) setColumnWidth(i, columnWidths_[i]);
}

void ProportionalTreeWidget::rebalanceColumnWeight(int column, int newSize)
{
    if (columnWidths_.size() != columnCount()) return;
    QList<int> minimums(columnCount(), themeManager_->metrics().table.minimumColumnWidth);
    QList<int> order;
    for (int i = 0; i < columnCount(); ++i) order.append(header()->logicalIndex(i));
    columnLayout_.resizeColumn(column, newSize, columnWidths_, minimums, order);
    applyColumnWidths();
}

}
