#pragma once

#include "Monitor/MonitorDrop.h"
#include "Monitor/MonitorStateCut.h"
#include "Monitor/MonitorStore.h"
#include "TimeSeries/MonitorCursorModel.h"

#include <QAbstractTableModel>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace VNX {

enum class MonitorVoicesVolumeState
{
    Numeric = 0,
    Gap,
    Unsupported,
    Unsubscribed,
};

enum class MonitorVoicesTableState
{
    UnsupportedLayout = 0,
    NoLegalFrame,
    EmptyFrame,
    Rows,
    Incomplete,
};

struct MonitorVoicesRow
{
    std::uint64_t instanceHandle = 0;
    std::uint64_t instanceGeneration = 0;
    std::uint64_t executeFrame = 0;
    std::uint32_t reason = 0;
    float volumeDb = 0.0f;
    MonitorVoicesVolumeState volumeState = MonitorVoicesVolumeState::Gap;
    std::uint64_t storeGeneration = 0;
};

class MonitorVoicesModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column
    {
        Identity = 0,
        Volume,
        Pitch,
        Filter,
        Cpu,
        ColumnCount
    };

    explicit MonitorVoicesModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void setRows(QVector<MonitorVoicesRow> rows);
    const MonitorVoicesRow* rowAt(int row) const;
    QString cellText(int row, int column) const;

    static QString identityText(const MonitorVoicesRow& row);
    static QString volumeText(const MonitorVoicesRow& row);
    static QString gapColumnText();

private:
    QVector<MonitorVoicesRow> rows_;
};

struct MonitorVoicesProjection
{
    MonitorVoicesTableState tableState = MonitorVoicesTableState::UnsupportedLayout;
    QVector<MonitorVoicesRow> rows;
};

MonitorVoicesProjection projectVoiceRows(
    const std::shared_ptr<const Monitor::MonitorStoreSnapshot>& snapshot,
    const MonitorCursorSnapshot& cursor,
    bool stateStreamSubscribed);

} // namespace VNX
