#include "TableItemDelegate.h"
#include "TableGroupHeader.h"
#include "ProportionalTreeWidget.h"

#include "Core/ThemeManager.h"
#include "Core/QtControlSkin.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTableView>

#include <algorithm>

namespace VNX {

TableItemDelegate::TableItemDelegate(QObject* parent, ThemeManager* themeManager)
    : QStyledItemDelegate(parent),
      themeManager_(themeManager ? themeManager : &ThemeManager::instance())
{
    if (auto* view = qobject_cast<QAbstractItemView*>(parent)) {
        view->setMouseTracking(true);
        view->viewport()->installEventFilter(this);
    }
}

bool TableItemDelegate::eventFilter(QObject* watched, QEvent* event)
{
    if (auto* view = qobject_cast<QAbstractItemView*>(parent()); view && watched == view->viewport()) {
        if (view->itemDelegate() == this
            && (event->type() == QEvent::MouseMove || event->type() == QEvent::Leave)) {
            const auto hoverIndex = [&](const QPoint& position) {
                const auto index = view->indexAt(position);
                const auto first = index.siblingAtColumn(0);
                return first.data(TableGroupRole).toBool() ? first : index;
            };
            const auto previous = hoverIndex(hoverPosition_);
            hoverPosition_ = event->type() == QEvent::MouseMove
                ? static_cast<QMouseEvent*>(event)->position().toPoint() : QPoint(-1, -1);
            const auto current = hoverIndex(hoverPosition_);
            if (previous.data(Qt::CheckStateRole).isValid()) view->viewport()->update(view->visualRect(previous));
            if (current != previous && current.data(Qt::CheckStateRole).isValid())
                view->viewport()->update(view->visualRect(current));
        }
        return false;
    }
    return QStyledItemDelegate::eventFilter(watched, event);
}

QWidget* TableItemDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                         const QModelIndex& index) const
{
    auto* editor = QStyledItemDelegate::createEditor(parent, option, index);
    if (editor) QtControlSkin::setSemanticProperty(editor, "tableEditor", true);
    return editor;
}

void TableItemDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                             const QModelIndex& index) const
{
    if (option.widget && option.widget->property("compositeTable").toBool()) {
        editor->setGeometry(option.rect);
        return;
    }
    const auto& metrics = themeManager_->metrics().table;
    const auto* tree = qobject_cast<const ProportionalTreeWidget*>(option.widget);
    QRect rect = tree ? tree->cellContentsRect(option.rect, index) : option.rect;
    rect.adjust(metrics.cellPaddingX, metrics.cellPaddingY, -metrics.cellPaddingX, -metrics.cellPaddingY);
    rect.setWidth(std::max(0, rect.width()));
    rect.setHeight(std::max(0, rect.height()));
    const int height = std::min(rect.height(), QtControlSkin(themeManager_->snapshot()).fieldHeight());
    rect.setTop(rect.top() + (rect.height() - height) / 2);
    rect.setHeight(height);
    editor->setGeometry(rect);
}

QSize TableItemDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    if (index.siblingAtColumn(0).data(TableGroupRole).toBool()) {
        size.setHeight(TableGroupHeader::preferredHeight(themeManager_));
        return size;
    }
    const auto& metrics = themeManager_->metrics().table;
    size.setHeight(std::max(metrics.rowHeight,
        std::max(size.height(), QtControlSkin(themeManager_->snapshot()).fieldHeight())
            + metrics.cellPaddingY * 2));
    return size;
}

