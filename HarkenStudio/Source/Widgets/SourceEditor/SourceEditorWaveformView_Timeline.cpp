#include "SourceEditorWaveformView.h"

#include "Core/FontManager.h"
#include "Core/NumericFormat.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/WaveformRenderer.h"
#include "Widgets/Common/AmplitudeAxis.h"
#include "Widgets/Common/AmplitudeCurvePopup.h"
#include "Widgets/Common/TimeAxis.h"

#include <QActionGroup>
#include <QContextMenuEvent>
#include <QDialog>
#include "Widgets/Common/DecimalSpinBox.h"
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QToolButton>
#include <QToolTip>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace VNX {
namespace {

constexpr int kChannelHeight = 88;
constexpr int kScrollSteps = 1000000;
constexpr double kMaximumVerticalZoom = 32.0;

void placeAbove(QWidget* popup, const QPoint& anchor)
{
    QPoint position(anchor.x() - popup->width() / 2, anchor.y() - popup->height() - 6);
    if (const auto* screen = QGuiApplication::screenAt(anchor)) {
        const QRect available = screen->availableGeometry();
        position.setX(std::clamp(position.x(), available.left(),
            std::max(available.left(), available.right() - popup->width() + 1)));
        position.setY(std::max(available.top(), position.y()));
    }
    popup->move(position);
}

}

