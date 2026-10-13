#include "Monitor/MonitorScopePanel.h"
#include "Monitor/MonitorPanelKeys.h"
#include "Monitor/ScopeTriggerProjection.h"

#include <QLabel>
#include <QVBoxLayout>

namespace VNX {

MonitorScopePanel::MonitorScopePanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorScopePanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    gapLabel_ = new QLabel(this);
    gapLabel_->setObjectName(QStringLiteral("MonitorScopeGap"));
    gapLabel_->setWordWrap(true);
    root->addWidget(gapLabel_);
    root->addStretch(1);

    subscribeCursor();
    refreshFace();
}

MonitorScopePanel::~MonitorScopePanel()
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

void MonitorScopePanel::unsubscribeCursor()
{
    if (subscribedCursor_ && cursorToken_ != 0)
        subscribedCursor_->unsubscribe(cursorToken_);
    subscribedCursor_ = nullptr;
    cursorToken_ = 0;
}

void MonitorScopePanel::subscribeCursor()
{
    unsubscribeCursor();
    subscribedCursor_ = &cursorModel();
    cursorToken_ = subscribedCursor_->subscribe(
        [this](const MonitorCursorSnapshot& snapshot) { onSharedCursor(snapshot); });
}

void MonitorScopePanel::setSharedCursor(MonitorCursorModel* cursor)
{
    unsubscribeCursor();
    sharedCursor_ = cursor;
    subscribeCursor();
    refreshFace();
}

MonitorCursorModel& MonitorScopePanel::cursorModel()
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

const MonitorCursorModel& MonitorScopePanel::cursorModel() const
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

void MonitorScopePanel::bindSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool)
{
    snapshot_ = std::move(snapshot);
    boundGeneration_ = snapshot_ ? snapshot_->generation : 0;
    refreshFace();
}

QString MonitorScopePanel::gapMessage() const
{
    return gapLabel_ ? gapLabel_->text() : QString();
}

QVariantMap MonitorScopePanel::snapshotParams() const
{
    return {};
}

void MonitorScopePanel::restoreParams(const QVariantMap&)
{
}

void MonitorScopePanel::refreshFace()
{
    const auto projection = Monitor::projectScopeFace(snapshot_);
    boundGeneration_ = projection.generation;
    cursorModel().setContributorWindow(
        kMonitorCursorContributorScope, std::nullopt, std::nullopt);
    if (!gapLabel_)
        return;
    gapLabel_->setText(QStringLiteral(
        "Scope window is unpublished. Gap: P5-3. Trigger is unpublished."));
    gapLabel_->setVisible(true);
}

void MonitorScopePanel::onSharedCursor(const MonitorCursorSnapshot&)
{
    refreshFace();
}

} // namespace VNX
