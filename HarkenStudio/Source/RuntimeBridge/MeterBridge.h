#pragma once

#include "Monitor/MonitorTypes.h"

#include <span>
#include <vector>

namespace HarkenEditor::Bridge {

enum class MeterMeasurement { Rms, Spectrum, LufsMomentary };

struct MeterCaptureRequest {
    harkenstudio::ObjectId bus;
    MeterMeasurement measurement = MeterMeasurement::Spectrum;
    uint64_t tapId = 0;
};

struct MeterMeasurementFrame {
    bool invalidated = false;
    bool unavailable = false;
    bool rmsAvailable = false;
    float rms = 0.0f;
    bool lufsAvailable = false;
    float lufs = 0.0f;
    uint32_t sampleRate = 0;
    uint32_t fftSize = 0;
    std::vector<float> spectrumDb;
};

int32_t applyMeterCapture(uint64_t session, uint64_t lease, std::span<const MeterCaptureRequest> requests,
                         VNX::Monitor::MonitorEffectiveCapturePlan& accepted);
bool decodeMeterMeasurement(const VNX::Monitor::MonitorRecordHeader& header,
                            std::span<const uint8_t> payload,
                            MeterMeasurementFrame& frame);

}
