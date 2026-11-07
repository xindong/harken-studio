#include "DialogTitleBar.h"
#include "Core/ThemeLayout.h"
#include <QHBoxLayout>
#include <QLabel>

namespace VNX {

DialogTitleBar::DialogTitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName("TitleBarWidget");
    ThemeManager::instance().bindMetrics(this, [this](const ThemeMetrics& metrics) {
        setFixedHeight(qMax(metrics.chrome.titleBarHeight, fontMetrics().height() + metrics.spacingSmall));
    });

    layout_ = new QHBoxLayout(this);
    ThemeLayout::margins(layout_, ThemeLayout::Space::Small, ThemeLayout::Space::None, ThemeLayout::Space::None, ThemeLayout::Space::None);
    ThemeLayout::spacing(layout_, ThemeLayout::Space::Small);

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
