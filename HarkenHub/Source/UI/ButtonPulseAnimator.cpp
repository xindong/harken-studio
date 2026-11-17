#include "UI/ButtonPulseAnimator.h"

#include "Platform/UserSession.h"

#include <QAbstractButton>
#include <QColor>
#include <QEasingCurve>
#include <QPalette>

namespace VNH {

ButtonPulseAnimator::ButtonPulseAnimator(QObject* parent)
    : QObject(parent)
{
    animation_.setDuration(1000);
    animation_.setLoopCount(3);
    animation_.setEasingCurve(QEasingCurve::InOutSine);
    connect(&animation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                if (!button_)
                    return;
                const QColor color = value.value<QColor>();
                const QString textColor = color.lightness() >= 145
                                              ? QStringLiteral("#000000")
                                              : QStringLiteral("#E6EBF2");
                button_->setStyleSheet(QStringLiteral(
                    "background-color: %1; border: 1px solid palette(highlight); "
                    "color: %2; font-weight: 600;")
                                           .arg(color.name(), textColor));
            });
    connect(&animation_, &QVariantAnimation::finished, this, [this]() {
        if (button_)
            button_->setStyleSheet(finalStyleSheet_);
    });
}

void ButtonPulseAnimator::start(QAbstractButton* button)
{
    if (!button)
        return;
    if (!systemAnimationsEnabled()) {
        stop();
        return;
    }
    if (animation_.state() == QAbstractAnimation::Running) {
        animation_.stop();
        if (button_ && button_ != button)
            button_->setStyleSheet(finalStyleSheet_);
    } else {
        finalStyleSheet_ = button->styleSheet();
    }
    if (button_ != button)
        finalStyleSheet_ = button->styleSheet();
    button_ = button;
    const QColor accent = button->palette().color(QPalette::Highlight);
    animation_.setStartValue(accent.darker(150));
    animation_.setKeyValueAt(0.5, accent);
    animation_.setEndValue(accent.darker(150));
    animation_.start();
}

void ButtonPulseAnimator::stop()
{
    animation_.stop();
    if (button_)
        button_->setStyleSheet(finalStyleSheet_);
    button_.clear();
    finalStyleSheet_.clear();
}

}
