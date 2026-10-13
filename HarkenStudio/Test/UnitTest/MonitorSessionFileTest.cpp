#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorSessionFile.h"
#include "Monitor/support/MonitorFixtureLoader.h"
#include "Monitor/support/MonitorSha256.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef HARKEN_MONITOR_FIXTURE_DIR
#define HARKEN_MONITOR_FIXTURE_DIR "."
#endif
#ifndef HARKEN_MONITOR_EDITOR_FIXTURE_DIR
#define HARKEN_MONITOR_EDITOR_FIXTURE_DIR "."
#endif

namespace {

std::string sessionPath(const char* name)
{
    return std::string(HARKEN_MONITOR_FIXTURE_DIR) + "/session/" + name;
}

std::vector<uint8_t> readAll(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
}

bool fixtureManifestMatches(const char* name, const std::vector<uint8_t>& bytes)
{
    const auto manifestBytes = readAll(std::string(HARKEN_MONITOR_FIXTURE_DIR) +
                                       "/session/manifest.json");
    const std::string manifest(manifestBytes.begin(), manifestBytes.end());
    if (manifest.find("\"schemaVersion\": 1") == std::string::npos ||
        manifest.find("\"isProvisional\": false") == std::string::npos)
        return false;

    const std::string pathEntry = std::string("\"path\": \"") + name + "\"";
    const size_t begin = manifest.find(pathEntry);
    if (begin == std::string::npos)
        return false;
    const size_t end = manifest.find('}', begin);
    if (end == std::string::npos)
        return false;
    const std::string entry = manifest.substr(begin, end - begin);
    return entry.find("\"isProvisional\": false") != std::string::npos &&
           entry.find(VNX::Monitor::TestSupport::sha256Hex(bytes)) != std::string::npos;
}

std::vector<uint8_t> loadSessionFixture(const char* name)
{
    const auto bytes = readAll(sessionPath(name));
    if (bytes.empty() || !fixtureManifestMatches(name, bytes))
        return {};
    return bytes;
}

VNX::Monitor::MonitorCapabilities storeCapabilities(uint64_t resolvedQueueBytes =
                                            VN_MONITOR_HARD_CONSUMER_QUEUE_BYTES)
{
    VNX::Monitor::MonitorCapabilities capabilities{};
    capabilities.resolvedConsumerQueueBytes = resolvedQueueBytes;
    capabilities.hardConsumerQueueBytes = VN_MONITOR_HARD_CONSUMER_QUEUE_BYTES;
    return capabilities;
}

void writeU16(std::vector<uint8_t>* out, uint16_t value)
{
    out->push_back(static_cast<uint8_t>(value));
    out->push_back(static_cast<uint8_t>(value >> 8));
}

void writeU32(std::vector<uint8_t>* out, uint32_t value)
{
    out->push_back(static_cast<uint8_t>(value));
    out->push_back(static_cast<uint8_t>(value >> 8));
    out->push_back(static_cast<uint8_t>(value >> 16));
    out->push_back(static_cast<uint8_t>(value >> 24));
}

void writeU64(std::vector<uint8_t>* out, uint64_t value)
{
    writeU32(out, static_cast<uint32_t>(value));
    writeU32(out, static_cast<uint32_t>(value >> 32));
}

uint32_t crc32c(const uint8_t* bytes, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82f63b78u & ((crc & 1u) != 0 ? 0xffffffffu : 0u));
    }
    return ~crc;
}

std::vector<uint8_t> packedFileHeader()
{
    std::vector<uint8_t> header;
    writeU32(&header, VN_MONITOR_SESSION_FORMAT_MAJOR);
    writeU32(&header, VN_MONITOR_SESSION_FORMAT_MINOR);
    writeU32(&header, 0);
    writeU64(&header, VN_MONITOR_SESSION_FILE_MAGIC);
    writeU64(&header, 64u);
    writeU64(&header, 1u);
    writeU64(&header, 2u);
    writeU32(&header, 0);
    writeU64(&header, 3u);
    writeU32(&header, 0);
    header.resize(64, 0);
    header[60] = 0;
    header[61] = 0;
    header[62] = 0;
    header[63] = 0;
    const uint32_t headerCrc = crc32c(header.data(), header.size());
    header[60] = static_cast<uint8_t>(headerCrc);
    header[61] = static_cast<uint8_t>(headerCrc >> 8);
    header[62] = static_cast<uint8_t>(headerCrc >> 16);
    header[63] = static_cast<uint8_t>(headerCrc >> 24);
    return header;
}

