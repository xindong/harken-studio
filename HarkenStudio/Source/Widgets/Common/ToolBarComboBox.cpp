#include "ToolBarComboBox.h"

#include "Core/QtControlSkin.h"

namespace harken::studio {

namespace {

void configureToolBarCombo(QComboBox* combo)
{
    QtControlSkin::setSemanticProperty(combo, "fieldRole", QStringLiteral("toolBar"));
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(12);
    combo->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

}

ToolBarComboBox::ToolBarComboBox(QWidget* parent)
    : QComboBox(parent)
{
    configureToolBarCombo(this);
}

ToolBarMenuComboBox::ToolBarMenuComboBox(QWidget* parent)
    : MenuComboBox(parent)
{
    configureToolBarCombo(this);
}

}
