#include "harken/core/CurrentSelection.h"

#include <algorithm>
#include <limits>

namespace harken {
namespace {

constexpr std::uint8_t kDomainSelection = 0x53;
constexpr std::uint8_t kAlgorithmVersion = 0x1A;
constexpr std::uint8_t kWorldSemantic = 0x01;
constexpr std::uint8_t kProgramStableId = 0x02;
constexpr std::uint8_t kProgramSemantic = 0x03;
constexpr std::uint8_t kMemoryScope = 0x04;
constexpr std::uint8_t kEmitterSlot = 0x05;
constexpr std::uint8_t kEmitterGeneration = 0x06;
constexpr std::uint8_t kResetEpoch = 0x07;
constexpr std::uint8_t kRootStableId = 0x08;
constexpr std::uint8_t kCallerSeed = 0x09;
constexpr std::uint8_t kAcceptedPostOrdinal = 0x0A;
constexpr std::uint8_t kPathArray = 0x0B;
constexpr std::uint8_t kPathNode = 0x0C;
constexpr std::uint8_t kPathKind = 0x0D;
constexpr std::uint8_t kPathObjectId = 0x0E;
constexpr std::uint8_t kPathTrackId = 0x0F;
constexpr std::uint8_t kPathClipId = 0x10;
constexpr std::uint8_t kPathClipGuidHigh = 0x11;
constexpr std::uint8_t kPathClipGuidLow = 0x12;
constexpr std::uint8_t kPathChildOrdinal = 0x13;
constexpr std::uint8_t kOccurrenceOrdinal = 0x14;
constexpr std::uint8_t kLoopIteration = 0x15;
constexpr std::uint8_t kQueueCycleOrdinal = 0x16;
constexpr std::uint8_t kSelectionOrdinal = 0x17;
constexpr std::uint8_t kDiscipline = 0x19;

constexpr std::size_t kFixedSeedBytes = 192;
constexpr std::size_t kPathNodeBytes = 108;

bool ValidPlayableKind(PlayableKindV1 kind) noexcept {
    return kind == PlayableKindV1::Sound ||
           kind == PlayableKindV1::Container ||
           kind == PlayableKindV1::Track ||
           kind == PlayableKindV1::Piece;
}

bool ValidSeedInput(const SelectionSeedInputV1& input) noexcept {
    if (input.worldSemanticFingerprint == 0 ||
        input.programStableId == VnObjectId{} ||
        input.programSemanticFingerprint == 0 || input.rootStableId == VnObjectId{} ||
        input.acceptedPostOrdinal == 0 ||
        input.pathCount > kSelectionPathNodeCapacity ||
        (input.pathCount != 0 && input.path == nullptr) ||
        (input.discipline != SelectionDisciplineV1::Draw &&
         input.discipline != SelectionDisciplineV1::Rotation &&
         input.discipline != SelectionDisciplineV1::Sequence)) {
        return false;
    }
    if (input.memoryScope == MemoryScopeV1::Global) {
        if (input.emitterSlot != UINT32_MAX || input.emitterGeneration != 0) {
            return false;
        }
    } else if (input.memoryScope == MemoryScopeV1::Emitter) {
        if (input.emitterSlot == UINT32_MAX || input.emitterGeneration == 0) {
            return false;
        }
    } else {
        return false;
    }
    for (std::uint32_t i = 0; i < input.pathCount; ++i) {
        if (!ValidPlayableKind(input.path[i].kind) ||
            input.path[i].objectId == VnObjectId{}) {
            return false;
        }
    }
    return true;
}

class ByteWriter {
public:
    ByteWriter(std::uint8_t* output, std::size_t capacity) noexcept
        : output_(output), capacity_(capacity) {}

    bool U8(std::uint8_t value) noexcept {
        if (size_ >= capacity_) return false;
        output_[size_++] = value;
        return true;
    }

    bool U16(std::uint16_t value) noexcept {
        return U8(static_cast<std::uint8_t>(value)) &&
               U8(static_cast<std::uint8_t>(value >> 8));
    }

    bool U32(std::uint32_t value) noexcept {
        return U16(static_cast<std::uint16_t>(value)) &&
               U16(static_cast<std::uint16_t>(value >> 16));
    }

    bool U64(std::uint64_t value) noexcept {
        return U32(static_cast<std::uint32_t>(value)) &&
               U32(static_cast<std::uint32_t>(value >> 32));
    }

