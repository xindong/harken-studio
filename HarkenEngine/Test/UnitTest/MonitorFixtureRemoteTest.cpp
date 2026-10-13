#include "monitor_remote_codec.h"

#include "JsonReader.h"

#include "harken_remote/WireFormat.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef HARKEN_MONITOR_REMOTE_FIXTURE_DIR
#define HARKEN_MONITOR_REMOTE_FIXTURE_DIR "."
#endif

namespace {

std::string FixturePath(const char* name) {
    return std::string(HARKEN_MONITOR_REMOTE_FIXTURE_DIR) + "/" + name;
}

std::vector<uint8_t> ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << path;
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

const uint64_t kSchema = 11;
const uint64_t kBuild = 22;
const uint64_t kProgram = 33;
const uint64_t kCapability = 44;

VnMonitorRemoteDecodeResult DecodeHandshake(const std::vector<uint8_t>& bytes,
                                            uint32_t profile,
                                            uint32_t required,
                                            const uint8_t* principal,
                                            uint64_t principalBytes) {
    return vn_monitor_decode_remote_handshake(bytes.data(), bytes.size(), profile, kSchema,
                                              kBuild, kProgram, kCapability, required,
                                              principal, principalBytes);
}

void ExpectFailClosed(const VnMonitorRemoteDecodeResult& decoded, VnResult status) {
    EXPECT_EQ(decoded.status, status);
    EXPECT_EQ(decoded.grantedPermissionMask, 0u);
    EXPECT_EQ(decoded.bytesConsumed, 0u);
    EXPECT_EQ(decoded.messageCounter, 0u);
    EXPECT_EQ(decoded.handshake.structSize, 0u);
    EXPECT_EQ(decoded.principal.profile, 0u);
    EXPECT_EQ(decoded.principal.localAclSid, 0u);
}

} // namespace

TEST(MonitorFixture, RemotePackedSizesAreNotNativeStructs) {
    EXPECT_EQ(VN_MONITOR_REMOTE_HANDSHAKE_PACKED_BYTES, 120u);
    EXPECT_EQ(VN_MONITOR_REMOTE_FRAME_COUNTER_PACKED_BYTES, 16u);
    EXPECT_EQ(sizeof(VnMonitorRemoteHandshake),
              static_cast<size_t>(VN_MONITOR_REMOTE_HANDSHAKE_PACKED_BYTES));
    EXPECT_EQ(sizeof(VnMonitorRemoteFrameCounter),
              static_cast<size_t>(VN_MONITOR_REMOTE_FRAME_COUNTER_PACKED_BYTES));
    EXPECT_EQ(VN_MONITOR_REMOTE_NONCE_BYTES, 32u);
    EXPECT_EQ(VN_MONITOR_REMOTE_SPKI_SHA256_BYTES, 32u);
    EXPECT_EQ(VN_MONITOR_REMOTE_MESSAGE_COUNTER_START, 1u);
}

TEST(MonitorFixture, RemoteDisabledHandshakeHasNoEndpoint) {
    const auto bytes = ReadAll(FixturePath("handshake_disabled.bin"));
    auto decoded = DecodeHandshake(bytes, VN_MONITOR_SECURITY_DISABLED, 0, nullptr, 0);
    ASSERT_EQ(decoded.status, VN_OK);
    EXPECT_EQ(decoded.handshake.securityProfile, VN_MONITOR_SECURITY_DISABLED);
    EXPECT_EQ(decoded.grantedPermissionMask, 0u);
    EXPECT_EQ(decoded.messageCounter, VN_MONITOR_REMOTE_MESSAGE_COUNTER_START);
}

TEST(MonitorFixture, RemoteLocalAclAndLanMtlsRoundTrip) {
    const auto local = ReadAll(FixturePath("handshake_local_acl.bin"));
    const auto sid = ReadAll(FixturePath("principal_local_acl.bin"));
    auto localDecoded = DecodeHandshake(local, VN_MONITOR_SECURITY_LOCAL_ACL,
                                        VN_MONITOR_PERM_OBSERVE_STATE, sid.data(), sid.size());
    ASSERT_EQ(localDecoded.status, VN_OK);
    EXPECT_EQ(localDecoded.handshake.securityProfile, VN_MONITOR_SECURITY_LOCAL_ACL);
    EXPECT_NE(localDecoded.handshake.clientNonce[0], 0u);
    EXPECT_EQ(localDecoded.principal.localAclSid, 0x1111111111111111ull);
    EXPECT_EQ(localDecoded.principal.lanMtlsSpkiSha256[0], 0u);

    const auto mtls = ReadAll(FixturePath("handshake_lan_mtls.bin"));
    const auto spki = ReadAll(FixturePath("principal_lan_mtls.bin"));
    auto mtlsDecoded = DecodeHandshake(mtls, VN_MONITOR_SECURITY_LAN_MTLS,
                                       VN_MONITOR_PERM_OBSERVE_STATE, spki.data(), spki.size());
    ASSERT_EQ(mtlsDecoded.status, VN_OK);
    EXPECT_EQ(mtlsDecoded.handshake.securityProfile, VN_MONITOR_SECURITY_LAN_MTLS);
    EXPECT_EQ(mtlsDecoded.principal.localAclSid, 0u);
    EXPECT_EQ(mtlsDecoded.principal.lanMtlsSpkiSha256[0], 0u);
    EXPECT_EQ(mtlsDecoded.principal.lanMtlsSpkiSha256[1], 1u);
}

