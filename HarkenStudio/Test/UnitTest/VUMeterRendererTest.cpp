#include "Core/BuiltinTheme.h"
#include "Widgets/Meter/VUMeterRenderer.h"

#include <gtest/gtest.h>
#include "Core/FontManager.h"

#include <QApplication>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QFont testFont(int pixelSize)
{
    QFont font;
    font.setPixelSize(pixelSize);
    return font;
}

VNX::VUMeterHeaderFrame headerFrame()
{
    VNX::VUMeterHeaderFrame header;
    header.canvasBounds = QRect(0, 0, 400, 300);
    header.titleBounds = QRect(4, 3, 392, 18);
    header.title = QStringLiteral("VU Meter");
    header.titleFont = testFont(11);
    return header;
}

std::array<QRect, 4> analogBounds()
{
    return {
        QRect(4, 23, 193, 134),
        QRect(203, 23, 193, 134),
        QRect(4, 163, 193, 134),
        QRect(203, 163, 193, 134),
    };
}

std::array<QRect, 4> barBounds()
{
    return {
        QRect(4, 23, 92, 274),
        QRect(104, 23, 92, 274),
        QRect(204, 23, 92, 274),
        QRect(304, 23, 92, 274),
    };
}

VNX::VUAnalogMeterRenderFrame analogFrame()
{
    VNX::VUAnalogMeterRenderFrame frame;
    frame.header = headerFrame();
    frame.dialCount = 4;
    frame.valueFont = testFont(9);
    frame.labelFont = testFont(10);
    const float marks[] = {-20.0f, -10.0f, -7.0f, -5.0f, -3.0f,
                           0.0f,   1.0f,   2.0f,  3.0f};
    const char* labels[] = {"-20", "-10", "-7", "-5", "-3",
                            "0", "+1", "+2", "+3"};
    const auto position = [](float db) {
        return (db + 20.0f) / 23.0f;
    };
    for (int index = 0; index < 9; ++index) {
        frame.ticks.append({position(marks[index]),
                            QString::fromLatin1(labels[index]),
                            marks[index] >= 0.0f});
    }
    frame.overloadStartPosition = position(0.0f);
    frame.overloadEndPosition = position(3.0f);

    const auto bounds = analogBounds();
    const float levels[] = {-20.0f, -7.0f, 0.0f, 3.0f};
    const char* channels[] = {"L", "R", "Mid", "Side"};
    for (int index = 0; index < 4; ++index) {
        frame.dials[index].bounds = bounds[index];
        frame.dials[index].needlePosition = position(levels[index]);
        frame.dials[index].valueLabel =
            QString("%1 dB").arg(levels[index], 0, 'f', 1);
        frame.dials[index].channelLabel = QString::fromLatin1(channels[index]);
    }
    return frame;
}

VNX::VUBarMeterRenderFrame barFrame()
{
    VNX::VUBarMeterRenderFrame frame;
    frame.header = headerFrame();
    frame.barCount = 4;
    frame.valueFont = testFont(9);
    frame.labelFont = testFont(10);
    const auto bounds = barBounds();
    const float levels[] = {-50.0f, -20.0f, 0.0f, 3.0f};
    const char* channels[] = {"L", "R", "Mid", "Side"};
    for (int index = 0; index < 4; ++index) {
        VNX::VUBarFrame& bar = frame.bars[index];
        bar.bounds = bounds[index];
        const auto y = [&bar](float db) {
            const float normalized = std::clamp((db + 50.0f) / 53.0f,
                                                0.0f,
                                                1.0f);
            return bar.bounds.bottom() - normalized * bar.bounds.height();
        };
        bar.levelTop = y(levels[index]);
        for (float db : {3.0f, 0.0f, -3.0f, -10.0f, -20.0f, -30.0f})
            bar.gridLines.append(y(db));
        bar.valueLabel = QString::number(levels[index], 'f', 1);
        bar.channelLabel = QString::fromLatin1(channels[index]);
    }
    return frame;
}

float angle(qreal position)
{
    const float start = kPi * 0.78f;
    const float end = kPi * 0.22f;
    return start - static_cast<float>(position) * (start - end);
}

QPointF polar(float centerX, float centerY, float radius, float value)
{
    return QPointF(centerX + std::cos(value) * radius,
                   centerY - std::sin(value) * radius);
}

