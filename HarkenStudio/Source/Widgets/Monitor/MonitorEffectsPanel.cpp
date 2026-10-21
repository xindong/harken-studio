#include "Monitor/MonitorEffectsPanel.h"
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

MonitorEffectsPanel::MonitorEffectsPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorEffectsPanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    gapLabel_ = new QLabel(this);
    gapLabel_->setObjectName(QStringLiteral("MonitorEffectsGap"));
    gapLabel_->setWordWrap(true);
    root->addWidget(gapLabel_);

    model_ = new MonitorEffectsModel(this);
    auto* tableFrame = new TableWidget(TableOptions::display(), this);
    table_ = tableFrame->createModelView(QStringLiteral("MonitorEffectsTable"), model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(tableFrame, 1);

    connect(table_, &QTableView::clicked, this, &MonitorEffectsPanel::onTableClicked);
    subscribeCursor();
    refreshGapLabel();
}

MonitorEffectsPanel::~MonitorEffectsPanel()
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

void MonitorEffectsPanel::unsubscribeCursor()
{
    if (subscribedCursor_ && cursorToken_ != 0)
        subscribedCursor_->unsubscribe(cursorToken_);
    subscribedCursor_ = nullptr;
    cursorToken_ = 0;
}

void MonitorEffectsPanel::subscribeCursor()
{
    unsubscribeCursor();
    subscribedCursor_ = &cursorModel();
    cursorToken_ = subscribedCursor_->subscribe(
        [this](const MonitorCursorSnapshot& snapshot) { onSharedCursor(snapshot); });
}

void MonitorEffectsPanel::setSharedCursor(MonitorCursorModel* cursor)
{
    unsubscribeCursor();
    sharedCursor_ = cursor;
    subscribeCursor();
    rebuildVisibleRows();
}

MonitorCursorModel& MonitorEffectsPanel::cursorModel()
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

const MonitorCursorModel& MonitorEffectsPanel::cursorModel() const
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

void MonitorEffectsPanel::bindSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool)
{
    snapshot_ = std::move(snapshot);
    boundGeneration_ = snapshot_ ? snapshot_->generation : 0;
    rebuildVisibleRows();
}

QString MonitorEffectsPanel::gapMessage() const
{
    return gapLabel_ ? gapLabel_->text() : QString();
}

int MonitorEffectsPanel::visibleRowCount() const
{
    return model_ ? model_->rowCount() : 0;
}

const Monitor::EffectMonitorRow* MonitorEffectsPanel::visibleRowAt(int row) const
{
    return model_ ? model_->rowAt(row) : nullptr;
}

QVariantMap MonitorEffectsPanel::snapshotParams() const
{
    QVariantMap params;
    if (table_ && table_->horizontalHeader())
        params.insert(QStringLiteral("headerState"), table_->horizontalHeader()->saveState());
    return params;
}

void MonitorEffectsPanel::restoreParams(const QVariantMap& params)
{
    if (table_ && table_->horizontalHeader() && params.contains(QStringLiteral("headerState")))
        table_->horizontalHeader()->restoreState(params.value(QStringLiteral("headerState")).toByteArray());
}

MonitorCursorReject MonitorEffectsPanel::lockCurrentRow()
{
    if (!table_ || !table_->currentIndex().isValid())
        return MonitorCursorReject::HistoryUnavailable;
    return lockRow(table_->currentIndex().row());
}

MonitorCursorReject MonitorEffectsPanel::lockExecuteFrameForTest(std::optional<qint64> executeFrame)
{
    if (!executeFrame.has_value())
        return MonitorCursorReject::HistoryUnavailable;
    return cursorModel().lockTo(*executeFrame);
}

void MonitorEffectsPanel::rebuildVisibleRows()
{
    const auto projection = Monitor::projectEffectRows(snapshot_, cursorModel().snapshot().executeFrame);
    tableState_ = projection.tableState;
    QVector<Monitor::EffectMonitorRow> rows;
    rows.reserve(static_cast<int>(projection.rows.size()));
    for (const auto& row : projection.rows)
        rows.push_back(row);
    if (model_)
        model_->setRows(std::move(rows));
    syncCursorRetainedWindow();
    refreshGapLabel();
}

void MonitorEffectsPanel::syncCursorRetainedWindow()
{
    if (!snapshot_
        || tableState_ == Monitor::MonitorEffectsTableState::UnsupportedLayout
        || tableState_ == Monitor::MonitorEffectsTableState::Incomplete)
    {
        cursorModel().setContributorWindow(kMonitorCursorContributorEffects, std::nullopt, std::nullopt);
        return;
    }

    std::optional<std::int64_t> minFrame;
    std::optional<std::int64_t> maxFrame;
    for (const auto& rec : snapshot_->records)
    {
        if (rec.header.entityKind != VNX::Monitor::kMonitorEntityEffect)
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
        cursorModel().setContributorWindow(kMonitorCursorContributorEffects, std::nullopt, std::nullopt);
        return;
    }
    cursorModel().setContributorWindow(kMonitorCursorContributorEffects, *minFrame, *maxFrame + 1);
}

void MonitorEffectsPanel::refreshGapLabel()
{
    if (!gapLabel_ || !table_)
        return;
    switch (tableState_)
    {
    case Monitor::MonitorEffectsTableState::UnsupportedLayout:
        gapLabel_->setText(QStringLiteral("Effect instance payload is Unsupported. Gap: P4-4."));
        gapLabel_->setVisible(true);
        table_->setVisible(false);
        return;
    case Monitor::MonitorEffectsTableState::NoLegalFrame:
        gapLabel_->clear();
        gapLabel_->setVisible(false);
        table_->setVisible(false);
        return;
    case Monitor::MonitorEffectsTableState::EmptyFrame:
        gapLabel_->clear();
        gapLabel_->setVisible(false);
        table_->setVisible(true);
        return;
    case Monitor::MonitorEffectsTableState::Incomplete:
        gapLabel_->setText(QStringLiteral("Effect state is Incomplete. Gap: P2-4."));
        gapLabel_->setVisible(true);
        table_->setVisible(false);
        return;
    case Monitor::MonitorEffectsTableState::Rows:
        gapLabel_->clear();
        gapLabel_->setVisible(false);
        table_->setVisible(true);
        return;
    }
}

MonitorCursorReject MonitorEffectsPanel::lockRow(int row)
{
    const Monitor::EffectMonitorRow* entry = visibleRowAt(row);
    if (!entry)
        return MonitorCursorReject::HistoryUnavailable;
    if (entry->executeFrame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return MonitorCursorReject::HistoryUnavailable;
    return cursorModel().lockTo(static_cast<std::int64_t>(entry->executeFrame));
}

void MonitorEffectsPanel::onTableClicked(const QModelIndex& index)
{
    if (!index.isValid())
        return;
    lockRow(index.row());
}

void MonitorEffectsPanel::onSharedCursor(const MonitorCursorSnapshot&)
{
    rebuildVisibleRows();
}

} // namespace VNX
