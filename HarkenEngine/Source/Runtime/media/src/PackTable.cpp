#include "harken/media/PackTable.h"

#include "harken/core/EngineConfig.h"
#include "harken/core/Utf8Path.h"
#include "harken/media/PackFormat.h"

#include <algorithm>
#include <map>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace harken {

namespace {

VnPackHandle MakePackHandle(std::uint32_t slot,
                                 std::uint32_t generation) {
    return (static_cast<std::uint64_t>(generation) << 32) | slot;
}


PackResourceClosureToken MakeResourceClosureToken(
    std::size_t rowIndex, std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32) |
           (static_cast<std::uint64_t>(rowIndex) + 1);
}

// Reads the whole file into a fresh 64-byte-aligned buffer. Returns an
// empty buffer on any I/O failure or on a size disagreement between the
// directory entry and the actual read.
std::uint8_t* ReadWholeFileAligned(const char* utf8Path,
                                   std::uint64_t* outBytes) {
    // The public path contract is UTF-8; the shared validator decodes it as
    // such regardless of the ANSI codepage and refuses ill-formed bytes
    // (returning the empty buffer that this function's callers read as
    // VN_BAD_PACK) instead of throwing out of the pack loader.
    std::filesystem::path path;
    if (!TryMakeUtf8Path(utf8Path, &path)) {
        return nullptr;
    }
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 ||
        size > static_cast<std::uintmax_t>(SIZE_MAX)) {
        return nullptr;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return nullptr;
    }
    std::uint8_t* buffer = static_cast<std::uint8_t*>(::operator new[](
        static_cast<std::size_t>(size),
        std::align_val_t{kPackBlobAlign}, std::nothrow));
    if (buffer == nullptr) {
        return nullptr;
    }
    in.read(reinterpret_cast<char*>(buffer),
            static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        ::operator delete[](buffer, std::align_val_t{kPackBlobAlign});
        return nullptr;
    }
    *outBytes = size;
    return buffer;
}

// Validates one parsed TOC entry against the container invariants: known
// format and flags, plausible PCM facts, exact blob size for f32 planar,
// aligned in-bounds blob placement after the TOC, and coherent loop
// metadata. Bounds math is ordered so no expression can wrap.
bool ValidateTocEntry(const PackTocEntry& e, std::uint64_t fileBytes,
                      std::uint64_t blobRegionStart) {
    if (e.sampleFormat != kPackSampleFormatF32Planar) {
        return false;
    }
    if ((e.flags & ~kPackKnownFlagsMask) != 0) {
        return false;
    }
    if (e.sampleRate == 0 || e.channelCount == 0 ||
        e.channelCount > kMaxChannels || e.frameCount == 0) {
        return false;
    }
    // Exact-size rule: for f32 planar the blob is precisely
    // frameCount * channelCount floats; any other byte count is an
    // inconsistent container, not padding.
    const std::uint64_t channels = e.channelCount;
    if (e.frameCount > UINT64_MAX / (channels * sizeof(float))) {
        return false;
    }
    if (e.blobBytes != e.frameCount * channels * sizeof(float)) {
        return false;
    }
    if (e.blobOffset % kPackBlobAlign != 0) {
        return false;
    }
    if (e.blobOffset < blobRegionStart || e.blobOffset > fileBytes ||
        e.blobBytes > fileBytes - e.blobOffset) {
        return false;
    }
    if ((e.flags & kPackFlagLoopable) != 0) {
        const std::uint64_t loopEnd =
            e.loopEndFrame == 0 ? e.frameCount : e.loopEndFrame;
        if (loopEnd > e.frameCount || e.loopStartFrame >= loopEnd) {
            return false;
        }
    } else if (e.loopStartFrame != 0 || e.loopEndFrame != 0) {
        // Loop frames without the loopable flag can only come from a
        // broken writer; v1 refuses the inconsistency.
        return false;
    }
    return true;
}

} // namespace

void PackTable::AlignedDeleter::operator()(std::uint8_t* p) const {
    ::operator delete[](p, std::align_val_t{kPackBlobAlign});
}

PackTable::PackTable(std::uint32_t capacity,
                     std::size_t retirementCapacity,
                     std::size_t resourceClosureCapacity) {
    if (resourceClosureCapacity > UINT32_MAX)
        throw std::length_error("pack resource closure capacity overflow");
    if (!m_slots.Reserve(capacity) ||
        !m_retiring.Reserve(retirementCapacity != 0 ? retirementCapacity : capacity) ||
        !m_resourceClosures.Reserve(static_cast<std::uint32_t>(resourceClosureCapacity)))
        throw std::bad_alloc{};
}

bool PackTable::PrepareContentStorage() noexcept {
    return m_resourceClosures.ConstructThrough(m_resourceClosures.Capacity()) &&
        m_slots.ConstructThrough(m_slots.Capacity());
}

VnResult PackTable::PrepareRetiringRow(
    VnPackHandle handle, std::size_t predecessorEntryCount,
    std::size_t successorEntryCount,
    RetiredReplace* outReservation,
    RetiringGeneration** outRow) noexcept {
    if (outRow == nullptr) {
        return VN_INVALID_ARG;
    }
    *outRow = nullptr;
    if (outReservation != nullptr) {
        *outReservation = {};
    }

    std::size_t selected = m_retiring.size();
    bool anyReusable = false;
    for (std::size_t i = 0; i < m_retiring.size(); ++i) {
        RetiringGeneration& row = m_retiring[i];
        if (row.state != RetiringState::Quarantined) {
            anyReusable = true;
        }
        if (row.state != RetiringState::Free) {
            continue;
        }
        if (selected == m_retiring.size()) {
            selected = i;
        }
        if (row.entries.capacity() >= predecessorEntryCount &&
            row.successorEntries.capacity() >= successorEntryCount) {
            selected = i;
            break;
        }
    }
    if (selected == m_retiring.size() && selected < m_retiring.Capacity()) {
        if (!m_retiring.ConstructThrough(selected + 1u)) return VN_BACKPRESSURE;
    }
    if (selected == m_retiring.size()) {
        return m_retiring.size() != 0 && !anyReusable
                   ? VN_RUNTIME_RECREATE_REQUIRED
                   : VN_BACKPRESSURE;
    }

    RetiringGeneration& row = m_retiring[selected];
    try {
        row.entries.reserve(predecessorEntryCount);
        row.successorEntries.reserve(successorEntryCount);
    } catch (...) {
        return VN_BACKPRESSURE;
    }
    row.state = RetiringState::Prepared;
    row.handle = handle;
    if (outReservation != nullptr) {
        outReservation->rowIndex = selected;
        outReservation->rowGeneration = row.generation;
    }
    *outRow = &row;
    return VN_OK;
}

PackTable::RetiringGeneration* PackTable::RetiringRowFor(
    const RetiredReplace& reservation) noexcept {
    if (!reservation.IsPrepared() ||
        reservation.rowIndex >= m_retiring.size()) {
        return nullptr;
    }
    RetiringGeneration& row = m_retiring[reservation.rowIndex];
    return row.state == RetiringState::Prepared &&
                   row.generation == reservation.rowGeneration
               ? &row
               : nullptr;
}

void PackTable::ResetRetiringRow(RetiringGeneration& row) noexcept {
    row.handle = 0;
    row.revisionKey = {};
    row.resourceToken = 0;
    row.closureRefCount = 0;
    row.bytes.reset();
    row.packEntries.clear();
    row.entries.clear();
    row.successorEntries.clear();
    row.state = RetiringState::Free;
}

void PackTable::ReleaseRetiringRow(RetiringGeneration& row) noexcept {
    ResetRetiringRow(row);
    if (row.generation == std::numeric_limits<std::uint64_t>::max()) {
        row.state = RetiringState::Quarantined;
    } else {
        ++row.generation;
    }
}

std::uint64_t* PackTable::ResourceClosureRefCountFor(
    PackResourceRevisionToken resourceToken) noexcept {
    if (resourceToken == 0) {
        return nullptr;
    }
    for (Slot& slot : m_slots) {
        if (slot.loaded && slot.resourceToken == resourceToken) {
            return &slot.closureRefCount;
        }
    }
    for (RetiringGeneration& row : m_retiring) {
        if ((row.state == RetiringState::Prepared ||
             row.state == RetiringState::Live) &&
            row.resourceToken == resourceToken) {
            return &row.closureRefCount;
        }
    }
    return nullptr;
}

const PackTable::ResourceClosureRow* PackTable::ResourceClosureRowFor(
    PackResourceClosureToken closureToken) const noexcept {
    const std::uint32_t encodedIndex =
        static_cast<std::uint32_t>(closureToken);
    const std::uint32_t generation =
        static_cast<std::uint32_t>(closureToken >> 32);
    if (encodedIndex == 0 || generation == 0) {
        return nullptr;
    }
    const std::size_t rowIndex =
        static_cast<std::size_t>(encodedIndex - 1);
    if (rowIndex >= m_resourceClosures.size()) {
        return nullptr;
    }
    const ResourceClosureRow& row = m_resourceClosures[rowIndex];
    return row.state == ResourceClosureState::Live &&
                   row.generation == generation
               ? &row
               : nullptr;
}

PackTable::ResourceClosureRow* PackTable::ResourceClosureRowFor(
    PackResourceClosureToken closureToken) noexcept {
    return const_cast<ResourceClosureRow*>(
        static_cast<const PackTable*>(this)->ResourceClosureRowFor(
            closureToken));
}

const PackResourceRevisionToken* PackTable::ResourceClosureTokens(
    std::size_t rowIndex) const noexcept {
    const auto& tokens = m_resourceClosures[rowIndex].tokens;
    return tokens == nullptr ? nullptr : tokens->data();
}

VnResult PackTable::SelectResourceClosureRow(std::size_t* out) const noexcept {
    if (m_slots.size() == 0) return VN_BACKPRESSURE;
    bool anyReusable = false;
    for (std::size_t i = 0; i < m_resourceClosures.size(); ++i) {
        const auto& row = m_resourceClosures[i];
        anyReusable = anyReusable || row.state != ResourceClosureState::Quarantined;
        if (row.state == ResourceClosureState::Free) { *out = i; return VN_OK; }
    }
    return m_resourceClosures.size() != 0 && !anyReusable ? VN_RUNTIME_RECREATE_REQUIRED : VN_BACKPRESSURE;
}

VnResult PackTable::AcquireResourceClosure(
    const PackResourceRevisionToken* resourceTokens,
    std::size_t resourceTokenCount,
    PackResourceClosureToken* outClosureToken) noexcept {
    if (outClosureToken == nullptr || resourceTokens == nullptr || resourceTokenCount == 0)
        return VN_INVALID_ARG;
    *outClosureToken = 0;
    std::size_t selected = 0;
    const auto available = SelectResourceClosureRow(&selected);
    if (available != VN_OK) return available;
    try {
        auto tokens = std::make_shared<std::vector<PackResourceRevisionToken>>();
        for (std::size_t i = 0; i < resourceTokenCount; ++i) {
            const auto token = resourceTokens[i];
            if (std::find(tokens->begin(), tokens->end(), token) != tokens->end()) continue;
            const auto* refs = ResourceClosureRefCountFor(token);
            if (refs == nullptr) return VN_CONTENT_NOT_LOADED;
            if (*refs == UINT64_MAX) return VN_RUNTIME_RECREATE_REQUIRED;
            if (tokens->size() == m_slots.size()) return VN_BACKPRESSURE;
            tokens->push_back(token);
        }
        return AcquirePreparedResourceClosure(tokens, outClosureToken);
    } catch (...) { return VN_BACKPRESSURE; }
}

