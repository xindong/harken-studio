#include "Monitor/MonitorLineagePanel.h"
#include "Monitor/LineageProjection.h"
#include "Monitor/MonitorPanelKeys.h"

#include <QLabel>
#include <QVBoxLayout>

namespace VNX {

MonitorLineagePanel::MonitorLineagePanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorLineagePanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    gapLabel_ = new QLabel(this);
    gapLabel_->setObjectName(QStringLiteral("MonitorLineageGap"));
    gapLabel_->setWordWrap(true);
    root->addWidget(gapLabel_);
    root->addStretch(1);

    subscribeCursor();
    refreshFace();
}

MonitorLineagePanel::~MonitorLineagePanel()
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

void MonitorLineagePanel::unsubscribeCursor()
{
    if (subscribedCursor_ && cursorToken_ != 0)
        subscribedCursor_->unsubscribe(cursorToken_);
    subscribedCursor_ = nullptr;
    cursorToken_ = 0;
}

void MonitorLineagePanel::subscribeCursor()
{
    unsubscribeCursor();
    subscribedCursor_ = &cursorModel();
    cursorToken_ = subscribedCursor_->subscribe(
        [this](const MonitorCursorSnapshot& snapshot) { onSharedCursor(snapshot); });
}

void MonitorLineagePanel::setSharedCursor(MonitorCursorModel* cursor)
{
    unsubscribeCursor();
    sharedCursor_ = cursor;
    subscribeCursor();
    refreshFace();
}

MonitorCursorModel& MonitorLineagePanel::cursorModel()
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

const MonitorCursorModel& MonitorLineagePanel::cursorModel() const
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

void MonitorLineagePanel::bindSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool)
{
    snapshot_ = std::move(snapshot);
    boundGeneration_ = snapshot_ ? snapshot_->generation : 0;
    refreshFace();
}

QString MonitorLineagePanel::gapMessage() const
{
    return gapLabel_ ? gapLabel_->text() : QString();
}

QVariantMap MonitorLineagePanel::snapshotParams() const
{
    return {};
}

void MonitorLineagePanel::restoreParams(const QVariantMap&)
{
}

void MonitorLineagePanel::refreshFace()
{
    const auto projection = Monitor::projectLineageFace(snapshot_);
    boundGeneration_ = projection.generation;
    cursorModel().setContributorWindow(
        kMonitorCursorContributorLineage, std::nullopt, std::nullopt);
    if (!gapLabel_)
        return;
    gapLabel_->setText(QStringLiteral(
        "Lineage adjacency is unpublished. Gap: P4-2. Layer is not a runtime node. Gap: G21. Jump is not wired."));
    gapLabel_->setVisible(true);
}

void MonitorLineagePanel::onSharedCursor(const MonitorCursorSnapshot&)
{
    refreshFace();
}

} // namespace VNX
