
#pragma once

#include <QUuid>
#include <QWidget>

class QLabel;

namespace VNX {

class RehearsalSlider;
struct ProjectItem;

class AxisRehearsalRow : public QWidget
{
    Q_OBJECT

public:
    AxisRehearsalRow(ProjectItem* item, const QString& table,
                     int occupiedSlots, QWidget* parent = nullptr);

    void refresh();

private:
    QUuid variableId_;
    QLabel* name_ = nullptr;
    QLabel* kind_ = nullptr;
    QLabel* table_ = nullptr;
    RehearsalSlider* slider_ = nullptr;
    bool updating_ = false;
};

}
