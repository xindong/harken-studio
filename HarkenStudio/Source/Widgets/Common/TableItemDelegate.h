#pragma once

#include <QStyledItemDelegate>

namespace VNX {

class ThemeManager;

enum TableDataRole {
    TableGroupRole = Qt::UserRole + 1024,
    TableTextRole,
    TableDropTargetRole,
    TableCursorRole
};

enum class TableTextState { Normal, Muted, Disabled, Error, Modified, Custom, Success, Warning };

class TableItemDelegate : public QStyledItemDelegate
{
public:
    explicit TableItemDelegate(QObject* parent = nullptr, ThemeManager* themeManager = nullptr);
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                           const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                               const QModelIndex& index) const override;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override;
    virtual void paintContents(QPainter* painter, const QStyleOptionViewItem& option,
                               const QModelIndex& index) const;

private:
    ThemeManager* themeManager_;
};

}
