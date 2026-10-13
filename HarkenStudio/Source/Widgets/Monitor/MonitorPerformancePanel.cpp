#include "Monitor/MonitorPerformancePanel.h"
#include "Monitor/MonitorCaptureControlStrip.h"
#include "Monitor/MonitorRemoteStatusStrip.h"
#include "Monitor/MonitorPanelKeys.h"

#include "TimeSeries/TimeSeriesChart.h"

#include <QCheckBox>
#include <QEvent>
#include <QMouseEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace VNX {
namespace {

constexpr int kClickSlopPx = 4;

} // namespace

MonitorPerformancePanel::MonitorPerformancePanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorPerformancePanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    captureControlStrip_ = new MonitorCaptureControlStrip(this);
    root->addWidget(captureControlStrip_);

    remoteStatusStrip_ = new MonitorRemoteStatusStrip(this);
    remoteStatusStrip_->hide();
    root->addWidget(remoteStatusStrip_);

    chart_ = new TimeSeriesChart(this);
    chart_->setObjectName(QStringLiteral("MonitorPerformanceChart"));
    chart_->installEventFilter(this);
    root->addWidget(chart_, 1);

    cursorMark_ = new MonitorCursorMark(this);
    cursorMark_->setObjectName(QStringLiteral("MonitorPerformanceCursorMark"));
    root->addWidget(cursorMark_);

    seriesLayout_ = new QVBoxLayout();
    seriesLayout_->setContentsMargins(0, 0, 0, 0);
    root->addLayout(seriesLayout_);

    subscribeCursor();
}

MonitorPerformancePanel::~MonitorPerformancePanel()
{
    if (subscribedCursor_ == &ownedCursor_)
        unsubscribeCursor();
    else
    {
        subscribedCursor_ = nullptr;
        cursorToken_ = 0;
        sharedCursor_ = nullptr;
    }
}

void MonitorPerformancePanel::setTimeSeriesAvailable(bool available)
{
    timeSeriesAvailable_ = available;
    if (chart_)
        chart_->setVisible(available);
}

void MonitorPerformancePanel::applyCaptureControlState(const Monitor::MonitorCaptureControlState& state)
{
    if (captureControlStrip_)
        captureControlStrip_->applyState(state);
}

void MonitorPerformancePanel::applyRemoteStatusState(const Monitor::MonitorRemoteStatusState& state)
{
    if (remoteStatusStrip_)
    {
        remoteStatusStrip_->applyState(state);
        remoteStatusStrip_->setVisible(state.face != Monitor::MonitorRemoteFace::Unsupported);
    }
}

void MonitorPerformancePanel::applyCapturePlanPreview(const Monitor::CaptureBudgetPreview& preview)
{
    if (captureControlStrip_)
        captureControlStrip_->applyPreview(preview);
}

void MonitorPerformancePanel::setCaptureControlStatusText(const QString& text)
{
    if (captureControlStrip_)
        captureControlStrip_->setStatusText(text);
}

QString MonitorPerformancePanel::captureControlStatusText() const
{
    return captureControlStrip_ ? captureControlStrip_->statusText() : QString();
}

QString MonitorPerformancePanel::remoteStatusFaceText() const
{
    return remoteStatusStrip_ ? remoteStatusStrip_->faceText() : QString();
}

QString MonitorPerformancePanel::remoteStatusPermissionText() const
{
    return remoteStatusStrip_ ? remoteStatusStrip_->permissionText() : QString();
}

QString MonitorPerformancePanel::remoteRecordDeniedReason() const
{
    return remoteStatusStrip_ ? remoteStatusStrip_->recordDeniedReason() : QString();
}

bool MonitorPerformancePanel::remoteControlEnabled() const
{
    return remoteStatusStrip_ && remoteStatusStrip_->controlEnabled();
}

bool MonitorPerformancePanel::remoteRecordEnabled() const
{
    return remoteStatusStrip_ && remoteStatusStrip_->recordEnabled();
}

QString MonitorPerformancePanel::capturePlanPreviewText() const
{
    return captureControlStrip_ ? captureControlStrip_->previewText() : QString();
}

bool MonitorPerformancePanel::capturePlanApplyEnabled() const
{
    return captureControlStrip_ && captureControlStrip_->applyEnabled();
}

bool MonitorPerformancePanel::setWindow(qint64 start, qint64 end)
{
    if (!chartModel_.setWindow(start, end))
        return false;
    pushModelToChart();
    return true;
}

bool MonitorPerformancePanel::setSeriesVisible(const QString& id, bool visible)
{
    if (!chartModel_.setSeriesVisible(id.toStdString(), visible))
        return false;
    if (QCheckBox* box = seriesToggles_.value(id))
        box->setChecked(visible);
    pushModelToChart();
    return true;
}

void MonitorPerformancePanel::setPaused(bool paused)
{
    paused_ = paused;
}

bool MonitorPerformancePanel::applyChartModel(const TimeSeriesChartModel& model)
{
    chartModel_ = model;
    timeSeriesAvailable_ = !chartModel_.isEmptyView() || !chartModel_.series().empty();
    syncCursorRetainedWindow();
    rebuildSeriesToggles();
    pushModelToChart();
    if (chart_)
        chart_->setVisible(timeSeriesAvailable_);
    return true;
}

