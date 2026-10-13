#pragma once

#include <QChildEvent>
#include <QEvent>
#include <QObject>

namespace VNX {
namespace FocusFilter {

inline void installRecursively(QObject* host, QObject* root)
{
    if (!host || !root)
        return;
    root->installEventFilter(host);
    for (QObject* child : root->children())
        installRecursively(host, child);
}

inline bool dispatch(QObject* host, QObject* watched, QEvent* event)
{
    if (!host || !event)
        return false;
    const QEvent::Type type = event->type();
    if (type == QEvent::FocusIn)
        return watched != host;
    if (type == QEvent::ChildAdded) {
        if (auto* childEvent = static_cast<QChildEvent*>(event))
            installRecursively(host, childEvent->child());
    }
    return false;
}

}
}
