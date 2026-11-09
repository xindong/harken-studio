#include "harken/media/VorbisDecoder.h"
#include "harken/media/PackFormat.h"
#include "../../../../ThirdParty/vorbis-decoder/decoder.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace harken {
VorbisCodec::VorbisCodec(std::size_t budget, unsigned workers) : states(budget) {
    if (!settings.Initialize(256 * 1024)) throw std::bad_alloc{};
    for (unsigned i = 0; i <= workers; ++i) {
        auto workspace = std::make_unique<VorbisWorkspace>();
        if (!workspace->Initialize()) throw std::bad_alloc{};
        workspaces.push_back(std::move(workspace));
    }
}

std::shared_ptr<VorbisResident> VorbisResident::Prepare(VorbisCodec& codec,
    std::vector<std::uint8_t> bytes, std::size_t budget, VorbisPrepareResult& result) {
    result = VorbisPrepareResult::OutOfMemory;
    if (bytes.size() > budget) return {};
    auto source = std::make_shared<VorbisResident>();
    source->bytes_ = std::move(bytes);
    result = VorbisPrepareResult::Invalid;
    if (!ParseVorbisTestMedia(source->bytes_.data(), source->bytes_.size(), source->media_)) return {};
    const auto overhead = source->ResidentBytes();
    result = VorbisPrepareResult::OutOfMemory;
    if (overhead > budget) return {};
    source->settings_ = codec.settings.Prepare(source->media_, budget - overhead, result);
    if (!source->settings_) return {};
    result = VorbisPrepareResult::Invalid;
    if (!source->settings_->ValidateResident(source->media_)) return {};
    source->codec_ = &codec;
    result = VorbisPrepareResult::Ok;
    return source;
}
std::size_t VorbisResident::ResidentBytes() const noexcept {
    return sizeof(*this) + bytes_.capacity() + media_.seek.capacity() * sizeof(VorbisSeekPoint) +
        media_.segmentChecksums.capacity() * sizeof(std::uint64_t) + (settings_ ? settings_->Bytes() : 0);
}
PCMView VorbisResident::View() const noexcept {
    PCMView view;
    view.encoded = this; view.frames = media_.frames; view.channelCount = media_.channels;
    view.sampleRate = media_.sampleRate; view.contentRevision = media_.revision;
    view.loop = {media_.loopStart, media_.loopEnd};
    return view;
}
MediaDecoder* VorbisResident::Acquire() const noexcept {
    auto* state = codec_->states.Acquire(settings_);
    if (state) {
        state->resident = this; state->nextFrame = UINT64_MAX;
        state->decodedFrames = state->decodedOffset = state->discard = 0;
    }
    return state;
}
bool VorbisStatePool::State::Read(std::uint64_t frame, std::uint32_t requested,
    std::uint32_t thread, PCMView& output) noexcept {
    if (!resident || thread >= resident->codec_->workspaces.size() || !requested) return false;
    const auto& media = resident->media_;
    if (frame >= media.frames) return false;
    if (frame != nextFrame) {
        const auto* point = media.Seek(frame);
        if (!point) return false;
        Restart(); decodedFrames = decodedOffset = 0;
        packetOffset = point->packetOffset;
        const auto skip = frame - point->frame + point->discardFrames;
        if (skip > UINT32_MAX) return false;
        discard = static_cast<std::uint32_t>(skip);
        if (frame == media.loopStart) {
            packetOffset = media.loopStartPacket; discard = media.loopDiscard;
        }
        nextFrame = frame;
    }
    auto& workspace = *resident->codec_->workspaces[thread];
    while (decodedOffset == decodedFrames) {
        const std::uint8_t* packet = nullptr;
        std::uint16_t bytes = 0;
        if (!media.Packet(packetOffset, packet, bytes) || !Decode(workspace, packet, bytes, planes, decodedFrames))
            return false;
        packetOffset += 2u + bytes;
        decodedOffset = std::min(discard, decodedFrames);
        discard -= decodedOffset;
    }
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        std::min(requested, decodedFrames - decodedOffset), media.frames - frame));
    output = {};
    output.channelCount = media.channels; output.sampleRate = media.sampleRate; output.frames = count;
    for (unsigned ch = 0; ch < media.channels; ++ch) output.channels[ch] = planes[ch] + decodedOffset;
    decodedOffset += count; nextFrame += count;
    return true;
}

