#pragma once

#include "MeterRenderCommon.h"
#include "Core/ThemeSnapshot.h"

class QPainter;

namespace VNX {

class SpectrumMeterRenderer
{
public:
    static SpectrumMeterLayout layout(const QRect& bounds,
                                      const MeterStyle::Spectrum& style);

    static void render(QPainter& painter,
                       const MeterStyle& style,
                       const SpectrumMeterRenderFrame& frame);
};

}
