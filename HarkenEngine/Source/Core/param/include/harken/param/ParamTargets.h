// Parameter target identity: the u32 target IDs that address per-voice
// destinations, the combine-domain vocabulary, and the filter neutral
// constants. Built-in targets occupy 0..1023; 1024+ is reserved for future
// user-defined targets. Per-target FACTS (domain, clamp span, neutral,
// unit, transition support, wire opcode) live in the scoped metadata table
// of ParamTargetTable.h — the single point of truth — together with the
// DomainOfTarget / ClampResolvedTarget lookups that consume it.
#ifndef HARKEN_PARAM_PARAM_TARGETS_H
#define HARKEN_PARAM_PARAM_TARGETS_H

#include <cstdint>
#include <limits>

namespace harken {

// Built-in per-voice targets. Values are frozen once shipped: bindings and
// tools address targets by these numbers.
inline constexpr std::uint32_t kTargetVoiceVolume = 0;         // dB
inline constexpr std::uint32_t kTargetVoicePitch = 1;          // cents
inline constexpr std::uint32_t kTargetVoiceLowpassCutoff = 2;  // Hz
inline constexpr std::uint32_t kTargetVoiceHighpassCutoff = 3; // Hz
inline constexpr std::uint32_t kTargetVoicePan = 4;            // -1..+1
inline constexpr std::uint32_t kTargetVoiceWidth = 5;          // 0..1
inline constexpr std::uint32_t kTargetVoiceLowShelfGainDb = 6; // dB
inline constexpr std::uint32_t kTargetVoiceLowpassQ = 7;       // Q
inline constexpr std::uint32_t kTargetVoiceHighpassQ = 8;      // Q
inline constexpr std::uint32_t kTargetVoiceLowShelfFreqHz = 9; // Hz
inline constexpr std::uint32_t kTargetVoiceLowShelfQ = 10;     // Q
inline constexpr std::uint32_t kTargetVoiceHighShelfGainDb = 11; // dB
inline constexpr std::uint32_t kTargetVoiceHighShelfFreqHz = 12; // Hz
inline constexpr std::uint32_t kTargetVoiceHighShelfQ = 13;      // Q

// Count of targets the combine state carries per voice today. Reserved IDs
// extend this without renumbering.
inline constexpr std::uint32_t kBuiltinTargetCount = 14;

// How contributions to a target reduce onto its authored base value:
//   AddDb    — gains accumulate in the dB domain;
//   AddCents — pitch accumulates in cents (1200 * log2(ratio));
//   MinHz    — lowpass cutoffs take the most-closed (smallest) value;
//   MaxHz    — highpass cutoffs take the most-closed (largest) value;
//   Add      — plain unit-less sum (pan offsets accumulate);
//   Max      — unit-less most-open (largest) value (stereo width);
//   AddLogHz — octave offsets scale an authored frequency base;
//   AddLogQ  — octave offsets scale an authored Q base.
enum class CombineDomain : std::uint8_t {
    AddDb,
    AddCents,
    MinHz,
    MaxHz,
    Add,
    Max,
    AddLogHz,
    AddLogQ
};

// Neutral base values for the filter targets. A lowpass with no
// contribution is fully open — represented as float max so the min
// reduction passes any real cutoff through and an untouched target reads
// back as "bypass". A highpass with no contribution sits at 0 Hz, the
// neutral element of the max reduction and equally a bypass.
inline constexpr float kLowpassOpenHz = std::numeric_limits<float>::max();
inline constexpr float kHighpassClosedHz = 0.0f;
inline constexpr float kToneDefaultQ = 0.70710678118654752440f;
inline constexpr float kToneLowShelfDefaultHz = 250.0f;
inline constexpr float kToneHighShelfDefaultHz = 6000.0f;

struct VoiceTonePreset {
    bool lowpassEnabled = false;
    bool highpassEnabled = false;
    bool lowShelfEnabled = false;
    bool highShelfEnabled = false;
    float lowpassFrequencyHz = kLowpassOpenHz;
    float lowpassQ = kToneDefaultQ;
    float highpassFrequencyHz = kHighpassClosedHz;
    float highpassQ = kToneDefaultQ;
    float lowShelfFrequencyHz = kToneLowShelfDefaultHz;
    float lowShelfQ = kToneDefaultQ;
    float lowShelfGainDb = 0.0f;
    float highShelfFrequencyHz = kToneHighShelfDefaultHz;
    float highShelfQ = kToneDefaultQ;
    float highShelfGainDb = 0.0f;
};

} // namespace harken

#endif // HARKEN_PARAM_PARAM_TARGETS_H
