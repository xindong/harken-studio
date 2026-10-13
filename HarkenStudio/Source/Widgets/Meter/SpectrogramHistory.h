#pragma once

#include <QImage>
#include <QList>
#include <QSize>
#include <QVector>

#include <cstdint>
#include <span>

namespace VNX {

class SpectrogramHistory
{
public:
    SpectrogramHistory() = default;
    SpectrogramHistory(uint32_t sampleRate, uint32_t fftSize, int capacity);

    void append(std::span<const float> decibels);
    int columns() const { return columns_; }
    int capacity() const { return capacity_; }
    const QImage& image(QSize physicalPixels, double columnsPerPixel,
                        const QList<QRgb>& palette) const;

private:
    struct TimeSample
    {
        int first = -1;
        int last = -1;
        float fraction = 0.0f;
    };

    struct FrequencySample
    {
        int lower;
        int upper;
        int first;
        int last;
        float fraction;
    };

    uint32_t sampleRate_ = 0;
    uint32_t fftSize_ = 0;
    int capacity_ = 0;
    int columns_ = 0;
    int writeColumn_ = 0;
    QVector<float> powers_;
    mutable bool imageDirty_ = true;
    mutable QImage image_;
    mutable double columnsPerPixel_ = 0.0;
    mutable QList<QRgb> palette_;
    mutable QVector<TimeSample> timeSamples_;
    mutable QVector<FrequencySample> frequencySamples_;
    mutable QVector<float> temporal_;
    mutable QVector<float> rowPowers_;
};

}
