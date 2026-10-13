#include "monitor_session_codec.h"

#include "JsonReader.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef HARKEN_MONITOR_SESSION_FIXTURE_DIR
#define HARKEN_MONITOR_SESSION_FIXTURE_DIR "."
#endif

namespace {

std::string FixturePath(const char* name) {
    return std::string(HARKEN_MONITOR_SESSION_FIXTURE_DIR) + "/" + name;
}

std::vector<uint8_t> ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << path;
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

void WriteU32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

void WriteU64(uint8_t* p, uint64_t value) {
    WriteU32(p, static_cast<uint32_t>(value));
    WriteU32(p + 4, static_cast<uint32_t>(value >> 32));
}

VnMonitorSessionScanResult ScanNamed(const char* name,
                                     std::vector<uint8_t>* bytes,
                                     std::vector<VnMonitorSessionCommitView>* out) {
    *bytes = ReadAll(FixturePath(name));
    EXPECT_FALSE(bytes->empty()) << name;
    out->assign(8, VnMonitorSessionCommitView{});
    VnMonitorDecodedRecord scratch[8]{};
    uint8_t skip[1024]{};
    auto result = vn_monitor_scan_session(bytes->data(), bytes->size(), out->data(), 8,
                                          scratch, 8, skip, sizeof(skip));
    out->resize(result.commitsWritten);
    return result;
}

void ScanCheckedIn(const char* name,
                   std::vector<uint8_t>* bytes,
                   std::vector<VnMonitorSessionCommitView>* out) {
    auto result = ScanNamed(name, bytes, out);
    ASSERT_EQ(result.status, VN_OK) << name;
}

} // namespace

TEST(MonitorFixture, SessionPackedSizesAreNotNativeStructs) {
    EXPECT_EQ(VN_MONITOR_SESSION_CHUNK_HEADER_BYTES, 32u);
    EXPECT_EQ(VN_MONITOR_SESSION_COMMIT_FOOTER_BYTES, 24u);
    EXPECT_EQ(VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES, 64u);
    EXPECT_NE(sizeof(VnMonitorSessionChunkHeader),
              static_cast<size_t>(VN_MONITOR_SESSION_CHUNK_HEADER_BYTES));
    EXPECT_NE(sizeof(VnMonitorSessionCommitFooter),
              static_cast<size_t>(VN_MONITOR_SESSION_COMMIT_FOOTER_BYTES));
    EXPECT_NE(sizeof(VnMonitorSessionFileHeader),
              static_cast<size_t>(VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES));
}

TEST(MonitorFixture, SessionChunkHeaderCrcCoversTwentyEightBytes) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits;
    ScanCheckedIn("legal.bin", &bytes, &commits);
    ASSERT_EQ(commits.size(), 1u);
    ASSERT_GE(commits[0].rawByteCount, 32u);
    const uint8_t* header = commits[0].rawBytes;
    const uint32_t stored = static_cast<uint32_t>(header[28]) |
                            (static_cast<uint32_t>(header[29]) << 8) |
                            (static_cast<uint32_t>(header[30]) << 16) |
                            (static_cast<uint32_t>(header[31]) << 24);
    EXPECT_EQ(vn_monitor_crc32c(header, 28), stored);
    uint8_t zeroed[32];
    std::memcpy(zeroed, header, 32);
    zeroed[28] = 0;
    zeroed[29] = 0;
    zeroed[30] = 0;
    zeroed[31] = 0;
    EXPECT_NE(vn_monitor_crc32c(zeroed, 32), stored);
}

TEST(MonitorFixture, SessionLegalFileCommitsEventChunk) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits;
    ScanCheckedIn("legal.bin", &bytes, &commits);
    ASSERT_EQ(commits.size(), 1u);
    EXPECT_EQ(commits[0].kind, VN_MONITOR_CHUNK_EVENTS);
    EXPECT_EQ(commits[0].chunkSequence, 1u);
    EXPECT_EQ(commits[0].visible, 1u);
    EXPECT_EQ(commits[0].flags, VN_MONITOR_CHUNK_REQUIRED);
}

TEST(MonitorFixture, SessionMissingFooterKeepsNoCommit) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits;
    auto result = ScanNamed("truncated.bin", &bytes, &commits);
    ASSERT_EQ(result.status, VN_OK);
    EXPECT_EQ(commits.size(), 0u);
    EXPECT_LT(result.bytesConsumed, bytes.size());
}

