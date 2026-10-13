#pragma once

#include "MeterRenderCommon.h"
#include "Core/ThemeSnapshot.h"

class QPainter;

namespace VNX {

class MeterInteractionRenderer
{
public:
    static void render(QPainter& painter,
                       const MeterStyle::Interaction& style,
                       const MeterInteractionOverlay& overlay);
};

}
