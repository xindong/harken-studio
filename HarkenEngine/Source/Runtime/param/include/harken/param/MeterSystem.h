// Private meter probes, logic-thread side: the probe row table and the
// ballistics that smooth drained block peaks into per-probe levels.
//
// A probe is a private envelope detector over one bus tap. Probes are
// never Variables: the binding evaluator reads a probe's level directly
// from this table and folds it through each probe binding's curve; no
// Variable definition, bank, ticket, or history is involved. Several
// probes may observe one bus (different taps or ballistics); each row is
// keyed by its stable id and generation.
//
// Ballistics run on the audio-frame timebase: per drained block, one-pole
// smoothing advances by that block's frame count, never by wall-clock tick
// spacing, so the envelope after any block sequence is identical regardless
// of how ticks quantize the drain — deterministic on the null backend,
// frame-rate independent in game, golden-testable in closed form:
//
//   coeff(ms) = exp(-blockFrames / (sampleRate * ms / 1000))
//   env       = peak + (env - peak) * (peak > env ? attackCoeff
//                                                 : releaseCoeff)
//
// This is the standard first-order (RC one-pole) exponential averager of
// envelope detection; see e.g. U. Zoelzer, "Digital Audio Signal
// Processing" (envelope followers) — a public textbook formula, not an
// in-house invention. A zero time constant selects an instant coefficient
// of 0 (the envelope lands on the peak in one block).
//
// A probe's level is the ballistics output in dBFS, floored at -96 (never
// -inf), of the bus tap its identity selects. Logic thread only; fixed
// storage, preallocated at construction. Lifecycle rows carry the
// withdrawal fence state the engine seam walks when a probe uninstalls or
// its source bus dies: contributions ramp to neutral, publication settles,
// an audio fence passes, then the row retires and its generation is spent.
#ifndef HARKEN_PARAM_METER_SYSTEM_H
#define HARKEN_PARAM_METER_SYSTEM_H

#include <harken/ObjectId.h>
#include <harken/Harken.h>

#include "harken/core/MeterBlockRing.h"
#include "harken/core/VariableSliceTable.h"

#include <cstdint>
#include <vector>

namespace harken {

// The dBFS floor of a probe level: silence reads -96, never -inf.
inline constexpr float kMeterFloorDb = -96.0f;

struct MeterProbeIdentity {
    VnObjectId probeStableId{};
    std::uint32_t probeGeneration = 0;
    VnBusId sourceBusStableId{};
    std::uint32_t sourceBusGeneration = 0;
    std::uint8_t tap = 0;
};

class MeterSystem {
public:
    enum class State : std::uint8_t {
        Free = 0,
        Installed = 1,
        Withdrawing = 2,
        Retired = 3,
        Quarantined = 4,
    };

    struct Probe {
        State state = State::Free;
        MeterProbeIdentity identity{};
        std::uint32_t attackMs = 0;
        std::uint32_t releaseMs = 0;
        std::uint32_t sourceBusIndex = 0;
        // Per-block one-pole coefficients, precomputed at install time from
        // the session's fixed block size and sample rate.
        float attackCoeff = 0.0f;
        float releaseCoeff = 0.0f;
        // Smoothed level in the linear peak domain (dBFS conversion happens
        // at the read). Starts at silence.
        float envelope = 0.0f;
        // The ring's write cursor sampled at the install call's entry — the
        // entry drain's boundary. Records appended before it predate this
        // probe and never advance it; records at or past it reach it.
        std::uint64_t epochPosition = 0;
        // Withdrawal fence state, driven by ServiceWithdrawals: the widest
        // transition of the probe's neutralized bindings, the command
        // frontier its neutral publication must settle past, and the audio
        // block the ramp must cross before the row retires.
        std::uint32_t withdrawalRampMs = 0;
        std::uint64_t withdrawalCommandFrontier = 0;
        std::uint64_t withdrawalChunkCommandFrontier = 0;
        std::uint64_t withdrawalAudioFenceBlock = 0;
        std::uint64_t withdrawalLogicSequence = 0;
        VariableSliceRef withdrawalSlice{};
        bool withdrawalFrontierCaptured = false;
        bool withdrawalSettled = false;
        bool withdrawalProjected = false;
        bool withdrawalSliceCaptured = false;
        bool withdrawalPageCommitted = false;
        bool withdrawalAllParticipantsReleased = false;
    };

    MeterSystem(std::uint32_t maxProbes, std::uint32_t blockFrames,
                std::uint32_t sampleRate);

    // Installs a probe row. Refuses a zero or duplicate stable id, an
    // out-of-range tap, an invalid bus index, and a full table; a spent
    // generation counter quarantines the slot instead of wrapping. The new
    // generation is minted from the session-global checked sequence.
    VnResult Install(VnObjectId probeStableId,
                     VnBusId sourceBusStableId,
                     std::uint32_t sourceBusIndex,
                     std::uint32_t sourceBusGeneration, std::uint32_t tap,
                     std::uint32_t attackMs, std::uint32_t releaseMs,
                     std::uint64_t epochPosition,
                     std::uint32_t* installedGeneration = nullptr);

