#include "FilterResponseCanvas.h"

#include "Core/FontManager.h"
#include "Core/NumericFormat.h"
#include "Core/ThemeManager.h"
#include "Widgets/CurveEditor/CurveRenderer.h"

#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>

namespace VNX {

FilterResponseCanvas::FilterResponseCanvas(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("FilterResponseCanvas"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFont(FontManager::instance().getFont(FontRole::Caption));
    profile_.axisScale = CurveAxisScale::LinearValue;
    profile_.yMin = -48.0f;
    profile_.yMax = 48.0f;
    profile_.logarithmicX = true;
    profile_.xDecimals = 0;
    profile_.xAxisLabel = tr("Frequency (Hz)");
    profile_.yAxisLabel = tr("Gain (dB)");
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged,
        this, [this] { update(); });
    connect(&FontManager::instance(), &FontManager::fontChanged, this, [this] {
        setFont(FontManager::instance().getFont(FontRole::Caption));
        updateGeometry();
        update();
    });
}

void FilterResponseCanvas::setFilter(const FilterState& filter, double sampleRate)
{
    filter_ = filter;
    sampleRate_ = sampleRate;
    update();
}

void FilterResponseCanvas::setReadOnly(bool readOnly)
{
    if (readOnly_ == readOnly) return;
    if (readOnly) finishDrag(true);
    readOnly_ = readOnly;
    update();
}

void FilterResponseCanvas::cancelEdit()
{
    finishDrag(true);
}

void FilterResponseCanvas::commitEdit()
{
    finishDrag(false);
}

QSize FilterResponseCanvas::sizeHint() const { return {480, 240}; }
QSize FilterResponseCanvas::minimumSizeHint() const { return {160, 120}; }

CurvePlotGeometry FilterResponseCanvas::geometry() const
{
    return {rect(), profile_, 20.0f, 20000.0f,
        ThemeManager::instance().snapshot()->domains.curve,
        FontManager::instance().getFont(FontRole::Axis), font()};
}

QPointF FilterResponseCanvas::sectionPoint(int index, const CurvePlotGeometry& plot) const
{
    const auto kind = static_cast<FilterKind>(index);
    const auto& section = filter_.sections[static_cast<size_t>(index)];
    return plot.toScreen(section.frequency,
        FilterSemantics::sectionResponseDb(kind, section, section.frequency, sampleRate_));
}

int FilterResponseCanvas::sectionAt(const QPointF& position) const
{
    const auto plot = geometry();
    const auto& metrics = ThemeManager::instance().snapshot()->domains.curve.metrics;
    double nearest = metrics.pointSelectedRadius + metrics.pointBorderWidth + 4.0;
    int found = -1;
    for (int i = 0; i < 4; ++i) {
        if (FilterSemantics::sectionIsDefault(static_cast<FilterKind>(i), filter_.sections[i])) continue;
        const double distance = QLineF(position, sectionPoint(i, plot)).length();
        if (distance < nearest) {
            nearest = distance;
            found = i;
        }
    }
    return found;
}

CurveRenderData FilterResponseCanvas::renderData() const
{
    const auto plot = geometry();
    auto data = plot.renderData().render;
    data.frameVisible = false;
    const auto& colors = ThemeManager::instance().colors();
    data.curveColor = colors.accent;
    const int samples = std::max(2, static_cast<int>(std::ceil(data.graphRect.width() * devicePixelRatioF())));
    for (int sample = 0; sample <= samples; ++sample) {
        const double frequency = 20.0 * std::pow(1000.0, double(sample) / samples);
        const auto point = plot.toScreen(frequency, FilterSemantics::responseDb(filter_, frequency, sampleRate_));
        if (sample == 0) data.continuousPath.moveTo(point);
        else data.continuousPath.lineTo(point);
    }
    for (int i = 0; i < 4; ++i) {
        const auto kind = static_cast<FilterKind>(i);
        const auto& section = filter_.sections[i];
        if (FilterSemantics::sectionIsDefault(kind, section)) continue;
        CurveRenderOverlay overlay;
        overlay.color = colors.categoricalColor(i);
        for (int sample = 0; sample <= samples; ++sample) {
            const double frequency = 20.0 * std::pow(1000.0, double(sample) / samples);
            const auto point = plot.toScreen(frequency,
                FilterSemantics::sectionResponseDb(kind, section, frequency, sampleRate_));
            if (sample == 0) overlay.path.moveTo(point);
            else overlay.path.lineTo(point);
        }
        data.overlays.append(overlay);
        CurveRenderPoint marker;
        marker.position = sectionPoint(i, plot);
        marker.color = colors.categoricalColor(i);
        marker.selected = dragged_ == i;
        marker.hovered = hovered_ == i;
        data.points.append(marker);
    }
    data.crosshair.visible = hoverActive_ && dragged_ < 0 && data.graphRect.contains(hoverPosition_);
    if (data.crosshair.visible) {
        const auto value = plot.fromScreen(hoverPosition_);
        data.crosshair.position = hoverPosition_;
        data.crosshair.label = tr("%1 Hz · %2 dB")
            .arg(formatAxisValue(value.x(), 1))
            .arg(formatAxisValue(value.y(), 1));
    }
    return data;
}

void FilterResponseCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    CurveRenderer::render(painter, ThemeManager::instance().snapshot()->domains.curve,
        CurveColorRole::ParameterBinding, renderData());
}

