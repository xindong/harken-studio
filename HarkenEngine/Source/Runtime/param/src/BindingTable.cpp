// Definition registry and attachment pool of the public binding system.
// Every entry validates exhaustively
// before touching pool state; replaces stage and install at the fixed
// update phase; bus attachments evaluate into the public Variable slot of the
// Variable slot with zero audio-thread involvement.
#include "harken/core/LogicAllocCheckpoint.h"
#include <harken/core/BindingTransition.h>
#include "harken/param/BindingTable.h"

#include <harken/Harken.h>

#include "harken/param/EffectParamLocator.h"
#include "harken/render/generated/harken_effect_registry.g.h"

#include <bit>
#include <cmath>
#include <algorithm>
#include <tuple>

namespace harken {

namespace {

// One pool-exhaustion log record naming which pool refused (the
// no-binding refusal's contract: the remedy must be unambiguous).
void LogPoolExhausted(LogRing* log, std::uint64_t pool,
                      std::uint64_t capacity) {
    if (log == nullptr) {
        return;
    }
    LogRecord record{};
    record.level = VN_LOG_WARN;
    record.code = kLogCodeBindingPoolExhausted;
    record.arg0 = pool;
    record.arg1 = capacity;
    log->Push(record);
}

} // namespace

namespace {
auto DependencyOrder(BindingDependencyKind kind, const BindingDependencyIdentity& value) noexcept {
    return std::tuple{kind, value.source.high, value.source.low, value.object, value.scope,
        value.target, value.generation, value.detail};
}
}

BindingDependencyIndex::BindingDependencyIndex(const BindingDependencyIndex& other) {
    if (!Reset(0, 0) ||
        !inputs_.ConstructThrough(other.inputs_.ConstructedCount()) ||
        !keys_.ConstructThrough(other.keys_.ConstructedCount()) ||
        !targets_.ConstructThrough(other.targets_.ConstructedCount())) throw harken::detail::LogicPreparationFailure{};
    std::copy(other.inputs_.begin(), other.inputs_.end(), inputs_.begin());
    std::copy(other.keys_.begin(), other.keys_.end(), keys_.begin());
    std::copy(other.targets_.begin(), other.targets_.end(), targets_.begin());
    freeKeys_ = other.freeKeys_;
    freeTargets_ = other.freeTargets_;
    dirtyTargets_ = other.dirtyTargets_;
    freeKeys_.reserve(keys_.ConstructedCount());
    freeTargets_.reserve(targets_.ConstructedCount());
    dirtyTargets_.reserve(targets_.ConstructedCount());
    preparedBindings_ = other.preparedBindings_;
    preparedBindingCount_ = other.preparedBindingCount_;
    slotCount_ = other.slotCount_;
    freeSlots_ = other.freeSlots_;
    freeSlots_.reserve(other.inputs_.ConstructedCount() / 3u);
    root_ = other.root_;
    version_ = other.version_;
}

bool BindingDependencyIndex::Reset(std::uint32_t bindingCapacity, std::uint32_t targetCapacity,
    std::uint32_t keyCapacity) noexcept {
    if (memguard::IsAudioTagged() || root_ != UINT32_MAX || preparedBindingCount_ != 0 ||
        inputs_.HasPreparedBacking() || keys_.HasPreparedBacking() || targets_.HasPreparedBacking() ||
        bindingCapacity > UINT32_MAX / 3u) return false;
    const auto needed = std::uint64_t{bindingCapacity} * 3u + targetCapacity;
    if (keyCapacity == 0 && needed > UINT32_MAX) return false;
    if (keyCapacity == 0) keyCapacity = static_cast<std::uint32_t>(needed);
    GrowingRowStorage<InputNode> inputs;
    GrowingRowStorage<Key> keys;
    GrowingRowStorage<DirtyRow> targets;
    inputs_.Swap(inputs);
    keys_.Swap(keys);
    targets_.Swap(targets);
    std::vector<std::uint32_t>().swap(freeKeys_);
    std::vector<std::uint32_t>().swap(freeTargets_);
    std::vector<std::uint32_t>().swap(dirtyTargets_);
    std::vector<std::uint32_t>().swap(freeSlots_);
    std::vector<std::uint64_t>().swap(preparedBindings_);
    preparedBindingCount_ = 0;
    slotCount_ = 0;
    version_ = 0;
    return true;
}

bool BindingDependencyIndex::PrepareInputs(std::uint32_t count, std::uint32_t occupied,
                                          StableStoragePreparation* backing) noexcept {
    const auto metadata = [this](std::uint32_t required) noexcept {
        try {
            freeSlots_.reserve(required / 3u);
            const auto words = (std::uint64_t{required / 3u} + 63u) / 64u;
            if (preparedBindings_.size() < words) preparedBindings_.resize(static_cast<std::size_t>(words), 0);
            return true;
        } catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::BindingStorage); return false; }
    };
    return backing != nullptr ? inputs_.PrepareBacking(count, occupied, backing, metadata)
                              : inputs_.ConstructThrough(count, metadata);
}

bool BindingDependencyIndex::PrepareKeys(std::uint32_t count, std::uint32_t occupied,
                                        StableStoragePreparation* backing) noexcept {
    const auto previous = keys_.ConstructedCount();
    const auto metadata = [this](std::uint32_t required) noexcept {
        try { freeKeys_.reserve(required); return true; }
        catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::BindingStorage); return false; }
    };
    const bool ready = backing != nullptr ? keys_.PrepareBacking(count, occupied, backing, metadata)
                                          : keys_.ConstructThrough(count, metadata);
    if (!ready) return false;
    for (auto next = previous; next < keys_.ConstructedCount(); ++next) freeKeys_.push_back(next);
    return true;
}

bool BindingDependencyIndex::PrepareTargets(std::uint32_t count, std::uint32_t occupied,
                                           StableStoragePreparation* backing) noexcept {
    const auto previous = targets_.ConstructedCount();
    const auto metadata = [this](std::uint32_t required) noexcept {
        try { freeTargets_.reserve(required); dirtyTargets_.reserve(required); return true; }
        catch (...) { harken::detail::RecordCurrentLogicAllocFailure(harken::detail::LogicAllocSite::BindingStorage); return false; }
    };
    const bool ready = backing != nullptr ? targets_.PrepareBacking(count, occupied, backing, metadata)
                                          : targets_.ConstructThrough(count, metadata);
    if (!ready) return false;
    for (auto next = previous; next < targets_.ConstructedCount(); ++next) freeTargets_.push_back(next);
    return true;
}

bool BindingDependencyIndex::PrepareBacking(std::uint32_t count, BackingPreparation* out) noexcept {
    if (out == nullptr || memguard::IsAudioTagged() ||
        std::any_of(out->begin(), out->end(), [](const auto& hold) { return hold.Valid(); })) return false;
    if (count > UINT32_MAX / 4u) return false;
    auto occupiedInputs = inputs_.ConstructedCount() / 3u;
    while (occupiedInputs != 0 && inputs_[(occupiedInputs - 1u) * 3u].fact == nullptr &&
           !HasBindingPreparation(occupiedInputs - 1u)) --occupiedInputs;
    occupiedInputs = std::max(occupiedInputs, slotCount_);
    const auto occupiedKeys = static_cast<std::uint32_t>(keys_.ConstructedCount() - freeKeys_.size()) +
        preparedBindingCount_ * 4u;
    const auto occupiedTargets = static_cast<std::uint32_t>(targets_.ConstructedCount() - freeTargets_.size()) +
        preparedBindingCount_;
    BackingPreparation candidate;
    if (!PrepareInputs(count * 3u, occupiedInputs * 3u, &candidate[0]) ||
        !PrepareKeys(count * 4u, occupiedKeys, &candidate[1]) ||
        !PrepareTargets(count, occupiedTargets, &candidate[2])) return false;
    *out = std::move(candidate);
    return true;
}

BindingDependencyIndex::BackingUse BindingDependencyIndex::UseBacking(const BackingPreparation* backing) noexcept {
    return {backing == nullptr ? StableStoragePreparedUse{} : (*backing)[0].Use(),
            backing == nullptr ? StableStoragePreparedUse{} : (*backing)[1].Use(),
            backing == nullptr ? StableStoragePreparedUse{} : (*backing)[2].Use()};
}

bool BindingDependencyIndex::PrepareBinding(std::uint32_t slot) noexcept {
    if (memguard::IsAudioTagged() || slot >= UINT32_MAX / 3u ||
        !PrepareInputs((slot + 1u) * 3u, 0)) return false;
    if (HasBindingPreparation(slot)) return true;
    if (inputs_[slot * 3u].generation == UINT32_MAX) return false;
    const auto keyNeed = std::uint64_t{keys_.ConstructedCount() - freeKeys_.size()} +
        std::uint64_t{preparedBindingCount_ + 1u} * 4u;
    const auto targetNeed = std::uint64_t{targets_.ConstructedCount() - freeTargets_.size()} +
        preparedBindingCount_ + 1u;
    if (keyNeed > UINT32_MAX || targetNeed > UINT32_MAX ||
        !PrepareKeys(static_cast<std::uint32_t>(keyNeed), 0) ||
        !PrepareTargets(static_cast<std::uint32_t>(targetNeed), 0)) return false;
    preparedBindings_[slot / 64u] |= std::uint64_t{1} << (slot % 64u);
    ++preparedBindingCount_;
    return true;
}

bool BindingDependencyIndex::AllocateBindingSlot(std::uint32_t* out) noexcept {
    if (out == nullptr || memguard::IsAudioTagged()) return false;
    const bool append = freeSlots_.empty();
    const auto slot = append ? slotCount_ : freeSlots_.back();
    if (!PrepareBinding(slot)) return false;
    if (append) ++slotCount_;
    else freeSlots_.pop_back();
    *out = slot;
    return true;
}

void BindingDependencyIndex::ReleaseBindingSlot(std::uint32_t slot) noexcept {
    if (slot == UINT32_MAX) return;
    assert(slot < slotCount_ && inputs_[slot * 3u].fact == nullptr);
    CancelBindingPreparation(slot);
    // Exhausted generations are retired permanently, never recycled.
    if (inputs_[slot * 3u].generation != UINT32_MAX) freeSlots_.push_back(slot);
}

bool BindingDependencyIndex::HasBindingPreparation(std::uint32_t slot) const noexcept {
    return slot / 64u < preparedBindings_.size() &&
        (preparedBindings_[slot / 64u] & (std::uint64_t{1} << (slot % 64u))) != 0;
}

void BindingDependencyIndex::CancelBindingPreparation(std::uint32_t slot) noexcept {
    if (!HasBindingPreparation(slot)) return;
    preparedBindings_[slot / 64u] &= ~(std::uint64_t{1} << (slot % 64u));
    assert(preparedBindingCount_ != 0);
    --preparedBindingCount_;
}

const BindingDependencyIndex::InputNode* BindingDependencyIndex::Resolve(BindingDependencyToken token) const noexcept {
    if (!token.Valid() || token.slot >= inputs_.ConstructedCount() / 3u) return nullptr;
    const auto& node = inputs_[token.slot * 3u];
    return node.fact != nullptr && node.generation == token.generation ? &node : nullptr;
}

