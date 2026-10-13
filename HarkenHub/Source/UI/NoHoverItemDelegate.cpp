#include "UI/NoHoverItemDelegate.h"

#include <QStyle>

namespace VNH {

void NoHoverItemDelegate::paint(QPainter* painter,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
    QStyleOptionViewItem adjusted = option;
    adjusted.state &= ~QStyle::State_MouseOver;
    QStyledItemDelegate::paint(painter, adjusted, index);
}

}
