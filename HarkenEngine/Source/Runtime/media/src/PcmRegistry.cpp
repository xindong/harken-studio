#include "harken/media/PcmRegistry.h"

#include "harken/core/MediaContentRevision.h"

#include <cstdint>
#include <cassert>
#include <cstring>
#include <new>
#include <utility>

namespace harken {

namespace {

PcmRegisterFault g_pcmRegisterFault = PcmRegisterFault::None;

VnResult ValidateDesc(const VnPCMDesc* desc) {
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

std::vector<float> StageSamples(const VnPCMDesc& desc) {
    std::vector<float> stagedSamples;
    if (desc.policy != VN_PCM_POLICY_COPY) {
        return stagedSamples;
    }
    if (PcmRegisterFaultForTests() == PcmRegisterFault::SampleBacking) {
        throw std::bad_alloc{};
    }

    const std::size_t totalFloats =
        static_cast<std::size_t>(desc.frames) * desc.channels;
    stagedSamples.resize(totalFloats);
    if (desc.layout == VN_PCM_LAYOUT_INTERLEAVED) {
        std::memcpy(stagedSamples.data(), desc.samplesInterleaved,
                    totalFloats * sizeof(float));
    } else {
        for (std::uint32_t channel = 0; channel < desc.channels; ++channel) {
            std::memcpy(stagedSamples.data() +
                            static_cast<std::size_t>(channel) * desc.frames,
                        desc.samplesPlanar[channel],
                        static_cast<std::size_t>(desc.frames) * sizeof(float));
        }
    }
    return stagedSamples;
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
    std::vector<float> stagedSamples = StageSamples(*desc);
    return CommitPCM(soundId, desc, std::move(stagedSamples),
                     contentRevision, outCookie, loop);
}

VnResult PCMRegistry::CommitPCM(VnSoundId soundId, const VnPCMDesc* desc,
                                std::vector<float>&& stagedSamples,
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

    // Past this point every operation is noexcept: a vector move (default
    // std::allocator propagates on move assignment) and scalar/pointer
    // fills. No further failure can leave the map in a half-registered
    // state.
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

    if (desc->layout == VN_PCM_LAYOUT_INTERLEAVED) {
        const float* base = (desc->policy == VN_PCM_POLICY_COPY)
                                ? entry.ownedSamples.data()
                                : desc->samplesInterleaved;
        view.stride = desc->channels;
        for (std::uint32_t ch = 0; ch < desc->channels; ++ch) {
            view.channels[ch] = base + ch;
        }
    } else {
        view.stride = 1;
        for (std::uint32_t ch = 0; ch < desc->channels; ++ch) {
            view.channels[ch] =
                (desc->policy == VN_PCM_POLICY_COPY)
                    ? entry.ownedSamples.data() +
                          static_cast<std::size_t>(ch) * desc->frames
                    : desc->samplesPlanar[ch];
        }
    }

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
    std::vector<float> stagedSamples = StageSamples(*desc);
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
    for (std::size_t i = 0; i < registrationCount; ++i) {
        const PackReferenceRegistration& registration = registrations[i];
        const VnResult valid = ValidateDesc(&registration.desc);
        if (valid != VN_OK) {
            return valid;
        }
        std::uint64_t contentRevision = 0;
        if (!ComputeContentRevision(registration.desc, &contentRevision, registration.loop)) {
            return VN_INVALID_ARG;
        }
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
        entry.view.stride = registration.desc.layout == VN_PCM_LAYOUT_PLANAR
                                ? 1
                                : registration.desc.channels;
        entry.view.contentRevision = contentRevision;
        entry.view.loop = registration.loop;
        entry.view.useCount = &entry.uses;
        if (registration.desc.policy == VN_PCM_POLICY_COPY) {
            for (std::uint32_t ch = 0; ch < registration.desc.channels; ++ch) {
                entry.view.channels[ch] = entry.ownedSamples.data() +
                    (registration.desc.layout == VN_PCM_LAYOUT_PLANAR
                        ? static_cast<std::size_t>(ch) * registration.desc.frames : ch);
            }
        } else if (registration.desc.layout == VN_PCM_LAYOUT_PLANAR) {
            for (std::uint32_t ch = 0; ch < registration.desc.channels; ++ch) {
                entry.view.channels[ch] =
                    registration.desc.samplesPlanar[ch];
            }
        } else {
            for (std::uint32_t ch = 0; ch < registration.desc.channels; ++ch) {
                entry.view.channels[ch] =
                    registration.desc.samplesInterleaved + ch;
            }
        }
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