const BindingDependencyIndex::DirtyRow* BindingDependencyIndex::Resolve(BindingDirtyTarget target) const noexcept {
    if (!target.Valid() || target.slot >= targets_.ConstructedCount()) return nullptr;
    const auto& row = targets_[target.slot];
    return row.key != UINT32_MAX && row.generation == target.generation ? &row : nullptr;
}

BindingDependencyKind BindingDependencyIndex::Kind(const Key& key) noexcept {
    return static_cast<BindingDependencyKind>(key.kindHeight >> 24);
}

BindingDependencyIdentity BindingDependencyIndex::Identity(const Key& key) const noexcept {
    return key.count == 0 ? key.retired : key.locator.read(key.locator.fact, Kind(key));
}

std::uint32_t BindingDependencyIndex::Find(BindingDependencyKind kind,
    const BindingDependencyIdentity& identity) const noexcept {
    auto key = root_;
    const auto wanted = DependencyOrder(kind, identity);
    while (key != UINT32_MAX) {
        const auto& row = keys_[key];
        const auto current = DependencyOrder(Kind(row), Identity(row));
        if (wanted == current) return key;
        key = wanted < current ? row.left : row.right;
    }
    return UINT32_MAX;
}

std::uint32_t BindingDependencyIndex::Height(std::uint32_t key) const noexcept {
    return key == UINT32_MAX ? 0 : keys_[key].kindHeight & 0x00ffffffu;
}

void BindingDependencyIndex::UpdateHeight(std::uint32_t key) noexcept {
    auto& row = keys_[key];
    row.kindHeight = (row.kindHeight & 0xff000000u) | (1u + std::max(Height(row.left), Height(row.right)));
}

std::uint32_t BindingDependencyIndex::Rotate(std::uint32_t key, bool left) noexcept {
    auto& row = keys_[key];
    const auto child = left ? row.right : row.left;
    auto& other = keys_[child];
    const auto middle = left ? other.left : other.right;
    if (left) { row.right = middle; other.left = key; }
    else { row.left = middle; other.right = key; }
    if (middle != UINT32_MAX) keys_[middle].parent = key;
    other.parent = row.parent;
    if (row.parent == UINT32_MAX) root_ = child;
    else if (keys_[row.parent].left == key) keys_[row.parent].left = child;
    else keys_[row.parent].right = child;
    row.parent = child;
    UpdateHeight(key);
    UpdateHeight(child);
    return child;
}

void BindingDependencyIndex::Rebalance(std::uint32_t key) noexcept {
    while (key != UINT32_MAX) {
        UpdateHeight(key);
        auto& row = keys_[key];
        if (Height(row.left) > Height(row.right) + 1u) {
            if (Height(keys_[row.left].right) > Height(keys_[row.left].left)) (void)Rotate(row.left, true);
            key = Rotate(key, false);
        } else if (Height(row.right) > Height(row.left) + 1u) {
            if (Height(keys_[row.right].left) > Height(keys_[row.right].right)) (void)Rotate(row.right, false);
            key = Rotate(key, true);
        }
        key = keys_[key].parent;
    }
}

std::uint32_t BindingDependencyIndex::EnsureKey(BindingDependencyKind kind, std::uint32_t node) noexcept {
    const auto& input = inputs_[node];
    const auto identity = input.read(input.fact, kind);
    auto current = root_;
    auto parent = UINT32_MAX;
    const auto wanted = DependencyOrder(kind, identity);
    bool left = false;
    while (current != UINT32_MAX) {
        const auto& row = keys_[current];
        const auto candidate = DependencyOrder(Kind(row), Identity(row));
        if (wanted == candidate) return current;
        parent = current;
        left = wanted < candidate;
        current = left ? row.left : row.right;
    }
    assert(!freeKeys_.empty());
    const auto slot = freeKeys_.back();
    freeKeys_.pop_back();
    auto& key = keys_[slot];
    key = Key{};
    key.retired = identity;
    key.kindHeight = (static_cast<std::uint32_t>(kind) << 24) | 1u;
    key.parent = parent;
    if (parent == UINT32_MAX) root_ = slot;
    else if (left) keys_[parent].left = slot;
    else keys_[parent].right = slot;
    Rebalance(parent);
    return slot;
}

void BindingDependencyIndex::Reanchor(Key& key, std::uint32_t node) noexcept {
    const auto& input = inputs_[node];
    key.locator.fact = input.fact;
    key.locator.read = input.read;
    key.locator.factVersion = input.generation;
    key.locator.representative = node;
    key.locator.order = input.order;
}

void BindingDependencyIndex::LinkSource(std::uint32_t node, std::uint32_t key) noexcept {
    auto& row = keys_[key];
    auto& input = inputs_[node];
    input.key = key;
    input.nextSource = row.head;
    row.head = node;
    if (row.count++ == 0) { row.locator = Locator{}; Reanchor(row, node); }
}

void BindingDependencyIndex::EraseKey(std::uint32_t key) noexcept {
    // Rotate this exact row down; no surviving key or borrowed fact moves.
    while (keys_[key].left != UINT32_MAX && keys_[key].right != UINT32_MAX)
        (void)Rotate(key, Height(keys_[key].right) > Height(keys_[key].left));
    const auto parent = keys_[key].parent;
    const auto child = keys_[key].left == UINT32_MAX ? keys_[key].right : keys_[key].left;
    if (child != UINT32_MAX) keys_[child].parent = parent;
    if (parent == UINT32_MAX) root_ = child;
    else if (keys_[parent].left == key) keys_[parent].left = child;
    else keys_[parent].right = child;
    keys_[key] = Key{};
    freeKeys_.push_back(key);
    Rebalance(parent);
}

void BindingDependencyIndex::UnlinkSource(std::uint32_t node) noexcept {
    const auto keyIndex = inputs_[node].key;
    if (keyIndex == UINT32_MAX) return;
    auto& key = keys_[keyIndex];
    auto* link = &key.head;
    while (*link != node) { assert(*link != UINT32_MAX); link = &inputs_[*link].nextSource; }
    *link = inputs_[node].nextSource;
    if (--key.count == 0) EraseKey(keyIndex);
    else if (key.locator.representative == node) Reanchor(key, key.head);
    inputs_[node].key = UINT32_MAX;
}

bool BindingDependencyIndex::Register(std::uint32_t slot, const void* fact, ReadIdentity read,
    std::uint32_t order, bool local, bool liveDefinition, bool probe, BindingDependencyToken* out) noexcept {
    if (out == nullptr || out->Valid() || fact == nullptr || read == nullptr ||
        slot >= inputs_.ConstructedCount() / 3u || !HasBindingPreparation(slot)) return false;
    auto& primary = inputs_[slot * 3u];
    if (primary.fact != nullptr || primary.generation == UINT32_MAX) return false;
    std::size_t needed = 0;
    const auto targetFound = Find(BindingDependencyKind::Target, read(fact, BindingDependencyKind::Target));
    needed += targetFound == UINT32_MAX;
    const auto inputKind = probe ? BindingDependencyKind::Probe : BindingDependencyKind::Global;
    const auto inputFound = Find(inputKind, read(fact, inputKind));
    const auto localFound = local && !probe
        ? Find(BindingDependencyKind::Local, read(fact, BindingDependencyKind::Local)) : UINT32_MAX;
    const auto definitionFound = liveDefinition
        ? Find(BindingDependencyKind::Definition, read(fact, BindingDependencyKind::Definition)) : UINT32_MAX;
    needed += inputFound == UINT32_MAX;
    if (local && !probe) needed += localFound == UINT32_MAX;
    if (liveDefinition) needed += definitionFound == UINT32_MAX;
    if (freeKeys_.size() < needed || (targetFound == UINT32_MAX && freeTargets_.empty())) return false;
    const auto generation = primary.generation;
    for (std::uint32_t i = 0; i < 3; ++i) {
        auto& node = inputs_[slot * 3u + i];
        node = InputNode{};
        node.fact = fact;
        node.read = read;
        node.generation = generation;
        node.order = order;
    }
    // Key slots remain stable across insertions and tree rotations.
    const auto target = targetFound != UINT32_MAX ? targetFound
        : EnsureKey(BindingDependencyKind::Target, slot * 3u);
    auto& key = keys_[target];
    auto targetRow = UINT32_MAX;
    if (key.count != 0) targetRow = key.locator.targetRow;
    else {
        for (std::uint32_t index = 0; index < targets_.ConstructedCount(); ++index)
            if (targets_[index].key == target) { targetRow = index; break; }
        if (targetRow == UINT32_MAX) {
            targetRow = freeTargets_.back();
            freeTargets_.pop_back();
            targets_[targetRow].key = target;
        }
    }
    auto* link = &key.head;
    while (*link != UINT32_MAX && std::tuple{inputs_[*link].order, *link} < std::tuple{order, slot * 3u})
        link = &inputs_[*link].nextTarget;
    primary.nextTarget = *link;
    *link = slot * 3u;
    if (key.count++ == 0) { key.locator = Locator{}; Reanchor(key, slot * 3u); }
    key.locator.targetRow = targetRow;
    for (std::uint32_t i = 0; i < 3; ++i) inputs_[slot * 3u + i].target = target;
    LinkSource(slot * 3u, inputFound != UINT32_MAX ? inputFound : EnsureKey(inputKind, slot * 3u));
    if (local && !probe) LinkSource(slot * 3u + 1u, localFound != UINT32_MAX ? localFound
        : EnsureKey(BindingDependencyKind::Local, slot * 3u + 1u));
    if (liveDefinition) LinkSource(slot * 3u + 2u, definitionFound != UINT32_MAX ? definitionFound
        : EnsureKey(BindingDependencyKind::Definition, slot * 3u + 2u));
    *out = {slot, generation};
    CancelBindingPreparation(slot);
    MarkTarget(target);
    return true;
}

bool BindingDependencyIndex::Relocate(BindingDependencyToken token, const void* fact, ReadIdentity read,
                                      std::uint32_t currentOrder) noexcept {
    if (Resolve(token) == nullptr || fact == nullptr || read == nullptr) return false;
    const auto primaryIndex = token.slot * 3u;
    auto& primary = inputs_[primaryIndex];
    auto& target = keys_[primary.target];
    const bool reordered = currentOrder != UINT32_MAX && currentOrder != primary.order;
    if (reordered) {
        auto* link = &target.head;
        while (*link != primaryIndex) {
            assert(*link != UINT32_MAX);
            link = &inputs_[*link].nextTarget;
        }
        *link = primary.nextTarget;
    }
    for (std::uint32_t i = 0; i < 3; ++i) {
        const auto nodeIndex = primaryIndex + i;
        auto& node = inputs_[nodeIndex];
        node.fact = fact;
        node.read = read;
        if (reordered) node.order = currentOrder;
        if (node.key != UINT32_MAX && keys_[node.key].locator.representative == nodeIndex)
            Reanchor(keys_[node.key], nodeIndex);
    }
    if (reordered) {
        auto* link = &target.head;
        while (*link != UINT32_MAX &&
               std::tuple{inputs_[*link].order, *link} < std::tuple{currentOrder, primaryIndex})
            link = &inputs_[*link].nextTarget;
        primary.nextTarget = *link;
        *link = primaryIndex;
    }
    if (target.locator.representative == primaryIndex) Reanchor(target, primaryIndex);
    if (reordered) MarkTarget(primary.target);
    return true;
}

bool BindingDependencyIndex::DetachLocal(BindingDependencyToken token) noexcept {
    const auto* primary = Resolve(token);
    if (primary == nullptr) return false;
    UnlinkSource(token.slot * 3u + 1u);
    MarkTarget(primary->target);
    return true;
}

