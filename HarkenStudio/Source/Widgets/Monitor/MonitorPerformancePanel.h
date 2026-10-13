#pragma once

#include "Monitor/CapturePlanIntent.h"
#include "Monitor/MonitorCaptureControl.h"
#include "Monitor/MonitorRemoteStatus.h"
#include "TimeSeries/MonitorCursorMark.h"
#include "TimeSeries/MonitorCursorModel.h"
#include "TimeSeries/TimeSeriesChartModel.h"

#include <QHash>
#include <QPoint>
#include <QString>
#include <QVariantMap>
#include <QWidget>

class QCheckBox;
class QVBoxLayout;

namespace VNX {

class TimeSeriesChart;
class MonitorCaptureControlStrip;
class MonitorRemoteStatusStrip;

class MonitorPerformancePanel : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorPerformancePanel(QWidget* parent = nullptr);
    ~MonitorPerformancePanel() override;

    TimeSeriesChart* chart() const { return chart_; }
    void setSharedCursor(MonitorCursorModel* cursor);
    MonitorCursorModel& cursorModel();
    const MonitorCursorModel& cursorModel() const;
    MonitorCursorMark* cursorMark() const { return cursorMark_; }
    const TimeSeriesChartModel& chartModel() const { return chartModel_; }

    void setTimeSeriesAvailable(bool available);
    bool timeSeriesAvailable() const { return timeSeriesAvailable_; }
    void applyCaptureControlState(const Monitor::MonitorCaptureControlState& state);
    void applyRemoteStatusState(const Monitor::MonitorRemoteStatusState& state);
    void applyCapturePlanPreview(const Monitor::CaptureBudgetPreview& preview);
    void setCaptureControlStatusText(const QString& text);
    QString captureControlStatusText() const;
    QString remoteStatusFaceText() const;
    QString remoteStatusPermissionText() const;
    QString remoteRecordDeniedReason() const;
    bool remoteControlEnabled() const;
    bool remoteRecordEnabled() const;
    QString capturePlanPreviewText() const;
    bool capturePlanApplyEnabled() const;

    bool setWindow(qint64 start, qint64 end);
    bool setSeriesVisible(const QString& id, bool visible);
    bool paused() const { return paused_; }
    void setPaused(bool paused);

    bool applyChartModel(const TimeSeriesChartModel& model);
    QVariantMap snapshotParams() const;
    void restoreParams(const QVariantMap& params);

    MonitorCursorReject lockFromChartX(int logicalX);
    bool advanceLiveForTest(qint64 executeFrame);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildSeriesToggles();
    void syncCursorRetainedWindow();
    void pushModelToChart();
    qint64 frameFromLogicalX(int logicalX) const;
    void onSharedCursor(const MonitorCursorSnapshot& snapshot);
    void unsubscribeCursor();
    void subscribeCursor();

    TimeSeriesChart* chart_ = nullptr;
    MonitorCaptureControlStrip* captureControlStrip_ = nullptr;
    MonitorRemoteStatusStrip* remoteStatusStrip_ = nullptr;
    MonitorCursorMark* cursorMark_ = nullptr;
    QVBoxLayout* seriesLayout_ = nullptr;
    QHash<QString, QCheckBox*> seriesToggles_;
    TimeSeriesChartModel chartModel_;
    MonitorCursorModel ownedCursor_;
    MonitorCursorModel* sharedCursor_ = nullptr;
    MonitorCursorModel* subscribedCursor_ = nullptr;
    std::uint64_t cursorToken_ = 0;
    bool timeSeriesAvailable_ = false;
    bool paused_ = false;
    QPoint pressPos_;
    bool trackingPress_ = false;
};

} // namespace VNX
