#include "MeterBridge.h"

#include "NativeMonitorValues.h"

#include <QtEndian>

#include <bit>
#include <cmath>
#include <limits>

namespace HarkenEditor::Bridge {

int32_t applyMeterCapture(uint64_t session, uint64_t lease, std::span<const MeterCaptureRequest> requests,
                         VNX::Monitor::MonitorEffectiveCapturePlan& accepted)
{
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(snapshot);
    const auto captured = Vn_CaptureSnapshot(&snapshot);
    if (captured != VN_OK)
        return captured;
    std::vector<VnMonitorTapDescriptor> taps;
    std::vector<VnMonitorMeasurementRequest> measurements;
    const auto count = uint32_t(VN_SNAPSHOT_MAX_BUSES);
    for (const auto& capture : requests) {
        const auto nativeBus = toNative(capture.bus);
        uint32_t index = 0;
        while (index < count && (snapshot.busIds[index] != nativeBus || snapshot.busGenerations[index] == 0))
            ++index;
        if (index == count)
            return VN_UNKNOWN_ID;

        VnMonitorTapDescriptor tap{};
        tap.structSize = sizeof(tap);
        tap.tapKind = nativeBus == VN_BUS_MASTER
            ? VN_MONITOR_TAP_MASTER_OUTPUT : VN_MONITOR_TAP_BUS_POST_EFFECTS;
        tap.tapDescriptorId = capture.tapId;
        tap.tapDescriptorGeneration = snapshot.busGenerations[index];
        tap.ownerKind = VN_MONITOR_ENTITY_BUS;
        tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
        tap.ownerStableId = nativeBus;
        tap.ownerGeneration = snapshot.busGenerations[index];
        taps.push_back(tap);

        VnMonitorMeasurementRequest request{};
        request.structSize = sizeof(request);
        switch (capture.measurement) {
        case MeterMeasurement::Rms:
            request.kind = VN_MONITOR_MEASURE_RMS;
            break;
        case MeterMeasurement::Spectrum:
            request.kind = VN_MONITOR_MEASURE_SPECTRUM;
            break;
        case MeterMeasurement::LufsMomentary:
            request.kind = VN_MONITOR_MEASURE_LUFS_MOMENTARY;
            break;
        }
        request.channelMode = capture.measurement == MeterMeasurement::Spectrum
            ? VN_MONITOR_CHANNEL_MONO_AVERAGE : VN_MONITOR_CHANNEL_PER_CHANNEL;
        request.downsample = 1;
        request.tapIndex = static_cast<uint32_t>(measurements.size());
        request.tapDescriptorId = tap.tapDescriptorId;
        if (capture.measurement == MeterMeasurement::Spectrum)
            request.fftSize = 2048;
        measurements.push_back(request);
    }

    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = static_cast<uint32_t>(taps.size());
    plan.taps = taps.data();
    plan.measurementCount = static_cast<uint32_t>(measurements.size());
    plan.measurements = measurements.data();
    plan.downsample = 1;
    VnMonitorEffectiveCapturePlan result{};
    result.structSize = sizeof(result);
    const auto applied = Vn_MonitorApplyCapturePlan(session, lease, &plan, &result);
    if (applied == VN_OK)
        accepted = fromNative(result);
    return applied;
}

bool decodeMeterMeasurement(const VNX::Monitor::MonitorRecordHeader& header,
                            std::span<const uint8_t> payload,
                            MeterMeasurementFrame& frame)
{
    if (header.recordKind == VN_MONITOR_RECORD_TAP_BINDING_CHANGED
        || header.recordKind == VN_MONITOR_RECORD_MEASUREMENT_SEGMENT_RESET) {
        frame = {};
        frame.invalidated = true;
        return true;
    }
    if (header.recordVersion != 1 || header.streamKind != VN_MONITOR_STREAM_OPTIONAL_AUDIO
        || (header.validityFlags & VN_MONITOR_VALID_ARMED) == 0)
        return false;
    const bool unavailable = (header.validityFlags & VN_MONITOR_VALID_NUMERIC) == 0
        || (header.validityFlags & (VN_MONITOR_VALID_WARMUP | VN_MONITOR_VALID_UNSUPPORTED)) != 0;
    const auto word = [&payload](size_t offset) {
        return qFromLittleEndian<uint32_t>(payload.data() + offset);
    };
    if (header.recordKind == VN_MONITOR_RECORD_METER) {
        if (payload.size() != sizeof(VnMonitorMeterPayload)
            || word(0) != sizeof(VnMonitorMeterPayload)
            || word(offsetof(VnMonitorMeterPayload, algorithmVersion)) != 1)
            return false;
        const auto algorithm = word(offsetof(VnMonitorMeterPayload, algorithmId));
        if (algorithm != VN_MONITOR_MEASURE_RMS && algorithm != VN_MONITOR_MEASURE_LUFS_MOMENTARY)
            return false;
        MeterMeasurementFrame decoded;
        if (unavailable) {
            decoded.unavailable = true;
        } else if (algorithm == VN_MONITOR_MEASURE_RMS) {
            if (word(offsetof(VnMonitorMeterPayload, rmsUnitId)) != 1)
                return false;
            const float rms = std::bit_cast<float>(word(offsetof(VnMonitorMeterPayload, rmsBits)));
            if (!std::isfinite(rms) || rms < 0.0f)
                return false;
            decoded.rmsAvailable = true;
            decoded.rms = rms;
        } else {
            if (word(offsetof(VnMonitorMeterPayload, lufsSupported)) != 1)
                return false;
            const float lufs = std::bit_cast<float>(word(offsetof(VnMonitorMeterPayload, lufsMomentaryBits)));
            if (std::isnan(lufs) || lufs == std::numeric_limits<float>::infinity())
                return false;
            decoded.lufsAvailable = true;
            decoded.lufs = lufs;
        }
        frame = std::move(decoded);
        return true;
    }
    if (header.recordKind != VN_MONITOR_RECORD_SPECTRUM
        || payload.size() < sizeof(VnMonitorSpectrumPayload)
        || word(0) != sizeof(VnMonitorSpectrumPayload))
        return false;
    const auto fftSize = word(offsetof(VnMonitorSpectrumPayload, fftSize));
    const auto bins = word(offsetof(VnMonitorSpectrumPayload, binCount));
    const auto sampleRate = word(offsetof(VnMonitorSpectrumPayload, sampleRate));
    if (fftSize != 2048 || bins != fftSize / 2 + 1 || sampleRate == 0
        || payload.size() != sizeof(VnMonitorSpectrumPayload) + bins * sizeof(float))
        return false;
    MeterMeasurementFrame decoded;
    if (unavailable) {
        decoded.unavailable = true;
        frame = std::move(decoded);
        return true;
    }
    decoded.sampleRate = sampleRate;
    decoded.fftSize = fftSize;
    decoded.spectrumDb.reserve(bins);
    for (uint32_t bin = 0; bin < bins; ++bin) {
        const float db = std::bit_cast<float>(word(sizeof(VnMonitorSpectrumPayload) + bin * 4));
        if (!std::isfinite(db))
            return false;
        decoded.spectrumDb.push_back(db);
    }
    frame = std::move(decoded);
    return true;
}

}