void SourceEditorWaveformView::setupTimeline()
{
    setMinimumSize(240, 160);
    connect(&FontManager::instance(), &FontManager::fontChanged,
        this, [this] { refreshTimelineControls(); update(); });
    units_ = new QToolButton(this);
    units_->setObjectName(QStringLiteral("sourceRangeUnits"));
    units_->setAutoRaise(true);
    units_->setArrowType(Qt::DownArrow);
    units_->setCursor(Qt::PointingHandCursor);
    units_->setAccessibleName(tr("Time unit"));
    QtControlSkin::setSemanticProperty(units_, "flatIconButton", true);
    QtControlSkin::setSemanticProperty(units_, "navigationRole", QStringLiteral("chevron"));
    units_->installEventFilter(this);
    auto* menu = new QMenu(units_);
    menu->setObjectName(QStringLiteral("sourceRangeUnitsMenu"));
    auto* group = new QActionGroup(menu);
    group->setExclusive(true);
    auto* seconds = menu->addAction(tr("Seconds"));
    seconds->setObjectName(QStringLiteral("sourceRangeSeconds"));
    seconds->setData(false);
    auto* frames = menu->addAction(tr("Frames"));
    frames->setObjectName(QStringLiteral("sourceRangeFrames"));
    frames->setData(true);
    for (auto* action : {seconds, frames}) {
        action->setCheckable(true);
        group->addAction(action);
    }
    seconds->setChecked(true);
    connect(units_, &QToolButton::clicked, this, [this, menu] {
        menu->popup(units_->mapToGlobal(QPoint(0, units_->height())));
    });
    connect(group, &QActionGroup::triggered, this, [this](QAction* action) {
        frameDisplay_ = action->data().toBool();
        refreshTimelineControls();
        update();
    });

    const auto button = [&](const QString& name, const QString& tooltip) {
        auto* result = new QToolButton(this);
        result->setObjectName(name);
        result->setAutoRaise(true);
        result->setToolTip(tooltip);
        result->setAccessibleName(tooltip);
        result->setCursor(Qt::PointingHandCursor);
        QtControlSkin::setSemanticProperty(result, "flatIconButton", true);
        result->installEventFilter(this);
        return result;
    };
    zoomOut_ = button(QStringLiteral("sourceWaveZoomOut"), tr("Zoom out time"));
    zoomIn_ = button(QStringLiteral("sourceWaveZoomIn"), tr("Zoom in time"));
    verticalZoomIn_ = button(QStringLiteral("sourceWaveVerticalZoomIn"), tr("Zoom in vertically"));
    verticalZoomOut_ = button(QStringLiteral("sourceWaveVerticalZoomOut"), tr("Zoom out vertically"));
    connect(zoomOut_, &QToolButton::clicked, this,
        [this] { zoomAround(waveformRect().center().x(), 0.5); });
    connect(zoomIn_, &QToolButton::clicked, this,
        [this] { zoomAround(waveformRect().center().x(), 2.0); });
    connect(verticalZoomIn_, &QToolButton::clicked, this,
        [this] { zoomVertically(channelViewportRect().center().y(), 1.25); });
    connect(verticalZoomOut_, &QToolButton::clicked, this,
        [this] { zoomVertically(channelViewportRect().center().y(), 0.8); });

    scroll_ = new QScrollBar(Qt::Horizontal, this);
    scroll_->setObjectName(QStringLiteral("sourceWaveScroll"));
    scroll_->setCursor(Qt::ArrowCursor);
    QtControlSkin::setSemanticProperty(scroll_, "scrollRole", QStringLiteral("canvas"));
    scroll_->installEventFilter(this);
    connect(scroll_, &QScrollBar::valueChanged, this, [this](int value) {
        const double start = scroll_->maximum() > 0
            ? double(value) / scroll_->maximum() * (double(totalFrames()) - viewFrameCount()) : 0.0;
        setViewRange(start, viewFrameCount());
    });
    channelScroll_ = new QScrollBar(Qt::Vertical, this);
    channelScroll_->setObjectName(QStringLiteral("sourceWaveChannelScroll"));
    channelScroll_->setCursor(Qt::ArrowCursor);
    QtControlSkin::setSemanticProperty(channelScroll_, "scrollRole", QStringLiteral("canvas"));
    channelScroll_->installEventFilter(this);
    connect(channelScroll_, &QScrollBar::valueChanged, this, [this] { update(); });

    handleValue_ = new QLabel(this, Qt::ToolTip | Qt::WindowTransparentForInput);
    handleValue_->setObjectName(QStringLiteral("sourceTrimValue"));
    handleValue_->setAttribute(Qt::WA_ShowWithoutActivating);
    handleValue_->setAutoFillBackground(true);
    handleValue_->setFrameStyle(QFrame::Box | QFrame::Plain);
    handleValue_->setContentsMargins(7, 4, 7, 4);
    handleValue_->hide();

    detailTimer_.setSingleShot(true);
    detailTimer_.setInterval(60);
    connect(&detailTimer_, &QTimer::timeout, this, &SourceEditorWaveformView::requestDetail);
    edgeScroll_.setInterval(16);
    connect(&edgeScroll_, &QTimer::timeout, this, [this] {
        const QRect plot = waveformRect();
        const int outside = dragPosition_.x() < plot.left()
            ? dragPosition_.x() - plot.left()
            : dragPosition_.x() > plot.right() ? dragPosition_.x() - plot.right() : 0;
        if (activeHandle_ == Handle::None || outside == 0) {
            edgeScroll_.stop();
            return;
        }
        const double step = std::clamp(double(outside) / std::max(1, plot.width()), -0.08, 0.08);
        setViewRange(viewStart_ + step * viewFrameCount(), viewFrameCount());
        dragHandle(dragPosition_);
    });
    refreshTimelineControls();
    layoutTimeline();
}

QRect SourceEditorWaveformView::waveformRect() const
{
    if (presentation_ == Presentation::Preview)
        return rect().adjusted(8, 8, -8, -8);
    const QFontMetrics metrics(FontManager::instance().getFont(FontRole::Axis), this);
    const int chevronExtent = ThemeManager::instance().metrics().navigationChevronExtent;
    const int axisWidth = std::max(metrics.horizontalAdvance(QStringLiteral("-96.00")) + 8, chevronExtent);
    const int rulerHeight = std::max(metrics.height() + 5, chevronExtent);
    const int rightWidth = channelScroll_ ? channelScroll_->sizeHint().width() : 0;
    const int footerHeight = scroll_ ? scroll_->sizeHint().height() : 0;
    return QRect(axisWidth, rulerHeight,
        std::max(1, width() - axisWidth - rightWidth - 1),
        std::max(1, height() - rulerHeight - footerHeight - 1));
}

QRect SourceEditorWaveformView::channelViewportRect() const
{
    const QRect plot = waveformRect();
    return QRect(1, plot.top() + kTimeAxisHandleBandHeight, plot.right(),
        std::max(1, plot.height() - kTimeAxisHandleBandHeight));
}

