#pragma once

#include "Core/ThemeSnapshot.h"

#include <QRectF>

class QPainter;

namespace VNX {

struct RotaryKnobRenderFrame
{
    QRectF bounds;
    double position = 0.0;
    bool enabled = true;
    bool hovered = false;
    bool pressed = false;
};

class RotaryKnobRenderer
{
public:
    static void render(QPainter& painter,
                       const UiTokens& style,
                       const RotaryKnobRenderFrame& frame);
};

}
