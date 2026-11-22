#pragma once

#include <QScrollArea>

namespace harken::studio {

class PanelScrollArea : public QScrollArea
{
public:
    explicit PanelScrollArea(QWidget* parent = nullptr);
};

}
