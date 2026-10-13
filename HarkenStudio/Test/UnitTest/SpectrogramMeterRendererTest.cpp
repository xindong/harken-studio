#include "Core/BuiltinTheme.h"
#include "Widgets/Meter/SpectrogramHistory.h"
#include "Widgets/Meter/SpectrogramMeterRenderer.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QList<QRgb> grayscalePalette()
{
    QList<QRgb> colors;
    for (int index = 0; index < 256; ++index)
        colors.append(qRgb(index, index, index));
    return colors;
}

}

TEST(SpectrogramMeterRenderer, FinishedRasterStaysInsidePlot)
{
    ensureApp();
    const auto style = VNX::createBuiltinDarkTheme().domains.meter;
    VNX::SpectrogramMeterRenderFrame frame;
    frame.canvasBounds = QRect(0, 0, 360, 240);
    frame.plotBounds = QRect(40, 30, 80, 40);
    frame.hasPlot = true;
    frame.waterfallImage = QImage(160, 80, QImage::Format_RGB32);
    frame.waterfallImage.fill(Qt::red);
    for (int column = 80; column < frame.waterfallImage.width(); ++column)
        for (int row = 0; row < frame.waterfallImage.height(); ++row)
            frame.waterfallImage.setPixelColor(column, row, Qt::green);
    QImage image(frame.canvasBounds.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        VNX::SpectrogramMeterRenderer::render(painter, style, frame);
    }
    EXPECT_EQ(image.pixelColor(42, 50), QColor(Qt::red));
    EXPECT_EQ(image.pixelColor(118, 50), QColor(Qt::green));
    EXPECT_EQ(image.pixelColor(122, 50), style.spectrogramMeter.colors.canvasBackground);
}

TEST(SpectrogramHistory, RingWrapKeepsNewestAtRightAndSnapshotsSurviveResize)
{
    const auto palette = grayscalePalette();
    VNX::SpectrogramHistory history(48000, 2048, 4);
    std::vector<float> frame(1025);
    for (float db : {-72.0f, -60.0f, -48.0f, -36.0f, -24.0f, -12.0f}) {
        std::fill(frame.begin(), frame.end(), db);
        history.append(frame);
    }
    EXPECT_EQ(history.columns(), 4);
    const QImage original = history.image(QSize(4, 9), 1.0, palette);
    EXPECT_EQ(original.pixelColor(0, 4).red(), 102);
    EXPECT_EQ(original.pixelColor(1, 4).red(), 128);
    EXPECT_EQ(original.pixelColor(2, 4).red(), 153);
    EXPECT_EQ(original.pixelColor(3, 4).red(), 179);
    const auto frozen = history;
    std::fill(frame.begin(), frame.end(), 0.0f);
    history.append(frame);
    EXPECT_EQ(frozen.image(QSize(4, 9), 1.0, palette), original);
    EXPECT_EQ(history.image(QSize(4, 9), 1.0, palette).pixelColor(3, 4).red(), 204);
    const QImage enlarged = frozen.image(QSize(13, 513), 4.0 / 13.0, palette);
    EXPECT_EQ(enlarged.pixelColor(0, 256).red(), 102);
    EXPECT_EQ(enlarged.pixelColor(12, 256).red(), 179);
    EXPECT_EQ(frozen.image(QSize(4, 9), 1.0, palette), original);
    const QImage recent = frozen.image(QSize(2, 9), 1.0, palette);
    EXPECT_EQ(recent.pixelColor(0, 4).red(), 153);
    EXPECT_EQ(recent.pixelColor(1, 4).red(), 179);
    for (float db : {6.0f, 12.0f, 18.0f, 24.0f}) {
        std::fill(frame.begin(), frame.end(), db);
        history.append(frame);
    }
    const QImage live = history.image(QSize(4, 9), 1.0, palette);
    EXPECT_EQ(live.pixelColor(3, 4), QColor(Qt::white));
    EXPECT_EQ(frozen.image(QSize(4, 9), 1.0, palette), original);
    auto reversedPalette = palette;
    std::reverse(reversedPalette.begin(), reversedPalette.end());
    const QImage recolored = history.image(QSize(4, 9), 1.0, reversedPalette);
    for (int x = 0; x < live.width(); ++x)
        EXPECT_EQ(recolored.pixelColor(x, 4).red(), 255 - live.pixelColor(x, 4).red());
    const QImage resized = history.image(QSize(8, 17), 0.5, palette);
    EXPECT_EQ(resized.pixelColor(0, 8), live.pixelColor(0, 4));
    EXPECT_EQ(resized.pixelColor(7, 8), live.pixelColor(3, 4));
    EXPECT_EQ(history.image(QSize(4, 9), 1.0, palette), live);
    EXPECT_EQ(frozen.image(QSize(4, 9), 1.0, palette), original);
}

