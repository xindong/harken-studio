#include "Widgets/Meter/SpectrogramHistory.h"
#include "Widgets/Meter/MeterColorMap.h"
#include "Widgets/Meter/MeterScale.h"
#include "Widgets/Meter/SpectrogramMeterRenderer.h"
#include "Core/BuiltinTheme.h"

#include <gtest/gtest.h>

#include <QImage>
#include <QPainter>

#include <algorithm>
#include <vector>

namespace {

QList<QRgb> grayscalePalette()
{
    QList<QRgb> colors;
    for (int index = 0; index < 256; ++index)
        colors.append(qRgb(index, index, index));
    return colors;
}

}

TEST(SpectrogramHistory, SnapshotSurvivesLaterAppend)
{
    const auto palette = grayscalePalette();
    VNX::SpectrogramHistory history(48000, 2048, 4);
    std::vector<float> frame(1025);
    for (float db : {-72.0f, -60.0f, -48.0f, -36.0f, -24.0f, -12.0f}) {
        std::fill(frame.begin(), frame.end(), db);
        history.append(frame);
    }
    const QImage original = history.image(QSize(4, 9), 1.0, palette);
    const auto frozen = history;
    std::fill(frame.begin(), frame.end(), 0.0f);
    history.append(frame);
    EXPECT_EQ(frozen.image(QSize(4, 9), 1.0, palette), original);
    EXPECT_NE(history.image(QSize(4, 9), 1.0, palette), original);
}

TEST(SpectrogramHistory, PaletteUpdatesPreserveRgbaForEmptyAndSampledPixels)
{
    VNX::SpectrogramHistory history(48000, 2048, 2);
    std::vector<float> frame(1025, VNX::MeterScale::kMinDb);
    history.append(frame);
    std::fill(frame.begin(), frame.end(), VNX::MeterScale::kSpectrogramMaxDb);
    history.append(frame);
    for (int alpha : {255, 0, 64, 128, 255}) {
        SCOPED_TRACE(alpha);
        const QList<QRgb> palette{qRgba(42, 86, 130, alpha), qRgba(170, 110, 50, 255 - alpha)};
        const auto& image = history.image(QSize(3, 8), 1.0, palette);
        for (int y = 0; y < image.height(); ++y) {
            EXPECT_EQ(image.pixelColor(0, y), QColor::fromRgba(palette.front()));
            EXPECT_EQ(image.pixelColor(1, y), QColor::fromRgba(palette.front()));
            EXPECT_EQ(image.pixelColor(2, y), QColor::fromRgba(palette.back()));
        }
    }
}

TEST(SpectrogramMeterRenderer, HistoryColorsComposeOnceOverCanvas)
{
    auto style = VNX::createBuiltinDarkTheme().domains.meter;
    auto& colors = style.spectrogramMeter.colors;
    colors.canvasBackground = QColor(24, 48, 72);
    colors.grid = Qt::transparent;
    VNX::SpectrogramHistory history(48000, 2048, 4);
    VNX::SpectrogramMeterRenderFrame frame;
    frame.canvasBounds = QRect(0, 0, 8, 10);
    frame.plotBounds = QRect(1, 1, 6, 8);
    frame.hasPlot = true;
    const auto render = [&] {
        QImage image(frame.canvasBounds.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        VNX::SpectrogramMeterRenderer::render(painter, style, frame);
        return image;
    };
    for (int alpha : {255, 0, 64, 128, 255}) {
        SCOPED_TRACE(alpha);
        colors.emptyHistory = QColor(180, 90, 30, alpha);
        const auto palette = VNX::MeterColorMap::colorTable(colors);
        frame.waterfallImage = {};
        QImage expected(frame.canvasBounds.size(), QImage::Format_ARGB32_Premultiplied);
        expected.fill(colors.canvasBackground);
        {
            QPainter painter(&expected);
            painter.fillRect(frame.plotBounds, colors.emptyHistory);
        }
        EXPECT_EQ(render(), expected);
        frame.waterfallImage = history.image(frame.plotBounds.size(), 1.0, palette);
        EXPECT_EQ(render(), expected);
        history.append(std::vector<float>(1025, VNX::MeterScale::kMinDb));
        frame.waterfallImage = history.image(frame.plotBounds.size(), 1.0, palette);
        EXPECT_EQ(render(), expected);
    }
}
