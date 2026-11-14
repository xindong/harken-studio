#pragma once

#include "harken/render/EffectPool.h"
#include "harken/render/BusActivityPlan.h"
#include <cstring>
#include <algorithm>
#include <bit>
#include <cassert>
#include <span>
#include <vector>

namespace harken {

// Logic-owned identity survives DSP retirement. A chain shares its scope;
// genuinely mixed scopes use sparse exceptions without weakening ownership.
class BusEffectSettings {
public:
    struct Setting {
        std::uint32_t type = kEffectTypeNone;
        EffectObjectIdentity identity{};
        VnPackHandle owner = 0;
    };
    struct Write { std::uint32_t bus, slot; Setting value; };
    Setting Get(std::uint32_t bus, std::uint32_t slot) const noexcept {
        if (slot >= kMaxFxSlotsPerBus) return {};
        const auto found = Lower(bus);
        if (found == m_rows.end() || found->bus != bus || !(found->mask & (1u << slot))) return {};
        auto scope = found->common;
        const auto key = Key(bus, slot);
        const auto extra = Extra(key);
        if (extra != m_exceptions.end() && extra->key == key) scope = extra->scope;
        return {found->types[slot], {found->ids[slot], scope.project}, scope.owner};
    }
    bool Prepare(std::span<const Write> writes) noexcept {
        try {
            std::vector<Write> ordered;
            ordered.reserve(writes.size());
            for (const auto& write : writes) {
                if (write.value.type == kEffectTypeNone) continue;
                if (write.slot >= kMaxFxSlotsPerBus || !FindEffectRow(write.value.type)) return false;
                ordered.push_back(write);
            }
            std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.bus < b.bus; });
            std::size_t groups = m_rows.size(), exceptions = m_exceptions.size();
            for (std::size_t begin = 0; begin < ordered.size();) {
                const auto bus = ordered[begin].bus;
                const auto found = Lower(bus);
                const bool existing = found != m_rows.end() && found->bus == bus;
                if (!existing) ++groups;
                const auto common = existing ? found->common : ScopeOf(ordered[begin].value);
                unsigned mask = existing ? found->mask : 0;
                bool mixed = false;
                if (existing) for (std::uint32_t slot = 0; slot < kMaxFxSlotsPerBus; ++slot)
                    if (mask & (1u << slot)) mixed |= ScopeOf(Get(bus, slot)) != common;
                auto end = begin;
                for (; end < ordered.size() && ordered[end].bus == bus; ++end) {
                    mask |= 1u << ordered[end].slot;
                    mixed |= ScopeOf(ordered[end].value) != common;
                }
                // Includes the stored common even when its original slot was cleared.
                // Clear/reinsert can choose any incoming scope; this union bound
                // covers every operation prefix without simulating the transaction.
                if (mixed) {
                    const auto extra = std::popcount(mask);
                    if (extra > m_exceptions.max_size() - exceptions) return false;
                    exceptions += extra;
                }
                begin = end;
            }
            m_rows.reserve(groups);
            m_exceptions.reserve(exceptions);
            return true;
        } catch (...) { return false; }
    }
    void Set(std::uint32_t bus, std::uint32_t slot, Setting value) noexcept {
        assert(slot < kMaxFxSlotsPerBus);
        auto found = Lower(bus);
        const auto key = Key(bus, slot);
        auto extra = Extra(key);
        if (value.type == kEffectTypeNone) {
            if (extra != m_exceptions.end() && extra->key == key) m_exceptions.erase(extra);
            if (found == m_rows.end() || found->bus != bus || !(found->mask & (1u << slot))) return;
            found->coldMask &= ~(1u << slot);
            found->mask &= ~(1u << slot);
            --m_count;
            if (!found->coldMask) EraseColdExtra(bus);
            if (!found->mask) m_rows.erase(found);
            return;
        }
        assert(FindEffectRow(value.type));
        if (found == m_rows.end() || found->bus != bus) {
            RequireRoom(m_rows);
            Row row{}; row.bus = bus; row.common = ScopeOf(value);
            found = m_rows.insert(found, row);
        }
        const auto scope = ScopeOf(value);
        if (scope == found->common) {
            if (extra != m_exceptions.end() && extra->key == key) m_exceptions.erase(extra);
        } else if (extra != m_exceptions.end() && extra->key == key) extra->scope = scope;
        else { RequireRoom(m_exceptions); m_exceptions.insert(extra, {key, scope}); }
        if (!(found->mask & (1u << slot))) ++m_count;
        found->mask |= 1u << slot;
        found->types[slot] = static_cast<std::uint8_t>(value.type);
        found->ids[slot] = value.identity.effectId;
    }
    void ClearBus(std::uint32_t bus) noexcept {
        const auto found = Lower(bus);
        if (found == m_rows.end() || found->bus != bus) return;
        EraseColdExtra(bus);
        m_count -= std::popcount(found->mask);
        m_rows.erase(found);
        const auto begin = Extra(Key(bus, 0));
        auto end = begin;
        while (end != m_exceptions.end() && (end->key >> 32) == bus) ++end;
        m_exceptions.erase(begin, end);
    }
    bool HasOwner(std::uint32_t bus, VnPackHandle owner = 0) const noexcept {
        for (std::uint32_t slot = 0; slot < kMaxFxSlotsPerBus; ++slot) {
            const auto setting = Get(bus, slot);
            if (setting.owner != 0 && (owner == 0 || setting.owner == owner)) return true;
        }
        return false;
    }
    // ACK prepares every variable-size payload before changing any logical row.
    bool CommitDormant(std::span<const BusDormantCapture* const> captures, std::uint64_t publication,
        const void* context, BusTree::EffectRevisionReader revision) noexcept {
        struct Prepared {
            std::uint32_t bus;
            std::array<std::uint32_t, kMaxFxSlotsPerBus> values{}, seeds{};
            std::uint8_t mask = 0, rendered = 0;
            ColdExtra extra;
        };
        try {
            std::vector<Prepared> prepared;
            prepared.reserve(captures.size());
            std::size_t additional = 0;
            for (const auto* capture : captures) {
                const auto found = Lower(capture->bus.index);
                if (found == m_rows.end() || found->bus != capture->bus.index) continue;
                Prepared next{}; next.bus = capture->bus.index; next.extra.bus = next.bus;
                std::size_t payloadBytes = 0;
                for (std::uint32_t slot = 0; slot < kMaxFxSlotsPerBus; ++slot) {
                    const auto& state = capture->effects[slot];
                    const auto seed = capture->revisions[slot];
                    if (!(found->mask & (1u << slot)) || state.type != found->types[slot] || seed == 0 ||
                        revision(context, capture->bus, slot, state.type) != seed) continue;
                    next.mask |= 1u << slot;
                    if (state.hasRendered) next.rendered |= 1u << slot;
                    next.seeds[slot] = seed;
                    if (state.type == VN_EFFECT_GAIN) next.values[slot] = std::bit_cast<std::uint32_t>(state.values.gain);
                    else {
                        next.values[slot] = static_cast<std::uint32_t>(payloadBytes);
                        next.extra.sampleRate = state.sampleRate;
                        payloadBytes += ValueBytes(state.type);
                    }
                }
                next.extra.payload.resize(payloadBytes);
                for (std::uint32_t slot = 0; slot < kMaxFxSlotsPerBus; ++slot) {
                    const auto& state = capture->effects[slot];
                    if (!(next.mask & (1u << slot)) || state.type == VN_EFFECT_GAIN) continue;
                    const void* source = state.type == VN_EFFECT_THREE_BAND_TONE
                        ? static_cast<const void*>(&capture->tones[slot]) : &state.values;
                    std::memcpy(next.extra.payload.data() + next.values[slot], source, ValueBytes(state.type));
                }
                const auto extra = ColdLower(next.bus);
                if (payloadBytes != 0 && (extra == m_coldExtras.end() || extra->bus != next.bus)) ++additional;
                prepared.push_back(std::move(next));
            }
            if (additional > m_coldExtras.max_size() - m_coldExtras.size()) return false;
            m_coldExtras.reserve(m_coldExtras.size() + additional);
            for (auto& next : prepared) {
                auto found = Lower(next.bus);
                found->coldValues = next.values;
                found->coldSeeds = next.seeds;
                found->coldMask = next.mask;
                found->coldRendered = next.rendered;
                found->coldPublication = next.mask != 0 ? publication : 0;
                auto extra = ColdLower(next.bus);
                if (next.extra.payload.empty()) {
                    if (extra != m_coldExtras.end() && extra->bus == next.bus) m_coldExtras.erase(extra);
                } else if (extra != m_coldExtras.end() && extra->bus == next.bus) *extra = std::move(next.extra);
                else m_coldExtras.insert(extra, std::move(next.extra));
            }
            return true;
        } catch (...) { return false; }
    }
    BusTree::DormantEffectReceipt DormantReceipt(BusSlotRef bus, const void* context,
        BusTree::EffectRevisionReader revision) const noexcept {
        const auto found = Lower(bus.index);
        if (found == m_rows.end() || found->bus != bus.index) return {};
        std::uint8_t mask = 0;
        for (std::uint32_t slot = 0; slot < kMaxFxSlotsPerBus; ++slot)
            if ((found->coldMask & found->mask & (1u << slot)) &&
                revision(context, bus, slot, found->types[slot]) == found->coldSeeds[slot]) mask |= 1u << slot;
        return mask != 0 ? BusTree::DormantEffectReceipt{found->coldPublication, mask} : BusTree::DormantEffectReceipt{};
    }
    bool RestoreDormant(BusTree::Bus& bus, const void* context, BusTree::EffectRevisionReader revision) const noexcept {
        const auto receipt = DormantReceipt({bus.busIndex, bus.generation}, context, revision);
        if (receipt.mask == 0) return true;
        const auto found = Lower(bus.busIndex);
        const auto extra = ColdLower(bus.busIndex);
        for (std::uint32_t slot = 0; slot < kMaxFxSlotsPerBus; ++slot) {
            if (!(receipt.mask & (1u << slot)) || bus.fxSlotEffectRef[slot].type != found->types[slot]) continue;
            BuiltinEffectDormantState state;
            state.type = found->types[slot];
            state.hasRendered = (found->coldRendered & (1u << slot)) != 0;
            ToneDormantState tone;
            if (state.type == VN_EFFECT_GAIN) state.values.gain = std::bit_cast<float>(found->coldValues[slot]);
            else {
                if (extra == m_coldExtras.end() || extra->bus != bus.busIndex) return false;
                state.sampleRate = extra->sampleRate;
                const auto offset = found->coldValues[slot];
                const auto bytes = ValueBytes(state.type);
                if (offset > extra->payload.size() || bytes > extra->payload.size() - offset) return false;
                void* target = state.type == VN_EFFECT_THREE_BAND_TONE ? static_cast<void*>(&tone) : &state.values;
                std::memcpy(target, extra->payload.data() + offset, bytes);
            }
            if (!RestoreBuiltinEffectDormantState(bus.fxSlots[slot].state, state,
                state.type == VN_EFFECT_THREE_BAND_TONE ? &tone : nullptr)) return false;
        }
        return true;
    }
    void ConsumeDormant(std::uint32_t bus) noexcept {
        const auto found = Lower(bus);
        if (found == m_rows.end() || found->bus != bus || found->coldMask == 0) return;
        found->coldMask = 0;
        found->coldPublication = 0;
        EraseColdExtra(bus);
    }
    std::size_t Count() const noexcept { return m_count; }
    std::size_t StorageBytes() const noexcept {
        std::size_t bytes = m_rows.capacity() * sizeof(Row) + m_exceptions.capacity() * sizeof(Exception) +
            m_coldExtras.capacity() * sizeof(ColdExtra);
        for (const auto& extra : m_coldExtras) bytes += extra.payload.capacity();
        return bytes;
    }
