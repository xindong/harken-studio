#pragma once

#include "Core/ThemeSnapshot.h"
#include "Core/Timeline/TimelineTypes.h"

#include <QString>
#include <QVector>

#include <cstdint>

namespace VNX {

inline constexpr int kTimelinePixelOrigin = 8;

struct TimelineTick
{
    int64_t coordinate = 0;
    int pixel = 0;
    bool major = false;
    bool labeled = false;
    QString label;
};

struct TimelineTickSet
{
    QVector<TimelineTick> ticks;
    int64_t majorStep = 0;
    int64_t minorStep = 0;
};

int64_t timelineCoordinateUnit(const PieceTimeSpec& spec);
int64_t timelineSnapGrid(const PieceTimeSpec& spec);

TimelineTickSet buildTimelineTicks(const PieceTimeSpec& spec,
                                   int64_t viewOrigin,
                                   int viewOffset,
                                   double pixelsPerUnit,
                                   int width,
                                   const ThemeMetrics::Timeline& metrics,
                                   int pixelOrigin = kTimelinePixelOrigin);

}
