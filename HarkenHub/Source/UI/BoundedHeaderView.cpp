#include "UI/BoundedHeaderView.h"

#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>
#include <QVector>

#include <cmath>

namespace VNH {

BoundedHeaderView::BoundedHeaderView(Qt::Orientation orientation,
                                     QWidget* parent)
    : QHeaderView(orientation, parent)
{
}

void BoundedHeaderView::setFillSection(int logicalIndex)
{
    fillSection_ = logicalIndex;
}

void BoundedHeaderView::mousePressEvent(QMouseEvent* event)
{
    dragSection_ = -1;
    if (event->button() == Qt::LeftButton &&
        orientation() == Qt::Horizontal) {
        const int position = qRound(event->position().x());
        const int grip = qMax(
            6, style()->pixelMetric(QStyle::PM_HeaderGripMargin, nullptr, this));
        for (int logicalIndex = 0; logicalIndex < count(); ++logicalIndex) {
            if (sectionResizeMode(logicalIndex) != Interactive)
                continue;
            const int boundary = sectionViewportPosition(logicalIndex) +
                                 sectionSize(logicalIndex);
            if (std::abs(position - boundary) > grip)
                continue;
            dragSection_ = logicalIndex;
            dragStartPosition_ = position;
            dragStartSize_ = sectionSize(logicalIndex);
            const int fillCapacity =
                fillSection_ >= 0
                    ? qMax(0, sectionSize(fillSection_) - minimumSectionSize())
                    : 0;
            dragMaximumSize_ = dragStartSize_ + fillCapacity;
            break;
        }
    }
    QHeaderView::mousePressEvent(event);
}

void BoundedHeaderView::mouseMoveEvent(QMouseEvent* event)
{
    if (dragSection_ < 0 || !(event->buttons() & Qt::LeftButton)) {
        QHeaderView::mouseMoveEvent(event);
        return;
    }

    const int requestedSize =
        dragStartSize_ + qRound(event->position().x()) - dragStartPosition_;
    if (requestedSize <= dragMaximumSize_) {
        QHeaderView::mouseMoveEvent(event);
        return;
    }

    QPointF localPosition = event->position();
    const qreal boundedX = dragStartPosition_ +
                           dragMaximumSize_ - dragStartSize_;
    const qreal delta = boundedX - localPosition.x();
    localPosition.setX(boundedX);
    QPointF globalPosition = event->globalPosition();
    globalPosition.rx() += delta;
    QMouseEvent boundedEvent(
        event->type(), localPosition, globalPosition, event->button(),
        event->buttons(), event->modifiers(), event->pointingDevice());
    QHeaderView::mouseMoveEvent(&boundedEvent);
}

void BoundedHeaderView::mouseReleaseEvent(QMouseEvent* event)
{
    QHeaderView::mouseReleaseEvent(event);
    dragSection_ = -1;
}

void BoundedHeaderView::resizeEvent(QResizeEvent* event)
{
    QHeaderView::resizeEvent(event);
    constrainSectionsToViewport();
}

void BoundedHeaderView::showEvent(QShowEvent* event)
{
    QHeaderView::showEvent(event);
    constrainSectionsToViewport();
}

void BoundedHeaderView::constrainSectionsToViewport()
{
    if (orientation() != Qt::Horizontal || fillSection_ < 0 ||
        fillSection_ >= count())
        return;

    const int overflow = length() - viewport()->width();
    if (overflow <= 0)
        return;

    const int minimum = minimumSectionSize();
    QVector<int> sections;
    QVector<int> capacities;
    int totalCapacity = 0;
    for (int logicalIndex = 0; logicalIndex < count(); ++logicalIndex) {
        if (logicalIndex == fillSection_ || isSectionHidden(logicalIndex) ||
            sectionResizeMode(logicalIndex) != Interactive)
            continue;
        const int capacity = qMax(0, sectionSize(logicalIndex) - minimum);
        if (capacity == 0)
            continue;
        sections.push_back(logicalIndex);
        capacities.push_back(capacity);
        totalCapacity += capacity;
    }

    int remaining = qMin(overflow, totalCapacity);
    int remainingCapacity = totalCapacity;
    for (int index = 0; index < sections.size() && remaining > 0; ++index) {
        const int reduction = index + 1 == sections.size()
            ? remaining
            : qMin(capacities[index],
                   static_cast<int>(std::ceil(
                       static_cast<double>(remaining) * capacities[index] /
                       remainingCapacity)));
        resizeSection(sections[index],
                      sectionSize(sections[index]) - reduction);
        remaining -= reduction;
        remainingCapacity -= capacities[index];
    }
}

}