int SourceEditorWaveformView::channelContentHeight() const
{
    const QFontMetrics metrics(FontManager::instance().getFont(FontRole::Axis), this);
    const int minimumHeight = std::max(kChannelHeight, metrics.height() * 4 + 16);
    return int(std::lround(std::max(minimumHeight, channelViewportRect().height()) * verticalZoom_));
}

QRect SourceEditorWaveformView::channelWaveformRect() const
{
    const QFontMetrics metrics(FontManager::instance().getFont(FontRole::Axis), this);
    QRect viewport = channelViewportRect();
    viewport.setLeft(waveformRect().left());
    return waveformAmplitudeBounds(viewport, channelContentHeight(), channelScroll_->value(), metrics);
}

double SourceEditorWaveformView::viewFrameCount() const
{
    return viewCount_ > 0.0 ? viewCount_ : double(totalFrames());
}

void SourceEditorWaveformView::setViewRange(double startFrame, double frameCount)
{
    if (presentation_ != Presentation::TrimEditor)
        return;
    const double total = double(totalFrames());
    viewCount_ = std::clamp(frameCount, std::min(8.0, total), total);
    viewStart_ = std::clamp(startFrame, 0.0, total - viewCount_);
    refreshTimelineControls();
    detailTimer_.start();
    update();
}

void SourceEditorWaveformView::zoomAround(int x, double factor)
{
    if (factor <= 0.0 || totalFrames() == 0)
        return;
    const double ratio = std::clamp(double(x - waveformRect().left())
        / std::max(1, waveformRect().width() - 1), 0.0, 1.0);
    const double focus = viewStart_ + ratio * viewFrameCount();
    const double count = std::clamp(viewFrameCount() / factor,
        std::min(8.0, double(totalFrames())), double(totalFrames()));
    setViewRange(focus - ratio * count, count);
}

void SourceEditorWaveformView::zoomVertically(int y, double factor)
{
    if (totalFrames() == 0)
        return;
    const QRect viewport = channelViewportRect();
    const int anchor = std::clamp(y - viewport.top(), 0, viewport.height());
    const double focus = double(channelScroll_->value() + anchor) / channelContentHeight();
    verticalZoom_ = std::clamp(verticalZoom_ * factor, 1.0, kMaximumVerticalZoom);
    refreshTimelineControls();
    channelScroll_->setValue(int(std::lround(focus * channelContentHeight() - anchor)));
    update();
}

bool SourceEditorWaveformView::isTimeRuler(const QPoint& position) const
{
    const QRect plot = waveformRect();
    return presentation_ == Presentation::TrimEditor && totalFrames() > 0
        && position.y() >= 0 && position.y() < plot.top()
        && position.x() >= plot.left() && position.x() <= plot.right();
}

bool SourceEditorWaveformView::isPanArea(const QPoint& position) const
{
    return presentation_ == Presentation::TrimEditor && totalFrames() > 0
        && (isTimeRuler(position) || waveformRect().contains(position));
}

QRect SourceEditorWaveformView::trimGripRect(Handle handle) const
{
    const int x = handlePosition(static_cast<int>(handle));
    if (x < 0)
        return {};
    return QRect(handle == Handle::TrimStart ? x : x - kTimeAxisHandleWidth + 1,
        waveformRect().top(), kTimeAxisHandleWidth, kTimeAxisHandleBandHeight - 2);
}

int SourceEditorWaveformView::positionAt(double frame) const
{
    const double ratio = (frame - viewStart_) / std::max(1.0, viewFrameCount());
    if (ratio < 0.0 || ratio > 1.0)
        return -1;
    return waveformRect().left() + int(std::lround(ratio * (waveformRect().width() - 1)));
}

