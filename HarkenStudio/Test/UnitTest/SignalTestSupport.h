#ifndef HARKEN_TEST_SIGNAL_TEST_SUPPORT_H
#define HARKEN_TEST_SIGNAL_TEST_SUPPORT_H

#include <harken/Harken.h>
#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <vector>

namespace harkentest {

// Peak floor below which a buffer counts as silent for comparison purposes.
inline constexpr float kSilenceFloor = 1e-4f;

inline std::vector<float> MakeSine(std::size_t frames, double freqHz,
                                   double sampleRate, float amplitude) {
    std::vector<float> s(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        s[n] = amplitude * static_cast<float>(std::sin(
                   2.0 * 3.14159265358979323846 * freqHz *
                   static_cast<double>(n) / sampleRate));
    }
    return s;
}

inline std::vector<float> MakeImpulse(std::size_t frames) {
    std::vector<float> s(frames, 0.0f);
    s[0] = 1.0f;
    return s;
}

inline std::vector<float> MakeDC(std::size_t frames, float value) {
    return std::vector<float>(frames, value);
}

inline float Peak(const float* data, std::size_t count) {
    float peak = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const float a = std::fabs(data[i]);
        if (a > peak) {
            peak = a;
        }
    }
    return peak;
}

inline float Peak(const std::vector<float>& data) {
    return Peak(data.data(), data.size());
}

#define HARKENTEST_ASSERT_NON_SILENT(buffer)                                  \
    ASSERT_GT(harkentest::Peak(buffer), harkentest::kSilenceFloor)             \
        << "silence-guard: compared signal is silent"

// --- PCM descriptor builders ---

inline VnPCMDesc MonoDesc(const std::vector<float>& samples,
                          std::uint32_t sampleRate,
                          std::uint32_t policy = VN_PCM_POLICY_COPY) {
    VnPCMDesc desc{};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = sampleRate;
    desc.channels = 1;
    desc.layout = VN_PCM_LAYOUT_INTERLEAVED;
    desc.policy = policy;
    desc.frames = samples.size();
    desc.samplesInterleaved = samples.data();
    return desc;
}

// A multichannel interleaved PCM descriptor over `interleaved` (channels x
// frames, frame-major). The caller keeps `interleaved` alive for the
// registration call. Used by the channel-order and multichannel-source goldens.
inline VnPCMDesc InterleavedDesc(const std::vector<float>& interleaved,
                                 std::uint32_t channels,
                                 std::uint32_t sampleRate,
                                 std::uint32_t policy = VN_PCM_POLICY_COPY) {
    VnPCMDesc desc{};
    desc.structSize = sizeof(VnPCMDesc);
    desc.sampleRate = sampleRate;
    desc.channels = channels;
    desc.layout = VN_PCM_LAYOUT_INTERLEAVED;
    desc.policy = policy;
    desc.frames = channels > 0 ? interleaved.size() / channels : 0;
    desc.samplesInterleaved = interleaved.data();
    return desc;
}

inline VnPlayDesc MakePlayDesc(VnSoundId soundId, float volumeDb = 0.0f,
                               float pitchRatio = 0.0f) {
    VnPlayDesc desc{};
    desc.structSize = sizeof(VnPlayDesc);
    desc.soundId = soundId;
    desc.busId = VN_BUS_MASTER;
    desc.volumeDb = volumeDb;
    desc.pitchRatio = pitchRatio;
    // Full-vintage descriptor: loopCount is an explicit field here (0 would
    // mean infinite), so default to a single pass. Loop tests set it.
    desc.loopCount = 1;
    return desc;
}

inline VnPlayDesc MakePlayDesc3D(VnSoundId soundId, float x, float y,
                                 float z, float volumeDb = 0.0f) {
    VnPlayDesc desc = MakePlayDesc(soundId, volumeDb);
    desc.spatialized = 1;
    desc.positionX = x;
    desc.positionY = y;
    desc.positionZ = z;
    return desc;
}

inline VnVec3 Vec3(float x, float y, float z) {
    VnVec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

inline VnCurveHandlePair EndpointHandles(const VnCurvePoint& a,
                                         const VnCurvePoint& b) {
    return VnCurveHandlePair{a.x, a.y, b.x, b.y};
}

inline VnCurveHandlePair EndpointHandles(
    const VnAttenuationCurvePoint& a,
    const VnAttenuationCurvePoint& b) {
    return VnCurveHandlePair{a.x, a.y, b.x, b.y};
}

// Baseline attenuation descriptor: chosen rolloff between dmin and dmax,
// every other feature neutral — air-absorption endpoints far above
// Nyquist (the wire boundary disables the lowpass, exact bypass), no
// proximity boost, no cone, no overrides. Tests switch on exactly the
// feature they measure.
inline VnAttenuationDesc MakeAttenuationDesc(
    float dmin, float dmax, uint32_t rolloff = VN_ROLLOFF_LINEAR) {
    VnAttenuationDesc desc{};
    desc.structSize = sizeof(VnAttenuationDesc);
    desc.rolloff = rolloff;
    desc.minDistance = dmin;
    desc.maxDistance = dmax;
    desc.airAbsorptionNearHz = 1.0e9f;
    desc.airAbsorptionFarHz = 1.0e9f;
    return desc;
}

}

#endif
