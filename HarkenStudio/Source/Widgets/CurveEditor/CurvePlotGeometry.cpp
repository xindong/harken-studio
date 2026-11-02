#include "CurvePlotGeometry.h"

#include "Core/BindingSemantics.h"
#include "Core/NumericFormat.h"

#include <QFontMetrics>
#include <algorithm>
#include <cmath>

namespace VNX {
namespace {

double tickStep(double minimum)
{
    const double magnitude = std::pow(10.0, std::floor(std::log10(minimum)));
    for (double multiple : {1.0, 2.0, 5.0, 10.0}) {
        if (multiple * magnitude >= minimum)
            return multiple * magnitude;
    }
    return 10.0 * magnitude;
}

}

CurvePlotGeometry::CurvePlotGeometry(const QRectF& bounds, const CurvePlotProfile& profile,
    float xMin, float xMax, const CurveStyle& style, const QFont& tickFont, const QFont& labelFont,
    std::optional<QRectF> graph)
    : bounds_(bounds), profile_(profile), xMin_(xMin), xMax_(xMax), style_(style),
      tickFont_(tickFont), labelFont_(labelFont)
{
    if (graph) {
        graph_ = *graph;
        return;
    }
    const auto& metrics = style_.metrics;
    const int minimumLeft = profile_.axisScale != CurveAxisScale::LinearValue
        ? metrics.amplitudeGraphLeftInset : metrics.linearGraphLeftInset;
    const QFontMetrics fm(tickFont_);
    const int labelWidth = std::max(fm.horizontalAdvance(formatDecimal(profile_.yMin, profile_.valueDecimals)),
        fm.horizontalAdvance(formatDecimal(profile_.yMax, profile_.valueDecimals)));
    const qreal left = std::min<qreal>(std::max(minimumLeft, labelWidth + 12), bounds.width() * 0.4);
    const qreal right = std::min<qreal>(metrics.graphRightInset, bounds.width() * 0.1);
    const qreal top = std::min<qreal>(std::max(metrics.graphTopInset, fm.height() / 2 + 2), bounds.height() * 0.2);
    const qreal bottom = std::min<qreal>(std::max(metrics.graphBottomInset, fm.height() + 8), bounds.height() * 0.3);
    graph_ = QRectF(bounds.left() + left, bounds.top() + top,
        std::max(1.0, bounds.width() - left - right), std::max(1.0, bounds.height() - top - bottom));
}

QPointF CurvePlotGeometry::toScreen(double x, double value) const
{
    const double nx = std::clamp(profile_.logarithmicX
        ? std::log(std::max<double>(xMin_, x) / xMin_) / std::log(double(xMax_) / xMin_)
        : (x - xMin_) / (double(xMax_) - xMin_), 0.0, 1.0);
    double ny = profile_.yMin == profile_.yMax ? 0.5
        : curveAxisNormalizedY(profile_.axisScale, profile_.yMin, profile_.yMax,
            static_cast<float>(value));
    if (profile_.clampYToPlot) ny = std::clamp(ny, 0.0, 1.0);
    return {graph_.left() + nx * graph_.width(), graph_.bottom() - ny * graph_.height()};
}

QPointF CurvePlotGeometry::fromScreen(const QPointF& point) const
{
    const double nx = std::clamp((point.x() - graph_.left()) / graph_.width(), 0.0, 1.0);
    const double ny = std::clamp((graph_.bottom() - point.y()) / graph_.height(), 0.0, 1.0);
    return {profile_.logarithmicX ? xMin_ * std::pow(double(xMax_) / xMin_, nx)
        : xMin_ + nx * (double(xMax_) - xMin_),
        curveAxisValueFromNormalizedY(profile_.axisScale, profile_.yMin, profile_.yMax, ny)};
}

CurvePlotData CurvePlotGeometry::renderData() const
{
    const auto& style = style_;
    CurvePlotData result;
    auto& data = result.render;
    data.tickFont = tickFont_;
    const QFontMetrics fontMetrics(data.tickFont);
    data.canvasRect = bounds_;
    data.graphRect = graphRect();
    data.xAxisLabel = profile_.xAxisVisible ? profile_.xAxisLabel : QString{};
    data.yAxisLabel = profile_.yAxisLabel;

    const auto& metrics = style.metrics;
    const QRectF& graph = data.graphRect;
    const qreal textHeight = fontMetrics.height();
    const qreal gap = metrics.amplitudeTickLabelRightGap;
    const qreal inset = metrics.xAxisLabelTopOffset;
    const qreal titleHeight = QFontMetrics(labelFont_).height();
    data.yAxisLabelRect = QRectF(graph.left() + inset, graph.top() + inset,
        std::max(0.0, graph.width() - inset * 2.0), titleHeight);
    data.xAxisLabelRect = QRectF(graph.left() + inset, graph.bottom() - inset - titleHeight,
        std::max(0.0, graph.width() - inset * 2.0), titleHeight);
    if (!graph.contains(data.yAxisLabelRect))
        data.yAxisLabel.clear();
    if (!graph.contains(data.xAxisLabelRect) || data.yAxisLabelRect.intersects(data.xAxisLabelRect))
        data.xAxisLabel.clear();
    const CurveAxisScale scale = profile_.axisScale;
    const bool amplitudeAxis = scale != CurveAxisScale::LinearValue;
    const auto addTick = [&](const QRectF& rect, Qt::Alignment alignment,
                             const QString& text, CurveAxisBound bound = CurveAxisBound::None) {
        if (rect.width() < fontMetrics.horizontalAdvance(QStringLiteral("0"))
            || rect.top() < 0.0 || rect.bottom() > bounds_.height())
            return;
        for (const auto& existing : data.textTicks) {
            if (existing.rect.adjusted(-gap / 2.0, -2.0, gap / 2.0, 2.0).intersects(rect))
                return;
        }
        CurveRenderTextTick tick;
        tick.placement = CurveRenderTextTick::Placement::Rect;
        tick.rect = rect;
        tick.alignment = alignment;
        tick.text = fontMetrics.elidedText(text, Qt::ElideRight, static_cast<int>(rect.width()));
        tick.bound = bound;
        tick.editable = (bound == CurveAxisBound::XMax && profile_.xMaxEditable)
            || ((bound == CurveAxisBound::YMin || bound == CurveAxisBound::YMax)
                && profile_.yRangeEditable);
        data.textTicks.append(tick);
    };
    QVector<double> yValues{profile_.yMax, profile_.yMin};
    const int rows = std::clamp(static_cast<int>(std::round(graph.height()
        / std::max(metrics.amplitudeGridTargetRowSpacing, textHeight + 8.0))),
        amplitudeAxis ? metrics.amplitudeGridMinimumRows : 1, metrics.amplitudeGridMaximumRows);
    const double ySpan = static_cast<double>(profile_.yMax) - profile_.yMin;
    const double yStep = ySpan > 0.0 ? tickStep(ySpan / rows) : 1.0;
    const double firstY = std::ceil(profile_.yMin / yStep) * yStep;
    const bool mirrored = scale == CurveAxisScale::MirroredAmplitudeDb;
    const double zeroY = mirrored ? curveAxisNormalizedY(scale, profile_.yMin, profile_.yMax, 0.0f) : 0.0;
    if (mirrored) {
        const qreal y = toScreen(xMin_, 0.0f).y();
        data.gridLines.append(QLineF(graph.left(), y, graph.right(), y));
        yValues.append(0.0);
    }
    for (int i = mirrored ? -rows : 0; i <= rows; ++i) {
        const double normalizedY = zeroY + double(i) / rows;
        if (mirrored && (i == 0 || normalizedY <= 0.0 || normalizedY >= 1.0))
            continue;
        const float value = amplitudeAxis
            ? curveAxisValueFromNormalizedY(scale, profile_.yMin, profile_.yMax, normalizedY)
            : static_cast<float>(firstY + i * yStep);
        if (value <= profile_.yMin || value >= profile_.yMax)
            continue;
        const qreal y = toScreen(xMin_, value).y();
        data.gridLines.append(QLineF(graph.left(), y, graph.right(), y));
        yValues.append(value);
    }
    const QStringList yLabels = formatAxisValues(yValues, profile_.valueDecimals);
    for (int i = 0; i < yValues.size(); ++i) {
        const qreal y = toScreen(xMin_, yValues[i]).y();
        const auto bound = i == 0 ? CurveAxisBound::YMax
            : i == 1 ? CurveAxisBound::YMin : CurveAxisBound::None;
        addTick(QRectF(0.0, y - textHeight / 2.0, std::max(0.0, graph.left() - gap), textHeight),
            Qt::AlignRight | Qt::AlignVCenter, yLabels[i], bound);
    }

    const auto xTickText = [this](float x) {
        if (profile_.logarithmicX)
            return x >= 1000.0f ? formatAxisValue(x / 1000.0, 2) + QStringLiteral("k")
                               : formatAxisValue(x, 1);
        return profile_.xTicksUseLabels
            ? BindingSemantics::axisTickLabel(x, profile_.xTickLabels)
            : QString::number(x, 'f', profile_.xDecimals);
    };
    QVector<float> xValues;
    if (profile_.xAxisVisible) xValues = {xMin_, xMax_};
    const qreal columnSpacing = std::max<qreal>(48.0,
        std::max(fontMetrics.horizontalAdvance(xTickText(xMin_)),
                  fontMetrics.horizontalAdvance(xTickText(xMax_))) + gap * 2.0);
    const int columns = std::max(1, static_cast<int>(graph.width() / columnSpacing));
    double xStep = tickStep((static_cast<double>(xMax_) - xMin_) / columns);
    if (profile_.discretePreview)
        xStep = std::max(1.0, xStep);
    if (profile_.logarithmicX && profile_.xAxisVisible) {
        const int firstDecade = static_cast<int>(std::floor(std::log10(xMin_)));
        const int lastDecade = static_cast<int>(std::ceil(std::log10(xMax_)));
        qreal previous = graph.left();
        for (int decade = firstDecade; decade <= lastDecade; ++decade) {
            for (double factor : {1.0, 2.0, 5.0}) {
                const double value = factor * std::pow(10.0, decade);
                if (value <= xMin_ || value >= xMax_) continue;
                const qreal x = toScreen(value, profile_.yMin).x();
                if (x - previous < columnSpacing / 2.0 || graph.right() - x < columnSpacing / 2.0)
                    continue;
                data.gridLines.append(QLineF(x, graph.top(), x, graph.bottom()));
                xValues.append(static_cast<float>(value));
                previous = x;
            }
        }
    } else {
        const double firstX = std::ceil(xMin_ / xStep) * xStep;
        for (int i = 0; profile_.xAxisVisible && i <= columns; ++i) {
            const float value = static_cast<float>(firstX + i * xStep);
            if (value <= xMin_ || value >= xMax_)
                continue;
            const qreal x = toScreen(value, profile_.yMin).x();
            data.gridLines.append(QLineF(x, graph.top(), x, graph.bottom()));
            if (profile_.xMaxEditValue.value_or(xMax_) != xMin_)
                xValues.append(value);
        }
    }
    if (profile_.xAxisVisible && profile_.xTicksUseLabels) {
        for (const auto& label : profile_.xTickLabels) {
            if (label.anchor > xMin_ && label.anchor < xMax_)
                xValues.append(static_cast<float>(label.anchor));
        }
    }
    QStringList xLabels;
    for (int i = 0; i < xValues.size(); ++i)
        xLabels.append(xTickText(i == 1 ? profile_.xMaxEditValue.value_or(xValues[i]) : xValues[i]));
    if (!profile_.xTicksUseLabels)
        xLabels = compactAxisLabels(std::move(xLabels));
    for (int i = 0; i < xValues.size(); ++i) {
        const auto bound = i == 0 ? CurveAxisBound::XMin
            : i == 1 ? CurveAxisBound::XMax : CurveAxisBound::None;
        const qreal w = std::min<qreal>(fontMetrics.horizontalAdvance(xLabels[i]) + 4.0,
            std::max(0.0, graph.width() / 2.0 - gap));
        const qreal x = toScreen(xValues[i], profile_.yMin).x();
        const qreal left = i == 0 ? x : i == 1 ? x - w : x - w / 2.0;
        addTick(QRectF(left, graph.bottom() + metrics.xMaxLabelTopOffset, w, textHeight),
            Qt::AlignCenter, xLabels[i], bound);
    }

    result.xStep = xStep;
    result.yStep = yStep;
    return result;
}

}
