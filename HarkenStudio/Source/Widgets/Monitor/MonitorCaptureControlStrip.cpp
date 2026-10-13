#include "Monitor/MonitorCaptureControlStrip.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QString>

namespace VNX {

MonitorCaptureControlStrip::MonitorCaptureControlStrip(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorCapturePlanBar"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 4, 8, 4);
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("MonitorCaptureControlStatus"));
    statusLabel_->setWordWrap(true);
    root->addWidget(statusLabel_);
    previewLabel_ = new QLabel(this);
    previewLabel_->setObjectName(QStringLiteral("MonitorCapturePlanPreview"));
    previewLabel_->setWordWrap(true);
    root->addWidget(previewLabel_);
    setStatusText(QStringLiteral("Unavailable"));
}

void MonitorCaptureControlStrip::applyState(const VNX::Monitor::MonitorCaptureControlState& state)
{
    applyEnabled_ = VNX::Monitor::canApply(state);
    setStatusText(QString::fromStdString(VNX::Monitor::captureControlStatusText(state)));
}

void MonitorCaptureControlStrip::applyPreview(const VNX::Monitor::CaptureBudgetPreview& preview)
{
    if (previewLabel_)
        previewLabel_->setText(QString::fromStdString(VNX::Monitor::capturePlanFaceText(preview)));
}

void MonitorCaptureControlStrip::setStatusText(const QString& text)
{
    if (statusLabel_)
        statusLabel_->setText(text);
}

QString MonitorCaptureControlStrip::statusText() const
{
    return statusLabel_ ? statusLabel_->text() : QString();
}

QString MonitorCaptureControlStrip::previewText() const
{
    return previewLabel_ ? previewLabel_->text() : QString();
}

bool MonitorCaptureControlStrip::applyEnabled() const
{
    return applyEnabled_;
}

} // namespace VNX