TEST(MonitorFixture, RemoteUnknownProfileAndMismatchFailClosed) {
    const auto unknown = ReadAll(FixturePath("handshake_unknown_profile.bin"));
    ExpectFailClosed(DecodeHandshake(unknown, VN_MONITOR_SECURITY_DISABLED, 0, nullptr, 0),
                     VN_MONITOR_INCOMPATIBLE);

    const auto mismatch = ReadAll(FixturePath("handshake_mismatch.bin"));
    ExpectFailClosed(DecodeHandshake(mismatch, VN_MONITOR_SECURITY_LOCAL_ACL, 0, nullptr, 0),
                     VN_MONITOR_INCOMPATIBLE);

    auto trailing = unknown;
    trailing.push_back(0xAA);
    ExpectFailClosed(DecodeHandshake(trailing, VN_MONITOR_SECURITY_DISABLED, 0, nullptr, 0),
                     VN_MONITOR_INCOMPATIBLE);
}

TEST(MonitorFixture, RemoteDisabledExpectedRejectsOtherProfiles) {
    const auto local = ReadAll(FixturePath("handshake_local_acl.bin"));
    const auto sid = ReadAll(FixturePath("principal_local_acl.bin"));
    ExpectFailClosed(DecodeHandshake(local, VN_MONITOR_SECURITY_DISABLED, 0, sid.data(), sid.size()),
                     VN_MONITOR_INCOMPATIBLE);

    const auto mtls = ReadAll(FixturePath("handshake_lan_mtls.bin"));
    const auto spki = ReadAll(FixturePath("principal_lan_mtls.bin"));
    ExpectFailClosed(DecodeHandshake(mtls, VN_MONITOR_SECURITY_DISABLED, 0, spki.data(), spki.size()),
                     VN_MONITOR_INCOMPATIBLE);
}

TEST(MonitorFixture, RemoteUnknownPermissionBitsFailClosed) {
    const auto unknownBits = ReadAll(FixturePath("handshake_unknown_permission.bin"));
    const auto sid = ReadAll(FixturePath("principal_local_acl.bin"));
    ExpectFailClosed(DecodeHandshake(unknownBits, VN_MONITOR_SECURITY_LOCAL_ACL, 0, sid.data(),
                                     sid.size()),
                     VN_INVALID_ARG);
}

TEST(MonitorFixture, RemoteReservedAndStructSizeRejected) {
    const auto reserved = ReadAll(FixturePath("handshake_reserved_nonzero.bin"));
    ExpectFailClosed(DecodeHandshake(reserved, VN_MONITOR_SECURITY_LOCAL_ACL, 0, nullptr, 0),
                     VN_INVALID_ARG);

    const auto badSize = ReadAll(FixturePath("handshake_struct_size_bad.bin"));
    ExpectFailClosed(DecodeHandshake(badSize, VN_MONITOR_SECURITY_LOCAL_ACL, 0, nullptr, 0),
                     VN_MONITOR_INCOMPATIBLE);
}

TEST(MonitorFixture, RemotePermissionGrantAndDenyMatrix) {
    const char* names[] = {
        "observe_state", "observe_events", "observe_measurements", "capture_control",
        "record",        "raw_audio",      "symbols",
    };
    const uint32_t bits[] = {
        VN_MONITOR_PERM_OBSERVE_STATE,        VN_MONITOR_PERM_OBSERVE_EVENTS,
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS, VN_MONITOR_PERM_CAPTURE_CONTROL,
        VN_MONITOR_PERM_RECORD,               VN_MONITOR_PERM_RAW_AUDIO,
        VN_MONITOR_PERM_SYMBOLS,
    };
    const auto sid = ReadAll(FixturePath("principal_local_acl.bin"));
    const auto spki = ReadAll(FixturePath("principal_lan_mtls.bin"));
    for (size_t i = 0; i < 7; ++i) {
        const auto grant = ReadAll(FixturePath((std::string("perm_grant_") + names[i] + ".bin").c_str()));
        auto granted = DecodeHandshake(grant, VN_MONITOR_SECURITY_LOCAL_ACL, bits[i], sid.data(),
                                       sid.size());
        EXPECT_EQ(granted.status, VN_OK) << names[i];
        EXPECT_EQ(granted.grantedPermissionMask, bits[i]) << names[i];

        const auto deny = ReadAll(FixturePath((std::string("perm_deny_") + names[i] + ".bin").c_str()));
        ExpectFailClosed(DecodeHandshake(deny, VN_MONITOR_SECURITY_LAN_MTLS, bits[i], spki.data(),
                                         spki.size()),
                         VN_MONITOR_PERMISSION_DENIED);
    }
}