std::vector<uint8_t> packedCommit(uint16_t kind, uint16_t flags, uint64_t sequence,
                                  const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> header;
    writeU16(&header, kind);
    writeU16(&header, flags);
    writeU16(&header, 1u);
    writeU16(&header, static_cast<uint16_t>(VN_MONITOR_SESSION_CHUNK_HEADER_BYTES));
    writeU64(&header, payload.size());
    writeU64(&header, sequence);
    writeU32(&header, crc32c(payload.data(), payload.size()));
    writeU32(&header, 0);
    const uint32_t headerCrc = crc32c(header.data(), 28);
    header[28] = static_cast<uint8_t>(headerCrc);
    header[29] = static_cast<uint8_t>(headerCrc >> 8);
    header[30] = static_cast<uint8_t>(headerCrc >> 16);
    header[31] = static_cast<uint8_t>(headerCrc >> 24);

    std::vector<uint8_t> footer;
    writeU64(&footer, VN_MONITOR_SESSION_COMMIT_MAGIC);
    writeU64(&footer, sequence);
    writeU64(&footer, payload.size());

    std::vector<uint8_t> out = header;
    out.insert(out.end(), payload.begin(), payload.end());
    out.insert(out.end(), footer.begin(), footer.end());
    return out;
}

std::string writeTempSession(const std::vector<uint8_t>& bytes, const char* name)
{
    const std::string path = std::string(HARKEN_MONITOR_EDITOR_FIXTURE_DIR) + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path;
}

} // namespace

TEST(MonitorSessionFile, LegalFixtureRestoresToLastCompleteFooter)
{
    const auto bytes = loadSessionFixture("legal.bin");
    ASSERT_FALSE(bytes.empty());

    const auto result = VNX::Monitor::MonitorSessionFile::restore(sessionPath("legal.bin"));
    EXPECT_EQ(result.status, VN_OK);
    EXPECT_EQ(result.bytesConsumed, bytes.size());
    ASSERT_EQ(result.commits.size(), 1u);
    EXPECT_EQ(result.commits[0].chunkSequence, 1u);
    ASSERT_EQ(result.records.size(), 1u);
    EXPECT_EQ(result.records[0].header.recordKind, VN_MONITOR_RECORD_EVENT);
    EXPECT_EQ(result.records[0].header.deliverySequence, 0u);
}

TEST(MonitorSessionFile, LegalFixtureEntersStoreOnlyThroughAppendBatch)
{
    ASSERT_FALSE(loadSessionFixture("legal.bin").empty());
    VNX::Monitor::MonitorStore store(storeCapabilities());
    ASSERT_TRUE(store.usable());

    const auto result = VNX::Monitor::MonitorSessionFile::restore(sessionPath("legal.bin"),
                                                                    &store);
    EXPECT_EQ(result.status, VN_OK);
    EXPECT_FALSE(result.storeFailed);
    EXPECT_EQ(result.storeResult.status, VN_OK);
    EXPECT_EQ(store.generation(), 1u);
    const auto snapshot = store.snapshot();
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->records.size(), 1u);
    EXPECT_TRUE(result.records.empty());
}

TEST(MonitorSessionFile, TruncatedFixtureDoesNotSearchForFooter)
{
    const auto bytes = loadSessionFixture("truncated.bin");
    ASSERT_FALSE(bytes.empty());

    const auto result = VNX::Monitor::MonitorSessionFile::restore(sessionPath("truncated.bin"));
    EXPECT_EQ(result.status, VN_OK);
    EXPECT_TRUE(result.commits.empty());
    EXPECT_LT(result.bytesConsumed, bytes.size());
}

