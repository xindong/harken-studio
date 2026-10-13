#include "Monitor/MonitorDspTimelinePanel.h"
#include "Monitor/DspTimelineProjection.h"
#include "Monitor/MonitorPanelKeys.h"

#include <QLabel>
#include <QVBoxLayout>

namespace VNX {

MonitorDspTimelinePanel::MonitorDspTimelinePanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorDspTimelinePanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    gapLabel_ = new QLabel(this);
    gapLabel_->setObjectName(QStringLiteral("MonitorDspTimelineGap"));
    gapLabel_->setWordWrap(true);
    root->addWidget(gapLabel_);
    root->addStretch(1);

    subscribeCursor();
    refreshFace();
}

MonitorDspTimelinePanel::~MonitorDspTimelinePanel()
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

void MonitorDspTimelinePanel::unsubscribeCursor()
{
    if (subscribedCursor_ && cursorToken_ != 0)
        subscribedCursor_->unsubscribe(cursorToken_);
    subscribedCursor_ = nullptr;
    cursorToken_ = 0;
}

void MonitorDspTimelinePanel::subscribeCursor()
{
    unsubscribeCursor();
    subscribedCursor_ = &cursorModel();
    cursorToken_ = subscribedCursor_->subscribe(
        [this](const MonitorCursorSnapshot& snapshot) { onSharedCursor(snapshot); });
}

void MonitorDspTimelinePanel::setSharedCursor(MonitorCursorModel* cursor)
{
    unsubscribeCursor();
    sharedCursor_ = cursor;
    subscribeCursor();
    refreshFace();
}

MonitorCursorModel& MonitorDspTimelinePanel::cursorModel()
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

const MonitorCursorModel& MonitorDspTimelinePanel::cursorModel() const
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

void MonitorDspTimelinePanel::bindSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool)
{
    snapshot_ = std::move(snapshot);
    boundGeneration_ = snapshot_ ? snapshot_->generation : 0;
    refreshFace();
}

QString MonitorDspTimelinePanel::gapMessage() const
{
    return gapLabel_ ? gapLabel_->text() : QString();
}

QVariantMap MonitorDspTimelinePanel::snapshotParams() const
{
    return {};
}

void MonitorDspTimelinePanel::restoreParams(const QVariantMap&)
{
}

void MonitorDspTimelinePanel::refreshFace()
{
    const auto projection = Monitor::projectDspTimelineFace(snapshot_);
    boundGeneration_ = projection.generation;
    cursorModel().setContributorWindow(
        kMonitorCursorContributorDspTimeline, std::nullopt, std::nullopt);
    if (!gapLabel_)
        return;
    gapLabel_->setText(QStringLiteral(
        "DSP wall / work / critical path are Unsupported. Gap: G19."));
    gapLabel_->setVisible(true);
}

void MonitorDspTimelinePanel::onSharedCursor(const MonitorCursorSnapshot&)
{
    refreshFace();
}

} // namespace VNX
