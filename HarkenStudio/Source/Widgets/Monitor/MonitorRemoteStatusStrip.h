#pragma once

#include "Monitor/MonitorRemoteStatus.h"

#include <QString>
#include <QWidget>

class QLabel;

namespace VNX {

class MonitorRemoteStatusStrip : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorRemoteStatusStrip(QWidget* parent = nullptr);

    void applyState(const Monitor::MonitorRemoteStatusState& state);
    QString faceText() const;
    QString permissionText() const;
    bool controlEnabled() const;
    bool recordEnabled() const;
    QString recordDeniedReason() const;

private:
    QLabel* faceLabel_ = nullptr;
    QLabel* permissionLabel_ = nullptr;
    bool controlEnabled_ = false;
    bool recordEnabled_ = false;
    QString recordDeniedReason_;
};

} // namespace VNX
