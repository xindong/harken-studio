#pragma once

#include "MeterRenderCommon.h"
#include "Core/ThemeSnapshot.h"

class QPainter;

namespace VNX {

class SpectrogramMeterRenderer
{
public:
    static SpectrogramMeterLayout layout(
        const QRect& bounds,
        const MeterStyle::Spectrogram& style);

    static void render(QPainter& painter,
                       const MeterStyle& style,
                       const SpectrogramMeterRenderFrame& frame);
};

}