TEST(MonitorSessionFile, DamagedTailKeepsLaterCompleteCommit)
{
    const auto bytes = loadSessionFixture("recovered_tail.bin");
    ASSERT_FALSE(bytes.empty());

    const auto result = VNX::Monitor::MonitorSessionFile::restore(
        sessionPath("recovered_tail.bin"));
    EXPECT_EQ(result.status, VN_OK);
    ASSERT_EQ(result.commits.size(), 1u);
    EXPECT_EQ(result.commits[0].chunkSequence, 2u);
    EXPECT_EQ(result.droppedRequired, 1u);
    EXPECT_EQ(result.bytesConsumed, bytes.size());
}

TEST(MonitorSessionFile, TrailingJunkStopsAtNextCandidate)
{
    const auto bytes = loadSessionFixture("trailing_junk.bin");
    ASSERT_FALSE(bytes.empty());

    const auto result = VNX::Monitor::MonitorSessionFile::restore(
        sessionPath("trailing_junk.bin"));
    EXPECT_EQ(result.status, VN_OK);
    ASSERT_EQ(result.commits.size(), 1u);
    EXPECT_LT(result.bytesConsumed, bytes.size());
}

TEST(MonitorSessionFile, UnknownOptionalKeepsPayloadBytes)
{
    const auto bytes = loadSessionFixture("optional_unknown.bin");
    ASSERT_FALSE(bytes.empty());

    const auto result = VNX::Monitor::MonitorSessionFile::restore(
        sessionPath("optional_unknown.bin"));
    EXPECT_EQ(result.status, VN_OK);
    ASSERT_EQ(result.commits.size(), 2u);
    EXPECT_EQ(result.commits[0].chunkSequence, 1u);
    EXPECT_EQ(result.commits[0].kind, 200u);
    ASSERT_EQ(result.commits[0].skippedBytes.size(), 4u);
    EXPECT_EQ(std::vector<uint8_t>(result.commits[0].skippedBytes),
              std::vector<uint8_t>(bytes.begin() + 96, bytes.begin() + 100));
}

TEST(MonitorSessionFile, RequiredDecodeFailureDropsWholeCommit)
{
    ASSERT_FALSE(loadSessionFixture("required_decode_fail.bin").empty());

    const auto result = VNX::Monitor::MonitorSessionFile::restore(
        sessionPath("required_decode_fail.bin"));
    EXPECT_EQ(result.status, VN_OK);
    EXPECT_TRUE(result.commits.empty());
    EXPECT_TRUE(result.records.empty());
    EXPECT_EQ(result.droppedRequired, 1u);
}

TEST(MonitorSessionFile, SourceHashDoesNotChange)
{
    const auto before = loadSessionFixture("legal.bin");
    ASSERT_FALSE(before.empty());
    const auto beforeHash = VNX::Monitor::TestSupport::sha256Hex(before);

    const auto result = VNX::Monitor::MonitorSessionFile::restore(sessionPath("legal.bin"));
    ASSERT_EQ(result.status, VN_OK);

    const auto after = readAll(sessionPath("legal.bin"));
    ASSERT_FALSE(after.empty());
    EXPECT_EQ(VNX::Monitor::TestSupport::sha256Hex(after), beforeHash);
}

