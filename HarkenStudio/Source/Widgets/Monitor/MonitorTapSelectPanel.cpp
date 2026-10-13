#include "Monitor/MonitorTapSelectPanel.h"
#include "Monitor/MonitorPanelKeys.h"
#include "Monitor/MonitorTapBinding.h"

#include <QLabel>
#include <QVBoxLayout>

namespace VNX {

MonitorTapSelectPanel::MonitorTapSelectPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorTapSelectPanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    gapLabel_ = new QLabel(this);
    gapLabel_->setObjectName(QStringLiteral("MonitorTapSelectGap"));
    gapLabel_->setWordWrap(true);
    root->addWidget(gapLabel_);
    root->addStretch(1);

    subscribeCursor();
    refreshFace();
}

MonitorTapSelectPanel::~MonitorTapSelectPanel()
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

void MonitorTapSelectPanel::unsubscribeCursor()
{
    if (subscribedCursor_ && cursorToken_ != 0)
        subscribedCursor_->unsubscribe(cursorToken_);
    subscribedCursor_ = nullptr;
    cursorToken_ = 0;
}

void MonitorTapSelectPanel::subscribeCursor()
{
    unsubscribeCursor();
    subscribedCursor_ = &cursorModel();
    cursorToken_ = subscribedCursor_->subscribe(
        [this](const MonitorCursorSnapshot& snapshot) { onSharedCursor(snapshot); });
}

void MonitorTapSelectPanel::setSharedCursor(MonitorCursorModel* cursor)
{
    unsubscribeCursor();
    sharedCursor_ = cursor;
    subscribeCursor();
    refreshFace();
}

MonitorCursorModel& MonitorTapSelectPanel::cursorModel()
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

const MonitorCursorModel& MonitorTapSelectPanel::cursorModel() const
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

void MonitorTapSelectPanel::bindSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool)
{
    snapshot_ = std::move(snapshot);
    boundGeneration_ = snapshot_ ? snapshot_->generation : 0;
    refreshFace();
}

QString MonitorTapSelectPanel::gapMessage() const
{
    return gapLabel_ ? gapLabel_->text() : QString();
}

QVariantMap MonitorTapSelectPanel::snapshotParams() const
{
    return {};
}

void MonitorTapSelectPanel::restoreParams(const QVariantMap&)
{
}

void MonitorTapSelectPanel::refreshFace()
{
    const auto projection = Monitor::projectTapSelectFace(snapshot_);
    boundGeneration_ = projection.generation;
    cursorModel().setContributorWindow(
        kMonitorCursorContributorTapSelect, std::nullopt, std::nullopt);
    if (!gapLabel_)
        return;
    gapLabel_->setText(QStringLiteral(
        "Tap registry is unpublished. Gap: P5-1. Binding is unpublished."));
    gapLabel_->setVisible(true);
}

void MonitorTapSelectPanel::onSharedCursor(const MonitorCursorSnapshot&)
{
    refreshFace();
}

} // namespace VNX
