#pragma once

#include "MenuComboBox.h"

namespace harken::studio {

class ToolBarComboBox : public QComboBox
{
    Q_OBJECT

public:
    explicit ToolBarComboBox(QWidget* parent = nullptr);
};

class ToolBarMenuComboBox final : public MenuComboBox
{
    Q_OBJECT

public:
    explicit ToolBarMenuComboBox(QWidget* parent = nullptr);
};

}
