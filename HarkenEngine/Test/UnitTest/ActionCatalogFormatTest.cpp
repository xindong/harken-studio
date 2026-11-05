#include "UnitTest/CanonicalPackSetActivate.h"
#include "Common/ObjectIdTestSupport.h"
#include "Common/PackFoundationFixture.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace harken;
using harkentest::packfoundation::PartitionPayload;
using harkentest::packfoundation::RequirementPayload;

constexpr std::uint64_t kInitId = 11;
constexpr std::uint64_t kContentId = 22;

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align) {
    return (value + align - 1) / align * align;
}

class ImageBuilder {
public:
    explicit ImageBuilder(std::uint32_t packKind) : packKind_(packKind) {}

    std::size_t AddEntry(std::uint32_t kind, std::uint64_t packed,
                         std::vector<std::uint8_t> payload,
                         std::uint32_t vintage = 0) {
        if (kind == kPackEntryKindBusDefinitionSnapshot ||
            kind == kPackEntryKindBindingAttachment ||
            kind == kPackEntryKindPackDependencyEdge) {
            const auto index =
                AddEntry(kind, VnObjectId{}, std::move(payload), vintage);
            SetRelationOrdinal(index, packed);
            return index;
        }
        return AddEntry(kind, harkentest::ObjectId(packed), std::move(payload),
                        vintage);
    }

    std::size_t AddEntry(std::uint32_t kind, VnObjectId id,
                         std::vector<std::uint8_t> payload,
                         std::uint32_t vintage = 0) {
        if (vintage == 0) {
            vintage = PackEntryVintage(kind);
        }
        Entry entry;
        entry.kind = kind;
        entry.vintage = vintage;
        entry.id = id;
        entry.payload = std::move(payload);
        entries_.push_back(std::move(entry));
        return entries_.size() - 1;
    }

    void SetRelationOrdinal(std::size_t index, std::uint64_t ordinal) {
        entries_[index].relationOrdinal = ordinal;
    }

    std::vector<std::uint8_t> Build() {
        const std::uint64_t dirOffset = kPackFileHeaderBytes;
        const std::uint64_t dirBytes =
            static_cast<std::uint64_t>(entries_.size()) *
            kPackDirectoryEntryBytes;
        const std::uint64_t dirEnd = dirOffset + dirBytes;
        payloadOffsets_.assign(entries_.size(), 0);
        std::uint64_t cursor = AlignUp(dirEnd, kPackPayloadAlign);
        std::uint64_t contentEnd = dirEnd;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            payloadOffsets_[i] = cursor;
            contentEnd = cursor + entries_[i].payload.size();
            cursor = AlignUp(contentEnd, kPackPayloadAlign);
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(contentEnd), 0);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            std::memcpy(bytes.data() +
                            static_cast<std::size_t>(payloadOffsets_[i]),
                        entries_[i].payload.data(), entries_[i].payload.size());
        }
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            PackDirectoryEntry record;
            record.kind = entries_[i].kind;
            record.vintage = entries_[i].vintage;
            record.id = entries_[i].id;
            record.relationOrdinal = entries_[i].relationOrdinal;
            record.offset = payloadOffsets_[i];
            record.size = entries_[i].payload.size();
            record.payloadChecksum = Fnv1a64(
                bytes.data() + static_cast<std::size_t>(payloadOffsets_[i]),
                static_cast<std::size_t>(record.size));
            SerializePackDirectoryEntry(
                record, bytes.data() + static_cast<std::size_t>(dirOffset) +
                            i * kPackDirectoryEntryBytes);
        }
        PackFileHeader header;
        header.formatVersion = kPackFormatVersion;
        header.headerBytes = kPackFileHeaderBytes;
        header.fileBytes = contentEnd;
        header.packKind = packKind_;
        header.entryCount = static_cast<std::uint32_t>(entries_.size());
        header.directoryOffset = dirOffset;
        header.directoryChecksum =
            Fnv1a64(bytes.data() + static_cast<std::size_t>(dirOffset),
                    static_cast<std::size_t>(dirBytes));
        SerializePackFileHeader(header, bytes.data());
        return bytes;
    }

private:
    struct Entry {
        std::uint32_t kind = 0;
        std::uint32_t vintage = 0;
        VnObjectId id{};
        std::uint64_t relationOrdinal = 0;
        std::vector<std::uint8_t> payload;
    };

    std::uint32_t packKind_ = kPackKindInit;
    std::vector<Entry> entries_;
    std::vector<std::uint64_t> payloadOffsets_;
};