TEST(SpectrogramHistory, FiniteLevelCalibrationAndPowerInterpolationStayWithinInputRange)
{
    const auto palette = grayscalePalette();
    std::vector<float> frame(1025);
    for (const auto& level : {std::pair{-96.0f, 0}, std::pair{-72.0f, 51},
                             std::pair{-24.0f, 153}, std::pair{0.0f, 204}, std::pair{24.0f, 255}}) {
        VNX::SpectrogramHistory history(48000, 2048, 1);
        std::fill(frame.begin(), frame.end(), level.first);
        history.append(frame);
        const auto& image = history.image(QSize(7, 513), 1.0 / 7.0, palette);
        EXPECT_EQ(image.pixelColor(3, 256).red(), level.second);
    }
    VNX::SpectrogramHistory history(48000, 2048, 2);
    std::fill(frame.begin(), frame.end(), -20.0f);
    history.append(frame);
    std::fill(frame.begin(), frame.end(), 0.0f);
    history.append(frame);
    const QImage midpoint = history.image(QSize(3, 9), 2.0 / 3.0, palette);
    const double interpolatedDb = midpoint.pixelColor(1, 4).red() * 120.0 / 255.0 - 96.0;
    EXPECT_NEAR(interpolatedDb, 10.0 * std::log10(0.505), 0.24);
    const auto& expanded = history.image(QSize(31, 9), 2.0 / 31.0, palette);
    for (int x = 0; x < expanded.width(); ++x) {
        EXPECT_GE(expanded.pixelColor(x, 4).red(), 162);
        EXPECT_LE(expanded.pixelColor(x, 4).red(), 204);
        if (x > 0)
            EXPECT_GE(expanded.pixelColor(x, 4).red(), expanded.pixelColor(x - 1, 4).red());
    }
    VNX::SpectrogramHistory frequencyMidpoint(81920, 2048, 1);
    std::fill(frame.begin(), frame.end(), -20.0f);
    frame[1] = 0.0f;
    frequencyMidpoint.append(frame);
    const auto& interpolated = frequencyMidpoint.image(QSize(1, 513), 1.0, palette);
    const double lowerEdgeDb = interpolated.pixelColor(0, 512).red() * 120.0 / 255.0 - 96.0;
    EXPECT_NEAR(lowerEdgeDb, 10.0 * std::log10(0.505), 0.24);
}

TEST(SpectrogramHistory, TimeCompressionPreservesSingleFramePulses)
{
    const auto palette = grayscalePalette();
    for (int pulse : {0, 5, 15}) {
        VNX::SpectrogramHistory history(48000, 2048, 16);
        std::vector<float> frame(1025);
        for (int column = 0; column < 16; ++column) {
            std::fill(frame.begin(), frame.end(), column == pulse ? 0.0f : -96.0f);
            history.append(frame);
        }
        const auto& image = history.image(QSize(4, 1), 4.0, palette);
        for (int x = 0; x < image.width(); ++x)
            EXPECT_EQ(image.pixelColor(x, 0).red(), x == pulse / 4 ? 204 : 0);
    }
}

TEST(SpectrogramHistory, FixedTimeScaleKeepsRightOverlapAcrossResizeAndRingWrap)
{
    const auto palette = grayscalePalette();
    for (int frames : {5, 80}) {
        VNX::SpectrogramHistory history(48000, 2048, 64);
        std::vector<float> frame(1025);
        for (int column = 0; column < frames; ++column) {
            std::fill(frame.begin(), frame.end(), -72.0f + (column % 11) * 6.0f);
            history.append(frame);
        }
        for (double columnsPerPixel : {0.65, 1.7}) {
            SCOPED_TRACE(::testing::Message() << "frames=" << frames << " scale=" << columnsPerPixel);
            const QImage wide = history.image(QSize(47, 33), columnsPerPixel, palette);
            for (int width : {17, 9}) {
                const QImage narrow = history.image(QSize(width, 33), columnsPerPixel, palette);
                EXPECT_EQ(narrow, wide.copy(wide.width() - width, 0, width, wide.height()));
            }
            const int emptyColumns = std::max(0,
                int(std::floor(wide.width() - history.columns() / columnsPerPixel)));
            for (int x = 0; x < emptyColumns; ++x)
                EXPECT_EQ(wide.pixelColor(x, 16).red(), 0);
            EXPECT_GT(wide.pixelColor(wide.width() - 1, 16).red(), 0);
        }
    }
}