void BindingDependencyIndex::MarkTarget(std::uint32_t key) noexcept {
    auto row = keys_[key].count == 0 ? UINT32_MAX : keys_[key].locator.targetRow;
    if (row == UINT32_MAX)
        for (std::uint32_t i = 0; i < targets_.ConstructedCount(); ++i)
            if (targets_[i].key == key) { row = i; break; }
    assert(row != UINT32_MAX);
    auto& target = targets_[row];
    if (target.version == 0) {
        const auto identity = DependencyOrder(BindingDependencyKind::Target, Identity(keys_[key]));
        const auto it = std::lower_bound(dirtyTargets_.begin(), dirtyTargets_.end(), identity,
            [&](std::uint32_t value, const auto& wanted) {
                return DependencyOrder(BindingDependencyKind::Target, Identity(keys_[targets_[value].key])) < wanted;
            });
        dirtyTargets_.insert(it, row);
        for (std::uint32_t i = 0; i < dirtyTargets_.size(); ++i) targets_[dirtyTargets_[i]].membership = i;
    }
    if (version_ != UINT64_MAX) ++version_;
    target.version = version_;
}

bool BindingDependencyIndex::Remove(BindingDependencyToken token) noexcept {
    const auto* primary = Resolve(token);
    if (primary == nullptr) return false;
    const auto targetIndex = primary->target;
    auto& key = keys_[targetIndex];
    const auto identity = Identity(key);
    MarkTarget(targetIndex);
    auto* link = &key.head;
    while (*link != token.slot * 3u) { assert(*link != UINT32_MAX); link = &inputs_[*link].nextTarget; }
    *link = primary->nextTarget;
    if (--key.count == 0) key.retired = identity;
    else if (key.locator.representative == token.slot * 3u) Reanchor(key, key.head);
    for (std::uint32_t i = 0; i < 3; ++i) {
        const auto node = token.slot * 3u + i;
        UnlinkSource(node);
        inputs_[node] = InputNode{};
        inputs_[node].generation = token.generation + (token.generation != UINT32_MAX);
    }
    return true;
}

void BindingDependencyIndex::Notify(BindingDependencyKind kind, const BindingDependencyIdentity& identity) noexcept {
    const auto key = Find(kind, identity);
    if (key == UINT32_MAX) return;
    for (auto node = keys_[key].head; node != UINT32_MAX; node = inputs_[node].nextSource)
        MarkTarget(inputs_[node].target);
}

void BindingDependencyIndex::NotifyVariable(VnVariableId variable, VnEmitterHandle emitter) noexcept {
    BindingDependencyIdentity identity{};
    identity.source = variable;
    identity.object = emitter;
    Notify(emitter == 0 ? BindingDependencyKind::Global : BindingDependencyKind::Local, identity);
}

void BindingDependencyIndex::NotifyDefinition(BindingId definition) noexcept {
    BindingDependencyIdentity identity{};
    identity.source = definition;
    Notify(BindingDependencyKind::Definition, identity);
}

void BindingDependencyIndex::Mark(BindingDependencyToken token) noexcept {
    if (const auto* node = Resolve(token)) MarkTarget(node->target);
}

bool BindingDependencyIndex::TargetOf(BindingDependencyToken token, BindingDirtyTarget* out) const noexcept {
    const auto* node = Resolve(token);
    if (node == nullptr || out == nullptr) return false;
    const auto row = keys_[node->target].locator.targetRow;
    *out = {row, targets_[row].generation};
    return true;
}

std::uint64_t BindingDependencyIndex::TargetVersion(BindingDirtyTarget target) const noexcept {
    const auto* row = Resolve(target);
    return row == nullptr ? 0 : row->version;
}

bool BindingDependencyIndex::DirtyTargetAt(std::uint32_t index, BindingDirtyTarget* out) const noexcept {
    if (out == nullptr || index >= dirtyTargets_.size()) return false;
    const auto row = dirtyTargets_[index];
    *out = {row, targets_[row].generation};
    return true;
}

bool BindingDependencyIndex::TargetIdentity(BindingDirtyTarget target, BindingDependencyIdentity* out) const noexcept {
    const auto* row = Resolve(target);
    if (row == nullptr || out == nullptr) return false;
    *out = Identity(keys_[row->key]);
    return true;
}

BindingDependencyToken BindingDependencyIndex::FirstContribution(BindingDirtyTarget target) const noexcept {
    const auto* row = Resolve(target);
    if (row == nullptr) return {};
    const auto node = keys_[row->key].head;
    return node == UINT32_MAX ? BindingDependencyToken{} : BindingDependencyToken{node / 3u, inputs_[node].generation};
}

BindingDependencyToken BindingDependencyIndex::NextContribution(BindingDependencyToken token) const noexcept {
    const auto* row = Resolve(token);
    if (row == nullptr || row->nextTarget == UINT32_MAX) return {};
    const auto node = row->nextTarget;
    return {node / 3u, inputs_[node].generation};
}

const void* BindingDependencyIndex::Fact(BindingDependencyToken token) const noexcept {
    const auto* row = Resolve(token);
    return row == nullptr ? nullptr : row->fact;
}

bool BindingDependencyIndex::CommitTarget(BindingDirtyTarget target, std::uint64_t version) noexcept {
    const auto* resolved = Resolve(target);
    if (resolved == nullptr || version == 0 || version == UINT64_MAX || resolved->version != version) return false;
    auto& row = targets_[target.slot];
    dirtyTargets_.erase(dirtyTargets_.begin() + row.membership);
    for (std::uint32_t i = 0; i < dirtyTargets_.size(); ++i) targets_[dirtyTargets_[i]].membership = i;
    row.version = 0;
    row.membership = UINT32_MAX;
    if (keys_[row.key].count == 0) {
        EraseKey(row.key);
        row.key = UINT32_MAX;
        if (row.generation != UINT64_MAX) { ++row.generation; freeTargets_.push_back(target.slot); }
    }
    return true;
}

bool BindingDependencyIndex::HasBusPublicationTarget(BusPublicationTarget target) const noexcept {
    const auto key = Find(BindingDependencyKind::Target, target.Identity());
    return key != UINT32_MAX && keys_[key].count != 0;
}

std::uint32_t BindingDependencyIndex::BusPublicationTargetCount() const noexcept {
    std::uint32_t count = 0;
    for (const auto& key : keys_) {
        if (key.count == 0 || Kind(key) != BindingDependencyKind::Target) continue;
        const auto identity = Identity(key);
        if (identity.scope == kTargetScopeBus &&
            identity.detail == static_cast<std::uint32_t>(BindingSourceKind::Variable)) ++count;
    }
    return count;
}

std::size_t BindingDependencyIndex::StorageBytes() const noexcept {
    return inputs_.RequestedBytes() + keys_.RequestedBytes() + targets_.RequestedBytes() +
        (freeKeys_.capacity() + freeTargets_.capacity() + dirtyTargets_.capacity() + freeSlots_.capacity()) * sizeof(std::uint32_t) +
        preparedBindings_.capacity() * sizeof(std::uint64_t);
}

BindingTable::BindingTable(std::uint32_t maxDefinitions,
                           std::uint32_t maxAttachments,
                           std::uint32_t maxBuses, std::uint32_t renderBindingCapacity,
                           std::uint32_t targetCapacity)
    : m_projectionAttachments(maxAttachments) {
    if (!m_definitions.ConstructThrough(maxDefinitions) ||
        !m_attachments.ConstructThrough(maxAttachments) ||
        !m_voiceEffectInstances.ConstructThrough(maxAttachments))
        throw harken::detail::LogicPreparationFailure{};
    m_voiceAttachmentIndex.reserve(maxAttachments);
    m_definitionIndex.reserve(maxDefinitions);
    (void)maxBuses;
    const auto bindings = std::uint64_t{renderBindingCapacity} + 2ull * maxAttachments;
    const auto targets = targetCapacity == 0 ? bindings + 3ull * kVariableSliceTargetHardLimit : targetCapacity;
    if (bindings > UINT32_MAX || targets > UINT32_MAX ||
        !m_dependencies.Reset(static_cast<std::uint32_t>(bindings), static_cast<std::uint32_t>(targets)))
        throw harken::detail::LogicPreparationFailure{};
}

BindingTable::BindingTable(const BindingTable& other)
    : m_voiceCatalogContext(nullptr), m_prepareVoiceCatalog(nullptr),
      m_dependencies(other.m_dependencies),
      m_projectionAttachments(other.m_projectionAttachments), m_projectionActive(other.m_projectionActive),
      m_definitionCount(other.m_definitionCount), m_hasPendingDefinitions(other.m_hasPendingDefinitions),
      m_attachmentCount(other.m_attachmentCount),
      m_voiceEffectInstanceCount(other.m_voiceEffectInstanceCount),
      m_definitionHighWater(other.m_definitionHighWater), m_definitionRefusedCount(other.m_definitionRefusedCount),
      m_attachmentHighWater(other.m_attachmentHighWater), m_attachmentRefusedCount(other.m_attachmentRefusedCount) {
    if (!m_definitions.ConstructThrough(other.m_definitions.size()) ||
        !m_attachments.ConstructThrough(other.m_attachments.size()) ||
        !m_voiceEffectInstances.ConstructThrough(other.m_voiceEffectInstances.size()))
        throw harken::detail::LogicPreparationFailure{};
    m_definitionIndex = other.m_definitionIndex;
    m_definitionIndex.reserve(other.m_definitions.size());
    m_voiceAttachmentIndex = other.m_voiceAttachmentIndex;
    m_voiceAttachmentIndex.reserve(other.m_attachments.size());
    std::copy(other.m_definitions.begin(), other.m_definitions.end(), m_definitions.begin());
    std::copy(other.m_attachments.begin(), other.m_attachments.end(), m_attachments.begin());
    std::copy(other.m_voiceEffectInstances.begin(), other.m_voiceEffectInstances.end(), m_voiceEffectInstances.begin());
    for (auto& row : m_attachments) {
        if (!row.used) continue;
        row.dependencyDefinition = &m_definitions[row.definitionIndex];
        if (row.dependency.Valid())
            (void)m_dependencies.Relocate(row.dependency, &row, &ReadAttachmentDependency);
    }
    for (auto& row : m_voiceEffectInstances) {
        if (!row.used) continue;
        row.dependencyDefinition = &m_definitions[row.definitionIndex];
        if (row.dependency.Valid()) (void)m_dependencies.Relocate(row.dependency, &row, &ReadEffectDependency);
    }
}

bool BindingTable::PrepareCapacity(std::uint64_t definitions, std::uint64_t attachments) noexcept {
    if (definitions > UINT32_MAX || attachments > UINT32_MAX || m_projectionActive) return false;
    const auto grow = [](std::uint32_t current, std::uint64_t required) {
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(UINT32_MAX,
            std::max(required, std::uint64_t{current} * 2u)));
    };
    if (definitions > m_definitions.size() &&
        !m_definitions.ConstructThrough(grow(m_definitions.size(), definitions),
            [this](std::uint32_t count) noexcept {
                try { m_definitionIndex.reserve(count); return true; }
                catch (...) {
                    detail::RecordCurrentLogicAllocFailure(detail::LogicAllocSite::BindingStorage);
                    return false;
                }
            })) return false;
    if (attachments > m_attachments.size() &&
        !m_attachments.ConstructThrough(grow(m_attachments.size(), attachments),
            [this](std::uint32_t count) noexcept {
                try {
                    m_projectionAttachments.resize(count);
                    m_voiceAttachmentIndex.reserve(count);
                    return true;
                } catch (...) {
                    detail::RecordCurrentLogicAllocFailure(detail::LogicAllocSite::BindingStorage);
                    return false;
                }
            })) return false;
    return true;
}

