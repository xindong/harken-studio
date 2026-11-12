#ifndef HARKEN_API_MONITOR_IDENTITY_STATE_H
#define HARKEN_API_MONITOR_IDENTITY_STATE_H

#include "MonitorIdentityCodec.h"
#include "MonitorExecutionProducer.h"
#include <span>
#include <vector>

namespace harken::monitor {

struct MonitorIdentityDelta {
    std::uint64_t engine = 0;
    std::uint64_t sourceSequence = 0;
    std::uint64_t revision = 0;
    MonitorExecutionFact fact{};
};
using MonitorIdentityProducer = MonitorValueProducer<MonitorIdentityDelta>;
static_assert(std::is_trivially_copyable_v<MonitorIdentityDelta>);
static_assert(sizeof(MonitorIdentityDelta) <= 416);

inline bool SameIdentityValue(const MonitorExecutionFact& left, const MonitorExecutionFact& right) noexcept {
    if (left.engine == 0 || right.engine == 0) return left.engine == right.engine;
    // Equal object representations are a fast proof; unequal padding must not
    // generate spurious updates, so compare canonical bytes on that path.
    if (std::memcmp(&left, &right, sizeof(left)) == 0) return true;
    auto a = left;
    auto b = right;
    a.sourceSequence = b.sourceSequence = 1;
    a.timestamp = b.timestamp = 0;
    SetSourceField(a, VN_MONITOR_FACT_FIELD_TIMESTAMP, false);
    SetSourceField(b, VN_MONITOR_FACT_FIELD_TIMESTAMP, false);
    std::uint8_t first[kIdentityRecordBytes]{}, second[kIdentityRecordBytes]{};
    return PackIdentityFact(first, sizeof(first), a, 0, 1) == sizeof(first) &&
        PackIdentityFact(second, sizeof(second), b, 0, 1) == sizeof(second) &&
        std::memcmp(first, second, sizeof(first)) == 0;
}

// Logic-owned diagnostic cache. It neither retains nor changes native owners.
// Reset allocates at resource setup; Refresh and Copy perform bounded copies.
class MonitorIdentityMirror {
public:
    void Reset(std::size_t capacity) {
        rows_.assign(capacity, {});
        revision_ = 1;
        unavailable_ = 0;
    }
    void Clear() noexcept {
        std::vector<MonitorExecutionFact>().swap(rows_);
        revision_ = 1;
        unavailable_ = 0;
    }
    std::size_t Capacity() const noexcept { return rows_.size(); }
    std::uint64_t StorageBytes() const noexcept { return rows_.size() * sizeof(MonitorExecutionFact); }
    std::uint64_t Revision() const noexcept { return revision_; }
    std::uint64_t Unavailable() const noexcept { return unavailable_; }

    template<class Capture, class Publish>
    bool Refresh(Capture&& capture, Publish&& publish, bool allowChange = true) noexcept {
        bool changed = false;
        for (std::size_t slot = 0; slot != rows_.size(); ++slot) {
            MonitorExecutionFact current{};
            if (!capture(slot, current)) current = {};
            auto& previous = rows_[slot];
            if (SameIdentityValue(previous, current)) continue;
            if (!changed) {
                if (!allowChange || revision_ == UINT64_MAX) {
                    if (unavailable_ != UINT64_MAX) ++unavailable_;
                    return false;
                }
                ++revision_;
                changed = true;
            }
            if (previous.engine != 0 && (current.engine == 0 || current.engine != previous.engine ||
                current.subjectNamespace != previous.subjectNamespace ||
                current.body.execution.identity.handle != previous.body.execution.identity.handle)) {
                auto removed = previous;
                removed.kind = VN_MONITOR_FACT_IDENTITY_TOMBSTONE;
                publish(removed, revision_);
            }
            if (current.engine != 0) publish(current, revision_);
            // A failed publication is accounted by the producer. Re-emitting
            // it on a later unchanged boundary would invent another change.
            previous = current;
        }
        return true;
    }

    bool Copy(std::span<MonitorExecutionFact> out, std::uint32_t& copied,
        std::uint64_t (&counts)[9]) const noexcept {
        copied = 0;
        std::fill(std::begin(counts), std::end(counts), 0);
        bool complete = unavailable_ == 0;
        for (const auto& row : rows_) {
            if (row.engine == 0) continue;
            if (!IdentityNamespace(row.subjectNamespace)) { complete = false; continue; }
            ++counts[row.subjectNamespace];
            if (copied == out.size()) { complete = false; continue; }
            out[copied++] = row;
            complete &= (row.flags & kMonitorFactIdentityIncomplete) == 0;
        }
        return complete;
    }

private:
    std::vector<MonitorExecutionFact> rows_;
    std::uint64_t revision_ = 1;
    std::uint64_t unavailable_ = 0;
};

} // namespace harken::monitor

#endif
