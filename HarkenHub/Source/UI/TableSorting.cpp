#include "UI/TableSorting.h"

#include <QDateTime>
#include <QHeaderView>
#include <QVersionNumber>

namespace VNH {
namespace {

constexpr int kSortValueRole = Qt::UserRole + 100;

}

SortableTableItem::SortableTableItem(const QString& text)
    : SortableTableItem(text, text.isEmpty() ? QVariant() : QVariant(text))
{
}

SortableTableItem::SortableTableItem(const QString& text, const QVariant& sortValue)
    : QTableWidgetItem(text)
    , collator_(QLocale().language() == QLocale::C ? QLocale(QLocale::English) : QLocale())
{
    setData(kSortValueRole, sortValue);
    collator_.setCaseSensitivity(Qt::CaseInsensitive);
    collator_.setNumericMode(true);
}

bool SortableTableItem::operator<(const QTableWidgetItem& other) const
{
    const QVariant left = data(kSortValueRole);
    const QVariant right = other.data(kSortValueRole);
    if (left.isValid() != right.isValid())
        return tableWidget()->horizontalHeader()->sortIndicatorOrder() == Qt::AscendingOrder
            ? left.isValid() : right.isValid();
    if (!left.isValid())
        return false;
    if (left.metaType() == QMetaType::fromType<QDateTime>())
        return left.toDateTime() < right.toDateTime();
    if (left.metaType() == QMetaType::fromType<QVersionNumber>())
        return QVersionNumber::compare(left.value<QVersionNumber>(), right.value<QVersionNumber>()) < 0;
    return collator_.compare(left.toString(), right.toString()) < 0;
}

TableSorting::TableSorting(QTableWidget* table, const QString& settingsKey, int sortableColumnCount)
    : QObject(table)
    , table_(table)
    , settingsKey_(settingsKey)
    , sort_(HubSettings::tableSort(settingsKey))
{
    if (sort_.column < 0 || sort_.column >= sortableColumnCount)
        sort_ = {};
    auto* header = table->horizontalHeader();
    header->setSectionsClickable(true);
    header->setSortIndicatorClearable(false);
    header->setSortIndicatorShown(true);
    header->setSortIndicator(sort_.column, sort_.order);
    sortTimer_.setSingleShot(true);
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(250);
    connect(&sortTimer_, &QTimer::timeout, this, &TableSorting::apply);
    connect(&saveTimer_, &QTimer::timeout, this, &TableSorting::save);
    connect(header, &QHeaderView::sectionClicked, this,
        [this, header, sortableColumnCount](int column) {
            if (column < 0 || column >= sortableColumnCount) {
                header->setSortIndicator(sort_.column, sort_.order);
                return;
            }
            sort_.order = column == sort_.column && sort_.order == Qt::AscendingOrder
                ? Qt::DescendingOrder : Qt::AscendingOrder;
            sort_.column = column;
            header->setSortIndicator(sort_.column, sort_.order);
            changed_ = true;
            if (!refreshing_)
                sortTimer_.start();
        });
}

TableSorting::~TableSorting()
{
    save();
}

void TableSorting::beginRefresh()
{
    refreshing_ = true;
    sortTimer_.stop();
}

void TableSorting::endRefresh()
{
    refreshing_ = false;
    apply();
}

void TableSorting::apply()
{
    table_->sortItems(sort_.column, sort_.order);
    if (changed_)
        saveTimer_.start();
}

void TableSorting::save()
{
    if (changed_ && HubSettings::setTableSort(settingsKey_, sort_))
        changed_ = false;
}

}
