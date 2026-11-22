
#pragma once

#include "Widgets/Shared/TreeExpansionWidget.h"
#include <QPersistentModelIndex>
#include <Qt>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QMimeData;

namespace harken::studio {

constexpr int kResourceIdRole = Qt::UserRole + 32;
constexpr int kResourcePathRole = Qt::UserRole + 33;
constexpr int kResourceFolderRole = Qt::UserRole + 34;

class ResourcesTreeWidget : public TreeExpansionWidget
{
    Q_OBJECT

public:
    explicit ResourcesTreeWidget(QWidget* parent = nullptr);

    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    bool importDroppedAudio(const QMimeData* mimeData, const QString& folder = {});
    bool moveDroppedResources(const QMimeData* mimeData, const QString& folder);
    QString currentFolder() const;
    void showRootContextMenu(const QPoint& globalPosition);
    void setRootDropTarget(QWidget* target);

public slots:
    void newFolder();

signals:
    void contentRevealRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void startDrag(Qt::DropActions actions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const override;
    void drawRow(QPainter* painter, const QStyleOptionViewItem& options, const QModelIndex& index) const override;

private:
    QString folderForItem(QTreeWidgetItem* item) const;
    QString renameResource(const QString& path, const QString& name);
    void renameCurrent();
    void deleteCurrentFolder();
    void showContextMenu(QTreeWidgetItem* item, const QPoint& globalPosition);
    void showError(const QString& message);
    void selectPath(const QString& path);
    bool acceptsMime(const QMimeData* mime) const;
    Qt::DropAction dropActionFor(const QMimeData* mime, const QString& folder) const;
    void performDrop(QDropEvent* event, const QString& folder);
    void clearDropHighlight();
    QPersistentModelIndex dropFolder_;
    QWidget* rootDropTarget_ = nullptr;
};

}
