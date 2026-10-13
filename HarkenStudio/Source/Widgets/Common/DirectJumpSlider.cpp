#include "DirectJumpSlider.h"

#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>

namespace VNX {

DirectJumpSlider::DirectJumpSlider(Qt::Orientation orientation, QWidget* parent)
    : QSlider(orientation, parent)
{
}

QRect DirectJumpSlider::handleRect() const
{
    QStyleOptionSlider option;
    initStyleOption(&option);
    return style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
}

bool DirectJumpSlider::event(QEvent* event)
{
    switch (event->type())
    {
        case QEvent::UngrabMouse:
        case QEvent::Hide:
        case QEvent::WindowDeactivate:
            if (directDrag_)
            {
                directDrag_ = false;
                setSliderDown(false);
            }
            break;
        default:
            break;
    }
    return QSlider::event(event);
}

void DirectJumpSlider::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton
        || (event->buttons() ^ event->button())
        || minimum() == maximum())
    {
        QSlider::mousePressEvent(event);
        return;
    }

    const QRect handle = handleRect();
    if (handle.contains(event->position().toPoint()))
    {
        QSlider::mousePressEvent(event);
        return;
    }

    directDrag_ = true;
    dragOffset_ = orientation() == Qt::Horizontal
        ? handle.center().x() - handle.left()
        : handle.center().y() - handle.top();
    event->accept();
    setSliderDown(true);
    setSliderPosition(positionFromPoint(event->position()));
}

void DirectJumpSlider::mouseMoveEvent(QMouseEvent* event)
{
    if (!directDrag_)
    {
        QSlider::mouseMoveEvent(event);
        return;
    }

    if (!(event->buttons() & Qt::LeftButton))
    {
        directDrag_ = false;
        setSliderDown(false);
        event->ignore();
        return;
    }

    setSliderPosition(positionFromPoint(event->position()));
    event->accept();
}

void DirectJumpSlider::mouseReleaseEvent(QMouseEvent* event)
{
    if (!directDrag_ || event->button() != Qt::LeftButton)
    {
        QSlider::mouseReleaseEvent(event);
        return;
    }

    directDrag_ = false;
    setSliderPosition(positionFromPoint(event->position()));
    setSliderDown(false);
    event->accept();
}

int DirectJumpSlider::positionFromPoint(const QPointF& point) const
{
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect groove = style()->subControlRect(
        QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(
        QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);

    const bool horizontal = orientation() == Qt::Horizontal;
    const int sliderMin = horizontal ? groove.left() : groove.top();
    const int sliderMax = horizontal
        ? groove.right() - handle.width() + 1
        : groove.bottom() - handle.height() + 1;
    const int coordinate = qRound(horizontal ? point.x() : point.y());
    return QStyle::sliderValueFromPosition(
        minimum(), maximum(), coordinate - dragOffset_ - sliderMin,
        sliderMax - sliderMin, option.upsideDown);
}

}
