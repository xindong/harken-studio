// Probe row table and ballistics. The engine-session lifecycle entries
// (install/uninstall orchestration, binding neutralization) live with the
// engine wiring, which owns the Engine type; the pure row/ballistics state
// lives here.
#include "harken/param/MeterSystem.h"

#include <algorithm>
#include <cmath>

namespace harken {

namespace {

// One-pole per-block coefficient: exp(-blockFrames / (sampleRate * ms /
// 1000)). The remaining-fraction form — after one block the envelope keeps
// coeff of its distance to the peak. A zero time constant is the instant
// detector (coefficient 0: the envelope lands on the peak in one block);
// branching keeps the intent explicit rather than leaning on the IEEE
// divide-by-zero chain. Computed in double, stored as float, so the golden
// tests reproduce the exact stored value from the same expression.
float BlockCoeff(std::uint32_t blockFrames, std::uint32_t sampleRate,
                 std::uint32_t ms) {
    if (ms == 0) {
        return 0.0f;
    }
    const double tauFrames =
        static_cast<double>(sampleRate) * static_cast<double>(ms) / 1000.0;
    return static_cast<float>(
        std::exp(-static_cast<double>(blockFrames) / tauFrames));
}

} // namespace

MeterSystem::MeterSystem(std::uint32_t maxProbes, std::uint32_t blockFrames,
                         std::uint32_t sampleRate)
    : probes_(maxProbes), blockFrames_(blockFrames),
      sampleRate_(sampleRate),
      overflowScratch_(static_cast<std::size_t>(kMeterTapLaneCount) *
                           maxProbes,
                       0.0f),
      overflowGenerationScratch_(
          static_cast<std::size_t>(kMeterTapLaneCount) * maxProbes, 0) {}

MeterSystem::Probe* MeterSystem::Find(VnObjectId probeStableId) noexcept {
    for (Probe& probe : probes_) {
        if (probe.state != State::Free && probe.state != State::Retired &&
            probe.identity.probeStableId == probeStableId) {
            return &probe;
        }
    }
    return nullptr;
}

const MeterSystem::Probe* MeterSystem::Find(
    VnObjectId probeStableId) const noexcept {
    for (const Probe& probe : probes_) {
        if (probe.state != State::Free && probe.state != State::Retired &&
            probe.identity.probeStableId == probeStableId) {
            return &probe;
        }
    }
    return nullptr;
}

VnResult MeterSystem::Install(VnObjectId probeStableId,
                              VnBusId sourceBusStableId,
                              std::uint32_t sourceBusIndex,
                              std::uint32_t sourceBusGeneration,
                              std::uint32_t tap, std::uint32_t attackMs,
                              std::uint32_t releaseMs,
                              std::uint64_t epochPosition,
                              std::uint32_t* installedGeneration) {
    if (probeStableId == VnObjectId{} || tap > VN_BUS_METER_TAP_POST_FADER) {
        ++refusalCount_;
        return VN_INVALID_ARG;
    }
    if (Find(probeStableId) != nullptr) {
        ++refusalCount_;
        return VN_IN_USE;
    }
    Probe* free = nullptr;
    for (Probe& probe : probes_) {
        if (probe.state == State::Free || probe.state == State::Retired) {
            free = &probe;
            break;
        }
    }
    if (free == nullptr) {
        ++refusalCount_;
        return VN_NO_BINDING;
    }
    if (probeGenerationExhausted_) {
        ++refusalCount_;
        return VN_BACKPRESSURE;
    }
    const std::uint32_t nextGeneration = nextProbeGeneration_;
    if (nextProbeGeneration_ == UINT32_MAX) {
        probeGenerationExhausted_ = true;
    } else {
        ++nextProbeGeneration_;
    }
    Probe value{};
    value.state = State::Installed;
    value.identity.probeStableId = probeStableId;
    value.identity.probeGeneration = nextGeneration;
    value.identity.sourceBusStableId = sourceBusStableId;
    value.identity.sourceBusGeneration = sourceBusGeneration;
    value.identity.tap = static_cast<std::uint8_t>(tap);
    value.attackMs = attackMs;
    value.releaseMs = releaseMs;
    value.sourceBusIndex = sourceBusIndex;
    value.attackCoeff = BlockCoeff(blockFrames_, sampleRate_, attackMs);
    value.releaseCoeff = BlockCoeff(blockFrames_, sampleRate_, releaseMs);
    value.envelope = 0.0f; // born at silence: the floor until signal lands
    // Records already queued in the transport predate this probe and never
    // advance it.
    value.epochPosition = epochPosition;
    *free = value;
    ++activeCount_;
    highWater_ = std::max<std::uint64_t>(highWater_, activeCount_);
    if (installedGeneration != nullptr) {
        *installedGeneration = nextGeneration;
    }
    return VN_OK;
}

void MeterSystem::BeginWithdrawal(Probe& probe,
                                  std::uint32_t rampMs) noexcept {
    if (probe.state != State::Installed) {
        return;
    }
    probe.state = State::Withdrawing;
    probe.withdrawalRampMs = rampMs;
    probe.withdrawalCommandFrontier = 0;
    probe.withdrawalChunkCommandFrontier = 0;
    probe.withdrawalAudioFenceBlock = 0;
    probe.withdrawalLogicSequence = 0;
    probe.withdrawalSlice = {};
    probe.withdrawalFrontierCaptured = false;
    probe.withdrawalSettled = false;
    probe.withdrawalProjected = false;
    probe.withdrawalSliceCaptured = false;
    probe.withdrawalPageCommitted = false;
    probe.withdrawalAllParticipantsReleased = false;
    --activeCount_;
}

std::uint32_t MeterSystem::ServiceWithdrawals(
    bool, std::uint64_t, std::uint64_t,
    std::uint64_t blocksRendered) noexcept {
    for (Probe& probe : probes_) {
        if (probe.state == State::Withdrawing &&
            !probe.withdrawalFrontierCaptured &&
            !probe.withdrawalSlice.Valid()) {
            probe.withdrawalAudioFenceBlock =
                blocksRendered == UINT64_MAX ? UINT64_MAX
                                             : blocksRendered + 1u;
            probe.withdrawalFrontierCaptured = true;
            probe.withdrawalSettled = true;
        }
    }
    return ServiceWithdrawals(blocksRendered);
}

std::uint32_t MeterSystem::ServiceWithdrawals(
    std::uint64_t blocksRendered) noexcept {
    std::uint32_t retired = 0;
    for (Probe& probe : probes_) {
        if (probe.state != State::Withdrawing) {
            continue;
        }
        if (!probe.withdrawalSettled ||
            blocksRendered < probe.withdrawalAudioFenceBlock) {
            continue;
        }
        probe.state = State::Retired;
        ++retired;
    }
    return retired;
}

std::uint32_t MeterSystem::Drain(MeterBlockRing& ring) {
    // The boundary snapshot keeps the drain a fixed block sequence:
    // records appended while this drain runs wait for the next round,
    // and the validated take below refuses to apply compressed content
    // ahead of queued records that really precede it (attack/release do
    // not commute — reordering a compressed peak past later silence
    // would change the envelope's end value).
    return DrainUpTo(ring, ring.WriteCursor());
}

std::uint32_t MeterSystem::DrainUpTo(MeterBlockRing& ring,
                                     std::uint64_t boundary) {
    std::uint32_t drained = 0;
    std::uint64_t blockIndex = 0;
    const float* preFaderPeaks = nullptr;
    const float* postFaderPeaks = nullptr;
    const float* postEffectsPeaks = nullptr;
    const std::uint32_t* generations = nullptr;
    while (ring.FrontPosition() < boundary &&
           ring.FrontTaps(&blockIndex, &preFaderPeaks, &postFaderPeaks,
                          &postEffectsPeaks, &generations)) {
        const float* tapRows[kMeterTapLaneCount] = {
            postEffectsPeaks, preFaderPeaks, postFaderPeaks};
        // The record's append position, exact and consumer-owned: a probe
        // consumes only records appended at or after its epoch — peaks
        // another consumer kept flowing before this probe existed must not
        // advance a freshly reset envelope.
        const std::uint64_t position = ring.FrontPosition();
        for (Probe& probe : probes_) {
            if (probe.state != State::Installed ||
                position < probe.epochPosition) {
                continue;
            }
            const std::uint32_t bus = probe.sourceBusIndex;
            if (bus >= ring.MaxBuses() ||
                generations[bus] != probe.identity.sourceBusGeneration) {
                continue;
            }
            const float peak = tapRows[probe.identity.tap][bus];
            const float coeff = peak > probe.envelope ? probe.attackCoeff
                                                      : probe.releaseCoeff;
            // env moves toward the peak, keeping coeff of its distance —
            // float arithmetic throughout so the closed-form goldens
            // reproduce the trajectory bit-exactly.
            probe.envelope = peak + (probe.envelope - peak) * coeff;
        }
        ring.PopFront();
        ++drained;
    }
    // The overflow side-record, applied through the ring's VALIDATED
    // take (the ring header's protocol note). The COUNTER's delta is the
    // existence of one compressed block — lanes may be all zero (silent
    // compressed blocks still cost one release step; time compression
    // folds many blocks into one, never into zero). The take applies
    // only when the newest merge's position does not exceed the queue
    // front — otherwise records still queued are EARLIER than the
    // compressed content and the block defers untouched until a later
    // round has consumed them — and the stamp is re-checked after the
    // exchange: a merge landing inside the take window merges the taken
    // lanes back and defers the block whole (delayed, never lost, never
    // applied out of order). Delivered to every installed probe, no epoch
    // gate: the install call's entry drain consumes or defers the
    // side-record at the entry boundary, so an applied block's content
    // post-dates every consumer (up to the bounded overflow-time
    // degradation recorded in the ring header).
    const std::uint64_t counted = ring.OverflowCount();
    if (counted != lastSeenOverflowCount_ &&
        ring.OverflowPosition() <= ring.FrontPosition()) {
        float* const overflowPostEffectsPeaks =
            overflowScratch_.data() +
            MeterTapOffset(VN_BUS_METER_TAP_POST_EFFECTS, ring.MaxBuses());
        float* const overflowPreFaderPeaks =
            overflowScratch_.data() +
            MeterTapOffset(VN_BUS_METER_TAP_PRE_FADER, ring.MaxBuses());
        float* const overflowPostFaderPeaks =
            overflowScratch_.data() +
            MeterTapOffset(VN_BUS_METER_TAP_POST_FADER, ring.MaxBuses());
        ring.TakeOverflowTaps(overflowPreFaderPeaks, overflowPostFaderPeaks,
                              overflowPostEffectsPeaks,
                              overflowGenerationScratch_.data());
        if (ring.OverflowPosition() > ring.FrontPosition()) {
            ring.MergeBackOverflowTaps(overflowPreFaderPeaks,
                                       overflowPostFaderPeaks,
                                       overflowPostEffectsPeaks,
                                       overflowGenerationScratch_.data());
        } else {
            for (Probe& probe : probes_) {
                if (probe.state != State::Installed) {
                    continue;
                }
                const std::size_t lane =
                    MeterTapOffset(probe.identity.tap, ring.MaxBuses()) +
                    probe.sourceBusIndex;
                if (lane >= overflowScratch_.size() ||
                    overflowGenerationScratch_[lane] !=
                        probe.identity.sourceBusGeneration) {
                    continue;
                }
                const float peak = overflowScratch_[lane];
                const float coeff = peak > probe.envelope
                                        ? probe.attackCoeff
                                        : probe.releaseCoeff;
                probe.envelope = peak + (probe.envelope - peak) * coeff;
            }
            ++drained;
            // The watermark takes the PRE-take counter read: a merge
            // whose lanes escaped the exchange keeps its count pending,
            // so its peaks deliver on a later round instead of
            // stranding in the lanes behind an absorbed count. A merge
            // whose lanes WERE captured but whose count lands after
            // this read splits into this peak step plus one later
            // all-silent release step — the release-conservative
            // residual in the ring header.
            lastSeenOverflowCount_ = counted;
        }
    }
    return drained;
}

float MeterSystem::ValueDb(VnObjectId probeStableId) const noexcept {
    const Probe* probe = Find(probeStableId);
    if (probe == nullptr || probe->state != State::Installed ||
        !(probe->envelope > 0.0f)) {
        return kMeterFloorDb; // silence (and any denormal-flushed zero)
    }
    const float db = 20.0f * std::log10(probe->envelope);
    return db < kMeterFloorDb ? kMeterFloorDb : db;
}

} // namespace harken
