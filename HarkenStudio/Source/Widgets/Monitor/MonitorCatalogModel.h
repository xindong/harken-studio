#pragma once

#include "Monitor/EventCaptureController.h"

#include <QAbstractTableModel>
#include <QString>

namespace VNX {

class MonitorCatalogModel final : public QAbstractTableModel
{
public:
    enum Column { Time, Level, Type, Description, RequestId, Object, ObjectId, GameObject, GameObjectId, ColumnCount };
    explicit MonitorCatalogModel(Monitor::EventCaptureController* capture, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    const Monitor::CapturedEvent* eventAt(int row) const;
    QString details(int row) const;
    static QString levelText(const Monitor::CapturedEvent& event);
    static QString typeText(const Monitor::CapturedEvent& event);
    static QString description(const Monitor::CapturedEvent& event);

private:
    QPointer<Monitor::EventCaptureController> capture_;
};

} // namespace VNX
