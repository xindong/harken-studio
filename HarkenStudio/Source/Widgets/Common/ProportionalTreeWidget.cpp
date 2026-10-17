#include "ProportionalTreeWidget.h"

#include "Core/QtControlSkin.h"
#include "Core/FontManager.h"
#include "Core/ThemeManager.h"
#include "ReorderHeaderView.h"
#include "TableGroupHeader.h"
#include "TableItemDelegate.h"

#include <QEvent>
#include <QMouseEvent>

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
    TableGroupHeader::paint(*painter, rect, first.data().toString(), themeManager_);
}

QTreeWidgetItem* ProportionalTreeWidget::createGroup(const QString& text)
{
    setUniformRowHeights(false);
    auto* group = new QTreeWidgetItem(this, {text});
    group->setFlags(Qt::ItemIsEnabled);
    group->setData(0, TableGroupRole, true);
    return group;
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