void SourceEditorWaveformView::layoutTimeline()
{
    if (!units_)
        return;
    const QRect plot = waveformRect();
    const int footerY = plot.bottom() + 1;
    const int horizontalExtent = scroll_->sizeHint().height();
    const int verticalExtent = channelScroll_->sizeHint().width();
    const int chevronExtent = ThemeManager::instance().metrics().navigationChevronExtent;
    units_->setGeometry((plot.left() - chevronExtent) / 2,
        (plot.top() - chevronExtent) / 2, chevronExtent, chevronExtent);
    zoomIn_->setGeometry(plot.left(), footerY, horizontalExtent, horizontalExtent);
    zoomOut_->setGeometry(plot.right() + 1 - horizontalExtent, footerY, horizontalExtent, horizontalExtent);
    scroll_->setGeometry(plot.left() + horizontalExtent, footerY,
        std::max(1, plot.width() - 2 * horizontalExtent), horizontalExtent);
    verticalZoomIn_->setGeometry(plot.right() + 1, plot.top(), verticalExtent, verticalExtent);
    verticalZoomOut_->setGeometry(plot.right() + 1, plot.bottom() + 1 - verticalExtent,
        verticalExtent, verticalExtent);
    const int available = channelViewportRect().height();
    channelScroll_->setGeometry(plot.right() + 1, plot.top() + verticalExtent,
        verticalExtent, std::max(1, plot.height() - 2 * verticalExtent));
    channelScroll_->setRange(0, std::max(0, channelContentHeight() - available));
    channelScroll_->setPageStep(available);
    channelScroll_->setSingleStep(std::max(1, available / 10));
    channelScroll_->setEnabled(totalFrames() > 0 && channelScroll_->maximum() > 0);
}

void SourceEditorWaveformView::refreshTimelineControls()
{
    if (!units_)
        return;
    const bool valid = totalFrames() > 0;
    units_->setEnabled(valid);
    zoomOut_->setEnabled(valid && viewFrameCount() < double(totalFrames()));
    zoomIn_->setEnabled(valid && viewFrameCount() > 8.0);
    verticalZoomIn_->setEnabled(valid && verticalZoom_ < kMaximumVerticalZoom);
    verticalZoomOut_->setEnabled(valid && verticalZoom_ > 1.0);
    {
        QSignalBlocker blocker(scroll_);
        const double ratio = valid ? viewFrameCount() / double(totalFrames()) : 1.0;
        const int page = std::clamp(int(std::lround(ratio * kScrollSteps)), 1, kScrollSteps);
        scroll_->setRange(0, kScrollSteps - page);
        scroll_->setPageStep(page);
        scroll_->setSingleStep(std::max(1, page / 10));
        const double maxStart = double(totalFrames()) - viewFrameCount();
        scroll_->setValue(maxStart > 0.0
            ? int(std::lround(viewStart_ / maxStart * scroll_->maximum())) : 0);
        scroll_->setEnabled(valid && page < kScrollSteps);
    }
    auto& manager = ThemeManager::instance();
    const auto theme = manager.snapshot();
    const int iconExtent = std::min(scroll_->sizeHint().height(), channelScroll_->sizeHint().width());
    const ThemeAssetVariant variant{QSize(iconExtent, iconExtent), devicePixelRatioF(), 0, theme->ui.colors.textSecondary};
    for (auto* button : {zoomIn_, zoomOut_, verticalZoomIn_, verticalZoomOut_})
        button->setIconSize(QSize(iconExtent, iconExtent));
    units_->setToolTip(frameDisplay_ ? tr("Time unit: audio sample frames") : tr("Time unit: seconds"));
    zoomOut_->setIcon(manager.resolveIcon(QString::fromLatin1(ThemeAssetIds::TableRemove), variant).icon);
    zoomIn_->setIcon(manager.resolveIcon(QString::fromLatin1(ThemeAssetIds::TableAdd), variant).icon);
    verticalZoomIn_->setIcon(zoomIn_->icon());
    verticalZoomOut_->setIcon(zoomOut_->icon());
    QPalette palette = handleValue_->palette();
    palette.setColor(QPalette::Window, theme->ui.colors.bgSecondary);
    palette.setColor(QPalette::WindowText, theme->ui.colors.textPrimary);
    handleValue_->setPalette(palette);
    layoutTimeline();
}

