#include "Monitor/MonitorVoicesPanel.h"
#include "Widgets/Common/TableWidget.h"
#include "Monitor/MonitorPanelKeys.h"

#include "Monitor/MonitorTypes.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTableView>
#include <QVBoxLayout>

#include <limits>

namespace VNX {
namespace {

bool parseObjectId(const QString& text, std::uint64_t* out)
{
    if (!out)
        return false;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;
    bool ok = false;
    const qulonglong value = trimmed.toULongLong(&ok, 10);
    if (!ok)
        return false;
    *out = static_cast<std::uint64_t>(value);
    return true;
}

} // namespace

MonitorVoicesPanel::MonitorVoicesPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorVoicesPanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    gapLabel_ = new QLabel(this);
    gapLabel_->setObjectName(QStringLiteral("MonitorVoicesGap"));
    gapLabel_->setWordWrap(true);
    root->addWidget(gapLabel_);

    objectFilter_ = new QLineEdit(this);
    objectFilter_->setObjectName(QStringLiteral("MonitorVoicesObjectFilter"));
    objectFilter_->setPlaceholderText(QStringLiteral("Object ID"));
    root->addWidget(objectFilter_);

    model_ = new MonitorVoicesModel(this);
    auto* tableFrame = new TableWidget(TableOptions::display(), this);
    tableFrame->setColumnSizing(TableColumnSizing::Scrollable);
    table_ = tableFrame->createModelView(QStringLiteral("MonitorVoicesTable"), model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(tableFrame, 1);

    connect(objectFilter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filterObject_ = text;
        rebuildVisibleRows();
    });
    connect(table_, &QTableView::clicked, this, &MonitorVoicesPanel::onTableClicked);
    subscribeCursor();
    refreshGapLabel();
}

MonitorVoicesPanel::~MonitorVoicesPanel()
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

void MonitorVoicesPanel::unsubscribeCursor()
{
    if (subscribedCursor_ && cursorToken_ != 0)
        subscribedCursor_->unsubscribe(cursorToken_);
    subscribedCursor_ = nullptr;
    cursorToken_ = 0;
}

void MonitorVoicesPanel::subscribeCursor()
{
    unsubscribeCursor();
    subscribedCursor_ = &cursorModel();
    cursorToken_ = subscribedCursor_->subscribe(
        [this](const MonitorCursorSnapshot& snapshot) { onSharedCursor(snapshot); });
}

void MonitorVoicesPanel::setSharedCursor(MonitorCursorModel* cursor)
{
    unsubscribeCursor();
    sharedCursor_ = cursor;
    subscribeCursor();
    rebuildVisibleRows();
}

MonitorCursorModel& MonitorVoicesPanel::cursorModel()
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

const MonitorCursorModel& MonitorVoicesPanel::cursorModel() const
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

void MonitorVoicesPanel::bindSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool stateStreamSubscribed)
{
    snapshot_ = std::move(snapshot);
    stateStreamSubscribed_ = stateStreamSubscribed;
    boundGeneration_ = snapshot_ ? snapshot_->generation : 0;
    rebuildVisibleRows();
}

QString MonitorVoicesPanel::gapMessage() const
{
    return gapLabel_ ? gapLabel_->text() : QString();
}

QVariantMap MonitorVoicesPanel::snapshotParams() const
{
    QVariantMap params;
    params.insert(QStringLiteral("filterObject"), filterObject_);
    if (table_ && table_->horizontalHeader())
        params.insert(QStringLiteral("headerState"), table_->horizontalHeader()->saveState());
    return params;
}

void MonitorVoicesPanel::restoreParams(const QVariantMap& params)
{
    filterObject_ = params.value(QStringLiteral("filterObject")).toString();
    if (objectFilter_)
    {
        const QSignalBlocker blocker(objectFilter_);
        objectFilter_->setText(filterObject_);
    }
    if (table_ && table_->horizontalHeader() && params.contains(QStringLiteral("headerState")))
        table_->horizontalHeader()->restoreState(params.value(QStringLiteral("headerState")).toByteArray());
    rebuildVisibleRows();
}

MonitorCursorReject MonitorVoicesPanel::lockCurrentRow()
{
    if (!table_ || !table_->selectionModel())
        return MonitorCursorReject::HistoryUnavailable;
    const QModelIndex current = table_->selectionModel()->currentIndex();
    if (!current.isValid())
        return MonitorCursorReject::HistoryUnavailable;
    return lockRow(current.row());
}