TEST(MonitorFixture, SessionDamagedTailStillFindsLaterCommit) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits;
    auto result = ScanNamed("recovered_tail.bin", &bytes, &commits);
    ASSERT_EQ(result.status, VN_OK);
    ASSERT_EQ(commits.size(), 1u);
    EXPECT_EQ(commits[0].chunkSequence, 2u);
    EXPECT_EQ(commits[0].visible, 1u);
    EXPECT_EQ(result.droppedRequired, 1u);
    const auto legal = ReadAll(FixturePath("legal.bin"));
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(), bytes.data(),
                                          bytes.size()),
              VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE);
}

TEST(MonitorFixture, SessionOptionalCrcFailureIsPartial) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits;
    auto result = ScanNamed("optional_crc_fail.bin", &bytes, &commits);
    ASSERT_EQ(result.status, VN_OK);
    ASSERT_EQ(commits.size(), 1u);
    EXPECT_EQ(commits[0].kind, VN_MONITOR_CHUNK_EVENTS);
    EXPECT_EQ(commits[0].chunkSequence, 2u);
    EXPECT_EQ(result.droppedOptional, 1u);
    EXPECT_EQ(result.droppedRequired, 0u);
    const auto legal = ReadAll(FixturePath("legal.bin"));
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(), bytes.data(),
                                          bytes.size()),
              VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE);
}

TEST(MonitorFixture, SessionNineCommitsStayComparable) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits(16);
    bytes = ReadAll(FixturePath("nine_commits.bin"));
    ASSERT_FALSE(bytes.empty());
    VnMonitorDecodedRecord scratch[16]{};
    uint8_t skip[4096]{};
    auto result = vn_monitor_scan_session(bytes.data(), bytes.size(), commits.data(), 16,
                                          scratch, 16, skip, sizeof(skip));
    ASSERT_EQ(result.status, VN_OK);
    EXPECT_EQ(result.commitsWritten, 9u);
    EXPECT_EQ(result.droppedRequired, 0u);
    EXPECT_EQ(result.bytesConsumed, bytes.size());
    const auto legal = ReadAll(FixturePath("legal.bin"));
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(), bytes.data(),
                                          bytes.size()),
              VN_MONITOR_COMPARE_COMPARABLE);
    EXPECT_EQ(vn_monitor_compare_sessions(bytes.data(), bytes.size(), bytes.data(),
                                          bytes.size()),
              VN_MONITOR_COMPARE_COMPARABLE);
}

TEST(MonitorFixture, SessionNativeMemcpyIncompatible) {
    VnMonitorSessionFileHeader native{};
    native.structSize = sizeof(native);
    native.fileMagic = VN_MONITOR_SESSION_FILE_MAGIC;
    std::vector<uint8_t> bytes(sizeof(native) + 8, 0);
    std::memcpy(bytes.data(), &native, sizeof(native));
    VnMonitorSessionCommitView view{};
    VnMonitorDecodedRecord scratch{};
    uint8_t skip[8]{};
    auto result = vn_monitor_scan_session(bytes.data(), bytes.size(), &view, 1, &scratch,
                                          1, skip, sizeof(skip));
    EXPECT_EQ(result.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(result.commitsWritten, 0u);
}

TEST(MonitorFixture, SessionOptionalUnknownKeptRaw) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits;
    ScanCheckedIn("optional_unknown.bin", &bytes, &commits);
    ASSERT_EQ(commits.size(), 2u);
    EXPECT_EQ(commits[0].kind, 200u);
    EXPECT_EQ(commits[0].flags, VN_MONITOR_CHUNK_OPTIONAL);
    EXPECT_EQ(commits[0].payloadBytes, 4u);
    EXPECT_EQ(commits[0].rawByteCount, 32u + 4u + 24u);
    ASSERT_GE(bytes.size(), VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES +
                                static_cast<size_t>(commits[0].rawByteCount));
    EXPECT_EQ(std::memcmp(commits[0].rawBytes,
                          bytes.data() + VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES,
                          static_cast<size_t>(commits[0].rawByteCount)),
              0);
    ASSERT_GE(commits[0].payloadBytes, 4u);
    EXPECT_EQ(commits[0].payload[0], 0x11);
    EXPECT_EQ(commits[0].payload[1], 0x22);
    EXPECT_EQ(commits[0].payload[2], 0x33);
    EXPECT_EQ(commits[0].payload[3], 0x44);
    EXPECT_EQ(commits[1].kind, VN_MONITOR_CHUNK_EVENTS);
    EXPECT_EQ(commits[1].visible, 1u);
}

