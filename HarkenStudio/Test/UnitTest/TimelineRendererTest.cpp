#include "Core/BuiltinTheme.h"
#include "Core/FontManager.h"
#include "Widgets/Timeline/TimelineRenderer.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <cmath>

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

}

TEST(TimelineRenderer, TrackWaveformUsesVisibleSourceFramesAndClipsToAmplitudeViewport)
{
    ensureApp();
    const auto theme = VNX::createBuiltinDarkTheme();
    VNX::TimelineStyleView style;
    style.timeline = theme.domains.timeline;
    style.metrics = theme.ui.metrics;
    style.waveform = theme.domains.waveform;
    VNX::WaveformPeakData peaks;
    peaks.numPeaks = 4;
    peaks.numChannels = 1;
    peaks.sampleRate = 8;
    peaks.samplesPerPeak = 2;
    peaks.durationSec = 1.0;
    peaks.peaks = {-0.1f, 0.1f, -0.1f, 0.1f, -0.8f, 0.8f, -0.8f, 0.8f};
    VNX::TimelineClipItem item;
    item.rect = QRect(0, 0, 100, 100);
    item.waveform = &peaks;
    item.waveformViewport = QRect(2, 20, 96, 76);
    item.waveformRect = item.waveformViewport;
    item.waveformFrameCount = 4.0;
    auto render = [&](double start) {
        item.waveformStartFrame = start;
        VNX::TimelineRenderData data;
        data.clipItems.append(item);
        QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        VNX::TimelineRenderer::render(painter, style, data);
        return image;
    };
    const auto quiet = render(0.0);
    const auto loud = render(4.0);
    EXPECT_NE(quiet.pixelColor(50, 40), loud.pixelColor(50, 40));
    EXPECT_EQ(quiet.pixelColor(50, 10), loud.pixelColor(50, 10));
    item.waveformRect = {};
    const auto beyondSource = render(8.0);
    item.waveform = nullptr;
    EXPECT_EQ(beyondSource, render(8.0));
}

TEST(TimelineRenderer, AmplitudeLabelsStaySpacedAndPreserveVisibleZeroAcrossCenteredZoom)
{
    ensureApp();
    const auto theme = VNX::createBuiltinDarkTheme();
    const QFontMetrics font(VNX::FontManager::instance().getFont(VNX::FontRole::Axis));
    for (int height : {1, 8, 16, 24, 32, 40, 48, 60, 80, 81, 120, 121, 240, 241, 360, 361, 640, 641, 1000}) {
        for (double zoom : {1.0, 2.0, 32.0}) {
            SCOPED_TRACE(::testing::Message() << "height=" << height << " zoom=" << zoom);
            const QRect viewport(0, 30, 640, height);
            const int scroll = (int(std::lround(height * zoom)) - height) / 2;
            for (double exponent : {0.0, 0.3, 1.0}) {
                const auto ticks = VNX::TimelineRenderer::amplitudeTicks(viewport, zoom, scroll, theme, exponent);
                QVector<VNX::AmplitudeAxisTick> labeled;
                for (const auto& tick : ticks)
                    if (!tick.label.isEmpty()) labeled.append(tick);
                for (int i = 1; i < labeled.size(); ++i)
                    EXPECT_GE(labeled[i].y - labeled[i - 1].y, font.height());
                const QRect wave = VNX::waveformAmplitudeBounds(viewport, int(std::lround(height * zoom)), scroll, font);
                const int centerY = wave.top() + wave.height() / 2;
                const VNX::AmplitudeScale scale{-96.0,
                    -20.0 * std::log10(theme.domains.waveform.amplitudeScale), exponent};
                const double offset = scale.dbToPosition(0.0) * (wave.height() / 2);
                const bool zeroInView = (centerY - offset >= viewport.top() && centerY - offset <= viewport.bottom())
                    || (centerY + offset >= viewport.top() && centerY + offset <= viewport.bottom());
                if (wave.height() >= 2 && zeroInView) {
                    EXPECT_TRUE(std::any_of(labeled.begin(), labeled.end(), [](const auto& tick) {
                        return tick.db == 0.0;
                    }));
                }
            }
        }
    }
}

TEST(TimelineRenderer, TrackTickLabelsStayWithinRulerBounds)
{
    ensureApp();
    const auto theme = VNX::createBuiltinDarkTheme();
    VNX::TimeAxisTicks ticks;
    VNX::TimeAxisTick tick;
    tick.pixel = 90;
    tick.major = true;
    tick.label = QStringLiteral("123456");
    ticks.ticks.append(tick);
    QImage image(180, 30, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        VNX::drawTimeAxisRuler(painter, theme.ui.colors, QRect(20, 0, 100, 30), ticks);
    }
    int painted = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (x < 20 || x >= 120)
                EXPECT_EQ(image.pixelColor(x, y).alpha(), 0);
            else if (image.pixelColor(x, y).alpha() != 0)
                ++painted;
        }
    }
    EXPECT_GT(painted, 0);
}
