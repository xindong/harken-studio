#ifndef HARKEN_CORE_RUNTIME_WORK_INDEX_H
#define HARKEN_CORE_RUNTIME_WORK_INDEX_H

#include "harken/core/SparseRowStorage.h"

#include <array>
#include <bit>
#include <tuple>

namespace harken {

template <class Family>
struct RuntimeIndexIdentity {
    std::uint64_t generation = 0;
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t domain = 0;
    bool Valid() const noexcept { return generation != 0 && slot != UINT32_MAX; }
    bool operator==(const RuntimeIndexIdentity&) const noexcept = default;
};

struct RuntimeFrameClock {};
struct RuntimeCompletedSequenceClock {};

template <class Clock>
struct RuntimeDueTime {
    std::uint64_t value = 0;
};

// Each instance belongs to one serialized owner and one clock domain.
// It only wakes the authority; it does not decide the authority's state.
template <class Family, class Clock>
class RuntimeDueIndex {
public:
    using Identity = RuntimeIndexIdentity<Family>;
    using Time = RuntimeDueTime<Clock>;
    struct Due {
        std::uint64_t deadline = 0;
        Identity owner{};
        std::uint32_t ownerToHeapPos = UINT32_MAX;
        std::uint32_t heapPosToOwnerNode = UINT32_MAX;
    };
    static_assert(sizeof(Identity) == 16 && sizeof(Due) == 32);

