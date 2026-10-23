#pragma once

#include "Utils/ObjectId.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>

namespace HarkenEditor::Bridge {

enum class RemoteConnectionState { Disconnected, Connecting, Handshaking, Connected, Failed };
enum class RemoteEventCaptureState { Idle, Starting, Capturing, Stopping, Stopped, Failed };

struct RemoteEventCaptureStatus
{
    RemoteEventCaptureState state = RemoteEventCaptureState::Idle;
    qint32 result = 0;
    quint64 sessionEpoch = 0;
    quint64 startedTimestampNs = 0;
    quint64 stoppedTimestampNs = 0;
    quint64 sourceDroppedRecords = 0;
    quint64 deliveryDroppedRecords = 0;
    quint64 transportDroppedRecords = 0;
    quint32 acceptedExecutionFactVersion = 0;
    quint32 baselineAvailability = 0;
    quint32 requestedPermissionMask = 0;
    quint32 acceptedPermissionMask = 0;
    quint32 deniedPermissionMask = 0;
    quint32 unsupportedPermissionMask = 0;
    quint32 acceptedStreamKindMask = 0;
    quint32 symbolsDecision = 0;
    quint32 symbolAvailability = 0;
    quint64 identityUnavailableCount = 0;
    quint64 baselineStateCutId = 0;
    quint64 supportedFactKinds[2]{};
    QString error;
};

struct RemoteTarget
{
    QString instanceId;
    QString application;
    QString project;
    QString machine;
    QString address;
    QString engineVersion;
    QString buildId;
    QString platform;
    quint32 processId = 0;
    quint16 port = 0;
    quint32 protocolMajor = 0;
    quint32 protocolMinor = 0;
    quint64 capabilities = 0;
    quint32 sampleRate = 0;
    quint32 blockFrames = 0;
    bool compatible = false;
};

struct RemoteCatalogEntry
{
    harkenstudio::ObjectId id{};
    QString name;
    quint32 kind = 0;
};

struct RemoteRuntimeSnapshot
{
    bool valid = false;
    quint64 blockIndex = 0;
    quint32 activeVoices = 0;
    quint32 busCount = 0;
    quint32 callbackCpuUs = 0;
    quint32 updateCpuUs = 0;
    quint64 backpressureCount = 0;
    float peak = 0;
};

class RemoteConnection final : public QObject
{
    Q_OBJECT

public:
    static constexpr bool kLocalConnectionsOnly = true;

    explicit RemoteConnection(QObject* parent = nullptr);
    ~RemoteConnection() override;

    RemoteConnectionState state() const;
    QString statusText() const;
    RemoteTarget target() const;
    QVector<RemoteTarget> discoveredTargets() const;
    QVector<RemoteCatalogEntry> catalog() const;
    RemoteRuntimeSnapshot snapshot() const;
    bool captureEnabled() const;
    bool capturePending() const;
    bool canControl() const;
    bool canCapture() const;
    bool commandPending() const;
    quint64 lastPlaybackHandle() const;
    bool canCaptureEvents() const;
    RemoteEventCaptureStatus eventCaptureStatus() const;

    void discover(quint16 discoveryPort = 37840);
    void connectTarget(const QString& address, quint16 port);
    void disconnectTarget();
    void refreshCatalog();
    void setCaptureEnabled(bool enabled);
    void startEventCapture();
    void stopEventCapture();
    void postAction(harkenstudio::ObjectId actionId);
    void stopPlayback(quint64 playbackHandle);
    void stopAll();
    void setParameter(harkenstudio::ObjectId variableId, float value);

signals:
    void stateChanged();
    void targetsChanged();
    void catalogChanged();
    void snapshotChanged();
    void captureChanged();
    void commandStateChanged();
    void commandFinished(const QString& command, bool success, const QString& result);
    void monitorRecordsReceived(const QByteArray& records, quint32 recordCount);
    void eventCaptureChanged();
    void eventRecordsReceived(const QByteArray& records, quint32 recordCount, quint64 sessionEpoch);
    void discoveryFailed(const QString& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
