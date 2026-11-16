#pragma once
#include "TitleBarBase.h"

namespace VNX {

class FloatingTitleBar : public TitleBarBase {
    Q_OBJECT
public:
    explicit FloatingTitleBar(QWidget* parent = nullptr);
};

}
