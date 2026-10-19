#include "DialogTitleBar.h"
#include <QHBoxLayout>
#include <QLabel>

namespace VNX {

DialogTitleBar::DialogTitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName("TitleBarWidget");
    setFixedHeight(36);

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(8, 0, 0, 0);
    layout_->setSpacing(6);

    titleLabel_ = new QLabel(this);
    titleLabel_->setObjectName("titleBarLabel");

    layout_->addWidget(titleLabel_);
    layout_->addStretch();

    btnClose_ = new TitleBarButton(TitleBarButtonKind::Close, this);
    btnClose_->setObjectName("titleBarClose");
    btnClose_->setToolTip(tr("Close"));
    layout_->addWidget(btnClose_);
}

void DialogTitleBar::setTitle(const QString& title) {
    titleLabel_->setText(title);
}

}
