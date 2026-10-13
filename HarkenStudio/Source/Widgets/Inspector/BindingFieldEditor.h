#pragma once

#include "Core/CurveSelection.h"

#include <QFrame>
#include <QPointer>

#include <vector>

class QToolButton;

namespace VNX {

struct ProjectItem;
class TableWidget;
class BindingAddMenu;

class BindingFieldEditor final : public QFrame
{
    Q_OBJECT

public:
    explicit BindingFieldEditor(QWidget* parent = nullptr);
    ~BindingFieldEditor() override;

    void bind(ProjectItem* item, const BindingAddress& address);
    void selectBinding(const QUuid& owner, harkenstudio::ObjectId stableId);
    void showAddMenu();

signals:
    void curveSelectionChanged(const CurveSelection& selection);
    void navigateRequested(const QUuid& guid);

public slots:
    void selectRow(int index);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Row {
        ParameterBindingDef binding;
        QPointer<QWidget> widget;
    };

    void reload();
    void refreshCurves();
    QWidget* buildRow(int index);
    void changeVariable(int index, uint32_t objectId);
    void removeSelected();
    void navigateTo(const QUuid& guid);
    void resizeToRows();

    ProjectItem* item_ = nullptr;
    BindingAddress address_;
    TableWidget* table_ = nullptr;
    QWidget* rows_ = nullptr;
    QToolButton* add_ = nullptr;
    BindingAddMenu* addMenu_ = nullptr;
    QToolButton* remove_ = nullptr;
    std::vector<Row> entries_;
    harkenstudio::ObjectId selectedId_{};
    int selected_ = -1;
};

}
