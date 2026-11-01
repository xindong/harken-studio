#include "harken/media/PcmRegistry.h"

#include "harken/core/MediaContentRevision.h"

#include <cstdint>
#include <cassert>
#include <cstring>
#include <new>
#include <utility>
#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define HARKEN_PCM_HAS_SSE2 1
#else
#define HARKEN_PCM_HAS_SSE2 0
#endif

namespace harken {

namespace detail {

void PcmSampleDeleter::operator()(void* p) noexcept {
    if (pages != nullptr) {
        assert(p == pages->Address());
        pages.reset();
    } else if (format == PcmSampleFormat::ExactInt16) {
        delete[] static_cast<std::int16_t*>(p);
    } else {
        delete[] static_cast<float*>(p);
    }
}

} // namespace detail

namespace {

PcmRegisterFault g_pcmRegisterFault = PcmRegisterFault::None;

std::size_t PackSampleReservationBytes(std::size_t bytes) noexcept {
#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto granularity = static_cast<std::size_t>(info.dwAllocationGranularity);
    if (granularity != 0) {
        const auto granules = bytes / granularity + (bytes % granularity != 0);
        // Spread equally sized planar sources across virtual-page index patterns.
        // The extra granule stays uncommitted and does not extend sample capacity.
        if (granules % 2 == 0 && granules < SIZE_MAX / granularity)
            return (granules + 1) * granularity;
    }
#endif
    return bytes;
}

VnResult ValidateDescShape(const VnPCMDesc* desc) {
    if (desc == nullptr || desc->structSize != sizeof(VnPCMDesc)) {
        return VN_INVALID_ARG;
    }
    if (desc->sampleRate == 0 || desc->frames == 0 ||
        desc->channels == 0 || desc->channels > kMaxChannels) {
        return VN_INVALID_ARG;
    }
    // Sizes whose float count or byte count would not fit size_t are
    // rejected outright: a wrapped multiplication would allocate backing
    // smaller than the view it must support, and every later read past the
    // wrapped size would be out of bounds.
    if (desc->frames > SIZE_MAX / desc->channels) {
        return VN_INVALID_ARG;
    }
    if (desc->frames * desc->channels > SIZE_MAX / sizeof(float)) {
        return VN_INVALID_ARG;
    }
    if (desc->layout != VN_PCM_LAYOUT_INTERLEAVED &&
        desc->layout != VN_PCM_LAYOUT_PLANAR) {
        return VN_INVALID_ARG;
    }
    if (desc->policy != VN_PCM_POLICY_COPY &&
        desc->policy != VN_PCM_POLICY_REFERENCE) {
        return VN_INVALID_ARG;
    }
    return VN_OK;
}

VnResult ValidateDesc(const VnPCMDesc* desc) {
    const VnResult valid = ValidateDescShape(desc);
    if (valid != VN_OK) return valid;
    if (desc->layout == VN_PCM_LAYOUT_INTERLEAVED) {
        if (desc->samplesInterleaved == nullptr) {
            return VN_INVALID_ARG;
        }
    } else {
        if (desc->samplesPlanar == nullptr) {
            return VN_INVALID_ARG;
        }
        for (std::uint32_t ch = 0; ch < desc->channels; ++ch) {
            if (desc->samplesPlanar[ch] == nullptr) {
                return VN_INVALID_ARG;
            }
        }
    }
    return VN_OK;
}

bool ComputeContentRevision(const VnPCMDesc& desc,
                            std::uint64_t* outRevision, PcmLoopRegion loop = {}) {
    const float* channels[kMaxChannels] = {};
    std::uint32_t stride = 1;
    if (desc.layout == VN_PCM_LAYOUT_INTERLEAVED) {
        stride = desc.channels;
        for (std::uint32_t channel = 0; channel < desc.channels; ++channel) {
            channels[channel] = desc.samplesInterleaved + channel;
        }
    } else {
        for (std::uint32_t channel = 0; channel < desc.channels; ++channel) {
            channels[channel] = desc.samplesPlanar[channel];
        }
    }
    return ComputeMediaPcmContentRevisionV1(
        desc.sampleRate, desc.channels, desc.frames, channels, stride,
        outRevision, loop.startFrame, loop.endFrame);
}

bool IsExactInt16Span(const float* samples, std::size_t count) noexcept {
    std::size_t i = 0;
#if HARKEN_PCM_HAS_SSE2
    for (; count - i >= 4; i += 4) {
        const __m128 values = _mm_loadu_ps(samples + i);
        const __m128 inRange = _mm_and_ps(_mm_cmpge_ps(values, _mm_set1_ps(-1.0f)),
                                        _mm_cmplt_ps(values, _mm_set1_ps(1.0f)));
        // Reject out-of-range values before any integer conversion.
        if (_mm_movemask_ps(inRange) != 15) return false;
        const __m128i compact = _mm_cvttps_epi32(_mm_mul_ps(values, _mm_set1_ps(0x1p15f)));
        const __m128 restored = _mm_mul_ps(_mm_cvtepi32_ps(compact), _mm_set1_ps(0x1p-15f));
        const __m128i sameBits = _mm_cmpeq_epi32(_mm_castps_si128(values),
                                               _mm_castps_si128(restored));
        if (_mm_movemask_epi8(sameBits) != 0xffff) return false;
    }
#endif
    for (; i < count; ++i) {
        const float sample = samples[i];
        if (!(sample >= -1.0f && sample < 1.0f)) return false;
        const auto compact = static_cast<std::int16_t>(sample * 0x1p15f);
        const float restored = static_cast<float>(compact) * 0x1p-15f;
        // Compare the original bytes even when DAZ/FTZ is enabled. Negative
        // zero and nonzero subnormals must not silently become positive zero.
        if (std::memcmp(samples + i, &restored, sizeof(float)) != 0) return false;
    }
    return true;
}

bool IsExactInt16(const VnPCMDesc& desc, std::size_t count) noexcept {
    if (desc.layout == VN_PCM_LAYOUT_INTERLEAVED)
        return IsExactInt16Span(desc.samplesInterleaved, count);
    for (std::uint32_t ch = 0; ch < desc.channels; ++ch) {
        if (!IsExactInt16Span(desc.samplesPlanar[ch],
                              static_cast<std::size_t>(desc.frames))) return false;
    }
    return true;
}

void CopySampleSpan(float* dst, const float* src, std::size_t count) noexcept {
    std::memcpy(dst, src, count * sizeof(float));
}

void CopySampleSpan(std::int16_t* dst, const float* src,
                    std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        dst[i] = static_cast<std::int16_t>(src[i] * 0x1p15f);
}

template <class Sample>
detail::PcmSampleBuffer CopySamples(const VnPCMDesc& desc, std::size_t count,
                                    PcmSampleFormat format) {
    detail::PcmSampleBuffer stagedSamples;
    stagedSamples.get_deleter().format = format;
    const auto byteCount = count * sizeof(Sample);
    const auto pageBytes = detail::StablePageReservation::SystemPageBytes();
    // Bound committed tail padding below 1/16 of each large payload.
    if (pageBytes != 0 && pageBytes % alignof(Sample) == 0 &&
        byteCount / pageBytes >= 16) {
        auto& pages = stagedSamples.get_deleter().pages;
        pages = std::make_unique<detail::StablePageReservation>();
        if (!pages->Reserve(byteCount, alignof(Sample)) ||
            !pages->CommitThrough(byteCount)) {
            throw std::bad_alloc{};
        }
        // Start the typed array lifetime without initializing sample values.
        stagedSamples.reset(::new (pages->Address()) Sample[count]);
    } else {
        stagedSamples.reset(new Sample[count]);
    }
    auto* dst = static_cast<Sample*>(stagedSamples.get());
    if (desc.layout == VN_PCM_LAYOUT_INTERLEAVED) {
        CopySampleSpan(dst, desc.samplesInterleaved, count);
    } else {
        for (std::uint32_t channel = 0; channel < desc.channels; ++channel) {
            CopySampleSpan(dst + static_cast<std::size_t>(channel) * desc.frames,
                           desc.samplesPlanar[channel],
                           static_cast<std::size_t>(desc.frames));
        }
    }
    return stagedSamples;
}

detail::PcmSampleBuffer StageSamples(const VnPCMDesc& desc) {
    if (desc.policy != VN_PCM_POLICY_COPY) return {};
    if (PcmRegisterFaultForTests() == PcmRegisterFault::SampleBacking)
        throw std::bad_alloc{};
    const auto count = static_cast<std::size_t>(desc.frames) * desc.channels;
    const auto pageBytes = detail::StablePageReservation::SystemPageBytes();
    // Keep compact payloads on the existing page-backed allocation path too.
    // Smaller copies retain their original storage and allocation behavior.
    if (pageBytes != 0 && pageBytes % alignof(float) == 0 &&
        count * sizeof(std::int16_t) / pageBytes >= 16 && IsExactInt16(desc, count))
        return CopySamples<std::int16_t>(desc, count, PcmSampleFormat::ExactInt16);
    return CopySamples<float>(desc, count, PcmSampleFormat::Float32);
}

void SetSamplePointers(PCMView& view, const VnPCMDesc& desc,
                       const detail::PcmSampleBuffer& owned) noexcept {
    view.stride = desc.layout == VN_PCM_LAYOUT_PLANAR ? 1 : desc.channels;
    view.sampleFormat = owned != nullptr
        ? owned.get_deleter().format : PcmSampleFormat::Float32;
    for (std::uint32_t ch = 0; ch < desc.channels; ++ch) {
        if (owned != nullptr) {
            const auto offset = desc.layout == VN_PCM_LAYOUT_PLANAR
                ? static_cast<std::size_t>(ch) * desc.frames : ch;
            if (view.sampleFormat == PcmSampleFormat::ExactInt16)
                view.channels[ch] = static_cast<const std::int16_t*>(owned.get()) + offset;
            else
                view.channels[ch] = static_cast<const float*>(owned.get()) + offset;
        } else {
            view.channels[ch] = desc.layout == VN_PCM_LAYOUT_PLANAR
                ? desc.samplesPlanar[ch] : desc.samplesInterleaved + ch;
        }
    }
}

} // namespace

void SetPcmRegisterFaultForTests(PcmRegisterFault fault) {
    g_pcmRegisterFault = fault;
}

PcmRegisterFault PcmRegisterFaultForTests() { return g_pcmRegisterFault; }

std::uint64_t PCMRegistry::PreparedPackMutation::CookieOf(
    VnSoundId soundId) const noexcept {
    const auto it = m_staged.find(soundId);
    return it != m_staged.end() ? it->second->cookie : 0;
}

PCMRegistry::PackReference PCMRegistry::PreparedPackMutation::ReferenceOf(
    VnSoundId soundId) const noexcept {
    PackReference result;
    const auto it = m_staged.find(soundId);
    if (it != m_staged.end()) {
        result.entry_ = it->second;
    }
    return result;
}

VnResult PCMRegistry::RegisterPCM(VnSoundId soundId, const VnPCMDesc* desc,
                                  std::uint64_t* outCookie, PcmLoopRegion loop) {
    const VnResult validation = ValidateDesc(desc);
    if (validation != VN_OK) {
        return validation;
    }
    std::uint64_t contentRevision = 0;
    if (!ComputeContentRevision(*desc, &contentRevision, loop)) {
        return VN_INVALID_ARG;
    }
    if (m_entries.find(soundId) != m_entries.end()) {
        // The id is occupied — a state conflict, same family as unregister
        // while playing. Re-registering under a live id is refused; the
        // caller unregisters first.
        return VN_IN_USE;
    }

    // Complete the fallible sample copy before committing the registry entry.
    auto stagedSamples = StageSamples(*desc);
    return CommitPCM(soundId, desc, std::move(stagedSamples),
                     contentRevision, outCookie, loop);
}

VnResult PCMRegistry::CommitPCM(VnSoundId soundId, const VnPCMDesc* desc,
                                detail::PcmSampleBuffer&& stagedSamples,
                                std::uint64_t contentRevision,
                                std::uint64_t* outCookie, PcmLoopRegion loop) {
    if (PcmRegisterFaultForTests() == PcmRegisterFault::MapCommit) {
        throw std::bad_alloc{};
    }
    auto [it, inserted] = m_entries.try_emplace(soundId, std::make_shared<Entry>());
    if (!inserted) {
        // The id occupied the map between the lookup above and this call —
        // defensive only; the registry has no concurrent registration path.
        // Never overwrite an entry already resolving voices.
        return VN_IN_USE;
    }
    Entry& entry = *it->second;

    // Past this point every operation is noexcept: pointer ownership transfer
    // and scalar/pointer fills. No further failure can leave the map in a
    // half-registered state.
    entry.ownedSamples = std::move(stagedSamples);

    // The provenance stamp is unique across the registry's whole life, so
    // an unregister/re-register under the same id is always observable to
    // a cookie-holding owner. 0 is reserved as the no-registration
    // sentinel, so the counter skips it on wraparound.
    if (m_nextCookie == 0) {
        m_nextCookie = 1;
    }
    entry.cookie = m_nextCookie++;

    PCMView& view = entry.view;
    view.channelCount = desc->channels;
    view.sampleRate = desc->sampleRate;
    view.frames = desc->frames;
    view.contentRevision = contentRevision;
    view.loop = loop;
    view.useCount = &entry.uses;

    SetSamplePointers(view, *desc, entry.ownedSamples);

    if (outCookie != nullptr) {
        *outCookie = entry.cookie;
    }

    return VN_OK;
}

VnResult PCMRegistry::UnregisterPCM(VnSoundId soundId) {
    auto it = m_entries.find(soundId);
    if (it == m_entries.end()) {
        return VN_UNKNOWN_ID;
    }
    if (it->second->uses.load(std::memory_order_acquire) != 0) {
        return VN_IN_USE;
    }
    m_entries.erase(it);
    return VN_OK;
}

VnResult PCMRegistry::ReplacePCM(VnSoundId soundId, const VnPCMDesc* desc) {
    PreparedReplaceToken prepared;
    const VnResult result = PrepareReplacePCM(soundId, desc, &prepared);
    return result == VN_OK ? CommitPreparedReplacePCM(&prepared) : result;
}

VnResult PCMRegistry::PrepareReplacePCM(VnSoundId soundId,
                                        const VnPCMDesc* desc,
                                        PreparedReplaceToken* out,
                                        PcmLoopRegion loop) const {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = PreparedReplaceToken{};
    const VnResult validation = ValidateDesc(desc);
    if (validation != VN_OK) {
        return validation;
    }
    std::uint64_t contentRevision = 0;
    if (!ComputeContentRevision(*desc, &contentRevision, loop)) {
        return VN_INVALID_ARG;
    }
    auto it = m_entries.find(soundId);
    if (it == m_entries.end()) {
        return VN_UNKNOWN_ID; // replace never implicitly registers
    }
    // Structural generation bound: one retiring generation per asset. The
    // caller sweeps drained generations before calling, so this refusal
    // means voices are genuinely still draining the previous old content.
    if (HasRetired(soundId)) {
        return VN_IN_USE;
    }
    out->m_soundId = soundId;
    out->m_desc = desc;
    out->m_contentRevision = contentRevision;
    out->m_loop = loop;
    out->m_generation = &it->second->uses;
    return VN_OK;
}

VnResult PCMRegistry::CommitPreparedReplacePCM(
    PreparedReplaceToken* token) {
    if (token == nullptr || !token->IsPrepared()) {
        return VN_INVALID_ARG;
    }
    auto it = m_entries.find(token->m_soundId);
    if (it == m_entries.end() || &it->second->uses != token->m_generation ||
        HasRetired(token->m_soundId)) {
        return VN_IN_USE;
    }
    const VnSoundId soundId = token->m_soundId;
    const VnPCMDesc* desc = token->m_desc;
    const std::uint64_t contentRevision = token->m_contentRevision;
    auto stagedSamples = StageSamples(*desc);
    // Every allocation that could fail is taken BEFORE the swap becomes
    // irreversible. The retired-list slot is reserved first: a bad_alloc
    // here propagates with the registry untouched (the registration
    // surface's posture), and the push at the end then cannot throw while
    // `old` is the only owner of memory draining voices still read.
    m_retired.reserve(m_retired.size() + 1);
    // Extract the old registration whole — the node keeps its address, so
    // every view and use-count pointer a consuming voice captured stays
    // valid — then register the new content under the now-vacant id through
    // the ordinary path (fresh provenance cookie included). The insert
    // cannot be refused: the descriptor validated above and the id is
    // vacant. Map growth CAN throw, so the
    // unwind restores the extracted node: erase any partial insert (a
    // u64-key erase never throws), then reinsert the node — the table held
    // this element before the extract and buckets never shrink, so the
    // reinsert triggers no rehash and no allocation. Old content stays
    // fully in force on every failure path.
    RetiredGeneration old;
    old.m_node = m_entries.extract(it);
    VnResult registered = VN_OK;
    try {
        registered = CommitPCM(soundId, desc, std::move(stagedSamples),
                               contentRevision, nullptr, token->m_loop);
    } catch (...) {
        m_entries.erase(soundId);
        m_entries.insert(std::move(old.m_node));
        throw;
    }
    if (registered != VN_OK) {
        m_entries.insert(std::move(old.m_node)); // defensive: restore intact
        return registered;
    }
    if (!old.Drained()) {
        m_retired.push_back(std::move(old)); // nothrow: reserved above
    }
    *token = PreparedReplaceToken{};
    // A drained old generation destroys here, on the logic thread, as `old`
    // goes out of scope — no pin, no snapshot presence.
    return VN_OK;
}

bool PCMRegistry::ExtractRetiring(VnSoundId soundId,
                                  std::uint64_t expectedCookie,
                                  RetiredGeneration* out) {
    auto it = m_entries.find(soundId);
    if (it == m_entries.end() || it->second->cookie != expectedCookie) {
        return false;
    }
    out->m_node = m_entries.extract(it);
    return true;
}

VnResult PCMRegistry::PreparePackSamples(
    VnSoundId soundId, const VnPCMDesc* desc, PreparedPackSamples* out,
    PcmLoopRegion loop) {
    if (out == nullptr) return VN_INVALID_ARG;
    if (out->IsPrepared()) return VN_IN_USE;
    const VnResult valid = ValidateDesc(desc);
    if (valid != VN_OK || desc->policy != VN_PCM_POLICY_COPY) return VN_INVALID_ARG;
    PackSampleBuilder builder;
    auto result = BeginPackSamples(soundId, desc, &builder, loop);
    if (result != VN_OK) return result;
    if (desc->layout == VN_PCM_LAYOUT_INTERLEAVED) {
        result = builder.Append(desc->samplesInterleaved,
                                static_cast<std::size_t>(desc->frames) * desc->channels);
    } else {
        for (std::uint32_t ch = 0; ch < desc->channels && result == VN_OK; ++ch)
            result = builder.Append(desc->samplesPlanar[ch], static_cast<std::size_t>(desc->frames));
    }
    return result == VN_OK ? builder.Complete(out) : result;
}

VnResult PCMRegistry::BeginPackSamples(
    VnSoundId soundId, const VnPCMDesc* shape, PackSampleBuilder* out,
    PcmLoopRegion loop, const PreparedPackSamples* equalCandidate) {
    if (out == nullptr || out->m_entry != nullptr) return VN_INVALID_ARG;
    const VnResult valid = ValidateDescShape(shape);
    if (valid != VN_OK || shape->policy != VN_PCM_POLICY_COPY ||
        ((loop.startFrame != 0 || loop.endFrame != 0) &&
         (loop.startFrame >= loop.endFrame || loop.endFrame > shape->frames)))
        return VN_INVALID_ARG;
    PackSampleBuilder builder;
    auto entry = std::make_shared<Entry>();
    entry->view.channelCount = shape->channels;
    entry->view.sampleRate = shape->sampleRate;
    entry->view.frames = shape->frames;
    entry->view.stride = shape->layout == VN_PCM_LAYOUT_PLANAR ? 1 : shape->channels;
    entry->view.loop = loop;
    entry->view.useCount = &entry->uses;
    builder.m_soundId = soundId;
    builder.m_count = static_cast<std::size_t>(shape->frames) * shape->channels;
    builder.m_entry = std::move(entry);
    if (equalCandidate != nullptr && equalCandidate->IsPrepared() &&
        equalCandidate->m_soundId == soundId && equalCandidate->m_entry->cookie == 0) {
        const auto& candidate = equalCandidate->m_entry->view;
        const auto& target = builder.m_entry->view;
        if (candidate.sampleRate == target.sampleRate &&
            candidate.channelCount == target.channelCount &&
            candidate.frames == target.frames && candidate.stride == target.stride &&
            candidate.loop.startFrame == loop.startFrame && candidate.loop.endFrame == loop.endFrame)
            builder.m_equalCandidate = equalCandidate->m_entry;
    }
    if (builder.m_equalCandidate == nullptr) {
        const auto result = builder.PrepareStorage();
        if (result != VN_OK) return result;
    }
    *out = std::move(builder);
    return VN_OK;
}

VnResult PCMRegistry::PackSampleBuilder::PrepareStorage() {
    m_status = VN_BACKPRESSURE;
    if (PcmRegisterFaultForTests() == PcmRegisterFault::SampleBacking)
        throw std::bad_alloc{};
    auto& storage = m_entry->ownedSamples;
    const auto pageBytes = detail::StablePageReservation::SystemPageBytes();
    if (pageBytes != 0 && pageBytes % alignof(float) == 0 &&
        m_count * sizeof(std::int16_t) / pageBytes >= 16) {
        auto& pages = storage.get_deleter().pages;
        pages = std::make_unique<detail::StablePageReservation>();
        if (!pages->Reserve(PackSampleReservationBytes(m_count * sizeof(float)), alignof(float))) {
            m_reservationUnavailable = true;
            return m_status = VN_BACKPRESSURE;
        }
        storage.reset(pages->Address());
        m_entry->view.sampleFormat = PcmSampleFormat::ExactInt16;
    } else {
        storage.reset(new float[m_count]);
    }
    return m_status = VN_OK;
}

VnResult PCMRegistry::PackSampleBuilder::Append(const float* samples, std::size_t count) {
    if (m_entry == nullptr) return VN_INVALID_ARG;
    if (m_status != VN_OK) return m_status;
    if (count > m_count - m_written || (count != 0 && samples == nullptr))
        return m_status = VN_INVALID_ARG;
    if (count == 0) return VN_OK;
    std::size_t finiteIndex = 0;
#if HARKEN_PCM_HAS_SSE2
    const __m128i exponentMask = _mm_set1_epi32(0x7f800000);
    for (; count - finiteIndex >= 4; finiteIndex += 4) {
        const __m128i bits = _mm_castps_si128(_mm_loadu_ps(samples + finiteIndex));
        const __m128i invalid = _mm_cmpeq_epi32(_mm_and_si128(bits, exponentMask), exponentMask);
        if (_mm_movemask_epi8(invalid) != 0) return m_status = VN_INVALID_ARG;
    }
#endif
    for (; finiteIndex < count; ++finiteIndex) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, samples + finiteIndex, sizeof(bits));
        if ((bits & 0x7f800000u) == 0x7f800000u) return m_status = VN_INVALID_ARG;
    }
    if (m_equalCandidate != nullptr) {
        const auto readPhysical = [](const PCMView& view, std::size_t index) {
            const auto channel = static_cast<std::uint32_t>(view.stride == 1
                ? index / view.frames : index % view.channelCount);
            const auto frame = view.stride == 1 ? index % view.frames : index / view.channelCount;
            return view.ReadSample(channel, frame);
        };
        bool equal = true;
        for (std::size_t i = 0; i < count; ++i) {
            const float sample = readPhysical(m_equalCandidate->view, m_written + i);
            if (std::memcmp(&sample, samples + i, sizeof(sample)) != 0) {
                equal = false;
                break;
            }
        }
        if (equal) {
            m_written += count;
            return VN_OK;
        }
        const auto candidate = std::move(m_equalCandidate);
        const auto prefix = m_written;
        m_written = 0;
        const auto prepared = PrepareStorage();
        if (prepared != VN_OK) return prepared;
        float restored[256];
        while (m_written < prefix) {
            const auto remaining = prefix - m_written;
            const auto step = remaining < 256 ? remaining : 256;
            for (std::size_t i = 0; i < step; ++i)
                restored[i] = readPhysical(candidate->view, m_written + i);
            const auto result = Store(restored, step);
            if (result != VN_OK) return result;
        }
    }
    return Store(samples, count);
}

