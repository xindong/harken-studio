#pragma once

#include "Core/ThemeSnapshot.h"
#include "Widgets/Common/AxisCrosshair.h"

#include <QFont>
#include <QLineF>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <optional>

class QPainter;

namespace VNX {

enum class CurveAxisBound { None, XMin, XMax, YMin, YMax };

struct CurveRenderTextTick
{
    enum class Placement {
        Point,
        Rect
    };

    Placement placement = Placement::Point;
    QPointF point;
    QRectF rect;
    Qt::Alignment alignment = Qt::AlignLeft;
    QString text;
    CurveAxisBound bound = CurveAxisBound::None;
    bool editable = false;
};

struct CurveRenderPoint
{
    QPointF position;
    bool selected = false;
    bool hovered = false;
};

struct CurveRenderHandle
{
    QPointF anchor;
    QPointF position;
    bool active = false;
};

struct CurveRenderGhost
{
    bool visible = false;
    QPointF from;
    QPointF to;
};

struct CurveRenderLasso
{
    bool visible = false;
    QRectF rect;
};

using CurveRenderCrosshair = AxisCrosshair;

struct CurveRenderOverlay
{
    QPainterPath path;
    std::optional<QPainterPath> discretePath;
    CurveColorRole colorRole = CurveColorRole::ParameterBinding;
    QColor color;
    bool selectable = false;
};

struct CurveRenderPlayhead
{
    bool visible = false;
    qreal x = 0.0;
};

struct CurveRenderData
{
    bool frameVisible = true;
    bool clipToPlot = false;
    QRectF canvasRect;
    QRectF graphRect;
    QFont tickFont;
    QString xAxisLabel;
    QString yAxisLabel;
    QRectF xAxisLabelRect;
    QRectF yAxisLabelRect;
    QVector<QLineF> gridLines;
    QVector<CurveRenderTextTick> textTicks;
    QPainterPath continuousPath;
    QColor curveColor;
    std::optional<QPainterPath> discretePath;
    QVector<CurveRenderOverlay> overlays;
    QVector<CurveRenderPoint> points;
    QVector<CurveRenderHandle> handles;
    CurveRenderGhost ghost;
    CurveRenderLasso lasso;
    CurveRenderCrosshair crosshair;
    CurveRenderPlayhead playhead;
};

class CurveRenderer
{
public:
    static void renderBackground(QPainter& painter, const CurveStyle& style, const CurveRenderData& data);
    static void renderForeground(QPainter& painter, const CurveStyle& style,
                                 CurveColorRole colorRole, const CurveRenderData& data);
    static void render(QPainter& painter,
                       const CurveStyle& style,
                       CurveColorRole colorRole,
                       const CurveRenderData& data);
};

}