QVariantMap MonitorPerformancePanel::snapshotParams() const
{
    QVariantMap params;
    params.insert(QStringLiteral("windowStart"), static_cast<qlonglong>(chartModel_.windowStart()));
    params.insert(QStringLiteral("windowEnd"), static_cast<qlonglong>(chartModel_.windowEnd()));
    params.insert(QStringLiteral("paused"), paused_);
    QVariantList visible;
    for (const auto& series : chartModel_.series())
    {
        QVariantMap row;
        row.insert(QStringLiteral("id"), QString::fromStdString(series.id));
        row.insert(QStringLiteral("visible"), series.visible);
        visible.append(row);
    }
    params.insert(QStringLiteral("series"), visible);
    return params;
}

void MonitorPerformancePanel::restoreParams(const QVariantMap& params)
{
    const qint64 start = params.value(QStringLiteral("windowStart")).toLongLong();
    const qint64 end = params.value(QStringLiteral("windowEnd")).toLongLong();
    if (end > start)
        chartModel_.setWindow(start, end);
    paused_ = params.value(QStringLiteral("paused")).toBool();

    const QVariantList series = params.value(QStringLiteral("series")).toList();
    for (const QVariant& row : series)
    {
        const QVariantMap map = row.toMap();
        const QString id = map.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        chartModel_.setSeriesVisible(id.toStdString(), map.value(QStringLiteral("visible")).toBool());
    }
    rebuildSeriesToggles();
    syncCursorRetainedWindow();
    pushModelToChart();
}

void MonitorPerformancePanel::unsubscribeCursor()
{
    if (subscribedCursor_ && cursorToken_ != 0)
        subscribedCursor_->unsubscribe(cursorToken_);
    subscribedCursor_ = nullptr;
    cursorToken_ = 0;
}

void MonitorPerformancePanel::subscribeCursor()
{
    unsubscribeCursor();
    subscribedCursor_ = &cursorModel();
    cursorToken_ = subscribedCursor_->subscribe(
        [this](const MonitorCursorSnapshot& snapshot) { onSharedCursor(snapshot); });
}

void MonitorPerformancePanel::setSharedCursor(MonitorCursorModel* cursor)
{
    unsubscribeCursor();
    sharedCursor_ = cursor;
    subscribeCursor();
}

MonitorCursorModel& MonitorPerformancePanel::cursorModel()
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

const MonitorCursorModel& MonitorPerformancePanel::cursorModel() const
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

void MonitorPerformancePanel::onSharedCursor(const MonitorCursorSnapshot& snapshot)
{
    if (cursorMark_)
        cursorMark_->updateData(snapshot);
}

MonitorCursorReject MonitorPerformancePanel::lockFromChartX(int logicalX)
{
    return cursorModel().lockTo(frameFromLogicalX(logicalX));
}

bool MonitorPerformancePanel::advanceLiveForTest(qint64 executeFrame)
{
    if (paused_)
        return false;
    return cursorModel().advanceLive(executeFrame);
}

bool MonitorPerformancePanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != chart_)
        return QWidget::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress)
    {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton)
        {
            trackingPress_ = true;
            pressPos_ = mouse->pos();
        }
    }
    else if (event->type() == QEvent::MouseButtonRelease)
    {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton)
        {
            if (trackingPress_)
            {
                const QPoint delta = mouse->pos() - pressPos_;
                if (std::abs(delta.x()) <= kClickSlopPx && std::abs(delta.y()) <= kClickSlopPx)
                    lockFromChartX(mouse->pos().x());
            }
            trackingPress_ = false;
        }
    }
    return false;
}

void MonitorPerformancePanel::rebuildSeriesToggles()
{
    if (!seriesLayout_)
        return;
    qDeleteAll(seriesToggles_);
    seriesToggles_.clear();
    for (const auto& series : chartModel_.series())
    {
        const QString id = QString::fromStdString(series.id);
        auto* box = new QCheckBox(id, this);
        box->setObjectName(QStringLiteral("MonitorSeriesToggle_%1").arg(id));
        box->setChecked(series.visible);
        connect(box, &QCheckBox::toggled, this, [this, id](bool checked) {
            chartModel_.setSeriesVisible(id.toStdString(), checked);
            pushModelToChart();
        });
        seriesLayout_->addWidget(box);
        seriesToggles_.insert(id, box);
    }
}

void MonitorPerformancePanel::syncCursorRetainedWindow()
{
    std::int64_t minX = 0;
    std::int64_t maxX = 0;
    if (!chartModel_.retainedRange(&minX, &maxX) || maxX < minX)
    {
        cursorModel().setContributorWindow(
            kMonitorCursorContributorPerformance, std::nullopt, std::nullopt);
        return;
    }
    cursorModel().setContributorWindow(
        kMonitorCursorContributorPerformance, minX, maxX + 1);
}

void MonitorPerformancePanel::pushModelToChart()
{
    if (chart_)
        chart_->updateData(chartModel_);
}

qint64 MonitorPerformancePanel::frameFromLogicalX(int logicalX) const
{
    const auto start = chartModel_.windowStart();
    const auto end = chartModel_.windowEnd();
    const int width = chart_ ? chart_->width() : 0;
    if (width <= 0 || end <= start)
        return start;
    const auto clamped = std::clamp(logicalX, 0, width - 1);
    return columnSampleX(start, end, width, clamped);
}

} // namespace VNX
