#pragma once

#include "Core/CurveSemantics.h"
#include "Core/VariableSemantics.h"
#include "CurveRenderer.h"

#include <optional>
#include <vector>

namespace VNX {

struct CurvePlotProfile
{
    CurveAxisScale axisScale = CurveAxisScale::AmplitudeDb;
    float yMin = -96.0f;
    float yMax = 0.0f;
    std::optional<float> valueMinimum;
    std::optional<float> valueMaximum;
    QString xAxisLabel;
    QString yAxisLabel;
    bool xAxisVisible = true;
    QString valueUnit{QStringLiteral("dB")};
    int valueDecimals = 1;
    bool discretePreview = false;
    bool xMaxEditable = false;
    float xMaxMinimum = 1.0f;
    float xMaxMaximum = 100000.0f;
    int xDecimals = 2;
    std::optional<float> xMaxEditValue;
    bool yRangeEditable = false;
    float yRangeMinimum = 0.0f;
    float yRangeMaximum = 1.0f;
    bool xTicksUseLabels = false;
    std::vector<VariableLabel> xTickLabels;
    bool logarithmicX = false;
    bool clampYToPlot = true;
};

struct CurvePlotData
{
    CurveRenderData render;
    double xStep = 1.0;
    double yStep = 1.0;
};

class CurvePlotGeometry
{
public:
    CurvePlotGeometry(const QRectF& bounds, const CurvePlotProfile& profile,
                      float xMin, float xMax, const CurveStyle& style,
                      const QFont& tickFont, const QFont& labelFont,
                      std::optional<QRectF> graph = std::nullopt);
    const QRectF& graphRect() const { return graph_; }
    QPointF toScreen(double x, double value) const;
    QPointF fromScreen(const QPointF& point) const;
    CurvePlotData renderData() const;

private:
    QRectF bounds_;
    QRectF graph_;
    const CurvePlotProfile& profile_;
    float xMin_;
    float xMax_;
    const CurveStyle& style_;
    QFont tickFont_;
    QFont labelFont_;
};

}
