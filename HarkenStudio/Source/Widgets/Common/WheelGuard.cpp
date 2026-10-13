
#include "WheelGuard.h"

#include <QEvent>
#include <QWheelEvent>
#include <QWidget>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QSlider>

namespace VNX {

namespace {

bool isGuardableValueWidget(const QWidget* w)
{
    return qobject_cast<const QAbstractSpinBox*>(w)
        || qobject_cast<const QComboBox*>(w)
        || qobject_cast<const QSlider*>(w);
}

}

WheelGuard::WheelGuard(QObject* parent)
    : QObject(parent)
{
}

WheelGuard* WheelGuard::instance()
{
    static WheelGuard s;
    return &s;
}

bool WheelGuard::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type t = event->type();

    if (t == QEvent::Polish) {
        if (auto* w = qobject_cast<QWidget*>(watched)) {
            if (isGuardableValueWidget(w) && w->focusPolicy() == Qt::WheelFocus)
                w->setFocusPolicy(Qt::StrongFocus);
        }
        return false;
    }

    if (t != QEvent::Wheel)
        return false;

    auto* w = qobject_cast<QWidget*>(watched);
    if (!w || !isGuardableValueWidget(w))
        return false;

    if (w->hasFocus())
        return false;

    event->ignore();
    return true;
}

}
