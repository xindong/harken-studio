#pragma once

#include "Monitor/EffectMonitorProjection.h"

#include <QAbstractTableModel>
#include <QString>
#include <QVector>

namespace VNX {

class MonitorEffectsModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column
    {
        Identity = 0,
        Bypass,
        Tail,
        Latency,
        Params,
        GainReduction,
        ColumnCount
    };

    explicit MonitorEffectsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void setRows(QVector<Monitor::EffectMonitorRow> rows);
    const Monitor::EffectMonitorRow* rowAt(int row) const;
    QString cellText(int row, int column) const;

    static QString identityText(const Monitor::EffectMonitorRow& row);
    static QString cellStateText(Monitor::EffectCellState state);
    static QString latencyText(const Monitor::EffectMonitorRow& row);

private:
    QVector<Monitor::EffectMonitorRow> rows_;
};

} // namespace VNX