VnResult PCMRegistry::PackSampleBuilder::Store(const float* samples, std::size_t count) {
    auto& storage = m_entry->ownedSamples;
    auto& view = m_entry->view;
    const bool expand = view.sampleFormat == PcmSampleFormat::ExactInt16 &&
                        !IsExactInt16Span(samples, count);
    const bool compact = view.sampleFormat == PcmSampleFormat::ExactInt16 && !expand;
    // This owner must retain the complete final payload. Commit its current
    // representation once, as the ordinary COPY path does, rather than making
    // a system commitment for every bounded input window. Promotion still
    // commits only the additional Float32 pages before expanding the prefix.
    const auto bytes = m_count * (compact ? sizeof(std::int16_t) : sizeof(float));
    if (storage.get_deleter().pages != nullptr &&
        !storage.get_deleter().pages->CommitThrough(bytes))
        return m_status = VN_BACKPRESSURE;
    auto* destination = static_cast<std::uint8_t*>(storage.get());
    if (expand) {
        for (std::size_t i = m_written; i != 0; --i) {
            std::int16_t value = 0;
            std::memcpy(&value, destination + (i - 1) * sizeof(value), sizeof(value));
            const float restored = static_cast<float>(value) * 0x1p-15f;
            std::memcpy(destination + (i - 1) * sizeof(float), &restored, sizeof(restored));
        }
        view.sampleFormat = PcmSampleFormat::Float32;
    }
    if (compact) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = static_cast<std::int16_t>(samples[i] * 0x1p15f);
            std::memcpy(destination + (m_written + i) * sizeof(value), &value, sizeof(value));
        }
    } else {
        std::memcpy(destination + m_written * sizeof(float), samples, count * sizeof(float));
    }
    m_written += count;
    return VN_OK;
}

