// The compiled effect chain: a storage-independent, immutable compile artifact.
//
// An authored object carries an effect signal graph (nodes and edges, no slot
// concept). Compiling turns the graph selected for an occurrence into this: an
// ordered entry list plus the resource demand a loader gates against. The
// artifact is deliberately host-independent — it names no physical slot, no
// engine instance and no session budget — because the same chain must lower to
// different execution backends without the artifact or its fingerprint moving.
// Which slot an entry ends up in is the lowering stage's answer and never enters
// the chain or any fingerprint.
//
// A chain is stored once per declaring object and referenced by index, so two
// occurrences that select the same object's graph share one artifact. Two
// different objects that happen to author identical chains are two artifacts
// here; recognizing them as one is a content-level deduplication this stage does
// not attempt.
#ifndef HARKEN_PROJECT_SCHEMA_EFFECT_CHAIN_H
#define HARKEN_PROJECT_SCHEMA_EFFECT_CHAIN_H

#include "harken_project_schema/Model.h"

#include <cstdint>
#include <vector>

namespace harkenprojectschema {

// One effect in a compiled chain, in execution order.
//
// stableNodeId is the authored node's identity, kept so a parameter binding, a
// diagnostic or a later incremental diff can name the authored node an entry
// came from. It is lineage, not audible content: renumbering nodes must not move
// the semantic fingerprint, so the semantic hash excludes it.
//
// paramBlockRef indexes the owning chain's paramBlocks. The indirection is what
// lets identical parameter payloads be shared without duplicating them into
// every entry; the index itself is an addressing detail, so a semantic hash
// covers the block's contents rather than the index.
//
// flags is reserved: no authoring surface produces a non-zero value yet, and its
// bit meanings are append-only. It is carried and hashed now so that the first
// real flag changes the semantic fingerprint without a format change.
struct CompiledEffectChainEntry {
    GraphNodeId stableNodeId = kReservedGraphNodeId;
    EffectTypeId effectType = kReservedEffectType;
    std::uint32_t paramBlockRef = 0;
    std::uint32_t flags = 0;
    harkenstudio::ObjectId effectId{};
};

inline bool operator==(const CompiledEffectChainEntry& a,
                       const CompiledEffectChainEntry& b) {
    return a.stableNodeId == b.stableNodeId && a.effectType == b.effectType &&
           a.paramBlockRef == b.paramBlockRef && a.flags == b.flags &&
           a.effectId == b.effectId;
}
inline bool operator!=(const CompiledEffectChainEntry& a,
                       const CompiledEffectChainEntry& b) {
    return !(a == b);
}

// One entry's parameter payload, in declared order — the authored order of the
// node's parameters, carried verbatim. Values stay raw bits exactly as the model
// holds them, so equality, serialization and hashing all compare the same bits
// and no decimal rounding can enter.
struct EffectParamBlock {
    std::vector<EffectParam> params;
};

inline bool operator==(const EffectParamBlock& a, const EffectParamBlock& b) {
    return a.params == b.params;
}
inline bool operator!=(const EffectParamBlock& a, const EffectParamBlock& b) {
    return !(a == b);
}

// What one effect type demands of a loader, aggregated over a chain.
// instanceCount is how many entries of that type the chain holds — the number of
// pooled instances a load must admit. requiredParameterCount is zero when no
// parameter is authored and otherwise one past the largest authored parameter
// id, so parameter id 0 remains distinguishable from an empty payload. A loader
// reads this count against the type's registered parameter count.
struct EffectTypeDemand {
    EffectTypeId effectType = kReservedEffectType;
    std::uint32_t instanceCount = 0;
    std::uint64_t requiredParameterCount = 0;
};

inline bool operator==(const EffectTypeDemand& a, const EffectTypeDemand& b) {
    return a.effectType == b.effectType &&
           a.instanceCount == b.instanceCount &&
           a.requiredParameterCount == b.requiredParameterCount;
}
inline bool operator!=(const EffectTypeDemand& a, const EffectTypeDemand& b) {
    return !(a == b);
}

// Capability flags a chain requires of its execution host. Append-only: a new
// requirement takes the next bit and never renumbers an existing one.
inline constexpr std::uint32_t kEffectChainRequiresVoiceHost = 1u << 0;

// The resource demand of one chain — what a loader gates against, never a budget
// or an environment value itself. maxChainLength is how many entries must execute
// in series; requiredCapabilities is the flag set above; perTypeDemand carries
// one row per distinct effect type, ascending by effect type so the triple is a
// deterministic function of the chain.
//
// The demand is a property of the selected chain alone. It deliberately holds no
// session budget and no toolchain identity, so raising a budget to admit a chain
// never changes the chain or its fingerprint.
struct EffectChainResources {
    std::uint32_t maxChainLength = 0;
    std::uint32_t requiredCapabilities = 0;
    std::vector<EffectTypeDemand> perTypeDemand;
};

inline bool operator==(const EffectChainResources& a,
                       const EffectChainResources& b) {
    return a.maxChainLength == b.maxChainLength &&
           a.requiredCapabilities == b.requiredCapabilities &&
           a.perTypeDemand == b.perTypeDemand;
}
inline bool operator!=(const EffectChainResources& a,
                       const EffectChainResources& b) {
    return !(a == b);
}

// One compiled chain: the object whose declaration it came from, its entries in
// execution order, the parameter blocks those entries point at, and its resource
// demand. An entry count of zero is a real, meaningful chain — the declared-empty
// case, which overrides an inherited chain with no effects at all — and is
// distinct from an occurrence that selected no chain.
struct CompiledEffectChain {
    StableId owner = kUnmintedStableId;
    std::vector<CompiledEffectChainEntry> entries;
    std::vector<EffectParamBlock> paramBlocks;
    EffectChainResources resources;
};

inline bool operator==(const CompiledEffectChain& a,
                       const CompiledEffectChain& b) {
    return a.owner == b.owner && a.entries == b.entries &&
           a.paramBlocks == b.paramBlocks && a.resources == b.resources;
}
inline bool operator!=(const CompiledEffectChain& a,
                       const CompiledEffectChain& b) {
    return !(a == b);
}

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_EFFECT_CHAIN_H
