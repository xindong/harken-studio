#include "Monitor/MonitorRecordingPanel.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace VNX {

MonitorRecordingPanel::MonitorRecordingPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MonitorRecordingPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    faceLabel_ = new QLabel(this);
    faceLabel_->setObjectName(QStringLiteral("MonitorRecordingFace"));
    root->addWidget(faceLabel_);
    reasonLabel_ = new QLabel(this);
    reasonLabel_->setObjectName(QStringLiteral("MonitorRecordingReason"));
    reasonLabel_->setWordWrap(true);
    root->addWidget(reasonLabel_);
    startButton_ = new QPushButton(QStringLiteral("StartRecording"), this);
    startButton_->setObjectName(QStringLiteral("MonitorRecordingStart"));
    root->addWidget(startButton_);
    stopRecordingButton_ = new QPushButton(QStringLiteral("StopRecording"), this);
    stopRecordingButton_->setObjectName(QStringLiteral("MonitorRecordingStop"));
    root->addWidget(stopRecordingButton_);
    stopCaptureButton_ = new QPushButton(QStringLiteral("StopCapture"), this);
    stopCaptureButton_->setObjectName(QStringLiteral("MonitorRecordingStopCapture"));
    root->addWidget(stopCaptureButton_);
    closeButton_ = new QPushButton(QStringLiteral("Close"), this);
    closeButton_->setObjectName(QStringLiteral("MonitorRecordingClose"));
    root->addWidget(closeButton_);
    QObject::connect(startButton_, &QPushButton::clicked, this, &MonitorRecordingPanel::onStart);
    QObject::connect(stopRecordingButton_, &QPushButton::clicked, this,
                     &MonitorRecordingPanel::onStopRecording);
    QObject::connect(stopCaptureButton_, &QPushButton::clicked, this,
                     &MonitorRecordingPanel::onStopCapture);
    QObject::connect(closeButton_, &QPushButton::clicked, this, &MonitorRecordingPanel::onClose);
    applyState({});
}

void MonitorRecordingPanel::applyState(const Monitor::MonitorRecordingBrowserState& state)
{
    state_ = state;
    if (faceLabel_)
        faceLabel_->setText(QString::fromLatin1(Monitor::recordingFaceText(state.face)));
    if (reasonLabel_)
        reasonLabel_->setText(QString::fromStdString(state.reason));
    if (startButton_)
        startButton_->setEnabled(state.startEnabled);
    if (stopRecordingButton_)
        stopRecordingButton_->setEnabled(state.stopRecordingEnabled);
}

QString MonitorRecordingPanel::faceText() const
{
    return faceLabel_ ? faceLabel_->text() : QString();
}

QString MonitorRecordingPanel::reasonText() const
{
    return reasonLabel_ ? reasonLabel_->text() : QString();
}

bool MonitorRecordingPanel::startEnabled() const
{
    return startButton_ && startButton_->isEnabled();
}

bool MonitorRecordingPanel::stopRecordingEnabled() const
{
    return stopRecordingButton_ && stopRecordingButton_->isEnabled();
}

void MonitorRecordingPanel::onStart()
{
    const auto dispatched = Monitor::dispatchRecordingCommand(
        state_, Monitor::MonitorRecordingCommand::StartRecording);
    if (dispatched.accepted)
    {
        ++startClicks_;
        lastAccepted_ = dispatched.command;
        emit recordingCommandRequested(dispatched.command);
    }
}

void MonitorRecordingPanel::onStopRecording()
{
    const auto dispatched = Monitor::dispatchRecordingCommand(
        state_, Monitor::MonitorRecordingCommand::StopRecording);
    if (dispatched.accepted)
    {
        ++stopRecordingClicks_;
        lastAccepted_ = dispatched.command;
        emit recordingCommandRequested(dispatched.command);
    }
}

void MonitorRecordingPanel::onStopCapture()
{
    const auto dispatched = Monitor::dispatchRecordingCommand(
        state_, Monitor::MonitorRecordingCommand::StopCapture);
    if (dispatched.accepted)
    {
        ++stopCaptureClicks_;
        lastAccepted_ = dispatched.command;
        emit recordingCommandRequested(dispatched.command);
    }
}

void MonitorRecordingPanel::onClose()
{
    const auto dispatched = Monitor::dispatchRecordingCommand(
        state_, Monitor::MonitorRecordingCommand::CloseSession);
    if (dispatched.accepted)
    {
        ++closeClicks_;
        lastAccepted_ = dispatched.command;
        emit recordingCommandRequested(dispatched.command);
    }
}

} // namespace VNX
