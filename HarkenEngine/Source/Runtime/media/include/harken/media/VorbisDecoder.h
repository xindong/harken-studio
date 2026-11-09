#ifndef HARKEN_MEDIA_VORBIS_DECODER_H
#define HARKEN_MEDIA_VORBIS_DECODER_H
#include "harken/media/VorbisMedia.h"
#include "harken/core/MediaCodec.h"
#include "harken/core/PcmView.h"
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>

struct HvTables;
struct HvSetup;
struct HvState;
struct HvWorkspace;

namespace harken {
class VorbisResident;
class VorbisCodec;
enum class VorbisPrepareResult { Ok, Invalid, Unsupported, OutOfMemory };

class VorbisSettings {
public:
    ~VorbisSettings();
    VorbisSettings(const VorbisSettings&) = delete;
    VorbisSettings& operator=(const VorbisSettings&) = delete;
    unsigned Channels() const noexcept;
    unsigned LongBlock() const noexcept;
    std::size_t Bytes() const noexcept;
    std::size_t StateBytes() const noexcept;
    long PacketBlock(const std::uint8_t* bytes, std::size_t count) const noexcept;
    bool ValidateResident(const VorbisMedia& media) const noexcept;
private:
    friend class VorbisSettingsCache;
    friend class VorbisStatePool;
    VorbisSettings() = default;
    std::shared_ptr<HvTables> tables_;
    HvSetup* setup_ = nullptr;
    std::vector<std::uint8_t> headers_;
};

// Logic-thread cache. Weak entries do not keep unused media alive. Identity
// includes identification and setup bytes; hashes only accelerate lookup.
class VorbisSettingsCache {
public:
    bool Initialize(std::size_t tableBudget);
    std::shared_ptr<const VorbisSettings> Prepare(const VorbisMedia& media,
        std::size_t resourceBudget, VorbisPrepareResult& result);
    std::size_t TableBytes() const noexcept;
private:
    std::shared_ptr<HvTables> tables_;
    std::unordered_multimap<std::uint64_t, std::weak_ptr<const VorbisSettings>> settings_;
};

class VorbisWorkspace {
public:
    bool Initialize();
    std::size_t Bytes() const noexcept { return count_ * sizeof(std::max_align_t) + packet_.size(); }
    std::size_t HighWater() const noexcept;
private:
    friend class VorbisStatePool;
    std::unique_ptr<std::max_align_t[]> storage_;
    std::size_t count_ = 0;
    HvWorkspace* workspace_ = nullptr;
    // One wrapped packet per render worker, shared by all voices on that worker.
    std::array<std::uint8_t, UINT16_MAX> packet_{};
};

// State ownership moves from logic preparation to one render voice. Render
// retirement only publishes a flag; logic reclaims settings and cached memory.
class VorbisStatePool {
public:
    struct State : MediaDecoder {
        bool Read(std::uint64_t frame, std::uint32_t frames,
                  std::uint32_t renderThread, PCMView& output) noexcept override;
        StreamDecodeResult ReadStream(StreamRing& ring, std::uint32_t frames,
            std::uint32_t renderThread, PCMView& output) noexcept override;
        StreamDecodeResult CropStream(StreamRing& ring, std::uint32_t frames,
            std::uint32_t renderThread) noexcept override;
        void Restart() noexcept;
        bool Decode(VorbisWorkspace& workspace, const std::uint8_t* packet, std::size_t bytes,
                    const float* (&pcm)[8], unsigned& frames) noexcept;
        void Retire() noexcept override { retired.store(true, std::memory_order_release); }
    private:
        friend class VorbisStatePool;
        friend class VorbisResident;
        friend class VorbisCodec;
        const VorbisResident* resident = nullptr;
        VorbisCodec* codec = nullptr;
        const StreamRing* streamRing = nullptr;
        std::uint32_t streamGeneration = 0;
        std::uint64_t packetOffset = 0;
        std::uint64_t nextFrame = UINT64_MAX;
        std::uint32_t discard = 0, decodedFrames = 0, decodedOffset = 0;
        const float* planes[8]{};
        State* next = nullptr;
        State* nextFree = nullptr;
        std::unique_ptr<std::max_align_t[]> storage;
        std::shared_ptr<const VorbisSettings> settings;
        HvState* decoder = nullptr;
        std::size_t backingBytes = 0;
        unsigned key = 0;
        bool available = false;
        std::atomic<bool> retired{false};
    };
    explicit VorbisStatePool(std::size_t budget) noexcept : budget_(budget) {}
    ~VorbisStatePool();
    VorbisStatePool(const VorbisStatePool&) = delete;
    VorbisStatePool& operator=(const VorbisStatePool&) = delete;
    State* Acquire(std::shared_ptr<const VorbisSettings> settings);
    void Collect() noexcept;
    std::size_t Bytes() const noexcept { return bytes_; }
    std::size_t ActiveBytes() const noexcept { return activeBytes_; }
    std::size_t HighWater() const noexcept { return highWater_; }
    static std::size_t RequiredBytes(const VorbisSettings& settings) noexcept;
private:
    void TrimFree() noexcept;
    State* all_ = nullptr;
    // Legal channel counts and power-of-two blocks determine the key domain.
    std::array<State*, 64> free_{};
    std::size_t budget_ = 0, bytes_ = 0, activeBytes_ = 0, highWater_ = 0;
};

// Engine lifetime owns tables, bounded reusable state and one workspace for
// each render thread. Resources own only compressed bytes and shared setup.
class VorbisCodec {
public:
    VorbisCodec(std::size_t stateBudget, unsigned renderWorkers);
    MediaDecoder* AcquireStream(std::shared_ptr<const VorbisSettings> settings);
    VorbisSettingsCache settings;
    VorbisStatePool states;
    std::vector<std::unique_ptr<VorbisWorkspace>> workspaces;
};

class VorbisResident final : public EncodedMedia {
public:
    static std::shared_ptr<VorbisResident> Prepare(VorbisCodec& codec,
        std::vector<std::uint8_t> bytes, std::size_t budget, VorbisPrepareResult& result);
    MediaDecoder* Acquire() const noexcept override;
    std::size_t ResidentBytes() const noexcept override;
    std::span<const std::uint8_t> ContentBytes() const noexcept override { return bytes_; }
    PCMView View() const noexcept;
private:
    friend struct VorbisStatePool::State;
    VorbisCodec* codec_ = nullptr;
    std::vector<std::uint8_t> bytes_;
    VorbisMedia media_;
    std::shared_ptr<const VorbisSettings> settings_;
};

// A stream retains only its immutable prefix, index and shared setup. The
// manager owns external byte supply; the voice owns the acquired decoder.
class VorbisStreamMedia final : public EncodedMedia {
public:
    // Packet bytes are accepted only from a prefix already covered by Pack
    // integrity validation. Metadata-only I/O never grants that authority.
    static std::shared_ptr<VorbisStreamMedia> Prepare(VorbisCodec& codec,
        std::vector<std::uint8_t> prefix, std::uint64_t mediaBytes,
        std::size_t budget, VorbisPrepareResult& result, bool validatedPackPrefix = false);
    MediaDecoder* Acquire() const noexcept override;
    std::size_t ResidentBytes() const noexcept override;
    // A prefix alone cannot prove complete media content equality.
    std::span<const std::uint8_t> ContentBytes() const noexcept override { return {}; }
    const VorbisMedia& Media() const noexcept { return media_; }
    const VorbisSettings& Settings() const noexcept { return *settings_; }
    std::span<const std::uint8_t> Prefix() const noexcept { return prefix_; }
    std::span<const std::uint8_t> Prefetch() const noexcept {
        return std::span<const std::uint8_t>(prefix_).subspan(static_cast<std::size_t>(media_.packetDataOffset));
    }
private:
    VorbisCodec* codec_ = nullptr;
    std::vector<std::uint8_t> prefix_;
    VorbisMedia media_;
    std::shared_ptr<const VorbisSettings> settings_;
};
}
#endif