VnResult PackTable::AcquirePreparedResourceClosure(
    const std::shared_ptr<const std::vector<PackResourceRevisionToken>>& resourceTokens,
    PackResourceClosureToken* outClosureToken) noexcept {
    if (outClosureToken == nullptr || resourceTokens == nullptr || resourceTokens->empty()) return VN_INVALID_ARG;
    *outClosureToken = 0;
    std::size_t selected = 0;
    const auto available = SelectResourceClosureRow(&selected);
    if (available != VN_OK) return available;
    if (resourceTokens->size() > m_slots.size()) return VN_BACKPRESSURE;
    for (std::size_t i = 0; i < resourceTokens->size(); ++i) {
        const auto token = (*resourceTokens)[i];
        const auto* refs = ResourceClosureRefCountFor(token);
        if (refs == nullptr) return VN_CONTENT_NOT_LOADED;
        if (*refs == UINT64_MAX) return VN_RUNTIME_RECREATE_REQUIRED;
        for (std::size_t j = 0; j < i; ++j)
            if ((*resourceTokens)[j] == token) return VN_INVALID_ARG;
    }
    auto& row = m_resourceClosures[selected];
    row.tokens = resourceTokens;
    for (const auto token : *row.tokens) ++*ResourceClosureRefCountFor(token);
    row.resourceCount = static_cast<std::uint32_t>(row.tokens->size());
    row.state = ResourceClosureState::Live;
    *outClosureToken = MakeResourceClosureToken(selected, row.generation);
    return VN_OK;
}

VnResult PackTable::ValidateResourceClosure(
    PackResourceClosureToken closureToken) const noexcept {
    return ResourceClosureRowFor(closureToken) != nullptr ? VN_OK
                                                          : VN_STALE_HANDLE;
}

VnResult PackTable::SetResourceClosureGenerationForTests(
    std::size_t rowIndex, std::uint32_t generation) noexcept {
    if (rowIndex >= m_resourceClosures.Capacity() || generation == 0) {
        return VN_INVALID_ARG;
    }
    if (!m_resourceClosures.ConstructThrough(static_cast<std::uint32_t>(rowIndex + 1u)))
        return VN_BACKPRESSURE;
    ResourceClosureRow& row = m_resourceClosures[rowIndex];
    if (row.state != ResourceClosureState::Free) {
        return VN_IN_USE;
    }
    if (generation < row.generation) {
        return VN_INVALID_ARG;
    }
    row.generation = generation;
    return VN_OK;
}

VnResult PackTable::SetSlotGenerationForTests(
    std::size_t rowIndex, std::uint32_t generation) noexcept {
    if (rowIndex >= m_slots.Capacity() || generation == 0) {
        return VN_INVALID_ARG;
    }
    if (!m_slots.ConstructThrough(static_cast<std::uint32_t>(rowIndex + 1u)))
        return VN_BACKPRESSURE;
    Slot& slot = m_slots[rowIndex];
    if (slot.loaded || generation < slot.generation) {
        return VN_IN_USE;
    }
    slot.generation = generation;
    return VN_OK;
}

VnResult PackTable::ReleaseResourceClosure(
    PackResourceClosureToken closureToken) noexcept {
    ResourceClosureRow* row = ResourceClosureRowFor(closureToken);
    if (row == nullptr) {
        return VN_STALE_HANDLE;
    }
    const std::size_t rowIndex =
        static_cast<std::size_t>(static_cast<std::uint32_t>(closureToken) - 1);
    const PackResourceRevisionToken* stored = ResourceClosureTokens(rowIndex);
    for (std::size_t i = 0; i < row->resourceCount; ++i) {
        const std::uint64_t* refs = ResourceClosureRefCountFor(stored[i]);
        if (refs == nullptr || *refs == 0) {
            return VN_RUNTIME_RECREATE_REQUIRED;
        }
    }
    for (std::size_t i = 0; i < row->resourceCount; ++i) {
        --*ResourceClosureRefCountFor(stored[i]);
    }
    row->tokens.reset();
    row->resourceCount = 0;
    if (row->generation == std::numeric_limits<std::uint32_t>::max()) {
        row->state = ResourceClosureState::Quarantined;
    } else {
        ++row->generation;
        row->state = ResourceClosureState::Free;
    }
    return VN_OK;
}

VnResult PackTable::ReadAndValidate(
    const char* utf8Path, AlignedBuffer* outBytes,
    std::vector<PackTocEntry>* outParsed) const {
    std::uint64_t fileBytes = 0;
    AlignedBuffer bytes(ReadWholeFileAligned(utf8Path, &fileBytes));
    if (bytes == nullptr || fileBytes < kPackHeaderBytes) {
        return VN_BAD_PACK;
    }

    PackHeader header;
    if (!ParsePackHeader(bytes.get(), &header)) {
        return VN_BAD_PACK;
    }
    if (!SupportedPackFormatVersion(header.formatVersion) ||
        header.headerBytes != kPackHeaderBytes ||
        header.fileBytes != fileBytes || header.entryCount == 0) {
        return VN_BAD_PACK;
    }
    const std::uint64_t tocBytes =
        static_cast<std::uint64_t>(header.entryCount) *
        kPackTocEntryBytes;
    if (header.tocOffset < header.headerBytes ||
        header.tocOffset > fileBytes ||
        tocBytes > fileBytes - header.tocOffset) {
        return VN_BAD_PACK;
    }
    const std::uint8_t* toc = bytes.get() + header.tocOffset;
    if (Fnv1a64(toc, static_cast<std::size_t>(tocBytes)) !=
        header.tocChecksum) {
        return VN_BAD_PACK;
    }

    // Full validation before the caller touches any registry or slot state:
    // past this loop the only remaining failure mode is an id conflict.
    const std::uint64_t blobRegionStart = header.tocOffset + tocBytes;
    std::vector<PackTocEntry> parsed(header.entryCount);
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        parsed[i] =
            ParsePackTocEntry(toc + i * kPackTocEntryBytes);
        if (!ValidateTocEntry(parsed[i], fileBytes, blobRegionStart)) {
            return VN_BAD_PACK;
        }
        if (Fnv1a64(bytes.get() + parsed[i].blobOffset,
                    static_cast<std::size_t>(parsed[i].blobBytes)) !=
            parsed[i].blobChecksum) {
            return VN_BAD_PACK;
        }
    }
    // A TOC that repeats an id is malformed (the compiler refuses to build
    // one); catching it here keeps VN_IN_USE meaning "conflicts with an
    // existing registration" only.
    {
        std::vector<VnObjectId> ids;
        ids.reserve(parsed.size());
        for (const PackTocEntry& e : parsed) {
            ids.push_back(e.soundId);
        }
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
            return VN_BAD_PACK;
        }
    }
    *outBytes = std::move(bytes);
    *outParsed = std::move(parsed);
    return VN_OK;
}

VnResult PackTable::Load(const char* utf8Path, PCMRegistry& registry,
                              VnPackHandle* outHandle,
                              ReservedIdFn isReserved, void* reservedCtx) {
    if (!PrepareContentStorage()) return VN_BACKPRESSURE;
    const std::uint32_t capacity =
        static_cast<std::uint32_t>(m_slots.size());
    std::uint32_t slotIndex = 0;
    for (; slotIndex < capacity; ++slotIndex) {
        if (!m_slots[slotIndex].loaded) {
            break;
        }
    }
    if (slotIndex == capacity) {
        ++m_noPackCount;
        return VN_NO_PACK;
    }

    AlignedBuffer bytes;
    std::vector<PackTocEntry> parsed;
    const VnResult valid = ReadAndValidate(utf8Path, &bytes, &parsed);
    if (valid != VN_OK) {
        return valid;
    }
    const std::uint32_t entryCount =
        static_cast<std::uint32_t>(parsed.size());

    // Flat playable-ID space: no bulk-registration exemption. An entry id
    // already owned by a playable surface outside this registry (a container or
    // a registered stream) is refused before any registration, so the load
    // stays all-or-nothing (registry duplicates are caught per entry below).
    if (isReserved != nullptr) {
        for (const PackTocEntry& e : parsed) {
            if (isReserved(reservedCtx, e.soundId)) {
                return VN_IN_USE;
            }
        }
    }

    // Every allocation the success path needs happens BEFORE the first
    // registration: once reference-mode entries exist in the registry,
    // a throw escaping this function would leave them pointing into a
    // freed buffer, so from the first RegisterPCM to the return the path
    // is registry calls returning codes, POD writes, and moves of this
    // preallocated storage only.
    std::vector<PackEntry> entries;
    entries.reserve(parsed.size());
    for (const PackTocEntry& e : parsed) {
        PackEntry entry;
        entry.soundId = e.soundId;
        entry.mediaChecksum = e.blobChecksum;
        entry.mediaChecksumKnown = true;
        entry.flags = e.flags;
        entry.frameCount = e.frameCount;
        entry.loopStartFrame = e.loopStartFrame;
        entry.loopEndFrame = e.loopEndFrame;
        entries.push_back(entry);
    }

    // All-or-nothing registration: a refusal at entry i unwinds entries
    // 0..i-1, leaving the registry exactly as it was. The rollback
    // unregisters cannot be refused — the ids were registered inside this
    // call and the externally synchronized API means no voice can have
    // started on them. Each success stamps its provenance cookie into the
    // preallocated metadata (POD write).
    //
    // The transaction boundary also contains exceptions: RegisterPCM's
    // map insertion can throw bad_alloc after earlier entries were
    // registered, and those reference-mode entries would outlive the
    // buffer this function still owns. The catch runs the same rollback
    // (allocation-free: u64-key map erase does not throw) and rethrows;
    // the slot itself is untouched until the success writes below, so
    // unwinding leaves no partial pack behind. Propagating the system
    // allocation failure itself matches the engine's existing posture
    // (Vn_RegisterPCM's copy path has thrown uncaught since M1).
    std::uint32_t registered = 0;
    try {
        for (std::uint32_t i = 0; i < entryCount; ++i) {
            const PackTocEntry& e = parsed[i];
            const float* planes[kMaxChannels] = {};
            const float* blob =
                reinterpret_cast<const float*>(bytes.get() + e.blobOffset);
            for (std::uint32_t ch = 0; ch < e.channelCount; ++ch) {
                planes[ch] =
                    blob + static_cast<std::size_t>(ch) * e.frameCount;
            }
            VnPCMDesc desc{};
            desc.structSize = sizeof(VnPCMDesc);
            desc.sampleRate = e.sampleRate;
            desc.channels = e.channelCount;
            desc.layout = VN_PCM_LAYOUT_PLANAR;
            desc.policy = VN_PCM_POLICY_REFERENCE;
            desc.frames = e.frameCount;
            desc.samplesPlanar = planes;
            const VnResult result = registry.RegisterPCM(
                e.soundId, &desc, &entries[i].registrationCookie);
            if (result != VN_OK) {
                for (std::uint32_t r = 0; r < i; ++r) {
                    (void)registry.UnregisterPCM(parsed[r].soundId);
                }
                return result;
            }
            entries[i].registration = registry.ReferenceOf(
                e.soundId, entries[i].registrationCookie);
            registered = i + 1;
        }
    } catch (...) {
        for (std::uint32_t r = 0; r < registered; ++r) {
            (void)registry.UnregisterPCM(parsed[r].soundId);
        }
        // The in-flight entry may have been inserted before the throw
        // (a partial RegisterPCM); unregistering it is benign either way
        // — an absent id just reads back VN_UNKNOWN_ID, and it cannot be
        // anyone else's registration (a duplicate would have been
        // refused, not thrown).
        if (registered < entryCount) {
            (void)registry.UnregisterPCM(parsed[registered].soundId);
        }
        throw;
    }

    Slot& slot = m_slots[slotIndex];
    slot.revisionKey = {};
    slot.resourceToken = 0;
    slot.closureRefCount = 0;
    slot.entries = std::move(entries);
    slot.bytes = std::move(bytes);
    slot.loaded = true;
    const std::uint64_t loaded = LoadedCount();
    if (loaded > m_loadedHighWater) {
        m_loadedHighWater = loaded;
    }
    *outHandle = MakePackHandle(slotIndex, slot.generation);
    return VN_OK;
}

