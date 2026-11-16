#include "TitleBarBase.h"

#include "Core/ThemeLayout.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QScreen>
#include <QShowEvent>
#include <QWindow>

namespace VNX {

TitleBarBase::TitleBarBase(Qt::WindowFlags controls, QWidget* parent) : QWidget(parent) {
    setObjectName("TitleBarWidget");
    setAttribute(Qt::WA_StyledBackground);

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    ThemeLayout::spacing(layout_, ThemeLayout::Space::Section);
    layout_->addStretch();

    const auto addButton = [this](const QString& name, TitleBarButtonKind kind, const QString& tip) {
        auto* button = new TitleBarButton(kind, this);
        button->setObjectName(name);
        button->setToolTip(tip);
        layout_->addWidget(button);
        return button;
    };
    if (controls.testFlag(Qt::WindowMinimizeButtonHint))
        btnMin_ = addButton("titleBarMin", TitleBarButtonKind::Minimize, tr("Minimize"));
    if (controls.testFlag(Qt::WindowMaximizeButtonHint))
        btnMax_ = addButton("titleBarMax", TitleBarButtonKind::Maximize, tr("Maximize"));
    if (controls.testFlag(Qt::WindowCloseButtonHint))
        btnClose_ = addButton("titleBarClose", TitleBarButtonKind::Close, tr("Close"));

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &TitleBarBase::requestChromeMetricsSync);
    trackWindow();
    syncChromeMetrics();
}

bool TitleBarBase::eventFilter(QObject* watched, QEvent* event) {
    if (watched == trackedWindow_.data() &&
        (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Show))
        syncMaxButton();
    return QWidget::eventFilter(watched, event);
}

void TitleBarBase::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange ||
        event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange)
        requestChromeMetricsSync();
}

void TitleBarBase::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    trackWindow();
    syncChromeMetrics();
}

void TitleBarBase::trackWindow() {
    QWidget* topLevel = window();
    if (trackedWindow_ != topLevel) {
        if (trackedWindow_) {
            trackedWindow_->removeEventFilter(this);
            if (QWindow* handle = trackedWindow_->windowHandle())
                disconnect(handle, &QWindow::screenChanged, this, &TitleBarBase::trackScreen);
        }
        trackedWindow_ = topLevel;
        trackedWindow_->installEventFilter(this);
    }
    if (QWindow* handle = topLevel->windowHandle()) {
        connect(handle, &QWindow::screenChanged, this, &TitleBarBase::trackScreen,
                Qt::UniqueConnection);
        trackScreen(handle->screen());
    }
    syncMaxButton();
}

void TitleBarBase::trackScreen(QScreen* screen) {
    if (trackedScreen_ == screen)
        return;
    if (trackedScreen_)
        disconnect(trackedScreen_, nullptr, this, nullptr);
    trackedScreen_ = screen;
    if (screen)
        connect(screen, &QScreen::logicalDotsPerInchChanged,
                this, &TitleBarBase::requestChromeMetricsSync);
    requestChromeMetricsSync();
}

void TitleBarBase::syncMaxButton() {
    if (!btnMax_ || !trackedWindow_)
        return;
    const bool maximized = trackedWindow_->isMaximized();
    btnMax_->setButtonKind(maximized ? TitleBarButtonKind::Restore : TitleBarButtonKind::Maximize);
    btnMax_->setToolTip(maximized ? tr("Restore Down") : tr("Maximize"));
}

void TitleBarBase::requestChromeMetricsSync() {
    if (metricsSyncQueued_)
        return;
    metricsSyncQueued_ = true;
    QMetaObject::invokeMethod(this, &TitleBarBase::syncChromeMetrics, Qt::QueuedConnection);
}

void TitleBarBase::syncChromeMetrics() {
    metricsSyncQueued_ = false;
    const auto& metrics = ThemeManager::instance().metrics();
    const int contentHeight = qMax(fontMetrics().height() + metrics.spacingSmall, syncContentMetrics());
    const int barHeight = qMax(metrics.chrome.titleBarHeight, contentHeight);
    setFixedHeight(barHeight);
    for (auto* button : {btnMin_, btnMax_, btnClose_})
        if (button)
            button->setFixedSize(metrics.chrome.windowButtonWidth, barHeight);
    updateContentGeometry();
}

int TitleBarBase::syncContentMetrics() {
    return 0;
}

void TitleBarBase::updateContentGeometry() {
}

}
