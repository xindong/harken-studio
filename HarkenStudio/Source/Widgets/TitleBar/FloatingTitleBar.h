#pragma once
#include "TitleBarBase.h"

namespace harken::studio {

class FloatingTitleBar : public TitleBarBase {
    Q_OBJECT
public:
    explicit FloatingTitleBar(QWidget* parent = nullptr);
};

}