VnResult PackTable::Unload(VnPackHandle handle,
                                PCMRegistry& registry) {
    Slot* slot = SlotFor(handle);
    if (slot == nullptr) {
        ++m_staleHandleHits;
        return VN_STALE_HANDLE;
    }
    const VnResult canUnload = CanUnload(handle, registry);
    if (canUnload != VN_OK) {
        return canUnload;
    }
    // No voice can start between the gate above and these unregisters
    // (raw voice starts share the externally synchronized API thread), and use
    // counts only fall on the audio side, so the refusal-free path holds.
    for (const PackEntry& entry : slot->entries) {
        if (registry.CookieOf(entry.soundId) != entry.registrationCookie ||
            OtherPackReferences(handle, entry.soundId, entry.registrationCookie)) {
            continue;
        }
        (void)registry.UnregisterPCM(entry.soundId);
    }
    slot->entries.clear();
    slot->bytes.reset();
    slot->revisionKey = {};
    slot->resourceToken = 0;
    slot->closureRefCount = 0;
    slot->loaded = false;
    // Same wraparound rule as voice slots: generation 0 would let slot 0
    // mint handle 0, and zero is structurally never a valid handle.
    ++slot->generation;
    if (slot->generation == 0) {
        slot->generation = 1;
    }
    return VN_OK;
}

VnResult PackTable::Replace(VnPackHandle handle, const char* utf8Path,
                            PCMRegistry& registry, ReservedIdFn isReserved,
                            void* reservedCtx) {
    PreparedReplaceToken prepared;
    const VnResult valid = PrepareReplace(handle, utf8Path, registry,
                                          isReserved, reservedCtx, &prepared);
    return valid == VN_OK ? CommitPreparedReplace(&prepared, registry)
                          : valid;
}

VnResult PackTable::PrepareReplace(VnPackHandle handle, const char* utf8Path,
                                   const PCMRegistry& registry,
                                   ReservedIdFn isReserved,
                                   void* reservedCtx,
                                   PreparedReplaceToken* out) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    Slot* slot = SlotFor(handle);
    if (slot == nullptr) {
        ++m_staleHandleHits;
        return VN_STALE_HANDLE;
    }
    // One retiring generation per asset (the structural bound): the caller
    // sweeps drained generations first, so this refusal means the previous
    // old content genuinely still has logical voices.
    if (HasRetiring(handle)) {
        return VN_IN_USE;
    }

    PreparedReplaceToken prepared;
    const VnResult valid =
        ReadAndValidate(utf8Path, &prepared.m_bytes, &prepared.m_parsed);
    if (valid != VN_OK) {
        return valid;
    }

    // Ownership screen, before any state change: a new entry id may collide
    // only with a registration this pack still owns (that entry is about to
    // retire and its id become vacant); anything else — a foreign
    // registration, a container, a stream — refuses the whole replace with
    // the old pack untouched.
    for (const PackTocEntry& e : prepared.m_parsed) {
        if (isReserved != nullptr && isReserved(reservedCtx, e.soundId)) {
            return VN_IN_USE;
        }
        const std::uint64_t cookie = registry.CookieOf(e.soundId);
        if (cookie == 0) {
            continue; // vacant id: a plain registration below
        }
        bool ownedByThisPack = false;
        for (const PackEntry& owned : slot->entries) {
            if (owned.soundId == e.soundId &&
                owned.registrationCookie == cookie) {
                ownedByThisPack = true;
                break;
            }
        }
        if (!ownedByThisPack) {
            return VN_IN_USE;
        }
    }

    // Preallocate the success path's storage before the first registry
    // mutation (the Load() discipline).
    prepared.m_entries.reserve(prepared.m_parsed.size());
    for (const PackTocEntry& e : prepared.m_parsed) {
        PackEntry entry;
        entry.soundId = e.soundId;
        entry.mediaChecksum = e.blobChecksum;
        entry.mediaChecksumKnown = true;
        entry.flags = e.flags;
        entry.frameCount = e.frameCount;
        entry.loopStartFrame = e.loopStartFrame;
        entry.loopEndFrame = e.loopEndFrame;
        prepared.m_entries.push_back(entry);
    }
    prepared.m_owner = this;
    prepared.m_handle = handle;
    *out = std::move(prepared);
    return VN_OK;
}

VnResult PackTable::CommitPreparedReplace(PreparedReplaceToken* token,
                                          PCMRegistry& registry) {
    if (token == nullptr || token->m_owner != this) {
        return VN_INVALID_ARG;
    }
    Slot* slot = SlotFor(token->m_handle);
    if (slot == nullptr || HasRetiring(token->m_handle)) {
        return VN_IN_USE;
    }
    AlignedBuffer& bytes = token->m_bytes;
    std::vector<PackTocEntry>& parsed = token->m_parsed;
    std::vector<PackEntry>& entries = token->m_entries;
    RetiringGeneration* old = nullptr;
    const VnResult reserved = PrepareRetiringRow(
        token->m_handle, slot->entries.size(), 0, nullptr, &old);
    if (reserved != VN_OK) {
        return reserved;
    }
    old->revisionKey = slot->revisionKey;
    old->resourceToken = slot->resourceToken;

    // Retire every entry the pack still owns: extract it whole from the
    // registry — regardless of use count; draining voices keep reading it
    // through their captured views — so an id shared by both TOCs is vacant
    // for the re-registration below and a removed id fails new lookups the
    // moment this call returns. Entries the caller re-registered with their
    // own content (cookie mismatch) are not the pack's and stay untouched.
    for (const PackEntry& owned : slot->entries) {
        PCMRegistry::RetiredGeneration g;
        if (registry.ExtractRetiring(owned.soundId, owned.registrationCookie,
                                     &g)) {
            old->entries.push_back(std::move(g));
        }
    }

    // Register the new TOC. Nothing can refuse it — validation passed, every
    // colliding id was proven the pack's own and extracted above, and the
    // externally synchronized API admits no interleaving registration — so a
    // failure here is a system allocation throw. The unwind unregisters what
    // was already registered FIRST (u64-key erases, never throwing), then
    // reinserts the extracted old nodes — the table held every one of them
    // before the extraction and buckets never shrink, so with the new
    // entries erased the reinsert triggers no rehash and no allocation —
    // then rethrows: the registry reads exactly as before the call (the
    // Load() posture on bad_alloc, extended to the swap).
    std::uint32_t registered = 0;
    try {
        for (std::size_t i = 0; i < parsed.size(); ++i) {
            const PackTocEntry& e = parsed[i];
            const float* planes[kMaxChannels] = {};
            const float* blob =
                reinterpret_cast<const float*>(bytes.get() + e.blobOffset);
            for (std::uint32_t ch = 0; ch < e.channelCount; ++ch) {
                planes[ch] =
                    blob + static_cast<std::size_t>(ch) * e.frameCount;
            }
            VnPCMDesc desc{};
            desc.structSize = sizeof(VnPCMDesc);
            desc.sampleRate = e.sampleRate;
            desc.channels = e.channelCount;
            desc.layout = VN_PCM_LAYOUT_PLANAR;
            desc.policy = VN_PCM_POLICY_REFERENCE;
            desc.frames = e.frameCount;
            desc.samplesPlanar = planes;
            const VnResult result = registry.RegisterPCM(
                e.soundId, &desc, &entries[i].registrationCookie);
            if (result != VN_OK) {
                // Unreachable by the ownership screen; unwind defensively.
                for (std::size_t r = 0; r < i; ++r) {
                    (void)registry.UnregisterPCM(parsed[r].soundId);
                }
                for (PCMRegistry::RetiredGeneration& g : old->entries) {
                    registry.ReinsertRetired(std::move(g));
                }
                ReleaseRetiringRow(*old);
                return result;
            }
            entries[i].registration = registry.ReferenceOf(
                e.soundId, entries[i].registrationCookie);
            registered = static_cast<std::uint32_t>(i) + 1;
        }
    } catch (...) {
        for (std::uint32_t r = 0; r < registered; ++r) {
            (void)registry.UnregisterPCM(parsed[r].soundId);
        }
        // The in-flight entry may have been inserted before the throw (a
        // partial RegisterPCM); unregistering it is benign either way.
        if (registered < parsed.size()) {
            (void)registry.UnregisterPCM(parsed[registered].soundId);
        }
        for (PCMRegistry::RetiredGeneration& g : old->entries) {
            registry.ReinsertRetired(std::move(g));
        }
        ReleaseRetiringRow(*old);
        throw;
    }

    // Swap the slot to the new generation. The handle survives: the slot
    // generation is untouched, only the content changed.
    old->bytes = std::move(slot->bytes);
    old->packEntries = std::move(slot->entries);
    old->closureRefCount = slot->closureRefCount;
    slot->bytes = std::move(bytes);
    slot->entries = std::move(entries);
    slot->revisionKey = {};
    slot->resourceToken = 0;
    slot->closureRefCount = 0;
    // Pin the old generation only while voices still read it; a fully
    // drained one (nothing was playing) frees right here on the logic
    // thread.
    if (old->closureRefCount == 0 && old->Drained()) {
        ReleaseRetiringRow(*old);
    } else {
        old->state = RetiringState::Live;
    }
    *token = PreparedReplaceToken{};
    return VN_OK;
}

