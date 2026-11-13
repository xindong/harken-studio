#pragma once

#include "Monitor/MonitorSessionFile.h"

#include <QObject>
#include <QString>

namespace HarkenEditor::Bridge { class RemoteConnection; }
namespace VNX::Monitor { class EventCaptureController; }

namespace VNX {

class RemoteSession final : public QObject
{
    Q_OBJECT

public:
    static RemoteSession& instance();

    HarkenEditor::Bridge::RemoteConnection& connection();
    Monitor::EventCaptureController& capture();
    bool startRecording(const QString& path);
    void stopRecording();
    Monitor::MonitorRecordingStatus recordingStatus() const { return recorder_.status(); }
    void shutdown();

private:
    RemoteSession() = default;
    void ensure();

    HarkenEditor::Bridge::RemoteConnection* connection_ = nullptr;
    Monitor::EventCaptureController* capture_ = nullptr;
    Monitor::MonitorSessionWriter recorder_;
};

}