TEST(MonitorSessionFile, NonSessionNativeLayoutIsIncompatible)
{
    const std::string path = std::string(HARKEN_MONITOR_EDITOR_FIXTURE_DIR) +
                             "/native_layout.bin";
    ASSERT_FALSE(readAll(path).empty());
    const auto result = VNX::Monitor::MonitorSessionFile::restore(path);
    EXPECT_EQ(result.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_TRUE(result.commits.empty());
}

TEST(MonitorSessionFile, StoreCapacityStopsWithoutReportingSuccess)
{
    ASSERT_FALSE(loadSessionFixture("legal.bin").empty());
    VNX::Monitor::MonitorStore store(storeCapabilities(0));
    ASSERT_TRUE(store.usable());

    const auto result = VNX::Monitor::MonitorSessionFile::restore(sessionPath("legal.bin"),
                                                                    &store);
    EXPECT_TRUE(result.storeFailed);
    EXPECT_EQ(result.storeResult.reject, VNX::Monitor::MonitorStoreReject::CapacitySession);
    EXPECT_TRUE(result.storeResult.retryable);
    EXPECT_EQ(result.reject, VNX::Monitor::MonitorStoreReject::CapacitySession);
    EXPECT_TRUE(result.retryable);
    EXPECT_NE(result.status, VN_OK);
    EXPECT_TRUE(result.commits.empty());
}

TEST(MonitorSessionFile, EmptyUnknownOptionalKeepsIdentityWithStore)
{
    std::vector<uint8_t> bytes = packedFileHeader();
    const auto chunk = packedCommit(200u, static_cast<uint16_t>(VN_MONITOR_CHUNK_OPTIONAL),
                                    1u, {});
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    const std::string path = writeTempSession(bytes, "empty_unknown_optional.bin");

    const auto withoutStore = VNX::Monitor::MonitorSessionFile::restore(path);
    EXPECT_EQ(withoutStore.status, VN_OK);
    ASSERT_EQ(withoutStore.commits.size(), 1u);
    EXPECT_TRUE(withoutStore.commits[0].skippedBytes.empty());
    ASSERT_EQ(withoutStore.records.size(), 1u);
    EXPECT_TRUE(withoutStore.records[0].unknownOptional);
    EXPECT_TRUE(withoutStore.records[0].skippedBytes.empty());

    VNX::Monitor::MonitorStore store(storeCapabilities());
    ASSERT_TRUE(store.usable());
    const auto withStore = VNX::Monitor::MonitorSessionFile::restore(path, &store);
    EXPECT_EQ(withStore.status, VN_OK);
    EXPECT_FALSE(withStore.storeFailed);
    EXPECT_EQ(withStore.droppedOptional, 0u);
    ASSERT_EQ(withStore.commits.size(), 1u);
    EXPECT_TRUE(withStore.commits[0].skippedBytes.empty());
    const auto snapshot = store.snapshot();
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->records.size(), 1u);
    EXPECT_TRUE(snapshot->records[0].unknownOptional);
    EXPECT_TRUE(snapshot->byStream[0].empty());
    EXPECT_TRUE(snapshot->byRecordKind.empty());
    std::remove(path.c_str());
}

TEST(MonitorSessionFile, AllocatorDeniedStopsWithoutReportingSuccess)
{
    ASSERT_FALSE(loadSessionFixture("legal.bin").empty());
    const auto result = VNX::Monitor::MonitorSessionFile::restore(
        sessionPath("legal.bin"),
        nullptr,
        [](std::size_t) { return false; });
    EXPECT_NE(result.status, VN_OK);
    EXPECT_EQ(result.reject, VNX::Monitor::MonitorStoreReject::AllocatorDenied);
    EXPECT_TRUE(result.retryable);
    EXPECT_TRUE(result.commits.empty());
    EXPECT_EQ(result.droppedRequired, 0u);
}

TEST(MonitorSessionFile, PermanentDecodeIncompatibilityIsNotRetryable)
{
    const auto fixture = VNX::Monitor::TestSupport::loadEditorFixture("required_unknown.bin");
    ASSERT_EQ(fixture.status, VN_OK);
    ASSERT_FALSE(fixture.packed.empty());

    std::vector<uint8_t> bytes = packedFileHeader();
    const auto chunk = packedCommit(static_cast<uint16_t>(VN_MONITOR_CHUNK_EVENTS),
                                    static_cast<uint16_t>(VN_MONITOR_CHUNK_REQUIRED),
                                    1u, fixture.packed);
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    const std::string path = writeTempSession(bytes, "permanent_incompatible.bin");

    const auto result = VNX::Monitor::MonitorSessionFile::restore(path);
    EXPECT_NE(result.status, VN_OK);
    EXPECT_EQ(result.reject, VNX::Monitor::MonitorStoreReject::None);
    EXPECT_FALSE(result.retryable);
    EXPECT_TRUE(result.commits.empty());
    std::remove(path.c_str());
}