void PackTable::SweepRetiring(RetainResourceTokenFn retain,
                              void* retainCtx) {
    for (RetiringGeneration& row : m_retiring) {
        if (row.state != RetiringState::Live || row.closureRefCount != 0 ||
            !row.Drained()) {
            continue;
        }
        if (row.resourceToken != 0 &&
            (retain == nullptr || retain(retainCtx, row.resourceToken))) {
            continue;
        }
        ReleaseRetiringRow(row);
    }
}

std::size_t PackTable::RetiringCount() const {
    std::size_t count = 0;
    for (const RetiringGeneration& row : m_retiring) {
        if (row.state == RetiringState::Live) {
            ++count;
        }
    }
    return count;
}

bool PackTable::HasRetiring(VnPackHandle handle) const {
    for (const RetiringGeneration& g : m_retiring) {
        if (g.state == RetiringState::Live && g.handle == handle) {
            return true;
        }
    }
    return false;
}

const std::vector<PackEntry>* PackTable::Entries(
    VnPackHandle handle) const {
    const Slot* slot = SlotFor(handle);
    return slot != nullptr ? &slot->entries : nullptr;
}

const PackEntry* PackTable::FindEntryBySound(
    VnSoundId soundId, const PCMRegistry& registry,
    VnPackHandle* outOwner) const {
    if (outOwner != nullptr) *outOwner = 0;
    const auto currentCookie = registry.CookieOf(soundId);
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        const Slot& slot = m_slots[index];
        if (!slot.loaded) {
            continue;
        }
        for (const PackEntry& entry : slot.entries) {
            if (entry.soundId != soundId) {
                continue;
            }
            // Serve metadata only while the registration under the id is
            // still the pack's own: after the caller re-registers the id
            // with other content, pack loop facts would be lies about it.
            if (currentCookie != entry.registrationCookie) {
                continue;
            }
            if (outOwner != nullptr) {
                *outOwner = MakePackHandle(static_cast<std::uint32_t>(index),
                                           slot.generation);
            }
            return &entry;
        }
    }
    return nullptr;
}

std::uint32_t PackTable::LoadedCount() const {
    std::uint32_t count = 0;
    for (const Slot& slot : m_slots) {
        if (slot.loaded) {
            ++count;
        }
    }
    return count;
}

PackTable::Slot* PackTable::SlotFor(VnPackHandle handle) {
    const std::uint32_t slotIndex = static_cast<std::uint32_t>(handle);
    const std::uint32_t generation =
        static_cast<std::uint32_t>(handle >> 32);
    if (slotIndex >= m_slots.size()) {
        return nullptr;
    }
    Slot& slot = m_slots[slotIndex];
    if (!slot.loaded || slot.generation != generation) {
        return nullptr;
    }
    return &slot;
}

const PackTable::Slot* PackTable::SlotFor(
    VnPackHandle handle) const {
    return const_cast<PackTable*>(this)->SlotFor(handle);
}

VnResult PackTable::PrepareResourceRevisionToken(
    const PackRevisionKey* revisionKey, VnPackHandle expectedHandle,
    PackResourceRevisionToken* outToken) const noexcept {
    if (outToken == nullptr) {
        return VN_INVALID_ARG;
    }
    *outToken = 0;
    if (revisionKey == nullptr) {
        return VN_OK;
    }
    if (revisionKey->handle == 0 ||
        revisionKey->handle != expectedHandle ||
        revisionKey->packStableId == 0 ||
        revisionKey->slotGeneration !=
            static_cast<std::uint32_t>(expectedHandle >> 32)) {
        return VN_INVALID_ARG;
    }
    if (m_resourceRevisionTokensExhausted) {
        return VN_RUNTIME_RECREATE_REQUIRED;
    }
    *outToken = m_nextResourceRevisionToken;
    return VN_OK;
}

void PackTable::CommitResourceRevisionToken(
    PackResourceRevisionToken token) noexcept {
    if (token == 0) {
        return;
    }
    assert(!m_resourceRevisionTokensExhausted &&
           token == m_nextResourceRevisionToken);
    if (token == std::numeric_limits<PackResourceRevisionToken>::max()) {
        m_resourceRevisionTokensExhausted = true;
    } else {
        ++m_nextResourceRevisionToken;
    }
}

VnResult PackTable::PrepareLoad(PreparedLoadToken* out) noexcept {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = PreparedLoadToken{};
    if (!PrepareContentStorage()) return VN_BACKPRESSURE;
    const std::uint32_t capacity =
        static_cast<std::uint32_t>(m_slots.size());
    std::uint32_t slotIndex = 0;
    for (; slotIndex < capacity; ++slotIndex) {
        if (!m_slots[slotIndex].loaded) {
            break;
        }
    }
    if (slotIndex == capacity) {
        ++m_noPackCount;
        return VN_NO_PACK;
    }
    const std::uint32_t generation = m_slots[slotIndex].generation;
    out->m_owner = this;
    out->m_slotIndex = slotIndex;
    out->m_generation = generation;
    out->m_handle = MakePackHandle(slotIndex, generation);
    return VN_OK;
}

void PackTable::CancelPreparedLoad(
    PreparedLoadToken* token) noexcept {
    if (token != nullptr) {
        *token = PreparedLoadToken{};
    }
}

PackTable::PreparedSetMutation::~PreparedSetMutation() noexcept {
    if (m_owner != nullptr) {
        m_owner->CancelPreparedSet(this);
    }
}

PackTable::PreparedSetMutation::PreparedSetMutation(
    PreparedSetMutation&& other) noexcept {
    *this = std::move(other);
}

PackTable::PreparedSetMutation& PackTable::PreparedSetMutation::operator=(
    PreparedSetMutation&& other) noexcept {
    if (this != &other) {
        if (m_owner != nullptr) {
            m_owner->CancelPreparedSet(this);
        }
        m_owner = std::exchange(other.m_owner, nullptr);
        m_registry = std::exchange(other.m_registry, nullptr);
        m_changes = std::move(other.m_changes);
        m_members = std::move(other.m_members);
        m_retiringMedia = std::move(other.m_retiringMedia);
        m_pcm = std::move(other.m_pcm);
    }
    return *this;
}

VnPackHandle PackTable::PreparedSetMutation::HandleAt(
    std::size_t member) const noexcept {
    return member < m_members.size() ? m_changes[m_members[member]].handle : 0;
}

PackResourceRevisionToken PackTable::PreparedSetMutation::ResourceTokenAt(
    std::size_t member) const noexcept {
    return member < m_members.size()
               ? m_changes[m_members[member]].resourceToken : 0;
}

