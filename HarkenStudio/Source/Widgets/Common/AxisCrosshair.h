#pragma once

#include "Core/ThemeSnapshot.h"

#include <QAbstractButton>
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

class AxisIndicatorButton final : public QAbstractButton
{
public:
    AxisIndicatorButton(const QString& iconId, QWidget* parent);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent* event) override;
    bool hitButton(const QPoint& position) const override;
};

void drawAxisReadout(QPainter& painter, const CurveStyle& style,
                     const QRectF& bounds, const QString& label);

void drawAxisCrosshairLines(QPainter& painter, const CurveStyle& style,
                           const QRectF& bounds, const QPointF& position,
                           Qt::Orientations axes = Qt::Horizontal | Qt::Vertical);

void drawAxisCrosshair(QPainter& painter, const CurveStyle& style,
                      const QRectF& bounds, const AxisCrosshair& crosshair);

}