void PCMRegistry::PackSampleBuilder::PrepareSampleView() noexcept {
    auto& view = m_entry->view;
    auto& storage = m_entry->ownedSamples;
    storage.get_deleter().format = view.sampleFormat;
    if (storage.get_deleter().pages != nullptr) {
        if (view.sampleFormat == PcmSampleFormat::ExactInt16)
            ::new (storage.get()) std::int16_t[m_count];
        else
            ::new (storage.get()) float[m_count];
    }
    VnPCMDesc shape{};
    shape.channels = view.channelCount;
    shape.frames = view.frames;
    shape.layout = view.stride == 1 ? VN_PCM_LAYOUT_PLANAR : VN_PCM_LAYOUT_INTERLEAVED;
    SetSamplePointers(view, shape, storage);
}

void PCMRegistry::PackSampleBuilder::MoveCompleted(PreparedPackSamples* out) noexcept {
    out->m_soundId = m_soundId;
    out->m_entry = std::move(m_entry);
}

VnResult PCMRegistry::PackSampleBuilder::Complete(PreparedPackSamples* out) {
    if (out == nullptr || out->IsPrepared() || m_entry == nullptr || m_written != m_count)
        return VN_INVALID_ARG;
    if (m_status != VN_OK) return m_status;
    if (m_equalCandidate != nullptr) {
        out->m_soundId = m_soundId;
        out->m_entry = std::move(m_equalCandidate);
        m_entry.reset();
        return VN_OK;
    }
    PrepareSampleView();
    auto& view = m_entry->view;
    const MediaPcmContentRevisionInputV1 input{view.sampleRate, view.channelCount,
        view.frames, nullptr, view.stride, view.loop.startFrame, view.loop.endFrame};
    auto hash = detail::BeginMediaPcmRevisionV1(input);
    for (std::uint64_t frame = 0; frame < view.frames; ++frame) {
        for (std::uint32_t ch = 0; ch < view.channelCount; ++ch) {
            const float sample = view.ReadSample(ch, frame);
            std::uint32_t bits = 0;
            if (!detail::ReadMediaPcmRevisionBitsV1(&sample, 0, &bits))
                return m_status = VN_INVALID_ARG;
            hash.AddU32(bits);
        }
    }
    view.contentRevision = detail::FinishMediaPcmRevisionV1(hash, input);
    MoveCompleted(out);
    return VN_OK;
}