VnResult PackTable::PrepareCanonicalSet(
    const std::vector<CanonicalSetMember>& members,
    const std::vector<VnPackHandle>& removed, PCMRegistry& registry,
    PreparedSetMutation* out,
    const std::vector<PackResourceRevisionToken>& retained) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    if (out->IsPrepared() || m_pendingSetMutation || registry.m_pendingSetMutation) {
        return VN_IN_USE;
    }
    if (members.size() > m_slots.Capacity()) {
        return VN_INVALID_ARG;
    }
    if (!PrepareContentStorage()) return VN_BACKPRESSURE;
    *out = PreparedSetMutation{};
    m_conflictingSoundId = {};
    const auto newRevisions = std::count_if(members.begin(), members.end(),
        [](const auto& member) { return !member.keep; });
    if (newRevisions != 0 &&
        (m_resourceRevisionTokensExhausted ||
         static_cast<std::uint64_t>(newRevisions - 1) > UINT64_MAX - m_nextResourceRevisionToken)) {
        return VN_RUNTIME_RECREATE_REQUIRED;
    }
    PreparedSetMutation prepared;
    prepared.m_owner = this;
    prepared.m_registry = &registry;
    m_pendingSetMutation = true;
    registry.m_pendingSetMutation = true;
    try {
        prepared.m_changes.resize(m_slots.size());
        prepared.m_members.reserve(members.size());
        std::vector<std::uint8_t> removing(m_slots.size(), 0);
        std::vector<std::uint8_t> assigned(m_slots.size(), 0);
        for (const VnPackHandle handle : removed) {
            const Slot* slot = SlotFor(handle);
            if (slot == nullptr) {
                return VN_STALE_HANDLE;
            }
            const auto index = static_cast<std::uint32_t>(handle);
            if (removing[index] != 0) {
                return VN_INVALID_ARG;
            }
            if (slot->generation == UINT32_MAX) {
                return VN_RUNTIME_RECREATE_REQUIRED;
            }
            removing[index] = 1;
            prepared.m_changes[index].touched = true;
        }
        for (const auto& member : members) {
            if (member.expectedHandle == 0) {
                continue;
            }
            const Slot* slot = SlotFor(member.expectedHandle);
            if (slot == nullptr) {
                return VN_STALE_HANDLE;
            }
            const auto index = static_cast<std::uint32_t>(member.expectedHandle);
            if (assigned[index] != 0 || removing[index] != 0 ||
                slot->revisionKey.packStableId != member.revisionKey.packStableId) {
                return VN_INVALID_ARG;
            }
            assigned[index] = 1;
        }
        for (std::size_t i = 0; i < members.size(); ++i) {
            const auto id = members[i].revisionKey.packStableId;
            if (id == 0) {
                return VN_INVALID_ARG;
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (members[j].revisionKey.packStableId == id) {
                    return VN_INVALID_ARG;
                }
            }
            for (std::size_t index = 0; index < m_slots.size(); ++index) {
                if (m_slots[index].loaded && removing[index] == 0 &&
                    assigned[index] == 0 && m_slots[index].revisionKey.packStableId == id) {
                    return VN_INVALID_ARG;
                }
            }
        }
        std::uint64_t newRevisionOrdinal = 0;
        for (std::size_t i = 0; i < members.size(); ++i) {
            const CanonicalSetMember& member = members[i];
            if (member.keep) {
                const auto* slot = SlotFor(member.expectedHandle);
                if (slot == nullptr || member.bytes != nullptr || member.media != nullptr ||
                    slot->revisionKey.packStableId != member.revisionKey.packStableId)
                    return VN_INVALID_ARG;
                const auto index = static_cast<std::uint32_t>(member.expectedHandle);
                auto& change = prepared.m_changes[index];
                change.handle = member.expectedHandle;
                change.resourceToken = slot->resourceToken;
                prepared.m_members.push_back(index);
                continue;
            }
            if (member.revisionKey.packStableId == 0 || member.media == nullptr ||
                (!member.media->empty() &&
                 (member.bytes == nullptr || !*member.bytes))) {
                return VN_INVALID_ARG;
            }
            std::size_t index = static_cast<std::uint32_t>(member.expectedHandle);
            if (member.expectedHandle == 0) {
                for (index = 0; index < m_slots.size(); ++index) {
                    if (assigned[index] == 0 &&
                        (!m_slots[index].loaded || removing[index] != 0)) {
                        break;
                    }
                }
                if (index == m_slots.size()) {
                    ++m_noPackCount;
                    return VN_NO_PACK;
                }
                assigned[index] = 1;
            }
            auto& change = prepared.m_changes[index];
            change.touched = true;
            change.hasSuccessor = true;
            const auto generation = m_slots[index].generation + removing[index];
            change.handle = MakePackHandle(static_cast<std::uint32_t>(index), generation);
            change.revisionKey = member.revisionKey;
            change.revisionKey.handle = change.handle;
            change.revisionKey.slotGeneration = generation;
            change.resourceToken = m_nextResourceRevisionToken + newRevisionOrdinal++;
            change.entries.resize(member.media->size());
            for (std::size_t entry = 0; entry < member.media->size(); ++entry) {
                const auto& media = (*member.media)[entry];
                auto& next = change.entries[entry];
                next.soundId = media.soundId;
                next.mediaChecksum = media.blobChecksum;
                next.mediaChecksumKnown = true;
                next.flags = media.flags;
                next.frameCount = media.frameCount;
                next.loopStartFrame = media.loopStartFrame;
                next.loopEndFrame = media.loopEndFrame;
            }
            prepared.m_members.push_back(index);
        }
        for (std::size_t index = 0; index < m_slots.size(); ++index) {
            auto& change = prepared.m_changes[index];
            const auto& slot = m_slots[index];
            if (!change.touched || !slot.loaded) {
                continue;
            }
            RetiringGeneration* row = nullptr;
            const VnResult reserved = PrepareRetiringRow(
                MakePackHandle(static_cast<std::uint32_t>(index), slot.generation),
                slot.entries.size(), 0, &change.predecessor, &row);
            if (reserved != VN_OK) {
                return reserved;
            }
            change.retainAnchor = slot.resourceToken != 0 &&
                std::find(retained.begin(), retained.end(), slot.resourceToken) != retained.end();
        }
        struct MediaSource {
            const PackTocEntry* media = nullptr;
            const float* samples = nullptr;
            PCMRegistry::PackReference reference;
        };
        std::map<VnSoundId, MediaSource> mediaUnion;
        const auto sameMedia = [](const MediaSource& left, const MediaSource& right) {
            const auto& a = *left.media;
            const auto& b = *right.media;
            if (a.sampleRate != b.sampleRate || a.channelCount != b.channelCount ||
                a.frameCount != b.frameCount || a.flags != b.flags ||
                a.loopStartFrame != b.loopStartFrame || a.loopEndFrame != b.loopEndFrame) {
                return false;
            }
            for (std::uint32_t channel = 0; channel < a.channelCount; ++channel) {
                const auto offset = static_cast<std::size_t>(channel) * a.frameCount;
                if (std::memcmp(left.samples + offset, right.samples + offset,
                                static_cast<std::size_t>(a.frameCount) * sizeof(float)) != 0) {
                    return false;
                }
            }
            return true;
        };
        for (const auto& member : members) {
            if (member.keep) continue;
            for (const auto& media : *member.media) {
                MediaSource source{&media, reinterpret_cast<const float*>(
                    member.bytes->get() + media.blobOffset), {}};
                const auto [it, inserted] = mediaUnion.emplace(media.soundId, source);
                if (!inserted && !sameMedia(it->second, source)) {
                    m_conflictingSoundId = media.soundId;
                    return VN_IN_USE;
                }
            }
        }
        const auto unaffectedOwns = [&](VnSoundId id, std::uint64_t cookie) {
            for (std::size_t index = 0; index < m_slots.size(); ++index) {
                const auto& slot = m_slots[index];
                if (!slot.loaded || prepared.m_changes[index].touched) {
                    continue;
                }
                for (const auto& entry : slot.entries) {
                    if (entry.soundId == id && entry.registrationCookie == cookie &&
                        entry.registration.OwnsSamples()) {
                        return true;
                    }
                }
            }
            return false;
        };
        std::vector<PCMRegistry::PackReferenceRegistration> registrations;
        registrations.reserve(mediaUnion.size());
        std::vector<std::array<const float*, kMaxChannels>> planes(mediaUnion.size());
        std::size_t sourceIndex = 0;
        for (auto& [id, source] : mediaUnion) {
            const auto& media = *source.media;
            const auto cookie = registry.CookieOf(id);
            if (cookie != 0) {
                const PackEntry* old = FindEntryBySound(id, registry);
                if (old == nullptr || !old->registration.OwnsSamples()) {
                    m_conflictingSoundId = id;
                    return VN_IN_USE;
                }
                const PCMView view = old->registration.View();
                bool equal = view.sampleRate == media.sampleRate &&
                    view.channelCount == media.channelCount && view.frames == media.frameCount &&
                    view.stride == 1 &&
                    view.loop.startFrame == ((media.flags & kPackFlagLoopable) != 0 ? media.loopStartFrame : 0) &&
                    view.loop.endFrame == ((media.flags & kPackFlagLoopable) != 0 ? media.loopEndFrame : 0) &&
                    old->flags == media.flags &&
                    old->loopStartFrame == media.loopStartFrame &&
                    old->loopEndFrame == media.loopEndFrame;
                for (std::uint32_t ch = 0; equal && ch < media.channelCount; ++ch) {
                    equal = std::memcmp(view.channels[ch], source.samples +
                        static_cast<std::size_t>(ch) * media.frameCount,
                        static_cast<std::size_t>(media.frameCount) * sizeof(float)) == 0;
                }
                if (equal) {
                    source.reference = old->registration;
                    continue;
                }
                if (unaffectedOwns(id, cookie)) {
                    m_conflictingSoundId = id;
                    return VN_IN_USE;
                }
            }
            auto& channelPlanes = planes[sourceIndex++];
            for (std::uint32_t ch = 0; ch < media.channelCount; ++ch) {
                channelPlanes[ch] = source.samples + static_cast<std::size_t>(ch) * media.frameCount;
            }
            PCMRegistry::PackReferenceRegistration registration{};
            registration.soundId = id;
            registration.loop = (media.flags & kPackFlagLoopable) != 0
                ? PcmLoopRegion{media.loopStartFrame, media.loopEndFrame} : PcmLoopRegion{};
            registration.desc.structSize = sizeof(VnPCMDesc);
            registration.desc.sampleRate = media.sampleRate;
            registration.desc.channels = media.channelCount;
            registration.desc.layout = VN_PCM_LAYOUT_PLANAR;
            registration.desc.policy = VN_PCM_POLICY_COPY;
            registration.desc.frames = media.frameCount;
            registration.desc.samplesPlanar = channelPlanes.data();
            registrations.push_back(registration);
        }
        std::map<VnSoundId, PreparedSetMutation::RetiringMedia> retiring;
        for (std::size_t index = 0; index < m_slots.size(); ++index) {
            if (!prepared.m_changes[index].touched || !m_slots[index].loaded) {
                continue;
            }
            for (const auto& entry : m_slots[index].entries) {
                if (registry.CookieOf(entry.soundId) != entry.registrationCookie ||
                    unaffectedOwns(entry.soundId, entry.registrationCookie)) {
                    continue;
                }
                const auto next = mediaUnion.find(entry.soundId);
                if (next != mediaUnion.end() &&
                    next->second.reference.Cookie() == entry.registrationCookie) {
                    continue;
                }
                retiring.emplace(entry.soundId, PreparedSetMutation::RetiringMedia{
                    entry.soundId, entry.registrationCookie, index});
            }
        }
        prepared.m_retiringMedia.reserve(retiring.size());
        for (const auto& [id, row] : retiring) {
            prepared.m_retiringMedia.push_back(row);
        }
        const VnResult staged = registry.PreparePackMutation(
            registrations.data(), registrations.size(), retiring.size(), &prepared.m_pcm);
        if (staged != VN_OK) {
            return staged;
        }
        for (auto& [id, source] : mediaUnion) {
            if (!source.reference.Valid()) {
                source.reference = prepared.m_pcm.ReferenceOf(id);
            }
        }
        for (const auto index : prepared.m_members) {
            for (auto& entry : prepared.m_changes[index].entries) {
                entry.registration = mediaUnion.at(entry.soundId).reference;
                entry.registrationCookie = entry.registration.Cookie();
            }
        }
        *out = std::move(prepared);
        return VN_OK;
    } catch (const std::bad_alloc&) {
        return VN_BACKPRESSURE;
    } catch (const std::length_error&) {
        return VN_PACK_RESIDENT_CAPACITY;
    }
}

void PackTable::CancelPreparedSet(PreparedSetMutation* prepared) noexcept {
    if (prepared == nullptr || prepared->m_owner != this) {
        return;
    }
    for (auto& change : prepared->m_changes) {
        if (auto* row = RetiringRowFor(change.predecessor)) {
            ReleaseRetiringRow(*row);
        }
    }
    m_pendingSetMutation = false;
    prepared->m_registry->m_pendingSetMutation = false;
    prepared->m_owner = nullptr;
    prepared->m_registry = nullptr;
    prepared->m_pcm = PCMRegistry::PreparedPackMutation{};
    prepared->m_changes.clear();
    prepared->m_members.clear();
    prepared->m_retiringMedia.clear();
}

void PackTable::CommitPreparedSet(PreparedSetMutation* prepared,
                                   PCMRegistry& registry) noexcept {
    const bool valid = prepared != nullptr && prepared->m_owner == this &&
        prepared->m_registry == &registry;
    assert(valid);
    if (!valid) {
        return;
    }
    for (const auto& media : prepared->m_retiringMedia) {
        auto* row = RetiringRowFor(prepared->m_changes[media.slotIndex].predecessor);
        assert(row != nullptr && row->entries.size() < row->entries.capacity());
        PCMRegistry::RetiredGeneration retired;
        const bool extracted = registry.ExtractRetiring(media.id, media.cookie, &retired);
        assert(extracted);
        if (extracted) {
            row->entries.push_back(std::move(retired));
        }
    }
    for (std::size_t index = 0; index < prepared->m_changes.size(); ++index) {
        auto& change = prepared->m_changes[index];
        auto* row = RetiringRowFor(change.predecessor);
        if (row == nullptr) {
            continue;
        }
        auto& slot = m_slots[index];
        row->revisionKey = slot.revisionKey;
        row->resourceToken = slot.resourceToken;
        row->packEntries = std::move(slot.entries);
        row->bytes = std::move(slot.bytes);
        row->closureRefCount = slot.closureRefCount;
        slot.revisionKey = {};
        slot.resourceToken = 0;
        slot.closureRefCount = 0;
        slot.loaded = false;
        if (!change.hasSuccessor) {
            ++slot.generation;
        }
    }
    registry.CommitPreparedPackMutation(&prepared->m_pcm);
    for (const auto index : prepared->m_members) {
        auto& change = prepared->m_changes[index];
        if (!change.touched) continue;
        auto& slot = m_slots[index];
        slot.generation = static_cast<std::uint32_t>(change.handle >> 32);
        slot.entries = std::move(change.entries);
        slot.revisionKey = change.revisionKey;
        slot.resourceToken = change.resourceToken;
        slot.loaded = true;
        CommitResourceRevisionToken(change.resourceToken);
    }
    for (auto& change : prepared->m_changes) {
        if (auto* row = RetiringRowFor(change.predecessor)) {
            if (change.retainAnchor || row->closureRefCount != 0 || !row->Drained()) {
                row->state = RetiringState::Live;
            } else {
                ReleaseRetiringRow(*row);
            }
        }
    }
    m_loadedHighWater = std::max(m_loadedHighWater, static_cast<std::uint64_t>(LoadedCount()));
    m_pendingSetMutation = false;
    prepared->m_registry->m_pendingSetMutation = false;
    prepared->m_owner = nullptr;
    prepared->m_registry = nullptr;
}

