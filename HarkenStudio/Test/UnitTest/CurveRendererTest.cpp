#include "Core/BuiltinTheme.h"
#include "Widgets/Common/AxisCrosshair.h"
#include "Widgets/CurveEditor/CurveCanvas.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QPainter>

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

TEST(CurveRenderer, CrosshairRemainsVisibleOnLightAndDarkBackgroundsAndClipsToPlot)
{
    ensureApp();
    const auto style = VNX::createBuiltinDarkTheme().domains.curve;
    const QRect plot(20, 20, 200, 140);
    const QPoint position = plot.center();
    for (const QColor background : {QColor(13, 17, 26), QColor(240, 240, 240)}) {
        QImage image(240, 180, QImage::Format_ARGB32_Premultiplied);
        image.fill(background);
        {
            QPainter painter(&image);
            VNX::drawAxisCrosshairLines(painter, style, QRectF(plot), position);
        }
        int horizontalContrast = 0;
        for (int x = plot.left() + 4; x < plot.right() - 4; ++x)
            horizontalContrast += std::abs(qGray(image.pixel(x, position.y())) - qGray(background.rgb())) > 100;
        int verticalContrast = 0;
        for (int y = plot.top() + 4; y < plot.bottom() - 4; ++y)
            verticalContrast += std::abs(qGray(image.pixel(position.x(), y)) - qGray(background.rgb())) > 100;
        EXPECT_GT(horizontalContrast, plot.width() / 4);
        EXPECT_GT(verticalContrast, plot.height() / 4);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (!plot.contains(x, y))
                    EXPECT_EQ(image.pixelColor(x, y), background);
    }
}

TEST(CurveRenderer, MirroredVolumeMappingAndZeroTickStaySymmetric)
{
    ensureApp();
    VNX::CurveCanvas canvas;
    VNX::CurveCanvasProfile profile;
    profile.axisScale = VNX::CurveAxisScale::MirroredAmplitudeDb;
    profile.yMax = 24.0f;
    canvas.setProfile(profile);
    canvas.setXDomain(0.0f, 7.0f);
    canvas.setCurve({{0.0f, -96.0f}, {1.0f, -24.0f}, {2.0f, -12.0f}, {3.0f, -6.0f},
                     {4.0f, 0.0f}, {5.0f, 6.0f}, {6.0f, 12.0f}, {7.0f, 24.0f}});
    for (const QSize size : {QSize(240, 140), QSize(1000, 700)}) {
        canvas.resize(size);
        const auto data = canvas.renderData();
        ASSERT_EQ(data.points.size(), 8);
        const qreal zero = data.points[4].position.y();
        for (int offset = 1; offset <= 3; ++offset)
            EXPECT_NEAR(data.points[4 - offset].position.y() - zero,
                        zero - data.points[4 + offset].position.y(), 1e-6);
        EXPECT_NEAR(zero, data.graphRect.center().y(), data.graphRect.height() * 0.02);
        bool foundZero = false;
        for (const auto& tick : data.textTicks) {
            if (tick.rect.right() < data.graphRect.left()) {
                bool numeric = false;
                const double value = tick.text.toDouble(&numeric);
                if (numeric && value == 0.0) {
                    foundZero = true;
                    EXPECT_NEAR(tick.rect.center().y(), zero, 1e-6);
                }
            }
        }
        EXPECT_TRUE(foundZero);
    }
}

TEST(CurveRenderer, MirroredVolumeCurveAndOverlayStayStraightAcrossZero)
{
    ensureApp();
    VNX::CurveCanvas canvas;
    VNX::CurveCanvasProfile profile;
    profile.axisScale = VNX::CurveAxisScale::MirroredAmplitudeDb;
    profile.yMax = 24.0f;
    canvas.setProfile(profile);
    canvas.setXDomain(0.0f, 10.0f);
    canvas.resize(600, 420);
    canvas.setCurve({{0.0f, -24.0f}, {10.0f, 24.0f}});
    canvas.setOverlays({{canvas.curve(), canvas.curveHandles()}});
    const auto data = canvas.renderData();
    ASSERT_EQ(data.overlays.size(), 1);
    for (const auto& path : {data.continuousPath, data.overlays.front().path}) {
        ASSERT_FALSE(path.isEmpty());
        for (int i = 0; i < path.elementCount(); ++i) {
            const auto point = path.elementAt(i);
            const double x = (point.x - data.graphRect.left()) / data.graphRect.width();
            const double firstY = data.points.front().position.y();
            const double lastY = data.points.back().position.y();
            EXPECT_NEAR(point.y, firstY + (lastY - firstY) * x, 1e-3);
        }
    }
}