MonitorCursorReject MonitorVoicesPanel::lockExecuteFrameForTest(
    std::optional<qint64> executeFrame)
{
    if (!executeFrame.has_value())
        return MonitorCursorReject::HistoryUnavailable;
    return cursorModel().lockTo(*executeFrame);
}

int MonitorVoicesPanel::visibleRowCount() const
{
    return model_ ? model_->rowCount() : 0;
}

const MonitorVoicesRow* MonitorVoicesPanel::visibleRowAt(int row) const
{
    return model_ ? model_->rowAt(row) : nullptr;
}

const MonitorVoicesRow* MonitorVoicesPanel::selectedRow() const
{
    if (!table_ || !table_->selectionModel())
        return nullptr;
    const QModelIndex current = table_->selectionModel()->currentIndex();
    if (!current.isValid())
        return nullptr;
    return visibleRowAt(current.row());
}

void MonitorVoicesPanel::rebuildVisibleRows()
{
    const auto projection =
        projectVoiceRows(snapshot_, cursorModel().snapshot(), stateStreamSubscribed_);
    tableState_ = projection.tableState;
    allRows_ = projection.rows;

    QVector<MonitorVoicesRow> visible;
    visible.reserve(allRows_.size());
    for (const auto& row : allRows_)
    {
        if (rowMatchesFilter(row))
            visible.push_back(row);
    }
    if (model_)
        model_->setRows(std::move(visible));
    syncCursorRetainedWindow();
    refreshGapLabel();
}

void MonitorVoicesPanel::syncCursorRetainedWindow()
{
    if (!snapshot_ || tableState_ == MonitorVoicesTableState::UnsupportedLayout
        || tableState_ == MonitorVoicesTableState::Incomplete)
    {
        cursorModel().setContributorWindow(kMonitorCursorContributorVoices, std::nullopt, std::nullopt);
        return;
    }

    std::optional<std::int64_t> minFrame;
    std::optional<std::int64_t> maxFrame;
    for (const auto& rec : snapshot_->records)
    {
        if (rec.header.entityKind != VNX::Monitor::kMonitorEntityVoice)
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
        cursorModel().setContributorWindow(kMonitorCursorContributorVoices, std::nullopt, std::nullopt);
        return;
    }
    cursorModel().setContributorWindow(kMonitorCursorContributorVoices, *minFrame, *maxFrame + 1);
}

void MonitorVoicesPanel::refreshGapLabel()
{
    if (!gapLabel_ || !table_)
        return;
    switch (tableState_)
    {
    case MonitorVoicesTableState::UnsupportedLayout:
        gapLabel_->setText(QStringLiteral("Voice state layout is Unsupported. Gap: P3-4."));
        gapLabel_->setVisible(true);
        table_->setVisible(false);
        return;
    case MonitorVoicesTableState::NoLegalFrame:
        gapLabel_->clear();
        gapLabel_->setVisible(false);
        table_->setVisible(false);
        return;
    case MonitorVoicesTableState::EmptyFrame:
        gapLabel_->clear();
        gapLabel_->setVisible(false);
        table_->setVisible(true);
        return;
    case MonitorVoicesTableState::Incomplete:
        gapLabel_->setText(QStringLiteral("Voice state is Incomplete. Gap: P2-4."));
        gapLabel_->setVisible(true);
        table_->setVisible(false);
        return;
    case MonitorVoicesTableState::Rows:
        gapLabel_->clear();
        gapLabel_->setVisible(false);
        table_->setVisible(true);
        return;
    }
}

bool MonitorVoicesPanel::rowMatchesFilter(const MonitorVoicesRow& row) const
{
    const QString objectText = filterObject_.trimmed();
    if (objectText.isEmpty())
        return true;
    std::uint64_t objectId = 0;
    if (!parseObjectId(objectText, &objectId))
        return false;
    return objectId == row.instanceHandle;
}

MonitorCursorReject MonitorVoicesPanel::lockRow(int row)
{
    const MonitorVoicesRow* entry = visibleRowAt(row);
    if (!entry)
        return MonitorCursorReject::HistoryUnavailable;
    if (entry->executeFrame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return MonitorCursorReject::HistoryUnavailable;
    return cursorModel().lockTo(static_cast<std::int64_t>(entry->executeFrame));
}

void MonitorVoicesPanel::onTableClicked(const QModelIndex& index)
{
    if (!index.isValid())
        return;
    lockRow(index.row());
}

void MonitorVoicesPanel::onSharedCursor(const MonitorCursorSnapshot&)
{
    rebuildVisibleRows();
}

} // namespace VNX