bool BindingTable::PrepareEffectInstanceCapacity(std::uint64_t count) noexcept {
    if (count > UINT32_MAX) return false;
    if (count <= m_voiceEffectInstances.size()) return true;
    const auto capacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(UINT32_MAX,
        std::max(count, std::uint64_t{m_voiceEffectInstances.size()} * 2u)));
    return m_voiceEffectInstances.ConstructThrough(capacity);
}

std::span<const std::uint32_t> BindingTable::VoiceAttachmentRange(VnObjectId object) const noexcept {
    const auto begin = std::lower_bound(m_voiceAttachmentIndex.begin(), m_voiceAttachmentIndex.end(), object,
        [this](std::uint32_t slot, VnObjectId id) { return m_attachments[slot].objectId < id; });
    const auto end = std::upper_bound(begin, m_voiceAttachmentIndex.end(), object,
        [this](VnObjectId id, std::uint32_t slot) { return id < m_attachments[slot].objectId; });
    return {begin, end};
}

void BindingTable::IndexVoiceAttachment(const Attachment& row) noexcept {
    if (row.scopeKind != kTargetScopeVoice ||
        m_definitions[row.definitionIndex].sourceKind != BindingSourceKind::Variable) return;
    const auto slot = m_attachments.IndexOf(&row);
    const auto at = std::lower_bound(m_voiceAttachmentIndex.begin(), m_voiceAttachmentIndex.end(), slot,
        [this](std::uint32_t a, std::uint32_t b) {
            return std::pair(m_attachments[a].objectId, a) < std::pair(m_attachments[b].objectId, b);
        });
    assert(m_voiceAttachmentIndex.size() < m_voiceAttachmentIndex.capacity());
    m_voiceAttachmentIndex.insert(at, slot);
}

void BindingTable::UnindexVoiceAttachment(const Attachment& row) noexcept {
    if (row.scopeKind != kTargetScopeVoice ||
        m_definitions[row.definitionIndex].sourceKind != BindingSourceKind::Variable) return;
    const auto slot = m_attachments.IndexOf(&row);
    const auto at = std::lower_bound(m_voiceAttachmentIndex.begin(), m_voiceAttachmentIndex.end(), slot,
        [this](std::uint32_t a, std::uint32_t b) {
            return std::pair(m_attachments[a].objectId, a) < std::pair(m_attachments[b].objectId, b);
        });
    if (at != m_voiceAttachmentIndex.end() && *at == slot) m_voiceAttachmentIndex.erase(at);
}

BindingDependencyIdentity BindingTable::ReadAttachmentDependency(const void* fact, BindingDependencyKind kind) noexcept {
    const auto& row = *static_cast<const Attachment*>(fact);
    const auto& definition = *row.dependencyDefinition;
    BindingDependencyIdentity identity{};
    if (kind == BindingDependencyKind::Target) {
        identity.object = row.scopeKind == kTargetScopeVoice || row.scopeKind == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM
            ? row.instanceHandle : row.busIndex;
        identity.scope = row.scopeKind;
        identity.target = definition.targetId;
        identity.generation = row.targetGeneration;
        identity.detail = static_cast<std::uint32_t>(definition.sourceKind);
    } else if (kind == BindingDependencyKind::Definition) identity.source = definition.id;
    else if (kind == BindingDependencyKind::Probe) {
        identity.source = definition.probeStableId;
        identity.generation = definition.probeGeneration;
    } else identity.source = definition.variableId;
    return identity;
}

BindingDependencyIdentity BindingTable::ReadEffectDependency(const void* fact, BindingDependencyKind kind) noexcept {
    const auto& row = *static_cast<const VoiceEffectInstance*>(fact);
    const auto& definition = *row.dependencyDefinition;
    BindingDependencyIdentity identity{};
    if (kind == BindingDependencyKind::Target) {
        identity.object = row.renderHandle;
        identity.scope = VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
        identity.target = definition.targetId;
    } else if (kind == BindingDependencyKind::Definition) identity.source = definition.id;
    else {
        identity.source = definition.variableId;
        if (kind == BindingDependencyKind::Local) identity.object = row.dependencyEmitter;
    }
    return identity;
}

bool BindingTable::PrepareAttachmentDependency(Attachment& row, std::uint32_t scope) noexcept {
    if (scope == kTargetScopeVoice)
        return m_prepareVoiceCatalog == nullptr || m_prepareVoiceCatalog(m_voiceCatalogContext);
    return m_dependencies.AllocateBindingSlot(&row.dependencySlot);
}

void BindingTable::ReleaseAttachmentDependency(Attachment& row) noexcept {
    UnindexVoiceAttachment(row);
    if (row.dependency.Valid()) (void)m_dependencies.Remove(row.dependency);
    m_dependencies.ReleaseBindingSlot(row.dependencySlot);
    row.dependency = {};
    row.dependencySlot = UINT32_MAX;
}

void BindingTable::RegisterAttachmentDependency(Attachment& row) noexcept {
    IndexVoiceAttachment(row);
    row.dependencyDefinition = &m_definitions[row.definitionIndex];
    if (row.scopeKind == kTargetScopeVoice && row.dependencyDefinition->sourceKind == BindingSourceKind::Variable) return;
    const auto slot = row.dependencySlot;
    const bool registered = m_dependencies.Register(slot, &row, &ReadAttachmentDependency, m_attachments.IndexOf(&row), false, true,
        row.dependencyDefinition->sourceKind == BindingSourceKind::MeterProbe, &row.dependency);
    assert(registered);
    (void)registered;
}

bool BindingTable::PrepareEffectDependency(std::uint32_t slot) noexcept {
    auto& row = m_voiceEffectInstances[slot];
    return row.dependencySlot == UINT32_MAX
        ? m_dependencies.AllocateBindingSlot(&row.dependencySlot)
        : m_dependencies.PrepareBinding(row.dependencySlot);
}

std::uint32_t BindingTable::EffectDependencySlot(const VoiceEffectInstance& row) const noexcept {
    return row.dependencySlot;
}

void BindingTable::RegisterEffectDependency(VoiceEffectInstance& row) noexcept {
    row.dependencyDefinition = &m_definitions[row.definitionIndex];
    const auto slot = EffectDependencySlot(row);
    const bool registered = m_dependencies.Register(slot, &row, &ReadEffectDependency, m_voiceEffectInstances.IndexOf(&row), true, true, false, &row.dependency);
    assert(registered);
    (void)registered;
}

void BindingTable::RemoveEffectDependency(VoiceEffectInstance& row) noexcept {
    if (row.dependency.Valid()) { (void)m_dependencies.Remove(row.dependency); row.dependency = {}; }
}

bool BindingTable::RefreshEffectDependencyEmitter(VoiceEffectInstance& row, VnEmitterHandle emitter) noexcept {
    if (row.dependencyEmitter == emitter) return true;
    if (emitter == 0) {
        if (row.dependency.Valid() && !m_dependencies.DetachLocal(row.dependency)) return false;
        row.dependencyEmitter = 0;
        return true;
    }
    if (!PrepareEffectDependency(m_voiceEffectInstances.IndexOf(&row))) return false;
    RemoveEffectDependency(row);
    row.dependencyEmitter = emitter;
    RegisterEffectDependency(row);
    return true;
}

bool BindingTable::BeginVariableProjection() noexcept {
    if (m_projectionActive) return false;
    for (std::size_t i = 0; i < m_attachments.size(); ++i) {
        const auto& attachment = m_attachments[i];
        m_projectionAttachments[i] = {attachment.lastContribution,
                                       attachment.lastHadValue};
    }
    m_projectionActive = true;
    return true;
}

void BindingTable::RestoreVariableProjection() noexcept {
    if (!m_projectionActive) return;
    for (std::size_t i = 0; i < m_attachments.size(); ++i) {
        auto& attachment = m_attachments[i];
        const auto& saved = m_projectionAttachments[i];
        attachment.lastContribution = saved.lastContribution;
        attachment.lastHadValue = saved.lastHadValue;
        m_dependencies.Mark(attachment.dependency);
    }
    m_projectionActive = false;
}

std::uint64_t BindingTable::VariableProjectionStorageBytes() const noexcept {
    return m_projectionAttachments.size() * sizeof(AttachmentEvaluation);
}

VnResult BindingTable::ValidateDescriptor(std::uint32_t scopeKind,
                                          std::uint32_t targetId,
                                          const CurvePoint* points,
                                          const CurveHandlePair* handles,
                                          std::uint32_t pointCount,
                                          std::uint32_t transitionMs,
                                          Curve* outCurve) {
    // The curve rules are exactly Curve::SetPoints' (finite coordinates,
    // valid Bezier handles, strictly increasing x, 1..8 points); a refused
    // set leaves *outCurve empty and nothing else was touched yet.
    if (!IsValidBindingTransitionMs(transitionMs)
        || !outCurve->SetPoints(points, handles, pointCount)) {
        return VN_INVALID_ARG;
    }
    // Effect-parameter scopes: the targetId is a packed locator, not a target-
    // meta id, so it is validated against the effect-parameter metadata
    // authority instead of the voice/bus target table. The located parameter
    // must exist (known effect type, in-range index) and accept a binding (its
    // combine domain is not None); a transition is refused (effect parameters
    // are block-boundary steps, no ramp field). Object/slot liveness is checked
    // at attach/apply time against the installed effect, not here.
    if (scopeKind == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM ||
        scopeKind == VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
        const EffectParamLocator loc = UnpackEffectParamLocator(targetId);
        const EffectParamMeta* pm =
            FindEffectParamMeta(loc.effectType, loc.paramIndex);
        if (pm == nullptr || pm->combine == EffectParamCombine::None) {
            return VN_INVALID_ARG;
        }
        if (transitionMs != 0) {
            return VN_INVALID_ARG;
        }
        return VN_OK;
    }
    // Known scoped target only — including scope kinds that do not exist
    // yet, refused rather than guessed (section 8).
    const TargetMeta* meta = FindTargetMeta(scopeKind, targetId);
    if (meta == nullptr) {
        return VN_INVALID_ARG;
    }
    // Section 9.1: a nonzero transition on a snap-only target refuses
    // loudly at registration, never silently ignored.
    if (transitionMs != 0 && !meta->supportsTransition) {
        return VN_INVALID_ARG;
    }
    return VN_OK;
}

VnResult BindingTable::CanRegister(BindingId id, VnVariableId variableId,
                                   std::uint32_t scopeKind,
                                   std::uint32_t targetId,
                                   const CurvePoint* points,
                                   const CurveHandlePair* handles,
                                   std::uint32_t pointCount,
                                   std::uint32_t transitionMs) const {
    // The Variable id is structural identity, never validated (any id may
    // drive a binding; an unset Variable simply contributes nothing).
    (void)variableId;
    Curve curve;
    const VnResult valid = ValidateDescriptor(scopeKind, targetId, points,
                                              handles, pointCount,
                                              transitionMs, &curve);
    if (valid != VN_OK) {
        return valid;
    }
    if (Find(id) != nullptr) {
        return VN_IN_USE;
    }
    return VN_OK;
}

