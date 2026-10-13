#pragma once

#include "Monitor/MonitorRecordingBrowser.h"

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace VNX {

class MonitorRecordingPanel : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorRecordingPanel(QWidget* parent = nullptr);

    void applyState(const Monitor::MonitorRecordingBrowserState& state);
    QString faceText() const;
    QString reasonText() const;
    bool startEnabled() const;
    bool stopRecordingEnabled() const;
    int startClicks() const { return startClicks_; }
    int stopRecordingClicks() const { return stopRecordingClicks_; }
    int stopCaptureClicks() const { return stopCaptureClicks_; }
    int closeClicks() const { return closeClicks_; }
    Monitor::MonitorRecordingCommand lastAccepted() const { return lastAccepted_; }

signals:
    void recordingCommandRequested(Monitor::MonitorRecordingCommand command);

private:
    void onStart();
    void onStopRecording();
    void onStopCapture();
    void onClose();

    Monitor::MonitorRecordingBrowserState state_ {};
    QLabel* faceLabel_ = nullptr;
    QLabel* reasonLabel_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopRecordingButton_ = nullptr;
    QPushButton* stopCaptureButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    int startClicks_ = 0;
    int stopRecordingClicks_ = 0;
    int stopCaptureClicks_ = 0;
    int closeClicks_ = 0;
    Monitor::MonitorRecordingCommand lastAccepted_ = Monitor::MonitorRecordingCommand::None;
};

} // namespace VNX
