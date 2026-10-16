#pragma once

#include "Core/GridSemantics.h"

#include <QWidget>

#include <optional>
#include <utility>
#include <vector>

class QAction;
class QTableWidget;
class QToolButton;

namespace VNX {

struct ProjectItem;
class ObjectRefWidget;
class MatrixTableWidget;

class DiscreteGridWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DiscreteGridWidget(QWidget* parent = nullptr);

    void setBoundItem(ProjectItem* item);
    ProjectItem* boundItem() const { return boundItem_; }

    void reload();

    bool commitCellEdit(int row, int axis, std::optional<int> cell);
    bool commitAddAxis(const QUuid& variableId);
    bool commitRemoveAxis(int axis);
    bool commitReplaceAxis(int axis, const QUuid& variableId);
    bool commitAddRow(const QUuid& target);
    bool commitRemoveRow(int row);
    bool commitMoveRow(int from, int to);
    bool commitReplaceRow(int row, const QUuid& target);

    int winnerRow() const { return winner_; }
    QTableWidget* table() const { return table_; }

    void openAddAxisSelector(QWidget* anchor = nullptr);
    void openAddRowSelector();

signals:
    void derivedRefreshed();
    void navigateToItem(uint32_t objectID);
    void locateItem(uint32_t objectID);
    void drillRequested(const QUuid& target);
    void sourceEditorRequested(const QUuid& soundGuid, const QUuid& entryId);

private:
    void buildUi();

    void rebuildTable();
    void refreshDerived();

    bool commitGrid(const DiscreteGridDef& next, const QString& undoText);

    void moveAxis(int from, int to);
    void rebuildReferenceWidgets();
    void refreshActions();

    std::vector<std::optional<float>> axisEvalValues() const;
    bool isNestedGridTarget(const QUuid& target) const;

    ProjectItem* boundItem_ = nullptr;
    DiscreteGridDef grid_;
    int winner_ = -1;

    bool updating_ = false;
    std::optional<std::pair<int, int>> requestedSelection_;

    ObjectRefWidget* axisAdder_ = nullptr;
    ObjectRefWidget* rowAdder_ = nullptr;
    QToolButton* addBtn_ = nullptr;
    QToolButton* removeBtn_ = nullptr;
    QAction* addRowAction_ = nullptr;
    QTableWidget* table_ = nullptr;
    MatrixTableWidget* matrix_ = nullptr;
    std::vector<ObjectRefWidget*> axisHeaderRefs_;
    std::vector<ObjectRefWidget*> targetRefs_;
};

}
