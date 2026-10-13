
#include "TreeRowStyle.h"
#include "InlineRenameEdit.h"
#include "RenameClickHandler.h"

#include "Core/ProjectManager.h"
#include "Core/ThemeManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Widgets/Common/LayoutDebugTint.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QIcon>
#include <QModelIndex>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTreeWidgetItem>
#include <QTreeWidget>

namespace VNX::TreeRowStyle {

namespace {

constexpr int kIconTextGapPx = 4;

// Painter-side tint for delegate-drawn tree rows, where the stylesheet-based
// appliers never reach.
void dbgFill(QPainter* p, const QRect& r, DebugTint::Role role, int alpha)
{
    if (!DebugTint::kEnabled)
        return;
    p->fillRect(r, DebugTint::color(role, alpha));
}

QIcon chevronIcon(bool expanded, const QColor& color)
{
    return ThemeManager::instance().resolveIcon(
        QString::fromLatin1(expanded ? ThemeAssetIds::NavigationChevronDown
                                     : ThemeAssetIds::NavigationChevronRight),
        ThemeAssetVariant{QSize(), 1.0, 0, color}).icon;
}

void paintBucketCaret(QPainter* p, const QRect& box, bool expanded)
{
    const int chevronExtent = ThemeManager::instance().metrics().navigationChevronExtent;
    const QRect iconRect(box.left() + (box.width() - chevronExtent) / 2,
                         box.top() + (box.height() - chevronExtent) / 2,
                         chevronExtent, chevronExtent);
    chevronIcon(expanded, ThemeManager::instance().colors().textSecondary)
        .paint(p, iconRect);
}

}

QTreeWidgetItem* makeFolderRow(const QString& text)
{
    auto* row = new QTreeWidgetItem();
    row->setText(0, text);
    row->setIcon(0, ThemeManager::instance().resolveToggleIcon(
        QString::fromLatin1(ThemeAssetIds::TreeFolderOpen),
        QString::fromLatin1(ThemeAssetIds::TreeFolderClosed)).icon);
    row->setData(0, kBucketRole, true);
    return row;
}

QTreeWidgetItem* makeBucketRow(const QString& folderKey)
{
    return makeFolderRow(ProjectManager::bucketDisplayName(folderKey));
}

void paintRowAccent(QPainter* p, const QRect& rowRect)
{
    p->fillRect(QRect(rowRect.left(), rowRect.top(), 2, rowRect.height()),
                ThemeManager::instance().colors().accent);
}

void paintBucketRow(QPainter* p, const QStyleOptionViewItem& opt,
                    const QModelIndex& idx, bool expanded)
{
    const QRect r = opt.rect;
    const bool hover    = opt.state & QStyle::State_MouseOver;
    const bool selected = opt.state & QStyle::State_Selected;

    const auto& tc = ThemeManager::instance().colors();
    p->save();
    p->fillRect(r, hover ? tc.bgHover : tc.headerBackground);
    p->setPen(tc.bgPrimary);
    p->drawLine(r.left(), r.bottom(), r.right(), r.bottom());
    if (selected)
        p->fillRect(r, tc.bgSelected);
    p->restore();
    if (selected)
        paintRowAccent(p, r);

    const int caretW = ThemeManager::instance().metrics().navigationChevronExtent;
    const QRect caretBox(r.right() - caretW, r.top(), caretW, r.height());
    const int iconExtent = ThemeManager::instance().metrics().objectIconExtent;
    const QRect iconCell(r.left(), r.top(), iconExtent, r.height());
    const QRect iconRect(iconCell.left(), iconCell.top() + (iconCell.height() - iconExtent) / 2,
                         iconExtent, iconExtent);
    const int textX = iconCell.right() + 1;
    const QRect textRect(textX, r.top(), r.right() - caretW - 2 - textX, r.height());
    const QRect textInner = textRect.adjusted(kIconTextGapPx, 0, 0, 0);

    dbgFill(p, r, DebugTint::Role::Head, 70);
    dbgFill(p, textRect, DebugTint::Role::Flex, 90);
    dbgFill(p, iconCell, DebugTint::Role::Label, 130);
    dbgFill(p, caretBox, DebugTint::Role::ColToggle, 130);

    paintBucketCaret(p, caretBox, expanded);

    const QIcon icon = qvariant_cast<QIcon>(idx.data(Qt::DecorationRole));
    if (!icon.isNull())
        icon.paint(p, iconRect, Qt::AlignCenter, QIcon::Normal,
                   expanded ? QIcon::On : QIcon::Off);
    p->save();
    p->setFont(opt.font);
    p->setPen(tc.textPrimary);
    const QString txt = idx.data(Qt::DisplayRole).toString();
    p->drawText(textInner, Qt::AlignVCenter | Qt::AlignLeft,
                opt.fontMetrics.elidedText(txt, Qt::ElideRight, textInner.width()));
    p->restore();
}

void ItemRowDelegate::paint(QPainter* p, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    QStyle* st = opt.widget ? opt.widget->style() : QApplication::style();

    QStyleOptionViewItem bg = opt;
    bg.text.clear();
    bg.icon = QIcon();
    bg.features &= ~(QStyleOptionViewItem::HasDecoration | QStyleOptionViewItem::HasDisplay);
    st->drawControl(QStyle::CE_ItemViewItem, &bg, p, opt.widget);

    const QRect r = opt.rect;
    const QSize ds = opt.decorationSize;
    const QRect iconCell(r.left(), r.top(), ds.width(), r.height());
    const QRect textCell(iconCell.right() + 1, r.top(),
                         r.right() - iconCell.right(), r.height());
    const QRect textInner = textCell.adjusted(kIconTextGapPx, 0, -kIconTextGapPx, 0);

    dbgFill(p, textCell, DebugTint::Role::Flex, 90);
    dbgFill(p, iconCell, DebugTint::Role::Label, 130);

    if (!opt.icon.isNull()) {
        const QRect iconRect(iconCell.left(),
                             iconCell.top() + (iconCell.height() - ds.height()) / 2,
                             ds.width(), ds.height());
        const QIcon::Mode mode = !(opt.state & QStyle::State_Enabled) ? QIcon::Disabled
                               : (opt.state & QStyle::State_Selected) ? QIcon::Selected
                                                                      : QIcon::Normal;
        opt.icon.paint(p, iconRect, Qt::AlignCenter, mode,
                       (opt.state & QStyle::State_Open) ? QIcon::On : QIcon::Off);
    }

    p->save();
    p->setFont(opt.font);
    if (!(opt.state & QStyle::State_Enabled))
        p->setPen(ThemeManager::instance().colors().textDisabled);
    else
        p->setPen(opt.palette.color((opt.state & QStyle::State_Selected)
                                        ? QPalette::HighlightedText
                                        : QPalette::Text));
    p->drawText(textInner, Qt::AlignVCenter | Qt::AlignLeft,
                opt.fontMetrics.elidedText(opt.text, opt.textElideMode, textInner.width()));
    p->restore();
}

void ItemRowDelegate::setRenameEditorFactory(RenameEditorFactory factory)
{
    renameEditorFactory_ = std::move(factory);
    auto* tree = qobject_cast<QTreeWidget*>(parent());
    new RenameClickHandler(tree, tree->viewport(), [this, tree](const QPoint& point)
        -> RenameClickHandler::Target {
        const auto index = tree->indexAt(point);
        if (!index.isValid() || !(index.flags() & Qt::ItemIsEditable)
            || tree->selectedItems().size() != 1 || !tree->selectionModel()->isSelected(index))
            return {};
        for (auto* editor : tree->findChildren<InlineRenameEdit*>())
            if (!editor->isHidden()) return {};
        QStyleOptionViewItem option;
        option.initFrom(tree);
        option.rect = tree->visualRect(index);
        option.decorationSize = tree->iconSize();
        initStyleOption(&option, index);
        QRect label = option.rect.adjusted(option.decorationSize.width() + kIconTextGapPx,
                                           0, -kIconTextGapPx, 0);
        label.setWidth(qMin(label.width(), option.fontMetrics.horizontalAdvance(option.text)));
        if (!label.contains(point)) return {};
        const QPersistentModelIndex persistent(index);
        return {QVariant::fromValue(persistent), [tree, persistent] {
            tree->editItem(tree->itemFromIndex(persistent), persistent.column());
        }};
    }, this);
}

QWidget* ItemRowDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                      const QModelIndex& index) const
{
    if (!renameEditorFactory_)
        return QStyledItemDelegate::createEditor(parent, option, index);
    auto* editor = renameEditorFactory_(parent, index);
    if (editor) {
        auto* self = const_cast<ItemRowDelegate*>(this);
        connect(editor, &InlineRenameEdit::finished, self, [self, editor] {
            emit self->closeEditor(editor, QAbstractItemDelegate::RevertModelCache);
        });
    }
    return editor;
}

void ItemRowDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
    if (!qobject_cast<InlineRenameEdit*>(editor))
        QStyledItemDelegate::setEditorData(editor, index);
}

void ItemRowDelegate::setModelData(QWidget* editor, QAbstractItemModel* model,
                                  const QModelIndex& index) const
{
    if (!qobject_cast<InlineRenameEdit*>(editor))
        QStyledItemDelegate::setModelData(editor, model, index);
}

void ItemRowDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                          const QModelIndex& index) const
{
    if (!qobject_cast<InlineRenameEdit*>(editor)) {
        QStyledItemDelegate::updateEditorGeometry(editor, option, index);
        return;
    }
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    editor->setGeometry(opt.rect.adjusted(opt.decorationSize.width() + kIconTextGapPx, 0, 0, 0));
}

bool ItemRowDelegate::eventFilter(QObject* watched, QEvent* event)
{
    return qobject_cast<InlineRenameEdit*>(watched)
        ? false : QStyledItemDelegate::eventFilter(watched, event);
}

void paintBranches(QPainter* painter, const QRect& rect, const QModelIndex& index,
                   const QAbstractItemModel* model, int indentation, bool expanded,
                   bool rootIsDecorated)
{
    if (indentation <= 0 || rect.width() <= 0)
        return;

    const auto& tc = ThemeManager::instance().colors();
    const QColor guideColor = tc.borderSecondary;
    const QColor chevronColor = tc.textSecondary;

    int depth = rootIsDecorated ? 1 : 0;
    for (QModelIndex p = index.parent(); p.isValid(); p = p.parent())
        ++depth;

    // One cell per indent column: self/chevron column pink, ancestors alternating.
    if (DebugTint::kEnabled)
    {
        for (int col = 0; col < depth; ++col)
        {
            const QRect cell(rect.left() + col * indentation, rect.top(),
                             indentation, rect.height());
            const auto role = (col == depth - 1) ? DebugTint::Role::ColToggle
                            : (col % 2 ? DebugTint::Role::Gutter
                                       : DebugTint::Role::Section);
            dbgFill(painter, cell, role, 90);
        }
    }

    const int yTop  = rect.top();
    const int yBot  = rect.bottom();
    const int yMid  = rect.top() + rect.height() / 2;

    const auto colX = [&](int col) {
        return rect.left() + col * indentation + indentation / 2;
    };
    const auto hasNextSibling = [&](const QModelIndex& idx) {
        return idx.row() + 1 < model->rowCount(idx.parent());
    };

    const bool hasKids = model->hasChildren(index);
    const int xSelf = colX(depth - 1);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setPen(QPen(guideColor, 1));

    QModelIndex ancestor = index.parent();
    for (int col = depth - 3; col >= 0; --col) {
        if (hasNextSibling(ancestor))
            painter->drawLine(colX(col), yTop, colX(col), yBot);
        ancestor = ancestor.parent();
    }

    if (depth >= 2) {
        const int xJoint = colX(depth - 2);
        painter->drawLine(xJoint, yTop, xJoint, hasNextSibling(index) ? yBot : yMid);
        painter->drawLine(xJoint, yMid, hasKids ? xSelf - 7 : rect.right(), yMid);
    }

    if (hasKids) {
        const int chevronExtent = ThemeManager::instance().metrics().navigationChevronExtent;
        const QRect iconRect(xSelf - chevronExtent / 2, yMid - chevronExtent / 2,
                             chevronExtent, chevronExtent);
        chevronIcon(expanded, chevronColor).paint(painter, iconRect);
    }

    painter->restore();
}

}
