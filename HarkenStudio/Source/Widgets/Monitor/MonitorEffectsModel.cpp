#include "Monitor/MonitorEffectsModel.h"

namespace VNX {

MonitorEffectsModel::MonitorEffectsModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int MonitorEffectsModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return rows_.size();
}

int MonitorEffectsModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant MonitorEffectsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || role != Qt::DisplayRole)
        return {};
    return cellText(index.row(), index.column());
}

QVariant MonitorEffectsModel::headerData(int section,
                                         Qt::Orientation orientation,
                                         int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section)
    {
    case Identity:
        return QStringLiteral("ID");
    case Bypass:
        return QStringLiteral("Bypass");
    case Tail:
        return QStringLiteral("Tail");
    case Latency:
        return QStringLiteral("Latency");
    case Params:
        return QStringLiteral("Params");
    case GainReduction:
        return QStringLiteral("GR");
    default:
        return {};
    }
}

Qt::ItemFlags MonitorEffectsModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void MonitorEffectsModel::setRows(QVector<Monitor::EffectMonitorRow> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const Monitor::EffectMonitorRow* MonitorEffectsModel::rowAt(int row) const
{
    if (row < 0 || row >= rows_.size())
        return nullptr;
    return &rows_[row];
}

QString MonitorEffectsModel::cellText(int row, int column) const
{
    const Monitor::EffectMonitorRow* entry = rowAt(row);
    if (!entry)
        return {};
    switch (column)
    {
    case Identity:
        return identityText(*entry);
    case Bypass:
        return entry->bypassPublished ? QString::number(entry->bypass01)
                                      : cellStateText(Monitor::EffectCellState::Gap);
    case Tail:
        return entry->tailPublished ? QString::number(entry->tailFrames)
                                    : cellStateText(Monitor::EffectCellState::Gap);
    case Latency:
        return latencyText(*entry);
    case Params:
        return cellStateText(entry->paramsState);
    case GainReduction:
        return cellStateText(entry->grState);
    default:
        return {};
    }
}

QString MonitorEffectsModel::identityText(const Monitor::EffectMonitorRow& row)
{
    return QStringLiteral("%1:%2").arg(row.instanceHandle).arg(row.instanceGeneration);
}

QString MonitorEffectsModel::cellStateText(Monitor::EffectCellState state)
{
    switch (state)
    {
    case Monitor::EffectCellState::Unavailable:
        return QStringLiteral("Unavailable");
    case Monitor::EffectCellState::Unsupported:
        return QStringLiteral("Unsupported");
    case Monitor::EffectCellState::Gap:
        return QStringLiteral("Gap");
    case Monitor::EffectCellState::Numeric:
        return {};
    }
    return QStringLiteral("Gap");
}

QString MonitorEffectsModel::latencyText(const Monitor::EffectMonitorRow& row)
{
    if (row.latencyState != Monitor::EffectCellState::Numeric)
        return cellStateText(row.latencyState);
    return QString::number(row.latencyFrames);
}

} // namespace VNX