    bool LaneU32(std::uint8_t tag, std::uint32_t value) noexcept {
        return U8(tag) && U8(4) && U16(0) && U32(value);
    }

    bool LaneU64(std::uint8_t tag, std::uint64_t value) noexcept {
        return U8(tag) && U8(8) && U16(0) && U64(value);
    }

    bool LaneObjectId(std::uint8_t tag, VnObjectId value) noexcept {
        if (!U8(tag) || !U8(16) || !U16(0)) return false;
        for (const auto byte : ObjectIdBytes(value)) {
            if (!U8(byte)) return false;
        }
        return true;
    }

    std::size_t Size() const noexcept { return size_; }

private:
    std::uint8_t* output_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

bool WriteSeed(const SelectionSeedInputV1& input,
               ByteWriter& writer) noexcept {
    const std::uint64_t callerSeed =
        input.callerSeed == 0 ? SplitMix64::kDefaultSeed : input.callerSeed;
    if (!writer.U8(kDomainSelection) || !writer.U8(1) || !writer.U16(0) ||
        !writer.LaneU32(kAlgorithmVersion, 1) ||
        !writer.LaneU64(kWorldSemantic, input.worldSemanticFingerprint) ||
        !writer.LaneObjectId(kProgramStableId, input.programStableId) ||
        !writer.LaneU64(kProgramSemantic,
                        input.programSemanticFingerprint) ||
        !writer.LaneU32(kMemoryScope,
                        static_cast<std::uint32_t>(input.memoryScope)) ||
        !writer.LaneU32(kEmitterSlot, input.emitterSlot) ||
        !writer.LaneU32(kEmitterGeneration, input.emitterGeneration) ||
        !writer.LaneU64(kResetEpoch, input.resetEpoch) ||
        !writer.LaneObjectId(kRootStableId, input.rootStableId) ||
        !writer.LaneU64(kCallerSeed, callerSeed) ||
        !writer.LaneU64(kAcceptedPostOrdinal, input.acceptedPostOrdinal) ||
        !writer.U8(kPathArray) || !writer.U8(1) || !writer.U16(0) ||
        !writer.U32(input.pathCount)) {
        return false;
    }
    for (std::uint32_t i = 0; i < input.pathCount; ++i) {
        const SelectionPathNodeV1& node = input.path[i];
        if (!writer.U8(kPathNode) || !writer.U8(1) || !writer.U16(0) ||
            !writer.U32(100) ||
            !writer.LaneU32(kPathKind,
                            static_cast<std::uint32_t>(node.kind)) ||
            !writer.LaneObjectId(kPathObjectId, node.objectId) ||
            !writer.LaneObjectId(kPathTrackId, node.trackId) ||
            !writer.LaneObjectId(kPathClipId, node.clipId) ||
            !writer.LaneU64(kPathClipGuidHigh, node.clipGuidHigh) ||
            !writer.LaneU64(kPathClipGuidLow, node.clipGuidLow) ||
            !writer.LaneU32(kPathChildOrdinal, node.childOrdinal)) {
            return false;
        }
    }
    return writer.LaneU32(kOccurrenceOrdinal, input.occurrenceOrdinal) &&
           writer.LaneU32(kLoopIteration, input.loopIteration) &&
           writer.LaneU64(kQueueCycleOrdinal, input.queueCycleOrdinal) &&
           writer.LaneU64(kSelectionOrdinal, input.selectionOrdinal) &&
           writer.LaneU32(kDiscipline,
                           static_cast<std::uint32_t>(input.discipline));
}

bool Contains(const std::uint32_t* values, std::uint32_t count,
              std::uint32_t value) noexcept {
    for (std::uint32_t i = 0; i < count; ++i) {
        if (values[i] == value) return true;
    }
    return false;
}

bool ValidChildren(const SelectionChildV1* children,
                   std::uint32_t childCount) noexcept {
    if (children == nullptr || childCount == 0) return false;
    for (std::uint32_t i = 0; i < childCount; ++i) {
        if (i != 0 && children[i - 1].stableChildOrdinal >=
                          children[i].stableChildOrdinal) {
            return false;
        }
    }
    return true;
}

bool ValidRecent(const SelectionChildV1* children, std::uint32_t childCount,
                 const std::uint32_t* recent,
                 std::uint32_t recentCount) noexcept {
    if (recentCount >= childCount ||
        (recentCount != 0 && recent == nullptr)) {
        return false;
    }
    for (std::uint32_t i = 0; i < recentCount; ++i) {
        if (Contains(recent, i, recent[i])) return false;
        bool found = false;
        for (std::uint32_t c = 0; c < childCount; ++c) {
            found = found || children[c].stableChildOrdinal == recent[i];
        }
        if (!found) return false;
    }
    return true;
}

bool AddWeight(std::uint64_t value, std::uint64_t* total) noexcept {
    if (*total > (std::numeric_limits<std::uint64_t>::max)() - value) {
        return false;
    }
    *total += value;
    return true;
}

}

std::size_t SelectionSeedByteCountV1(
    const SelectionSeedInputV1& input) noexcept {
    if (!ValidSeedInput(input)) return 0;
    return kFixedSeedBytes +
           static_cast<std::size_t>(input.pathCount) * kPathNodeBytes;
}

bool EncodeSelectionSeedV1(const SelectionSeedInputV1& input,
                           std::uint8_t* output,
                           std::size_t outputCapacity,
                           std::size_t* outputSize) noexcept {
    if (outputSize == nullptr) return false;
    const std::size_t required = SelectionSeedByteCountV1(input);
    *outputSize = required;
    if (required == 0 || output == nullptr || outputCapacity < required) {
        return false;
    }
    ByteWriter writer(output, outputCapacity);
    return WriteSeed(input, writer) && writer.Size() == required;
}

bool ComputeSelectionSeedV1(const SelectionSeedInputV1& input,
                            std::uint64_t* output) noexcept {
    if (output == nullptr) return false;
    std::uint8_t bytes[kFixedSeedBytes +
                       kSelectionPathNodeCapacity * kPathNodeBytes]{};
    std::size_t size = 0;
    if (!EncodeSelectionSeedV1(input, bytes, sizeof(bytes), &size)) {
        return false;
    }
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (std::size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    }
    *output = hash;
    return true;
}

SelectionStatusV1 SelectRandomDrawV1(
    const SelectionChildV1* children, std::uint32_t childCount,
    const std::uint32_t* recentChildOrdinals, std::uint32_t recentCount,
    SplitMix64* stream, std::uint32_t* selectedChildOrdinal) noexcept {
    if (!ValidChildren(children, childCount) ||
        !ValidRecent(children, childCount, recentChildOrdinals, recentCount) ||
        stream == nullptr || selectedChildOrdinal == nullptr) {
        return SelectionStatusV1::InvalidInput;
    }
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < childCount; ++i) {
        if (children[i].frequencyWeight == 0 ||
            Contains(recentChildOrdinals, recentCount,
                     children[i].stableChildOrdinal)) {
            continue;
        }
        if (!AddWeight(children[i].frequencyWeight, &total)) {
            return SelectionStatusV1::WeightOverflow;
        }
    }
    if (total == 0) return SelectionStatusV1::InvalidAvoid;
    const std::uint64_t draw = stream->Bounded(total);
    std::uint64_t cumulative = 0;
    for (std::uint32_t i = 0; i < childCount; ++i) {
        if (children[i].frequencyWeight == 0 ||
            Contains(recentChildOrdinals, recentCount,
                     children[i].stableChildOrdinal)) {
            continue;
        }
        cumulative += children[i].frequencyWeight;
        if (draw < cumulative) {
            *selectedChildOrdinal = children[i].stableChildOrdinal;
            return SelectionStatusV1::Ok;
        }
    }
    return SelectionStatusV1::InvalidInput;
}

SelectionStatusV1 BuildRandomRotationV1(
    const SelectionChildV1* children, std::uint32_t childCount,
    const std::uint32_t* initialRecentChildOrdinals,
    std::uint32_t initialRecentCount, SplitMix64* stream,
    std::uint32_t* remainingIndexScratch, std::uint32_t scratchCapacity,
    std::uint32_t* outputChildOrdinals,
    std::uint32_t outputCapacity) noexcept {
    if (!ValidChildren(children, childCount) ||
        !ValidRecent(children, childCount, initialRecentChildOrdinals,
                     initialRecentCount) ||
        stream == nullptr ||
        remainingIndexScratch == nullptr || outputChildOrdinals == nullptr) {
        return SelectionStatusV1::InvalidInput;
    }
    if (scratchCapacity < childCount || outputCapacity < childCount) {
        return SelectionStatusV1::OutputCapacity;
    }
    for (std::uint32_t i = 0; i < childCount; ++i) {
        if (children[i].orderWeight == 0 ||
            children[i].frequencyWeight != 0) {
            return SelectionStatusV1::InvalidInput;
        }
        remainingIndexScratch[i] = i;
    }
    std::uint32_t remaining = childCount;
    for (std::uint32_t position = 0; position < childCount; ++position) {
        std::uint64_t total = 0;
        for (std::uint32_t i = 0; i < remaining; ++i) {
            const SelectionChildV1& child =
                children[remainingIndexScratch[i]];
            const std::uint32_t currentWindowCount =
                std::min(initialRecentCount, position);
            const std::uint32_t initialWindowCount =
                initialRecentCount - currentWindowCount;
            const bool blockedByInitial =
                initialWindowCount != 0 &&
                Contains(initialRecentChildOrdinals + currentWindowCount,
                         initialWindowCount, child.stableChildOrdinal);
            const bool blockedByCurrent =
                currentWindowCount != 0 &&
                Contains(outputChildOrdinals +
                             (position - currentWindowCount),
                         currentWindowCount,
                         child.stableChildOrdinal);
            if (!blockedByInitial && !blockedByCurrent &&
                !AddWeight(child.orderWeight, &total)) {
                return SelectionStatusV1::WeightOverflow;
            }
        }
        if (total == 0) return SelectionStatusV1::InvalidAvoid;
        const std::uint64_t draw = stream->Bounded(total);
        std::uint64_t cumulative = 0;
        std::uint32_t selectedRemaining = UINT32_MAX;
        for (std::uint32_t i = 0; i < remaining; ++i) {
            const SelectionChildV1& child =
                children[remainingIndexScratch[i]];
            const std::uint32_t currentWindowCount =
                std::min(initialRecentCount, position);
            const std::uint32_t initialWindowCount =
                initialRecentCount - currentWindowCount;
            const bool blockedByInitial =
                initialWindowCount != 0 &&
                Contains(initialRecentChildOrdinals + currentWindowCount,
                         initialWindowCount, child.stableChildOrdinal);
            const bool blockedByCurrent =
                currentWindowCount != 0 &&
                Contains(outputChildOrdinals +
                             (position - currentWindowCount),
                         currentWindowCount,
                         child.stableChildOrdinal);
            if (blockedByInitial || blockedByCurrent) continue;
            cumulative += child.orderWeight;
            if (draw < cumulative) {
                selectedRemaining = i;
                outputChildOrdinals[position] = child.stableChildOrdinal;
                break;
            }
        }
        if (selectedRemaining == UINT32_MAX) {
            return SelectionStatusV1::InvalidInput;
        }
        for (std::uint32_t i = selectedRemaining + 1; i < remaining; ++i) {
            remainingIndexScratch[i - 1] = remainingIndexScratch[i];
        }
        --remaining;
    }
    return SelectionStatusV1::Ok;
}

SelectionStatusV1 SelectSequenceV1(
    const SelectionChildV1* children, std::uint32_t childCount,
    std::uint32_t cursor, bool alreadyStopped,
    SequenceEndPolicyV1 endPolicy, SequenceSelectionV1* output) noexcept {
    if (!ValidChildren(children, childCount) || output == nullptr ||
        cursor >= childCount ||
        (endPolicy != SequenceEndPolicyV1::Wrap &&
         endPolicy != SequenceEndPolicyV1::StopAtEnd)) {
        return SelectionStatusV1::InvalidInput;
    }
    if (alreadyStopped) return SelectionStatusV1::SequenceComplete;
    const SelectionChildV1* selected = nullptr;
    for (std::uint32_t i = 0; i < childCount; ++i) {
        std::uint32_t prior = 0;
        for (std::uint32_t j = 0; j < childCount; ++j) {
            prior += children[j].authoredStableOrder <
                             children[i].authoredStableOrder
                         ? 1u
                         : 0u;
        }
        if (prior == cursor) {
            if (selected != nullptr) return SelectionStatusV1::InvalidInput;
            selected = &children[i];
        }
    }
    if (selected == nullptr) return SelectionStatusV1::InvalidInput;
    output->selectedChildOrdinal = selected->stableChildOrdinal;
    if (cursor + 1 < childCount) {
        output->nextCursor = cursor + 1;
        output->stopped = false;
    } else if (endPolicy == SequenceEndPolicyV1::Wrap) {
        output->nextCursor = 0;
        output->stopped = false;
    } else {
        output->nextCursor = cursor;
        output->stopped = true;
    }
    return SelectionStatusV1::Ok;
}

}