TEST(MonitorFixture, RemoteCounterStartsAtOneAndOverflowFails) {
    const auto start = ReadAll(FixturePath("counter_start.bin"));
    uint64_t value = 0;
    EXPECT_EQ(vn_monitor_decode_remote_counter(start.data(), start.size(), 0, &value), VN_OK);
    EXPECT_EQ(value, VN_MONITOR_REMOTE_MESSAGE_COUNTER_START);
    EXPECT_EQ(vn_monitor_decode_remote_counter(start.data(), start.size(), 1, &value),
              VN_MONITOR_INCOMPATIBLE);

    const auto overflow = ReadAll(FixturePath("counter_overflow.bin"));
    EXPECT_EQ(vn_monitor_decode_remote_counter(overflow.data(), overflow.size(), UINT64_MAX,
                                               &value),
              VN_MONITOR_INCOMPATIBLE);
    EXPECT_EQ(value, 0u);
}

TEST(MonitorFixture, RemoteAllowlistDoesNotIntersectWireOpcodes) {
    // Regeneration refreshes the wire inventory while preserving Monitor protocol bytes.

    std::ifstream functionsIn(FixturePath("allowlist_monitor_functions.json"));
    ASSERT_TRUE(functionsIn);
    std::string functionsText((std::istreambuf_iterator<char>(functionsIn)),
                              std::istreambuf_iterator<char>());
    harkenhost::JsonValue functionsRoot;
    std::string error;
    ASSERT_TRUE(harkenhost::ParseJson(functionsText, &functionsRoot, &error)) << error;
    const harkenhost::JsonValue* names = functionsRoot.Find("names");
    ASSERT_NE(names, nullptr);
    EXPECT_GE(names->items.size(), 19u);

    std::ifstream opcodesIn(FixturePath("allowlist_wire_opcodes.json"));
    ASSERT_TRUE(opcodesIn);
    std::string opcodesText((std::istreambuf_iterator<char>(opcodesIn)),
                            std::istreambuf_iterator<char>());
    harkenhost::JsonValue opcodesRoot;
    ASSERT_TRUE(harkenhost::ParseJson(opcodesText, &opcodesRoot, &error)) << error;
    const harkenhost::JsonValue* entries = opcodesRoot.Find("entries");
    ASSERT_NE(entries, nullptr);
    EXPECT_EQ(entries->items.size(), harkenremote::kWireOpcodeCount);
    for (const auto& nameItem : names->items) {
        for (const auto& entry : entries->items) {
            const harkenhost::JsonValue* opcodeName = entry.Find("name");
            ASSERT_NE(opcodeName, nullptr);
            EXPECT_NE(nameItem.text, opcodeName->text);
        }
    }
}

TEST(MonitorFixture, RemoteDefaultClosedHasNoEndpoint) {
    EXPECT_EQ(VN_MONITOR_DEFAULT_REMOTE_CLIENTS, 4u);
    EXPECT_EQ(VN_MONITOR_HARD_REMOTE_CLIENTS, 16u);
    std::ifstream in(FixturePath("contract.expected.json"));
    ASSERT_TRUE(in);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    harkenhost::JsonValue root;
    std::string error;
    ASSERT_TRUE(harkenhost::ParseJson(text, &root, &error)) << error;
    const harkenhost::JsonValue* closed = root.Find("defaultClosed");
    ASSERT_NE(closed, nullptr);
    EXPECT_EQ(closed->Find("endpointName")->text, "");
    uint64_t port = 1;
    ASSERT_TRUE(closed->Find("defaultPort")->AsU64(&port));
    EXPECT_EQ(port, 0u);
    EXPECT_EQ(closed->Find("defaultPipe")->text, "");

    VnMonitorCapabilities capabilities{};
    capabilities.structSize = sizeof(capabilities);
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorGetCapabilities(&capabilities));
    VnMonitorSessionDesc sessionDesc{};
    sessionDesc.structSize = sizeof(sessionDesc);
    VnMonitorSessionHandle handle = 1;
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorOpenSession(&sessionDesc, &handle));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorApplyCapturePlan(1, 1, nullptr, nullptr));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorStartRecording(1, 1, nullptr));
}

TEST(MonitorFixture, RemoteCheckedInHandshakeIsPackedNotNativePoison) {
    const auto packed = ReadAll(FixturePath("handshake_local_acl.bin"));
    ASSERT_EQ(packed.size(), VN_MONITOR_REMOTE_HANDSHAKE_PACKED_BYTES);
    VnMonitorRemoteHandshake native{};
    native.structSize = 0xFFFFFFFFu;
    native.securityProfile = 0xFFFFFFFFu;
    std::vector<uint8_t> poisoned(sizeof(native), 0xA5);
    std::memcpy(poisoned.data(), &native, sizeof(native));
    EXPECT_NE(std::memcmp(packed.data(), poisoned.data(), packed.size()), 0);
    const auto sid = ReadAll(FixturePath("principal_local_acl.bin"));
    auto decoded = DecodeHandshake(packed, VN_MONITOR_SECURITY_LOCAL_ACL,
                                   VN_MONITOR_PERM_OBSERVE_STATE, sid.data(), sid.size());
    EXPECT_EQ(decoded.status, VN_OK);
}
