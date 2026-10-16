#pragma once

#include <QScrollArea>

namespace VNX {

class PanelScrollArea : public QScrollArea
{
public:
    explicit PanelScrollArea(QWidget* parent = nullptr);
};

}
