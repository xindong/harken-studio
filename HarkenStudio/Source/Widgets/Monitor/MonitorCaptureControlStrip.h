#pragma once

#include "Monitor/CapturePlanIntent.h"
#include "Monitor/MonitorCaptureControl.h"

#include <QString>
#include <QWidget>

class QLabel;

namespace harken::studio {

class MonitorCaptureControlStrip : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorCaptureControlStrip(QWidget* parent = nullptr);

    void applyState(const harken::studio::Monitor::MonitorCaptureControlState& state);
    void applyPreview(const harken::studio::Monitor::CaptureBudgetPreview& preview);
    void setStatusText(const QString& text);
    QString statusText() const;
    QString previewText() const;
    bool applyEnabled() const;

private:
    QLabel* statusLabel_ = nullptr;
    QLabel* previewLabel_ = nullptr;
    bool applyEnabled_ = false;
};

} // namespace harken::studio
