#include "RehearsalTable.h"

#include "VariableRehearsalRow.h"
#include "Core/ProjectTypes.h"

#include <QScrollArea>
#include <QVBoxLayout>

namespace VNX {

RehearsalTable::RehearsalTable(QWidget* parent)
    : TableWidget(TableOptions::display(), parent)
{
    setColumns(QStringLiteral("auditionVariables"), {
        {QStringLiteral("variable"), tr("Variable"), 220, 100},
        {QStringLiteral("value"), tr("Value"), 200, 140}
    });
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void RehearsalTable::setVariables(const std::vector<const ProjectItem*>& variables)
{
    bool same = variables.size() == static_cast<size_t>(rows_.size());
    for (int i = 0; same && i < rows_.size(); ++i)
        same = variables[static_cast<size_t>(i)]->guid == rows_[i]->variableId();
    if (same) {
        for (auto* row : rows_)
            row->refresh();
    } else {
        qDeleteAll(rows_);
        rows_.clear();
        for (const auto* item : variables) {
            auto* row = new VariableRehearsalRow(item, this);
            rowsLayout()->addWidget(row);
            registerRow(row, row->cells());
            rows_.append(row);
        }
    }
    refreshLayout();
}

}