void TableItemDelegate::initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    if (const auto* tree = qobject_cast<const ProportionalTreeWidget*>(option->widget)) {
        option->rect = tree->cellContentsRect(option->rect, index);
        if (tree->rowProgressEndColumn() >= 0) option->backgroundBrush = Qt::NoBrush;
    }
    const auto& colors = themeManager_->colors();
    if (index.model()->headerData(index.column(), Qt::Horizontal, TableColumnAccentRole).toBool()
        && !index.siblingAtColumn(0).data(TableGroupRole).toBool())
        option->features |= QStyleOptionViewItem::Alternate;
    QColor text = colors.textPrimary;
    switch (static_cast<TableTextState>(index.data(TableTextRole).toInt())) {
    case TableTextState::Muted: text = colors.textMuted; break;
    case TableTextState::Disabled: text = colors.textDisabled; break;
    case TableTextState::Error: text = colors.error; break;
    case TableTextState::Modified: text = colors.accent; break;
    case TableTextState::Custom: text = colors.info; break;
    case TableTextState::Success: text = colors.success; break;
    case TableTextState::Warning: text = colors.warning; break;
    case TableTextState::Normal: break;
    }
    option->palette.setColor(QPalette::Text, text);
    option->palette.setColor(QPalette::HighlightedText, text);
    option->font.setBold(index.data(TableTextRole).toInt() == static_cast<int>(TableTextState::Modified));
    if (option->widget && option->features.testFlag(QStyleOptionViewItem::HasCheckIndicator)) {
        const auto rect = option->widget->style()->subElementRect(
            QStyle::SE_ItemViewItemCheckIndicator, option, option->widget);
        option->state.setFlag(QStyle::State_MouseOver, isCheckIndicatorHovered(rect));
    }
}

void TableItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
    QStyleOptionViewItem styled = option;
    const auto numberColumn = option.widget ? option.widget->property("rowNumberColumn") : QVariant();
    const bool composite = option.widget && option.widget->property("compositeTable").toBool();
    const bool plain = numberColumn.isValid() ? index.column() != numberColumn.toInt() : composite;
    if (composite || plain) {
        const auto background = index.data(Qt::BackgroundRole).value<QBrush>();
        painter->fillRect(option.rect, background.style() == Qt::NoBrush
            ? QBrush(themeManager_->colors().bgPrimary) : background);
    }
    if (plain) styled.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus);
    const auto* tree = qobject_cast<const ProportionalTreeWidget*>(option.widget);
    const auto& colors = themeManager_->colors();
    if (tree && tree->rowProgressEndColumn() >= 0) {
        painter->fillRect(option.rect, colors.bgPrimary);
        if (styled.state.testFlag(QStyle::State_Selected)) {
            painter->fillRect(option.rect, colors.bgSelected);
        } else {
            const auto background = index.data(Qt::BackgroundRole).value<QBrush>();
            if (background.style() != Qt::NoBrush)
                painter->fillRect(option.rect, background);
            else if (index.model()->headerData(index.column(), Qt::Horizontal, TableColumnAccentRole).toBool())
                painter->fillRect(option.rect, colors.bgSecondary);
        }
        const auto row = index.siblingAtColumn(0);
        const auto progress = row.data(TableRowProgressRole);
        if (progress.isValid() && !row.data(TableGroupRole).toBool()) {
            const auto state = static_cast<TableRowProgressState>(row.data(TableRowProgressStateRole).toInt());
            QColor fill = state == TableRowProgressState::Failed ? colors.error
                : state == TableRowProgressState::Canceled ? colors.textMuted : colors.accent;
            fill.setAlpha(state == TableRowProgressState::Complete ? 28 : 40);
            painter->fillRect(tree->rowProgressRect(option.rect, progress.toInt()).intersected(option.rect), fill);
        }
    } else if (tree && tree->cellContentsRect(option.rect, index) != option.rect) {
        QStyleOptionViewItem background = styled;
        initStyleOption(&background, index);
        background.rect = option.rect;
        tree->style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &background, painter, tree);
    }
    paintContents(painter, styled, index);
    painter->save();
    painter->setPen(colors.borderPrimary);
    const auto* table = qobject_cast<const QTableView*>(option.widget);
    if (!table || !table->showGrid())
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
    if (styled.state.testFlag(QStyle::State_Selected) && option.rect.left() == 0)
        painter->fillRect(QRect(option.rect.left(), option.rect.top(), 2, option.rect.height()), colors.accent);
    if (index.siblingAtColumn(0).data(TableDropTargetRole).toBool()) {
        painter->setPen(colors.accent);
        painter->drawLine(option.rect.topLeft(), option.rect.topRight());
        painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
    }
    painter->restore();
}

void TableItemDelegate::paintContents(QPainter* painter, const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const
{
    QStyledItemDelegate::paint(painter, option, index);
}

}