bool PackTable::OtherPackReferences(VnPackHandle excluded, VnSoundId soundId,
                                    std::uint64_t cookie) const noexcept {
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
        const Slot& slot = m_slots[i];
        if (!slot.loaded ||
            MakePackHandle(static_cast<std::uint32_t>(i), slot.generation) == excluded) {
            continue;
        }
        for (const PackEntry& entry : slot.entries) {
            if (entry.soundId == soundId && entry.registration.Valid() &&
                entry.registrationCookie == cookie) {
                return true;
            }
        }
    }
    return false;
}

VnResult PackTable::PrepareCanonicalMedia(
    VnPackHandle replacing, const AlignedBuffer& bytes,
    const std::vector<PackTocEntry>& parsedMedia,
    std::vector<PackEntry>& entries, PCMRegistry& registry,
    PCMRegistry::PreparedPackMutation* out) {
    std::vector<PCMRegistry::PackReferenceRegistration> registrations;
    std::vector<std::array<const float*, kMaxChannels>> planes(parsedMedia.size());
    registrations.reserve(parsedMedia.size());
    const Slot* predecessor = SlotFor(replacing);
    for (std::size_t i = 0; i < parsedMedia.size(); ++i) {
        const PackTocEntry& media = parsedMedia[i];
        const PcmLoopRegion loop = (media.flags & kPackFlagLoopable) != 0
            ? PcmLoopRegion{media.loopStartFrame, media.loopEndFrame}
            : PcmLoopRegion{};
        const float* blob = reinterpret_cast<const float*>(bytes.get() + media.blobOffset);
        const std::uint64_t cookie = registry.CookieOf(media.soundId);
        if (cookie != 0) {
            const PackEntry* existing = FindEntryBySound(media.soundId, registry);
            if (existing == nullptr || !existing->registration.OwnsSamples()) {
                return VN_IN_USE;
            }
            const PCMView view = existing->registration.View();
            bool equal = view.sampleRate == media.sampleRate &&
                view.channelCount == media.channelCount && view.frames == media.frameCount &&
                view.stride == 1 &&
                view.loop.startFrame == loop.startFrame &&
                view.loop.endFrame == loop.endFrame &&
                existing->flags == media.flags &&
                existing->loopStartFrame == media.loopStartFrame &&
                existing->loopEndFrame == media.loopEndFrame;
            for (std::uint32_t ch = 0; equal && ch < media.channelCount; ++ch) {
                equal = std::memcmp(view.channels[ch],
                    blob + static_cast<std::size_t>(ch) * media.frameCount,
                    static_cast<std::size_t>(media.frameCount) * sizeof(float)) == 0;
            }
            if (equal) {
                entries[i].registration = existing->registration;
                entries[i].registrationCookie = cookie;
                continue;
            }
            if (predecessor == nullptr ||
                OtherPackReferences(replacing, media.soundId, cookie)) {
                m_conflictingSoundId = media.soundId;
                return VN_IN_USE;
            }
        }
        for (std::uint32_t ch = 0; ch < media.channelCount; ++ch) {
            planes[i][ch] = blob + static_cast<std::size_t>(ch) * media.frameCount;
        }
        PCMRegistry::PackReferenceRegistration registration{};
        registration.soundId = media.soundId;
        registration.loop = loop;
        registration.desc.structSize = sizeof(VnPCMDesc);
        registration.desc.sampleRate = media.sampleRate;
        registration.desc.channels = media.channelCount;
        registration.desc.layout = VN_PCM_LAYOUT_PLANAR;
        registration.desc.policy = VN_PCM_POLICY_COPY;
        registration.desc.frames = media.frameCount;
        registration.desc.samplesPlanar = planes[i].data();
        registrations.push_back(registration);
    }
    std::size_t retiringCount = 0;
    if (predecessor != nullptr) {
        for (const PackEntry& entry : predecessor->entries) {
            if (registry.CookieOf(entry.soundId) == entry.registrationCookie &&
                !OtherPackReferences(replacing, entry.soundId, entry.registrationCookie) &&
                std::none_of(entries.begin(), entries.end(), [&](const PackEntry& next) {
                    return next.soundId == entry.soundId &&
                        next.registrationCookie == entry.registrationCookie;
                })) {
                ++retiringCount;
            }
        }
    }
    const VnResult staged = registry.PreparePackMutation(
        registrations.data(), registrations.size(), retiringCount, out);
    if (staged != VN_OK) {
        return staged;
    }
    for (PackEntry& entry : entries) {
        if (!entry.registration.Valid()) {
            entry.registration = out->ReferenceOf(entry.soundId);
            entry.registrationCookie = entry.registration.Cookie();
        }
    }
    return VN_OK;
}

VnResult PackTable::LoadPrepared(PreparedLoadToken* token,
                                 AlignedBuffer& bytes,
                                 std::vector<PackEntry>&& entries,
                                 const std::vector<PackTocEntry>& parsedMedia,
                                 PCMRegistry& registry,
                                 VnPackHandle* outHandle,
                                 const PackRevisionKey* revisionKey) {
    m_conflictingSoundId = {};
    assert(token != nullptr && token->m_owner == this &&
           token->m_slotIndex < m_slots.size() &&
           token->m_generation ==
               m_slots[token->m_slotIndex].generation &&
           !m_slots[token->m_slotIndex].loaded &&
           token->m_handle ==
               MakePackHandle(token->m_slotIndex,
                              token->m_generation) &&
           outHandle != nullptr &&
           entries.size() == parsedMedia.size());
    const std::uint32_t slotIndex = token->m_slotIndex;
    PackResourceRevisionToken resourceToken = 0;
    const VnResult tokenPrepared = PrepareResourceRevisionToken(
        revisionKey, token->m_handle, &resourceToken);
    if (tokenPrepared != VN_OK) {
        return tokenPrepared;
    }

    // The entry storage arrived prebuilt from staging, so nothing below
    // allocates on this table's behalf: only registry calls, POD cookie
    // stamps, and the noexcept commit moves remain.

    // Reference-mode registration with the v1 unwind posture: every id
    // was proven vacant by the caller's staging, so a refusal here is a
    // bookkeeping defect (unwound defensively) and the only organic
    // failure is a system allocation throw inside RegisterPCM.
    const bool canonical = revisionKey != nullptr && revisionKey->packStableId != 0;
    PCMRegistry::PreparedPackMutation media;
    if (canonical) {
        const VnResult staged = PrepareCanonicalMedia(
            0, bytes, parsedMedia, entries, registry, &media);
        if (staged != VN_OK) {
            return staged;
        }
        registry.CommitPreparedPackMutation(&media);
    }
    std::uint32_t registered = 0;
    try {
        for (std::size_t i = 0; !canonical && i < parsedMedia.size(); ++i) {
            const PackTocEntry& e = parsedMedia[i];
            const float* planes[kMaxChannels] = {};
            const float* blob =
                reinterpret_cast<const float*>(bytes.get() + e.blobOffset);
            for (std::uint32_t ch = 0; ch < e.channelCount; ++ch) {
                planes[ch] =
                    blob + static_cast<std::size_t>(ch) * e.frameCount;
            }
            VnPCMDesc desc{};
            desc.structSize = sizeof(VnPCMDesc);
            desc.sampleRate = e.sampleRate;
            desc.channels = e.channelCount;
            desc.layout = VN_PCM_LAYOUT_PLANAR;
            desc.policy = VN_PCM_POLICY_REFERENCE;
            desc.frames = e.frameCount;
            desc.samplesPlanar = planes;
            const VnResult result = registry.RegisterPCM(
                e.soundId, &desc, &entries[i].registrationCookie);
            if (result != VN_OK) {
                for (std::size_t r = 0; r < i; ++r) {
                    (void)registry.UnregisterPCM(parsedMedia[r].soundId);
                }
                return result;
            }
            entries[i].registration = registry.ReferenceOf(
                e.soundId, entries[i].registrationCookie);
            registered = static_cast<std::uint32_t>(i) + 1;
        }
    } catch (...) {
        for (std::uint32_t r = 0; r < registered; ++r) {
            (void)registry.UnregisterPCM(parsedMedia[r].soundId);
        }
        if (registered < parsedMedia.size()) {
            (void)registry.UnregisterPCM(parsedMedia[registered].soundId);
        }
        throw;
    }

    Slot& slot = m_slots[slotIndex];
    slot.entries = std::move(entries);
    // A resident pack registers no media, so nothing references the
    // buffer once parsing is done; holding it would pin dead bytes for
    // the engine's life.
    if (parsedMedia.empty() || canonical) {
        slot.bytes.reset();
    } else {
        slot.bytes = std::move(bytes);
    }
    slot.revisionKey = revisionKey != nullptr ? *revisionKey
                                              : PackRevisionKey{};
    slot.resourceToken = resourceToken;
    slot.closureRefCount = 0;
    CommitResourceRevisionToken(resourceToken);
    slot.loaded = true;
    const std::uint64_t loaded = LoadedCount();
    if (loaded > m_loadedHighWater) {
        m_loadedHighWater = loaded;
    }
    *outHandle = token->m_handle;
    *token = PreparedLoadToken{};
    return VN_OK;
}

VnResult PackTable::CanUnload(VnPackHandle handle,
                              const PCMRegistry& registry) const {
    const Slot* slot = SlotFor(handle);
    if (slot == nullptr) {
        return VN_STALE_HANDLE;
    }
    if (slot->closureRefCount != 0) {
        return VN_IN_USE;
    }
    for (const PackEntry& entry : slot->entries) {
        // Cookie provenance controls removal of the current registration;
        // original readers independently control the lifetime of Pack bytes.
        const PCMView original = entry.registration.View();
        if (!entry.registration.OwnsSamples() && original.useCount != nullptr &&
            original.useCount->load(std::memory_order_acquire) != 0) {
            return VN_IN_USE;
        }
        if (registry.CookieOf(entry.soundId) != entry.registrationCookie ||
            OtherPackReferences(handle, entry.soundId, entry.registrationCookie)) {
            continue;
        }
        const PCMView view = registry.Lookup(entry.soundId);
        if (view.Valid() && view.useCount != nullptr &&
            view.useCount->load(std::memory_order_acquire) != 0) {
            return VN_IN_USE;
        }
    }
    return VN_OK;
}

void PackTable::CommitUnloadEmptyResident(VnPackHandle handle) noexcept {
    Slot* slot = SlotFor(handle);
    assert(slot != nullptr &&
           "prepared Resident unload must retain a live Pack handle");
    assert(slot == nullptr || slot->entries.empty());
    assert(slot == nullptr || slot->closureRefCount == 0);
    if (slot == nullptr) {
        return;
    }
    slot->entries.clear();
    slot->bytes.reset();
    slot->revisionKey = {};
    slot->resourceToken = 0;
    slot->closureRefCount = 0;
    slot->loaded = false;
    ++slot->generation;
    if (slot->generation == 0) {
        slot->generation = 1;
    }
}

