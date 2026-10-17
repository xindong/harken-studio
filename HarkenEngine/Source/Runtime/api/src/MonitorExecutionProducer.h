#ifndef HARKEN_API_MONITOR_EXECUTION_PRODUCER_H
#define HARKEN_API_MONITOR_EXECUTION_PRODUCER_H

#include "MonitorProducerRing.h"
#include <harken/core/MonitorExecutionFact.h>
#include <atomic>
#include <cstring>

namespace harken::monitor {

inline constexpr std::uint32_t kExecutionSourceKind = 1;
inline constexpr std::uint64_t kExecutionLogicProducer = 3;
inline constexpr std::uint64_t kExecutionAudioProducer = 4;
inline constexpr std::uint64_t kExecutionLogicStream = 201;
inline constexpr std::uint64_t kExecutionAudioStream = 202;

// One tracker per existing SPSC ring. The producer publishes a contiguous
// attempt sequence after either a completed source copy or a declared drop.
// Only this private sequence can prove gaps: filtered public sequences cannot.
template<class Value>
class MonitorValueProducer {
public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    // Reset is only legal while the producer and drain are both stopped.
    void Reset(std::uint64_t lastSequence = 0) noexcept {
        next_ = lastSequence;
        published_.store(lastSequence, std::memory_order_relaxed);
        completed_.store(lastSequence, std::memory_order_relaxed);
        unavailable_.store(0, std::memory_order_relaxed);
        pending_ = {};
        hasPending_ = false;
    }

    bool Publish(MonitorProducerRing& ring, Value fact, std::uint64_t engine) noexcept {
        if (next_ == UINT64_MAX || engine == 0 || fact.engine != engine) {
            const auto count = unavailable_.load(std::memory_order_relaxed);
            if (count != UINT64_MAX) unavailable_.store(count + 1, std::memory_order_relaxed);
            return false;
        }
        fact.sourceSequence = ++next_;
        const bool pushed = ring.TryPush(reinterpret_cast<const std::uint8_t*>(&fact), sizeof(fact), kExecutionSourceKind);
        // A consumer may see the slot before this release. It must retain that
        // one pending value until Published reaches its sequence before fanout.
        published_.store(next_, std::memory_order_release);
        return pushed;
    }

    std::uint64_t Published() const noexcept { return published_.load(std::memory_order_acquire); }
    std::uint64_t Completed() const noexcept { return completed_.load(std::memory_order_acquire); }
    std::uint64_t Unavailable() const noexcept { return unavailable_.load(std::memory_order_relaxed); }

    // An observed fact could not be retained before reaching the source ring.
    // Reserve its source sequence so the drain reports the gap through the same fence.
    void DeclareDrop(std::uint64_t engine) noexcept {
        if (next_ == UINT64_MAX || engine == 0) {
            const auto count = unavailable_.load(std::memory_order_relaxed);
            if (count != UINT64_MAX) unavailable_.store(count + 1, std::memory_order_relaxed);
            return;
        }
        published_.store(++next_, std::memory_order_release);
    }

    bool HasPending() const noexcept { return hasPending_; }

    bool Stage(const std::uint8_t* bytes, std::uint32_t size) noexcept {
        if (hasPending_ || bytes == nullptr || size != sizeof(pending_)) return false;
        std::memcpy(&pending_, bytes, sizeof(pending_));
        hasPending_ = true;
        return true;
    }

    // Callbacks run on the non-realtime drain thread. Completion is published
    // only after projection/fanout or explicit loss accounting has finished.
    template<class FactFn, class DropFn>
    bool DispatchPending(FactFn&& fact, DropFn&& drop) {
        if (!hasPending_ || pending_.sourceSequence > Published()) return false;
        const auto previous = Completed();
        if (pending_.sourceSequence <= previous) return false;
        if (pending_.sourceSequence - previous > 1) drop(previous + 1, pending_.sourceSequence - 1);
        if (!fact(pending_)) drop(pending_.sourceSequence, pending_.sourceSequence);
        completed_.store(pending_.sourceSequence, std::memory_order_release);
        hasPending_ = false;
        return true;
    }

    // The caller takes the watermark BEFORE an empty ring read. That acquire
    // makes every successful slot through the watermark visible; remaining
    // attempts in this interval are therefore known failed pushes.
    template<class DropFn>
    bool FinishEmpty(std::uint64_t publishedBeforeEmptyRead, DropFn&& drop) {
        if (hasPending_) return false;
        const auto previous = Completed();
        if (publishedBeforeEmptyRead <= previous) return false;
        drop(previous + 1, publishedBeforeEmptyRead);
        completed_.store(publishedBeforeEmptyRead, std::memory_order_release);
        return true;
    }

private:
    std::uint64_t next_ = 0;
    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> unavailable_{0};
    Value pending_{};
    bool hasPending_ = false;
};

using MonitorExecutionProducer = MonitorValueProducer<MonitorExecutionFact>;

} // namespace harken::monitor

#endif