QPainterPath arcPath(float centerX,
                     float centerY,
                     float radius,
                     float start,
                     float end,
                     int segments)
{
    QPainterPath path;
    for (int index = 0; index <= segments; ++index) {
        const float t = static_cast<float>(index) / segments;
        const QPointF point = polar(centerX,
                                    centerY,
                                    radius,
                                    start - t * (start - end));
        if (index == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    return path;
}

void legacyHeader(QPainter& painter, const VNX::VUMeterHeaderFrame& frame)
{
    painter.fillRect(frame.canvasBounds, QColor(0x10, 0x15, 0x1F));
    painter.setPen(QColor(0xE0, 0xE0, 0xE0));
    painter.setFont(frame.titleFont);
    painter.drawText(frame.titleBounds,
                     Qt::AlignLeft | Qt::AlignVCenter,
                     frame.title);
}

void legacyAnalogDial(QPainter& painter,
                      const VNX::VUAnalogMeterRenderFrame& frame,
                      const VNX::VUAnalogDialFrame& dial)
{
    const QRect& area = dial.bounds;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x0F, 0x13, 0x18));
    painter.drawRoundedRect(area, 6.0, 6.0);

    const QRect labelStrip(area.left(), area.bottom() - 30, area.width(), 30);
    const QRect inner = area.adjusted(6, 4, -6, -34);
    const float centerX = static_cast<float>(inner.center().x());
    const float centerY = static_cast<float>(inner.bottom()) - 10.0f;
    const float radius = std::min(inner.width(), inner.height()) * 0.62f;
    painter.setPen(QPen(QColor(0x31, 0x38, 0x48), 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(arcPath(centerX, centerY, radius, angle(0.0), angle(1.0), 48));

    painter.setFont(VNX::FontManager::instance().getFont(VNX::FontRole::Axis));
    for (const VNX::VUAnalogTickFrame& tick : frame.ticks) {
        const float tickAngle = angle(tick.normalizedPosition);
        const float cosine = std::cos(tickAngle);
        const float sine = std::sin(tickAngle);
        const QColor color = tick.overload
            ? QColor(0xF4, 0x43, 0x36, 178)
            : QColor(0xAA, 0xAA, 0xAA);
        painter.setPen(QPen(color, 1.0));
        painter.drawLine(QPointF(centerX + cosine * radius * 0.85f,
                                 centerY - sine * radius * 0.85f),
                         QPointF(centerX + cosine * radius * 0.95f,
                                 centerY - sine * radius * 0.95f));
        painter.setPen(QColor(0xAA, 0xAA, 0xAA));
        painter.drawText(QRectF(centerX + cosine * radius * 0.75f - 12.0,
                                centerY - sine * radius * 0.75f - 5.0,
                                24.0,
                                10.0),
                         Qt::AlignCenter,
                         tick.text);
    }

    painter.setPen(QPen(QColor(0xF4, 0x43, 0x36, 102), 3.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(arcPath(centerX,
                             centerY,
                             radius * 0.92f,
                             angle(frame.overloadStartPosition),
                             angle(frame.overloadEndPosition),
                             12));

    const QPointF pivot(centerX, centerY);
    const QPointF needle = polar(centerX,
                                 centerY,
                                 radius * 0.88f,
                                 angle(dial.needlePosition));
    painter.setPen(QPen(QColor(0x00, 0xF0, 0xFF, 38), 3.5));
    painter.drawLine(pivot, needle);
    painter.setPen(QPen(QColor(0xE0, 0xE0, 0xE0, 229), 1.5));
    painter.drawLine(pivot, needle);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x00, 0xF0, 0xFF));
    painter.drawEllipse(pivot, 4.0, 4.0);

    painter.setPen(QColor(0x66, 0x66, 0x66));
    painter.setFont(frame.valueFont);
    painter.drawText(QRect(labelStrip.left(), labelStrip.top(), labelStrip.width(), 14),
                     Qt::AlignCenter,
                     dial.valueLabel);
    painter.setPen(QColor(0xE0, 0xE0, 0xE0));
    painter.setFont(frame.labelFont);
    painter.drawText(QRect(labelStrip.left(), labelStrip.top() + 14,
                           labelStrip.width(), 16),
                     Qt::AlignCenter,
                     dial.channelLabel);
}

QImage legacyAnalog(const VNX::VUAnalogMeterRenderFrame& frame)
{
    QImage image(400, 300, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    legacyHeader(painter, frame.header);
    for (int index = 0; index < frame.dialCount; ++index)
        legacyAnalogDial(painter, frame, frame.dials[index]);
    return image;
}

void legacyBar(QPainter& painter,
               const VNX::VUBarMeterRenderFrame& frame,
               const VNX::VUBarFrame& bar)
{
    const QRect& area = bar.bounds;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x0F, 0x13, 0x18));
    painter.drawRoundedRect(area, 3.0, 3.0);
    const QRectF meterRect(area.left() + 3,
                           bar.levelTop,
                           area.width() - 6,
                           area.bottom() - bar.levelTop);
    QLinearGradient gradient(0, area.top(), 0, area.bottom());
    gradient.setColorAt(0.0, QColor(0xFF, 0x17, 0x44, 229));
    gradient.setColorAt(0.2, QColor(0xFF, 0x98, 0x00));
    gradient.setColorAt(0.4, QColor(0xFF, 0xD6, 0x00));
    gradient.setColorAt(0.6, QColor(0x00, 0xC8, 0x53).lighter(115));
    gradient.setColorAt(1.0, QColor(0x00, 0xC8, 0x53));
    painter.fillRect(meterRect, gradient);

    painter.setPen(QColor(0x31, 0x38, 0x48));
    for (qreal y : bar.gridLines)
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    painter.setPen(QColor(0xE0, 0xE0, 0xE0));
    painter.setFont(frame.labelFont);
    painter.drawText(QRect(area.left(), area.bottom() - 14, area.width(), 14),
                     Qt::AlignCenter,
                     bar.channelLabel);
    painter.setPen(QColor(0x66, 0x66, 0x66));
    painter.setFont(frame.valueFont);
    painter.drawText(QRect(area.left(), area.bottom() - 26, area.width(), 12),
                     Qt::AlignCenter,
                     bar.valueLabel);
}

QImage legacyBars(const VNX::VUBarMeterRenderFrame& frame)
{
    QImage image(400, 300, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    legacyHeader(painter, frame.header);
    for (int index = 0; index < frame.barCount; ++index)
        legacyBar(painter, frame, frame.bars[index]);
    return image;
}

QImage currentAnalog(const VNX::VUAnalogMeterRenderFrame& frame,
                     const VNX::MeterStyle& style)
{
    QImage image(400, 300, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    VNX::VUMeterRenderer::renderAnalog(painter, style, frame);
    return image;
}

QImage currentBars(const VNX::VUBarMeterRenderFrame& frame,
                   const VNX::MeterStyle& style)
{
    QImage image(400, 300, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    VNX::VUMeterRenderer::renderBars(painter, style, frame);
    return image;
}

}

TEST(VUMeterRenderer, BuiltinModesMatchLegacyPixels)
{
    ensureApp();
    const VNX::MeterStyle& style = VNX::createBuiltinDarkTheme().domains.meter;
    const VNX::VUAnalogMeterRenderFrame analog = analogFrame();
    const VNX::VUBarMeterRenderFrame bars = barFrame();
    EXPECT_TRUE(currentAnalog(analog, style) == legacyAnalog(analog));
    EXPECT_TRUE(currentBars(bars, style) == legacyBars(bars));
}

TEST(VUMeterRenderer, StyleOverridesDriveBothModes)
{
    ensureApp();
    VNX::MeterStyle style = VNX::createBuiltinDarkTheme().domains.meter;
    const VNX::VUAnalogMeterRenderFrame analog = analogFrame();
    const VNX::VUBarMeterRenderFrame bars = barFrame();
    const QImage builtinAnalog = currentAnalog(analog, style);
    const QImage builtinBars = currentBars(bars, style);
    style.vuMeter.colors.canvasBackground = QColor(0x74, 0x15, 0xCE);
    style.vuMeter.colors.analogNeedle = QColor(0xF1, 0x20, 0xA4);
    style.vuMeter.metrics.analogNeedleWidth = 4.0;
    style.red = QColor(0x16, 0xE8, 0xA2);
    style.vuMeter.metrics.barHorizontalInset = 8.0;
    const QImage overriddenAnalog = currentAnalog(analog, style);
    const QImage overriddenBars = currentBars(bars, style);
    EXPECT_NE(builtinAnalog, overriddenAnalog);
    EXPECT_NE(builtinBars, overriddenBars);
    EXPECT_EQ(overriddenAnalog.pixelColor(2, 2),
              style.vuMeter.colors.canvasBackground);
    EXPECT_EQ(overriddenBars.pixelColor(2, 2),
              style.vuMeter.colors.canvasBackground);
}