VnResult BindingTable::RegisterBinding(BindingId id, VnVariableId variableId,
                                       std::uint32_t scopeKind,
                                       std::uint32_t targetId,
                                       const CurvePoint* points,
                                       const CurveHandlePair* handles,
                                       std::uint32_t pointCount,
                                       std::uint32_t transitionMs,
                                       LogRing* log,
                                       std::uint64_t ownerCookie) {
    const VnResult admitted = CanRegister(id, variableId, scopeKind, targetId,
                                          points, handles, pointCount,
                                          transitionMs);
    if (admitted != VN_OK) {
        if (admitted == kResultNoBinding) {
            ++m_definitionRefusedCount;
            LogPoolExhausted(log, kBindingPoolDefinitions,
                             m_definitions.size());
        }
        return admitted;
    }
    if (!PrepareCapacity(std::uint64_t{m_definitionCount} + 1u, m_attachmentCount))
        return VN_OUT_OF_MEMORY;
    // Rebuild the validated curve: the admission proved this set succeeds,
    // and SetPoints is pure, so the admission/commit split shares no
    // scratch state.
    Curve curve;
    if (ValidateDescriptor(scopeKind, targetId, points, handles, pointCount,
                           transitionMs, &curve) != VN_OK) {
        return VN_INVALID_ARG; // unreachable: the admission ran this check
    }
    Definition* slot = nullptr;
    for (Definition& def : m_definitions) {
        if (!def.used) {
            slot = &def;
            break;
        }
    }
    if (slot == nullptr) {
        // Unreachable: CanRegister proved a free slot exists and this
        // thread is the only mutator. Kept defensive (the attach twin).
        ++m_definitionRefusedCount;
        LogPoolExhausted(log, kBindingPoolDefinitions,
                         m_definitions.size());
        return kResultNoBinding;
    }
    *slot = Definition{};
    slot->used = true;
    slot->id = id;
    slot->variableId = variableId;
    slot->scopeKind = scopeKind;
    slot->targetId = targetId;
    slot->curve = curve;
    slot->transitionMs = transitionMs;
    slot->ownerCookie = ownerCookie;
    IndexDefinition(*slot);
    ++m_definitionCount;
    if (m_definitionCount > m_definitionHighWater) {
        m_definitionHighWater = m_definitionCount;
    }
    return VN_OK;
}

