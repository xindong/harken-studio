#include "CurveRenderer.h"

#include <QFont>
#include <QPainter>
#include <QPen>
#include <QtGlobal>

namespace VNX {
namespace {

QColor curveColor(const CurveStyle::Colors& colors, CurveColorRole role)
{
    switch (role) {
    case CurveColorRole::AttenuationVolume:
        return colors.attenuationVolume;
    case CurveColorRole::AttenuationPitch:
        return colors.attenuationPitch;
    case CurveColorRole::ParameterBinding:
        return colors.parameterBinding;
    case CurveColorRole::GridFade:
        return colors.gridFade;
    }
    Q_UNREACHABLE_RETURN(colors.attenuationVolume);
}

void drawGrid(QPainter& painter, const CurveStyle& style, const CurveRenderData& data)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(style.colors.grid, style.metrics.gridLineWidth));
    for (const QLineF& line : data.gridLines)
        painter.drawLine(line);

    painter.setPen(QPen(style.colors.plotBorder, style.metrics.plotBorderWidth));
    painter.drawRect(data.graphRect);
    painter.restore();

    painter.save();
    painter.setFont(data.tickFont);
    painter.setPen(style.colors.axisText);
    for (const CurveRenderTextTick& tick : data.textTicks) {
        if (tick.placement == CurveRenderTextTick::Placement::Rect)
            painter.drawText(tick.rect, tick.alignment, tick.text);
        else
            painter.drawText(tick.point, tick.text);
    }
    painter.restore();
}

