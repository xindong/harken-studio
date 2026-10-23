#pragma once

#include <QTreeWidget>

#include "TableColumnLayout.h"
#include "TableGroupHeader.h"

#include <optional>

namespace VNX {

class ThemeManager;

class ProportionalTreeWidget : public QTreeWidget
{
    Q_OBJECT

public:
    explicit ProportionalTreeWidget(const QList<int>& columnWeights, QWidget* parent = nullptr,
                                    ThemeManager* themeManager = nullptr);
    void setColumnsHidden(const QList<int>& columns, bool hidden);
    void setPinnedTrailingColumns(const QList<int>& columns);
    void setAccentedColumns(const QList<int>& columns);
    void setColumnSeparatorBefore(int column);
    void setRowProgressEndColumn(int column);
    int rowProgressEndColumn() const { return progressEndColumn_; }
    QRect rowProgressRect(const QRect& rowRect, int percent) const;
    void setColumnLayoutKey(const QString& key);
    void setColumnsFitToWidth(bool fit);
    void setHierarchyIndentation(int indentation);
    QRect cellContentsRect(const QRect& rect, const QModelIndex& index) const;
    QTreeWidgetItem* createGroup(const QString& text, const TableGroupOptions& options = {});
    void setGroupCheckState(QTreeWidgetItem* group, Qt::CheckState state, bool enabled = true);
    QModelIndex indexAt(const QPoint& point) const override;
    QRect visualRect(const QModelIndex& index) const override;

signals:
    void groupCheckStateRequested(QTreeWidgetItem* group, Qt::CheckState state);

protected:
    void paintEvent(QPaintEvent* event) override;
    void drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class GroupControl { None, CheckBox, Chevron };

    void applyColumnWidths();
    void rebalanceColumnWeight(int column, int newSize);
    void applyTheme();
    GroupControl groupControlAt(const QModelIndex& index, const QPoint& position) const;
    void requestGroupCheckState(QTreeWidgetItem* group);

    TableColumnLayout columnLayout_;
    QList<int> columnWidths_;
    ThemeManager* themeManager_;
    std::optional<int> hierarchyIndentation_;
    bool applyingColumnWidths_ = false;
    bool columnsFitToWidth_ = true;
    QPersistentModelIndex pressedGroup_;
    GroupControl pressedGroupControl_ = GroupControl::None;
    int separatorColumn_ = -1;
    int progressEndColumn_ = -1;
};

}