VnResult BindingTable::UnregisterBinding(BindingId id) {
    Definition* def = FindMutable(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (AnyAttachmentOf(id)) {
        return VN_IN_USE;
    }
    UnindexDefinition(*def);
    *def = Definition{};
    --m_definitionCount;
    return VN_OK;
}

VnResult BindingTable::ReplaceBinding(BindingId id, VnVariableId variableId,
                                      std::uint32_t scopeKind,
                                      std::uint32_t targetId,
                                      const CurvePoint* points,
                                      const CurveHandlePair* handles,
                                      std::uint32_t pointCount,
                                      std::uint32_t transitionMs) {
    Definition* def = FindMutable(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    // Structural identity is frozen: only curve and transitionMs may
    // change; a target move is detach + unregister + register by design.
    if (variableId != def->variableId || scopeKind != def->scopeKind ||
        targetId != def->targetId) {
        return VN_INVALID_ARG;
    }
    Curve curve;
    const VnResult valid = ValidateDescriptor(scopeKind, targetId, points,
                                              handles, pointCount,
                                              transitionMs, &curve);
    if (valid != VN_OK) {
        return valid;
    }
    // Fully validated: stage the revision. It installs at the next
    // update's fixed phase; until then every reader keeps evaluating the
    // current revision. A second same-tick replace overwrites the staged
    // one — the last revision is the one that becomes visible.
    def->pendingCurve = curve;
    def->pendingTransitionMs = transitionMs;
    def->pendingInstall = true;
    m_hasPendingDefinitions = true;
    return VN_OK;
}

VnResult BindingTable::PrepareRegisterDefinition(
    BindingId id, VnVariableId variableId, std::uint32_t scopeKind,
    std::uint32_t targetId, const CurvePoint* points,
    const CurveHandlePair* handles, std::uint32_t pointCount,
    std::uint32_t transitionMs, std::uint32_t slotIndex,
    std::uint64_t ownerCookie, PreparedDefinitionToken* out,
    bool slotWillBeVacant, bool idWillBeRemoved) {
    if (out == nullptr || slotIndex >= m_definitions.size()) {
        return VN_INVALID_ARG;
    }
    *out = PreparedDefinitionToken{};
    Curve curve;
    const VnResult valid =
        ValidateDescriptor(scopeKind, targetId, points, handles, pointCount,
                           transitionMs, &curve);
    if (valid != VN_OK) {
        return valid;
    }
    if (Find(id) != nullptr && !idWillBeRemoved) {
        return VN_IN_USE;
    }
    if (m_definitions[slotIndex].used && !slotWillBeVacant) {
        return kResultNoBinding;
    }
    Definition value;
    value.used = true;
    value.id = id;
    value.variableId = variableId;
    value.scopeKind = scopeKind;
    value.targetId = targetId;
    value.curve = curve;
    value.transitionMs = transitionMs;
    value.ownerCookie = ownerCookie;
    out->m_operation = PreparedDefinitionToken::Operation::Add;
    out->m_slotIndex = slotIndex;
    out->m_value = value;
    return VN_OK;
}

VnResult BindingTable::PrepareReplaceDefinition(
    BindingId id, VnVariableId variableId, std::uint32_t scopeKind,
    std::uint32_t targetId, const CurvePoint* points,
    const CurveHandlePair* handles, std::uint32_t pointCount,
    std::uint32_t transitionMs, PreparedDefinitionToken* out) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = PreparedDefinitionToken{};
    Definition* def = FindMutable(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (variableId != def->variableId || scopeKind != def->scopeKind ||
        targetId != def->targetId) {
        return VN_INVALID_ARG;
    }
    Curve curve;
    const VnResult valid =
        ValidateDescriptor(scopeKind, targetId, points, handles, pointCount,
                           transitionMs, &curve);
    if (valid != VN_OK) {
        return valid;
    }
    Definition value = *def;
    value.pendingCurve = curve;
    value.pendingTransitionMs = transitionMs;
    value.pendingInstall = true;
    out->m_operation = PreparedDefinitionToken::Operation::Replace;
    out->m_slotIndex =
        m_definitions.IndexOf(def);
    out->m_value = value;
    return VN_OK;
}

VnResult BindingTable::PrepareRemoveDefinition(
    BindingId id, PreparedDefinitionToken* out,
    bool attachmentsWillBeDetached) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = PreparedDefinitionToken{};
    Definition* def = FindMutable(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (AnyAttachmentOf(id) && !attachmentsWillBeDetached) {
        return VN_IN_USE;
    }
    out->m_operation = PreparedDefinitionToken::Operation::Remove;
    out->m_slotIndex =
        m_definitions.IndexOf(def);
    out->m_value = *def;
    return VN_OK;
}

void BindingTable::CancelPreparedDefinition(
    PreparedDefinitionToken* token) noexcept {
    if (token != nullptr) {
        *token = PreparedDefinitionToken{};
    }
}

void BindingTable::CommitPreparedDefinition(
    PreparedDefinitionToken* token, bool installBusRevisionImmediately) noexcept {
    if (token == nullptr || !token->IsPrepared()) {
        return;
    }
    Definition& slot = m_definitions[token->m_slotIndex];
    switch (token->m_operation) {
    case PreparedDefinitionToken::Operation::Add:
        slot = token->m_value;
        IndexDefinition(slot);
        ++m_definitionCount;
        if (m_definitionCount > m_definitionHighWater) {
            m_definitionHighWater = m_definitionCount;
        }
        break;
    case PreparedDefinitionToken::Operation::Replace:
        slot = token->m_value;
        if (installBusRevisionImmediately && slot.scopeKind == kTargetScopeBus &&
            slot.pendingInstall) {
            slot.curve = slot.pendingCurve;
            slot.transitionMs = slot.pendingTransitionMs;
            slot.pendingInstall = false;
            ++slot.revision;
            m_dependencies.NotifyDefinition(slot.id);
        }
        m_hasPendingDefinitions |= slot.pendingInstall;
        break;
    case PreparedDefinitionToken::Operation::Remove:
        UnindexDefinition(slot);
        slot = Definition{};
        --m_definitionCount;
        break;
    case PreparedDefinitionToken::Operation::None:
        break;
    }
    *token = PreparedDefinitionToken{};
}

VnResult BindingTable::CanAttach(BindingId id, std::uint32_t scopeKind,
                                 VnObjectId objectId) const {
    const Definition* def = Find(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (scopeKind != def->scopeKind) {
        return VN_INVALID_ARG;
    }
    if (FindAttachment(id, scopeKind, objectId) != nullptr) {
        return VN_IN_USE;
    }
    return VN_OK;
}

VnResult BindingTable::Attach(BindingId id, std::uint32_t scopeKind,
                              VnObjectId objectId, std::uint32_t busIndex,
                              LogRing* log, std::uint64_t ownerCookie,
                              std::uint32_t targetGeneration) {
    const VnResult admitted = CanAttach(id, scopeKind, objectId);
    if (admitted != VN_OK) {
        if (admitted == kResultNoBinding) {
            ++m_attachmentRefusedCount;
            LogPoolExhausted(log, kBindingPoolAttachments,
                             m_attachments.size());
        }
        return admitted;
    }
    if (!PrepareCapacity(m_definitionCount, std::uint64_t{m_attachmentCount} + 1u))
        return VN_OUT_OF_MEMORY;
    const Definition* def = Find(id);
    for (std::uint32_t i = 0; i < m_attachments.size(); ++i) {
        Attachment& a = m_attachments[i];
        if (a.used) {
            continue;
        }
        if (!PrepareAttachmentDependency(a, scopeKind)) return kResultNoBinding;
        a.used = true;
        a.bindingId = id;
        a.definitionIndex =
            m_definitions.IndexOf(def);
        a.scopeKind = scopeKind;
        a.objectId = objectId;
        a.busIndex = busIndex;
        a.targetGeneration = targetGeneration;
        a.ownerCookie = ownerCookie;
        RegisterAttachmentDependency(a);
        ++m_attachmentCount;
        if (m_attachmentCount > m_attachmentHighWater) {
            m_attachmentHighWater = m_attachmentCount;
        }
        return VN_OK;
    }
    // Unreachable: CanAttach proved a free slot exists and this thread is
    // the only mutator. Kept defensive.
    ++m_attachmentRefusedCount;
    LogPoolExhausted(log, kBindingPoolAttachments, m_attachments.size());
    return kResultNoBinding;
}

VnResult BindingTable::Detach(BindingId id, std::uint32_t scopeKind,
                              VnObjectId objectId, BusTargetStore* store) {
    PreparedAttachmentToken token;
    const auto result = PrepareDetach(id, scopeKind, objectId, &token);
    if (result == VN_OK) CommitPreparedAttachment(&token, store);
    return result;
}

VnResult BindingTable::PrepareAttach(
    BindingId id, std::uint32_t scopeKind, VnObjectId objectId,
    std::uint32_t busIndex, std::uint32_t slotIndex,
    std::uint64_t ownerCookie, PreparedAttachmentToken* out,
    std::uint32_t preparedDefinitionIndex,
    bool definitionWillBeAdded, bool slotWillBeVacant,
    bool duplicateWillBeDetached, std::uint32_t targetGeneration) {
    if (out == nullptr || slotIndex >= m_attachments.size()) {
        return VN_INVALID_ARG;
    }
    *out = PreparedAttachmentToken{};
    const Definition* def = Find(id);
    if (def == nullptr && !definitionWillBeAdded) {
        return VN_UNKNOWN_ID;
    }
    if (def != nullptr && !definitionWillBeAdded &&
        scopeKind != def->scopeKind) {
        return VN_INVALID_ARG;
    }
    if (FindAttachment(id, scopeKind, objectId) != nullptr &&
        !duplicateWillBeDetached) {
        return VN_IN_USE;
    }
    if (m_attachments[slotIndex].used && !slotWillBeVacant) {
        return kResultNoBinding;
    }
    Attachment value;
    if (!PrepareAttachmentDependency(value, scopeKind)) return kResultNoBinding;
    value.used = true;
    value.bindingId = id;
    value.definitionIndex =
        !definitionWillBeAdded && def != nullptr
            ? m_definitions.IndexOf(def)
            : preparedDefinitionIndex;
    value.scopeKind = scopeKind;
    value.objectId = objectId;
    value.busIndex = busIndex;
    value.targetGeneration = targetGeneration;
    value.ownerCookie = ownerCookie;
    out->m_operation = PreparedAttachmentToken::Operation::Add;
    out->m_slotIndex = slotIndex;
    out->m_value = value;
    return VN_OK;
}

VnResult BindingTable::PrepareDetach(
    BindingId id, std::uint32_t scopeKind, VnObjectId objectId,
    PreparedAttachmentToken* out) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = PreparedAttachmentToken{};
    const auto* attachment = FindAttachment(id, scopeKind, objectId);
    if (attachment == nullptr) return VN_UNKNOWN_ID;
    out->m_operation = PreparedAttachmentToken::Operation::Remove;
    out->m_slotIndex = m_attachments.IndexOf(attachment);
    out->m_value = *attachment;
    return VN_OK;
}

bool BindingTable::CanCommitPreparedAttachment(
    const PreparedAttachmentToken& token) const noexcept {
    if (token.m_slotIndex >= m_attachments.size()) return false;
    const Attachment& current = m_attachments[token.m_slotIndex];
    if (token.m_operation == PreparedAttachmentToken::Operation::Add) {
        return !current.used;
    }
    if (token.m_operation != PreparedAttachmentToken::Operation::Remove) return false;
    const Attachment& expected = token.m_value;
    return current.used && expected.used &&
        current.bindingId == expected.bindingId &&
        current.definitionIndex == expected.definitionIndex &&
        current.scopeKind == expected.scopeKind &&
        current.objectId == expected.objectId &&
        current.busIndex == expected.busIndex &&
        current.targetGeneration == expected.targetGeneration &&
        std::bit_cast<std::uint32_t>(current.lastContribution) ==
            std::bit_cast<std::uint32_t>(expected.lastContribution) &&
        current.lastHadValue == expected.lastHadValue &&
        current.meterFrozen == expected.meterFrozen &&
        current.ownerCookie == expected.ownerCookie;
}

void BindingTable::CancelPreparedAttachment(
    PreparedAttachmentToken* token) noexcept {
    if (token != nullptr) {
        if (token->m_operation == PreparedAttachmentToken::Operation::Add &&
            token->m_value.scopeKind != kTargetScopeVoice)
            m_dependencies.ReleaseBindingSlot(token->m_value.dependencySlot);
        *token = PreparedAttachmentToken{};
    }
}

void BindingTable::CommitPreparedAttachment(
    PreparedAttachmentToken* token, BusTargetStore* store) noexcept {
    if (token == nullptr || !token->IsPrepared()) {
        return;
    }
    Attachment& slot = m_attachments[token->m_slotIndex];
    if (token->m_operation == PreparedAttachmentToken::Operation::Add) {
        slot = token->m_value;
        RegisterAttachmentDependency(slot);
        ++m_attachmentCount;
        if (m_attachmentCount > m_attachmentHighWater) {
            m_attachmentHighWater = m_attachmentCount;
        }
    } else if (token->m_operation ==
               PreparedAttachmentToken::Operation::Remove) {
        const Attachment& removed = token->m_value;
        if (store != nullptr && removed.scopeKind == kTargetScopeBus &&
            removed.lastHadValue && (removed.targetGeneration == 0 ||
                store->ActiveGeneration(removed.busIndex) == removed.targetGeneration)) {
            const Definition& def =
                m_definitions[removed.definitionIndex];
            store->MarkVariableChanged(removed.busIndex, def.targetId,
                                       def.transitionMs);
        }
        ReleaseAttachmentDependency(slot);
        slot = Attachment{};
        --m_attachmentCount;
    }
    *token = PreparedAttachmentToken{};
}

std::uint32_t BindingTable::IndexedDefinition(BindingSourceKind kind, BindingId id) const noexcept {
    const auto key = std::pair(kind, id);
    const auto at = std::lower_bound(m_definitionIndex.begin(), m_definitionIndex.end(), key,
        [](const DefinitionIndexEntry& row, const auto& sought) {
            return std::pair(row.kind, row.id) < sought;
        });
    return at != m_definitionIndex.end() && at->kind == kind && at->id == id ? at->slot : UINT32_MAX;
}

void BindingTable::IndexDefinition(const Definition& row) noexcept {
    const auto key = std::pair(row.sourceKind, row.id);
    const auto at = std::lower_bound(m_definitionIndex.begin(), m_definitionIndex.end(), key,
        [](const DefinitionIndexEntry& value, const auto& sought) {
            return std::pair(value.kind, value.id) < sought;
        });
    assert(m_definitionIndex.size() < m_definitionIndex.capacity());
    m_definitionIndex.insert(at, {row.sourceKind, row.id, m_definitions.IndexOf(&row)});
}

void BindingTable::UnindexDefinition(const Definition& row) noexcept {
    const auto key = std::pair(row.sourceKind, row.id);
    const auto at = std::lower_bound(m_definitionIndex.begin(), m_definitionIndex.end(), key,
        [](const DefinitionIndexEntry& value, const auto& sought) {
            return std::pair(value.kind, value.id) < sought;
        });
    assert(at != m_definitionIndex.end() && at->slot == m_definitions.IndexOf(&row));
    m_definitionIndex.erase(at);
}

const BindingTable::Definition* BindingTable::Find(BindingId id) const {
    const auto slot = IndexedDefinition(BindingSourceKind::Variable, id);
    return slot == UINT32_MAX ? nullptr : &m_definitions[slot];
}

BindingTable::Definition* BindingTable::FindMutable(BindingId id) {
    const auto slot = IndexedDefinition(BindingSourceKind::Variable, id);
    return slot == UINT32_MAX ? nullptr : &m_definitions[slot];
}

VnResult BindingTable::RegisterProbeBinding(
    BindingId id, VnObjectId probeStableId,
    std::uint32_t probeGeneration, std::uint32_t scopeKind,
    std::uint32_t targetId, const CurvePoint* points,
    const CurveHandlePair* handles, std::uint32_t pointCount,
    std::uint32_t transitionMs) {
    if (FindProbeDefinition(id) != nullptr) {
        return VN_IN_USE;
    }
    Curve curve;
    if (!curve.SetPoints(points, handles, pointCount)) {
        return VN_INVALID_ARG;
    }
    if (!PrepareCapacity(std::uint64_t{m_definitionCount} + 1u, m_attachmentCount))
        return VN_OUT_OF_MEMORY;
    for (Definition& def : m_definitions) {
        if (def.used) {
            continue;
        }
        def = Definition{};
        def.used = true;
        def.id = id;
        def.sourceKind = BindingSourceKind::MeterProbe;
        def.probeStableId = probeStableId;
        def.probeGeneration = probeGeneration;
        def.scopeKind = scopeKind;
        def.targetId = targetId;
        def.curve = curve;
        def.transitionMs = transitionMs;
        IndexDefinition(def);
        ++m_definitionCount;
        if (m_definitionCount > m_definitionHighWater) {
            m_definitionHighWater = m_definitionCount;
        }
        return VN_OK;
    }
    return kResultNoBinding;
}

VnResult BindingTable::AttachProbe(BindingId id, std::uint32_t scopeKind,
                                   VnObjectId objectId, VnVoiceHandle instanceHandle,
                                   std::uint32_t busIndex,
                                   std::uint32_t targetGeneration) {
    const Definition* def = FindProbeDefinition(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    if (!PrepareCapacity(m_definitionCount, std::uint64_t{m_attachmentCount} + 1u))
        return VN_OUT_OF_MEMORY;
    for (Attachment& a : m_attachments) {
        if (a.used) {
            continue;
        }
        const auto slot = m_attachments.IndexOf(&a);
        a = Attachment{};
        if (!m_dependencies.AllocateBindingSlot(&a.dependencySlot)) return kResultNoBinding;
        a.used = true;
        a.bindingId = id;
        a.definitionIndex =
            m_definitions.IndexOf(def);
        a.scopeKind = scopeKind;
        a.objectId = objectId;
        a.instanceHandle = instanceHandle;
        a.busIndex = busIndex;
        a.targetGeneration = targetGeneration;
        RegisterAttachmentDependency(a);
        ++m_attachmentCount;
        if (m_attachmentCount > m_attachmentHighWater) {
            m_attachmentHighWater = m_attachmentCount;
        }
        return VN_OK;
    }
    return kResultNoBinding;
}

BindingTable::Definition* BindingTable::FindProbeDefinition(BindingId id) noexcept {
    const auto slot = IndexedDefinition(BindingSourceKind::MeterProbe, id);
    return slot == UINT32_MAX ? nullptr : &m_definitions[slot];
}

const BindingTable::Definition* BindingTable::FindProbeDefinition(BindingId id) const noexcept {
    const auto slot = IndexedDefinition(BindingSourceKind::MeterProbe, id);
    return slot == UINT32_MAX ? nullptr : &m_definitions[slot];
}

BindingTable::Attachment* BindingTable::FindProbeAttachment(
    BindingId id, std::uint32_t scopeKind, VnObjectId objectId, VnVoiceHandle instanceHandle,
    std::uint32_t targetGeneration) noexcept {
    for (Attachment& a : m_attachments) {
        if (a.used && a.bindingId == id && a.scopeKind == scopeKind &&
            a.objectId == objectId && a.instanceHandle == instanceHandle &&
            a.targetGeneration == targetGeneration &&
            m_definitions[a.definitionIndex].sourceKind ==
                BindingSourceKind::MeterProbe) {
            return &a;
        }
    }
    return nullptr;
}

void BindingTable::ReleaseProbeAttachment(Attachment& row) noexcept {
    if (row.used) {
        ReleaseAttachmentDependency(row);
        row = Attachment{};
        --m_attachmentCount;
    }
}

void BindingTable::ReleaseProbeDefinition(Definition& row) noexcept {
    if (row.used && row.sourceKind == BindingSourceKind::MeterProbe) {
        UnindexDefinition(row);
        row = Definition{};
        --m_definitionCount;
    }
}

bool BindingTable::AnyProbeAttachmentOf(BindingId id) const noexcept {
    for (const Attachment& a : m_attachments) {
        if (a.used && a.bindingId == id &&
            m_definitions[a.definitionIndex].sourceKind ==
                BindingSourceKind::MeterProbe) {
            return true;
        }
    }
    return false;
}

std::uint32_t BindingTable::ProbeDefinitionCount() const noexcept {
    std::uint32_t count = 0;
    for (const Definition& def : m_definitions) {
        if (def.used && def.sourceKind == BindingSourceKind::MeterProbe) {
            ++count;
        }
    }
    return count;
}

std::uint32_t BindingTable::ProbeAttachmentCount() const noexcept {
    std::uint32_t count = 0;
    for (const Attachment& a : m_attachments) {
        if (a.used && m_definitions[a.definitionIndex].sourceKind ==
                          BindingSourceKind::MeterProbe) {
            ++count;
        }
    }
    return count;
}

const BindingTable::Attachment*
BindingTable::FindAttachment(BindingId id, std::uint32_t scopeKind,
                             VnObjectId objectId) const {
    for (const Attachment& a : m_attachments) {
        if (a.used && a.bindingId == id && a.scopeKind == scopeKind && a.objectId == objectId &&
            m_definitions[a.definitionIndex].sourceKind == BindingSourceKind::Variable) {
            return &a;
        }
    }
    return nullptr;
}

BindingDefinitionView BindingTable::ViewOf(const Definition& def) const {
    BindingDefinitionView view;
    view.definitionId = def.id;
    view.variableId = def.variableId;
    view.targetId = def.targetId;
    view.curve = &def.curve;
    view.transitionMs = &def.transitionMs;
    view.revision = &def.revision;
    return view;
}

bool BindingTable::AnyAttachmentOf(BindingId id) const {
    for (const Attachment& a : m_attachments) {
        if (a.used && a.bindingId == id) {
            return true;
        }
    }
    return false;
}

void BindingTable::EvaluateBusAttachments(const ParamSystem& params,
                                          BusTargetStore& store) {
    for (const auto& row : m_attachments)
        if (row.used && row.scopeKind == kTargetScopeBus &&
            m_definitions[row.definitionIndex].sourceKind == BindingSourceKind::Variable)
            m_dependencies.Mark(row.dependency);
    EvaluateDirtyBusAttachments(params, store);
    // Legacy callers can seed the store without attaching a binding.
    if (m_attachmentCount == 0 && store.HasVariableContributions()) {
        for (std::uint32_t bus = 0; bus < store.MaxBuses(); ++bus)
            for (std::uint32_t target = 0; target < kBusTargetCount; ++target)
                if (store.SlotActive(bus, target, BusTargetStore::kSlotVariableIndex))
                    store.ClearSlot(bus, target, BusTargetStore::kSlotVariableIndex);
    }
}

void BindingTable::EvaluateDirtyBusAttachments(const ParamSystem& params, BusTargetStore& store) {
    BindingDirtyTarget target;
    for (std::uint32_t cursor = 0; m_dependencies.DirtyTargetAt(cursor, &target);) {
        BindingDependencyIdentity identity;
        if (!m_dependencies.TargetIdentity(target, &identity) || identity.scope != kTargetScopeBus ||
            identity.detail != static_cast<std::uint32_t>(BindingSourceKind::Variable)) {
            ++cursor;
            continue;
        }
        const auto version = m_dependencies.TargetVersion(target);
        const auto bus = static_cast<std::uint32_t>(identity.object);
        if (identity.generation != 0 && store.ActiveGeneration(bus) != identity.generation) {
            if (m_projectionActive || !m_dependencies.CommitTarget(target, version)) ++cursor;
            continue;
        }
        double accumulation = 0.0;
        bool active = false;
        const auto* meta = FindTargetMeta(kTargetScopeBus, identity.target);
        for (auto token = m_dependencies.FirstContribution(target); token.Valid();
             token = m_dependencies.NextContribution(token)) {
            const auto& row = *static_cast<const Attachment*>(m_dependencies.Fact(token));
            const auto& definition = *row.dependencyDefinition;
            float input = 0.0f;
            if (row.meterFrozen || !params.ScopedVariableValue(nullptr, 0, definition.variableId, &input)) continue;
            const auto contribution = definition.Evaluate(input);
            accumulation = active ? CombineValue(meta != nullptr ? meta->domain : CombineDomain::AddDb,
                                                 accumulation, contribution) : contribution;
            active = true;
        }
        if (active && !std::isfinite(store.ReduceWithSlot(
                bus, identity.target, BusTargetStore::kSlotVariableIndex, accumulation))) {
            ++cursor;
            continue;
        }
        const bool accepted = active || store.SlotActive(bus, identity.target, BusTargetStore::kSlotVariableIndex);
        if (active) store.SetSlot(bus, identity.target, BusTargetStore::kSlotVariableIndex, accumulation);
        else if (accepted) store.ClearSlot(bus, identity.target, BusTargetStore::kSlotVariableIndex);
        if (accepted) {
            for (auto token = m_dependencies.FirstContribution(target); token.Valid();
                 token = m_dependencies.NextContribution(token)) {
                auto& row = *const_cast<Attachment*>(static_cast<const Attachment*>(m_dependencies.Fact(token)));
                const auto& definition = *row.dependencyDefinition;
                float input = 0.0f;
                const bool hasValue = !row.meterFrozen &&
                    params.ScopedVariableValue(nullptr, 0, definition.variableId, &input);
                const float contribution = hasValue ? definition.Evaluate(input) : 0.0f;
                if (hasValue != row.lastHadValue || (hasValue && contribution != row.lastContribution))
                    store.MarkVariableChanged(bus, identity.target, definition.transitionMs);
                row.lastHadValue = hasValue;
                row.lastContribution = contribution;
            }
        }
        if (m_projectionActive || !m_dependencies.CommitTarget(target, version)) ++cursor;
    }
    store.ConsumePendingInput();
}

void BindingTable::NoteBusVariableWrite(VnVariableId variableId,
                                        BusTargetStore& store) noexcept {
    m_dependencies.NotifyVariable(variableId);
    BindingDirtyTarget target;
    for (std::uint32_t index = 0; m_dependencies.DirtyTargetAt(index, &target); ++index) {
        BindingDependencyIdentity identity;
        if (m_dependencies.TargetIdentity(target, &identity) && identity.scope == kTargetScopeBus &&
            identity.detail == static_cast<std::uint32_t>(BindingSourceKind::Variable) &&
            (identity.generation == 0 || store.ActiveGeneration(static_cast<std::uint32_t>(identity.object)) == identity.generation))
            store.MarkPendingInput(static_cast<std::uint32_t>(identity.object), identity.target);
    }
}

VnResult BindingTable::CanApplyVoiceEffectInstance(
    BindingId id, VnVoiceHandle publicHandle,
    VnVoiceHandle renderHandle) const {
    const Definition* def = Find(id);
    if (def == nullptr) {
        return VN_UNKNOWN_ID;
    }
    for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && inst.bindingId == id &&
            (inst.publicHandle == publicHandle ||
             inst.renderHandle == renderHandle)) {
            return VN_IN_USE;
        }
    }
    return VN_OK;
}

VnResult BindingTable::ApplyVoiceEffectInstance(BindingId id,
                                                VnVoiceHandle publicHandle,
                                                VnVoiceHandle renderHandle,
                                                LogRing* log,
                                                VnEmitterHandle emitter) {
    const VnResult admitted =
        CanApplyVoiceEffectInstance(id, publicHandle, renderHandle);
    if (admitted != VN_OK) {
        if (admitted == kResultNoBinding) {
            LogPoolExhausted(log, kBindingPoolVoiceInstances,
                             m_voiceEffectInstances.size());
        }
        return admitted;
    }
    if (!PrepareEffectInstanceCapacity(std::uint64_t{m_voiceEffectInstanceCount} + 1u))
        return VN_OUT_OF_MEMORY;
    const Definition* def = Find(id);
    const std::uint32_t defIndex =
        m_definitions.IndexOf(def);
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (!inst.used) {
            if (!PrepareEffectDependency(m_voiceEffectInstances.IndexOf(&inst)))
                return kResultNoBinding;
            inst.used = true;
            inst.parked = false;
            inst.pending = false;
            inst.bindingId = id;
            inst.definitionIndex = defIndex;
            inst.publicHandle = publicHandle;
            inst.renderHandle = renderHandle;
            inst.dependencyEmitter = emitter;
            RegisterEffectDependency(inst);
            ++m_voiceEffectInstanceCount;
            return VN_OK;
        }
    }
    return kResultNoBinding;
}

