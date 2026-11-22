#pragma once

#include "RuntimeBridge/MeterBridge.h"

namespace harken::studio {

struct MeterSourceUpdate {
    bool interrupted = false;
    std::vector<harken::studio::bridge::MeterMeasurementFrame> frames;
};

class MeterSource {
public:
    MeterSource() = default;
    MeterSource(const MeterSource&) = delete;
    MeterSource& operator=(const MeterSource&) = delete;
    ~MeterSource();
    bool start(harken::studio::types::ObjectId bus, harken::studio::bridge::MeterMeasurement measurement);
    void stop();
    MeterSourceUpdate poll();
    bool active() const { return tapId_ != 0; }
    int32_t result() const { return result_; }

private:
    struct Capture;
    static Capture& capture();
    uint64_t tapId_ = 0;
    int32_t result_ = Monitor::kOk;
    MeterSourceUpdate pending_;
};

}
