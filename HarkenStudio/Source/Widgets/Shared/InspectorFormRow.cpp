#include "InspectorFormRow.h"

#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/LayoutDebugTint.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QVariant>
#include <QWidget>

#include <algorithm>

namespace VNX {

void setInspectorFormSlotCount(QWidget* parent, int occupiedSlots)
{
    parent->setProperty("inspectorFormSlotCount", std::max(0, occupiedSlots));
}

int inspectorFormSlotCount(const QWidget* parent, int standaloneSlots)
{
    for (auto* widget = parent; widget; widget = widget->parentWidget()) {
        const auto count = widget->property("inspectorFormSlotCount");
        if (count.isValid()) return count.toInt();
    }
    return standaloneSlots;
}

void reserveInspectorContentSlots(QWidget* content)
{
    const auto& inspector = ThemeManager::instance().metrics().inspector;
    auto* layout = content->layout();
    auto margins = layout ? layout->contentsMargins() : content->contentsMargins();
    margins.setRight(margins.right()
        + inspector.slotWidthFor(inspectorFormSlotCount(content)));
    if (layout) layout->setContentsMargins(margins);
    else content->setContentsMargins(margins);
    setInspectorFormSlotCount(content, 0);
}

InspectorFormRow makeInspectorFormRow(QWidget* parent, const QString& key,
                                      const QString& text, QWidget* control,
                                      Qt::Alignment alignment,
                                      int occupiedSlots)
{
    const auto& inspector = ThemeManager::instance().metrics().inspector;
    InspectorFormRow result;
    result.row = new QWidget(parent);
    result.row->setObjectName(QStringLiteral("selectionFormRow_") + key);
    result.row->setProperty("layoutRole", QStringLiteral("inspectorFormRow"));
    result.row->setMinimumHeight(inspector.rowHeight);
    auto* layout = new QHBoxLayout(result.row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    DebugTint::tintContainer(result.row, DebugTint::Role::Row);

    result.label = new QLabel(text, result.row);
    result.label->setObjectName(QStringLiteral("selectionFormLabel_") + key);
    result.label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    result.label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    result.label->setContentsMargins(0, 0, inspector.columnPadding, 0);
    result.label->setToolTip(text);
    QtControlSkin::setSemanticProperty(result.label, "textRole",
                                       QStringLiteral("formLabel"));
    DebugTint::tintBare(result.label, DebugTint::Role::Label);
    layout->addWidget(result.label, inspector.labelColumnShare);

    result.valueCell = new QWidget(result.row);
    result.valueCell->setObjectName(QStringLiteral("selectionFormValue_") + key);
    result.valueCell->setProperty("columnRole", QStringLiteral("value"));
    result.valueCell->setSizePolicy(QSizePolicy::Ignored,
                                    result.valueCell->sizePolicy().verticalPolicy());
    auto* valueLayout = new QHBoxLayout(result.valueCell);
    valueLayout->setContentsMargins(inspector.columnPadding, 0,
                                    inspector.columnPadding, 0);
    valueLayout->setSpacing(0);
    DebugTint::tintContainer(result.valueCell, DebugTint::Role::Flex);
    if (alignment) {
        valueLayout->addWidget(control, 0, alignment);
    } else {
        control->setSizePolicy(QSizePolicy::Expanding,
                               control->sizePolicy().verticalPolicy());
        valueLayout->addWidget(control, 1);
    }
    layout->addWidget(result.valueCell, inspector.valueColumnShare);

    auto* gutter = new QWidget(result.row);
    gutter->setObjectName(QStringLiteral("selectionFormGutter_") + key);
    gutter->setProperty("columnRole", QStringLiteral("gutter"));
    gutter->setFixedWidth(inspector.slotWidthFor(
        inspectorFormSlotCount(parent, occupiedSlots)));
    DebugTint::tintContainer(gutter, DebugTint::Role::Gutter);
    layout->addWidget(gutter);
    return result;
}

}