VnResult BindingTable::ReserveVoiceEffectInstances(
    VnVoiceHandle publicHandle, const BindingId* ids, std::uint32_t count,
    LogRing* log) {
    if (publicHandle == 0) {
        return VN_INVALID_ARG;
    }
    for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && inst.publicHandle == publicHandle) {
            return VN_IN_USE;
        }
    }
    const VnResult preflight =
        CanReserveVoiceEffectInstances(ids, count, log);
    if (preflight != VN_OK) {
        return preflight;
    }
    if (!PrepareEffectInstanceCapacity(std::uint64_t{m_voiceEffectInstanceCount} + count))
        return VN_OUT_OF_MEMORY;
    std::uint32_t preparedCount = 0;
    for (std::uint32_t slot = 0; slot < m_voiceEffectInstances.size() && preparedCount < count; ++slot) {
        if (m_voiceEffectInstances[slot].used) continue;
        if (!PrepareEffectDependency(slot)) {
            for (std::uint32_t rollback = 0; rollback < slot; ++rollback)
                if (!m_voiceEffectInstances[rollback].used)
                {
                    auto& row = m_voiceEffectInstances[rollback];
                    m_dependencies.ReleaseBindingSlot(row.dependencySlot);
                    row.dependencySlot = UINT32_MAX;
                }
            return kResultNoBinding;
        }
        ++preparedCount;
    }
    std::uint32_t committed = 0;
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used || committed == count) {
            continue;
        }
        const Definition* def = Find(ids[committed]);
        inst.used = true;
        inst.parked = false;
        inst.pending = true;
        inst.bindingId = ids[committed];
        inst.definitionIndex =
            m_definitions.IndexOf(def);
        inst.publicHandle = publicHandle;
        inst.renderHandle = 0;
        ++committed;
        ++m_voiceEffectInstanceCount;
    }
    return VN_OK;
}

VnResult BindingTable::CanReserveVoiceEffectInstances(
    const BindingId* ids, std::uint32_t count, LogRing* log) const {
    if (count > 0 && ids == nullptr) {
        return VN_INVALID_ARG;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const Definition* def = Find(ids[i]);
        if (def == nullptr) {
            return VN_UNKNOWN_ID;
        }
        if (def->scopeKind != VN_TARGET_SCOPE_VOICE_EFFECT_PARAM) {
            return VN_INVALID_ARG;
        }
        for (std::uint32_t j = 0; j < i; ++j) {
            if (ids[j] == ids[i]) {
                return VN_IN_USE;
            }
        }
    }
    return VN_OK;
}

