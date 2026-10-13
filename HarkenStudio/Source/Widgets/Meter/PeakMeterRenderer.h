#pragma once

#include "MeterRenderCommon.h"
#include "Core/ThemeSnapshot.h"

class QPainter;

namespace VNX {

class PeakMeterRenderer
{
public:
    static PeakMeterLayout layout(const QRect& bounds,
                                  const MeterStyle::Peak& style,
                                  int channelCount = 2,
                                  PeakMeterLayoutMode mode = PeakMeterLayoutMode::Axis);

    static void render(QPainter& painter,
                       const MeterStyle& style,
                       const PeakMeterRenderFrame& frame);
};

}