bool PCMRegistry::PackSampleBuilder::TryCompletePair(PackSampleBuilder& following,
    PreparedPackSamples* firstOut, PreparedPackSamples* followingOut,
    VnResult (&results)[2]) {
    if (this == &following || firstOut == nullptr || followingOut == nullptr ||
        firstOut == followingOut || firstOut->IsPrepared() || followingOut->IsPrepared() ||
        m_entry == nullptr || following.m_entry == nullptr ||
        m_equalCandidate != nullptr || following.m_equalCandidate != nullptr ||
        m_status != VN_OK || following.m_status != VN_OK ||
        m_written != m_count || following.m_written != following.m_count ||
        m_entry->view.frames != following.m_entry->view.frames ||
        m_entry->view.channelCount != following.m_entry->view.channelCount) return false;
    PrepareSampleView();
    following.PrepareSampleView();
    auto& first = m_entry->view;
    auto& second = following.m_entry->view;
    const MediaPcmContentRevisionInputV1 firstInput{first.sampleRate, first.channelCount,
        first.frames, nullptr, first.stride, first.loop.startFrame, first.loop.endFrame};
    const MediaPcmContentRevisionInputV1 secondInput{second.sampleRate, second.channelCount,
        second.frames, nullptr, second.stride, second.loop.startFrame, second.loop.endFrame};
    std::uint64_t revisions[2]{};
    bool valid[2]{};
    detail::ComputeMediaPcmRevisionPairV1(firstInput, secondInput,
        [&](std::uint32_t channel, std::uint64_t frame, std::uint32_t* bits) {
            const auto sample = first.ReadSample(channel, frame);
            return detail::ReadMediaPcmRevisionBitsV1(&sample, 0, bits);
        },
        [&](std::uint32_t channel, std::uint64_t frame, std::uint32_t* bits) {
            const auto sample = second.ReadSample(channel, frame);
            return detail::ReadMediaPcmRevisionBitsV1(&sample, 0, bits);
        }, revisions, valid);
    results[0] = valid[0] ? VN_OK : VN_INVALID_ARG;
    results[1] = valid[1] ? VN_OK : VN_INVALID_ARG;
    if (valid[0]) { first.contentRevision = revisions[0]; MoveCompleted(firstOut); }
    else m_status = results[0];
    if (valid[1]) { second.contentRevision = revisions[1]; following.MoveCompleted(followingOut); }
    else following.m_status = results[1];
    return true;
}

