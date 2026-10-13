#pragma once

#include "Monitor/MonitorVoicesModel.h"
#include "TimeSeries/MonitorCursorModel.h"

#include <QString>
#include <QVariantMap>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>

class QLabel;
class QLineEdit;
class QTableView;

namespace VNX {

class MonitorVoicesPanel : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorVoicesPanel(QWidget* parent = nullptr);
    ~MonitorVoicesPanel() override;

    QTableView* table() const { return table_; }
    MonitorVoicesModel* tableModel() const { return model_; }
    void setSharedCursor(MonitorCursorModel* cursor);
    MonitorCursorModel& cursorModel();
    const MonitorCursorModel& cursorModel() const;

    void bindSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
                      bool stateStreamSubscribed = true);
    std::uint64_t boundGeneration() const { return boundGeneration_; }
    MonitorVoicesTableState tableState() const { return tableState_; }
    QString gapMessage() const;

    QVariantMap snapshotParams() const;
    void restoreParams(const QVariantMap& params);

    MonitorCursorReject lockCurrentRow();
    MonitorCursorReject lockExecuteFrameForTest(std::optional<qint64> executeFrame);

    int visibleRowCount() const;
    const MonitorVoicesRow* visibleRowAt(int row) const;
    const MonitorVoicesRow* selectedRow() const;

private:
    void rebuildVisibleRows();
    void syncCursorRetainedWindow();
    void refreshGapLabel();
    bool rowMatchesFilter(const MonitorVoicesRow& row) const;
    MonitorCursorReject lockRow(int row);
    void onTableClicked(const QModelIndex& index);
    void onSharedCursor(const MonitorCursorSnapshot& snapshot);
    void unsubscribeCursor();
    void subscribeCursor();

    QLabel* gapLabel_ = nullptr;
    QLineEdit* objectFilter_ = nullptr;
    QTableView* table_ = nullptr;
    MonitorVoicesModel* model_ = nullptr;

    MonitorCursorModel ownedCursor_;
    MonitorCursorModel* sharedCursor_ = nullptr;
    MonitorCursorModel* subscribedCursor_ = nullptr;
    std::uint64_t cursorToken_ = 0;
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot_;
    QVector<MonitorVoicesRow> allRows_;
    std::uint64_t boundGeneration_ = 0;
    MonitorVoicesTableState tableState_ = MonitorVoicesTableState::UnsupportedLayout;
    bool stateStreamSubscribed_ = true;
    QString filterObject_;
};

} // namespace VNX
