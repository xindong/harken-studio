#pragma once

#include "TableWidget.h"

class QTableWidget;

namespace VNX {

class MatrixHeaderView;

class MatrixTableWidget : public TableWidget
{
    Q_OBJECT

public:
    explicit MatrixTableWidget(QWidget* parent = nullptr, ThemeManager* themeManager = nullptr);

    QTableWidget* table() const { return table_; }
    void setHeaderWidget(int column, QWidget* widget);
    QWidget* headerWidget(int column) const;
    void clearHeaderWidgets();
    void setHeaderWarning(int column, bool warning);
    void setColumnsReorderable(bool enabled, int firstMovableColumn = 0);
    int selectedColumn() const { return selectedColumn_; }
    void setSelectedColumn(int column);
    void clearSelection() override;

signals:
    void columnMoveRequested(int from, int to);
    void columnSelectionChanged(int column);

private:
    void applyTheme() override;
    bool hasSelection() const override;

    ThemeManager* themeManager_;
    QTableWidget* table_ = nullptr;
    MatrixHeaderView* header_ = nullptr;
    int firstMovableColumn_ = 0;
    int selectedColumn_ = -1;
};

}
