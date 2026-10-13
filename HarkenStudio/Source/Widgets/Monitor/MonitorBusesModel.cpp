#include "Monitor/MonitorBusesModel.h"
#include "Utils/ObjectId.h"

namespace VNX {

MonitorBusesModel::MonitorBusesModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int MonitorBusesModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return rows_.size();
}

int MonitorBusesModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant MonitorBusesModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || role != Qt::DisplayRole)
        return {};
    return cellText(index.row(), index.column());
}

QVariant MonitorBusesModel::headerData(int section,
                                       Qt::Orientation orientation,
                                       int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section)
    {
    case Identity:
        return QStringLiteral("ID");
    case Parent:
        return QStringLiteral("Parent");
    case Mute:
        return QStringLiteral("Mute");
    case Solo:
        return QStringLiteral("Solo");
    case Tree:
        return QStringLiteral("Tree");
    case TapPostEffects:
        return QStringLiteral("Post FX");
    case TapPreFader:
        return QStringLiteral("Pre Fader");
    case TapPostFader:
        return QStringLiteral("Post Fader");
    default:
        return {};
    }
}

Qt::ItemFlags MonitorBusesModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void MonitorBusesModel::setRows(QVector<Monitor::BusMonitorRow> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const Monitor::BusMonitorRow* MonitorBusesModel::rowAt(int row) const
{
    if (row < 0 || row >= rows_.size())
        return nullptr;
    return &rows_[row];
}

QString MonitorBusesModel::cellText(int row, int column) const
{
    const Monitor::BusMonitorRow* entry = rowAt(row);
    if (!entry)
        return {};
    switch (column)
    {
    case Identity:
        return identityText(*entry);
    case Parent:
        return entry->parentPublished ? objectIdString(entry->routeBusId)
                                      : unsupportedText();
    case Mute:
        return entry->mutePublished ? QString::number(entry->mute01) : unsupportedText();
    case Solo:
        return soloText(*entry);
    case Tree:
    case TapPostEffects:
    case TapPreFader:
    case TapPostFader:
        return unsupportedText();
    default:
        return {};
    }
}

QString MonitorBusesModel::identityText(const Monitor::BusMonitorRow& row)
{
    return QStringLiteral("%1:%2").arg(row.instanceHandle).arg(row.instanceGeneration);
}

QString MonitorBusesModel::unsupportedText()
{
    return QStringLiteral("Unsupported");
}

QString MonitorBusesModel::soloText(const Monitor::BusMonitorRow& row)
{
    if (!row.soloAuthoredPublished)
        return unsupportedText();
    return QStringLiteral("%1 (authored; explicit/implicit undistinguished)")
        .arg(row.solo01);
}

} // namespace VNX
