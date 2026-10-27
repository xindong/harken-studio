
#pragma once

#include "Widgets/Shared/TreeExpansionWidget.h"

#include "Core/ContextInfo.h"
#include "Core/ProjectTypes.h"
#include "Widgets/Common/DropZoneWidget.h"

class QMouseEvent;

namespace VNX {

class ProjectTreePanel;

class ProjectTreeWidget : public TreeExpansionWidget, public AudioDropTarget, public IContextInfoSource
{
    Q_OBJECT

public:
    explicit ProjectTreeWidget(QWidget* parent = nullptr);

    void setPanel(ProjectTreePanel* panel) { panel_ = panel; }

    void setExternalDropHighlight(const QPoint& localPos, bool hasAudio);
    void clearExternalDropHighlight();
    ProjectItem* projectItemAt(const QPoint& localPos) const;

    ContextTopic contextTopicAt(const QPoint& pos) const override;

    bool canDropSelectionOn(ProjectItem* target) const;
    bool canDropSelectionAtBucketRoot(const QString& bucketKey) const;
    bool canDropSelectionIntoFolderRow(QTreeWidgetItem* row) const;
    void selectAll() override;

signals:
    void internalMoveRequested(ProjectItem* item,
                               ProjectItem* newParent,
                               const QString& bucketKey,
                               const QString& folderPath,
                               bool folderAddressValid);
    void externalFilesDropped(const QStringList& filePaths, QTreeWidgetItem* targetItem, const QString& bucketKey);

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void startDrag(Qt::DropActions actions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    void drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override;
    void drawBranches(QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override;

private:
    void clearDropHighlight();
    QList<ProjectItem*> draggedItems() const;
    QList<PhysicalFolder*> draggedFolders() const;

    ProjectTreePanel* panel_ = nullptr;
    QList<uint32_t> draggedObjectIDs_;
    QList<PhysicalFolder*> draggedFolders_;
    bool typedDrag_ = false;
    QTreeWidgetItem* highlightedItem_ = nullptr;
    QList<QTreeWidgetItem*> mousePressSelection_;
    QTreeWidgetItem* mousePressCurrent_ = nullptr;
    bool mouseReleaseHandled_ = false;
};

}