    bool PrepareOwner(Identity id) noexcept {
        if (memguard::IsAudioTagged() || !id.Valid()) return false;
        if (nodes_.IsConstructed(id.slot) && nodes_[id.slot].owner.Valid())
            return nodes_[id.slot].owner == id;
        if (prepared_ == UINT32_MAX || !nodes_.ConstructPageFor(id.slot) ||
            !nodes_.ConstructThrough(prepared_ + 1u)) return false;
        auto& node = nodes_[id.slot];
        // The other column may already locate a different owner's heap entry.
        node.deadline = 0;
        node.owner = id;
        node.ownerToHeapPos = UINT32_MAX;
        ++prepared_;
        return true;
    }
    bool ReleaseOwner(Identity id) noexcept {
        if (!Prepared(id) || nodes_[id.slot].ownerToHeapPos != UINT32_MAX) return false;
        auto& node = nodes_[id.slot];
        node.owner = {};
        node.deadline = 0;
        --prepared_;
        return true;
    }
    bool Prepared(Identity id) const noexcept {
        return id.Valid() && nodes_.IsConstructed(id.slot) && nodes_[id.slot].owner == id;
    }
    bool Contains(Identity id) const noexcept {
        return Prepared(id) && nodes_[id.slot].ownerToHeapPos != UINT32_MAX;
    }
    bool Insert(Identity id, Time time) noexcept {
        if (!Prepared(id) || Contains(id) || size_ >= prepared_) return false;
        auto& node = nodes_[id.slot];
        node.deadline = time.value;
        node.ownerToHeapPos = size_;
        nodes_[size_].heapPosToOwnerNode = id.slot;
        Repair(size_++);
        return true;
    }
    bool Retime(Identity id, Time time) noexcept {
        if (!Contains(id)) return false;
        nodes_[id.slot].deadline = time.value;
        Repair(nodes_[id.slot].ownerToHeapPos);
        return true;
    }
    bool Erase(Identity id) noexcept {
        if (!Contains(id)) return false;
        auto& node = nodes_[id.slot];
        const auto position = node.ownerToHeapPos;
        const auto last = nodes_[--size_].heapPosToOwnerNode;
        if (position != size_) {
            nodes_[position].heapPosToOwnerNode = last;
            nodes_[last].ownerToHeapPos = position;
        }
        nodes_[size_].heapPosToOwnerNode = UINT32_MAX;
        node.ownerToHeapPos = UINT32_MAX;
        node.deadline = 0;
        if (position != size_) Repair(position);
        return true;
    }
    bool Peek(Identity* owner, Time* time) const noexcept {
        if (size_ == 0 || owner == nullptr || time == nullptr) return false;
        const auto& node = nodes_[nodes_[0].heapPosToOwnerNode];
        *owner = node.owner;
        *time = {node.deadline};
        return true;
    }
    bool PopDue(Time now, Identity* out,
        bool strict = std::is_same_v<Clock, RuntimeCompletedSequenceClock>) noexcept {
        Time time{};
        Identity owner{};
        if (out == nullptr || !Peek(&owner, &time) ||
            (strict ? now.value <= time.value : now.value < time.value)) return false;
        *out = owner;
        return Erase(owner);
    }
    // The visitor must not mutate this index. Waking an owner need not discharge
    // its deadline: publication can still depend on the outstanding obligation.
    template <class Visitor>
    void VisitDue(Time now, Visitor&& visitor,
        bool strict = std::is_same_v<Clock, RuntimeCompletedSequenceClock>) const noexcept {
        VisitDueAt(0, now, visitor, strict);
    }
    std::uint32_t Size() const noexcept { return size_; }
    std::uint32_t PreparedCount() const noexcept { return prepared_; }
    std::size_t RequestedBytes() const noexcept { return nodes_.RequestedBytes(); }
    std::size_t DirectoryBytes() const noexcept { return nodes_.DirectoryBytes(); }
    std::size_t CommittedBytes() const noexcept { return nodes_.CommittedBytes(); }
    std::size_t ReservedBytes() const noexcept { return nodes_.ReservedBytes(); }
    void SetCommitPageLimitForTests(std::size_t pages) noexcept {
        nodes_.SetCommitPageLimitForTests(pages);
    }

private:
    template <class Visitor>
    void VisitDueAt(std::uint64_t position, Time now, Visitor& visitor, bool strict) const noexcept {
        if (position >= size_) return;
        const auto& node = nodes_[nodes_[static_cast<std::uint32_t>(position)].heapPosToOwnerNode];
        if (strict ? now.value <= node.deadline : now.value < node.deadline) return;
        visitor(node.owner);
        VisitDueAt(position * 2u + 1u, now, visitor, strict);
        VisitDueAt(position * 2u + 2u, now, visitor, strict);
    }
    bool Less(std::uint32_t a, std::uint32_t b) const noexcept {
        const auto& x = nodes_[a];
        const auto& y = nodes_[b];
        return std::tie(x.deadline, x.owner.domain, x.owner.slot, x.owner.generation) <
            std::tie(y.deadline, y.owner.domain, y.owner.slot, y.owner.generation);
    }
    void SwapPositions(std::uint32_t a, std::uint32_t b) noexcept {
        const auto x = nodes_[a].heapPosToOwnerNode;
        const auto y = nodes_[b].heapPosToOwnerNode;
        nodes_[a].heapPosToOwnerNode = y;
        nodes_[b].heapPosToOwnerNode = x;
        nodes_[x].ownerToHeapPos = b;
        nodes_[y].ownerToHeapPos = a;
    }
    void Repair(std::uint32_t position) noexcept {
        if (position != 0 && Less(nodes_[position].heapPosToOwnerNode,
                nodes_[(position - 1u) / 2u].heapPosToOwnerNode)) {
            while (position != 0) {
                const auto parent = (position - 1u) / 2u;
                if (!Less(nodes_[position].heapPosToOwnerNode, nodes_[parent].heapPosToOwnerNode)) break;
                SwapPositions(position, parent);
                position = parent;
            }
        } else {
            while (std::uint64_t{position} * 2u + 1u < size_) {
                auto child = position * 2u + 1u;
                if (child + 1u < size_ && Less(nodes_[child + 1u].heapPosToOwnerNode,
                        nodes_[child].heapPosToOwnerNode)) ++child;
                if (!Less(nodes_[child].heapPosToOwnerNode, nodes_[position].heapPosToOwnerNode)) break;
                SwapPositions(position, child);
                position = child;
            }
        }
    }

