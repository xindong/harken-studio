// Snapshot transport (audio -> logic): three plain-payload slots pair with a
// single-reader pin. The reader pins and confirms the published slot before
// copying it; the writer selects a slot that is neither published nor pinned.
// Sequentially consistent control operations close the store-load handshake,
// while the audio-side writer remains fixed-work and non-blocking.
#ifndef HARKEN_CORE_SNAPSHOT_BUFFER_H
#define HARKEN_CORE_SNAPSHOT_BUFFER_H

#include <harken/Harken.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace harken {

// Stage-tap measurement gating, following the allocation guard's pattern:
// measurement compiles in for Debug builds, and for Release only by the
// explicit HARKEN_ENABLE_STAGE_TAPS opt-in. Only the per-voice stage peak
// scans are gated — snapshot layout, voice states, counters, and high-water
// marks are identical and live in every build; gated-out taps simply read 0.
// The BUS OUTPUT peak scan left this gate in M6 (the section-11.7 declared
// behavior change): it feeds the meter Variable transport, a runtime
// feature, so it is compiled into all builds and Release snapshots read
// honest busPeaks values.
#if defined(_DEBUG) || defined(HARKEN_ENABLE_STAGE_TAPS)
#define HARKEN_STAGE_TAPS_ACTIVE 1
#else
#define HARKEN_STAGE_TAPS_ACTIVE 0
#endif

static_assert(std::is_trivially_copyable_v<VnVoiceSnapshot>,
              "snapshot entries cross threads by memcpy semantics");
static_assert(sizeof(VnVoiceSnapshot) == 24,
              "voice snapshot entry layout is frozen");
static_assert(std::is_trivially_copyable_v<VnSnapshot>,
              "the public snapshot must stay a plain POD");
static_assert(sizeof(VnSnapshot) == 5400,
              "snapshot layout is frozen (append-only header contract)");

// Peak absolute sample over planar channels [offset, offset + frames),
// max across channels: the stage-tap measurement. A peak (rather than
// the block's last sample) is used because a periodic signal's last
// sample can sit on a zero crossing and read as silence.
inline float StageTapPeak(const float* const* channels,
                          std::uint32_t channelCount, std::uint32_t offset,
                          std::uint32_t frames, float* channelPeaks = nullptr) {
    float peak = 0.0f;
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        const float* p = channels[ch] + offset;
        float channelPeak = 0.0f;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float a = (p[i] < 0.0f) ? -p[i] : p[i];
            if (a > channelPeak) {
                channelPeak = a;
            }
        }
        if (channelPeaks != nullptr)
            channelPeaks[ch] = channelPeak;
        if (channelPeak > peak)
            peak = channelPeak;
    }
    return peak;
}

