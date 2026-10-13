#pragma once

#include "Core/ThemeSnapshot.h"

#include <QPointF>
#include <QRectF>
#include <QString>

class QPainter;

namespace VNX {

struct AxisCrosshair
{
    bool visible = false;
    QPointF position;
    QString label;
};

void drawAxisCrosshairLines(QPainter& painter, const CurveStyle& style,
                           const QRectF& bounds, const QPointF& position,
                           Qt::Orientations axes = Qt::Horizontal | Qt::Vertical);

void drawAxisCrosshair(QPainter& painter, const CurveStyle& style,
                      const QRectF& bounds, const AxisCrosshair& crosshair);

}