std::vector<std::uint8_t> MakeInitPartition(std::uint64_t* bodyBytes) {
    return PartitionPayload(kInitId, 1, 0, 0, bodyBytes, 64,
        {0x0102030405060708ULL, 0x090a0b0c0d0e0f10ULL});
}

std::vector<std::uint8_t> BuildInitBare() {
    std::uint64_t bodyBytes = 0;
    std::vector<std::uint8_t> partition = MakeInitPartition(&bodyBytes);
    ImageBuilder builder(kPackKindInit);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kInitId,
                     RequirementPayload(kInitId, 1, 1, 0, bodyBytes));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kInitId,
                     std::move(partition));
    harkentest::packfoundation::AppendNeutralMasterSnapshot(&builder);
    return builder.Build();
}

std::vector<std::uint8_t> BuildInitWithGameplayCatalog(
    std::vector<std::uint8_t> catalog,
    std::uint32_t vintage = 1u) {
    std::uint64_t bodyBytes = 0;
    std::vector<std::uint8_t> partition = MakeInitPartition(&bodyBytes);
    ImageBuilder builder(kPackKindInit);
    builder.AddEntry(kPackEntryKindGameplayCatalog, 0, std::move(catalog),
                     vintage);
    builder.AddEntry(kPackEntryKindPackRequirementBlock, kInitId,
                     RequirementPayload(kInitId, 1, 1, 0, bodyBytes));
    builder.AddEntry(kPackEntryKindCanonicalDefinitionPartition, kInitId,
                     std::move(partition));
    harkentest::packfoundation::AppendNeutralMasterSnapshot(&builder);
    return builder.Build();
}

PackGameplayCatalogHeader MakeIdentity() {
    PackGameplayCatalogHeader header;
    header.projectGuid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    header.requiredCapabilityMask = 1;
    header.publicationPairingFingerprint = 0x11;
    header.initPackStableId = kInitId;
    header.initContentRevision = 1;
    return header;
}

PackGameplayCatalogActiveDraft MakeActive(std::string_view name,
                                          std::uint64_t actionStableId) {
    PackGameplayCatalogActiveDraft row;
    row.actionStableId = harkentest::ObjectId(actionStableId);
    row.nameBindingRevision = 1;
    row.callableFingerprint = 0xC0FFEE;
    row.ownerPackStableId = kContentId;
    row.providerClosureFingerprint = 0xABC;
    row.name = name;
    return row;
}

PackGameplayCatalogRetiredDraft MakeRetired(std::string_view name,
                                            std::uint64_t lastActionStableId) {
    PackGameplayCatalogRetiredDraft row;
    row.lastActionStableId = harkentest::ObjectId(lastActionStableId);
    row.lastNameBindingRevision = 2;
    row.reason = kGameplayCatalogRetiredReasonRenamed;
    row.name = name;
    return row;
}

std::string_view NameAt(const GameplayCatalogImage& catalog,
                        std::uint32_t offset, std::uint16_t length) {
    return std::string_view(
        reinterpret_cast<const char*>(catalog.namePool.data() + offset),
        length);
}

void FinalizeCatalogPayload(std::vector<std::uint8_t>* payload) {
    PackGameplayCatalogHeader header;
    LoadPackGameplayCatalogHeader(payload->data(), &header);
    for (std::uint32_t i = 0; i < header.activeCount; ++i) {
        std::uint8_t* row =
            payload->data() + kGameplayCatalogHeaderBytes +
            static_cast<std::size_t>(i) * kGameplayCatalogActiveRowBytes;
        StoreLE64(row + 72, Fnv1a64(row, 72));
    }
    StoreLE64(payload->data() + 24, 0);
    StoreLE64(payload->data() + 56, 0);
    StoreLE64(payload->data() + 88, 0);
    header.digest = GameplayCatalogDigest(payload->data(), payload->size());
    StoreLE64(payload->data() + 24, header.digest);
    header.publicationPairingFingerprint =
        GameplayCatalogPublicationPairing(header);
    StoreLE64(payload->data() + 56, header.publicationPairingFingerprint);
    StoreLE64(payload->data() + 88, Fnv1a64(payload->data(), 88));
}