TEST(SpectrogramHistory, SpeedScaleChangesPulseDistanceWithoutChangingHistory)
{
    const auto palette = grayscalePalette();
    VNX::SpectrogramHistory history(48000, 2048, 32);
    std::vector<float> frame(1025);
    for (int column = 0; column < 32; ++column) {
        std::fill(frame.begin(), frame.end(), column == 24 ? 0.0f : -96.0f);
        history.append(frame);
    }
    const auto pulseCenter = [](const QImage& image) {
        int maximum = -1;
        int first = 0;
        int last = 0;
        for (int x = 0; x < image.width(); ++x) {
            const int intensity = image.pixelColor(x, 0).red();
            if (intensity > maximum) {
                maximum = intensity;
                first = x;
            }
            if (intensity == maximum)
                last = x;
        }
        EXPECT_GT(maximum, 190);
        return (first + last) / 2.0;
    };
    const QImage normal = history.image(QSize(160, 1), 1.0, palette);
    const double normalDistance = normal.width() - 0.5 - pulseCenter(normal);
    for (double speed : {2.0, 10.0}) {
        const QImage faster = history.image(normal.size(), 1.0 / speed, palette);
        EXPECT_NEAR(faster.width() - 0.5 - pulseCenter(faster), normalDistance * speed, 0.5);
    }
    EXPECT_EQ(history.columns(), 32);
    EXPECT_EQ(history.image(normal.size(), 1.0, palette), normal);
}

TEST(SpectrogramHistory, FrequencyPositionsAndNearbyTonesSurvivePhysicalResize)
{
    const auto palette = grayscalePalette();
    for (uint32_t sampleRate : {32000u, 48000u, 96000u}) {
        const double binWidth = double(sampleRate) / 2048.0;
        const double maximumFrequency = std::min(20000.0, sampleRate / 2.0);
        for (double target : {100.0, 1000.0, 12000.0}) {
            const int bin = int(std::lround(target / binWidth));
            const double frequency = bin * binWidth;
            VNX::SpectrogramHistory history(sampleRate, 2048, 1);
            std::vector<float> frame(1025, -96.0f);
            frame[bin] = -6.0f;
            history.append(frame);
            for (int height : {160, 321, 1025}) {
                SCOPED_TRACE(::testing::Message() << "rate=" << sampleRate << " tone=" << frequency << " height=" << height);
                const auto& image = history.image(QSize(1, height), 1.0, palette);
                int maximum = -1;
                int first = 0;
                int last = 0;
                for (int row = 0; row < height; ++row) {
                    const int intensity = image.pixelColor(0, row).red();
                    if (intensity > maximum) {
                        maximum = intensity;
                        first = row;
                    }
                    if (intensity == maximum)
                        last = row;
                }
                const double expectedY = (height - 1) * std::log(maximumFrequency / frequency)
                    / std::log(maximumFrequency / 20.0);
                EXPECT_NEAR((first + last) / 2.0, expectedY, 1.5);
                EXPECT_NEAR(maximum, 191, 2);
            }
        }
    }
    VNX::SpectrogramHistory nearby(48000, 2048, 1);
    std::vector<float> frame(1025, -96.0f);
    frame[405] = -6.0f;
    frame[414] = -6.0f;
    nearby.append(frame);
    const auto& image = nearby.image(QSize(1, 2049), 1.0, palette);
    const int lowerToneY = int(std::lround(2048.0 * std::log(20000.0 / (405 * 48000.0 / 2048.0)) / std::log(1000.0)));
    const int upperToneY = int(std::lround(2048.0 * std::log(20000.0 / (414 * 48000.0 / 2048.0)) / std::log(1000.0)));
    EXPECT_NEAR(image.pixelColor(0, lowerToneY).red(), 191, 1);
    EXPECT_NEAR(image.pixelColor(0, upperToneY).red(), 191, 1);
    EXPECT_EQ(image.pixelColor(0, (lowerToneY + upperToneY) / 2).red(), 0);
}
