#pragma once

#include "Monitor/MonitorTypes.h"
#include "Core/ProjectObjectIndex.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUuid>

#include <deque>
#include <vector>

namespace HarkenEditor::Bridge { class RemoteConnection; }

namespace VNX::Monitor {

enum class EventCaptureState { Unavailable, Idle, Starting, Capturing, Stopping, Stopped, Failed };
enum class EventCaptureTarget { Local, Remote };

struct CapturedEvent
{
    MonitorRecordHeader header{};
    MonitorEventDetail detail{};
    QUuid projectGuid;
    QUuid objectGuid;
    QString objectName;
};

class EventCaptureController final : public QObject
{
    Q_OBJECT

public:
    explicit EventCaptureController(HarkenEditor::Bridge::RemoteConnection* remote,
                                     QObject* parent = nullptr);
    ~EventCaptureController() override;

    static constexpr int kMaxRows = 10000;
    static constexpr std::size_t kMaxBytes = 8 * 1024 * 1024;
    EventCaptureState state() const { return state_; }
    EventCaptureTarget target() const { return target_; }
    bool canStart() const;
    bool canStop() const { return state_ == EventCaptureState::Capturing; }
    QString statusText() const;
    QString targetText() const { return targetText_; }
    QString availableTargetText() const;
    quint64 sessionEpoch() const { return sessionEpoch_; }
    quint64 startedTimestampNs() const { return startedNs_; }
    quint64 sourceDropped() const { return sourceDropped_; }
    quint64 deliveryDropped() const { return deliveryDropped_; }
    quint64 transportDropped() const { return transportDropped_; }
    quint64 trimmedRecords() const { return trimmed_; }
    bool incomplete() const { return interrupted_ || sourceDropped_ || deliveryDropped_ || transportDropped_; }
    int rowCount() const { return static_cast<int>(records_.size()); }
    const CapturedEvent& eventAt(int row) const { return records_.at(static_cast<std::size_t>(row)); }

    void setLocalRuntimeAvailable(bool available);
    void start();
    void stop();
    void clear();
    void pollLocal();
    void setProjectObjects(ProjectObjectIndex objects);
    quint32 matchingProjectObject(const CapturedEvent& event) const;

signals:
    void stateChanged();
    void captureStarted();
    void recordsAboutToReset();
    void recordsReset();
    void recordsAboutToInsert(int first, int last);
    void recordsInserted();
    void recordsAboutToRemove(int first, int last);
    void recordsRemoved();
    void projectObjectsChanged();

private:
    void remoteStateChanged();
    void remoteCaptureChanged();
    void begin(quint64 epoch, quint64 timestamp);
    void consume(const QByteArray& bytes, quint32 count, quint64 epoch);
    void append(std::vector<CapturedEvent> records);
    void fail(const QString& reason);
    void closeLocal();
    void refreshAvailability();

    QPointer<HarkenEditor::Bridge::RemoteConnection> remote_;
    EventCaptureState state_ = EventCaptureState::Unavailable;
    EventCaptureTarget target_ = EventCaptureTarget::Local;
    bool localAvailable_ = false;
    bool interrupted_ = false;
    bool startedThisAttempt_ = false;
    QString targetText_;
    QString pendingTargetText_;
    QString error_;
    QString stopReason_;
    MonitorSessionHandle localSession_ = 0;
    quint64 sessionEpoch_ = 0;
    quint64 startedNs_ = 0;
    quint64 sourceDropped_ = 0;
    quint64 deliveryDropped_ = 0;
    quint64 transportDropped_ = 0;
    quint64 trimmed_ = 0;
    quint64 lastSourceSequence_ = 0;
    std::deque<CapturedEvent> records_;
    std::size_t retainedBytes_ = 0;
    ProjectObjectIndex projectObjects_;
    std::vector<std::uint8_t> pollBuffer_;
    QTimer timer_;
    QElapsedTimer transitionTimer_;
};

}
