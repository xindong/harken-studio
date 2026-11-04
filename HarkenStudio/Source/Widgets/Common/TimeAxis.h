#pragma once

#include "Core/FontManager.h"
#include "Core/ThemeSnapshot.h"

#include <QRect>
#include <QString>
#include <QVector>

#include <cstdint>

class QFontMetrics;
class QPainter;

namespace VNX {

inline constexpr int kTimeAxisHandleBandHeight = 12;
inline constexpr int kTimeAxisHandleWidth = 12;

struct NanosecondPosition
{
    int64_t nanoseconds = 0;
    double fraction = 0.0;

    NanosecondPosition shifted(double offset) const;
    double distanceTo(int64_t coordinate) const;
    int64_t rounded() const;
};

struct TimeAxisTick
{
    double value = 0.0;
    int pixel = 0;
    bool major = false;
    QString label;
    bool beat = false;
    QString secondaryLabel{};
    int64_t coordinate = 0;
};

struct TimeAxisTicks
{
    double majorStep = 0.0;
    double minorStep = 0.0;
    QVector<TimeAxisTick> ticks;
};

enum class TimeAxisLabelFormat { Seconds, MinutesSecondsMilliseconds };
enum class TimeAxisLabelAlignment { Center, AfterTick };

TimeAxisTicks buildTimeAxisTicks(double start, double span, int width,
    double minimumStep, int maximumDecimals, const QFontMetrics& metrics);
TimeAxisTicks buildNanosecondTimeAxisTicks(NanosecondPosition start, double span,
    int width, const QFontMetrics& metrics, int64_t minimumStep = 1,
    TimeAxisLabelFormat format = TimeAxisLabelFormat::Seconds);
QString formatTimeNanoseconds(int64_t nanoseconds, int minimumDecimals = 0, int maximumDecimals = 9);
QString formatTimeMinutesSecondsMilliseconds(int64_t nanoseconds,
    int minimumDecimals = 3, int maximumDecimals = 3);
void drawTimeAxisRuler(QPainter& painter, const ThemeColors& colors,
    const QRect& bounds, const TimeAxisTicks& ticks, FontRole role = FontRole::Axis,
    TimeAxisLabelAlignment alignment = TimeAxisLabelAlignment::Center, bool pairedLabels = false);
void drawTimeAxisGrid(QPainter& painter, const QColor& majorColor,
    const QRect& bounds, const TimeAxisTicks& ticks, const QColor& minorColor = {},
    const QColor& beatColor = {});
void drawTimeAxisHandle(QPainter& painter, const QColor& color,
    const QColor& grooveColor, const QRect& bounds);

}
