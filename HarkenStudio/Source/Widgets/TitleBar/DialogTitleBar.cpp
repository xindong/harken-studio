#include "DialogTitleBar.h"
#include "Core/ThemeLayout.h"
#include <QHBoxLayout>
#include <QLabel>

namespace harken::studio {

DialogTitleBar::DialogTitleBar(QWidget* parent)
    : TitleBarBase(Qt::WindowCloseButtonHint, parent) {
    ThemeLayout::margins(layout_, ThemeLayout::Space::Panel, ThemeLayout::Space::None, ThemeLayout::Space::None, ThemeLayout::Space::None);

    titleLabel_ = new QLabel(this);
    titleLabel_->setObjectName("titleBarLabel");

    layout_->insertWidget(0, titleLabel_);
}

void DialogTitleBar::setTitle(const QString& title) {
    titleLabel_->setText(title);
}

}
