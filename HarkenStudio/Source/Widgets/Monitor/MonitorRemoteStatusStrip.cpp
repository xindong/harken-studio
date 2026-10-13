#include "Monitor/MonitorRemoteStatusStrip.h"
#include "Monitor/MonitorTypes.h"

#include <QLabel>
#include <QStringList>
#include <QVBoxLayout>

namespace VNX {

MonitorRemoteStatusStrip::MonitorRemoteStatusStrip(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorRemoteStatus"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 4, 8, 4);
    faceLabel_ = new QLabel(this);
    faceLabel_->setObjectName(QStringLiteral("MonitorRemoteStatusFace"));
    faceLabel_->setWordWrap(true);
    root->addWidget(faceLabel_);
    permissionLabel_ = new QLabel(this);
    permissionLabel_->setObjectName(QStringLiteral("MonitorRemoteStatusPermissions"));
    permissionLabel_->setWordWrap(true);
    root->addWidget(permissionLabel_);
    applyState({});
}

void MonitorRemoteStatusStrip::applyState(const Monitor::MonitorRemoteStatusState& state)
{
    controlEnabled_ = Monitor::connectedHasControl(state);
    recordEnabled_ = Monitor::permissionGranted(state, VNX::Monitor::kMonitorPermRecord)
        && controlEnabled_;
    recordDeniedReason_.clear();
    if (!recordEnabled_)
        recordDeniedReason_ = QString::fromStdString(Monitor::recordDeniedReason(state));

    if (faceLabel_)
        faceLabel_->setText(QString::fromStdString(Monitor::remoteStatusFaceText(state)));

    QStringList rows;
    for (const auto& row : state.permissions)
    {
        rows.append(QStringLiteral("%1=%2")
                        .arg(QString::fromLatin1(row.id),
                             row.granted ? QStringLiteral("1") : QStringLiteral("0")));
    }
    if (permissionLabel_)
        permissionLabel_->setText(rows.join(QLatin1Char(' ')));
}

QString MonitorRemoteStatusStrip::faceText() const
{
    return faceLabel_ ? faceLabel_->text() : QString();
}

QString MonitorRemoteStatusStrip::permissionText() const
{
    return permissionLabel_ ? permissionLabel_->text() : QString();
}

bool MonitorRemoteStatusStrip::controlEnabled() const
{
    return controlEnabled_;
}

bool MonitorRemoteStatusStrip::recordEnabled() const
{
    return recordEnabled_;
}

QString MonitorRemoteStatusStrip::recordDeniedReason() const
{
    return recordDeniedReason_;
}

} // namespace VNX
