#pragma once

#include <QList>
#include <QUuid>
#include <QWidget>

#include <cstdint>

class QLabel;

namespace VNX {

class ObjectRefWidget;
class RehearsalSlider;
class ParameterValueEditor;
class TableWidget;
struct ProjectItem;

class VariableRehearsalRow : public QWidget
{
    Q_OBJECT

public:
    explicit VariableRehearsalRow(const ProjectItem* item, TableWidget* table);

    QUuid variableId() const { return variableId_; }
    QList<QWidget*> cells() const { return {variableCell_, valueCell_}; }
    void refresh();

private:
    void refreshValue();

    QUuid variableId_;
    QWidget* variableCell_ = nullptr;
    QWidget* valueCell_ = nullptr;
    ObjectRefWidget* reference_ = nullptr;
    RehearsalSlider* slider_ = nullptr;
    ParameterValueEditor* valueEditor_ = nullptr;
    bool updating_ = false;
};

}
