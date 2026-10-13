#include "Monitor/MonitorVoicesModel.h"

#include "Monitor/MonitorTypes.h"

#include "Monitor/MonitorDrop.h"
#include "Monitor/MonitorStateCut.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace VNX {
namespace {

struct VoiceKey
{
    std::uint64_t handle = 0;
    std::uint64_t generation = 0;

    bool operator<(const VoiceKey& other) const
    {
        if (handle != other.handle)
            return handle < other.handle;
        return generation < other.generation;
    }
};

bool hasUnsupportedVolume(std::uint32_t validityFlags)
{
    return (validityFlags & VNX::Monitor::kMonitorValidUnsupported) != 0;
}

bool hasNumericVolume(std::uint32_t validityFlags)
{
    return (validityFlags & VNX::Monitor::kMonitorValidNumeric) != 0
        && !hasUnsupportedVolume(validityFlags);
}

bool voiceReplayIsIncomplete(
    const std::shared_ptr<const Monitor::MonitorStoreSnapshot>& snapshot)
{
    if (!snapshot)
        return true;

    std::set<std::pair<std::uint64_t, std::uint64_t>> voiceStreams;
    for (const auto& rec : snapshot->records)
    {
        if (rec.header.entityKind != VNX::Monitor::kMonitorEntityVoice)
            continue;
        if (rec.header.recordKind != VNX::Monitor::kMonitorRecordStateUpsert
            && rec.header.recordKind != VNX::Monitor::kMonitorRecordStateTombstone)
            continue;
        if (rec.header.streamId == 0)
            continue;
        voiceStreams.emplace(rec.header.monitorSessionEpoch, rec.header.streamId);
    }

    const auto drops = Monitor::projectDrop(snapshot);
    for (const auto& reason : drops.reasons)
    {
        switch (reason.kind)
        {
        case Monitor::MonitorDropReasonKind::StageGap:
        case Monitor::MonitorDropReasonKind::StoreLocalSaturation:
        case Monitor::MonitorDropReasonKind::MalformedDrop:
            return true;
        case Monitor::MonitorDropReasonKind::BaselineMissing:
            if (voiceStreams.count({reason.epoch, reason.streamId}) != 0)
                return true;
            break;
        default:
            break;
        }
    }
    return Monitor::projectStateCut(snapshot).mandatoryResync;
}

} // namespace

MonitorVoicesModel::MonitorVoicesModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int MonitorVoicesModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return rows_.size();
}

int MonitorVoicesModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant MonitorVoicesModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || role != Qt::DisplayRole)
        return {};
    return cellText(index.row(), index.column());
}

QVariant MonitorVoicesModel::headerData(int section,
                                        Qt::Orientation orientation,
                                        int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section)
    {
    case Identity:
        return QStringLiteral("ID");
    case Volume:
        return QStringLiteral("Volume");
    case Pitch:
        return QStringLiteral("Pitch");
    case Filter:
        return QStringLiteral("Filter");
    case Cpu:
        return QStringLiteral("CPU");
    default:
        return {};
    }
}

