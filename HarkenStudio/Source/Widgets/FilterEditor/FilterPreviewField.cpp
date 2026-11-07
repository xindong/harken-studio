#include "FilterPreviewField.h"

#include "Core/ThemeManager.h"
#include "Widgets/CurveEditor/CurvePlotGeometry.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace VNX {

FilterPreviewField::FilterPreviewField(QWidget* parent)
    : PreviewField(parent)
{
    setObjectName(QStringLiteral("filterPreview"));
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Open Filter Editor"));
    setFilter(FilterSemantics::defaultState());
}

void FilterPreviewField::setFilter(const FilterState& filter, double sampleRate)
{
    filter_ = filter;
    sampleRate_ = sampleRate;
    setDimmed(FilterSemantics::isDefault(filter_));
    update();
}

void FilterPreviewField::setMixed(bool mixed)
{
    mixed_ = mixed;
    update();
}

void FilterPreviewField::paintPreview(QPainter& painter, const QRectF& bounds)
{
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& theme = ThemeManager::instance();
    if (mixed_) {
        painter.setPen(theme.colors().textSecondary);
        painter.drawText(bounds, Qt::AlignCenter, QStringLiteral("—"));
        return;
    }
    CurvePlotProfile profile;
    profile.axisScale = CurveAxisScale::LinearValue;
    profile.yMin = -48.0f;
    profile.yMax = 48.0f;
    profile.logarithmicX = true;
    const CurvePlotGeometry plot{rect(), profile, 20.0f, 20000.0f,
        theme.snapshot()->domains.curve, font(), font(), bounds};
    const int samples = std::max(2, static_cast<int>(std::ceil(bounds.width() * devicePixelRatioF())));
    QPainterPath path;
    for (int sample = 0; sample <= samples; ++sample) {
        const double ratio = double(sample) / samples;
        const double frequency = 20.0 * std::pow(1000.0, ratio);
        const QPointF point = plot.toScreen(frequency, FilterSemantics::responseDb(filter_, frequency, sampleRate_));
        if (sample == 0) path.moveTo(point);
        else path.lineTo(point);
    }
    painter.setClipRect(bounds);
    painter.setPen(QPen(FilterSemantics::isDefault(filter_) ? theme.colors().textSecondary : theme.colors().accent,
        theme.snapshot()->domains.curve.metrics.curveLineWidth));
    painter.drawPath(path);
}

}
