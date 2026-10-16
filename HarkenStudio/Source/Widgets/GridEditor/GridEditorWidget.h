#pragma once

#include "Core/ProjectTypes.h"

#include <QUuid>
#include <QWidget>

#include <vector>

class QHBoxLayout;
class QLabel;
class QShowEvent;
class QStackedWidget;

namespace VNX {

class MixingEditorWidget;
class DiscreteGridWidget;
class SelectionListWidget;
struct ProjectItem;

class GridEditorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit GridEditorWidget(QWidget* parent = nullptr);

    static bool acceptsItem(const ProjectItem* item);
    static bool acceptsItemType(ProjectItem::Type type);

    bool isPinned() const { return pinned_; }
    void setPinned(bool pinned);

    QUuid currentItemGuid() const;
    QUuid displayedItemGuid() const;
    ProjectItem* boundItem() const { return boundItem_; }
    int drillDepth() const { return static_cast<int>(drillPath_.size()); }
    bool isShowingGrid() const;
    DiscreteGridWidget* discreteGrid() const { return discreteGrid_; }
    MixingEditorWidget* mixingEditor() const { return mixingEditor_; }
    SelectionListWidget* selectionList() const { return selectionList_; }
    QString statusText() const;

    void drillInto(const QUuid& target);
    void drillBackTo(int depth);

public slots:
    void bindItem(ProjectItem* item);
    void clearEditor();

signals:
    void pinToggled(bool pinned);
    void navigateToItem(uint32_t objectID);
    void locateItem(uint32_t objectID);
    void sourceEditorRequested(const QUuid& soundGuid, const QUuid& entryId);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refreshFromModel();
    void showEmptyState(const QString& message);
    void rebuildCrumbs(const std::vector<ProjectItem*>& chain);
    void refreshChrome();

    bool pinned_ = false;
    ProjectItem* boundItem_ = nullptr;
    ProjectItem* displayItem_ = nullptr;
    std::vector<QUuid> drillPath_;

    QWidget* crumbBar_ = nullptr;
    QWidget* crumbsHost_ = nullptr;
    QHBoxLayout* crumbLayout_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    QWidget* gridPage_ = nullptr;
    QStackedWidget* editorStack_ = nullptr;
    DiscreteGridWidget* discreteGrid_ = nullptr;
    MixingEditorWidget* mixingEditor_ = nullptr;
    SelectionListWidget* selectionList_ = nullptr;
};

}
