#include "TableItemDelegate.h"
#include "TableGroupHeader.h"

#include "Core/ThemeManager.h"
#include "Core/QtControlSkin.h"

#include <QPainter>
#include <QTableView>

#include <algorithm>

namespace VNX {

TableItemDelegate::TableItemDelegate(QObject* parent, ThemeManager* themeManager)
    : QStyledItemDelegate(parent),
      themeManager_(themeManager ? themeManager : &ThemeManager::instance())
{
}

QWidget* TableItemDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                         const QModelIndex& index) const
{
    auto* editor = QStyledItemDelegate::createEditor(parent, option, index);
    if (editor) QtControlSkin::setSemanticProperty(editor, "tableEditor", true);
    return editor;
}

void TableItemDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                             const QModelIndex&) const
{
    if (option.widget && option.widget->property("compositeTable").toBool()) {
        editor->setGeometry(option.rect);
        return;
    }
    const auto& metrics = themeManager_->metrics().table;
    QRect rect = option.rect.adjusted(metrics.cellPaddingX, metrics.cellPaddingY,
                                     -metrics.cellPaddingX, -metrics.cellPaddingY);
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
    option->state &= ~QStyle::State_MouseOver;
    const auto& colors = themeManager_->colors();
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
}

void TableItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
    QStyleOptionViewItem styled = option;
    styled.state &= ~QStyle::State_MouseOver;
    const auto numberColumn = option.widget ? option.widget->property("rowNumberColumn") : QVariant();
    const bool composite = option.widget && option.widget->property("compositeTable").toBool();
    const bool plain = numberColumn.isValid() ? index.column() != numberColumn.toInt() : composite;
    if (composite || plain) {
        const auto background = index.data(Qt::BackgroundRole).value<QBrush>();
        painter->fillRect(option.rect, background.style() == Qt::NoBrush
            ? QBrush(themeManager_->colors().bgPrimary) : background);
    }
    if (plain) styled.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus);
    paintContents(painter, styled, index);
    const auto& colors = themeManager_->colors();
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
