#include "SpectrogramHistory.h"

#include "MeterScale.h"

#include <algorithm>
#include <cmath>

namespace VNX {

SpectrogramHistory::SpectrogramHistory(uint32_t sampleRate, uint32_t fftSize, int capacity)
    : sampleRate_(sampleRate)
    , fftSize_(fftSize)
    , capacity_(capacity)
    , powers_(qsizetype(capacity) * (fftSize / 2 + 1), 0.0f)
{
}

void SpectrogramHistory::append(std::span<const float> decibels)
{
    const int bins = fftSize_ / 2 + 1;
    if (capacity_ == 0 || decibels.size() != size_t(bins))
        return;
    float* destination = powers_.data();
    for (int bin = 0; bin < bins; ++bin)
        destination[qsizetype(bin) * capacity_ + writeColumn_] = std::pow(10.0f,
            std::clamp(decibels[bin], MeterScale::kMinDb, MeterScale::kSpectrogramMaxDb) / 10.0f);
    writeColumn_ = (writeColumn_ + 1) % capacity_;
    columns_ = std::min(columns_ + 1, capacity_);
    imageDirty_ = true;
}

const QImage& SpectrogramHistory::image(QSize physicalPixels, double columnsPerPixel,
                                       const QList<QRgb>& palette) const
{
    if (capacity_ == 0 || physicalPixels.isEmpty() || palette.isEmpty()
        || !std::isfinite(columnsPerPixel) || columnsPerPixel <= 0.0) {
        image_ = {};
        return image_;
    }
    if (!imageDirty_ && !image_.isNull() && image_.size() == physicalPixels
        && columnsPerPixel_ == columnsPerPixel && palette_ == palette)
        return image_;
    columnsPerPixel_ = columnsPerPixel;
    palette_ = palette;
    if (image_.size() != physicalPixels)
        image_ = QImage(physicalPixels, QImage::Format_RGB32);
    const int width = physicalPixels.width();
    const int height = physicalPixels.height();
    const int bins = fftSize_ / 2 + 1;
    timeSamples_.resize(width);
    auto* timeSamples = timeSamples_.data();
    for (int x = 0; x < width; ++x) {
        auto& sample = timeSamples[x];
        sample = {};
        const int agePixels = width - 1 - x;
        if (columnsPerPixel > 1.0) {
            const double firstAge = agePixels * columnsPerPixel;
            if (firstAge >= columns_)
                continue;
            const int first = int(std::floor(firstAge));
            const int last = int(std::min(double(columns_),
                std::ceil((agePixels + 1) * columnsPerPixel)));
            sample.first = (writeColumn_ + capacity_ - last) % capacity_;
            sample.last = (writeColumn_ + capacity_ - 1 - first) % capacity_;
        } else {
            const double age = (agePixels + 0.5) * columnsPerPixel;
            if (age >= columns_)
                continue;
            const double position = std::clamp(age - 0.5, 0.0, double(columns_ - 1));
            const int first = int(std::floor(position));
            const int last = std::min(first + 1, columns_ - 1);
            sample.first = (writeColumn_ + capacity_ - 1 - first) % capacity_;
            sample.last = (writeColumn_ + capacity_ - 1 - last) % capacity_;
            sample.fraction = float(position - first);
        }
    }

    temporal_.resize(qsizetype(width) * bins);
    float* temporal = temporal_.data();
    for (int bin = 0; bin < bins; ++bin) {
        const float* source = powers_.constData() + qsizetype(bin) * capacity_;
        float* destination = temporal + qsizetype(bin) * width;
        for (int x = 0; x < width; ++x) {
            const auto& sample = timeSamples[x];
            float power = 0.0f;
            if (sample.first >= 0) {
                if (columnsPerPixel > 1.0) {
                    if (sample.first <= sample.last) {
                        for (int index = sample.first; index <= sample.last; ++index)
                            power = std::max(power, source[index]);
                    } else {
                        for (int index = sample.first; index < capacity_; ++index)
                            power = std::max(power, source[index]);
                        for (int index = 0; index <= sample.last; ++index)
                            power = std::max(power, source[index]);
                    }
                } else {
                    power = std::lerp(source[sample.first], source[sample.last], sample.fraction);
                }
            }
            destination[x] = power;
        }
    }

    if (frequencySamples_.size() != height) {
        frequencySamples_.resize(height);
        const double binWidth = double(sampleRate_) / fftSize_;
        const double maximum = std::min(20000.0, sampleRate_ * 0.5);
        const double logarithmicRange = std::log(maximum / MeterScale::kMinFrequency);
        const auto frequencyBin = [&](double position) {
            return MeterScale::kMinFrequency * std::exp(position * logarithmicRange) / binWidth;
        };
        for (int y = 0; y < height; ++y) {
            const double position = height > 1 ? double(height - 1 - y) / (height - 1) : 0.5;
            const double halfPixel = height > 1 ? 0.5 / (height - 1) : 0.5;
            const double center = std::clamp(frequencyBin(position), 0.0, double(bins - 1));
            auto& sample = frequencySamples_[y];
            sample.lower = int(std::floor(center));
            sample.upper = std::min(sample.lower + 1, bins - 1);
            sample.fraction = float(center - sample.lower);
            sample.first = std::max(0, int(std::ceil(frequencyBin(std::max(0.0, position - halfPixel)))));
            sample.last = std::min(bins - 1, int(std::floor(frequencyBin(std::min(1.0, position + halfPixel)))));
        }
    }

    rowPowers_.resize(width);
    float* row = rowPowers_.data();
    for (int y = 0; y < height; ++y) {
        const auto& sample = frequencySamples_.at(y);
        const float* lower = temporal + qsizetype(sample.lower) * width;
        const float* upper = temporal + qsizetype(sample.upper) * width;
        for (int x = 0; x < width; ++x)
            row[x] = std::lerp(lower[x], upper[x], sample.fraction);
        for (int bin = sample.first; bin <= sample.last; ++bin) {
            const float* source = temporal + qsizetype(bin) * width;
            for (int x = 0; x < width; ++x)
                row[x] = std::max(row[x], source[x]);
        }
        auto* pixels = reinterpret_cast<QRgb*>(image_.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const float power = row[x];
            const float db = power > 0.0f ? 10.0f * std::log10(power) : MeterScale::kMinDb;
            const int color = qRound(MeterScale::dbToPosition(db, MeterScale::kSpectrogramMaxDb)
                * (palette.size() - 1));
            pixels[x] = palette[color];
        }
    }
    imageDirty_ = false;
    return image_;
}

}
