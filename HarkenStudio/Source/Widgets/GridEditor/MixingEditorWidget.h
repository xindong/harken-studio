#pragma once

#include "Core/MixingSemantics.h"
#include "Core/VariableSemantics.h"

#include <QPointer>
#include <QWidget>

class QToolButton;
class QStandardItemModel;

namespace VNX {

struct ProjectItem;
class CurveCanvas;
class MixingRowCanvas;
class ResizableTableView;
class ObjectRefWidget;
class TableWidget;

class MixingEditorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MixingEditorWidget(QWidget* parent = nullptr);
    ~MixingEditorWidget() override;

    void setBoundItem(ProjectItem* item);
    ProjectItem* boundItem() const { return boundItem_; }
    void reload();
    const MixingDef& mixingDef() const { return mixing_; }
    const QHash<QUuid, float>& childGains() const { return gains_; }
    CurveCanvas* canvas(int row) const;
    QString statusText() const;

    bool commitAddRow(const QUuid& variable);
    bool commitRemoveRow(int row);
    bool commitReplaceVariable(int row, const QUuid& variable);
    bool commitMoveRow(int from, int to);
    bool commitAddChild(int row, const QUuid& target);
    bool commitRemoveChild(int row, int child);
    bool commitReplaceChild(int row, int child, const QUuid& target);
    bool commitMoveChild(int row, int from, int to);
    bool commitCurve(int row, int child, const QVector<CurvePointDef>& points,
                     const QVector<CurveHandlePairDef>& handles, const QString& undoText);
    void selectChild(int row, int child);

signals:
    void derivedRefreshed();
    void drillRequested(const QUuid& target);
    void navigateToItem(uint32_t objectID);
    void locateItem(uint32_t objectID);
    void sourceEditorRequested(const QUuid& soundGuid, const QUuid& entryId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct RowUi {
        QWidget* page = nullptr;
        TableWidget* children = nullptr;
        MixingRowCanvas* canvas = nullptr;
        QToolButton* removeChild = nullptr;
        std::vector<QWidget*> childRows;
        std::vector<ObjectRefWidget*> refs;
        int selected = -1;
    };

    void buildRow(int index);
    void loadCurves(int row);
    void refreshRowSelection();
    void refreshDerived();
    void scheduleReload();
    void cancelEdits();
    bool commit(const MixingDef& next, const QString& text);
    bool validRow(int row) const;
    bool validChild(int row, int child) const;
    bool eligibleChild(int row, const QUuid& target, int replacing = -1) const;
    bool isCurrent(quint64 generation) const;
    void openChildSelector(int row, QWidget* anchor);

    ProjectItem* boundItem_ = nullptr;
    MixingDef mixing_;
    std::optional<MixingDef> preview_;
    QHash<QUuid, float> gains_;
    QHash<QUuid, QUuid> selectedChildren_;
    std::vector<RowUi> views_;
    TableWidget* table_ = nullptr;
    ResizableTableView* view_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QHash<QUuid, int> rowHeights_;
    std::optional<QUuid> selectedRow_;
    QToolButton* addRow_ = nullptr;
    QToolButton* removeRow_ = nullptr;
    quint64 generation_ = 0;
    bool updating_ = false;
    bool committingCurve_ = false;
    bool reloadPending_ = false;
};

}
