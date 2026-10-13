#include "Monitor/MonitorPlayablesPanel.h"
#include "Monitor/MonitorPanelKeys.h"
#include "Monitor/PlayableLifecycleProjection.h"

#include <QComboBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace VNX {
namespace {

constexpr int kVariableControlFilterIndex = 6;

} // namespace

MonitorPlayablesPanel::MonitorPlayablesPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorPlayablesPanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    kindFilter_ = new QComboBox(this);
    kindFilter_->setObjectName(QStringLiteral("MonitorPlayablesKindFilter"));
    kindFilter_->addItem(QStringLiteral("All"), QStringLiteral("all"));
    kindFilter_->addItem(QStringLiteral("Playable"), QStringLiteral("playable"));
    kindFilter_->addItem(QStringLiteral("Action"), QStringLiteral("action"));
    kindFilter_->addItem(QStringLiteral("Container"), QStringLiteral("container"));
    kindFilter_->addItem(QStringLiteral("Queue"), QStringLiteral("queue"));
    kindFilter_->addItem(QStringLiteral("Transition"), QStringLiteral("transition"));
    kindFilter_->addItem(QStringLiteral("VariableControl"), QStringLiteral("variableControl"));
    if (auto* model = qobject_cast<QStandardItemModel*>(kindFilter_->model()))
    {
        if (QStandardItem* item = model->item(kVariableControlFilterIndex))
            item->setEnabled(false);
    }
    root->addWidget(kindFilter_);

    gapLabel_ = new QLabel(this);
    gapLabel_->setObjectName(QStringLiteral("MonitorPlayablesGap"));
    gapLabel_->setWordWrap(true);
    root->addWidget(gapLabel_);
    root->addStretch(1);

    connect(kindFilter_, &QComboBox::currentIndexChanged, this, [this](int) {
        kindFilterValue_ = kindFilter_->currentData().toString();
        refreshFace();
    });
    kindFilterValue_ = kindFilter_->currentData().toString();

    subscribeCursor();
    refreshFace();
}

MonitorPlayablesPanel::~MonitorPlayablesPanel()
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

void MonitorPlayablesPanel::unsubscribeCursor()
{
    if (subscribedCursor_ && cursorToken_ != 0)
        subscribedCursor_->unsubscribe(cursorToken_);
    subscribedCursor_ = nullptr;
    cursorToken_ = 0;
}

void MonitorPlayablesPanel::subscribeCursor()
{
    unsubscribeCursor();
    subscribedCursor_ = &cursorModel();
    cursorToken_ = subscribedCursor_->subscribe(
        [this](const MonitorCursorSnapshot& snapshot) { onSharedCursor(snapshot); });
}

void MonitorPlayablesPanel::setSharedCursor(MonitorCursorModel* cursor)
{
    unsubscribeCursor();
    sharedCursor_ = cursor;
    subscribeCursor();
    refreshFace();
}

MonitorCursorModel& MonitorPlayablesPanel::cursorModel()
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

const MonitorCursorModel& MonitorPlayablesPanel::cursorModel() const
{
    return sharedCursor_ ? *sharedCursor_ : ownedCursor_;
}

void MonitorPlayablesPanel::bindSnapshot(
    std::shared_ptr<const Monitor::MonitorStoreSnapshot> snapshot,
    bool stateStreamSubscribed)
{
    snapshot_ = std::move(snapshot);
    stateStreamSubscribed_ = stateStreamSubscribed;
    boundGeneration_ = snapshot_ ? snapshot_->generation : 0;
    refreshFace();
}

QString MonitorPlayablesPanel::gapMessage() const
{
    return gapLabel_ ? gapLabel_->text() : QString();
}

bool MonitorPlayablesPanel::variableControlFilterEnabled() const
{
    if (!kindFilter_)
        return false;
    auto* model = qobject_cast<QStandardItemModel*>(kindFilter_->model());
    const QStandardItem* item = model ? model->item(kVariableControlFilterIndex) : nullptr;
    return item && item->isEnabled();
}

QVariantMap MonitorPlayablesPanel::snapshotParams() const
{
    QVariantMap params;
    params.insert(QStringLiteral("kindFilter"), kindFilterValue_);
    return params;
}

void MonitorPlayablesPanel::restoreParams(const QVariantMap& params)
{
    kindFilterValue_ = params.value(QStringLiteral("kindFilter")).toString();
    if (!kindFilter_)
        return;
    const int index = kindFilter_->findData(kindFilterValue_);
    const QSignalBlocker blocker(kindFilter_);
    if (index >= 0 && index != kVariableControlFilterIndex)
        kindFilter_->setCurrentIndex(index);
    else
        kindFilterValue_ = kindFilter_->currentData().toString();
}

void MonitorPlayablesPanel::refreshFace()
{
    const auto projection = Monitor::projectPlayablesFace(snapshot_);
    boundGeneration_ = projection.generation;
    cursorModel().setContributorWindow(
        kMonitorCursorContributorPlayables, std::nullopt, std::nullopt);
    if (!gapLabel_)
        return;
    gapLabel_->setText(QStringLiteral(
        "Playable as-of state is Unsupported. Gap: P4-1. VariableControl is unpublished. Gap: G6."));
    gapLabel_->setVisible(true);
}

void MonitorPlayablesPanel::onSharedCursor(const MonitorCursorSnapshot&)
{
    refreshFace();
}

} // namespace VNX
