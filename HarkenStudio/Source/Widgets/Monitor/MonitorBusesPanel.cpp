#include "Monitor/MonitorBusesPanel.h"
#include "Monitor/MonitorTypes.h"
#include "Widgets/Common/TableWidget.h"
#include "Monitor/MonitorPanelKeys.h"

#include <algorithm>
#include <QHeaderView>
#include <QLabel>
#include <QTableView>
#include <QVBoxLayout>

#include <limits>

namespace VNX {

MonitorBusesPanel::MonitorBusesPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorBusesPanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    gapLabel_ = new QLabel(this);
    gapLabel_->setObjectName(QStringLiteral("MonitorBusesGap"));
    gapLabel_->setWordWrap(true);
    root->addWidget(gapLabel_);

    model_ = new MonitorBusesModel(this);
    auto* tableFrame = new TableWidget(TableOptions::display(), this);
    table_ = tableFrame->createModelView(QStringLiteral("MonitorBusesTable"), model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(tableFrame, 1);

    connect(table_, &QTableView::clicked, this, &MonitorBusesPanel::onTableClicked);
    subscribeCursor();
    refreshGapLabel();
}

MonitorBusesPanel::~MonitorBusesPanel()
{
    if (subscribedCursor_ == &ownedCursor_)
        unsubscribeCursor();
    else
    {
        subscribedCursor_ = nullptr;
        cursorToken_ = 0;
        sharedCursor_ = nullptr;
    }
}

void MonitorBusesPanel::unsubscribeCursor()
{
    if (subscribedCursor_ && cursorToken_ != 0)
        subscribedCursor_->unsubscribe(cursorToken_);
    subscribedCursor_ = nullptr;
    cursorToken_ = 0;
}

void MonitorBusesPanel::subscribeCursor()
{
    unsubscribeCursor();
    subscribedCursor_ = &cursorModel();
    cursorToken_ = subscribedCursor_->subscribe(
        [this](const MonitorCursorSnapshot& snapshot) { onSharedCursor(snapshot); });
}

void MonitorBusesPanel::setSharedCursor(MonitorCursorModel* cursor)
{
    unsubscribeCursor();
    sharedCursor_ = cursor;
    subscribeCursor();
    rebuildVisibleRows();
}

MonitorCursorModel& MonitorBusesPanel::cursorModel()
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

const MonitorCursorModel& MonitorBusesPanel::cursorModel() const
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

void MonitorBusesPanel::bindSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool)
{
    snapshot_ = std::move(snapshot);
    boundGeneration_ = snapshot_ ? snapshot_->generation : 0;
    rebuildVisibleRows();
}

QString MonitorBusesPanel::gapMessage() const
{
    return gapLabel_ ? gapLabel_->text() : QString();
}

int MonitorBusesPanel::visibleRowCount() const
{
    return model_ ? model_->rowCount() : 0;
}

const Monitor::BusMonitorRow* MonitorBusesPanel::visibleRowAt(int row) const
{
    return model_ ? model_->rowAt(row) : nullptr;
}

QVariantMap MonitorBusesPanel::snapshotParams() const
{
    QVariantMap params;
    if (table_ && table_->horizontalHeader())
        params.insert(QStringLiteral("headerState"), table_->horizontalHeader()->saveState());
    return params;
}

void MonitorBusesPanel::restoreParams(const QVariantMap& params)
{
    if (table_ && table_->horizontalHeader() && params.contains(QStringLiteral("headerState")))
        table_->horizontalHeader()->restoreState(params.value(QStringLiteral("headerState")).toByteArray());
}

void MonitorBusesPanel::rebuildVisibleRows()
{
    const auto projection = Monitor::projectBusRows(snapshot_, cursorModel().snapshot().executeFrame);
    tableState_ = projection.tableState;
    QVector<Monitor::BusMonitorRow> rows;
    rows.reserve(static_cast<int>(projection.rows.size()));
    for (const auto& row : projection.rows)
        rows.push_back(row);
    if (model_)
        model_->setRows(std::move(rows));
    syncCursorRetainedWindow();
    refreshGapLabel();
}

void MonitorBusesPanel::syncCursorRetainedWindow()
{
    if (!snapshot_
        || tableState_ == Monitor::MonitorBusesTableState::UnsupportedLayout
        || tableState_ == Monitor::MonitorBusesTableState::Incomplete)
    {
        cursorModel().setContributorWindow(kMonitorCursorContributorBuses, std::nullopt, std::nullopt);
        return;
    }

    std::optional<std::int64_t> minFrame;
    std::optional<std::int64_t> maxFrame;
    for (const auto& rec : snapshot_->records)
    {
        if (rec.header.entityKind != VNX::Monitor::kMonitorEntityBus)
            continue;
        if (rec.header.recordKind != VNX::Monitor::kMonitorRecordStateUpsert
            && rec.header.recordKind != VNX::Monitor::kMonitorRecordStateTombstone)
            continue;
        if (rec.header.executeFrame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            continue;
        const auto frame = static_cast<std::int64_t>(rec.header.executeFrame);
        minFrame = minFrame ? std::min(*minFrame, frame) : frame;
        maxFrame = maxFrame ? std::max(*maxFrame, frame) : frame;
    }
    if (!minFrame || !maxFrame)
    {
        cursorModel().setContributorWindow(kMonitorCursorContributorBuses, std::nullopt, std::nullopt);
        return;
    }
    cursorModel().setContributorWindow(kMonitorCursorContributorBuses, *minFrame, *maxFrame + 1);
}

void MonitorBusesPanel::refreshGapLabel()
{
    if (!gapLabel_ || !table_)
        return;
    switch (tableState_)
    {
    case Monitor::MonitorBusesTableState::UnsupportedLayout:
        gapLabel_->setText(QStringLiteral("Bus topology tree is Unsupported. Gap: P4-5."));
        gapLabel_->setVisible(true);
        table_->setVisible(false);
        return;
    case Monitor::MonitorBusesTableState::NoLegalFrame:
        gapLabel_->clear();
        gapLabel_->setVisible(false);
        table_->setVisible(false);
        return;
    case Monitor::MonitorBusesTableState::EmptyFrame:
        gapLabel_->clear();
        gapLabel_->setVisible(false);
        table_->setVisible(true);
        return;
    case Monitor::MonitorBusesTableState::Incomplete:
        gapLabel_->setText(QStringLiteral("Bus state is Incomplete. Gap: P2-4."));
        gapLabel_->setVisible(true);
        table_->setVisible(false);
        return;
    case Monitor::MonitorBusesTableState::Rows:
        gapLabel_->setText(QStringLiteral("Tree view is Unsupported. Solo is authored only."));
        gapLabel_->setVisible(true);
        table_->setVisible(true);
        return;
    }
}

MonitorCursorReject MonitorBusesPanel::lockRow(int row)
{
    const Monitor::BusMonitorRow* entry = visibleRowAt(row);
    if (!entry)
        return MonitorCursorReject::HistoryUnavailable;
    if (entry->executeFrame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return MonitorCursorReject::HistoryUnavailable;
    return cursorModel().lockTo(static_cast<std::int64_t>(entry->executeFrame));
}

void MonitorBusesPanel::onTableClicked(const QModelIndex& index)
{
    if (!index.isValid())
        return;
    lockRow(index.row());
}

void MonitorBusesPanel::onSharedCursor(const MonitorCursorSnapshot&)
{
    rebuildVisibleRows();
}

} // namespace VNX
