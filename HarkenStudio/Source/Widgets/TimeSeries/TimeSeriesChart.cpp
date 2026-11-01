#include "TimeSeriesChart.h"

#include "Core/ThemeManager.h"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace VNX {
namespace {

QColor seriesColor(const DomainColors::Monitor& monitor, std::size_t index)
{
    const QColor palette[] = {
        monitor.cpuSafe,
        monitor.cpuWarning,
        monitor.cpuCritical,
        monitor.peakSafe,
        monitor.bufferNormal,
    };
    return palette[index % (sizeof(palette) / sizeof(palette[0]))];
}

QString hitText(const TimeSeriesHit& hit)
{
    switch (hit.kind)
    {
    case TimeSeriesHitKind::Numeric:
        return QString::fromStdString(hit.seriesId) + QStringLiteral("  ")
            + QString::number(hit.x) + QStringLiteral("  ")
            + QString::number(hit.y, 'f', 3);
    case TimeSeriesHitKind::Gap:
        return QString::fromStdString(hit.seriesId) + QStringLiteral("  gap");
    case TimeSeriesHitKind::Invalid:
        return QString::fromStdString(hit.seriesId) + QStringLiteral("  invalid");
    case TimeSeriesHitKind::WarmUp:
        return QString::fromStdString(hit.seriesId) + QStringLiteral("  warm-up");
    case TimeSeriesHitKind::HistoryUnavailable:
        return QStringLiteral("history unavailable");
    case TimeSeriesHitKind::Empty:
        return QString();
    }
    return {};
}

} // namespace

TimeSeriesChart::TimeSeriesChart(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(80, 48);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, this, [this] {
        buffer_ = {};
        update();
    });
}

void TimeSeriesChart::updateData(const TimeSeriesChartModel& model)
{
    model_ = model;
    rebuildRenderSeries();
    rebuildBuffer();
    update();
}

bool TimeSeriesChart::setWindow(std::int64_t start, std::int64_t end)
{
    if (!model_.setWindow(start, end))
        return false;
    rebuildRenderSeries();
    rebuildBuffer();
    update();
    return true;
}

void TimeSeriesChart::setTestDevicePixelRatio(qreal dpr)
{
    if (dpr <= 0.0)
        return;
    dprOverride_ = true;
    dpr_ = dpr;
    rebuildBuffer();
    update();
}

void TimeSeriesChart::zoomAround(std::int64_t focus, double factor)
{
    if (factor <= 0.0)
        return;
    const auto start = model_.windowStart();
    const auto end = model_.windowEnd();
    const auto span = static_cast<double>(end - start);
    auto nextSpan = span / factor;
    if (nextSpan < 1.0)
        nextSpan = 1.0;
    const auto ratio = span <= 0.0 ? 0.5 : static_cast<double>(focus - start) / span;
    auto nextStart = static_cast<std::int64_t>(std::llround(static_cast<double>(focus) - nextSpan * ratio));
    auto nextEnd = nextStart + static_cast<std::int64_t>(std::llround(nextSpan));
    if (nextEnd <= nextStart)
        nextEnd = nextStart + 1;
    setWindow(nextStart, nextEnd);
}

void TimeSeriesChart::panBySamples(std::int64_t delta)
{
    setWindow(model_.windowStart() + delta, model_.windowEnd() + delta);
}

QSize TimeSeriesChart::bufferPixelSize() const
{
    return buffer_.size();
}

void TimeSeriesChart::paintEvent(QPaintEvent*)
{
    if (buffer_.isNull())
        rebuildBuffer();
    QPainter painter(this);
    painter.drawImage(rect(), buffer_);
}

void TimeSeriesChart::resizeEvent(QResizeEvent*)
{
    rebuildRenderSeries();
    rebuildBuffer();
}

void TimeSeriesChart::wheelEvent(QWheelEvent* event)
{
    const auto focus = xFromLogical(event->position().toPoint().x());
    const auto factor = event->angleDelta().y() > 0 ? 1.25 : 0.8;
    zoomAround(focus, factor);
    event->accept();
}

void TimeSeriesChart::mouseMoveEvent(QMouseEvent* event)
{
    hover_ = event->pos();
    if (dragOrigin_)
    {
        const auto span = model_.windowEnd() - model_.windowStart();
        if (width() > 0 && span > 0)
        {
            const auto dx = event->pos().x() - dragOrigin_->x();
            const auto delta = -static_cast<std::int64_t>(
                std::llround(static_cast<double>(dx) * static_cast<double>(span) / static_cast<double>(width())));
            setWindow(dragWindowStart_ + delta, dragWindowStart_ + span + delta);
        }
    }
    rebuildBuffer();
    update();
}

void TimeSeriesChart::leaveEvent(QEvent*)
{
    hover_.reset();
    rebuildBuffer();
    update();
}

void TimeSeriesChart::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        dragOrigin_ = event->pos();
        dragWindowStart_ = model_.windowStart();
    }
}

void TimeSeriesChart::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        dragOrigin_.reset();
}

