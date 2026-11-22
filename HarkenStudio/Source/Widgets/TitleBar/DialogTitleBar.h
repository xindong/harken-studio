#pragma once
#include "TitleBarBase.h"

class QLabel;

namespace harken::studio {

class DialogTitleBar : public TitleBarBase {
    Q_OBJECT
public:
    explicit DialogTitleBar(QWidget* parent = nullptr);

    void setTitle(const QString& title);

private:
    QLabel*      titleLabel_{nullptr};
};

}