VnResult PackTable::CanReplacePreparedV2(
    VnPackHandle handle, const std::vector<PackTocEntry>& parsedMedia,
    const PCMRegistry& registry, ReservedIdFn isReserved,
    void* reservedCtx) const {
    const Slot* slot = SlotFor(handle);
    if (slot == nullptr) {
        return VN_STALE_HANDLE;
    }
    for (const PackTocEntry& e : parsedMedia) {
        if (isReserved != nullptr && isReserved(reservedCtx, e.soundId)) {
            return VN_IN_USE;
        }
        const std::uint64_t cookie = registry.CookieOf(e.soundId);
        if (cookie == 0) {
            continue; // vacant id: a plain registration at the swap
        }
        bool ownedByThisPack = false;
        for (const PackEntry& owned : slot->entries) {
            if (owned.soundId == e.soundId &&
                owned.registrationCookie == cookie) {
                ownedByThisPack = true;
                break;
            }
        }
        if (!ownedByThisPack &&
            !(slot->revisionKey.packStableId != 0 &&
              OtherPackReferences(handle, e.soundId, cookie))) {
            return VN_IN_USE;
        }
    }
    return VN_OK;
}

PackTable::PreparedMutation::~PreparedMutation() noexcept {
    if (m_owner != nullptr) {
        m_owner->CancelPreparedMutation(this);
    }
}

PackTable::PreparedMutation::PreparedMutation(PreparedMutation&& other) noexcept {
    *this = std::move(other);
}

PackTable::PreparedMutation& PackTable::PreparedMutation::operator=(
    PreparedMutation&& other) noexcept {
    if (this != &other) {
        if (m_owner != nullptr) {
            m_owner->CancelPreparedMutation(this);
        }
        m_owner = std::exchange(other.m_owner, nullptr);
        m_handle = std::exchange(other.m_handle, 0);
        m_unload = std::exchange(other.m_unload, false);
        m_revisionKey = std::exchange(other.m_revisionKey, PackRevisionKey{});
        m_resourceToken = std::exchange(other.m_resourceToken, 0);
        m_bytes = std::move(other.m_bytes);
        m_entries = std::move(other.m_entries);
        m_pcm = std::move(other.m_pcm);
        m_predecessor = std::exchange(other.m_predecessor, RetiredReplace{});
    }
    return *this;
}

VnResult PackTable::PrepareRevisionMutation(
    VnPackHandle handle, AlignedBuffer& bytes,
    std::vector<PackEntry>& entries,
    const std::vector<PackTocEntry>& parsedMedia, PCMRegistry& registry,
    PreparedMutation* out, const PackRevisionKey* revisionKey) {
    m_conflictingSoundId = {};
    if (out == nullptr || entries.size() != parsedMedia.size()) {
        return VN_INVALID_ARG;
    }
    *out = PreparedMutation{};
    Slot* slot = SlotFor(handle);
    if (slot == nullptr) {
        return VN_STALE_HANDLE;
    }
    PackResourceRevisionToken resourceToken = 0;
    const VnResult tokenPrepared = PrepareResourceRevisionToken(
        revisionKey, handle, &resourceToken);
    if (tokenPrepared != VN_OK) {
        return tokenPrepared;
    }
    std::size_t ownedCount = 0;
    for (const PackEntry& owned : slot->entries) {
        if (registry.CookieOf(owned.soundId) == owned.registrationCookie) {
            ++ownedCount;
        }
    }

    PreparedMutation prepared;
    RetiringGeneration* row = nullptr;
    const VnResult reserved = PrepareRetiringRow(
        handle, ownedCount, 0, &prepared.m_predecessor, &row);
    if (reserved != VN_OK) {
        return reserved;
    }
    prepared.m_owner = this;
    try {
        if (revisionKey != nullptr && revisionKey->packStableId != 0) {
            const VnResult staged = PrepareCanonicalMedia(
                handle, bytes, parsedMedia, entries, registry, &prepared.m_pcm);
            if (staged != VN_OK) {
                return staged;
            }
        } else {
            std::vector<PCMRegistry::PackReferenceRegistration> registrations;
            std::vector<std::array<const float*, kMaxChannels>> channelPlanes(parsedMedia.size());
            registrations.reserve(parsedMedia.size());
            for (std::size_t i = 0; i < parsedMedia.size(); ++i) {
                const PackTocEntry& e = parsedMedia[i];
                PCMRegistry::PackReferenceRegistration registration;
                registration.soundId = e.soundId;
                auto& planes = channelPlanes[i];
                const float* blob = reinterpret_cast<const float*>(
                    bytes.get() + e.blobOffset);
                for (std::uint32_t ch = 0; ch < e.channelCount; ++ch) {
                    planes[ch] =
                        blob + static_cast<std::size_t>(ch) * e.frameCount;
                }
                registration.desc.structSize = sizeof(VnPCMDesc);
                registration.desc.sampleRate = e.sampleRate;
                registration.desc.channels = e.channelCount;
                registration.desc.layout = VN_PCM_LAYOUT_PLANAR;
                registration.desc.policy = VN_PCM_POLICY_REFERENCE;
                registration.desc.frames = e.frameCount;
                registration.desc.samplesPlanar = planes.data();
                registrations.push_back(registration);
            }
            const VnResult staged = registry.PreparePackMutation(
                registrations.empty() ? nullptr : registrations.data(),
                registrations.size(), ownedCount, &prepared.m_pcm);
            if (staged != VN_OK) {
                return staged;
            }
            for (std::size_t i = 0; i < entries.size(); ++i) {
                entries[i].registration =
                    prepared.m_pcm.ReferenceOf(entries[i].soundId);
                entries[i].registrationCookie =
                    prepared.m_pcm.CookieOf(entries[i].soundId);
            }
        }
    } catch (const std::bad_alloc&) {
        return VN_BACKPRESSURE;
    }
    prepared.m_handle = handle;
    prepared.m_revisionKey =
        revisionKey != nullptr ? *revisionKey : PackRevisionKey{};
    prepared.m_resourceToken = resourceToken;
    if (revisionKey == nullptr || revisionKey->packStableId == 0) {
        prepared.m_bytes = std::move(bytes);
    }
    prepared.m_entries = std::move(entries);
    *out = std::move(prepared);
    return VN_OK;
}