void SourceEditorWaveformView::paintTimeline(QPainter& painter)
{
    const auto theme = ThemeManager::instance().snapshot();
    const auto& colors = theme->ui.colors;
    const QRect plot = waveformRect();
    const int rulerHeight = plot.top();
    const int axisWidth = plot.left();
    painter.setFont(FontManager::instance().getFont(FontRole::Axis));
    const QFontMetrics metrics(painter.font(), painter.device());
    painter.fillRect(QRect(0, 0, width(), rulerHeight), colors.bgSecondary);
    painter.fillRect(QRect(0, rulerHeight, axisWidth, plot.height()), colors.bgSecondary);

    const auto timeTicks = timeAxisTicks();
    drawTimeAxisRuler(painter, colors, QRect(plot.left(), 0, plot.width(), rulerHeight), timeTicks);
    drawTimeAxisGrid(painter, theme->domains.curve.colors.grid, plot, timeTicks);

    const QRect channelArea = channelViewportRect();
    const bool detailCovers = detail_.isValid() && viewStart_ >= double(detailStart_)
        && viewStart_ + viewFrameCount() <= double(detailStart_ + detailCount_);
    const auto& data = detailCovers ? detail_ : peaks_;
    const auto& waveStyle = theme->domains.waveform;
    painter.save();
    painter.setClipRect(channelArea);
    const QRect wave = channelWaveformRect();
    const int center = wave.top() + wave.height() / 2;
    const int half = wave.height() / 2;
    const auto labelRect = [&](int y) {
        return QRect(1, y - metrics.height() / 2, axisWidth - 8, metrics.height());
    };
    const auto ticks = buildAmplitudeAxisTicks(channelArea, center, half,
        metrics, theme->domains.curve.metrics.amplitudeGridTargetRowSpacing,
        WaveformRenderer::amplitudeScale(waveStyle, amplitudeExponent_));
    for (const auto& tick : ticks) {
        painter.setPen(theme->domains.curve.colors.grid);
        painter.drawLine(plot.left(), tick.y, plot.right(), tick.y);
        QRect bounds = labelRect(tick.y);
        if (tick.major)
            bounds.moveTop(std::clamp(bounds.top(), channelArea.top(),
                std::max(channelArea.top(), channelArea.bottom() - metrics.height() + 1)));
        if (channelArea.contains(bounds)) {
            painter.setPen(colors.textSecondary);
            painter.drawLine(axisWidth - 4, tick.y, axisWidth - 1, tick.y);
            painter.drawText(bounds, Qt::AlignRight | Qt::AlignVCenter, tick.label);
        }
    }
    if (state_ == State::Ready && data.isValid()) {
        WaveformRenderOptions options;
        options.startFrame = viewStart_ - (detailCovers ? double(detailStart_) : 0.0);
        options.frameCount = viewFrameCount();
        options.sampleCurve = true;
        options.amplitudeExponent = amplitudeExponent_;
        painter.save();
        painter.setClipRect(wave, Qt::IntersectClip);
        WaveformRenderer::render(painter, wave, data, waveStyle, options);
        painter.restore();
    }
    painter.restore();

    painter.setPen(colors.borderPrimary);
    painter.drawLine(plot.left(), 0, plot.left(), plot.bottom());
    painter.drawLine(plot.right() + 1, 0, plot.right() + 1, height());
    painter.drawLine(0, rulerHeight - 1, width(), rulerHeight - 1);
    painter.drawLine(0, plot.bottom() + 1, width(), plot.bottom() + 1);
    painter.save();
    painter.setClipRect(plot.adjusted(-1, 0, 1, 0));
    if (totalFrames() > 0) {
        const auto edge = [&](uint64_t frame) {
            const double ratio = std::clamp((double(frame) - viewStart_)
                / std::max(1.0, viewFrameCount()), 0.0, 1.0);
            return plot.left() + int(std::lround(ratio * (plot.width() - 1)));
        };
        const int left = edge(previewSource_.trimStartFrame);
        const int right = edge(previewSource_.trimEndFrame.value_or(totalFrames()));
        QColor muted = colors.bgPrimary;
        muted.setAlpha(175);
        painter.fillRect(QRect(plot.left(), plot.top(), left - plot.left(), plot.height()), muted);
        painter.fillRect(QRect(right, plot.top(), plot.right() - right + 1, plot.height()), muted);
        QColor selection = waveStyle.trimHandle;
        selection.setAlpha(35);
        painter.fillRect(QRect(left, plot.top(), std::max(0, right - left), kTimeAxisHandleBandHeight), selection);
        for (Handle handle : {Handle::TrimStart, Handle::TrimEnd}) {
            const int x = handlePosition(static_cast<int>(handle));
            if (x < 0)
                continue;
            const bool highlighted = activeHandle_ == handle || hoveredHandle_ == handle;
            const QColor color = highlighted ? waveStyle.trimHandle.lighter(130) : waveStyle.trimHandle;
            painter.setPen(QPen(color, highlighted ? 2 : 1));
            painter.drawLine(x, plot.top(), x, plot.bottom());
            const QRect grip = trimGripRect(handle);
            drawTimeAxisHandle(painter, color, colors.bgPrimary, grip);
        }
    }
    painter.restore();
    if (asset_ && state_ != State::Ready) {
        painter.setFont(FontManager::instance().getFont(FontRole::Caption));
        painter.setPen(colors.textMuted);
        painter.drawText(plot.adjusted(4, kTimeAxisHandleBandHeight, -4, 0), Qt::AlignCenter,
            state_ == State::Generating ? tr("Generating waveform...") : tr("Waveform unavailable"));
    }
}