TEST(MonitorFixture, SessionTraceMagicIncompatible) {
    std::vector<uint8_t> bytes(VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES, 0);
    WriteU32(bytes.data(), VN_MONITOR_SESSION_FORMAT_MAJOR);
    WriteU32(bytes.data() + 4, VN_MONITOR_SESSION_FORMAT_MINOR);
    WriteU64(bytes.data() + 12, 0x43525456ull);
    WriteU64(bytes.data() + 20, VN_MONITOR_SESSION_FILE_HEADER_PACKED_BYTES);
    WriteU32(bytes.data() + 60, vn_monitor_crc32c(bytes.data(), bytes.size()));
    VnMonitorSessionCommitView view{};
    VnMonitorDecodedRecord scratch{};
    uint8_t skip[8]{};
    auto result = vn_monitor_scan_session(bytes.data(), bytes.size(), &view, 1, &scratch,
                                          1, skip, sizeof(skip));
    EXPECT_EQ(result.status, VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(result.commitsWritten, 0u);
}

TEST(MonitorFixture, SessionTrailingJunkIsMissingData) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits;
    auto result = ScanNamed("trailing_junk.bin", &bytes, &commits);
    ASSERT_EQ(result.status, VN_OK);
    ASSERT_EQ(commits.size(), 1u);
    EXPECT_LT(result.bytesConsumed, bytes.size());
    const auto legal = ReadAll(FixturePath("legal.bin"));
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(), bytes.data(),
                                          bytes.size()),
              VN_MONITOR_COMPARE_MISSING_DATA);
}

TEST(MonitorFixture, SessionRequiredDecodeFailureOmitsCommit) {
    std::vector<uint8_t> bytes;
    std::vector<VnMonitorSessionCommitView> commits;
    auto result = ScanNamed("required_decode_fail.bin", &bytes, &commits);
    ASSERT_EQ(result.status, VN_OK);
    EXPECT_EQ(commits.size(), 0u);
    EXPECT_EQ(result.droppedRequired, 1u);
    EXPECT_EQ(result.bytesConsumed, bytes.size());
}

TEST(MonitorFixture, SessionCompareFourStates) {
    const auto legal = ReadAll(FixturePath("legal.bin"));
    const auto incomparable = ReadAll(FixturePath("incomparable.bin"));
    const auto missing = ReadAll(FixturePath("missing.bin"));
    const auto optional = ReadAll(FixturePath("optional_unknown.bin"));
    const auto trailing = ReadAll(FixturePath("trailing_junk.bin"));
    const auto recovered = ReadAll(FixturePath("recovered_tail.bin"));
    const auto nine = ReadAll(FixturePath("nine_commits.bin"));
    const auto optionalCrc = ReadAll(FixturePath("optional_crc_fail.bin"));
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(), legal.data(),
                                          legal.size()),
              VN_MONITOR_COMPARE_COMPARABLE);
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(),
                                          incomparable.data(), incomparable.size()),
              VN_MONITOR_COMPARE_INCOMPARABLE);
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(), missing.data(),
                                          missing.size()),
              VN_MONITOR_COMPARE_MISSING_DATA);
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(), optional.data(),
                                          optional.size()),
              VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE);
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(), trailing.data(),
                                          trailing.size()),
              VN_MONITOR_COMPARE_MISSING_DATA);
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(), recovered.data(),
                                          recovered.size()),
              VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE);
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(), nine.data(),
                                          nine.size()),
              VN_MONITOR_COMPARE_COMPARABLE);
    EXPECT_EQ(vn_monitor_compare_sessions(legal.data(), legal.size(), optionalCrc.data(),
                                          optionalCrc.size()),
              VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE);

    std::ifstream in(FixturePath("compare.expected.json"));
    ASSERT_TRUE(in);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    harkenhost::JsonValue root;
    std::string error;
    ASSERT_TRUE(harkenhost::ParseJson(text, &root, &error)) << error;
    const harkenhost::JsonValue* compare = root.Find("compare");
    ASSERT_NE(compare, nullptr);
    EXPECT_EQ(compare->Find("legal_vs_legal")->text, "VN_MONITOR_COMPARE_COMPARABLE");
    EXPECT_EQ(compare->Find("legal_vs_incomparable")->text,
              "VN_MONITOR_COMPARE_INCOMPARABLE");
    EXPECT_EQ(compare->Find("legal_vs_missing")->text,
              "VN_MONITOR_COMPARE_MISSING_DATA");
    EXPECT_EQ(compare->Find("legal_vs_optional")->text,
              "VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE");
    EXPECT_EQ(compare->Find("legal_vs_trailing")->text,
              "VN_MONITOR_COMPARE_MISSING_DATA");
    EXPECT_EQ(compare->Find("legal_vs_recovered")->text,
              "VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE");
    EXPECT_EQ(compare->Find("legal_vs_nine")->text, "VN_MONITOR_COMPARE_COMPARABLE");
    EXPECT_EQ(compare->Find("legal_vs_optional_crc")->text,
              "VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE");
}
