#pragma once

#include <QString>
#include <Qt>

class QWidget;
class QLabel;

namespace VNX {

struct InspectorFormRow
{
    QWidget* row = nullptr;
    QLabel* label = nullptr;
    QWidget* valueCell = nullptr;
};

void setInspectorFormSlotCount(QWidget* parent, int occupiedSlots);
int inspectorFormSlotCount(const QWidget* parent, int standaloneSlots = 2);
void reserveInspectorContentSlots(QWidget* content);

InspectorFormRow makeInspectorFormRow(QWidget* parent, const QString& key,
                                      const QString& text, QWidget* control,
                                      Qt::Alignment alignment = {},
                                      int occupiedSlots = 1);

}
