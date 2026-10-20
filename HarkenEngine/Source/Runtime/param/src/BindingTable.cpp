// Definition registry and attachment pool of the public binding system.
// Every entry validates exhaustively
// before touching pool state; replaces stage and install at the fixed
// update phase; bus attachments evaluate into the public Variable slot of the
// Variable slot with zero audio-thread involvement.
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
    if (!Reset(other.bindingCapacity_, other.targetCapacity_, other.keyCapacity_) ||
        !inputs_.ConstructThrough(other.inputs_.ConstructedCount()) ||
        !keys_.ConstructThrough(other.keys_.ConstructedCount()) ||
        !targets_.ConstructThrough(other.targets_.ConstructedCount())) throw std::bad_alloc{};
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
    root_ = other.root_;
    version_ = other.version_;
}

bool BindingDependencyIndex::Reset(std::uint32_t bindingCapacity, std::uint32_t targetCapacity,
    std::uint32_t keyCapacity) noexcept {
    if (memguard::IsAudioTagged() || root_ != UINT32_MAX || preparedBindingCount_ != 0 ||
        bindingCapacity > UINT32_MAX / 3u) return false;
    const auto needed = std::uint64_t{bindingCapacity} * 3u + targetCapacity;
    if (keyCapacity == 0 && needed > UINT32_MAX) return false;
    if (keyCapacity == 0) keyCapacity = static_cast<std::uint32_t>(needed);
    StableRowStorage<InputNode> inputs;
    StableRowStorage<Key> keys;
    StableRowStorage<DirtyRow> targets;
    if (!inputs.Reserve(bindingCapacity * 3u) || !keys.Reserve(keyCapacity) ||
        !targets.Reserve(targetCapacity)) return false;
    inputs_.Swap(inputs);
    keys_.Swap(keys);
    targets_.Swap(targets);
    std::vector<std::uint32_t>().swap(freeKeys_);
    std::vector<std::uint32_t>().swap(freeTargets_);
    std::vector<std::uint32_t>().swap(dirtyTargets_);
    std::vector<std::uint64_t>().swap(preparedBindings_);
    preparedBindingCount_ = 0;
    bindingCapacity_ = bindingCapacity;
    targetCapacity_ = targetCapacity;
    keyCapacity_ = keyCapacity;
    version_ = 0;
    return true;
}