TEST(ActionCatalogFormat, InitWithoutKind25StillParses) {
    const std::vector<std::uint8_t> bytes = BuildInitBare();
    PackCanonicalImage image;
    ASSERT_EQ(VN_OK, ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
    EXPECT_FALSE(image.hasGameplayCatalog);
    EXPECT_FALSE(image.hasCatalog);
}

TEST(ActionCatalogFormat, CatalogMustMatchActiveInitIdentity) {
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto activated = harkentest::ActivateInitBytesV1(
        std::filesystem::path(HARKEN_TEMP_DIR) / "gameplay-catalog-identity",
        BuildInitBare(), kInitId);
    ASSERT_EQ(VN_OK, activated.result);
    const auto before = session.Engine()->packComposition.PublishedHandle();
    for (bool wrongId : {false, true}) {
        auto identity = MakeIdentity();
        if (wrongId) ++identity.initPackStableId;
        else ++identity.initContentRevision;
        std::vector<std::uint8_t> payload;
        ASSERT_TRUE(EncodeGameplayCatalog(identity, {}, {}, &payload));
        VnPackByteSpan span{sizeof(span), 0, payload.data(), payload.size()};
        EXPECT_EQ(VN_PACK_STALE_REVISION,
                  Vn_SetGameplayCatalog(activated.initHandle, &span));
        EXPECT_EQ(before, session.Engine()->packComposition.PublishedHandle());
    }
}



TEST(ActionCatalogFormat, RoundTripOneActiveAndOneRetired) {
    std::vector<std::uint8_t> catalog;
    ASSERT_TRUE(EncodeGameplayCatalog(
        MakeIdentity(), {MakeActive("Play_Fire", 100)},
        {MakeRetired("Play_Old", 99)}, &catalog));
    GameplayCatalogImage image;
    ASSERT_EQ(VN_OK, ParseGameplayCatalog(catalog.data(), catalog.size(), &image));
    ASSERT_EQ(1u, image.active.size());
    ASSERT_EQ(1u, image.retired.size());
    EXPECT_EQ(harkentest::ObjectId(100u), image.active[0].actionStableId);
    EXPECT_EQ(std::string("Play_Fire"),
              std::string(NameAt(image,
                                 image.active[0].nameOffset,
                                 image.active[0].nameLength)));
    EXPECT_EQ(harkentest::ObjectId(99u), image.retired[0].lastActionStableId);
    EXPECT_EQ(std::string("Play_Old"),
              std::string(NameAt(image,
                                 image.retired[0].nameOffset,
                                 image.retired[0].nameLength)));
    EXPECT_EQ(kGameplayCatalogRetiredReasonRenamed,
              image.retired[0].reason);
    EXPECT_EQ(GameplayCatalogDigest(catalog.data(), catalog.size()),
              image.header.digest);
}

TEST(ActionCatalogFormat, DigestStableWhenDraftsShuffled) {
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    ASSERT_TRUE(EncodeGameplayCatalog(
        MakeIdentity(),
        {MakeActive("Play_Zulu", 8), MakeActive("Play_Alpha", 3)},
        {MakeRetired("Play_Old", 1)}, &first));
    ASSERT_TRUE(EncodeGameplayCatalog(
        MakeIdentity(),
        {MakeActive("Play_Alpha", 3), MakeActive("Play_Zulu", 8)},
        {MakeRetired("Play_Old", 1)}, &second));
    EXPECT_EQ(first, second);
    GameplayCatalogImage image;
    ASSERT_EQ(VN_OK,
              ParseGameplayCatalog(first.data(), first.size(), &image));
    EXPECT_EQ(LoadLE64(first.data() + 24), image.header.digest);
    EXPECT_EQ(std::string("Play_Alpha"),
              std::string(NameAt(image,
                                 image.active[0].nameOffset,
                                 image.active[0].nameLength)));
    EXPECT_EQ(std::string("Play_Zulu"),
              std::string(NameAt(image,
                                 image.active[1].nameOffset,
                                 image.active[1].nameLength)));
}

TEST(ActionCatalogFormat, TwoEncodesOfSameRowsAreByteIdentical) {
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    const auto identity = MakeIdentity();
    const auto active = MakeActive("Play_Fire", 100);
    const auto retired = MakeRetired("Play_Old", 99);
    ASSERT_TRUE(EncodeGameplayCatalog(identity, {active}, {retired}, &first));
    ASSERT_TRUE(EncodeGameplayCatalog(identity, {active}, {retired}, &second));
    EXPECT_EQ(first, second);
}

TEST(ActionCatalogFormat, RetiredGameplayEntryIsBadPack) {
    std::vector<std::uint8_t> catalog;
    ASSERT_TRUE(EncodeGameplayCatalog(
        MakeIdentity(), {MakeActive("Play_Fire", 100)}, {}, &catalog));
    const std::vector<std::uint8_t> bytes =
        BuildInitWithGameplayCatalog(std::move(catalog));
    PackCanonicalImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParsePackCanonicalImage(bytes.data(), bytes.size(), &image));
}