VnResult PackTable::PrepareUnloadMutation(VnPackHandle handle,
                                          const PCMRegistry& registry,
                                          PreparedMutation* out) {
    if (out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = PreparedMutation{};
    Slot* slot = SlotFor(handle);
    if (slot == nullptr) {
        return VN_STALE_HANDLE;
    }
    if (slot->generation == std::numeric_limits<std::uint32_t>::max()) {
        return VN_RUNTIME_RECREATE_REQUIRED;
    }
    std::size_t ownedCount = 0;
    for (const PackEntry& owned : slot->entries) {
        if (registry.CookieOf(owned.soundId) == owned.registrationCookie) {
            ++ownedCount;
        }
    }
    PreparedMutation prepared;
    RetiringGeneration* row = nullptr;
    const VnResult reserved = PrepareRetiringRow(
        handle, ownedCount, 0, &prepared.m_predecessor, &row);
    if (reserved != VN_OK) {
        return reserved;
    }
    prepared.m_owner = this;
    prepared.m_handle = handle;
    prepared.m_unload = true;
    *out = std::move(prepared);
    return VN_OK;
}

void PackTable::CancelPreparedMutation(PreparedMutation* prepared) noexcept {
    if (prepared == nullptr || prepared->m_owner != this) {
        return;
    }
    if (RetiringGeneration* row = RetiringRowFor(prepared->m_predecessor)) {
        ReleaseRetiringRow(*row);
    }
    prepared->m_pcm = PCMRegistry::PreparedPackMutation{};
    prepared->m_predecessor = RetiredReplace{};
    prepared->m_owner = nullptr;
}

void PackTable::CommitPreparedMutation(
    PreparedMutation* prepared, PCMRegistry& registry,
    RetiredReplace* outPredecessor) noexcept {
    assert(prepared != nullptr && prepared->m_owner == this);
    if (prepared == nullptr || prepared->m_owner != this) {
        return;
    }
    RetiringGeneration* row = RetiringRowFor(prepared->m_predecessor);
    Slot* slot = SlotFor(prepared->m_handle);
    assert(row != nullptr && slot != nullptr);
    if (row == nullptr || slot == nullptr) {
        return;
    }
    for (const PackEntry& owned : slot->entries) {
        if (OtherPackReferences(prepared->m_handle, owned.soundId,
                                owned.registrationCookie) ||
            std::any_of(prepared->m_entries.begin(), prepared->m_entries.end(),
                [&](const PackEntry& next) {
                    return next.soundId == owned.soundId &&
                        next.registrationCookie == owned.registrationCookie;
                })) {
            continue;
        }
        PCMRegistry::RetiredGeneration generation;
        if (registry.ExtractRetiring(owned.soundId,
                                     owned.registrationCookie, &generation)) {
            row->entries.push_back(std::move(generation));
        }
    }
    row->revisionKey = slot->revisionKey;
    row->resourceToken = slot->resourceToken;
    row->packEntries = std::move(slot->entries);
    if (!prepared->m_unload) {
        registry.CommitPreparedPackMutation(&prepared->m_pcm);
    }
    row->bytes = std::move(slot->bytes);
    row->closureRefCount = slot->closureRefCount;
    if (prepared->m_unload) {
        slot->revisionKey = {};
        slot->resourceToken = 0;
        slot->closureRefCount = 0;
        slot->loaded = false;
        ++slot->generation;
    } else {
        if (prepared->m_entries.empty()) {
            slot->bytes.reset();
        } else {
            slot->bytes = std::move(prepared->m_bytes);
        }
        slot->entries = std::move(prepared->m_entries);
        slot->revisionKey = prepared->m_revisionKey;
        slot->resourceToken = prepared->m_resourceToken;
        slot->closureRefCount = 0;
        CommitResourceRevisionToken(prepared->m_resourceToken);
    }
    RetiredReplace predecessor = std::move(prepared->m_predecessor);
    prepared->m_owner = nullptr;
    if (outPredecessor != nullptr) {
        *outPredecessor = std::move(predecessor);
    } else {
        ParkRetiredReplace(prepared->m_handle, std::move(predecessor),
                           false);
    }
}

VnResult PackTable::ReplacePreparedV2(
    VnPackHandle handle, AlignedBuffer& bytes,
    std::vector<PackEntry>& entries,
    const std::vector<PackTocEntry>& parsedMedia, PCMRegistry& registry,
    RetiredReplace* outPredecessor,
    const PackRevisionKey* revisionKey) {
    Slot* slot = SlotFor(handle);
    if (slot == nullptr) {
        return VN_STALE_HANDLE; // admission ran; defensive
    }
    if (entries.size() != parsedMedia.size()) {
        return VN_INVALID_ARG; // staging builds them together; defensive
    }
    PackResourceRevisionToken resourceToken = 0;
    const VnResult tokenPrepared = PrepareResourceRevisionToken(
        revisionKey, handle, &resourceToken);
    if (tokenPrepared != VN_OK) {
        return tokenPrepared;
    }

    RetiringGeneration* old = nullptr;
    const VnResult reserved = PrepareRetiringRow(
        handle, slot->entries.size(), entries.size(),
        outPredecessor, &old);
    if (reserved != VN_OK) {
        return reserved;
    }
    old->revisionKey = slot->revisionKey;
    old->resourceToken = slot->resourceToken;

    // Retire every still-owned old entry whole (draining voices keep
    // reading through their captured views); ids shared by both
    // generations become vacant for the re-registration below. From here
    // to the commit tail, any throw or refusal restores the retired
    // entries and unregisters the partial registrations, so a nonzero
    // return always means "old revision intact, revision retryable".
    std::uint32_t registered = 0;
    const auto unwind = [&]() {
        for (std::uint32_t r = 0; r < registered; ++r) {
            (void)registry.UnregisterPCM(parsedMedia[r].soundId);
        }
        if (registered < parsedMedia.size()) {
            (void)registry.UnregisterPCM(parsedMedia[registered].soundId);
        }
        for (PCMRegistry::RetiredGeneration& g : old->entries) {
            registry.ReinsertRetired(std::move(g));
        }
        ReleaseRetiringRow(*old);
        if (outPredecessor != nullptr) {
            *outPredecessor = RetiredReplace{};
        }
    };
    try {
        for (const PackEntry& owned : slot->entries) {
            PCMRegistry::RetiredGeneration g;
            if (registry.ExtractRetiring(owned.soundId,
                                         owned.registrationCookie, &g)) {
                old->entries.push_back(std::move(g));
            }
        }
        for (std::size_t i = 0; i < parsedMedia.size(); ++i) {
            const PackTocEntry& e = parsedMedia[i];
            const float* planes[kMaxChannels] = {};
            const float* blob =
                reinterpret_cast<const float*>(bytes.get() + e.blobOffset);
            for (std::uint32_t ch = 0; ch < e.channelCount; ++ch) {
                planes[ch] =
                    blob + static_cast<std::size_t>(ch) * e.frameCount;
            }
            VnPCMDesc desc{};
            desc.structSize = sizeof(VnPCMDesc);
            desc.sampleRate = e.sampleRate;
            desc.channels = e.channelCount;
            desc.layout = VN_PCM_LAYOUT_PLANAR;
            desc.policy = VN_PCM_POLICY_REFERENCE;
            desc.frames = e.frameCount;
            desc.samplesPlanar = planes;
            const VnResult result = registry.RegisterPCM(
                e.soundId, &desc, &entries[i].registrationCookie);
            if (result != VN_OK) {
                // Unreachable by the admission; unwind defensively.
                unwind();
                return result;
            }
            entries[i].registration = registry.ReferenceOf(
                e.soundId, entries[i].registrationCookie);
            registered = static_cast<std::uint32_t>(i) + 1;
        }
    } catch (...) {
        unwind();
        return VN_BACKPRESSURE; // transient shortage; zero net mutations
    }

    // Commit tail: moves and POD writes only, nothing below can throw.
    old->bytes = std::move(slot->bytes);
    old->packEntries = std::move(slot->entries);
    old->closureRefCount = slot->closureRefCount;
    if (parsedMedia.empty()) {
        slot->bytes.reset(); // a resident revision pins no buffer
    } else {
        slot->bytes = std::move(bytes);
    }
    slot->entries = std::move(entries);
    slot->revisionKey = revisionKey != nullptr ? *revisionKey
                                              : PackRevisionKey{};
    slot->resourceToken = resourceToken;
    slot->closureRefCount = 0;
    CommitResourceRevisionToken(resourceToken);
    if (outPredecessor == nullptr) {
        if (old->closureRefCount == 0 && old->Drained()) {
            ReleaseRetiringRow(*old);
        } else {
            old->state = RetiringState::Live;
        }
    }
    return VN_OK;
}

void PackTable::ParkRetiredReplace(VnPackHandle handle,
                                   RetiredReplace&& retired,
                                   bool retainAnchor) {
    RetiringGeneration* row = RetiringRowFor(retired);
    assert(row != nullptr);
    assert(row == nullptr || row->handle == handle);
    if (row == nullptr) {
        return;
    }
    if (retainAnchor || row->closureRefCount != 0 || !row->Drained()) {
        row->state = RetiringState::Live;
    } else {
        ReleaseRetiringRow(*row);
    }
    retired = {};
}

void PackTable::RestorePreparedReplaceV2(VnPackHandle handle,
                                         RetiredReplace& predecessor,
                                         PCMRegistry& registry) noexcept {
    RetiringGeneration* row = RetiringRowFor(predecessor);
    Slot* slot = SlotFor(handle);
    assert(row != nullptr);
    assert(slot != nullptr);
    assert(row == nullptr || slot == nullptr ||
           row->successorEntries.capacity() >=
                                  slot->entries.size());
    if (row == nullptr || slot == nullptr) {
        return;
    }
    const PackRevisionKey successorKey = slot->revisionKey;
    const PackResourceRevisionToken successorToken = slot->resourceToken;
    const std::uint64_t successorClosureRefCount = slot->closureRefCount;
    for (const PackEntry& owned : slot->entries) {
        PCMRegistry::RetiredGeneration g;
        if (registry.ExtractRetiring(owned.soundId,
                                     owned.registrationCookie, &g)) {
            row->successorEntries.push_back(std::move(g));
        }
    }
    for (PCMRegistry::RetiredGeneration& g : row->entries) {
        registry.ReinsertRetired(std::move(g));
    }

    AlignedBuffer successorBytes = std::move(slot->bytes);
    auto successorPackEntries = std::move(slot->entries);
    slot->bytes = std::move(row->bytes);
    slot->entries = std::move(row->packEntries);
    slot->revisionKey = row->revisionKey;
    slot->resourceToken = row->resourceToken;
    slot->closureRefCount = row->closureRefCount;

    row->bytes = std::move(successorBytes);
    row->entries = std::move(row->successorEntries);
    row->revisionKey = successorKey;
    row->resourceToken = successorToken;
    row->closureRefCount = successorClosureRefCount;
    row->packEntries = std::move(successorPackEntries);
    if (row->closureRefCount == 0 && row->Drained()) {
        ReleaseRetiringRow(*row);
    } else {
        row->state = RetiringState::Live;
    }
    predecessor = {};
}

VnPackHandle PackTable::OwnerOfSound(VnSoundId soundId,
                                     const PCMRegistry& registry) const {
    VnPackHandle owner = 0;
    (void)FindEntryBySound(soundId, registry, &owner);
    return owner;
}

bool PackTable::ActiveRevisionIdentity(
    VnPackHandle handle, PackRevisionKey* outKey,
    PackResourceRevisionToken* outToken) const noexcept {
    if (outKey == nullptr || outToken == nullptr) {
        return false;
    }
    *outKey = {};
    *outToken = 0;
    const Slot* slot = SlotFor(handle);
    if (slot == nullptr || slot->resourceToken == 0) {
        return false;
    }
    *outKey = slot->revisionKey;
    *outToken = slot->resourceToken;
    return true;
}

VnResult PackTable::LookupExact(
    PackResourceRevisionToken resourceToken, VnSoundId soundId,
    const PCMRegistry& registry, PCMView* out, std::uint64_t* mediaChecksum, bool* checksumKnown) const noexcept {
    if (checksumKnown != nullptr) *checksumKnown = false;
    if (mediaChecksum != nullptr) *mediaChecksum = 0;
    if (resourceToken == 0 || out == nullptr) {
        return VN_INVALID_ARG;
    }
    *out = {};
    for (const RetiringGeneration& row : m_retiring) {
        if (row.state != RetiringState::Live ||
            row.resourceToken != resourceToken) {
            continue;
        }
        for (const PCMRegistry::RetiredGeneration& entry : row.entries) {
            if (entry.SoundId() == soundId) {
                *out = entry.View();
                if (out->Valid())
                    for (const auto& metadata : row.packEntries)
                        if (metadata.soundId == soundId) {
                            if (mediaChecksum != nullptr) *mediaChecksum = metadata.mediaChecksum;
                            if (checksumKnown != nullptr) *checksumKnown = metadata.mediaChecksumKnown;
                            break;
                        }
                return out->Valid() ? VN_OK : VN_CONTENT_NOT_LOADED;
            }
        }
        for (const PackEntry& entry : row.packEntries) {
            if (entry.soundId == soundId && entry.registration.Valid()) {
                *out = entry.registration.View();
                if (mediaChecksum != nullptr) *mediaChecksum = entry.mediaChecksum;
                if (checksumKnown != nullptr) *checksumKnown = entry.mediaChecksumKnown;
                return VN_OK;
            }
        }
        return VN_CONTENT_NOT_LOADED;
    }
    for (const Slot& slot : m_slots) {
        if (!slot.loaded || slot.resourceToken != resourceToken) {
            continue;
        }
        for (const PackEntry& entry : slot.entries) {
            if (entry.soundId == soundId && entry.registration.Valid()) {
                *out = entry.registration.View();
                if (mediaChecksum != nullptr) *mediaChecksum = entry.mediaChecksum;
                if (checksumKnown != nullptr) *checksumKnown = entry.mediaChecksumKnown;
                return VN_OK;
            }
            if (entry.soundId != soundId ||
                registry.CookieOf(soundId) != entry.registrationCookie) {
                continue;
            }
            const PCMView view = registry.Lookup(soundId);
            if (view.Valid()) {
                *out = view;
                if (mediaChecksum != nullptr) *mediaChecksum = entry.mediaChecksum;
                if (checksumKnown != nullptr) *checksumKnown = entry.mediaChecksumKnown;
                return VN_OK;
            }
        }
        return VN_CONTENT_NOT_LOADED;
    }
    return VN_CONTENT_NOT_LOADED;
}

VnResult PackTable::LookupExactClosure(
    PackResourceClosureToken closureToken, VnSoundId soundId,
    const PCMRegistry& registry, PCMView* out, std::uint64_t* mediaChecksum, bool* checksumKnown) const noexcept {
    if (checksumKnown != nullptr) *checksumKnown = false;
    if (mediaChecksum != nullptr) *mediaChecksum = 0;
    if (out == nullptr || closureToken == 0) {
        return VN_INVALID_ARG;
    }
    *out = {};
    const ResourceClosureRow* row = ResourceClosureRowFor(closureToken);
    if (row == nullptr) {
        return VN_CONTENT_NOT_LOADED;
    }
    const std::size_t rowIndex =
        static_cast<std::size_t>(static_cast<std::uint32_t>(closureToken) - 1);
    const PackResourceRevisionToken* stored =
        ResourceClosureTokens(rowIndex);
    for (std::size_t i = 0; i < row->resourceCount; ++i) {
        const VnResult result = LookupExact(stored[i], soundId, registry, out, mediaChecksum, checksumKnown);
        if (result == VN_OK) {
            return VN_OK;
        }
    }
    return VN_CONTENT_NOT_LOADED;
}

} // namespace harken
