#include "Core/BuiltinTheme.h"
#include "Widgets/Meter/SpectrumMeterRenderer.h"

#include <gtest/gtest.h>

#include <QApplication>
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

QImage currentRender(const VNX::SpectrumMeterRenderFrame& frame,
                     const VNX::MeterStyle& style)
{
    QImage image(360, 240, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    VNX::SpectrumMeterRenderer::render(painter, style, frame);
    return image;
}

}

TEST(SpectrumMeterRenderer, CurveMappingPreservesSignalValuesAndHeadroom)
{
    ensureApp();
    const auto theme = VNX::createBuiltinDarkTheme();
    VNX::SpectrumMeterRenderFrame frame;
    frame.canvasBounds = QRect(0, 0, 360, 240);
    frame.plotBounds = QRect(40, 30, 280, 180);
    frame.hasPlot = true;
    frame.amplitudeAxis.maximumDb = 6.0;
    for (double exponent : {0.0, 0.3, 1.0}) {
        frame.amplitudeAxis.exponent = exponent;
        for (double db : {-48.0, 0.0}) {
            const float amplitude = float(std::pow(10.0, db / 20.0));
            frame.smoothedLevels.fill(amplitude);
            frame.peakLevels.fill(amplitude);
            const QImage image = currentRender(frame, theme.domains.meter);
            const int signalY = int(std::lround(frame.plotBounds.bottom()
                - (frame.plotBounds.height() - 1) * frame.amplitudeAxis.dbToPosition(db)));
            EXPECT_NE(image.pixelColor(100, signalY),
                theme.domains.meter.spectrumMeter.colors.canvasBackground);
            EXPECT_EQ(image.pixelColor(100, std::max(frame.plotBounds.top() + 2, signalY - 12)),
                theme.domains.meter.spectrumMeter.colors.canvasBackground);
        }
    }
}

TEST(SpectrumMeterRenderer, CurveAndGlowStayInsidePlotAtRangeLimits)
{
    ensureApp();
    const auto style = VNX::createBuiltinDarkTheme().domains.meter;
    for (float level : {0.0f, 1.0f, 2.0f}) {
        VNX::SpectrumMeterRenderFrame frame;
        frame.canvasBounds = QRect(0, 0, 360, 240);
        frame.plotBounds = QRect(40, 30, 280, 180);
        frame.hasPlot = true;
        frame.smoothedLevels.fill(level);
        frame.peakLevels.fill(level);
        const QImage image = currentRender(frame, style);
        EXPECT_EQ(image.pixelColor(100, 28), style.spectrumMeter.colors.canvasBackground);
        EXPECT_EQ(image.pixelColor(100, 211), style.spectrumMeter.colors.canvasBackground);
        EXPECT_EQ(image.pixelColor(38, 100), style.spectrumMeter.colors.canvasBackground);
        EXPECT_EQ(image.pixelColor(321, 100), style.spectrumMeter.colors.canvasBackground);
    }
}
