#pragma once

#include <QTreeWidget>

#include "TableColumnLayout.h"

namespace VNX {

class ThemeManager;

class ProportionalTreeWidget : public QTreeWidget
{
    Q_OBJECT

public:
    explicit ProportionalTreeWidget(const QList<int>& columnWeights, QWidget* parent = nullptr,
                                    ThemeManager* themeManager = nullptr);
    void setColumnsHidden(const QList<int>& columns, bool hidden);
    void setColumnLayoutKey(const QString& key);
    void setColumnsFitToWidth(bool fit);
    QTreeWidgetItem* createGroup(const QString& text);

protected:
    void drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyColumnWidths();
    void rebalanceColumnWeight(int column, int newSize);
    void applyTheme();

    TableColumnLayout columnLayout_;
    QList<int> columnWidths_;
    ThemeManager* themeManager_;
    bool applyingColumnWidths_ = false;
    bool columnsFitToWidth_ = true;
};

}