VnResult PCMRegistry::PreparePackMutation(
    const PackReferenceRegistration* registrations,
    std::size_t registrationCount, std::size_t retiringCount,
    PreparedPackMutation* out) {
    if (out == nullptr ||
        (registrationCount != 0 && registrations == nullptr) ||
        retiringCount > m_entries.size()) {
        return VN_INVALID_ARG;
    }
    *out = PreparedPackMutation{};

    PreparedPackMutation prepared;
    prepared.m_staged.reserve(registrationCount);
    std::uint64_t nextCookie = m_nextCookie;
    std::uint64_t followingRevision = 0;
    bool followingComputed = false;
    bool followingValid = false;
    for (std::size_t i = 0; i < registrationCount; ++i) {
        const PackReferenceRegistration& registration = registrations[i];
        if (registration.samples != nullptr) {
            auto& samples = *registration.samples;
            if (!samples.IsPrepared() || samples.m_soundId != registration.soundId ||
                samples.m_entry->cookie != 0)
                return VN_INVALID_ARG;
            if (PcmRegisterFaultForTests() == PcmRegisterFault::MapCommit)
                throw std::bad_alloc{};
            auto [it, inserted] = prepared.m_staged.try_emplace(registration.soundId);
            if (!inserted) return VN_IN_USE;
            it->second = std::move(samples.m_entry);
            if (nextCookie == 0) nextCookie = 1;
            it->second->cookie = nextCookie++;
            continue;
        }
        const VnResult valid = ValidateDesc(&registration.desc);
        if (valid != VN_OK) {
            return valid;
        }
        std::uint64_t contentRevision = 0;
        bool revisionComputed = false;
        bool revisionValid = false;
        if (followingComputed) {
            contentRevision = followingRevision;
            revisionValid = followingValid;
            followingComputed = false;
            revisionComputed = true;
        } else if (registrationCount - i > 1 &&
                   registration.desc.policy == VN_PCM_POLICY_COPY &&
                   registration.desc.layout == VN_PCM_LAYOUT_PLANAR &&
                   registration.desc.channels <= 2) {
            const auto& next = registrations[i + 1];
            if (next.samples == nullptr && next.desc.policy == VN_PCM_POLICY_COPY &&
                next.desc.layout == VN_PCM_LAYOUT_PLANAR &&
                next.desc.channels == registration.desc.channels &&
                next.desc.frames == registration.desc.frames &&
                ValidateDesc(&next.desc) == VN_OK) {
                const MediaPcmContentRevisionInputV1 first{
                    registration.desc.sampleRate, registration.desc.channels,
                    registration.desc.frames, registration.desc.samplesPlanar, 1,
                    registration.loop.startFrame, registration.loop.endFrame};
                const MediaPcmContentRevisionInputV1 second{
                    next.desc.sampleRate, next.desc.channels, next.desc.frames,
                    next.desc.samplesPlanar, 1, next.loop.startFrame, next.loop.endFrame};
                std::uint64_t revisions[2]{};
                bool validRevisions[2]{};
                if (TryComputeMediaPcmContentRevisionPairV1(
                        first, second, revisions, validRevisions)) {
                    contentRevision = revisions[0];
                    revisionValid = validRevisions[0];
                    revisionComputed = true;
                    followingRevision = revisions[1];
                    followingValid = validRevisions[1];
                    followingComputed = true;
                }
            }
        }
        if (!revisionComputed)
            revisionValid = ComputeContentRevision(
                registration.desc, &contentRevision, registration.loop);
        if (!revisionValid) return VN_INVALID_ARG;
        // Consume the following result only at its original validation point.
        // Current allocation, duplicate-id and publication order stay unchanged.
        if (PcmRegisterFaultForTests() == PcmRegisterFault::MapCommit) {
            throw std::bad_alloc{};
        }
        auto [it, inserted] =
            prepared.m_staged.try_emplace(registration.soundId, std::make_shared<Entry>());
        if (!inserted) {
            return VN_IN_USE;
        }
        if (nextCookie == 0) {
            nextCookie = 1;
        }
        Entry& entry = *it->second;
        entry.ownedSamples = StageSamples(registration.desc);
        entry.cookie = nextCookie++;
        entry.view.channelCount = registration.desc.channels;
        entry.view.sampleRate = registration.desc.sampleRate;
        entry.view.frames = registration.desc.frames;
        entry.view.contentRevision = contentRevision;
        entry.view.loop = registration.loop;
        entry.view.useCount = &entry.uses;
        SetSamplePointers(entry.view, registration.desc, entry.ownedSamples);
    }

    const std::size_t successorCount =
        m_entries.size() - retiringCount + registrationCount;
    m_entries.reserve(successorCount);
    prepared.m_owner = this;
    prepared.m_nextCookie = nextCookie;
    *out = std::move(prepared);
    return VN_OK;
}

