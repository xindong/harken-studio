#pragma once

#include "Monitor/MonitorStore.h"
#include "TimeSeries/MonitorCursorModel.h"

#include <QString>
#include <QVariantMap>
#include <QWidget>

#include <cstdint>
#include <memory>

class QLabel;

namespace VNX {

class MonitorTapSelectPanel : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorTapSelectPanel(QWidget* parent = nullptr);
    ~MonitorTapSelectPanel() override;

    void setSharedCursor(MonitorCursorModel* cursor);
    MonitorCursorModel& cursorModel();
    const MonitorCursorModel& cursorModel() const;

    void bindSnapshot(std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
                      bool stateStreamSubscribed = true);
    std::uint64_t boundGeneration() const { return boundGeneration_; }
    QString gapMessage() const;
    int visibleRowCount() const { return 0; }
    bool jumpControlEnabled() const { return false; }

    QVariantMap snapshotParams() const;
    void restoreParams(const QVariantMap& params);

private:
    void refreshFace();
    void onSharedCursor(const MonitorCursorSnapshot& snapshot);
    void unsubscribeCursor();
    void subscribeCursor();

    QLabel* gapLabel_ = nullptr;

    MonitorCursorModel ownedCursor_;
    MonitorCursorModel* sharedCursor_ = nullptr;
    MonitorCursorModel* subscribedCursor_ = nullptr;
    std::uint64_t cursorToken_ = 0;
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot_;
    std::uint64_t boundGeneration_ = 0;
};

} // namespace VNX
