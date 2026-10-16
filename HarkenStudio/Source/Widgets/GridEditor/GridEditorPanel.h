#pragma once

#include "GridEditorWidget.h"

#include <QUuid>
#include <QWidget>

class QEvent;
class QFocusEvent;
class QObject;

namespace VNX {

class MixingEditorWidget;
class DiscreteGridWidget;
struct ProjectItem;

class GridEditorPanel : public QWidget
{
    Q_OBJECT

public:
    explicit GridEditorPanel(QWidget* parent = nullptr);

    static bool acceptsItem(const ProjectItem* item);

    int panelId() const { return panelId_; }
    void setPanelId(int id) { panelId_ = id; }
    bool isPinned() const { return editor_->isPinned(); }
    void setPinned(bool pinned);

    QUuid currentItemGuid() const { return editor_->currentItemGuid(); }
    QUuid displayedItemGuid() const { return editor_->displayedItemGuid(); }
    ProjectItem* boundItem() const { return editor_->boundItem(); }
    int drillDepth() const { return editor_->drillDepth(); }
    bool isShowingGrid() const { return editor_->isShowingGrid(); }
    DiscreteGridWidget* discreteGrid() const { return editor_->discreteGrid(); }
    MixingEditorWidget* mixingEditor() const { return editor_->mixingEditor(); }
    QString statusText() const { return editor_->statusText(); }

    void drillInto(const QUuid& target) { editor_->drillInto(target); }
    void drillBackTo(int depth) { editor_->drillBackTo(depth); }

public slots:
    void bindItem(ProjectItem* item);
    void clearEditor();

signals:
    void panelFocused(int panelId);
    void pinToggled(bool pinned);
    void navigateToItem(uint32_t objectID);
    void locateItem(uint32_t objectID);
    void sourceEditorRequested(const QUuid& soundGuid, const QUuid& entryId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

private:
    void installFocusFilterRecursively(QObject* root);

    int panelId_ = 0;
    GridEditorWidget* editor_ = nullptr;
};

}
