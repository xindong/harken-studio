#ifndef HARKEN_API_MONITOR_HOST_SYMBOL_H
#define HARKEN_API_MONITOR_HOST_SYMBOL_H

#include "MonitorHostDescription.h"
#include "MonitorExecutionProducer.h"
#include "harken/core/MonitorEmitterIdentity.h"
#include <span>
#include <vector>

namespace harken::monitor {

inline constexpr std::uint64_t kHostSymbolProducer = 7;
inline constexpr std::uint64_t kHostSymbolStream = 205;

struct MonitorHostSymbolBody {
    std::uint64_t emitter = 0, sdkSession = 0, localSequence = 0, registrationRevision = 0;
    std::uint16_t field = 0, factKind = 0, textBytes = 0, flags = 0;
    std::uint8_t text[256]{};
};

struct MonitorHostSymbolSource {
    std::uint64_t engine = 0, sourceSequence = 0, boundaryRevision = 0;
    MonitorHostSymbolBody value{};
};
using MonitorHostSymbolProducer = MonitorValueProducer<MonitorHostSymbolSource>;

struct MonitorHostSymbolValue {
    MonitorEmitterOrigin origin{};
    MonitorHostDescription description{};

    bool Present() const noexcept { return origin.host.Valid(); }
    bool Same(const MonitorHostSymbolValue& other) const noexcept {
        if (!Present() || !other.Present()) return Present() == other.Present();
        return origin.emitter == other.origin.emitter && origin.host == other.origin.host &&
            origin.registrationRevision == other.origin.registrationRevision && description.Same(other.description);
    }
};
static_assert(sizeof(MonitorHostSymbolBody) == 296);
static_assert(sizeof(MonitorHostSymbolSource) == 320);
static_assert(sizeof(MonitorHostSymbolValue) == 504);
static_assert(std::is_trivially_copyable_v<MonitorHostSymbolSource>);

inline MonitorHostSymbolSource HostSymbolField(const MonitorHostSymbolValue& host,
    unsigned field, unsigned kind, std::uint64_t revision) noexcept {
    MonitorHostSymbolSource source{};
    source.engine = host.origin.host.engineInstance;
    source.boundaryRevision = revision;
    auto& value = source.value;
    value.emitter = host.origin.emitter;
    value.sdkSession = host.origin.host.sdkSession;
    value.localSequence = host.origin.host.localSequence;
    value.registrationRevision = host.origin.registrationRevision;
    value.field = static_cast<std::uint16_t>(field);
    value.factKind = static_cast<std::uint16_t>(kind);
    value.flags = host.description.flags;
    const auto* text = host.description.name;
    if (field == VN_MONITOR_HOST_SYMBOL_NAME) value.textBytes = host.description.nameBytes;
    else if (field == VN_MONITOR_HOST_SYMBOL_PATH) {
        text = host.description.path;
        value.textBytes = host.description.pathBytes;
    } else if (field == VN_MONITOR_HOST_SYMBOL_SCENE) {
        text = host.description.scene;
        value.textBytes = host.description.sceneBytes;
    }
    if (value.textBytes <= sizeof(value.text)) std::memcpy(value.text, text, value.textBytes);
    return source;
}

// The mirror contains one description per live Emitter, not three wire rows.
// All allocation occurs at setup. Seed never publishes capture-off history.
class MonitorHostSymbolMirror {
public:
    void Reset(std::size_t capacity) { rows_.assign(capacity, {}); revision_ = 1; unavailable_ = 0; }
    void Clear() noexcept { std::vector<MonitorHostSymbolValue>().swap(rows_); revision_ = 1; unavailable_ = 0; }
    std::uint64_t Revision() const noexcept { return revision_; }
    std::uint64_t Unavailable() const noexcept { return unavailable_; }
    std::uint64_t StorageBytes() const noexcept { return rows_.size() * sizeof(MonitorHostSymbolValue); }

    template<class Capture>
    void Seed(Capture&& capture) noexcept {
        for (std::size_t slot = 0; slot != rows_.size(); ++slot)
            if (!capture(slot, rows_[slot])) rows_[slot] = {};
    }

    template<class Capture, class Publish>
    bool Refresh(Capture&& capture, Publish&& publish, bool allowChange = true) noexcept {
        bool changed = false;
        for (std::size_t slot = 0; slot != rows_.size(); ++slot) {
            MonitorHostSymbolValue current{};
            if (!capture(slot, current)) current = {};
            auto& previous = rows_[slot];
            if (previous.Same(current)) continue;
            if (!changed) {
                if (!allowChange || revision_ == UINT64_MAX) {
                    if (unavailable_ != UINT64_MAX) ++unavailable_;
                    return false;
                }
                ++revision_;
                changed = true;
            }
            if (previous.Present() && (!current.Present() ||
                previous.origin.emitter != current.origin.emitter || !(previous.origin.host == current.origin.host)))
                publish(previous, VN_MONITOR_SYMBOL_TOMBSTONE);
            if (current.Present()) publish(current, VN_MONITOR_SYMBOL_UPSERT);
            previous = current;
        }
        return true;
    }

    bool Copy(std::span<MonitorHostSymbolValue> out, std::uint32_t& copied,
        std::uint64_t& trueFieldCount) const noexcept {
        copied = 0;
        trueFieldCount = 0;
        bool complete = unavailable_ == 0;
        for (const auto& row : rows_) {
            if (!row.Present()) continue;
            trueFieldCount += 3;
            if (copied == out.size()) { complete = false; continue; }
            out[copied++] = row;
        }
        return complete;
    }

private:
    std::vector<MonitorHostSymbolValue> rows_;
    std::uint64_t revision_ = 1, unavailable_ = 0;
};

} // namespace harken::monitor
#endif