namespace {
HvMemory Memory() noexcept {
    return {nullptr, [](void*, std::size_t bytes) -> void* { return ::operator new(bytes, std::nothrow); },
        [](void*, void* p) { ::operator delete(p); }};
}
std::size_t AlignCount(std::size_t bytes) noexcept {
    return (bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
}
bool ValidatePositions(const VorbisMedia& media, const VorbisSettings& settings) noexcept {
    if (!media.packets || !media.packetBytes || !media.frames || !media.seekGranularity ||
        media.seek.size() != 1 + (media.frames - 1) / media.seekGranularity ||
        media.channels != settings.Channels() || media.blockLong != settings.LongBlock()) return false;
    std::uint64_t produced = 0, previousOffset = 0, lastStart = 0;
    long previousBlock = 0;
    std::size_t seek = 0;
    bool loopStart = media.loopEnd == 0, loopEnd = loopStart;
    for (std::uint64_t at = 0; at < media.packetBytes;) {
        const std::uint8_t* packet; std::uint16_t bytes;
        if (!media.Packet(at, packet, bytes)) return false;
        const auto block = settings.PacketBlock(packet, bytes);
        if (block <= 0) return false;
        if (previousBlock) {
            lastStart = produced;
            const auto frames = static_cast<std::uint64_t>(previousBlock / 4 + block / 4);
            if (frames > UINT64_MAX - produced) return false;
            produced += frames;
            while (seek < media.seek.size() && media.seek[seek].frame < produced) {
                const auto& point = media.seek[seek++];
                if (point.frame < lastStart || point.packetOffset != previousOffset ||
                    point.discardFrames != point.frame - lastStart) return false;
            }
            if (!loopStart && media.loopStart >= lastStart && media.loopStart < produced) {
                if (media.loopStartPacket != previousOffset || media.loopDiscard != media.loopStart - lastStart)
                    return false;
                loopStart = true;
            }
            if (!loopEnd && media.loopEnd > lastStart && media.loopEnd <= produced) {
                if (media.loopEndPacket != at || media.loopTrim != produced - media.loopEnd) return false;
                loopEnd = true;
            }
        }
        previousOffset = at; previousBlock = block; at += bytes + 2;
    }
    return seek == media.seek.size() && loopStart && loopEnd && media.frames > lastStart && media.frames <= produced;
}
}
VorbisSettings::~VorbisSettings() { hv_setup_destroy(setup_); }
unsigned VorbisSettings::Channels() const noexcept { return hv_setup_channels(setup_); }
unsigned VorbisSettings::LongBlock() const noexcept { return hv_setup_blocksize(setup_); }
std::size_t VorbisSettings::Bytes() const noexcept {
    return sizeof(*this) + headers_.capacity() + hv_setup_bytes(setup_);
}
std::size_t VorbisSettings::StateBytes() const noexcept { return hv_state_bytes(setup_); }
long VorbisSettings::PacketBlock(const std::uint8_t* bytes, std::size_t count) const noexcept {
    return hv_packet_blocksize(setup_, bytes, count);
}
bool VorbisSettings::ValidateResident(const VorbisMedia& media) const noexcept {
    return ValidatePositions(media, *this);
}
bool VorbisSettingsCache::Initialize(std::size_t budget) {
    if (tables_) return true;
    auto* raw = hv_tables_create(Memory(), budget);
    if (!raw) return false;
    try { tables_ = std::shared_ptr<HvTables>(raw, hv_tables_destroy); }
    catch (const std::bad_alloc&) { return false; }
    return true;
}
std::size_t VorbisSettingsCache::TableBytes() const noexcept { return hv_tables_bytes(tables_.get()); }
std::shared_ptr<const VorbisSettings> VorbisSettingsCache::Prepare(const VorbisMedia& media,
    std::size_t budget, VorbisPrepareResult& result) {
    result = VorbisPrepareResult::Invalid;
    if (!tables_ || !media.ident || !media.setup || media.identBytes != 30 || !media.setupBytes) return {};
    for (auto i = settings_.begin(); i != settings_.end();)
        if (i->second.expired()) i = settings_.erase(i); else ++i;
    const auto hash = Fnv1a64(media.setup, media.setupBytes, Fnv1a64(media.ident, media.identBytes));
    const auto found = settings_.equal_range(hash);
    const std::size_t headerBytes = std::size_t(media.identBytes) + media.setupBytes;
    for (auto i = found.first; i != found.second; ++i) {
        auto existing = i->second.lock();
        if (existing && existing->headers_.size() == headerBytes &&
            std::memcmp(existing->headers_.data(), media.ident, media.identBytes) == 0 &&
            std::memcmp(existing->headers_.data() + media.identBytes, media.setup, media.setupBytes) == 0) {
            if (existing->Bytes() > budget) { result = VorbisPrepareResult::OutOfMemory; return {}; }
            result = VorbisPrepareResult::Ok; return existing;
        }
    }
    result = VorbisPrepareResult::OutOfMemory;
    if (budget < sizeof(VorbisSettings) || headerBytes > budget - sizeof(VorbisSettings)) return {};
    try {
        std::shared_ptr<VorbisSettings> settings(new VorbisSettings);
        settings->tables_ = tables_;
        settings->headers_.reserve(headerBytes);
        settings->headers_.insert(settings->headers_.end(), media.ident, media.ident + media.identBytes);
        settings->headers_.insert(settings->headers_.end(), media.setup, media.setup + media.setupBytes);
        HvResult status;
        settings->setup_ = hv_setup_create(Memory(), budget - sizeof(VorbisSettings) - headerBytes,
            tables_.get(), media.ident, media.identBytes, media.setup, media.setupBytes, &status);
        if (!settings->setup_) {
            result = status == HV_UNSUPPORTED ? VorbisPrepareResult::Unsupported :
                status == HV_MEMORY ? VorbisPrepareResult::OutOfMemory : VorbisPrepareResult::Invalid;
            return {};
        }
        settings_.emplace(hash, settings);
        result = VorbisPrepareResult::Ok; return settings;
    } catch (const std::bad_alloc&) { return {}; }
}
bool VorbisWorkspace::Initialize() {
    if (workspace_) return true;
    const auto count = AlignCount(hv_workspace_bytes());
    std::unique_ptr<std::max_align_t[]> storage(new (std::nothrow) std::max_align_t[count]);
    if (!storage) return false;
    workspace_ = hv_workspace_init(storage.get(), count * sizeof(std::max_align_t));
    if (!workspace_) return false;
    storage_ = std::move(storage); count_ = count; return true;
}
std::size_t VorbisWorkspace::HighWater() const noexcept { return hv_workspace_high_water(workspace_); }
void VorbisStatePool::State::Restart() noexcept { hv_state_restart(decoder); }
bool VorbisStatePool::State::Decode(VorbisWorkspace& workspace, const std::uint8_t* packet,
    std::size_t bytes, const float* (&pcm)[8], unsigned& frames) noexcept {
    float** decoded = nullptr;
    if (hv_decode_packet(decoder, workspace.workspace_, packet, bytes, &decoded, &frames) != HV_OK) return false;
    static constexpr unsigned map6[] = {0, 2, 1, 5, 3, 4};
    static constexpr unsigned map8[] = {0, 2, 1, 7, 5, 6, 3, 4};
    const unsigned channels = settings->Channels();
    for (unsigned ch = 0; ch < channels; ++ch)
        pcm[ch] = frames ? decoded[channels == 6 ? map6[ch] : channels == 8 ? map8[ch] : ch] : nullptr;
    return true;
}
std::size_t VorbisStatePool::RequiredBytes(const VorbisSettings& settings) noexcept {
    return sizeof(State) + AlignCount(settings.StateBytes()) * sizeof(std::max_align_t);
}
VorbisStatePool::~VorbisStatePool() {
    while (all_) { auto* next = all_->next; delete all_; all_ = next; }
}
void VorbisStatePool::Collect() noexcept {
    for (auto* state = all_; state; state = state->next) {
        if (!state->available && state->retired.exchange(false, std::memory_order_acquire)) {
            activeBytes_ -= state->backingBytes + sizeof(State);
            state->settings.reset(); state->decoder = nullptr; state->available = true;
            state->nextFree = free_[state->key]; free_[state->key] = state;
        }
    }
}
void VorbisStatePool::TrimFree() noexcept {
    free_.fill(nullptr);
    auto** link = &all_;
    while (*link) {
        auto* state = *link;
        if (state->available) { *link = state->next; bytes_ -= state->backingBytes + sizeof(State); delete state; }
        else link = &state->next;
    }
}
VorbisStatePool::State* VorbisStatePool::Acquire(std::shared_ptr<const VorbisSettings> settings) {
    if (!settings) return nullptr;
    Collect();
    unsigned block = settings->LongBlock(), blockKey = 0;
    while (block > 64) { block >>= 1; ++blockKey; }
    const unsigned key = (settings->Channels() - 1) * 8 + blockKey;
    auto* state = free_[key];
    if (state) { free_[key] = state->nextFree; state->nextFree = nullptr; }
    else {
        const auto required = RequiredBytes(*settings);
        if (required > budget_ - activeBytes_) return nullptr;
        if (required > budget_ - bytes_) TrimFree();
        std::unique_ptr<State> created(new (std::nothrow) State);
        if (!created) return nullptr;
        const auto count = AlignCount(settings->StateBytes());
        created->storage.reset(new (std::nothrow) std::max_align_t[count]);
        if (!created->storage) return nullptr;
        created->backingBytes = count * sizeof(std::max_align_t); created->key = key;
        state = created.release(); state->next = all_; all_ = state;
        bytes_ += required; highWater_ = std::max(highWater_, bytes_);
    }
    state->settings = std::move(settings); state->available = false;
    state->decoder = hv_state_init(state->storage.get(), state->backingBytes, state->settings->setup_);
    activeBytes_ += state->backingBytes + sizeof(State);
    return state;
}
}
