#pragma once

#include <QRect>
#include <QString>
#include <QVector>

class QFontMetrics;

namespace VNX {

inline constexpr double kAmplitudeFloorDb = -96.0;

struct AmplitudeScale
{
    double minimumDb = kAmplitudeFloorDb;
    double maximumDb = 0.0;
    double exponent = 1.0;

    double dbToPosition(double db) const;
    double positionToDb(double position) const;
    double amplitudeToPosition(double amplitude) const;
    double positionToAmplitude(double position) const;
};

struct AmplitudeAxisTick
{
    int y = 0;
    QString label;
    bool major = false;
    double db = 0.0;
};

double amplitudeToDb(double amplitude);
QRect waveformAmplitudeBounds(const QRect& viewport, int contentHeight, int scroll,
    const QFontMetrics& font);
QVector<AmplitudeAxisTick> buildAmplitudeAxisTicks(const QRect& viewport, double zeroY,
    double amplitudeHeight, const QFontMetrics& font, double targetSpacing,
    const AmplitudeScale& scale = {}, bool bipolar = true);

}