void SourceEditorWaveformView::requestDetail()
{
    if (detailLoading_ || state_ != State::Ready || !peaks_.isValid() || totalFrames() == 0)
        return;
    const double framesPerPixel = viewFrameCount() / std::max(1, waveformRect().width());
    double overviewResolution = peaks_.samplesPerPeak;
    const double lastPeakStart = double(peaks_.numPeaks - 1) * peaks_.samplesPerPeak;
    if (viewStart_ + viewFrameCount() > lastPeakStart)
        overviewResolution = std::max(overviewResolution, double(totalFrames()) - lastPeakStart);
    if (overviewResolution <= framesPerPixel || peaks_.numPeaks == totalFrames())
        return;
    if (detail_.isValid() && viewStart_ >= double(detailStart_)
        && viewStart_ + viewFrameCount() <= double(detailStart_ + detailCount_)
        && (detail_.samplesPerPeak <= framesPerPixel || detail_.samplesPerPeak == 1))
        return;
    const uint64_t start = static_cast<uint64_t>(std::floor(std::max(0.0, viewStart_ - viewFrameCount() * 0.5)));
    const uint64_t end = static_cast<uint64_t>(std::ceil(std::min(double(totalFrames()),
        viewStart_ + viewFrameCount() * 1.5 + 1.0)));
    const uint32_t targetPeaks = static_cast<uint32_t>(std::clamp(waveformRect().width() * 4, 256, 16384));
    const quint64 revision = requestRevision_;
    const double requestedStart = viewStart_;
    const double requestedCount = viewFrameCount();
    const int requestedWidth = waveformRect().width();
    const QString path = path_;
    auto* watcher = new QFutureWatcher<WaveformPeakData>(this);
    detailLoading_ = true;
    connect(watcher, &QFutureWatcher<WaveformPeakData>::finished, this,
        [this, watcher, revision, start, end, requestedStart, requestedCount, requestedWidth] {
            detailLoading_ = false;
            if (revision == requestRevision_) {
                detail_ = watcher->result();
                detailStart_ = start;
                detailCount_ = end - start;
                update();
            }
            watcher->deleteLater();
            if (revision != requestRevision_ || requestedStart != viewStart_
                || requestedCount != viewFrameCount() || requestedWidth != waveformRect().width())
                detailTimer_.start();
        });
    watcher->setFuture(QtConcurrent::run([path, start, end, targetPeaks] {
        return WaveformCache::readRange(path, start, end - start, targetPeaks);
    }));
}

int SourceEditorWaveformView::secondsDecimals() const
{
    return asset_ && asset_->metadata.sampleRate > 0
        ? std::max(6, int(std::ceil(std::log10(double(asset_->metadata.sampleRate)))) + 1) : 6;
}

TimeAxisTicks SourceEditorWaveformView::timeAxisTicks() const
{
    const double rate = asset_ && asset_->metadata.sampleRate > 0 ? asset_->metadata.sampleRate : 1.0;
    const double unit = frameDisplay_ ? 1.0 : rate;
    return buildTimeAxisTicks(viewStart_ / unit, viewFrameCount() / unit, waveformRect().width(),
        frameDisplay_ ? 1.0 : 1.0 / rate, frameDisplay_ ? 0 : secondsDecimals(),
        QFontMetrics(FontManager::instance().getFont(FontRole::Axis), this));
}