void PCMRegistry::CommitPreparedPackMutation(
    PreparedPackMutation* prepared) noexcept {
    assert(prepared != nullptr && prepared->m_owner == this);
    if (prepared == nullptr || prepared->m_owner != this) {
        return;
    }
    for (auto it = prepared->m_staged.begin();
         it != prepared->m_staged.end();) {
        auto node = prepared->m_staged.extract(it++);
        const auto inserted = m_entries.insert(std::move(node));
        assert(inserted.inserted);
        (void)inserted;
    }
    m_nextCookie = prepared->m_nextCookie;
    prepared->m_owner = nullptr;
}

void PCMRegistry::ReinsertRetired(RetiredGeneration&& generation) {
    m_entries.insert(std::move(generation.m_node));
}

void PCMRegistry::SweepRetired() {
    for (std::size_t i = 0; i < m_retired.size();) {
        if (m_retired[i].Drained()) {
            m_retired.erase(m_retired.begin() +
                            static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

bool PCMRegistry::HasRetired(VnSoundId soundId) const {
    for (const RetiredGeneration& g : m_retired) {
        if (g.SoundId() == soundId) {
            return true;
        }
    }
    return false;
}

PCMRegistry::PackReference PCMRegistry::ReferenceOf(
    VnSoundId soundId, std::uint64_t expectedCookie) const noexcept {
    PackReference result;
    const auto it = m_entries.find(soundId);
    if (it != m_entries.end() && it->second->cookie == expectedCookie) {
        result.entry_ = it->second;
    }
    return result;
}

std::uint64_t PCMRegistry::CookieOf(VnSoundId soundId) const {
    auto it = m_entries.find(soundId);
    return it != m_entries.end() ? it->second->cookie : 0;
}

PCMView PCMRegistry::Lookup(VnSoundId soundId) const {
    auto it = m_entries.find(soundId);
    if (it == m_entries.end()) {
        return PCMView{};
    }
    return it->second->view;
}

} // namespace harken