bool BindingDependencyIndex::PrepareBinding(std::uint32_t slot) noexcept {
    if (memguard::IsAudioTagged() || slot >= bindingCapacity_ ||
        !inputs_.ConstructThrough((slot + 1u) * 3u)) return false;
    if (HasBindingPreparation(slot)) return true;
    if (inputs_[slot * 3u].generation == UINT32_MAX) return false;
    const auto keyNeed = std::uint64_t{preparedBindingCount_ + 1u} * 4u;
    const auto targetNeed = preparedBindingCount_ + 1u;
    const auto newKeys = std::uint64_t{keys_.ConstructedCount()} +
        (keyNeed > freeKeys_.size() ? keyNeed - freeKeys_.size() : 0u);
    const auto newTargets = std::uint64_t{targets_.ConstructedCount()} +
        (targetNeed > freeTargets_.size() ? targetNeed - freeTargets_.size() : 0u);
    if (newKeys > keyCapacity_ || newTargets > targetCapacity_) return false;
    try {
        preparedBindings_.resize((std::uint64_t{inputs_.ConstructedCount() / 3u} + 63u) / 64u, 0);
        freeKeys_.reserve(static_cast<std::size_t>(newKeys));
        while (keys_.ConstructedCount() < newKeys) {
            const auto next = keys_.ConstructedCount();
            if (!keys_.ConstructThrough(next + 1u)) return false;
            freeKeys_.push_back(next);
        }
        freeTargets_.reserve(static_cast<std::size_t>(newTargets));
        dirtyTargets_.reserve(static_cast<std::size_t>(newTargets));
        while (targets_.ConstructedCount() < newTargets) {
            const auto next = targets_.ConstructedCount();
            if (!targets_.ConstructThrough(next + 1u)) return false;
            freeTargets_.push_back(next);
        }
    } catch (...) { return false; }
    preparedBindings_[slot / 64u] |= std::uint64_t{1} << (slot % 64u);
    ++preparedBindingCount_;
    return true;
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
        const auto current = DependencyOrder(Kind(keys_[key]), Identity(keys_[key]));
        if (wanted == current) return key;
        key = wanted < current ? keys_[key].left : keys_[key].right;
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
        const auto candidate = DependencyOrder(Kind(keys_[current]), Identity(keys_[current]));
        if (wanted == candidate) return current;
        parent = current;
        left = wanted < candidate;
        current = left ? keys_[current].left : keys_[current].right;
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
    needed += Find(inputKind, read(fact, inputKind)) == UINT32_MAX;
    if (local && !probe) needed += Find(BindingDependencyKind::Local, read(fact, BindingDependencyKind::Local)) == UINT32_MAX;
    if (liveDefinition) needed += Find(BindingDependencyKind::Definition, read(fact, BindingDependencyKind::Definition)) == UINT32_MAX;
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
    const auto target = EnsureKey(BindingDependencyKind::Target, slot * 3u);
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
    LinkSource(slot * 3u, EnsureKey(probe ? BindingDependencyKind::Probe : BindingDependencyKind::Global, slot * 3u));
    if (local && !probe) LinkSource(slot * 3u + 1u, EnsureKey(BindingDependencyKind::Local, slot * 3u + 1u));
    if (liveDefinition) LinkSource(slot * 3u + 2u, EnsureKey(BindingDependencyKind::Definition, slot * 3u + 2u));
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

std::size_t BindingDependencyIndex::StorageBytes() const noexcept {
    return inputs_.RequestedBytes() + keys_.RequestedBytes() + targets_.RequestedBytes() +
        (freeKeys_.capacity() + freeTargets_.capacity() + dirtyTargets_.capacity()) * sizeof(std::uint32_t) +
        preparedBindings_.capacity() * sizeof(std::uint64_t);
}

BindingTable::BindingTable(std::uint32_t maxDefinitions,
                           std::uint32_t maxAttachments,
                           std::uint32_t maxBuses, std::uint32_t renderBindingCapacity,
                           std::uint32_t targetCapacity)
    : m_renderBindingCapacity(renderBindingCapacity),
      m_definitions(maxDefinitions),
      m_attachments(maxAttachments),
      m_projectionAttachments(maxAttachments),
      m_voiceEffectInstances(maxAttachments) {
    (void)maxBuses;
    const auto bindings = std::uint64_t{renderBindingCapacity} + 2ull * maxAttachments;
    const auto targets = targetCapacity == 0 ? bindings + 3ull * kVariableSliceTargetHardLimit : targetCapacity;
    if (bindings > UINT32_MAX || targets > UINT32_MAX ||
        !m_dependencies.Reset(static_cast<std::uint32_t>(bindings), static_cast<std::uint32_t>(targets)))
        throw std::bad_alloc{};
}

BindingTable::BindingTable(const BindingTable& other)
    : m_dependencies(other.m_dependencies), m_renderBindingCapacity(other.m_renderBindingCapacity),
      m_definitions(other.m_definitions), m_attachments(other.m_attachments),
      m_projectionAttachments(other.m_projectionAttachments), m_projectionActive(other.m_projectionActive),
      m_voiceEffectInstances(other.m_voiceEffectInstances),
      m_definitionCount(other.m_definitionCount), m_attachmentCount(other.m_attachmentCount),
      m_voiceEffectInstanceCount(other.m_voiceEffectInstanceCount),
      m_definitionHighWater(other.m_definitionHighWater), m_definitionRefusedCount(other.m_definitionRefusedCount),
      m_attachmentHighWater(other.m_attachmentHighWater), m_attachmentRefusedCount(other.m_attachmentRefusedCount) {
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

bool BindingTable::PrepareAttachmentDependency(std::uint32_t slot, std::uint32_t scope) noexcept {
    return scope == kTargetScopeVoice || m_dependencies.PrepareBinding(m_renderBindingCapacity + slot);
}

void BindingTable::RegisterAttachmentDependency(Attachment& row) noexcept {
    row.dependencyDefinition = &m_definitions[row.definitionIndex];
    if (row.scopeKind == kTargetScopeVoice && row.dependencyDefinition->sourceKind == BindingSourceKind::Variable) return;
    const auto slot = m_renderBindingCapacity + static_cast<std::uint32_t>(&row - m_attachments.data());
    const bool registered = m_dependencies.Register(slot, &row, &ReadAttachmentDependency, slot, false, true,
        row.dependencyDefinition->sourceKind == BindingSourceKind::MeterProbe, &row.dependency);
    assert(registered);
    (void)registered;
}

bool BindingTable::PrepareEffectDependency(std::uint32_t slot) noexcept {
    return m_dependencies.PrepareBinding(m_renderBindingCapacity + static_cast<std::uint32_t>(m_attachments.size()) + slot);
}

std::uint32_t BindingTable::EffectDependencySlot(const VoiceEffectInstance& row) const noexcept {
    return m_renderBindingCapacity + static_cast<std::uint32_t>(m_attachments.size()) +
        static_cast<std::uint32_t>(&row - m_voiceEffectInstances.data());
}

void BindingTable::RegisterEffectDependency(VoiceEffectInstance& row) noexcept {
    row.dependencyDefinition = &m_definitions[row.definitionIndex];
    const auto slot = EffectDependencySlot(row);
    const bool registered = m_dependencies.Register(slot, &row, &ReadEffectDependency, slot, true, true, false, &row.dependency);
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
    if (!PrepareEffectDependency(static_cast<std::uint32_t>(&row - m_voiceEffectInstances.data()))) return false;
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
    // The definition count tracks used slots exactly, so a full count is
    // exactly "no free slot" — the same refusal the slot scan produces.
    if (m_definitionCount >= m_definitions.size()) {
        return kResultNoBinding;
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
        static_cast<std::uint32_t>(def - m_definitions.data());
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
        static_cast<std::uint32_t>(def - m_definitions.data());
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
        break;
    case PreparedDefinitionToken::Operation::Remove:
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
    if (m_attachmentCount >= m_attachments.size()) {
        return kResultNoBinding;
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
    const Definition* def = Find(id);
    for (std::uint32_t i = 0; i < m_attachments.size(); ++i) {
        Attachment& a = m_attachments[i];
        if (a.used) {
            continue;
        }
        if (!PrepareAttachmentDependency(i, scopeKind)) return kResultNoBinding;
        a.used = true;
        a.bindingId = id;
        a.definitionIndex =
            static_cast<std::uint32_t>(def - m_definitions.data());
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
    for (Attachment& a : m_attachments) {
        if (a.used && a.bindingId == id && a.scopeKind == scopeKind &&
            a.objectId == objectId) {
            // A departing BUS contribution ramps back to neutral over the
            // definition's transitionMs (section 5.3): mark the target
            // dirty with that ramp folded, so the next publication carries
            // the exit. An attachment that contributed nothing (unset
            // Variable, frozen) departs silently — nothing to ramp.
            if (store != nullptr && a.scopeKind == kTargetScopeBus &&
                a.lastHadValue && (a.targetGeneration == 0 ||
                    store->ActiveGeneration(a.busIndex) == a.targetGeneration)) {
                const Definition& def = m_definitions[a.definitionIndex];
                store->MarkVariableChanged(a.busIndex, def.targetId,
                                           def.transitionMs);
            }
            if (a.dependency.Valid()) (void)m_dependencies.Remove(a.dependency);
            a = Attachment{};
            --m_attachmentCount;
            return VN_OK;
        }
    }
    return VN_UNKNOWN_ID;
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
    if (!PrepareAttachmentDependency(slotIndex, scopeKind)) return kResultNoBinding;
    Attachment value;
    value.used = true;
    value.bindingId = id;
    value.definitionIndex =
        !definitionWillBeAdded && def != nullptr
            ? static_cast<std::uint32_t>(def - m_definitions.data())
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
    for (std::uint32_t i = 0; i < m_attachments.size(); ++i) {
        const Attachment& attachment = m_attachments[i];
        if (attachment.used && attachment.bindingId == id &&
            attachment.scopeKind == scopeKind &&
            attachment.objectId == objectId) {
            out->m_operation = PreparedAttachmentToken::Operation::Remove;
            out->m_slotIndex = i;
            out->m_value = attachment;
            return VN_OK;
        }
    }
    return VN_UNKNOWN_ID;
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
            m_dependencies.CancelBindingPreparation(m_renderBindingCapacity + token->m_slotIndex);
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
        if (slot.dependency.Valid()) (void)m_dependencies.Remove(slot.dependency);
        slot = Attachment{};
        --m_attachmentCount;
    }
    *token = PreparedAttachmentToken{};
}

const BindingTable::Definition* BindingTable::Find(BindingId id) const {
    for (const Definition& def : m_definitions) {
        if (def.used && def.sourceKind == BindingSourceKind::Variable &&
            def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

BindingTable::Definition* BindingTable::FindMutable(BindingId id) {
    for (Definition& def : m_definitions) {
        if (def.used && def.sourceKind == BindingSourceKind::Variable &&
            def.id == id) {
            return &def;
        }
    }
    return nullptr;
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
    if (m_definitionCount >= m_definitions.size()) {
        return kResultNoBinding;
    }
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
    if (m_attachmentCount >= m_attachments.size()) {
        return kResultNoBinding;
    }
    for (Attachment& a : m_attachments) {
        if (a.used) {
            continue;
        }
        const auto slot = static_cast<std::uint32_t>(&a - m_attachments.data());
        if (!m_dependencies.PrepareBinding(m_renderBindingCapacity + slot)) return kResultNoBinding;
        a = Attachment{};
        a.used = true;
        a.bindingId = id;
        a.definitionIndex =
            static_cast<std::uint32_t>(def - m_definitions.data());
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

BindingTable::Definition* BindingTable::FindProbeDefinition(
    BindingId id) noexcept {
    for (Definition& def : m_definitions) {
        if (def.used && def.sourceKind == BindingSourceKind::MeterProbe &&
            def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

const BindingTable::Definition* BindingTable::FindProbeDefinition(
    BindingId id) const noexcept {
    for (const Definition& def : m_definitions) {
        if (def.used && def.sourceKind == BindingSourceKind::MeterProbe &&
            def.id == id) {
            return &def;
        }
    }
    return nullptr;
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
        if (row.dependency.Valid()) (void)m_dependencies.Remove(row.dependency);
        row = Attachment{};
        --m_attachmentCount;
    }
}

void BindingTable::ReleaseProbeDefinition(Definition& row) noexcept {
    if (row.used && row.sourceKind == BindingSourceKind::MeterProbe) {
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
        if (a.used && a.bindingId == id && a.scopeKind == scopeKind &&
            a.objectId == objectId) {
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
        float accumulation = 0.0f;
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
    for (const VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (!inst.used) {
            return VN_OK;
        }
    }
    return kResultNoBinding;
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
    const Definition* def = Find(id);
    const std::uint32_t defIndex =
        static_cast<std::uint32_t>(def - m_definitions.data());
    for (VoiceEffectInstance& inst : m_voiceEffectInstances) {
        if (!inst.used) {
            if (!PrepareEffectDependency(static_cast<std::uint32_t>(&inst - m_voiceEffectInstances.data())))
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
    std::uint32_t preparedCount = 0;
    for (std::uint32_t slot = 0; slot < m_voiceEffectInstances.size() && preparedCount < count; ++slot) {
        if (m_voiceEffectInstances[slot].used) continue;
        if (!PrepareEffectDependency(slot)) {
            for (std::uint32_t rollback = 0; rollback < slot; ++rollback)
                if (!m_voiceEffectInstances[rollback].used)
                    m_dependencies.CancelBindingPreparation(m_renderBindingCapacity +
                        static_cast<std::uint32_t>(m_attachments.size()) + rollback);
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
            static_cast<std::uint32_t>(def - m_definitions.data());
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
    const std::uint32_t freeCount =
        static_cast<std::uint32_t>(m_voiceEffectInstances.size()) -
        m_voiceEffectInstanceCount;
    if (count > freeCount) {
        LogPoolExhausted(log, kBindingPoolVoiceInstances,
                         m_voiceEffectInstances.size());
        return kResultNoBinding;
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
