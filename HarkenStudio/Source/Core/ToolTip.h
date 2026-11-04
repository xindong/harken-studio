#pragma once

#include <QPoint>
#include <QRect>
#include <QString>

class QWidget;

namespace VNX {

class ToolTip
{
public:
    static void initialize();
    static void showText(const QPoint& position, const QString& text,
                         QWidget* owner = nullptr, const QRect& rect = {}, int duration = -1);
    static void hideText();
    static QString text();
    static bool isVisible();
};

}
