#pragma once

#include <QGuiApplication>
#include <QList>
#include <QRect>
#include <QScreen>
#include <QWidget>

namespace VNX {

struct WindowPlacement
{
    QRect frame;
    QRect available;
};

inline QPoint boundedWindowPosition(const QRect& frame, const QRect& available)
{
    return {
        qBound(available.left(), frame.left(),
               qMax(available.left(), available.right() - frame.width() + 1)),
        qBound(available.top(), frame.top(),
               qMax(available.top(), available.bottom() - frame.height() + 1))};
}

inline WindowPlacement visibleWindowPlacement(
    const QRect& frame, const QList<QRect>& screens, int primaryIndex)
{
    if (!frame.isValid())
        return {frame, {}};

    int firstValid = -1;
    int selected = -1;
    qint64 bestArea = 0;
    bool titleReachable = false;
    const QRect title(frame.topLeft(), QSize(frame.width(), qMin(32, frame.height())));
    for (int i = 0; i < screens.size(); ++i) {
        const QRect& available = screens[i];
        if (!available.isValid())
            continue;
        if (firstValid < 0)
            firstValid = i;
        if (available.contains(frame))
            return {frame, available};
        const QRect overlap = available.intersected(frame);
        const qint64 area = overlap.isValid()
            ? qint64(overlap.width()) * overlap.height() : 0;
        if (area > bestArea) {
            bestArea = area;
            selected = i;
        }
        const QRect visibleTitle = available.intersected(title);
        titleReachable = titleReachable
            || (visibleTitle.height() == title.height()
                && visibleTitle.width() >= qMin(160, frame.width()));
    }
    if (firstValid < 0)
        return {frame, {}};
    if (selected < 0)
        selected = primaryIndex >= 0 && primaryIndex < screens.size()
                && screens[primaryIndex].isValid() ? primaryIndex : firstValid;

    const QRect available = screens[selected];
    if (titleReachable && frame.width() <= available.width()
        && frame.height() <= available.height())
        return {frame, available};

    QRect placed(frame.topLeft(), frame.size().boundedTo(available.size()));
    placed.moveTopLeft(boundedWindowPosition(placed, available));
    return {placed, available};
}

inline void ensureWindowVisible(QWidget* window)
{
    if (!window || !window->isWindow() || window->isMaximized()
        || window->isFullScreen() || window->isMinimized())
        return;

    const auto screens = QGuiApplication::screens();
    QList<QRect> available;
    for (QScreen* screen : screens)
        available.append(screen->availableGeometry());
    const QRect frame = window->frameGeometry();
    const auto placement = visibleWindowPlacement(
        frame, available, screens.indexOf(QGuiApplication::primaryScreen()));
    if (!placement.available.isValid() || placement.frame == frame)
        return;

    const QSize frameMargins = frame.size() - window->size();
    const QSize clientSize = placement.frame.size() - frameMargins;
    window->resize(qMax(1, clientSize.width()), qMax(1, clientSize.height()));
    const QRect actual = window->frameGeometry();
    const QPoint position = boundedWindowPosition(
        QRect(placement.frame.topLeft(), actual.size()), placement.available);
    window->move(window->pos() + position - actual.topLeft());
}

}