QString SourceEditorWaveformView::formatFrame(uint64_t frame) const
{
    return frameDisplay_ || !asset_ || asset_->metadata.sampleRate == 0
        ? tr("%1 frames").arg(frame)
        : tr("%1 s").arg(formatAxisValue(double(frame) / asset_->metadata.sampleRate,
            axisReadoutDecimalsForStep(timeAxisTicks().majorStep, secondsDecimals())));
}

void SourceEditorWaveformView::showHandleValue()
{
    if (!handleValue_ || activeHandle_ == Handle::None)
        return;
    handleValue_->setText(tr("%1: %2").arg(
        activeHandle_ == Handle::TrimStart ? tr("Trim In") : tr("Trim Out"),
        formatFrame(handleFrame(activeHandle_))));
    handleValue_->adjustSize();
    placeAbove(handleValue_, mapToGlobal(QPoint(
        std::clamp(handlePosition(static_cast<int>(activeHandle_)), waveformRect().left(), waveformRect().right()),
        waveformRect().top())));
    handleValue_->show();
}

void SourceEditorWaveformView::dragHandle(const QPoint& position)
{
    if (position == dragPosition_ && !dragMoved_)
        return;
    dragMoved_ = true;
    dragPosition_ = position;
    updatePreview(frameAt(position.x() - dragOffset_));
    if (position.x() < waveformRect().left() || position.x() > waveformRect().right()) {
        if (!edgeScroll_.isActive())
            edgeScroll_.start();
    } else {
        edgeScroll_.stop();
    }
}

void SourceEditorWaveformView::cancelGesture()
{
    hoverPosition_.reset();
    edgeScroll_.stop();
    panOrigin_.reset();
    panVerticalStart_.reset();
    panButton_ = Qt::NoButton;
    if (handleValue_)
        handleValue_->hide();
    if (valueEditor_)
        valueEditor_->reject();
    if (activeHandle_ != Handle::None) {
        activeHandle_ = Handle::None;
        previewSource_ = originalSource_;
        emit previewChanged();
    }
    hoveredHandle_ = Handle::None;
    unsetCursor();
    update();
}

void SourceEditorWaveformView::editHandleValue(Handle handle)
{
    if (handle == Handle::None || !asset_ || asset_->metadata.sampleRate == 0)
        return;
    cancelGesture();
    QToolTip::hideText();
    auto* dialog = new QDialog(this, Qt::Popup);
    valueEditor_ = dialog;
    dialog->setObjectName(QStringLiteral("sourceTrimValueEditor"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QHBoxLayout(dialog);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(8);
    auto* label = new QLabel(handle == Handle::TrimStart ? tr("Trim In") : tr("Trim Out"), dialog);
    label->setProperty("textRole", QStringLiteral("formLabel"));
    layout->addWidget(label);
    auto* spin = new DecimalSpinBox(dialog);
    spin->setObjectName(QStringLiteral("sourceTrimValueInput"));
    spin->setKeyboardTracking(false);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    const uint64_t initial = handleFrame(handle);
    const double divisor = frameDisplay_ ? 1.0 : asset_->metadata.sampleRate;
    spin->setDecimals(frameDisplay_ ? 0 : secondsDecimals());
    spin->setRange(0.0, double(std::max(totalFrames(), initial)) / divisor);
    spin->setSingleStep(1.0 / divisor);
    spin->setToolTip(frameDisplay_ ? tr("frames") : QStringLiteral("s"));
    spin->setValue(double(initial) / divisor);
    const double initialValue = spin->value();
    spin->setMinimumWidth(158);
    layout->addWidget(spin);
    for (int key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto* shortcut = new QShortcut(QKeySequence(key), dialog);
        connect(shortcut, &QShortcut::activated, dialog, [dialog, spin] {
            spin->interpretText();
            dialog->accept();
        });
    }
    connect(dialog, &QDialog::accepted, this, [this, handle, spin, initial, initialValue, divisor] {
        valueEditor_.clear();
        if (spin->value() == initialValue)
            return;
        const uint64_t frame = static_cast<uint64_t>(std::llround(spin->value() * divisor));
        if (frame == initial)
            return;
        originalSource_ = previewSource_;
        activeHandle_ = handle;
        updatePreview(frame);
        activeHandle_ = Handle::None;
        handleValue_->hide();
        update();
        if (previewSource_ == originalSource_)
            return;
        originalSource_ = previewSource_;
        if (commit_)
            commit_(previewSource_);
    });
    dialog->adjustSize();
    placeAbove(dialog, mapToGlobal(handleHitPoint(static_cast<int>(handle))));
    dialog->show();
    spin->setFocus();
    spin->selectAll();
}

void SourceEditorWaveformView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutTimeline();
    if (units_)
        detailTimer_.start();
}

void SourceEditorWaveformView::wheelEvent(QWheelEvent* event)
{
    if (!units_ || totalFrames() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const QPoint delta = event->angleDelta().isNull() ? event->pixelDelta() : event->angleDelta();
    const double steps = double(delta.y() != 0 ? delta.y() : delta.x())
        / (event->angleDelta().isNull() ? 40.0 : 120.0);
    if (event->modifiers().testFlag(Qt::ControlModifier))
        zoomVertically(int(event->position().y()), std::pow(1.25, steps));
    else
        zoomAround(int(event->position().x()), std::pow(1.25, steps));
    event->accept();
}

bool SourceEditorWaveformView::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Enter && !panOrigin_ && activeHandle_ == Handle::None) {
        hoverPosition_.reset();
        hoveredHandle_ = Handle::None;
        unsetCursor();
        QToolTip::hideText();
        update();
    }
    if (units_ && event->type() == QEvent::Wheel) {
        const auto* source = static_cast<QWheelEvent*>(event);
        QWheelEvent wheel(mapFromGlobal(source->globalPosition()), source->globalPosition(),
            source->pixelDelta(), source->angleDelta(), source->buttons(), source->modifiers(),
            source->phase(), source->inverted(), source->source());
        wheelEvent(&wheel);
        event->setAccepted(wheel.isAccepted());
        return wheel.isAccepted();
    }
    return QWidget::eventFilter(watched, event);
}

