#pragma once

#include "Widgets/Common/TableWidget.h"

#include <vector>

namespace VNX {

struct ProjectItem;
class VariableRehearsalRow;

class RehearsalTable : public TableWidget
{
    Q_OBJECT

public:
    explicit RehearsalTable(QWidget* parent = nullptr);

    void setVariables(const std::vector<const ProjectItem*>& variables);

private:
    QList<VariableRehearsalRow*> rows_;
};

}
