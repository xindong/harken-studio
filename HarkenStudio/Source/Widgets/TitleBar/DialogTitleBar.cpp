#include "DialogTitleBar.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>

namespace VNX {

DialogTitleBar::DialogTitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName("TitleBarWidget");
    setFixedHeight(36);

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(8, 0, 0, 0);
    layout_->setSpacing(6);

    logoLabel_ = new QLabel(this);
    logoLabel_->setObjectName("titleBarLogo");
    const auto logo = ThemeManager::instance().resolveImage(
        QString::fromLatin1(ThemeAssetIds::AppLogo),
        ThemeAssetVariant{QSize(20, 20), devicePixelRatioF(), 0, QColor()});
    if (logo.resource)
        logoLabel_->setPixmap(QPixmap::fromImage(logo.resource->image));
    logoLabel_->setFixedSize(20, 20);

    titleLabel_ = new QLabel(this);
    titleLabel_->setObjectName("titleBarLabel");

    layout_->addWidget(logoLabel_);
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