void SourceEditorWaveformView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (units_ && event->button() == Qt::LeftButton) {
        const Handle handle = hitTestHandle(event->position().toPoint());
        if (handle != Handle::None) {
            editHandleValue(handle);
        } else if (event->position().x() < waveformRect().left()) {
            verticalZoom_ = 1.0;
            channelScroll_->setValue(0);
            refreshTimelineControls();
            update();
        } else if (isTimeRuler(event->position().toPoint())) {
            setViewRange(0.0, double(totalFrames()));
        }
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void SourceEditorWaveformView::contextMenuEvent(QContextMenuEvent* event)
{
    const QRect plot = waveformRect();
    const QFontMetrics metrics(FontManager::instance().getFont(FontRole::Axis), this);
    const QRect axis = units_
        ? QRect(1, plot.top(), plot.left() - 1, plot.height())
        : QRect(plot.left(), plot.top(), metrics.horizontalAdvance(QStringLiteral("-96.00")) + 8,
            plot.height());
    if (axis.contains(event->pos())) {
        cancelGesture();
        QToolTip::hideText();
        showAmplitudeCurveMenu(this, event->globalPos(), QRect(mapToGlobal(axis.topLeft()), axis.size()),
            amplitudeExponent_, [this](double exponent) { setAmplitudeExponent(exponent); });
        event->accept();
        return;
    }
    if (units_ && editable_) {
        const Handle handle = hitTestHandle(event->pos());
        if (handle != Handle::None) {
            cancelGesture();
            QToolTip::hideText();
            auto* menu = new QMenu(this);
            menu->setObjectName(QStringLiteral("sourceTrimContextMenu"));
            menu->setAttribute(Qt::WA_DeleteOnClose);
            auto* enter = menu->addAction(tr("Enter Value..."), this,
                [this, handle] { editHandleValue(handle); });
            enter->setObjectName(QStringLiteral("sourceTrimEnterValue"));
            menu->popup(event->globalPos());
            event->accept();
            return;
        }
    }
    QWidget::contextMenuEvent(event);
}

void SourceEditorWaveformView::keyPressEvent(QKeyEvent* event)
{
    if (units_ && event->key() == Qt::Key_Escape) {
        cancelGesture();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SourceEditorWaveformView::hideEvent(QHideEvent* event)
{
    cancelGesture();
    QWidget::hideEvent(event);
}

}