// Everything the audio side publishes per block. All render-side facts in
// one POD so a capture is coherent as a set — voice states, taps, bus
// peaks, and arena marks all describe the same block. blockIndex and
// blockIndexEcho are coherence canaries: the writer fills blockIndex
// first and blockIndexEcho last, so a copy with matching canaries was not
// torn across the payload.
struct SnapshotPayload {
    std::uint64_t blockIndex = 0; // canary: the writer fills this first
    std::uint32_t activeVoiceCount = 0;
    std::uint32_t reserved0 = 0; // keeps 8-byte alignment stable
    VnVoiceSnapshot voices[VN_SNAPSHOT_MAX_VOICES] = {};
    std::uint32_t busCount = 0;
    float busPeaks[VN_SNAPSHOT_MAX_BUSES] = {};
    float busPeaksPreFader[VN_SNAPSHOT_MAX_BUSES] = {};
    float busPeaksPostFader[VN_SNAPSHOT_MAX_BUSES] = {};
    float busChannelPeaks[VN_SNAPSHOT_MAX_BUSES * VN_MAX_CHANNELS] = {};
    std::uint64_t arenaHighWaterBytes = 0;
    std::uint64_t arenaOverflowCount = 0;
    std::uint64_t voiceCompletedCount = 0;
    std::uint64_t blocksRendered = 0;
    std::uint64_t voiceStarvedCount = 0; // STARVED notifications emitted,
                                         // audio-owned
    // Per-stage CPU timing for the block this payload describes, in whole
    // microseconds from QPC tick reads taken at the stage boundaries inside
    // renderBlock. The measured windows share boundary reads, so
    // drain + voices + buses can never exceed whole (truncating conversion
    // from shared boundaries); the acceptance tolerance exists only as
    // headroom against a future rounding change. `whole` runs from callback
    // entry to just before this payload is filled — the publication write
    // itself is the one slice a same-block publication cannot contain.
    std::uint32_t audioCpuLastUs = 0;   // whole callback, this block
    std::uint32_t audioCpuPeakUs = 0;   // max whole-callback micros since init
    std::uint32_t audioDrainLastUs = 0; // command-drain stage (incl. the
                                        // output-silence memset ahead of it)
    std::uint32_t audioVoicesLastUs = 0; // voice cascades (incl. arena reset
                                         // and bus-plane setup ahead of them)
    std::uint32_t audioBusesLastUs = 0; // bus fold + master interleave
    // Job-parallel timing (M5 §10): the peak per-block busy span across the
    // render workers since init, in whole microseconds. Published here with
    // the timing septet through the same pinned buffer — a passive audio-thread
    // read, no allocation, no locks. Zero when rendering serially. Occupies the
    // slot the timing reserve held, so the echo stays 8-byte aligned.
    std::uint32_t renderWorkerBusyPeakUs = 0;
    // Audio-owned Dynamic Route and Compressor session diagnostics. These
    // counters cannot be read directly by the logic thread; publishing them
    // here preserves both the no-data-race rule and snapshot coherence.
    std::uint64_t dynamicTapGenerationMismatchCount = 0;
    std::uint64_t dynamicPlanPublicationCount = 0;
    std::uint64_t dynamicPlanRetireOverflowCount = 0;
    std::uint64_t compressorDetectorNonFiniteCount = 0;
    std::uint64_t compressorDetectorSaturationCount = 0;
    // Audio-owned command-drain high-water; the logic side consumes the
    // coherent published value rather than reading the live mixer state.
    std::uint64_t commandDeferredHighWater = 0;
    // Per-quantum producer-budget facts. The audio thread samples these from
    // the diagnostic transports while building this payload; the logic side
    // must consume the published copy rather than read the live transports.
    std::uint64_t logQuantumBudgetDropCount = 0;
    std::uint64_t notificationQuantumBudgetDropCount = 0;
    std::uint32_t logQuantumProducedHighWater = 0;
    std::uint32_t notificationQuantumProducedHighWater = 0;
    std::uint64_t deadlineMissQuanta = 0;
    std::uint64_t lateConsumedEvents = 0;
    std::uint64_t blockIndexEcho = 0; // canary: the writer fills this last
};

static_assert(std::is_trivially_copyable_v<SnapshotPayload>,
              "the payload crosses threads by memcpy semantics");

class SnapshotBuffer {
public:
    // Writer side (audio thread). Three slots guarantee one candidate that is
    // neither the last publication nor the single reader's current pin.
    SnapshotPayload& BeginWrite() {
        const std::uint32_t pin = m_pin.load(std::memory_order_seq_cst);
        m_writeIndex = (m_lastPublished + 1u) % kSlotCount;
        if (m_writeIndex == pin) {
            m_writeIndex = (m_writeIndex + 1u) % kSlotCount;
        }
        return m_slots[m_writeIndex];
    }

    void EndWrite() {
        m_published.store(m_writeIndex, std::memory_order_seq_cst);
        m_lastPublished = m_writeIndex;
    }

    // Reader side (logic thread). A confirmed pin gives exclusive read
    // ownership of the candidate until the copy completes. A failed bounded
    // capture leaves the caller's previous snapshot untouched.
    bool Capture(SnapshotPayload& out) const {
        for (int attempt = 0; attempt < kCaptureAttempts; ++attempt) {
            const std::uint32_t candidate =
                m_published.load(std::memory_order_seq_cst);
            m_pin.store(candidate, std::memory_order_seq_cst);
            if (m_published.load(std::memory_order_seq_cst) != candidate) {
                m_pin.store(kNoPin, std::memory_order_release);
                continue;
            }
            SnapshotPayload scratch;
            std::memcpy(&scratch, &m_slots[candidate], sizeof scratch);
            m_pin.store(kNoPin, std::memory_order_release);
            out = scratch;
            return true;
        }
        m_pin.store(kNoPin, std::memory_order_release);
        return false;
    }

private:
    static constexpr int kCaptureAttempts = 4;
    static constexpr std::uint32_t kSlotCount = 3;
    static constexpr std::uint32_t kNoPin = kSlotCount;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "control words must be lock-free on the audio thread");
    static_assert(kSlotCount == 3,
                  "single-reader ownership requires three slots");
    static_assert(kNoPin == kSlotCount,
                  "the no-pin value must immediately follow the slots");

    SnapshotPayload m_slots[kSlotCount] = {};
    std::atomic<std::uint32_t> m_published{0};
    mutable std::atomic<std::uint32_t> m_pin{kNoPin};
    std::uint32_t m_lastPublished = 0; // writer-owned
    std::uint32_t m_writeIndex = 1;    // writer-owned selected slot
};

} // namespace harken

#endif // HARKEN_CORE_SNAPSHOT_BUFFER_H
