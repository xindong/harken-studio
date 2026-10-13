#pragma once

#include "Core/ThemeSnapshot.h"

#include <QList>

#include <algorithm>
#include <array>
#include <cmath>

namespace VNX {

class MeterColorMap
{
public:
    static QList<QRgb> colorTable(const MeterStyle::Spectrogram::Colors& colors)
    {
        const std::array<QColor, 7> stops = {
            colors.emptyHistory, QColor(0x1A, 0x0A, 0x2E), QColor(0x4A, 0x1A, 0x6E),
            QColor(0xCC, 0x44, 0x22), QColor(0xEE, 0x88, 0x33), QColor(0xFF, 0xCC, 0x44),
            QColor(0xFF, 0xFF, 0xEE)
        };
        const std::array<float, 7> positions = {0.0f, 0.15f, 0.3f, 0.5f, 0.7f, 0.85f, 1.0f};
        QList<QRgb> table;
        table.reserve(256);
        int stop = 0;
        for (int i = 0; i < 256; ++i) {
            const float position = i / 255.0f;
            while (stop < int(stops.size()) - 2 && position > positions[stop + 1])
                ++stop;
            const float fraction = (position - positions[stop]) / (positions[stop + 1] - positions[stop]);
            const QColor& from = stops[stop];
            const QColor& to = stops[stop + 1];
            table.append(QColor::fromRgbF(
                std::lerp(from.redF(), to.redF(), fraction),
                std::lerp(from.greenF(), to.greenF(), fraction),
                std::lerp(from.blueF(), to.blueF(), fraction)).rgb());
        }
        return table;
    }
};

}
