#include "Monitor/MonitorCaptureControlStrip.h"
#include "Core/ThemeLayout.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QString>

namespace harken::studio {

MonitorCaptureControlStrip::MonitorCaptureControlStrip(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorCapturePlanBar"));
    auto* root = new QVBoxLayout(this);
    ThemeLayout::margins(root, ThemeLayout::Space::Panel, ThemeLayout::Space::Tight, ThemeLayout::Space::Panel, ThemeLayout::Space::Tight);
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

void MonitorCaptureControlStrip::applyState(const harken::studio::Monitor::MonitorCaptureControlState& state)
{
    applyEnabled_ = harken::studio::Monitor::canApply(state);
    setStatusText(QString::fromStdString(harken::studio::Monitor::captureControlStatusText(state)));
}

void MonitorCaptureControlStrip::applyPreview(const harken::studio::Monitor::CaptureBudgetPreview& preview)
{
    if (previewLabel_)
        previewLabel_->setText(QString::fromStdString(harken::studio::Monitor::capturePlanFaceText(preview)));
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

} // namespace harken::studio