bool BindingTable::ActivateReservedVoiceEffectInstances(
    VnVoiceHandle publicHandle, VnVoiceHandle renderHandle, VnEmitterHandle emitter) {
    bool found = false;
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (!inst.used || inst.publicHandle != publicHandle) {
            continue;
        }
        if (inst.pending) {
            inst.pending = false;
            inst.renderHandle = renderHandle;
            inst.dependencyEmitter = emitter;
            RegisterEffectDependency(inst);
            found = true;
        } else if (!inst.parked && inst.renderHandle == renderHandle) {
            found = true;
        }
    }
    return found;
}

void BindingTable::CancelReservedVoiceEffectInstances(
    VnVoiceHandle publicHandle) {
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && inst.pending &&
            inst.publicHandle == publicHandle) {
            m_dependencies.CancelBindingPreparation(EffectDependencySlot(inst));
            RemoveEffectDependency(inst);
            m_dependencies.ReleaseBindingSlot(inst.dependencySlot);
            inst = VoiceEffectInstance{};
            --m_voiceEffectInstanceCount;
        }
    }
}

bool BindingTable::ClearVoiceEffectInstance(VnVoiceHandle renderHandle,
                                            BindingId id) {
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && !inst.pending && inst.bindingId == id &&
            inst.renderHandle == renderHandle) {
            m_dependencies.CancelBindingPreparation(EffectDependencySlot(inst));
            RemoveEffectDependency(inst);
            m_dependencies.ReleaseBindingSlot(inst.dependencySlot);
            inst = VoiceEffectInstance{};
            --m_voiceEffectInstanceCount;
            return true;
        }
    }
    return false;
}

void BindingTable::RemoveVoiceEffectInstances(VnVoiceHandle renderHandle) {
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && !inst.pending &&
            inst.renderHandle == renderHandle) {
            m_dependencies.CancelBindingPreparation(EffectDependencySlot(inst));
            RemoveEffectDependency(inst);
            m_dependencies.ReleaseBindingSlot(inst.dependencySlot);
            inst = VoiceEffectInstance{};
            --m_voiceEffectInstanceCount;
        }
    }
}

bool BindingTable::PrepareVoiceEffectInstancePark(VnVoiceHandle renderHandle) noexcept {
    for (std::uint32_t slot = 0; slot < m_voiceEffectInstances.size(); ++slot) {
        const auto& instance = m_voiceEffectInstances[slot];
        if (instance.used && !instance.pending && !instance.parked && instance.renderHandle == renderHandle &&
            !PrepareEffectDependency(slot)) {
            CancelVoiceEffectInstancePark(renderHandle);
            return false;
        }
    }
    return true;
}

void BindingTable::CancelVoiceEffectInstancePark(VnVoiceHandle renderHandle) noexcept {
    for (const auto& instance : m_voiceEffectInstances)
        if (instance.used && !instance.pending && !instance.parked && instance.renderHandle == renderHandle)
            m_dependencies.CancelBindingPreparation(EffectDependencySlot(instance));
}

void BindingTable::ParkVoiceEffectInstances(VnVoiceHandle renderHandle) {
    const bool prepared = PrepareVoiceEffectInstancePark(renderHandle);
    assert(prepared);
    if (!prepared) return;
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && !inst.pending && !inst.parked &&
            inst.renderHandle == renderHandle) {
            RemoveEffectDependency(inst);
            inst.parked = true; // keep the slot: the reservation stays booked
        }
    }
}

void BindingTable::ReturnVoiceEffectInstances(VnVoiceHandle oldRender,
                                              VnVoiceHandle newRender,
                                              VnEmitterHandle emitter) {
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && inst.parked && inst.renderHandle == oldRender) {
            inst.renderHandle = newRender;
            inst.parked = false;
            inst.dependencyEmitter = emitter;
            RegisterEffectDependency(inst);
        }
    }
}

void BindingTable::PurgeVoiceEffectInstancesOf(
    BindingId id) noexcept {
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (inst.used && inst.bindingId == id) {
            m_dependencies.CancelBindingPreparation(EffectDependencySlot(inst));
            RemoveEffectDependency(inst);
            m_dependencies.ReleaseBindingSlot(inst.dependencySlot);
            inst = VoiceEffectInstance{};
            --m_voiceEffectInstanceCount;
        }
    }
}

bool BindingTable::HasEffectParamBinding(EffectParamTargetStore::Scope scope,
                                         std::uint64_t objectId,
                                         std::uint32_t slot,
                                         std::uint32_t paramIndex,
                                         std::uint32_t effectType) const {
    if (scope == EffectParamTargetStore::Scope::Bus) {
        for (const Attachment& a : m_attachments) {
            if (!a.used || a.scopeKind != VN_TARGET_SCOPE_BUS_EFFECT_PARAM ||
                a.busIndex != objectId ||
                m_definitions[a.definitionIndex].sourceKind !=
                    BindingSourceKind::Variable) {
                continue;
            }
            const EffectParamLocator loc = UnpackEffectParamLocator(
                m_definitions[a.definitionIndex].targetId);
            if (loc.slot == slot && loc.paramIndex == paramIndex &&
                loc.effectType == effectType) {
                return true;
            }
        }
        return false;
    }
    for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
        // A parked instance's render handle is stale and its store records are
        // cleared, so it never matches a live direct-set objectId; skip it.
        if (!inst.used || inst.pending || inst.parked ||
            inst.renderHandle != objectId) {
            continue;
        }
        const EffectParamLocator loc = UnpackEffectParamLocator(
            m_definitions[inst.definitionIndex].targetId);
        if (loc.slot == slot && loc.paramIndex == paramIndex &&
            loc.effectType == effectType) {
            return true;
        }
    }
    return false;
}

void BindingTable::EvaluateEffectParamBindings(const ParamSystem& params,
                                               EffectParamTargetStore& store,
                                               const EmitterTable* emitters) {
    store.BeginContributionSweep();
    // Bus-effect attachments resolve at global scope (a bus has no emitter).
    for (const Attachment& a : m_attachments) {
        if (!a.used || a.scopeKind != VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
            continue;
        }
        const Definition& def = m_definitions[a.definitionIndex];
        if (def.sourceKind != BindingSourceKind::Variable) {
            continue; // probe rows fold in the probe sweep, not here
        }
        float input = 0.0f;
        if (!params.ScopedVariableValue(nullptr, 0, def.variableId, &input)) {
            continue; // unset Variable contributes nothing (returns to base)
        }
        const EffectParamLocator loc = UnpackEffectParamLocator(def.targetId);
        std::uint32_t revision = 0;
        if (a.targetGeneration != 0 && !store.ExactTargetRevision(EffectParamTargetStore::Scope::Bus,
            a.busIndex, a.targetGeneration, loc.slot, loc.paramIndex, loc.effectType, &revision)) continue;
        store.AccumulateBinding(EffectParamTargetStore::Scope::Bus, a.busIndex,
                                loc.slot, loc.paramIndex, loc.effectType,
                                def.Evaluate(input));
    }
    // Voice-effect apply instances resolve at the voice's emitter scope, like a
    // built-in voice target binding.
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        // A parked instance holds its slot as a reservation but drives nothing:
        // its store records were cleared at park and its render handle is stale.
        if (!inst.used || inst.pending || inst.parked) {
            continue;
        }
        (void)RefreshEffectDependencyEmitter(inst, params.EmitterHandleOf(inst.renderHandle));
        const Definition& def = m_definitions[inst.definitionIndex];
        float input = 0.0f;
        if (!params.ScopedVariableValue(emitters,
                                        params.EmitterHandleOf(inst.renderHandle),
                                        def.variableId, &input)) {
            continue;
        }
        const EffectParamLocator loc = UnpackEffectParamLocator(def.targetId);
        store.AccumulateBinding(EffectParamTargetStore::Scope::Voice,
                                inst.renderHandle, loc.slot, loc.paramIndex,
                                loc.effectType, def.Evaluate(input));
    }
    store.EndContributionSweep();
    if (m_projectionActive) return;
    BindingDirtyTarget target;
    for (std::uint32_t cursor = 0; m_dependencies.DirtyTargetAt(cursor, &target);) {
        BindingDependencyIdentity identity;
        if (m_dependencies.TargetIdentity(target, &identity) &&
            (identity.scope == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM || identity.scope == VN_TARGET_SCOPE_BUS_EFFECT_PARAM) &&
            identity.detail == static_cast<std::uint32_t>(BindingSourceKind::Variable) &&
            m_dependencies.CommitTarget(target, m_dependencies.TargetVersion(target))) continue;
        ++cursor;
    }
}

void BindingTable::EvaluateDirtyEffectParamBindings(const ParamSystem& params,
    EffectParamTargetStore& store, const EmitterTable* emitters) {
    BindingDirtyTarget target;
    for (std::uint32_t cursor = 0; m_dependencies.DirtyTargetAt(cursor, &target);) {
        BindingDependencyIdentity identity;
        if (!m_dependencies.TargetIdentity(target, &identity) ||
            (identity.scope != VN_TARGET_SCOPE_VOICE_EFFECT_PARAM && identity.scope != VN_TARGET_SCOPE_BUS_EFFECT_PARAM) ||
            identity.detail != static_cast<std::uint32_t>(BindingSourceKind::Variable)) {
            ++cursor;
            continue;
        }
        const bool bus = identity.scope == VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
        bool ready = true;
        if (!bus) {
            for (auto token = m_dependencies.FirstContribution(target); token.Valid();) {
                const auto next = m_dependencies.NextContribution(token);
                auto& row = *const_cast<VoiceEffectInstance*>(
                    static_cast<const VoiceEffectInstance*>(m_dependencies.Fact(token)));
                ready &= RefreshEffectDependencyEmitter(row, params.EmitterHandleOf(row.renderHandle));
                token = next;
            }
        }
        if (!ready) { ++cursor; continue; }
        const auto version = m_dependencies.TargetVersion(target);
        const auto location = UnpackEffectParamLocator(identity.target);
        const auto scope = bus ? EffectParamTargetStore::Scope::Bus : EffectParamTargetStore::Scope::Voice;
        std::uint32_t revision = 0;
        if (identity.generation != 0 && !store.ExactTargetRevision(scope, identity.object, identity.generation,
            location.slot, location.paramIndex, location.effectType, &revision)) {
            if (m_projectionActive || !m_dependencies.CommitTarget(target, version)) ++cursor;
            continue;
        }
        store.BeginTargetContribution(scope, identity.object, location.slot, location.paramIndex, location.effectType);
        for (auto token = m_dependencies.FirstContribution(target); token.Valid();
             token = m_dependencies.NextContribution(token)) {
            const auto* attachment = bus ? static_cast<const Attachment*>(m_dependencies.Fact(token)) : nullptr;
            const auto* instance = bus ? nullptr : static_cast<const VoiceEffectInstance*>(m_dependencies.Fact(token));
            const auto& definition = bus ? *attachment->dependencyDefinition : *instance->dependencyDefinition;
            float input = 0.0f;
            if (!params.ScopedVariableValue(bus ? nullptr : emitters, bus ? 0 : instance->dependencyEmitter,
                                             definition.variableId, &input)) continue;
            store.AccumulateBinding(scope, identity.object, location.slot, location.paramIndex,
                                    location.effectType, definition.Evaluate(input));
        }
        store.EndTargetContribution(scope, identity.object, location.slot, location.paramIndex, location.effectType);
        if (m_projectionActive || !m_dependencies.CommitTarget(target, version)) ++cursor;
    }
}

} // namespace harken
