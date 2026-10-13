#pragma once

#include "MeterRenderCommon.h"
#include <QMargins>

class QFontMetrics;

namespace VNX::MeterScale {

inline constexpr float kMinDb = -96.0f;
inline constexpr float kPeakMaxDb = 6.0f;
inline constexpr float kSpectrumMaxDb = 6.0f;
inline constexpr float kSpectrogramMaxDb = 24.0f;
inline constexpr float kMinFrequency = 20.0f;

float dbToPosition(float db, float maximumDb = kPeakMaxDb);
float positionToDb(float position, float maximumDb = kPeakMaxDb);
QString frequencyLabel(float frequency);
QRect plotBounds(const QRect& canvas, const QMargins& insets, bool horizontalAxis);
QVector<SpectrumMeterFrequencyTick> frequencyTicks(float maximum, int extent,
                                                  const QFontMetrics& font, bool vertical);

}
