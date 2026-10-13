#pragma once

#include "Monitor/BusTopologyProjection.h"

#include <QAbstractTableModel>
#include <QString>
#include <QVector>

namespace VNX {

class MonitorBusesModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column
    {
        Identity = 0,
        Parent,
        Mute,
        Solo,
        Tree,
        TapPostEffects,
        TapPreFader,
        TapPostFader,
        ColumnCount
    };

    explicit MonitorBusesModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void setRows(QVector<Monitor::BusMonitorRow> rows);
    const Monitor::BusMonitorRow* rowAt(int row) const;
    QString cellText(int row, int column) const;

    static QString identityText(const Monitor::BusMonitorRow& row);
    static QString unsupportedText();
    static QString soloText(const Monitor::BusMonitorRow& row);

private:
    QVector<Monitor::BusMonitorRow> rows_;
};

} // namespace VNX