void TimeSeriesChart::rebuildRenderSeries()
{
    renderSeries_.clear();
    const int logicalWidth = std::max(1, width());
    for (const auto& series : model_.series())
    {
        if (!series.visible)
            continue;
        TimeSeriesRenderSeries rendered;
        rendered.id = series.id;
        const auto columns = buildTimeSeriesEnvelope(
            series, model_.windowStart(), model_.windowEnd(), logicalWidth);
        rendered.columns.reserve(columns.size());
        for (const auto& column : columns)
        {
            TimeSeriesRenderColumn out;
            out.column = column.column;
            out.empty = column.empty;
            out.hasState = column.hasState;
            out.state = column.state;
            out.min = column.min;
            out.max = column.max;
            rendered.columns.push_back(out);
        }
        renderSeries_.push_back(std::move(rendered));
    }
}

void TimeSeriesChart::rebuildBuffer()
{
    if (!dprOverride_)
        dpr_ = devicePixelRatioF();
    if (dpr_ <= 0.0)
        dpr_ = 1.0;
    const int pixelW = std::max(1, static_cast<int>(std::lround(width() * dpr_)));
    const int pixelH = std::max(1, static_cast<int>(std::lround(height() * dpr_)));
    if (buffer_.size() != QSize(pixelW, pixelH) || !qFuzzyCompare(buffer_.devicePixelRatio(), dpr_))
    {
        buffer_ = QImage(pixelW, pixelH, QImage::Format_ARGB32_Premultiplied);
        buffer_.setDevicePixelRatio(dpr_);
    }
    buffer_.fill(Qt::transparent);

    QPainter painter(&buffer_);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto snapshot = ThemeManager::instance().snapshot();
    const auto& ui = snapshot->ui.colors;
    const auto& monitor = snapshot->domains.monitor;
    painter.fillRect(QRectF(0, 0, width(), height()), ui.bgPrimary);

    std::size_t colorIndex = 0;
    for (const auto& rendered : renderSeries_)
    {
        const auto* source = model_.series(rendered.id);
        if (source == nullptr)
            continue;
        const auto yRange = model_.resolvedYRange(rendered.id);
        if (!yRange)
        {
            ++colorIndex;
            continue;
        }
        const auto yMin = yRange->first;
        const auto yMax = yRange->second;
        const auto spanY = yMax - yMin;
        if (spanY <= 0.0)
        {
            ++colorIndex;
            continue;
        }

        const auto color = seriesColor(monitor, colorIndex++);
        painter.setPen(QPen(color, 1.2));
        for (const auto& column : rendered.columns)
        {
            const qreal x = column.column + 0.5;
            if (column.hasState)
            {
                QColor mark = monitor.underrunIdle;
                if (column.state == TimeSeriesStateKind::Invalid)
                    mark = monitor.cpuCritical;
                else if (column.state == TimeSeriesStateKind::WarmUp)
                    mark = monitor.cpuWarning;
                else
                    mark = monitor.bufferWarning;
                mark.setAlphaF(mark.alphaF() * 96 / 255.0);
                painter.fillRect(QRectF(column.column, 0, 1, height()), mark);
                continue;
            }
            if (column.empty)
                continue;
            const qreal y1 = height() - ((column.min - yMin) / spanY) * height();
            const qreal y2 = height() - ((column.max - yMin) / spanY) * height();
            painter.drawLine(QPointF(x, y1), QPointF(x, y2));
        }

        painter.setPen(QPen(monitor.peakCaution, 1.0, Qt::DashLine));
        for (const auto& threshold : source->thresholds)
        {
            if (threshold.value < yMin || threshold.value > yMax)
                continue;
            const qreal y = height() - ((threshold.value - yMin) / spanY) * height();
            painter.drawLine(QPointF(0, y), QPointF(width(), y));
        }
    }

    const TimeSeriesSeriesSnapshot* hoverSeries = nullptr;
    for (const auto& series : model_.series())
    {
        if (series.visible)
        {
            hoverSeries = &series;
            break;
        }
    }
    if (hover_ && hoverSeries != nullptr)
    {
        const auto x = xFromLogical(hover_->x());
        const auto hit = model_.hitTest(x, hoverSeries->id);
        const auto text = hitText(hit);
        if (!text.isEmpty())
        {
            QColor line = ui.textPrimary;
            line.setAlphaF(line.alphaF() * 96 / 255.0);
            painter.setPen(line);
            painter.drawLine(hover_->x(), 0, hover_->x(), height());

            const QFontMetrics metrics(painter.font());
            const auto textWidth = metrics.horizontalAdvance(text) + 12;
            const auto textHeight = metrics.height() + 8;
            auto boxX = hover_->x() + 8;
            auto boxY = hover_->y() - textHeight - 8;
            if (boxX + textWidth > width())
                boxX = hover_->x() - textWidth - 8;
            if (boxY < 0)
                boxY = hover_->y() + 8;
            painter.fillRect(QRectF(boxX, boxY, textWidth, textHeight), ui.bgSecondary);
            painter.setPen(ui.textPrimary);
            painter.drawText(QRectF(boxX, boxY, textWidth, textHeight), Qt::AlignCenter, text);
        }
    }
}

std::int64_t TimeSeriesChart::xFromLogical(int logicalX) const
{
    const auto start = model_.windowStart();
    const auto end = model_.windowEnd();
    if (width() <= 0 || end <= start)
        return start;
    const auto clamped = std::clamp(logicalX, 0, width() - 1);
    return columnSampleX(start, end, width(), clamped);
}

} // namespace VNX
