#pragma once

#include <QCoreApplication>
#include <QMainWindow>
#include <QStatusTipEvent>

namespace VNX {

inline void showStatusTip(QWidget* source, const QString& message)
{
    for (QWidget* widget = source; widget; widget = widget->parentWidget()) {
        if (auto* window = qobject_cast<QMainWindow*>(widget)) {
            QStatusTipEvent feedback(message);
            QCoreApplication::sendEvent(window, &feedback);
            return;
        }
    }
}

}
