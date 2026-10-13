#pragma once

#include "Monitor/EffectMonitorProjection.h"
#include "Monitor/MonitorEffectsModel.h"
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

class MonitorEffectsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorEffectsPanel(QWidget* parent = nullptr);
    ~MonitorEffectsPanel() override;

    QTableView* table() const { return table_; }
    MonitorEffectsModel* tableModel() const { return model_; }
    void setSharedCursor(MonitorCursorModel* cursor);
    MonitorCursorModel& cursorModel();
    const MonitorCursorModel& cursorModel() const;

    void bindSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
                      bool stateStreamSubscribed = true);
    std::uint64_t boundGeneration() const { return boundGeneration_; }
    Monitor::MonitorEffectsTableState tableState() const { return tableState_; }
    QString gapMessage() const;
    int visibleRowCount() const;
    const Monitor::EffectMonitorRow* visibleRowAt(int row) const;

    QVariantMap snapshotParams() const;
    void restoreParams(const QVariantMap& params);

    MonitorCursorReject lockCurrentRow();
    MonitorCursorReject lockExecuteFrameForTest(std::optional<qint64> executeFrame);

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
    MonitorEffectsModel* model_ = nullptr;

    MonitorCursorModel ownedCursor_;
    MonitorCursorModel* sharedCursor_ = nullptr;
    MonitorCursorModel* subscribedCursor_ = nullptr;
    std::uint64_t cursorToken_ = 0;
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot_;
    std::uint64_t boundGeneration_ = 0;
    Monitor::MonitorEffectsTableState tableState_ = Monitor::MonitorEffectsTableState::UnsupportedLayout;
};

} // namespace VNX