    // Row access for the engine seam's lifecycle orchestration. Find skips
    // Free and Retired rows: a retired stable id is gone, only its spent
    // generation lingers in the slot for the next mint.
    Probe* Find(VnObjectId probeStableId) noexcept;
    const Probe* Find(VnObjectId probeStableId) const noexcept;
    std::uint32_t Capacity() const noexcept {
        return static_cast<std::uint32_t>(probes_.size());
    }
    Probe* RowAt(std::uint32_t slot) noexcept {
        return slot < probes_.size() ? &probes_[slot] : nullptr;
    }
    const Probe* RowAt(std::uint32_t slot) const noexcept {
        return slot < probes_.size() ? &probes_[slot] : nullptr;
    }

    // Flips an installed row into Withdrawing and resets its fence state.
    // Contributions remain frozen until the probe service owns their exact
    // current-slice claims.
    void BeginWithdrawal(Probe& probe, std::uint32_t rampMs) noexcept;

    // Compatibility seam for the detector-only unit test. The first three
    // arguments have no authority; production uses the one-argument form.
    std::uint32_t ServiceWithdrawals(bool,
                                     std::uint64_t,
                                     std::uint64_t,
                                     std::uint64_t blocksRendered) noexcept;
    // Retires only rows whose probe-owned current slices have settled and
    // whose probe-owned audio fence has passed.
    std::uint32_t ServiceWithdrawals(
        std::uint64_t blocksRendered) noexcept;

    // Drains the transport into the probe ENVELOPES only. Drain() snapshots
    // the ring's write cursor as its boundary and forwards to DrainUpTo:
    //  - ordinary records appended BEFORE the boundary advance every
    //    installed probe whose epoch is at or before the record's position
    //    and whose source bus generation matches, one block per record;
    //    records appended during the drain wait for the next round (real
    //    time order preserved);
    //  - then, when the overflow COUNTER moved since the last drain, the
    //    side-record is applied through the ring's VALIDATED take: only
    //    when the newest merge's position does not exceed the queue
    //    front (otherwise the block DEFERS untouched to a later round),
    //    re-checked after the lane exchange (a failed re-check merges
    //    the taken lanes back and defers likewise). An applied block
    //    advances ballistics ONE compressed step for EVERY installed
    //    probe — lanes may be all zero (silent compressed blocks still
    //    cost a release step: time compression folds many blocks into
    //    one, never into zero). No epoch gate: the install call's entry
    //    drain consumes or defers the side-record at the entry boundary
    //    (the ring header's attribution note and its bounded
    //    overflow-time degradation list).
    // Returns the number of blocks advanced (compressed block included).
    std::uint32_t Drain(MeterBlockRing& ring);
    // The boundary-explicit form, public so the boundary discipline is
    // unit-testable single-threaded (a record pushed past the boundary
    // must survive this round and be consumed by the next).
    std::uint32_t DrainUpTo(MeterBlockRing& ring, std::uint64_t boundary);

    // Current ballistics output of an installed probe in dBFS, floored at
    // -96. A stable id with no installed row reads the floor.
    float ValueDb(VnObjectId probeStableId) const noexcept;

    bool IsProbe(VnObjectId probeStableId) const noexcept {
        return Find(probeStableId) != nullptr;
    }
    std::uint32_t ActiveCount() const noexcept { return activeCount_; }
    // The native generation mint never wraps or resets during this session.
    // Withdrawal and retirement preserve the history of the first installation.
    bool HasInstallationHistory() const noexcept {
        return nextProbeGeneration_ != 1;
    }
    std::uint64_t HighWater() const noexcept { return highWater_; }
    std::uint64_t RefusalCount() const noexcept { return refusalCount_; }
    void NoteRefusal() noexcept { ++refusalCount_; }

    template <typename Fn>
    void ForEachInstalled(Fn&& fn) const {
        for (const Probe& probe : probes_) {
            if (probe.state == State::Installed) {
                fn(probe.identity, probe.attackMs, probe.releaseMs);
            }
        }
    }

private:
    std::vector<Probe> probes_;
    std::uint32_t blockFrames_ = 0;
    std::uint32_t sampleRate_ = 0;
    // Consumer-side landing row for the ring's overflow side-record,
    // preallocated so the drain allocates nothing.
    std::vector<float> overflowScratch_;
    std::vector<std::uint32_t> overflowGenerationScratch_;
    // The overflow counter total this consumer has accounted for: a
    // nonzero delta at drain time IS the existence of one compressed
    // block, independent of lane values. Advances only when a validated
    // take APPLIES, and to the pre-take counter read, so a deferred or
    // straddling merge keeps its count pending and its peaks deliverable.
    std::uint64_t lastSeenOverflowCount_ = 0;
    std::uint32_t nextProbeGeneration_ = 1;
    bool probeGenerationExhausted_ = false;
    std::uint32_t activeCount_ = 0;
    std::uint64_t highWater_ = 0;
    std::uint64_t refusalCount_ = 0;
};

} // namespace harken

#endif // HARKEN_PARAM_METER_SYSTEM_H
