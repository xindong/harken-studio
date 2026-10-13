#include "Monitor/MonitorSpectrumPanel.h"
#include "Monitor/MonitorPanelKeys.h"
#include "Monitor/SpectrumDisplayProjection.h"

#include <QLabel>
#include <QVBoxLayout>

namespace VNX {

MonitorSpectrumPanel::MonitorSpectrumPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorSpectrumPanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    gapLabel_ = new QLabel(this);
    gapLabel_->setObjectName(QStringLiteral("MonitorSpectrumGap"));
    gapLabel_->setWordWrap(true);
    root->addWidget(gapLabel_);
    root->addStretch(1);

    subscribeCursor();
    refreshFace();
}

MonitorSpectrumPanel::~MonitorSpectrumPanel()
{
    unsubscribeCursor();
    sharedCursor_ = nullptr;
}

void MonitorSpectrumPanel::unsubscribeCursor()
{
    if (subscribedCursor_ && cursorToken_ != 0)
        subscribedCursor_->unsubscribe(cursorToken_);
    subscribedCursor_ = nullptr;
    cursorToken_ = 0;
}

void MonitorSpectrumPanel::subscribeCursor()
{
    unsubscribeCursor();
    subscribedCursor_ = &cursorModel();
    cursorToken_ = subscribedCursor_->subscribe(
        [this](const MonitorCursorSnapshot& snapshot) { onSharedCursor(snapshot); });
}

void MonitorSpectrumPanel::setSharedCursor(MonitorCursorModel* cursor)
{
    unsubscribeCursor();
    sharedCursor_ = cursor;
    subscribeCursor();
    refreshFace();
}

MonitorCursorModel& MonitorSpectrumPanel::cursorModel()
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

const MonitorCursorModel& MonitorSpectrumPanel::cursorModel() const
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

void MonitorSpectrumPanel::bindSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool)
{
    snapshot_ = std::move(snapshot);
    boundGeneration_ = snapshot_ ? snapshot_->generation : 0;
    refreshFace();
}

QString MonitorSpectrumPanel::gapMessage() const
{
    return gapLabel_ ? gapLabel_->text() : QString();
}

QVariantMap MonitorSpectrumPanel::snapshotParams() const
{
    return {};
}

void MonitorSpectrumPanel::restoreParams(const QVariantMap&)
{
}

void MonitorSpectrumPanel::refreshFace()
{
    const auto projection = Monitor::projectSpectrumFace(snapshot_);
    boundGeneration_ = projection.generation;
    cursorModel().setContributorWindow(
        kMonitorCursorContributorSpectrum, std::nullopt, std::nullopt);
    if (!gapLabel_)
        return;
    gapLabel_->setText(QStringLiteral(
        "Spectrum bins are unpublished. Gap: P5-4. Binding is unpublished."));
    gapLabel_->setVisible(true);
}

void MonitorSpectrumPanel::onSharedCursor(const MonitorCursorSnapshot&)
{
    refreshFace();
}

} // namespace VNX
