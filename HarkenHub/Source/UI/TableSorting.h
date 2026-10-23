#pragma once

#include "Settings/HubSettings.h"

#include <QCollator>
#include <QTableWidgetItem>
#include <QTimer>

namespace VNH {

class SortableTableItem final : public QTableWidgetItem
{
public:
    explicit SortableTableItem(const QString& text);
    SortableTableItem(const QString& text, const QVariant& sortValue);
    bool operator<(const QTableWidgetItem& other) const override;

private:
    QCollator collator_;
};

class TableSorting final : public QObject
{
public:
    TableSorting(QTableWidget* table, const QString& settingsKey, int sortableColumnCount);
    ~TableSorting() override;
    void beginRefresh();
    void endRefresh();

private:
    void apply();
    void save();

    QTableWidget* table_;
    QString settingsKey_;
    TableSortPreference sort_;
    QTimer sortTimer_;
    QTimer saveTimer_;
    bool refreshing_ = false;
    bool changed_ = false;
};

}
