#include "ToastPresenter.h"

#include "Core/FontManager.h"
#include "Core/ThemeManager.h"

#include <QEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QHideEvent>
#include <QPainter>
#include <QPainterPath>

namespace VNX {

ToastPresenter::ToastPresenter(QWidget* host)
    : QWidget(host)
    , opacity_(new QGraphicsOpacityEffect(this))
{
    Q_ASSERT(host);
    setObjectName(QStringLiteral("toastPresenter"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setFont(FontManager::instance().getFont(FontRole::Body));
    hide();

    constexpr int lifetimeMs = 3000;
    constexpr int fadeInMs = 200;
    constexpr int fadeOutMs = 300;
    opacity_->setOpacity(0.0);
    setGraphicsEffect(opacity_);
    fadeAnimation_.setTargetObject(opacity_);
    fadeAnimation_.setPropertyName("opacity");
    fadeAnimation_.setDuration(lifetimeMs);
    fadeAnimation_.setKeyValueAt(0.0, 0.0);
    fadeAnimation_.setKeyValueAt(qreal(fadeInMs) / lifetimeMs, 1.0);
    fadeAnimation_.setKeyValueAt(qreal(lifetimeMs - fadeOutMs) / lifetimeMs, 1.0);
    fadeAnimation_.setKeyValueAt(1.0, 0.0);
    connect(&fadeAnimation_, &QPropertyAnimation::finished, this, &ToastPresenter::dismiss);
    connect(&FontManager::instance(), &FontManager::fontChanged, this, [this]() {
        setFont(FontManager::instance().getFont(FontRole::Body));
        updatePlacement();
        update();
    });
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, this, [this]() {
        update();
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &ToastPresenter::updatePlacement);
    host->installEventFilter(this);
}

void ToastPresenter::showMessage(const QString& message)
{
    dismiss();
    if (message.trimmed().isEmpty() || !parentWidget()->isVisible())
        return;

    message_ = message;
    setAccessibleName(message_);
    updatePlacement();
    if (message_.isEmpty())
        return;

    opacity_->setOpacity(0.0);
    show();
    raise();
    update();
    fadeAnimation_.start();
}

void ToastPresenter::dismiss()
{
    fadeAnimation_.stop();
    message_.clear();
    setAccessibleName(QString());
    hide();
}

void ToastPresenter::setViewportMargins(const QMargins& margins)
{
    if (viewportMargins_ == margins)
        return;
    viewportMargins_ = margins;
    updatePlacement();
}

bool ToastPresenter::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget()) {
        if (event->type() == QEvent::Resize)
            updatePlacement();
        else if (event->type() == QEvent::Hide)
            dismiss();
    }
    return QWidget::eventFilter(watched, event);
}

void ToastPresenter::hideEvent(QHideEvent* event)
{
    fadeAnimation_.stop();
    message_.clear();
    setAccessibleName(QString());
    QWidget::hideEvent(event);
}

void ToastPresenter::updatePlacement()
{
    if (message_.isEmpty())
        return;

    const auto& geometry = ThemeManager::instance().metrics();
    const int margin = geometry.spacingSmall;
    const QRect available = parentWidget()->rect()
        .marginsRemoved(viewportMargins_).adjusted(margin, margin, -margin, -margin);
    if (available.isEmpty()) {
        dismiss();
        return;
    }

    const QFontMetrics metrics(font());
    const int width = qMin(available.width(),
        qBound(geometry.toast.minimumWidth, metrics.horizontalAdvance(message_) + geometry.spacingLarge * 2, geometry.toast.maximumWidth));
    const QRect textBounds = metrics.boundingRect(
        QRect(0, 0, qMax(1, width - geometry.spacingLarge * 2), 0), Qt::TextWordWrap, message_);
    const int height = qMin(available.height(), qMax(geometry.control.largeHeight, textBounds.height() + geometry.spacingSmall * 2));
    setGeometry(available.x() + (available.width() - width) / 2,
                available.y(), width, height);
}

void ToastPresenter::paintEvent(QPaintEvent*)
{
    const auto& colors = ThemeManager::instance().colors();
    const auto& metrics = ThemeManager::instance().metrics();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath background;
    background.addRoundedRect(QRectF(rect()),
                              metrics.radiusMedium, metrics.radiusMedium);
    painter.fillPath(background, colors.bgSecondary);
    painter.setPen(colors.textPrimary);
    painter.setFont(font());
    painter.drawText(rect().adjusted(metrics.spacingLarge, metrics.spacingSmall, -metrics.spacingLarge, -metrics.spacingSmall),
                     Qt::AlignVCenter | Qt::TextWordWrap, message_);
}

}
