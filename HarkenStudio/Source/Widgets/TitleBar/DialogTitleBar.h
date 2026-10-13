#pragma once
#include "TitleBarButton.h"

#include <QWidget>

class QLabel;
class QHBoxLayout;

namespace VNX {

class DialogTitleBar : public QWidget {
    Q_OBJECT
public:
    explicit DialogTitleBar(QWidget* parent = nullptr);

    void setTitle(const QString& title);

    QPushButton* closeButton() const { return btnClose_; }

private:
    QHBoxLayout* layout_{nullptr};
    QLabel*      logoLabel_{nullptr};
    QLabel*      titleLabel_{nullptr};
    TitleBarButton* btnClose_{nullptr};
};

}
