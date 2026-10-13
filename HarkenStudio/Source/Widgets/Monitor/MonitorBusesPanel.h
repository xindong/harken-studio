#pragma once

#include "Monitor/BusTopologyProjection.h"
#include "Monitor/MonitorBusesModel.h"
#include "Monitor/MonitorStore.h"
#include "TimeSeries/MonitorCursorModel.h"

#include <QString>
#include <QVariantMap>
#include <QWidget>

#include <cstdint>
#include <memory>

class QLabel;
class QTableView;

namespace VNX {

class MonitorBusesPanel : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorBusesPanel(QWidget* parent = nullptr);
    ~MonitorBusesPanel() override;

    QTableView* table() const { return table_; }
    MonitorBusesModel* tableModel() const { return model_; }
    void setSharedCursor(MonitorCursorModel* cursor);
    MonitorCursorModel& cursorModel();
    const MonitorCursorModel& cursorModel() const;

    void bindSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
                      bool stateStreamSubscribed = true);
    std::uint64_t boundGeneration() const { return boundGeneration_; }
    Monitor::MonitorBusesTableState tableState() const { return tableState_; }
    QString gapMessage() const;
    int visibleRowCount() const;
    const Monitor::BusMonitorRow* visibleRowAt(int row) const;
    bool treeViewSupported() const { return false; }

    QVariantMap snapshotParams() const;
    void restoreParams(const QVariantMap& params);

private:
    void rebuildVisibleRows();
    void syncCursorRetainedWindow();
    void refreshGapLabel();
    MonitorCursorReject lockRow(int row);
    void onTableClicked(const QModelIndex& index);
    void onSharedCursor(const MonitorCursorSnapshot& snapshot);
    void unsubscribeCursor();
    void subscribeCursor();

    QLabel* gapLabel_ = nullptr;
    QTableView* table_ = nullptr;
    MonitorBusesModel* model_ = nullptr;

    MonitorCursorModel ownedCursor_;
    MonitorCursorModel* sharedCursor_ = nullptr;
    MonitorCursorModel* subscribedCursor_ = nullptr;
    std::uint64_t cursorToken_ = 0;
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot_;
    std::uint64_t boundGeneration_ = 0;
    Monitor::MonitorBusesTableState tableState_ = Monitor::MonitorBusesTableState::UnsupportedLayout;
};

} // namespace VNX
