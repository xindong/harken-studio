#ifndef HARKEN_MEDIA_VORBIS_DECODER_H
#define HARKEN_MEDIA_VORBIS_DECODER_H
#include "harken/media/VorbisMedia.h"
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>

struct HvTables;
struct HvSetup;
struct HvState;
struct HvWorkspace;

namespace harken {
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
    std::size_t Bytes() const noexcept { return count_ * sizeof(std::max_align_t); }
    std::size_t HighWater() const noexcept;
private:
    friend class VorbisStatePool;
    std::unique_ptr<std::max_align_t[]> storage_;
    std::size_t count_ = 0;
    HvWorkspace* workspace_ = nullptr;
};

// State ownership moves from logic preparation to one render voice. Render
// retirement only publishes a flag; logic reclaims settings and cached memory.
class VorbisStatePool {
public:
    struct State {
        void Restart() noexcept;
        bool Decode(VorbisWorkspace& workspace, const std::uint8_t* packet, std::size_t bytes,
                    const float* (&pcm)[8], unsigned& frames) noexcept;
        void Retire() noexcept { retired.store(true, std::memory_order_release); }
    private:
        friend class VorbisStatePool;
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
}
#endif