void FilterResponseCanvas::editSection(int index, const FilterSection& section)
{
    filter_.sections[index] = section;
    emit sectionEdited(static_cast<FilterKind>(index), section.frequency, section.q, section.gain);
    update();
}

void FilterResponseCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || readOnly_) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragged_ = sectionAt(event->position());
    if (dragged_ < 0) return;
    setFocus(Qt::MouseFocusReason);
    dragStart_ = filter_.sections[dragged_];
    emit editStarted();
    setCursor(Qt::ClosedHandCursor);
    update();
}

void FilterResponseCanvas::mouseMoveEvent(QMouseEvent* event)
{
    hoverPosition_ = event->position();
    hoverActive_ = true;
    if (dragged_ >= 0) {
        const auto value = geometry().fromScreen(event->position());
        auto section = filter_.sections[dragged_];
        section.frequency = std::clamp(value.x(), 20.0, 20000.0);
        if (dragged_ >= 2) section.gain = std::clamp(value.y() * 2.0, -48.0, 48.0);
        editSection(dragged_, section);
    } else {
        hovered_ = sectionAt(event->position());
        setCursor(hovered_ >= 0 && !readOnly_ ? Qt::OpenHandCursor : Qt::CrossCursor);
        if (hovered_ >= 0) {
            const std::array<QString, 4> names{tr("Low Pass"), tr("High Pass"), tr("Low Shelf"), tr("High Shelf")};
            const auto& section = filter_.sections[hovered_];
            setToolTip(tr("%1\nFrequency: %2 Hz\nQ Value: %3")
                .arg(names[hovered_], formatAxisValue(section.frequency, 1), formatAxisValue(section.q, 3))
                + (hovered_ >= 2 ? tr("\nGain: %1 dB").arg(formatAxisValue(section.gain, 1)) : QString()));
        } else setToolTip({});
    }
    update();
}

void FilterResponseCanvas::finishDrag(bool cancel)
{
    if (dragged_ < 0) return;
    const int index = dragged_;
    dragged_ = -1;
    if (cancel) editSection(index, dragStart_);
    emit editFinished();
    unsetCursor();
    update();
}

void FilterResponseCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) finishDrag(false);
    else QWidget::mouseReleaseEvent(event);
}

void FilterResponseCanvas::wheelEvent(QWheelEvent* event)
{
    if (dragged_ >= 0) { event->ignore(); return; }
    const int index = sectionAt(event->position());
    if (readOnly_ || index < 0) { event->ignore(); return; }
    auto section = filter_.sections[index];
    const double divisor = event->modifiers().testFlag(Qt::ShiftModifier) ? 12000.0 : 1200.0;
    section.q = std::clamp(section.q * std::pow(2.0, event->angleDelta().y() / divisor), 0.1, 10.0);
    emit editStarted();
    editSection(index, section);
    emit editFinished();
    event->accept();
}

void FilterResponseCanvas::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && dragged_ >= 0) finishDrag(true);
    else QWidget::keyPressEvent(event);
}

void FilterResponseCanvas::leaveEvent(QEvent* event)
{
    hoverActive_ = false;
    hovered_ = -1;
    update();
    QWidget::leaveEvent(event);
}

void FilterResponseCanvas::hideEvent(QHideEvent* event)
{
    finishDrag(true);
    QWidget::hideEvent(event);
}

}
