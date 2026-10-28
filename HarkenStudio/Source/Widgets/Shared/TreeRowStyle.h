
#pragma once

#include <QStyledItemDelegate>
#include <Qt>
#include <functional>

class QAbstractItemModel;
class QPainter;
class QRect;
class QString;
class QTreeWidgetItem;

namespace VNX { class InlineRenameEdit; }

namespace VNX::TreeRowStyle {

constexpr int kBucketRole = Qt::UserRole + 1;

QTreeWidgetItem* makeFolderRow(const QString& text);
QTreeWidgetItem* makeBucketRow(const QString& folderKey);

void paintRowAccent(QPainter* p, const QRect& rowRect);
void paintItemContents(QPainter* p, const QStyleOptionViewItem& option);

void paintBucketRow(QPainter* p, const QStyleOptionViewItem& opt,
                    const QModelIndex& idx, bool expanded);

void paintBranches(QPainter* painter, const QRect& rect, const QModelIndex& index,
                   const QAbstractItemModel* model, int indentation, bool expanded,
                   bool rootIsDecorated = false);

// Owns the item-row icon/text layout so the icon→title gap is an explicit
// constant shared with paintBucketRow — the default CE_ItemViewItem layout
// injects a style-driven margin that QSS cannot reach. Backgrounds still go
// through the style, so QSS hover/selected rules keep applying.
class ItemRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    using RenameEditorFactory = std::function<InlineRenameEdit*(QWidget*, const QModelIndex&)>;
    void setRenameEditorFactory(RenameEditorFactory factory);

    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                           const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor, QAbstractItemModel* model,
                       const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                               const QModelIndex& index) const override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    RenameEditorFactory renameEditorFactory_;
};

}