    SparseRowStorage<Due> nodes_;
    std::uint32_t size_ = 0;
    std::atomic<std::uint32_t> prepared_{0};
};

enum class RuntimeActivitySet : std::uint8_t { Active, Ready, NextBlock, Dirty, Retiring, Count };

template <class Family>
class RuntimeActivityIndex {
public:
    using Identity = RuntimeIndexIdentity<Family>;
    bool Prepare(Identity id) noexcept {
        if (memguard::IsAudioTagged() || !id.Valid()) return false;
        const auto page = id.slot / 256u;
        if (!pages_.ConstructPageFor(page)) return false;
        auto bit = page;
        for (auto& level : summaries_) {
            bit /= 64u;
            if (!level.ConstructPageFor(bit)) return false;
        }
        return true;
    }
    // Exact validation and all membership changes run on the same authority owner.
    // Cross-owner notifications must use the authority's existing pinned handoff.
    template <class Matches>
    bool SetMembership(Identity id, RuntimeActivitySet set, bool present, Matches&& matches) noexcept {
        const auto index = static_cast<std::uint32_t>(set);
        if (!id.Valid() || index >= kSetCount || !matches(id) ||
            !Prepared(id.slot)) return false;
        const auto page = id.slot / 256u;
        auto& bitmap = pages_[page].sets[index];
        const auto word = (id.slot % 256u) / 64u;
        const auto mask = std::uint64_t{1} << (id.slot % 64u);
        if (((bitmap.leaves[word] & mask) != 0) == present) return true;
        if (present) {
            bitmap.leaves[word] |= mask;
            bitmap.summary |= std::uint64_t{1} << word;
        } else {
            bitmap.leaves[word] &= ~mask;
            if (bitmap.leaves[word] == 0) bitmap.summary &= ~(std::uint64_t{1} << word);
        }
        if (bitmap.summary != 0) pages_[page].pageDirectory |= std::uint64_t{1} << index;
        else pages_[page].pageDirectory &= ~(std::uint64_t{1} << index);
        SetPage(index, page, bitmap.summary != 0);
        return true;
    }
    template <class Matches>
    bool MarkDirty(Identity id, Matches&& matches) noexcept {
        return SetMembership(id, RuntimeActivitySet::Dirty, true, matches);
    }
    template <class Matches>
    bool Remove(Identity id, Matches&& matches) noexcept {
        if (!id.Valid() || !matches(id)) return false;
        for (std::uint32_t set = 0; set < kSetCount; ++set)
            if (!SetMembership(id, static_cast<RuntimeActivitySet>(set), false, matches)) return false;
        return true;
    }
    template <class ResolveOwner>
    Identity Next(RuntimeActivitySet set, std::uint32_t* cursor, ResolveOwner&& resolve) const noexcept {
        const auto index = static_cast<std::uint32_t>(set);
        if (cursor == nullptr || index >= kSetCount) return {};
        while (*cursor != UINT32_MAX) {
            auto page = *cursor / 256u;
            const auto start = *cursor % 256u;
            auto slot = NextInPage(index, page, start);
            if (slot == UINT32_MAX) {
                page = NextSummaryBit(index, 0, page + 1u);
                if (page == UINT32_MAX) break;
                slot = NextInPage(index, page, 0);
                assert(slot != UINT32_MAX);
            }
            if (slot == UINT32_MAX) break;
            *cursor = slot + 1u;
            const auto id = resolve(slot);
            if (id.Valid() && id.slot == slot) return id;
        }
        *cursor = UINT32_MAX;
        return {};
    }
    std::size_t RequestedBytes() const noexcept {
        auto bytes = pages_.RequestedBytes();
        for (const auto& level : summaries_) bytes += level.RequestedBytes();
        return bytes;
    }
    std::size_t DirectoryBytes() const noexcept {
        auto bytes = pages_.DirectoryBytes();
        for (const auto& level : summaries_) bytes += level.DirectoryBytes();
        return bytes;
    }
    std::size_t CommittedBytes() const noexcept {
        auto bytes = pages_.CommittedBytes();
        for (const auto& level : summaries_) bytes += level.CommittedBytes();
        return bytes;
    }
    std::size_t ReservedBytes() const noexcept {
        auto bytes = pages_.ReservedBytes();
        for (const auto& level : summaries_) bytes += level.ReservedBytes();
        return bytes;
    }

private:
    static constexpr std::uint32_t kSetCount = static_cast<std::uint32_t>(RuntimeActivitySet::Count);
    struct Bitmap256 { std::uint64_t leaves[4]{}; std::uint64_t summary = 0; };
    struct Page { Bitmap256 sets[kSetCount]; std::uint64_t pageDirectory = 0; };
    static_assert(sizeof(Page) == 208);
    bool Prepared(std::uint32_t slot) const noexcept {
        auto bit = slot / 256u;
        if (!pages_.IsConstructed(bit)) return false;
        for (std::uint32_t level = 0; level < kLevelCount; ++level) {
            if (!summaries_[level].IsConstructed(bit / 64u)) return false;
            bit /= 64u;
        }
        return true;
    }
    void SetPage(std::uint32_t set, std::uint32_t page, bool present) noexcept {
        auto bit = page;
        for (std::uint32_t level = 0; level < kLevelCount; ++level) {
            auto& word = summaries_[level][bit / 64u][set];
            const auto mask = std::uint64_t{1} << (bit % 64u);
            if (present) word |= mask;
            else word &= ~mask;
            if (!present && word != 0) break;
            bit /= 64u;
        }
    }
    std::uint32_t NextSummaryBit(std::uint32_t set, std::uint32_t level, std::uint32_t start) const noexcept {
        const auto bitCount = pageCounts_[level];
        if (start >= bitCount) return UINT32_MAX;
        auto wordIndex = start / 64u;
        const auto read = [&](std::uint32_t word) noexcept {
            const auto slot = word;
            return summaries_[level].IsConstructed(slot) ? summaries_[level][slot][set] : std::uint64_t{0};
        };
        auto bits = read(wordIndex) & (UINT64_MAX << (start % 64u));
        if (bits == 0) {
            if (level + 1u == kLevelCount) return UINT32_MAX;
            wordIndex = NextSummaryBit(set, level + 1u, wordIndex + 1u);
            if (wordIndex == UINT32_MAX) return UINT32_MAX;
            bits = read(wordIndex);
            assert(bits != 0);
        }
        return wordIndex * 64u + static_cast<std::uint32_t>(std::countr_zero(bits));
    }
    std::uint32_t NextInPage(std::uint32_t set, std::uint32_t page, std::uint32_t start) const noexcept {
        if (!pages_.IsConstructed(page)) return UINT32_MAX;
        const auto& bitmap = pages_[page].sets[set];
        auto word = start / 64u;
        auto bits = bitmap.leaves[word] & (UINT64_MAX << (start % 64u));
        if (bits == 0) {
            const auto words = bitmap.summary & (UINT64_MAX << (word + 1u));
            if (words == 0) return UINT32_MAX;
            word = static_cast<std::uint32_t>(std::countr_zero(words));
            bits = bitmap.leaves[word];
        }
        return page * 256u + word * 64u + static_cast<std::uint32_t>(std::countr_zero(bits));
    }

    SparseRowStorage<Page> pages_;
    static constexpr unsigned kLevelCount = 4;
    static constexpr std::array<std::uint32_t, kLevelCount> pageCounts_{
        1u << 24, 1u << 18, 1u << 12, 1u << 6};
    std::array<SparseRowStorage<std::array<std::uint64_t, kSetCount>>, kLevelCount> summaries_;
};

} // namespace harken

#endif // HARKEN_CORE_RUNTIME_WORK_INDEX_H