private:
    struct Scope { VnObjectId project{}; VnPackHandle owner = 0; bool operator==(const Scope&) const = default; };
    struct Row {
        std::uint32_t bus = 0;
        std::uint8_t mask = 0, coldMask = 0, coldRendered = 0;
        std::array<std::uint8_t, kMaxFxSlotsPerBus> types{};
        std::array<VnObjectId, kMaxFxSlotsPerBus> ids{};
        Scope common;
        std::array<std::uint32_t, kMaxFxSlotsPerBus> coldValues{}, coldSeeds{};
        std::uint64_t coldPublication = 0;
    };
    static_assert(sizeof(Row) == 240);
    struct ColdExtra {
        std::uint32_t bus = 0, sampleRate = 0;
        std::vector<std::byte> payload;
    };
    static std::size_t ValueBytes(std::uint32_t type) noexcept {
        switch (type) {
        case VN_EFFECT_LIMITER: return sizeof(double);
        case VN_EFFECT_COMPRESSOR: return sizeof(BuiltinEffectDormantState::Values::compressor);
        case VN_EFFECT_FEEDBACK_DELAY: return sizeof(BuiltinEffectDormantState::Values::delay);
        case VN_EFFECT_THREE_BAND_TONE: return sizeof(ToneDormantState);
        default: return 0;
        }
    }
    std::vector<ColdExtra>::iterator ColdLower(std::uint32_t bus) noexcept {
        return std::lower_bound(m_coldExtras.begin(), m_coldExtras.end(), bus,
            [](const auto& row, auto value) { return row.bus < value; });
    }
    std::vector<ColdExtra>::const_iterator ColdLower(std::uint32_t bus) const noexcept {
        return std::lower_bound(m_coldExtras.begin(), m_coldExtras.end(), bus,
            [](const auto& row, auto value) { return row.bus < value; });
    }
    void EraseColdExtra(std::uint32_t bus) noexcept {
        const auto found = ColdLower(bus);
        if (found != m_coldExtras.end() && found->bus == bus) m_coldExtras.erase(found);
    }
    struct Exception { std::uint64_t key; Scope scope; };
    static_assert(kMaxFxSlotsPerBus == 8);
    static_assert([] { for (const auto& row : kEffectRegistry) if (row.effectType > UINT8_MAX) return false; return true; }());
    static Scope ScopeOf(const Setting& value) noexcept { return {value.identity.projectId, value.owner}; }
    static std::uint64_t Key(std::uint32_t bus, std::uint32_t slot) noexcept {
        return (static_cast<std::uint64_t>(bus) << 32) | slot;
    }
    template<class T> static void RequireRoom(const std::vector<T>& rows) noexcept {
        assert(rows.size() < rows.capacity());
        if (rows.size() == rows.capacity()) std::terminate();
    }
    std::vector<Row>::iterator Lower(std::uint32_t bus) noexcept {
        return std::lower_bound(m_rows.begin(), m_rows.end(), bus, [](const auto& row, auto value) { return row.bus < value; });
    }
    std::vector<Row>::const_iterator Lower(std::uint32_t bus) const noexcept {
        return std::lower_bound(m_rows.begin(), m_rows.end(), bus, [](const auto& row, auto value) { return row.bus < value; });
    }
    std::vector<Exception>::iterator Extra(std::uint64_t key) noexcept {
        return std::lower_bound(m_exceptions.begin(), m_exceptions.end(), key, [](const auto& row, auto value) { return row.key < value; });
    }
    std::vector<Exception>::const_iterator Extra(std::uint64_t key) const noexcept {
        return std::lower_bound(m_exceptions.begin(), m_exceptions.end(), key, [](const auto& row, auto value) { return row.key < value; });
    }
    std::vector<ColdExtra> m_coldExtras;
    std::vector<Row> m_rows;
    std::vector<Exception> m_exceptions;
    std::size_t m_count = 0;
};

} // namespace harken
