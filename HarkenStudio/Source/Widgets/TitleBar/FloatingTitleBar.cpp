#include "FloatingTitleBar.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QShowEvent>
#include <QSizePolicy>

namespace VNX {

FloatingTitleBar::FloatingTitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName("TitleBarWidget");
    setFixedHeight(36);

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(8, 0, 0, 0);
    layout_->setSpacing(6);

    logoLabel_ = new QLabel(this);
    logoLabel_->setObjectName("titleBarLogo");
    const auto logo = ThemeManager::instance().resolveImage(
        QString::fromLatin1(ThemeAssetIds::AppLogo),
        ThemeAssetVariant{QSize(24, 24), devicePixelRatioF(), 0, QColor()});
    if (logo.resource)
        logoLabel_->setPixmap(QPixmap::fromImage(logo.resource->image));
    logoLabel_->setFixedSize(24, 24);

    layout_->addWidget(logoLabel_);
    layout_->addStretch();

    auto makeWinBtn = [this](const QString& name, TitleBarButtonKind kind, const QString& tip) {
        auto* btn = new TitleBarButton(kind, this);
        btn->setObjectName(name);
        btn->setToolTip(tip);
        return btn;
    };

    btnMin_   = makeWinBtn("titleBarMin", TitleBarButtonKind::Minimize, tr("Minimize"));
    btnMax_   = makeWinBtn("titleBarMax", TitleBarButtonKind::Maximize, tr("Maximize"));
    btnClose_ = makeWinBtn("titleBarClose", TitleBarButtonKind::Close, tr("Close"));

    layout_->addWidget(btnMin_);
    layout_->addWidget(btnMax_);
    layout_->addWidget(btnClose_);

    trackWindow(parentWidget());
}

void FloatingTitleBar::updateMaxButton(bool maximized) {
    btnMax_->setButtonKind(maximized ? TitleBarButtonKind::Restore : TitleBarButtonKind::Maximize);
    btnMax_->setToolTip(maximized ? tr("Restore Down") : tr("Maximize"));
}

bool FloatingTitleBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == trackedWindow_.data() &&
        (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Show)) {
        syncMaxButton();
    }
    return QWidget::eventFilter(watched, event);
}

void FloatingTitleBar::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    trackWindow(window());
    syncMaxButton();
}

void FloatingTitleBar::trackWindow(QWidget* window) {
    QWidget* topLevel = window ? window->window() : nullptr;
    if (!topLevel)
        return;
    if (trackedWindow_ == topLevel) {
        syncMaxButton();
        return;
    }
    if (trackedWindow_)
        trackedWindow_->removeEventFilter(this);
    trackedWindow_ = topLevel;
    trackedWindow_->installEventFilter(this);
    syncMaxButton();
}

void FloatingTitleBar::syncMaxButton() {
    if (trackedWindow_)
        updateMaxButton(trackedWindow_->isMaximized());
}

}
