#include "RenameClickHandler.h"

#include <QApplication>
#include <QMouseEvent>
#include <QWidget>

#include <utility>

namespace VNX {

RenameClickHandler::RenameClickHandler(QWidget* focusScope, QWidget* mouseArea,
                                       TargetAt targetAt, QObject* parent)
    : QObject(parent ? parent : focusScope), focusScope_(focusScope), mouseArea_(mouseArea),
      targetAt_(std::move(targetAt))
{
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] {
        const auto key = pendingKey_;
        cancel();
        if (!ownsFocus() || !focusScope_->isActiveWindow() || !mouseArea_->isVisible()) return;
        const auto target = targetAt_(releasePosition_);
        if (key.isValid() && target.key == key && target.rename) target.rename();
    });
    connect(qApp, &QApplication::focusChanged, this, [this] {
        if (!ownsFocus()) resetFocus();
    });
    qApp->installEventFilter(this);
}

bool RenameClickHandler::ownsFocus() const
{
    auto* focus = QApplication::focusWidget();
    return focus == focusScope_ || (focus && focusScope_->isAncestorOf(focus));
}

void RenameClickHandler::cancel()
{
    timer_.stop();
    pressedKey_.clear();
    pendingKey_.clear();
}

void RenameClickHandler::resetFocus()
{
    cancel();
    ready_ = false;
}

bool RenameClickHandler::eventFilter(QObject* watched, QEvent* event)
{
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget) return false;
    const bool inside = widget == mouseArea_ || mouseArea_->isAncestorOf(widget);
    switch (event->type()) {
    case QEvent::WindowDeactivate:
        if (widget == focusScope_->window()) resetFocus();
        break;
    case QEvent::Hide:
        if (widget == focusScope_ || widget->isAncestorOf(focusScope_)) resetFocus();
        break;
    case QEvent::MouseButtonPress: {
        cancel();
        if (!inside) {
            ready_ = false;
            break;
        }
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const bool eligible = ready_ && ownsFocus() && focusScope_->isActiveWindow()
            && mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::NoModifier;
        ready_ = true;
        pressPosition_ = mouse->globalPosition().toPoint();
        if (eligible) pressedKey_ = targetAt_(mouseArea_->mapFromGlobal(pressPosition_)).key;
        break;
    }
    case QEvent::MouseMove: {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (pressedKey_.isValid()
            && (mouse->globalPosition().toPoint() - pressPosition_).manhattanLength()
                >= QApplication::startDragDistance())
            cancel();
        break;
    }
    case QEvent::MouseButtonRelease: {
        const auto key = pressedKey_;
        pressedKey_.clear();
        if (!key.isValid()) break;
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (!inside || mouse->button() != Qt::LeftButton || mouse->modifiers() != Qt::NoModifier
            || !ownsFocus() || !focusScope_->isActiveWindow()
            || (mouse->globalPosition().toPoint() - pressPosition_).manhattanLength()
                >= QApplication::startDragDistance())
            break;
        releasePosition_ = mouseArea_->mapFromGlobal(mouse->globalPosition().toPoint());
        if (targetAt_(releasePosition_).key == key) {
            pendingKey_ = key;
            timer_.start(QApplication::doubleClickInterval());
        }
        break;
    }
    case QEvent::MouseButtonDblClick:
    case QEvent::KeyPress:
    case QEvent::Wheel:
    case QEvent::ContextMenu:
        cancel();
        break;
    default:
        break;
    }
    return false;
}

}
