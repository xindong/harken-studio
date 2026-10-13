#pragma once

#include "MeterRenderCommon.h"
#include "Core/ThemeSnapshot.h"

class QPainter;

namespace VNX {

class VUMeterRenderer
{
public:
    static VUMeterLayout layout(const QRect& bounds,
                                const MeterStyle::VU& style,
                                VUMeterLayoutMode mode);

    static void renderAnalog(QPainter& painter,
                             const MeterStyle& style,
                             const VUAnalogMeterRenderFrame& frame);

    static void renderBars(QPainter& painter,
                           const MeterStyle& style,
                           const VUBarMeterRenderFrame& frame);
};

}
