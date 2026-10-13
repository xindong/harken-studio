#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/BuiltinTheme.h"
#include "Widgets/Common/WaveformRenderer.h"

#include <gtest/gtest.h>

#include <QImage>
#include <QPainter>
#include <QFontMetrics>

#include <algorithm>
#include <cmath>

TEST(WaveformRenderer, FrameRangeCropsEveryChannelAndColorModeWithoutIncludingTrimmedPeaks)
{
    VNX::WaveformPeakData data;
    data.numPeaks = 4;
    data.numChannels = 1;
    data.peaks = {
        -1.0f, 0.75f,
        -0.5f, 0.25f,
        -0.125f, 0.125f,
        0.0f, 0.9f,
    };
    data.bandData = {
        1.0f, 0.2f, 0.1f,
        0.3f, 1.0f, 0.4f,
        0.2f, 0.4f, 1.0f,
        0.8f, 0.8f, 0.2f,
    };
    data.samplesPerPeak = 100;
    data.sampleRate = 1000;
    data.durationSec = 0.4;
    data.numChannels = 2;
    data.peaks.insert(data.peaks.end(), {-0.7f, 0.7f, -0.3f, 0.3f, -0.1f, 0.2f, -1.0f, 1.0f});
    data.bandData.insert(data.bandData.end(), {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1});
    data.midSidePeaks = data.peaks;
    auto trimmed = data;
    trimmed.numPeaks = 2;
    trimmed.peaks.clear();
    trimmed.bandData.clear();
    trimmed.midSidePeaks.clear();
    for (int channel = 0; channel < 2; ++channel) {
        trimmed.peaks.insert(trimmed.peaks.end(), data.peaks.begin() + channel * 8 + 2,
            data.peaks.begin() + channel * 8 + 6);
        trimmed.midSidePeaks.insert(trimmed.midSidePeaks.end(), data.midSidePeaks.begin() + channel * 8 + 2,
            data.midSidePeaks.begin() + channel * 8 + 6);
        trimmed.bandData.insert(trimmed.bandData.end(), data.bandData.begin() + channel * 12 + 3,
            data.bandData.begin() + channel * 12 + 9);
    }
    const auto render = [](const VNX::WaveformPeakData& peaks, const VNX::WaveformRenderOptions& options) {
        QImage image(16, 40, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        VNX::WaveformRenderer::render(painter, image.rect(), peaks,
            VNX::createBuiltinDarkTheme().domains.waveform, options);
        return image;
    };
    for (const auto mode : {VNX::WaveColorMode::Static, VNX::WaveColorMode::MultiBand, VNX::WaveColorMode::ColorMap}) {
        for (const auto channel : {VNX::WaveChannelSource::Mono, VNX::WaveChannelSource::Left,
                VNX::WaveChannelSource::Right, VNX::WaveChannelSource::Mid, VNX::WaveChannelSource::Side}) {
            const VNX::WaveformRenderOptions options{mode, channel, {}};
            auto cropped = options;
            cropped.startFrame = 100;
            cropped.frameCount = 200;
            EXPECT_EQ(render(data, cropped), render(trimmed, options));
            EXPECT_NE(render(data, cropped), render(data, options));
        }
    }
    QImage blank(16, 40, QImage::Format_ARGB32_Premultiplied);
    blank.fill(Qt::transparent);
    VNX::WaveformRenderOptions singlePeak;
    singlePeak.startFrame = 150;
    singlePeak.frameCount = 1;
    EXPECT_NE(render(data, singlePeak), blank);
    auto wider = singlePeak;
    wider.startFrame = 100;
    wider.frameCount = 100;
    EXPECT_EQ(render(data, singlePeak), render(data, wider));
    data.sampleRate = 1000;
    data.durationSec = 0.45;
    singlePeak.startFrame = 420;
    singlePeak.frameCount = 30;
    wider.startFrame = 300;
    wider.frameCount = 150;
    EXPECT_NE(render(data, singlePeak), blank);
    EXPECT_EQ(render(data, singlePeak), render(data, wider));
}

TEST(WaveformRenderer, LinearAmplitudePreservesRelativeHeightAndPolarity)
{
    VNX::WaveformPeakData data;
    data.numPeaks = 4;
    data.numChannels = 1;
    data.samplesPerPeak = 1;
    data.sampleRate = 4;
    data.durationSec = 1;
    data.peaks = {-0.001f, 0.001f, -0.1f, 0.1f, -0.5f, 0.5f, -1.0f, 1.0f};
    auto style = VNX::createBuiltinDarkTheme().domains.waveform;
    style.amplitudeScale = 1;
    style.staticMode.quiet = style.staticMode.peak = Qt::white;
    QImage image(4, 120, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    VNX::WaveformRenderer::render(painter, image.rect(), data, style);
    painter.end();
    EXPECT_EQ(qAlpha(image.pixel(0, 59)), 0);
    EXPECT_GT(qAlpha(image.pixel(0, 60)), 0);
    EXPECT_GT(qAlpha(image.pixel(1, 55)), 0);
    EXPECT_GT(qAlpha(image.pixel(1, 64)), 0);
    EXPECT_EQ(qAlpha(image.pixel(1, 53)), 0);
    EXPECT_EQ(qAlpha(image.pixel(1, 66)), 0);
    EXPECT_GT(qAlpha(image.pixel(2, 31)), 0);
    EXPECT_GT(qAlpha(image.pixel(2, 88)), 0);
    EXPECT_EQ(qAlpha(image.pixel(2, 29)), 0);
    EXPECT_EQ(qAlpha(image.pixel(2, 90)), 0);
    EXPECT_GT(qAlpha(image.pixel(3, 0)), 0);
    EXPECT_GT(qAlpha(image.pixel(3, 119)), 0);
}

TEST(WaveformRenderer, PowerCurveRevealsQuietSignalsInEnvelopesAndSampleCurves)
{
    VNX::WaveformPeakData data;
    data.numPeaks = 2;
    data.numChannels = 1;
    data.samplesPerPeak = 1;
    data.sampleRate = 2;
    data.durationSec = 1;
    data.peaks = {-0.001f, -0.001f, 0.001f, 0.001f};
    const auto originalPeaks = data.peaks;
    auto style = VNX::createBuiltinDarkTheme().domains.waveform;
    style.amplitudeScale = 1.0;
    style.staticMode.quiet = style.staticMode.peak = Qt::white;
    for (bool sampleCurve : {false, true}) {
        const auto render = [&](double exponent) {
            QImage image(100, 400, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            VNX::WaveformRenderOptions options;
            options.sampleCurve = sampleCurve;
            options.frameCount = sampleCurve ? 1 : 2;
            options.amplitudeExponent = exponent;
            VNX::WaveformRenderer::render(painter, image.rect(), data, style, options);
            return image;
        };
        const auto height = [](const QImage& image) {
            int top = image.height();
            int bottom = -1;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qAlpha(image.pixel(x, y)) != 0) {
                        top = std::min(top, y);
                        bottom = std::max(bottom, y);
                    }
            return bottom - top + 1;
        };
        const auto linear = render(1.0);
        const auto power = render(0.3);
        const auto decibels = render(0.0);
        EXPECT_GT(height(power), height(linear));
        EXPECT_GT(height(decibels), height(power));
        EXPECT_EQ(data.peaks, originalPeaks);
    }
}

TEST(WaveformRenderer, ZeroThemeAmplitudeCollapsesSignalsWithoutInvalidCoordinates)
{
    VNX::WaveformPeakData data;
    data.numPeaks = 2;
    data.numChannels = 1;
    data.samplesPerPeak = 1;
    data.sampleRate = 2;
    data.durationSec = 1;
    data.peaks = {-1.0f, -1.0f, 1.0f, 1.0f};
    auto silence = data;
    std::fill(silence.peaks.begin(), silence.peaks.end(), 0.0f);
    auto style = VNX::createBuiltinDarkTheme().domains.waveform;
    style.amplitudeScale = 0.0;
    style.staticMode.quiet = style.staticMode.peak = Qt::white;
    for (double exponent : {0.0, 0.3, 1.0}) {
        const auto scale = VNX::WaveformRenderer::amplitudeScale(style, exponent);
        EXPECT_TRUE(std::isfinite(scale.dbToPosition(0.0)));
        EXPECT_TRUE(std::isfinite(scale.positionToDb(0.5)));
        EXPECT_DOUBLE_EQ(scale.positionToAmplitude(0.5), 0.0);
        EXPECT_TRUE(VNX::buildAmplitudeAxisTicks(QRect(0, 0, 100, 100), 50, 50,
            QFontMetrics(QFont()), 20, scale).isEmpty());
        for (bool sampleCurve : {false, true}) {
            const auto render = [&](const VNX::WaveformPeakData& peaks) {
                QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                VNX::WaveformRenderOptions options;
                options.frameCount = 1;
                options.sampleCurve = sampleCurve;
                options.amplitudeExponent = exponent;
                VNX::WaveformRenderer::render(painter, image.rect(), peaks, style, options);
                return image;
            };
            EXPECT_EQ(render(data), render(silence));
        }
    }
}

TEST(WaveformRenderer, ViewportUsesSampleCoordinatesAndAnExplicitChannel)
{
    VNX::WaveformPeakData data;
    data.numPeaks = 3;
    data.numChannels = 3;
    data.samplesPerPeak = 10;
    data.sampleRate = 100;
    data.durationSec = 0.25;
    data.peaks = {0, 0, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0,
                  -0.9f, -0.9f, 0.25f, 0.25f, 0.75f, 0.75f};
    auto style = VNX::createBuiltinDarkTheme().domains.waveform;
    style.amplitudeScale = 1;
    style.staticMode.quiet = style.staticMode.peak = Qt::white;
    VNX::WaveformRenderOptions options;
    options.channelIndex = 2;
    options.startFrame = 10;
    options.frameCount = 15;
    QImage image(15, 80, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    VNX::WaveformRenderer::render(painter, image.rect(), data, style, options);
    painter.end();
    EXPECT_GT(qAlpha(image.pixel(4, 30)) + qAlpha(image.pixel(4, 31)), 0);
    EXPECT_EQ(qAlpha(image.pixel(4, 10)), 0);
    EXPECT_GT(qAlpha(image.pixel(12, 10)) + qAlpha(image.pixel(12, 11)), 0);
    EXPECT_EQ(qAlpha(image.pixel(12, 40)), 0);
}

TEST(WaveformRenderer, SampleZoomConnectsIndividualSamplesAndMergesChannels)
{
    VNX::WaveformPeakData data;
    data.numPeaks = 3;
    data.numChannels = 3;
    data.samplesPerPeak = 1;
    data.sampleRate = 100;
    data.durationSec = 0.03;
    data.peaks = {0, 0, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0,
                  -0.9f, -0.9f, 0.9f, 0.9f, -0.9f, -0.9f};
    auto style = VNX::createBuiltinDarkTheme().domains.waveform;
    style.amplitudeScale = 1;
    style.staticMode.peak = Qt::white;
    for (bool mergeChannels : {false, true}) {
        VNX::WaveformRenderOptions options;
        options.channelIndex = mergeChannels ? -1 : 2;
        options.frameCount = 2;
        options.sampleCurve = true;
        QImage image(101, 80, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        VNX::WaveformRenderer::render(painter, image.rect(), data, style, options);
        painter.end();
        EXPECT_GT(qAlpha(image.pixel(25, 40)), 0);
        EXPECT_GT(qAlpha(image.pixel(50, mergeChannels ? 28 : 4)), 0);
        EXPECT_GT(qAlpha(image.pixel(75, 40)), 0);
        EXPECT_EQ(qAlpha(image.pixel(25, 60)), 0);
        EXPECT_EQ(qAlpha(image.pixel(50, 40)), 0);
    }
}