TEST(ActionCatalogFormat, CountOverflowIsCatalogCapacity) {
    std::vector<std::uint8_t> payload(kGameplayCatalogHeaderBytes, 0);
    StoreLE32(payload.data() + 0, 1);
    StoreLE32(payload.data() + 4, kPackGameplayCatalogVintage);
    StoreLE32(payload.data() + 32, kGameplayCatalogMaxEntries + 1);
    GameplayCatalogImage image;
    EXPECT_EQ(VN_PACK_CATALOG_CAPACITY,
              ParseGameplayCatalog(payload.data(), payload.size(), &image));
}

TEST(ActionCatalogFormat, SizeOverflowIsCatalogCapacity) {
    std::vector<std::uint8_t> payload(kGameplayCatalogMaxBytes + 1, 0);
    StoreLE32(payload.data() + 0, 1);
    StoreLE32(payload.data() + 4, kPackGameplayCatalogVintage);
    GameplayCatalogImage image;
    EXPECT_EQ(VN_PACK_CATALOG_CAPACITY,
              ParseGameplayCatalog(payload.data(), payload.size(), &image));
}

TEST(ActionCatalogFormat, DuplicateActiveNamesAreBadPack) {
    std::vector<std::uint8_t> catalog;
    ASSERT_TRUE(EncodeGameplayCatalog(
        MakeIdentity(),
        {MakeActive("Play_A", 5), MakeActive("Play_B", 6)}, {}, &catalog));
    PackGameplayCatalogHeader header;
    LoadPackGameplayCatalogHeader(catalog.data(), &header);
    const std::uint8_t* retiredBase =
        catalog.data() + kGameplayCatalogHeaderBytes +
        static_cast<std::size_t>(header.activeCount) *
            kGameplayCatalogActiveRowBytes;
    std::uint8_t* pool = catalog.data() + (retiredBase - catalog.data()) +
                         static_cast<std::size_t>(header.retiredCount) *
                             kGameplayCatalogRetiredRowBytes;
    std::memcpy(pool + 6, "Play_A", 6);
    FinalizeCatalogPayload(&catalog);
    GameplayCatalogImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParseGameplayCatalog(catalog.data(), catalog.size(), &image));
}

TEST(ActionCatalogFormat, DuplicateActionStableIdsAreBadPack) {
    std::vector<std::uint8_t> catalog;
    ASSERT_TRUE(EncodeGameplayCatalog(
        MakeIdentity(),
        {MakeActive("Play_A", 5), MakeActive("Play_B", 6)}, {}, &catalog));
    std::uint8_t* second =
        catalog.data() + kGameplayCatalogHeaderBytes +
        kGameplayCatalogActiveRowBytes;
    StoreLE64(second, 5);
    FinalizeCatalogPayload(&catalog);
    GameplayCatalogImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParseGameplayCatalog(catalog.data(), catalog.size(), &image));
}

TEST(ActionCatalogFormat, RetiredNameEqualToActiveIsBadPack) {
    std::vector<std::uint8_t> catalog;
    ASSERT_TRUE(EncodeGameplayCatalog(
        MakeIdentity(), {MakeActive("Play_A", 5)},
        {MakeRetired("Play_B", 9)}, &catalog));
    PackGameplayCatalogHeader header;
    LoadPackGameplayCatalogHeader(catalog.data(), &header);
    std::uint8_t* pool =
        catalog.data() + kGameplayCatalogHeaderBytes +
        static_cast<std::size_t>(header.activeCount) *
            kGameplayCatalogActiveRowBytes +
        static_cast<std::size_t>(header.retiredCount) *
            kGameplayCatalogRetiredRowBytes;
    std::memcpy(pool + 6, "Play_A", 6);
    FinalizeCatalogPayload(&catalog);
    GameplayCatalogImage image;
    EXPECT_EQ(VN_BAD_PACK,
              ParseGameplayCatalog(catalog.data(), catalog.size(), &image));
}

} // namespace
