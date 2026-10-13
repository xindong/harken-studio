#pragma once

#include "MeterRenderCommon.h"
#include "Core/ThemeSnapshot.h"

class QPainter;

namespace VNX {

class LoudnessMeterRenderer
{
public:
    static LoudnessMeterLayout layout(const QRect& bounds,
                                      const MeterStyle::Loudness& style);

    static void render(QPainter& painter,
                       const MeterStyle& style,
                       const LoudnessMeterRenderFrame& frame);
};

}
