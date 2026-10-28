#include "harken_program_registration/ProgramRegistration.h"

#include "harken/core/PlayablePropertyCut.h"
#include "UnitTest/StudioObjectIdTestSupport.h"

#include "harken_program_registration/EffectDescriptorWriter.h"
#include "harken_project_schema/FixedSlotLowerer.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/SegmentIR.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace harkenprogramreg {
namespace {

// ClockKernel's accepted sample-rate domain (see ClockKernel.h). Checked here so a
// bad device rate is a named refusal, not a downstream Reset failure.
constexpr std::int64_t kMinSampleRateHz = 1;
constexpr std::int64_t kMaxSampleRateHz = 3'000'000;

// A placeholder tempo for an Absolute segment's clock. TimeToFrame ignores the tempo
// map entirely (only the sample rate relates nanoseconds to frames), but Reset
// still requires one in-domain row; 120 BPM is in domain for every accepted rate.
constexpr std::int64_t kPlaceholderMicrosPerQuarter = 500'000;
constexpr std::int64_t kAbsolutePlaceholderMicrosPerQuarter = 60'000'000'000;

// The two enums differ in ordinal between the tooling model and the runtime marker
// index, so the mapping is explicit rather than a cast.
bool ToRuntimeMarkerKind(harkenprojectschema::MarkerKind kind,
                      harken::MarkerKind& mapped) {
    switch (kind) {
    case harkenprojectschema::MarkerKind::Custom:
        mapped = harken::MarkerKind::Custom;
        return true;
    case harkenprojectschema::MarkerKind::Exit:
        mapped = harken::MarkerKind::Exit;
        return true;
    }
    return false;
}

bool ToTimelineEventKind(harkenprojectschema::TemplateEventKind kind,
                         harken::TimelineEventKind* mapped) {
    if (mapped == nullptr) {
        return false;
    }
    switch (kind) {
    case harkenprojectschema::TemplateEventKind::SetStartCursor:
        *mapped = harken::TimelineEventKind::SetStartCursor;
        return true;
    case harkenprojectschema::TemplateEventKind::Start:
        *mapped = harken::TimelineEventKind::Start;
        return true;
    case harkenprojectschema::TemplateEventKind::Envelope:
        *mapped = harken::TimelineEventKind::Envelope;
        return true;
    case harkenprojectschema::TemplateEventKind::Stop:
        *mapped = harken::TimelineEventKind::Stop;
        return true;
    }
    return false;
}

bool ToRuntimeTimeType(harkenprojectschema::SegmentTimeKind kind,
                       harken::SegmentTimeType& mapped) {
    switch (kind) {
    case harkenprojectschema::SegmentTimeKind::Absolute:
        mapped = harken::SegmentTimeType::Absolute;
        return true;
    case harkenprojectschema::SegmentTimeKind::Musical:
        mapped = harken::SegmentTimeType::Musical;
        return true;
    }
    return false;
}

struct Wide {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
};

// Full 64x64 -> 128 unsigned multiply via 32-bit halves. This local portable
// path keeps registration bit-identical on MSVC, which has no __int128.
Wide MulU64(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t aLo = a & 0xFFFFFFFFull;
    const std::uint64_t aHi = a >> 32;
    const std::uint64_t bLo = b & 0xFFFFFFFFull;
    const std::uint64_t bHi = b >> 32;
    const std::uint64_t ll = aLo * bLo;
    const std::uint64_t lh = aLo * bHi;
    const std::uint64_t hl = aHi * bLo;
    const std::uint64_t hh = aHi * bHi;
    const std::uint64_t cross =
        (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
    return Wide{hh + (lh >> 32) + (hl >> 32) + (cross >> 32),
                (ll & 0xFFFFFFFFull) | (cross << 32)};
}

std::uint64_t DivideWideFloorSaturating(Wide numerator,
                                        std::uint64_t denominator) {
    // numerator.hi >= denominator means the exact quotient is at least 2^64.
    if (numerator.hi >= denominator) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    std::uint64_t quotient = 0;
    std::uint64_t remainder = numerator.hi;
    for (int bitIndex = 63; bitIndex >= 0; --bitIndex) {
        const bool carry = (remainder >> 63) != 0;
        remainder = (remainder << 1) | ((numerator.lo >> bitIndex) & 1u);
        if (carry || remainder >= denominator) {
            remainder -= denominator;
            quotient |= std::uint64_t{1} << bitIndex;
        }
    }
    return quotient;
}

std::int64_t SaturatingAddNonNegative(std::int64_t value,
                                      std::int64_t nonnegative) {
    const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    return value > maximum - nonnegative ? maximum : value + nonnegative;
}

bool LowerFadeFrames(const harken::ClockKernel& kernel,
                     harken::SegmentTimeType timeType,
                     std::int64_t occurrenceStart,
                     const harkenprojectschema::FadeSpec& fades,
                     std::uint64_t span, std::uint64_t* outIn,
                     std::uint64_t* outOut) {
    std::uint64_t rawIn = 0;
    std::uint64_t rawOut = 0;
    const bool inOk = timeType == harken::SegmentTimeType::Musical
                          ? kernel.TryTickSpanToFrames(
                                harken::MusicalTick{occurrenceStart},
                                fades.inLength, &rawIn)
                          : kernel.TryTimeSpanToFrames(
                                harken::AbsoluteTime{occurrenceStart},
                                fades.inLength, &rawIn);
    const bool outOk = timeType == harken::SegmentTimeType::Musical
                           ? kernel.TryTickSpanToFrames(
                                 harken::MusicalTick{occurrenceStart},
                                 fades.outLength, &rawOut)
                           : kernel.TryTimeSpanToFrames(
                                 harken::AbsoluteTime{occurrenceStart},
                                 fades.outLength, &rawOut);
    if (!inOk || !outOk) {
        return false;
    }
    if (rawIn > UINT64_MAX - rawOut) {
        return false;
    }
    const std::uint64_t total = rawIn + rawOut;
    std::uint64_t inFrames = rawIn;
    std::uint64_t outFrames = rawOut;
    if (total > span && total != 0) {
        inFrames = DivideWideFloorSaturating(MulU64(span, rawIn), total);
        outFrames = span - inFrames;
    }
    *outIn = inFrames < 2 ? 0 : inFrames;
    *outOut = outFrames < 2 ? 0 : outFrames;
    return true;
}

bool BuildScheduleKernel(harken::ScheduleTimeKindV1 domain,
                         std::int64_t microsPerQuarter,
                         std::uint64_t deviceRateHz,
                         harken::ClockKernel& kernel,
                         harken::SegmentTimeType& timeType,
                         harken::TempoSegment& tempo) {
    if (deviceRateHz == 0 || deviceRateHz > INT64_MAX) {
        return false;
    }
    tempo = {};
    if (domain == harken::ScheduleTimeKindV1::AbsoluteNanoseconds) {
        if (microsPerQuarter != 0) return false;
        timeType = harken::SegmentTimeType::Absolute;
        tempo.microsPerQuarter = kAbsolutePlaceholderMicrosPerQuarter;
    } else if (domain == harken::ScheduleTimeKindV1::MusicalTicks) {
        timeType = harken::SegmentTimeType::Musical;
        tempo.microsPerQuarter = microsPerQuarter;
    } else {
        return false;
    }
    return kernel.Reset(static_cast<std::int64_t>(deviceRateHz), &tempo, 1);
}

const harkenprojectschema::IrSegment* FindSegment(
    const harkenprojectschema::SegmentIR& ir, harkenstudio::ObjectId segmentId) {
    for (const harkenprojectschema::IrSegment& segment : ir.segments()) {
        if (segment.segmentId == segmentId) {
            return &segment;
        }
    }
    return nullptr;
}

const harkenprojectschema::IrTrack* FindTrack(
    const harkenprojectschema::SegmentIR& ir, harkenstudio::ObjectId trackId) {
    for (const harkenprojectschema::IrTrack& track : ir.tracks()) {
        if (track.trackId == trackId) {
            return &track;
        }
    }
    return nullptr;
}

RegistrationResult Refuse(RegistrationRefusal refusal) {
    RegistrationResult result;
    result.ok = false;
    result.refusal = refusal;
    return result;
}

RegistrationResult Refuse(RegistrationRefusal refusal,
                          const RegistrationRefusalContext& context) {
    RegistrationResult result = Refuse(refusal);
    result.refusalContext = context;
    return result;
}

// Maps a descriptor-writer refusal onto the registration code that names it, so
// the writer stays a pure byte function with no opinion about registration.
RegistrationRefusal RefusalForWriteStatus(EffectDescriptorWriteStatus status) {
    switch (status) {
    case EffectDescriptorWriteStatus::LayoutUnsupported:
        return RegistrationRefusal::EffectTypeDescriptorLayoutUnsupported;
    case EffectDescriptorWriteStatus::VintageUnsupported:
        return RegistrationRefusal::EffectTypeDescriptorVintageUnsupported;
    case EffectDescriptorWriteStatus::TooLarge:
        return RegistrationRefusal::EffectTypeDescriptorTooLarge;
    case EffectDescriptorWriteStatus::Ok:
        break;
    }
    return RegistrationRefusal::None;
}

// Admits one compiled chain against what this session actually resolved, and on
// success lowers it onto voice slots and fills the install steps.
//
// This is the load-time gate, and it is deliberately separate from the compile
// gate the chain already passed: the compiler refused a chain longer than the slot
// layout can hold, while this refuses one that does not fit the budget, the
// registered types or the pools of this particular session. Each condition the
// memo's admission rule names is checked as its own refusal so a caller learns
// which one failed:
//   - the budget: entry count against the resolved slot count;
//   - the capabilities: every demanded type registered, able to host a voice
//     insert, and exposing every parameter id the chain authored for it;
//   - the per-type resources: the instances one chain needs against that type's
//     pool.
// The pool check is the necessary per-chain condition, not a concurrency
// reservation: how many voices may hold instances at once is a runtime admission
// question and this stage neither knows nor pretends to answer it.
//
// Every entry also has its parameter payload checked and turned into the concrete
// descriptor the installation seam takes. Three things must hold before a byte is
// written, and each is its own refusal:
//   - the payload authors every parameter index of its type exactly once. A
//     missing index has no legal value to fill it with — the registry's neutral is
//     a binding-combine identity, not an authored default, and zeroes are outside
//     several types' accepted ranges — so a sparse or duplicated payload is
//     refused rather than completed by guesswork;
//   - every value lies inside that parameter's resolved range, which moves a
//     failure the engine would raise when a voice starts to the moment the program
//     is registered. A value that is not a number fails this comparison too, so it
//     is refused here rather than relying on a later check;
//   - the type's descriptor is the layout this build can write, proved by its own
//     captured size relation and vintage rather than assumed.
//
// A chain with no entries is admitted with no steps: overriding an inherited chain
// with nothing asks the session for no slot, no type and no instance.
bool AdmitAndLowerChain(const harkenprojectschema::CompiledEffectChain& chain,
                        const RegistrationParams& params,
                        std::vector<VoiceEffectSlot>* outSlots,
                        RegistrationRefusal* outRefusal,
                        RegistrationRefusalContext* outContext) {
    outSlots->clear();
    if (chain.entries.empty()) {
        return true;
    }
    if (chain.entries.size() > params.resolvedVoiceFxSlots) {
        *outRefusal = RegistrationRefusal::EffectChainExceedsResolvedSlots;
        outContext->expected = params.resolvedVoiceFxSlots;
        outContext->actual = chain.entries.size();
        return false;
    }
    for (const harkenprojectschema::EffectTypeDemand& demand :
         chain.resources.perTypeDemand) {
        outContext->effectType = demand.effectType;
        const auto typeIt = params.effectTypes.find(demand.effectType);
        if (typeIt == params.effectTypes.end()) {
            *outRefusal = RegistrationRefusal::EffectTypeUnavailable;
            return false;
        }
        const EffectTypeAvailability& available = typeIt->second;
        if (!available.hostsVoiceInserts) {
            *outRefusal = RegistrationRefusal::EffectTypeCannotHostVoice;
            return false;
        }
        // The authored private index space must fit inside the registered count;
        // zero means the chain authored no parameters of this type.
        const std::uint32_t parameterCount =
            static_cast<std::uint32_t>(available.parameters.size());
        if (demand.requiredParameterCount > parameterCount) {
            *outRefusal = RegistrationRefusal::EffectTypeParameterUnknown;
            outContext->expected = parameterCount;
            outContext->actual = demand.requiredParameterCount - 1u;
            return false;
        }
        if (demand.instanceCount > available.poolCapacity) {
            *outRefusal = RegistrationRefusal::EffectTypePoolInsufficient;
            outContext->expected = available.poolCapacity;
            outContext->actual = demand.instanceCount;
            return false;
        }
    }

    const harkenprojectschema::LowerEffectChainResult lowered =
        harkenprojectschema::LowerEffectChainToFixedSlots(
            chain, harkenprojectschema::AuthoringGuid{});
    if (!lowered.ok() || !lowered.chain.has_value()) {
        *outRefusal = RegistrationRefusal::EffectChainExceedsResolvedSlots;
        outContext->expected = harkenprojectschema::kFixedVoiceFxSlotCount;
        outContext->actual = chain.entries.size();
        return false;
    }

    outSlots->reserve(lowered.chain->slots.size());
    for (const harkenprojectschema::LoweredEffectSlot& loweredSlot :
         lowered.chain->slots) {
        const harkenprojectschema::CompiledEffectChainEntry& entry =
            chain.entries[loweredSlot.entryIndex];
        const EffectTypeAvailability& available =
            params.effectTypes.find(entry.effectType)->second;
        const std::uint32_t parameterCount =
            static_cast<std::uint32_t>(available.parameters.size());
        outContext->effectType = entry.effectType;

        if (available.descriptorVintage !=
            kSupportedEffectDescriptorVintage) {
            *outRefusal =
                RegistrationRefusal::EffectTypeDescriptorVintageUnsupported;
            outContext->expected = kSupportedEffectDescriptorVintage;
            outContext->actual = available.descriptorVintage;
            return false;
        }

        // The payload, gathered into index order. A slot per index, each filled at
        // most once, is what makes "every index exactly once" a counting question
        // rather than a sort.
        const harkenprojectschema::EffectParamBlock& block =
            chain.paramBlocks[entry.paramBlockRef];
        float ordered[kMaxEffectDescriptorParams] = {};
        bool authored[kMaxEffectDescriptorParams] = {};
        if (parameterCount > kMaxEffectDescriptorParams) {
            *outRefusal = RegistrationRefusal::EffectTypeDescriptorTooLarge;
            outContext->expected = kMaxEffectDescriptorParams;
            outContext->actual = parameterCount;
            return false;
        }
        if (block.params.size() != parameterCount) {
            *outRefusal = RegistrationRefusal::EffectTypeParameterSetIncomplete;
            outContext->expected = parameterCount;
            outContext->actual = block.params.size();
            return false;
        }
        for (const harkenprojectschema::EffectParam& param : block.params) {
            if (param.paramId >= parameterCount) {
                *outRefusal = RegistrationRefusal::EffectTypeParameterUnknown;
                outContext->expected = parameterCount;
                outContext->actual = param.paramId;
                return false;
            }
            if (authored[param.paramId]) {
                *outRefusal =
                    RegistrationRefusal::EffectTypeParameterSetIncomplete;
                outContext->expected = parameterCount;
                outContext->actual = param.paramId;
                return false;
            }
            authored[param.paramId] = true;
            ordered[param.paramId] = std::bit_cast<float>(param.valueBits);
        }
        for (std::uint32_t index = 0; index < parameterCount; ++index) {
            if (!authored[index]) {
                *outRefusal =
                    RegistrationRefusal::EffectTypeParameterSetIncomplete;
                outContext->expected = parameterCount;
                outContext->actual = index;
                return false;
            }
            const EffectParameterRange& range = available.parameters[index];
            // Written as a rejection of "outside", so a value that compares
            // unordered against both bounds is refused rather than admitted.
            if (!(ordered[index] >= range.clampMin &&
                  ordered[index] <= range.clampMax)) {
                *outRefusal =
                    RegistrationRefusal::EffectTypeParameterValueOutOfRange;
                outContext->expected = index;
                outContext->actual =
                    std::bit_cast<std::uint32_t>(ordered[index]);
                return false;
            }
        }

        VoiceEffectSlot step;
        step.slot = loweredSlot.slot;
        const EffectDescriptorWriteStatus status = WriteEffectDescriptor(
            static_cast<VnEffectType>(entry.effectType),
            available.descriptorSize, available.descriptorVintage, ordered,
            parameterCount, &step.descriptor, harkentest::ToRuntime(entry.effectId));
        if (status != EffectDescriptorWriteStatus::Ok) {
            *outRefusal = RefusalForWriteStatus(status);
            switch (*outRefusal) {
            case RegistrationRefusal::EffectTypeDescriptorVintageUnsupported:
                outContext->expected = kSupportedEffectDescriptorVintage;
                outContext->actual = available.descriptorVintage;
                break;
            case RegistrationRefusal::EffectTypeDescriptorTooLarge:
                outContext->expected = kMaxEffectDescriptorParams;
                outContext->actual = parameterCount;
                break;
            case RegistrationRefusal::EffectTypeDescriptorLayoutUnsupported:
                outContext->expected = EffectDescriptorSize(parameterCount);
                outContext->actual = available.descriptorSize;
                break;
            default:
                break;
            }
            return false;
        }
        outSlots->push_back(std::move(step));
    }
    return true;
}

} // namespace

harken::TimelineRuntimeInstallResult
ResolvedProgram::InstallTimeline() const noexcept {
    try {
        std::vector<harken::TimelineOccurrenceInstallView> occurrences;
        occurrences.reserve(timelineViews_.size());
        for (std::size_t i = 0; i < timelineViews_.size(); ++i) {
            const std::size_t resolvedIndex =
                timelineOccurrenceToResolved_[i];
            const ResolvedOccurrence& source = occurrences_[resolvedIndex];
            harken::TimelineOccurrenceInstallView occurrence{};
            occurrence.schedule = &timelineViews_[i];
            occurrence.voice.soundId = source.install.soundId;
            occurrence.voice.outputBusId = source.install.outputBus;
            occurrence.voice.volumeDb = source.install.baseVolumeDb;
            occurrence.voice.outputBusVolumeDb = source.install.outputBusVolumeDb;
            occurrence.voice.pitchRatio = source.install.basePitchRatio;
            occurrence.voice.pan = source.install.panning;
            if (!harken::IsValidAuthoredSpreadPercent(source.install.spread)) {
                return {harken::TimelineRuntimeInstallStatus::InvalidProgram,
                        0, 0};
            }
            occurrence.voice.spread =
                harken::AuthoredSpreadPercentToRuntimeWidth(
                    source.install.spread);
            occurrence.voice.attenuationDistanceScaling =
                source.install.attenuationDistanceScaling;
            occurrence.voice.attenuationResolved =
                source.install.attenuationResolved;
            occurrence.voice.attenuationId =
                source.install.resolvedAttenuation;
            occurrence.voice.loopCount = source.install.loopCount;
            occurrence.voice.spatialized = source.install.is3DEnabled;
            if (source.install.sends.size() > harken::kTimelineMaxVoiceSends ||
                source.install.effectSlots.size() >
                    harken::kTimelineMaxVoiceEffects) {
                return {harken::TimelineRuntimeInstallStatus::InvalidProgram, 0,
                        0};
            }
            occurrence.voice.sendCount =
                static_cast<std::uint32_t>(source.install.sends.size());
            for (std::uint32_t send = 0; send < occurrence.voice.sendCount;
                 ++send) {
                occurrence.voice.sends[send].targetBusId =
                    source.install.sends[send].targetBus;
                occurrence.voice.sends[send].levelDb =
                    source.install.sends[send].level;
            }
            occurrence.voice.effectCount = static_cast<std::uint32_t>(
                source.install.effectSlots.size());
            for (std::uint32_t effect = 0;
                 effect < occurrence.voice.effectCount; ++effect) {
                const VoiceEffectSlot& sourceEffect =
                    source.install.effectSlots[effect];
                if (sourceEffect.descriptor.size >
                    harken::kTimelineMaxEffectDescriptorBytes) {
                    return {harken::TimelineRuntimeInstallStatus::InvalidProgram,
                            0, 0};
                }
                harken::TimelineVoiceEffectInstall& targetEffect =
                    occurrence.voice.effects[effect];
                targetEffect.slot = sourceEffect.slot;
                targetEffect.type = sourceEffect.descriptor.type;
                targetEffect.descriptorSize = sourceEffect.descriptor.size;
                std::memcpy(targetEffect.descriptor,
                            sourceEffect.descriptor.data,
                            sourceEffect.descriptor.size);
            }
            occurrence.voice.target = source.timelineTarget;
            occurrences.push_back(occurrence);
        }
        const harken::TimelineProgramInstallView install{
            &timelineProgram_, occurrences.data(), occurrences.size()};
        return harken::InstallTimelineProgram(install);
    } catch (...) {
        return {harken::TimelineRuntimeInstallStatus::ProgramCapacity, 0, 0};
    }
}

std::uint64_t RescaleFrameCountFloor(std::uint64_t frameCount,
                                     std::uint64_t fromRateHz,
                                     std::uint64_t toRateHz) {
    if (frameCount == 0 || fromRateHz == 0 || toRateHz == 0) {
        return 0;
    }
    if (fromRateHz == toRateHz) {
        return frameCount;
    }
    return DivideWideFloorSaturating(MulU64(frameCount, toRateHz),
                                     fromRateHz);
}

bool TryRescaleFrameCountFloor(std::uint64_t frameCount,
                               std::uint64_t fromRateHz,
                               std::uint64_t toRateHz,
                               std::uint64_t* outFrames) noexcept {
    if (outFrames == nullptr || fromRateHz == 0 || toRateHz == 0) {
        return false;
    }
    if (frameCount == 0 || fromRateHz == toRateHz) {
        *outFrames = frameCount;
        return true;
    }
    const Wide numerator = MulU64(frameCount, toRateHz);
    if (numerator.hi >= fromRateHz) {
        return false;
    }
    *outFrames = DivideWideFloorSaturating(numerator, fromRateHz);
    return true;
}

bool TryMaterializeSchedulePosition(harken::ScheduleTimeKindV1 domain,
                                    std::int64_t authoredPosition,
                                    std::int64_t microsPerQuarter,
                                    std::uint64_t deviceRateHz,
                                    std::int64_t* outFrame) noexcept {
    if (outFrame == nullptr) return false;
    harken::ClockKernel kernel;
    harken::SegmentTimeType timeType{};
    harken::TempoSegment tempo{};
    if (!BuildScheduleKernel(domain, microsPerQuarter, deviceRateHz, kernel,
                             timeType, tempo)) {
        return false;
    }
    const std::int64_t frame =
        timeType == harken::SegmentTimeType::Musical
            ? kernel.TickToFrame(harken::MusicalTick{authoredPosition}).value
            : kernel.TimeToFrame(harken::AbsoluteTime{authoredPosition}).value;
    if (timeType == harken::SegmentTimeType::Musical &&
        kernel.FrameToTick(harken::SampleFrame{frame}).value !=
            authoredPosition) {
        return false;
    }
    *outFrame = frame;
    return true;
}

bool TryMaterializeScheduleSpan(harken::ScheduleTimeKindV1 domain,
                                std::int64_t authoredStart,
                                std::int64_t authoredLength,
                                std::int64_t microsPerQuarter,
                                std::uint64_t deviceRateHz,
                                std::uint64_t* outFrames) noexcept {
    if (outFrames == nullptr || authoredLength < 0) return false;
    harken::ClockKernel kernel;
    harken::SegmentTimeType timeType{};
    harken::TempoSegment tempo{};
    if (!BuildScheduleKernel(domain, microsPerQuarter, deviceRateHz, kernel,
                             timeType, tempo)) {
        return false;
    }
    return timeType == harken::SegmentTimeType::Musical
               ? kernel.TryTickSpanToFrames(
                     harken::MusicalTick{authoredStart}, authoredLength,
                     outFrames)
               : kernel.TryTimeSpanToFrames(
                     harken::AbsoluteTime{authoredStart}, authoredLength,
                     outFrames);
}

bool TryLowerScheduleFades(harken::ScheduleTimeKindV1 domain,
                           std::int64_t authoredStart,
                           std::int64_t authoredFadeIn,
                           std::int64_t authoredFadeOut,
                           std::int64_t microsPerQuarter,
                           std::uint64_t deviceRateHz,
                           std::uint64_t audibleSpanFrames,
                           std::uint64_t* outFadeInFrames,
                           std::uint64_t* outFadeOutFrames) noexcept {
    if (outFadeInFrames == nullptr || outFadeOutFrames == nullptr ||
        authoredFadeIn < 0 || authoredFadeOut < 0) {
        return false;
    }
    harken::ClockKernel kernel;
    harken::SegmentTimeType timeType{};
    harken::TempoSegment tempo{};
    if (!BuildScheduleKernel(domain, microsPerQuarter, deviceRateHz, kernel,
                             timeType, tempo)) {
        return false;
    }
    return LowerFadeFrames(
        kernel, timeType, authoredStart,
        harkenprojectschema::FadeSpec{authoredFadeIn, authoredFadeOut},
        audibleSpanFrames, outFadeInFrames, outFadeOutFrames);
}

RegistrationResult RegisterSegmentProgram(
    const harkenprojectschema::SegmentProgram& program,
    const RegistrationParams& params, harken::LogicCommitCoordinator& commit) {
    if (params.deviceSampleRateHz < kMinSampleRateHz ||
        params.deviceSampleRateHz > kMaxSampleRateHz) {
        return Refuse(RegistrationRefusal::InvalidDeviceRate);
    }

    const harkenprojectschema::SegmentIR& ir = program.ir();
    const harkenprojectschema::IrSegment* rootSegment =
        FindSegment(ir, harkentest::ToStudio(params.rootSegmentId));
    if (rootSegment == nullptr) {
        return Refuse(RegistrationRefusal::RootSegmentNotFound);
    }

    auto resolved = std::make_unique<ResolvedProgram>();

    // One conversion context: every occurrence of the root segment's tracks lives in
    // the root segment's coordinate system, so one clock and one marker index serve them
    // all. A nested container/segment clip is a start-only placeholder in its parent
    // track and is resolved against this same clock, not expanded into its own.
    resolved->clocks_.emplace_back();
    ResolvedProgram::ClockContext& ctx = resolved->clocks_.back();
    harken::SegmentTimeType timeType = harken::SegmentTimeType::Absolute;
    if (!ToRuntimeTimeType(rootSegment->timeSpec.kind, timeType)) {
        return Refuse(RegistrationRefusal::InvalidProgram);
    }

    if (timeType == harken::SegmentTimeType::Musical) {
        for (const harkenprojectschema::TempoMapEntry& row :
             rootSegment->timeSpec.tempoMap) {
            ctx.segments.push_back(harken::TempoSegment{
                harken::MusicalTick{row.tick}, row.microsPerQuarter});
        }
    } else {
        ctx.segments.push_back(harken::TempoSegment{
            harken::MusicalTick{0}, kPlaceholderMicrosPerQuarter});
    }
    if (!ctx.kernel.Reset(params.deviceSampleRateHz, ctx.segments.data(),
                          ctx.segments.size())) {
        return Refuse(RegistrationRefusal::ClockResetFailed);
    }

    // The marker index carries the segment's meter grid and marker table onto every view,
    // so a boundary realignment upstream has the same grid the frames were solved
    // against. A Musical segment takes its authored meter; an Absolute segment has no
    // musical grid, and the meter is unused there.
    harken::MeterSignature meter;
    if (timeType == harken::SegmentTimeType::Musical &&
        !rootSegment->timeSpec.meterMap.empty()) {
        meter.numerator =
            static_cast<std::int32_t>(rootSegment->timeSpec.meterMap[0].numerator);
        meter.denominator = static_cast<std::int32_t>(
            rootSegment->timeSpec.meterMap[0].denominator);
    }
    std::vector<harken::MarkerPoint> markerPoints;
    markerPoints.reserve(rootSegment->markers.size());
    for (const harkenprojectschema::IrMarker& marker : rootSegment->markers) {
        harken::MarkerKind kind = harken::MarkerKind::Custom;
        if (!ToRuntimeMarkerKind(marker.kind, kind)) {
            return Refuse(RegistrationRefusal::InvalidProgram);
        }
        markerPoints.push_back(harken::MarkerPoint{
            harkentest::ToRuntime(marker.id), kind, marker.position});
    }
    const std::size_t markerCapacity = std::max<std::size_t>(1, markerPoints.size());
    if (!ctx.markerIndex.Reset(timeType, &ctx.kernel, meter, markerPoints.data(),
                            markerPoints.size(), markerCapacity)) {
        return Refuse(RegistrationRefusal::MarkerIndexResetFailed);
    }
    ctx.hasMarkerIndex = true;

    // The sound mapping is checked here, ahead of the commit below, even though
    // the loop that resolves it runs later: issuing a sequence is observable to
    // the caller's coordinator and is never reissued, so a registration that
    // ends in a refusal must not consume one. This pass looks only, and the
    // resolution in the main loop refuses again on the same condition rather
    // than relying on this one having run.
    for (auto trackId : rootSegment->tracks) {
        const harkenprojectschema::IrTrack* track = FindTrack(ir, trackId);
        if (track == nullptr) {
            continue;
        }
        for (const harkenprojectschema::IrOccurrence& occ : track->occurrences) {
            if (occ.fades.inLength < 0 || occ.fades.outLength < 0) {
                return Refuse(RegistrationRefusal::InvalidFadeSpec);
            }
            if (occ.content.kind == harkenprojectschema::ContentKind::Sound &&
                params.soundIdMap.find(harkentest::ToRuntime(occ.content.target)) ==
                    params.soundIdMap.end()) {
                return Refuse(RegistrationRefusal::SoundTargetUnmapped);
            }
        }
    }

    // One authored load is one commit: every event of this registration carries the
    // same sequence, and the same-frame tiebreak falls to the ordering keys below.
    const harken::LogicCommitSequence sequence = commit.Issue();
    resolved->commitSequence_ = sequence;

    // A per-registration transaction ordinal, unique across tracks, so two
    // occurrences that land on one frame with equal event index still sort in a
    // fixed order rather than by pool insertion (which cadence could vary).
    std::uint32_t transactionOrdinal = 0;
    std::vector<RegistrationNote> notes;
    std::size_t occurrenceCapacity = 0;
    for (auto trackId : rootSegment->tracks) {
        const harkenprojectschema::IrTrack* track = FindTrack(ir, trackId);
        if (track != nullptr) {
            occurrenceCapacity += track->occurrences.size();
        }
    }
    if (occurrenceCapacity >
        resolved->timelinePayloads_.max_size() / 4u) {
        return Refuse(RegistrationRefusal::InvalidProgram);
    }
    resolved->timelinePayloads_.reserve(occurrenceCapacity * 4u);

    for (auto trackId : rootSegment->tracks) {
        const harkenprojectschema::IrTrack* track = FindTrack(ir, trackId);
        if (track == nullptr) {
            continue;
        }
        for (const harkenprojectschema::IrOccurrence& occ : track->occurrences) {
            // Built once and used by every note and by the resolved occurrence
            // itself, so a note and the occurrence it is about can never disagree
            // about which authored clip they name.
            const OccurrenceLineage lineage{/*hasSegment=*/true,
                                            harkentest::ToRuntime(rootSegment->segmentId),
                                            harkentest::ToRuntime(trackId),
                                            harkentest::ToRuntime(occ.key.clipId),
                                            occ.key.occurrenceOrdinal,
                                            harkentest::ToRuntime(occ.content.target)};

            const std::int64_t startOffset =
                (timeType == harken::SegmentTimeType::Musical)
                    ? ctx.kernel.TickToFrame(harken::MusicalTick{occ.time}).value
                    : ctx.kernel.TimeToFrame(harken::AbsoluteTime{occ.time}).value;

            std::int64_t stopOffset = startOffset;
            if (occ.hasDuration) {
                const std::uint64_t deviceDuration = RescaleFrameCountFloor(
                    occ.durationFrames, occ.resourceSampleRate,
                    static_cast<std::uint64_t>(params.deviceSampleRateHz));
                const std::uint64_t int64Maximum = static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max());
                const std::int64_t clampedDuration =
                    deviceDuration > int64Maximum
                        ? std::numeric_limits<std::int64_t>::max()
                        : static_cast<std::int64_t>(deviceDuration);
                stopOffset =
                    SaturatingAddNonNegative(startOffset, clampedDuration);
            }

            std::vector<harken::EventTemplate> events;
            events.reserve(occ.frameBatch.events.size());
            for (const harkenprojectschema::TemplateEvent& tev :
                 occ.frameBatch.events) {
                harken::EventTemplate ev;
                ev.frameOffsetFromEntry =
                    (tev.kind == harkenprojectschema::TemplateEventKind::Stop)
                        ? stopOffset
                        : startOffset;
                ev.logicCommitSequence = sequence;
                ev.eventIndex = occ.frameBatch.eventIndex;
                ev.transactionOrdinal = transactionOrdinal;
                ev.subeventPhase = tev.phasePriority;
                ev.payload = transactionOrdinal;
                events.push_back(ev);
            }
            if (events.empty()) {
                return Refuse(RegistrationRefusal::TransactionEventsEmpty);
            }

            // The install description: the folded audible bases and loop count pass
            // through; the merged sends and resolved output bus are mapped from
            // schema bus ids to engine bus ids, gated against the session's
            // resolved slot count, and defaulted when the caller supplied one.
            VoiceStartInstall install;
            install.baseVolumeDb = occ.baseVolumeDb;
            install.outputBusVolumeDb = occ.outputBusVolumeDb;
            install.basePitchRatio = occ.basePitchRatio;
            install.loopCount = occ.loopCount;

            // The subject's sound, resolved here rather than when the voice
            // starts. The session that registered the resident PCM or the stream
            // is the only party that knows the engine id, so resolving it now
            // turns an unregistered subject into a named refusal with a lineage
            // instead of a refused play with nothing to attribute it to. A clip
            // targeting a Container or a Segment plays nothing itself -- its
            // content is expanded when that content is triggered -- so it
            // resolves no sound and is left saying it starts nothing audible.
            if (occ.content.kind == harkenprojectschema::ContentKind::Sound) {
                const auto soundIt = params.soundIdMap.find(harkentest::ToRuntime(occ.content.target));
                if (soundIt == params.soundIdMap.end()) {
                    return Refuse(RegistrationRefusal::SoundTargetUnmapped);
                }
                install.startsAudibly = true;
                install.soundId = soundIt->second;
            }

            if (occ.sends.size() > params.resolvedVoiceSendSlots) {
                return Refuse(
                    RegistrationRefusal::SendCountExceedsResolvedSlots);
            }
            install.sends.reserve(occ.sends.size());
            for (const harkenprojectschema::Send& send : occ.sends) {
                const auto busIt = params.busIdMap.find(harkentest::ToRuntime(send.targetBusId));
                if (busIt == params.busIdMap.end()) {
                    return Refuse(RegistrationRefusal::BusIdUnmapped);
                }
                install.sends.push_back(
                    VoiceSend{busIt->second, send.sendLevel});
            }

            if (occ.outputBusResolved) {
                const auto busIt = params.busIdMap.find(harkentest::ToRuntime(occ.resolvedOutputBus));
                if (busIt == params.busIdMap.end()) {
                    return Refuse(RegistrationRefusal::BusIdUnmapped);
                }
                install.outputBus = busIt->second;
                install.outputBusResolved = true;
                install.outputBusDefaulted = false;
            } else if (params.callerDefaultOutputBus != VnObjectId{}) {
                install.outputBus = params.callerDefaultOutputBus;
                install.outputBusResolved = true;
                install.outputBusDefaulted = true;
            }

            install.panning = occ.panning;
            install.spread = occ.spread;
            install.is3DEnabled = occ.is3DEnabled;
            install.attenuationDistanceScaling = occ.attenuationDistanceScaling;
            if (occ.attenuationResolved) {
                const auto attenuationIt =
                    params.attenuationIdMap.find(harkentest::ToRuntime(occ.resolvedAttenuation));
                if (attenuationIt == params.attenuationIdMap.end()) {
                    return Refuse(
                        RegistrationRefusal::AttenuationTargetUnmapped);
                }
                install.attenuationResolved = true;
                install.resolvedAttenuation = attenuationIt->second;
            } else {
                notes.push_back(RegistrationNote{
                    RegistrationNoteCode::AttenuationUnresolvedNoBinding, lineage,
                    {}});
            }

            // The selected effect chain, admitted against this session and lowered
            // onto voice slots. An occurrence that selected nothing installs
            // nothing; one that selected a declared-empty chain also installs
            // nothing, and the note records that the emptiness was authored rather
            // than merely absent.
            if (occ.hasEffectChain) {
                if (occ.effectChainIndex >= ir.effectChains().size()) {
                    return Refuse(
                        RegistrationRefusal::EffectChainReferenceInvalid);
                }
                const harkenprojectschema::CompiledEffectChain& selected =
                    ir.effectChains()[occ.effectChainIndex];
                RegistrationRefusal chainRefusal = RegistrationRefusal::None;
                RegistrationRefusalContext chainContext;
                if (!AdmitAndLowerChain(selected, params, &install.effectSlots,
                                        &chainRefusal, &chainContext)) {
                    return Refuse(chainRefusal, chainContext);
                }
                if (selected.entries.empty()) {
                    notes.push_back(RegistrationNote{
                        RegistrationNoteCode::EffectChainSelectedEmpty, lineage,
                        harkentest::ToRuntime(selected.owner)});
                }
            }

            std::vector<harken::EventTemplate> timelineEvents;
            harken::TimelineSemanticTarget timelineTarget{};
            if (install.startsAudibly && stopOffset > startOffset) {
                const std::uint64_t span =
                    static_cast<std::uint64_t>(stopOffset - startOffset);
                std::uint64_t fadeInFrames = 0;
                std::uint64_t fadeOutFrames = 0;
                if (!LowerFadeFrames(ctx.kernel, timeType, occ.time, occ.fades,
                                     span, &fadeInFrames, &fadeOutFrames)) {
                    return Refuse(RegistrationRefusal::InvalidFadeSpec);
                }
                timelineTarget.programSemanticFingerprint =
                    program.programSemanticFingerprint();
                timelineTarget.clipGuidHigh = occ.clipGuid.high;
                timelineTarget.clipGuidLow = occ.clipGuid.low;
                timelineTarget.subjectGuidHigh = occ.contentTargetGuid.high;
                timelineTarget.subjectGuidLow = occ.contentTargetGuid.low;
                timelineTarget.trackId = harkentest::ToRuntime(trackId);
                timelineTarget.clipId = harkentest::ToRuntime(occ.key.clipId);
                timelineTarget.subjectId = harkentest::ToRuntime(occ.content.target);
                timelineTarget.occurrenceOrdinal = occ.key.occurrenceOrdinal;
                auto appendTimeline = [&](harken::TimelineEventPayload payload,
                                          std::int64_t frame,
                                          std::uint32_t phase) {
                    const std::uint64_t payloadIndex =
                        resolved->timelinePayloads_.size();
                    payload.resolvedOccurrenceIndex = transactionOrdinal;
                    payload.target = timelineTarget;
                    resolved->timelinePayloads_.push_back(payload);
                    harken::EventTemplate event{};
                    event.frameOffsetFromEntry = frame;
                    event.logicCommitSequence = sequence;
                    event.eventIndex = occ.frameBatch.eventIndex;
                    event.transactionOrdinal = transactionOrdinal * 3u;
                    event.subeventPhase = phase;
                    event.payload = payloadIndex;
                    event.authoredFadeOutCandidate =
                        payload.authoredFadeOutCandidate;
                    timelineEvents.push_back(event);
                };
                harken::TimelineEventPayload start{};
                start.kind = harken::TimelineEventKind::Start;
                appendTimeline(start, startOffset,
                               harkenprojectschema::kPhasePriorityStart);
                if (fadeInFrames >= 2) {
                    harken::TimelineEventPayload envelope{};
                    envelope.kind = harken::TimelineEventKind::Envelope;
                    envelope.envelopeDirection =
                        harken::TimelineEnvelopeDirection::FadeIn;
                    envelope.envelopeStartFrame = harken::AnchoredFrame(
                        params.entryAnchorFrame, startOffset);
                    envelope.envelopeEndFrame =
                        envelope.envelopeStartFrame + fadeInFrames;
                    appendTimeline(envelope, startOffset,
                                   harkenprojectschema::kPhasePriorityEnvelope);
                }
                if (fadeOutFrames >= 2) {
                    harken::TimelineEventPayload envelope{};
                    envelope.kind = harken::TimelineEventKind::Envelope;
                    envelope.envelopeDirection =
                        harken::TimelineEnvelopeDirection::FadeOut;
                    envelope.envelopeEndFrame = harken::AnchoredFrame(
                        params.entryAnchorFrame, stopOffset);
                    envelope.envelopeStartFrame =
                        envelope.envelopeEndFrame - fadeOutFrames;
                    envelope.authoredFadeOutCandidate = true;
                    appendTimeline(
                        envelope,
                        stopOffset - static_cast<std::int64_t>(fadeOutFrames),
                        harkenprojectschema::kPhasePriorityEnvelope);
                }
                harken::TimelineEventPayload stop{};
                stop.kind = harken::TimelineEventKind::Stop;
                stop.authoredFadeOutCandidate = fadeOutFrames >= 2;
                appendTimeline(stop, stopOffset,
                               harkenprojectschema::kPhasePriorityStop);
            }

            ResolvedOccurrence entry;
            entry.lineage = lineage;
            entry.install = std::move(install);
            entry.events = std::move(events);
            entry.timelineEvents = std::move(timelineEvents);
            entry.timelineTarget = timelineTarget;
            entry.entryAnchorFrame = params.entryAnchorFrame;
            entry.transactionId =
                static_cast<harken::TransactionId>(transactionOrdinal) + 1u;
            entry.timeType = timeType;
            entry.clockIndex = 0;
            entry.hasClock = true;
            resolved->occurrences_.push_back(std::move(entry));
            ++transactionOrdinal;
        }
    }

