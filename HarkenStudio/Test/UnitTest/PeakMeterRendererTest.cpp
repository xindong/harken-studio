#include "Core/BuiltinTheme.h"
#include "Widgets/Meter/PeakMeterRenderer.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QPainter>

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

TEST(PeakMeterRenderer, HoldHaloStaysWithinTheBarAtRangeLimits)
{
    ensureApp();
    const auto style = VNX::createBuiltinDarkTheme().domains.meter;
    for (const bool top : {false, true}) {
        VNX::PeakMeterRenderFrame frame;
        frame.canvasBounds = QRect(0, 0, 240, 300);
        frame.barCount = 1;
        auto& bar = frame.bars[0];
        bar.bounds = QRect(20, 30, 100, 180);
        bar.showHold = true;
        bar.holdY = top ? bar.bounds.top() : bar.bounds.bottom();
        QImage image(frame.canvasBounds.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            VNX::PeakMeterRenderer::render(painter, style, frame);
        }
        EXPECT_EQ(image.pixelColor(50, 28), style.peakMeter.colors.canvasBackground);
        EXPECT_EQ(image.pixelColor(50, 211), style.peakMeter.colors.canvasBackground);
        EXPECT_NE(image.pixelColor(50, int(bar.holdY)), style.peakMeter.colors.trackBackground);
    }
}
