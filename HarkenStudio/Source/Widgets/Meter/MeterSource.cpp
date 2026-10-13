#include "MeterSource.h"

#include "Monitor/MonitorDecoder.h"
#include "RuntimeBridge/MonitorBridge.h"

#include <algorithm>
#include <utility>

namespace VNX {
namespace {

namespace Monitoring = HarkenEditor::Bridge::Monitoring;

}

struct MeterSource::Capture {
    uint64_t session = 0;
    uint64_t lease = 0;
    uint64_t generation = 0;
    uint64_t nextTapId = 1;
    bool planDirty = false;
    std::vector<HarkenEditor::Bridge::MeterCaptureRequest> requests;
    std::vector<MeterSource*> consumers;
    std::vector<uint8_t> buffer = std::vector<uint8_t>(262144);

    void close(int32_t result = Monitor::kOk)
    {
        for (auto* consumer : consumers) {
            consumer->tapId_ = 0;
            consumer->result_ = result;
            consumer->pending_ = {true, {}};
        }
        consumers.clear();
        requests.clear();
        if (session != 0) {
            if (lease != 0) {
                Monitor::MonitorEffectiveCapturePlan stopped{};
                Monitoring::stopCapture(session, lease, &stopped);
                Monitoring::releaseCaptureControl(session, lease);
            }
            Monitoring::closeSession(session);
        }
        session = lease = generation = 0;
        planDirty = false;
    }

    int32_t apply()
    {
        Monitor::MonitorEffectiveCapturePlan plan{};
        const auto result = HarkenEditor::Bridge::applyMeterCapture(session, lease, requests, plan);
        planDirty = result == Monitor::kInUse;
        if (result == Monitor::kOk) {
            generation = plan.capturePlanGeneration;
        }
        return result;
    }

    int32_t add(MeterSource* consumer, harkenstudio::ObjectId bus,
                HarkenEditor::Bridge::MeterMeasurement measurement)
    {
        if (session == 0) {
            Monitor::MonitorSessionDesc desc{};
            desc.requestedStreamKindMask = Monitor::kMonitorStreamOptionalAudio | Monitor::kMonitorStreamEvent;
            desc.requestedPermissionMask = Monitor::kMonitorPermObserveMeasurements
                | Monitor::kMonitorPermObserveEvents | Monitor::kMonitorPermCaptureControl;
            desc.consumerQueueBytes = 1048576;
            auto result = Monitoring::openSession(&desc, &session);
            if (result == Monitor::kOk) {
                Monitor::MonitorCaptureControlStatus status{};
                result = Monitoring::acquireCaptureControl(session, &status);
                if (result == Monitor::kOk)
                    lease = status.leaseGeneration;
            }
            if (result != Monitor::kOk) {
                close(result);
                return result;
            }
        }
        const auto matches = [&](const auto& request) {
            return request.bus == bus && request.measurement == measurement;
        };
        const auto existing = std::find_if(requests.begin(), requests.end(), matches);
        if (existing != requests.end() && existing->tapId == consumer->tapId_)
            return Monitor::kOk;
        const uint64_t previousTap = consumer->tapId_;
        auto nextRequests = requests;
        bool changed = false;
        if (previousTap != 0 && std::none_of(consumers.begin(), consumers.end(), [&](const auto* other) {
                return other != consumer && other->tapId_ == previousTap;
            })) {
            std::erase_if(nextRequests, [previousTap](const auto& request) { return request.tapId == previousTap; });
            changed = true;
        }
        const auto target = std::find_if(nextRequests.begin(), nextRequests.end(), matches);
        const uint64_t targetTap = target == nextRequests.end() ? nextTapId++ : target->tapId;
        if (target == nextRequests.end()) {
            nextRequests.push_back({bus, measurement, targetTap});
            changed = true;
        }
        if (changed) {
            const bool wasDirty = planDirty;
            requests.swap(nextRequests);
            const auto result = apply();
            if (result != Monitor::kOk && result != Monitor::kInUse) {
                requests.swap(nextRequests);
                planDirty = wasDirty;
                if (previousTap != 0)
                    remove(consumer);
                else if (consumers.empty())
                    close(result);
                return result;
            }
        }
        consumer->tapId_ = targetTap;
        consumer->pending_ = {true, {}};
        if (previousTap == 0)
            consumers.push_back(consumer);
        return Monitor::kOk;
    }

    void remove(MeterSource* consumer)
    {
        const uint64_t tap = std::exchange(consumer->tapId_, 0);
        std::erase(consumers, consumer);
        if (consumers.empty()) {
            close();
        } else if (std::none_of(consumers.begin(), consumers.end(),
                               [tap](const auto* other) { return other->tapId_ == tap; })) {
            std::erase_if(requests, [tap](const auto& request) { return request.tapId == tap; });
            const auto result = apply();
            if (result != Monitor::kOk && !planDirty)
                close(result);
        }
    }

    void poll()
    {
        if (session == 0)
            return;
        if (planDirty) {
            const auto result = apply();
            if (result != Monitor::kOk && result != Monitor::kInUse) {
                close(result);
                return;
            }
        }
        Monitor::MonitorPollDesc desc{};
        desc.buffer = buffer.data();
        desc.bufferBytes = buffer.size();
        Monitor::MonitorPollResult polled{};
        const auto result = Monitoring::poll(session, &desc, &polled);
        if (result != Monitor::kOk) {
            close(result);
            return;
        }
        const auto batch = Monitor::decodePackedRecords(buffer.data(), polled.bytesWritten,
                                                        polled.recordsWritten);
        if (batch.status != Monitor::kOk) {
            close(batch.status);
            return;
        }
        for (const auto& record : batch.records) {
            if (record.header.recordKind == Monitor::kMonitorRecordDropSummary) {
                close();
                return;
            }
            if (record.header.capturePlanGeneration != generation)
                continue;
            HarkenEditor::Bridge::MeterMeasurementFrame frame;
            if (!HarkenEditor::Bridge::decodeMeterMeasurement(record.header, record.payload, frame))
                continue;
            if (frame.invalidated) {
                const auto removed = std::erase_if(requests, [&](const auto& request) {
                    return request.tapId == record.header.instanceHandle;
                });
                if (removed == 0)
                    continue;
                std::erase_if(consumers, [&](auto* consumer) {
                    if (consumer->tapId_ != record.header.instanceHandle)
                        return false;
                    consumer->tapId_ = 0;
                    consumer->pending_ = {true, {}};
                    return true;
                });
                if (consumers.empty()) {
                    close();
                    return;
                }
                planDirty = true;
                continue;
            }
            for (auto* consumer : consumers) {
                if (consumer->tapId_ != record.header.instanceHandle)
                    continue;
                if (frame.unavailable) {
                    consumer->pending_ = {true, {}};
                    continue;
                }
                if (consumer->pending_.frames.size() >= 64)
                    consumer->pending_ = {true, {}};
                consumer->pending_.frames.push_back(frame);
            }
        }
    }
};

MeterSource::Capture& MeterSource::capture()
{
    static Capture shared;
    return shared;
}

MeterSource::~MeterSource() { stop(); }

bool MeterSource::start(harkenstudio::ObjectId bus, HarkenEditor::Bridge::MeterMeasurement measurement)
{
    result_ = capture().add(this, bus, measurement);
    if (result_ != Monitor::kOk)
        pending_ = {};
    return result_ == Monitor::kOk;
}

void MeterSource::stop()
{
    if (active())
        capture().remove(this);
    pending_ = {};
}

MeterSourceUpdate MeterSource::poll()
{
    if (active())
        capture().poll();
    return std::exchange(pending_, {});
}

}