    // Build the views only after every occurrence is in place, so each borrows
    // stable storage. Reserve exactly, so the vector the planner points into never
    // reallocates.
    resolved->views_.reserve(resolved->occurrences_.size());
    for (const ResolvedOccurrence& occ : resolved->occurrences_) {
        const ResolvedProgram::ClockContext& clock =
            resolved->clocks_[occ.clockIndex];
        harken::ProgramView view;
        view.ownerInstance = params.ownerInstance;
        view.ownerInstanceScheduleEpoch = params.ownerInstanceScheduleEpoch;
        view.transactionId = occ.transactionId;
        view.entryAnchorFrame = occ.entryAnchorFrame;
        view.decisionLeadFrames = 0;
        view.events = occ.events.data();
        view.eventCount = occ.events.size();
        view.timeType = occ.timeType;
        view.clock = occ.hasClock ? &clock.kernel : nullptr;
        view.markerIndex =
            (occ.hasClock && clock.hasMarkerIndex) ? &clock.markerIndex : nullptr;
        view.boundaryType = harken::BoundaryType::Bar;
        resolved->views_.push_back(view);
    }

    resolved->timelineViews_.reserve(resolved->occurrences_.size());
    resolved->timelineOccurrenceToResolved_.reserve(
        resolved->occurrences_.size());
    for (std::size_t resolvedIndex = 0;
         resolvedIndex < resolved->occurrences_.size(); ++resolvedIndex) {
        ResolvedOccurrence& occ = resolved->occurrences_[resolvedIndex];
        if (occ.timelineEvents.empty()) {
            std::uint64_t logicalCompleteFrame = occ.entryAnchorFrame;
            for (const harken::EventTemplate& event : occ.events) {
                logicalCompleteFrame = std::max(
                    logicalCompleteFrame,
                    harken::AnchoredFrame(occ.entryAnchorFrame,
                                         event.frameOffsetFromEntry));
            }
            resolved->logicalOnlyOccurrences_.push_back(
                {static_cast<std::uint32_t>(resolvedIndex),
                 logicalCompleteFrame});
            continue;
        }
        const ResolvedProgram::ClockContext& clock =
            resolved->clocks_[occ.clockIndex];
        harken::ProgramView view{};
        view.transactionId = occ.transactionId;
        view.entryAnchorFrame = occ.entryAnchorFrame;
        view.events = occ.timelineEvents.data();
        view.eventCount = occ.timelineEvents.size();
        view.timeType = occ.timeType;
        view.rollingGroups = true;
        view.clock = occ.hasClock ? &clock.kernel : nullptr;
        view.markerIndex =
            (occ.hasClock && clock.hasMarkerIndex) ? &clock.markerIndex : nullptr;
        resolved->timelineViews_.push_back(view);
        resolved->timelineOccurrenceToResolved_.push_back(resolvedIndex);
    }
    resolved->timelineProgram_.generation = 1;
    resolved->timelineProgram_.payloads = resolved->timelinePayloads_.data();
    resolved->timelineProgram_.payloadCount =
        static_cast<std::uint32_t>(resolved->timelinePayloads_.size());

    // The session's per-type pool capacities travel on the program: a consumer
    // accounting for this program's effect demand runs after this call returns,
    // when the params no longer exist. Copied whole rather than filtered to the
    // demanded types, so the copy states what the session registered rather than
    // what this particular program happened to use.
    for (const auto& [effectType, availability] : params.effectTypes) {
        resolved->effectTypeCapacities_.emplace(effectType,
                                                availability.poolCapacity);
    }

    RegistrationResult result;
    result.ok = true;
    result.refusal = RegistrationRefusal::None;
    result.program = std::move(resolved);
    result.notes = std::move(notes);
    return result;
}

} // namespace harkenprogramreg
