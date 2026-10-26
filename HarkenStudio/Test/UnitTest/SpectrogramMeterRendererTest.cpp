#include "Widgets/Meter/SpectrogramHistory.h"

#include <gtest/gtest.h>

#include <QImage>

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