void drawCurve(QPainter& painter,
               const CurveStyle& style,
               const QColor& color,
               const CurveRenderData& data)
{
    painter.setBrush(Qt::NoBrush);
    if (data.discretePath) {
        QColor guide = color;
        guide.setAlphaF(guide.alphaF() * style.metrics.discreteGuideAlpha / 255.0);
        painter.setPen(QPen(guide,
                            style.metrics.discreteGuideLineWidth,
                            Qt::DashLine,
                            Qt::RoundCap,
                            Qt::RoundJoin));
        painter.drawPath(data.continuousPath);
        painter.setPen(QPen(color,
                            style.metrics.curveLineWidth,
                            Qt::SolidLine,
                            Qt::SquareCap,
                            Qt::MiterJoin));
        painter.drawPath(*data.discretePath);
    } else {
        painter.setPen(QPen(color,
                            style.metrics.curveLineWidth,
                            Qt::SolidLine,
                            Qt::RoundCap,
                            Qt::RoundJoin));
        painter.drawPath(data.continuousPath);
    }

    for (const CurveRenderHandle& handle : data.handles) {
        painter.setPen(QPen(style.colors.handleLine, style.metrics.handleLineWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(handle.anchor, handle.position);
        painter.setPen(QPen(handle.active ? style.colors.handleActiveBorder
                                         : style.colors.handleBorder,
                            style.metrics.handleBorderWidth));
        painter.setBrush(handle.active ? style.colors.handleActiveFill
                                       : style.colors.handleFill);
        painter.drawEllipse(handle.position,
                            style.metrics.handleRadius,
                            style.metrics.handleRadius);
    }

    for (const CurveRenderPoint& point : data.points) {
        painter.setPen(QPen(point.color.isValid() ? point.color
                            : point.selected ? style.colors.pointSelectedBorder : style.colors.pointBorder,
                            style.metrics.pointBorderWidth));
        painter.setBrush(point.selected ? style.colors.pointSelectedFill : style.colors.plotBackground);
        const qreal radius = point.selected || point.hovered ? style.metrics.pointSelectedRadius
                                            : style.metrics.pointRadius;
        painter.drawEllipse(point.position, radius, radius);
    }
}

void drawGhost(QPainter& painter,
               const CurveStyle& style,
               const CurveRenderGhost& ghost)
{
    if (!ghost.visible)
        return;

    painter.setPen(QPen(style.colors.ghost,
                        style.metrics.ghostLineWidth,
                        Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(ghost.from, ghost.to);
    painter.drawEllipse(ghost.to,
                        style.metrics.ghostRadius,
                        style.metrics.ghostRadius);
}

void drawAxisLabels(QPainter& painter,
                    const CurveStyle& style,
                    const CurveRenderData& data)
{
    painter.save();
    painter.setClipRect(data.graphRect);
    painter.setPen(style.colors.axisText);
    const auto draw = [&](const QRectF& rect, Qt::Alignment alignment, const QString& text) {
        painter.drawText(rect, alignment | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(text, Qt::ElideRight, static_cast<int>(rect.width())));
    };
    draw(data.xAxisLabelRect, Qt::AlignRight, data.xAxisLabel);
    draw(data.yAxisLabelRect, Qt::AlignLeft, data.yAxisLabel);
    painter.restore();
}

void drawLasso(QPainter& painter,
               const CurveStyle& style,
               const CurveRenderLasso& lasso)
{
    if (!lasso.visible)
        return;

    painter.setPen(QPen(style.colors.lassoBorder,
                        style.metrics.lassoBorderWidth,
                        Qt::DashLine));
    painter.setBrush(style.colors.lassoFill);
    painter.drawRect(lasso.rect.normalized());
}

void drawOverlays(QPainter& painter,
                  const CurveStyle& style,
                  const CurveRenderData& data)
{
    painter.setBrush(Qt::NoBrush);
    for (const CurveRenderOverlay& overlay : data.overlays) {
        QColor color = overlay.color.isValid() ? overlay.color : curveColor(style.colors, overlay.colorRole);
        if (!overlay.selectable)
            color.setAlphaF(color.alphaF() * style.metrics.discreteGuideAlpha / 255.0);
        QColor guide = color;
        if (overlay.discretePath && overlay.selectable)
            guide.setAlphaF(guide.alphaF() * style.metrics.discreteGuideAlpha / 255.0);
        painter.setPen(QPen(guide,
                            style.metrics.discreteGuideLineWidth,
                            overlay.selectable && !overlay.discretePath ? Qt::SolidLine : Qt::DashLine,
                            Qt::RoundCap,
                            Qt::RoundJoin));
        painter.drawPath(overlay.path);
        if (overlay.discretePath) {
            painter.setPen(QPen(color, style.metrics.discreteGuideLineWidth,
                overlay.selectable ? Qt::SolidLine : Qt::DashLine, Qt::SquareCap, Qt::MiterJoin));
            painter.drawPath(*overlay.discretePath);
        }
    }
}

void drawPlayhead(QPainter& painter,
                  const CurveStyle& style,
                  const CurveRenderData& data)
{
    if (!data.playhead.visible)
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(style.colors.playhead, style.metrics.crosshairLineWidth));
    painter.drawLine(QPointF(data.playhead.x, data.graphRect.top()),
                     QPointF(data.playhead.x, data.graphRect.bottom()));
    painter.restore();
}

}

void CurveRenderer::render(QPainter& painter,
                           const CurveStyle& style,
                           CurveColorRole colorRole,
                           const CurveRenderData& data)
{
    renderBackground(painter, style, data);
    renderForeground(painter, style, colorRole, data);
}

void CurveRenderer::renderBackground(QPainter& painter, const CurveStyle& style, const CurveRenderData& data)
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(data.canvasRect, style.colors.canvasBackground);
    painter.fillRect(data.graphRect, style.colors.plotBackground);
    if (data.frameVisible) {
        painter.setPen(QPen(style.colors.canvasBorder, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(data.canvasRect.adjusted(0.5, 0.5, -0.5, -0.5));
    }

    drawGrid(painter, style, data);
}

void CurveRenderer::renderForeground(QPainter& painter, const CurveStyle& style,
                                    CurveColorRole colorRole, const CurveRenderData& data)
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.save();
    const qreal padding = data.clipToPlot ? 0.0
        : style.metrics.pointSelectedRadius + style.metrics.pointBorderWidth;
    painter.setClipRect(data.graphRect.adjusted(-padding, -padding, padding, padding));
    drawOverlays(painter, style, data);
    drawCurve(painter, style, data.curveColor.isValid() ? data.curveColor
        : curveColor(style.colors, colorRole), data);
    drawPlayhead(painter, style, data);
    drawGhost(painter, style, data.ghost);
    drawLasso(painter, style, data.lasso);
    painter.restore();
    drawAxisLabels(painter, style, data);
    drawAxisCrosshair(painter, style, data.graphRect, data.crosshair);
}

}