Qt::ItemFlags MonitorVoicesModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void MonitorVoicesModel::setRows(QVector<MonitorVoicesRow> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const MonitorVoicesRow* MonitorVoicesModel::rowAt(int row) const
{
    if (row < 0 || row >= rows_.size())
        return nullptr;
    return &rows_[row];
}

QString MonitorVoicesModel::cellText(int row, int column) const
{
    const MonitorVoicesRow* entry = rowAt(row);
    if (!entry)
        return {};
    switch (column)
    {
    case Identity:
        return identityText(*entry);
    case Volume:
        return volumeText(*entry);
    case Pitch:
    case Filter:
    case Cpu:
        return gapColumnText();
    default:
        return {};
    }
}

QString MonitorVoicesModel::identityText(const MonitorVoicesRow& row)
{
    return QStringLiteral("%1:%2").arg(row.instanceHandle).arg(row.instanceGeneration);
}

QString MonitorVoicesModel::volumeText(const MonitorVoicesRow& row)
{
    switch (row.volumeState)
    {
    case MonitorVoicesVolumeState::Unsubscribed:
        return QStringLiteral("Not subscribed");
    case MonitorVoicesVolumeState::Unsupported:
        return QStringLiteral("Unsupported");
    case MonitorVoicesVolumeState::Gap:
        return QStringLiteral("Gap");
    case MonitorVoicesVolumeState::Numeric:
        return QString::number(static_cast<double>(row.volumeDb), 'f', 1);
    }
    return QStringLiteral("Gap");
}

QString MonitorVoicesModel::gapColumnText()
{
    return QStringLiteral("Unsupported");
}

MonitorVoicesProjection projectVoiceRows(
    const std::shared_ptr<const Monitor::MonitorStoreSnapshot>& snapshot,
    const MonitorCursorSnapshot& cursor,
    bool stateStreamSubscribed)
{
    MonitorVoicesProjection out;
    if (!snapshot)
    {
        out.tableState = MonitorVoicesTableState::UnsupportedLayout;
        return out;
    }

    std::vector<const Monitor::MonitorDecodedRecord*> voiceState;
    bool sawVoiceLayout = false;
    for (const auto& rec : snapshot->records)
    {
        if (rec.header.entityKind != VNX::Monitor::kMonitorEntityVoice)
            continue;
        if (rec.header.recordKind != VNX::Monitor::kMonitorRecordStateUpsert
            && rec.header.recordKind != VNX::Monitor::kMonitorRecordStateTombstone)
            continue;
        sawVoiceLayout = true;
        voiceState.push_back(&rec);
    }
    if (!sawVoiceLayout)
    {
        out.tableState = MonitorVoicesTableState::UnsupportedLayout;
        return out;
    }
    if (voiceReplayIsIncomplete(snapshot))
    {
        out.tableState = MonitorVoicesTableState::Incomplete;
        return out;
    }
    if (!cursor.executeFrame.has_value())
    {
        out.tableState = MonitorVoicesTableState::NoLegalFrame;
        return out;
    }

    const auto cursorFrame = static_cast<std::uint64_t>(*cursor.executeFrame);
    std::sort(voiceState.begin(), voiceState.end(),
              [](const Monitor::MonitorDecodedRecord* a, const Monitor::MonitorDecodedRecord* b) {
                  if (a->header.sourceSequence != b->header.sourceSequence)
                      return a->header.sourceSequence < b->header.sourceSequence;
                  return a->header.recordKind < b->header.recordKind;
              });

    std::map<VoiceKey, MonitorVoicesRow> live;
    for (const Monitor::MonitorDecodedRecord* rec : voiceState)
    {
        if (rec->header.executeFrame > cursorFrame)
            continue;
        VoiceKey key{rec->header.instanceHandle, rec->header.instanceGeneration};
        if (rec->header.recordKind == VNX::Monitor::kMonitorRecordStateTombstone)
        {
            live.erase(key);
            continue;
        }
        MonitorVoicesRow row;
        row.instanceHandle = rec->header.instanceHandle;
        row.instanceGeneration = rec->header.instanceGeneration;
        row.executeFrame = rec->header.executeFrame;
        row.reason = rec->header.reason;
        row.storeGeneration = snapshot->generation;
        if (!stateStreamSubscribed)
        {
            row.volumeState = MonitorVoicesVolumeState::Unsubscribed;
        }
        else if (hasUnsupportedVolume(rec->header.validityFlags))
        {
            row.volumeState = MonitorVoicesVolumeState::Unsupported;
        }
        else if (hasNumericVolume(rec->header.validityFlags)
                 && rec->payload.size() >= sizeof(VNX::Monitor::MonitorStateUpsert))
        {
            VNX::Monitor::MonitorStateUpsert payload {};
            std::memcpy(&payload, rec->payload.data(), sizeof(payload));
            row.volumeDb = payload.volumeDb;
            row.volumeState = MonitorVoicesVolumeState::Numeric;
        }
        else
        {
            row.volumeState = MonitorVoicesVolumeState::Gap;
        }
        live[key] = row;
    }

    out.rows.reserve(static_cast<int>(live.size()));
    for (auto& entry : live)
        out.rows.push_back(entry.second);
    out.tableState = out.rows.isEmpty() ? MonitorVoicesTableState::EmptyFrame
                                        : MonitorVoicesTableState::Rows;
    return out;
}

} // namespace VNX
