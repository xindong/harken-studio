#include <harken/ObjectId.h>
#include <harken/Harken.h>
#include <harken/core/LogRing.h>
#include <harken/core/CurrentWorldCodec.h>
#include <harken/param/VariableService.h>
#include <harken/media/PackFormatV2.h>
#include <harken/media/StreamFormat.h>
#include <harken_remote/Protocol.h>
#include <harken_remote/WireFormat.h>

#include <gtest/gtest.h>

#include <map>
#include <unordered_set>
#include <filesystem>

#if defined(HARKEN_HOST_PATH)
#include <process.h>
#endif

extern "C" VnObjectId ObjectIdRoundTripC(VnObjectId value);

TEST(ObjectId, PreservesBothHalvesAcrossTheCAbi)
{
    const VnObjectId id{UINT64_C(0xcacc73dc2e344d29), UINT64_C(0x894f3ed9db63e6fa)};
    EXPECT_EQ(ObjectIdRoundTripC(id), id);
    EXPECT_FALSE(harken::IsNull(id));
    EXPECT_TRUE(harken::IsNull({}));
}

TEST(ObjectId, Rfc4122BytesAndCanonicalTextAgree)
{
    const std::array<uint8_t, 16> bytes{
        0xca, 0xcc, 0x73, 0xdc, 0x2e, 0x34, 0x4d, 0x29,
        0x89, 0x4f, 0x3e, 0xd9, 0xdb, 0x63, 0xe6, 0xfa};
    const auto id = harken::ObjectIdFromBytes(bytes);
    EXPECT_EQ(id.high, UINT64_C(0xcacc73dc2e344d29));
    EXPECT_EQ(id.low, UINT64_C(0x894f3ed9db63e6fa));
    EXPECT_EQ(harken::ObjectIdBytes(id), bytes);
    EXPECT_EQ(harken::ObjectIdString(id), "cacc73dc-2e34-4d29-894f-3ed9db63e6fa");
    VnObjectId parsed{};
    ASSERT_TRUE(harken::ParseObjectId("{CACC73DC-2E34-4D29-894F-3ED9DB63E6FA}", &parsed));
    EXPECT_EQ(parsed, id);
    ASSERT_TRUE(harken::ParseObjectId(harken::ObjectIdString(id), &parsed));
    EXPECT_EQ(parsed, id);
}

TEST(ObjectId, InvalidTextDoesNotModifyTheDestination)
{
    const VnObjectId sentinel{42, 91};
    for (const auto text : {"", "91", "00000000000000000000000000000091",
                            "00000000-0000-0000-0000-00000000000g",
                            "00000000-0000-0000-0000-000000000001 ",
                            "0000000--0000-0000-0000-000000000001",
                            "{00000000-0000-0000-0000-000000000001)"}) {
        SCOPED_TRACE(text);
        auto value = sentinel;
        EXPECT_FALSE(harken::ParseObjectId(text, &value));
        EXPECT_EQ(value, sentinel);
    }
    EXPECT_FALSE(harken::ParseObjectId("00000000-0000-0000-0000-000000000001", nullptr));
}

TEST(ObjectId, OrderedAndHashedLookupsDistinguishTheHighHalf)
{
    const VnObjectId first{1, 17};
    const VnObjectId second{2, 17};
    const VnObjectId third{1, 18};
    EXPECT_NE(first, second);
    EXPECT_LT(first, second);
    EXPECT_LT(first, third);
    std::map<VnObjectId, int> ordered{{first, 1}, {second, 2}, {third, 3}};
    EXPECT_EQ(ordered.size(), 3u);
    EXPECT_EQ(ordered.at(second), 2);
    std::unordered_set<VnObjectId> hashed{first, second, third};
    EXPECT_EQ(hashed.size(), 3u);
    EXPECT_TRUE(hashed.contains(second));
    EXPECT_NE(harken::HashObjectId(first), harken::HashObjectId(second));
}

TEST(ObjectId, BothRemoteProtocolsUseCanonicalUuidBytes)
{
    const VnObjectId id{UINT64_C(0xcacc73dc2e344d29), UINT64_C(0x894f3ed9db63e6fa)};
    const auto expected = harken::ObjectIdBytes(id);
    harken::remote::Writer runtimeWriter;
    runtimeWriter.ObjectId(id);
    EXPECT_EQ(runtimeWriter.bytes, std::vector<uint8_t>(expected.begin(), expected.end()));
    harken::remote::Reader runtimeReader(runtimeWriter.bytes);
    VnObjectId decoded{};
    ASSERT_TRUE(runtimeReader.ObjectId(decoded));
    EXPECT_EQ(decoded, id);
    EXPECT_TRUE(runtimeReader.Done());

    std::vector<uint8_t> toolingBytes;
    harkenremote::WireWriter toolingWriter(&toolingBytes);
    toolingWriter.ObjectId(id);
    EXPECT_EQ(toolingBytes, runtimeWriter.bytes);
    harkenremote::WireReader toolingReader(toolingBytes.data(), toolingBytes.size());
    ASSERT_TRUE(toolingReader.ReadObjectId(&decoded));
    EXPECT_EQ(decoded, id);
    EXPECT_TRUE(toolingReader.AtEnd());

    for (size_t size = 0; size < 16; ++size) {
        harken::remote::Reader shortRuntime(expected.data(), size);
        harkenremote::WireReader shortTooling(expected.data(), size);
        EXPECT_FALSE(shortRuntime.ObjectId(decoded));
        EXPECT_FALSE(shortTooling.ReadObjectId(&decoded));
        EXPECT_EQ(decoded, id);
        EXPECT_EQ(shortRuntime.Remaining(), size);
        EXPECT_EQ(shortTooling.Remaining(), size);
    }
}

TEST(ObjectId, RuntimeBusAndPcmRegistriesDistinguishTheHighHalf)
{
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    config.sampleRate = 48000;
    config.blockFrames = 256;
    ASSERT_EQ(Vn_Init(&config), VN_OK);
    struct Terminate { ~Terminate() { Vn_Term(); } } terminate;
    const VnObjectId first{0xcacc73dc2e344d29, 0x894f3ed9db63e6fa};
    const VnObjectId second{0xbacc73dc2e344d29, first.low};
    EXPECT_EQ(Vn_CreateBus(first, VN_BUS_MASTER), VN_OK);
    EXPECT_EQ(Vn_CreateBus(second, VN_BUS_MASTER), VN_OK);
    EXPECT_NE(Vn_CreateBus(first, VN_BUS_MASTER), VN_OK);
    EXPECT_EQ(Vn_SetBusVolume(second, -6, 0), VN_OK);
    std::array<float, 256> samples{};
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(pcm);
    pcm.sampleRate = 48000;
    pcm.channels = 1;
    pcm.frames = samples.size();
    pcm.samplesInterleaved = samples.data();
    EXPECT_EQ(Vn_RegisterPCM(first, &pcm), VN_OK);
    EXPECT_EQ(Vn_RegisterPCM(second, &pcm), VN_OK);
    EXPECT_EQ(Vn_UnregisterPCM(first), VN_OK);
    EXPECT_EQ(Vn_UnregisterPCM(second), VN_OK);
}

TEST(ObjectId, DiagnosticRingPreservesIdentityAndProductionTime)
{
    harken::LogRing ring;
    harken::LogRecord sent{};
    sent.level = VN_LOG_WARN;
    sent.code = harken::kLogCodePackContentConflict;
    sent.objectId = {0xcacc73dc2e344d29, 0x894f3ed9db63e6fa};
    sent.arg0 = 12;
    sent.arg1 = 34;
    sent.timestampNs = 5678;
    ring.Push(sent);
    harken::LogRecord received{};
    ASSERT_TRUE(ring.TryPop(received));
    EXPECT_EQ(received.objectId, sent.objectId);
    EXPECT_EQ(received.timestampNs, sent.timestampNs);
    EXPECT_EQ(received.arg0, sent.arg0);
    EXPECT_EQ(received.arg1, sent.arg1);
}

TEST(ObjectId, PackDirectoryKeepsUuidSeparateFromRelationOrdinal)
{
    harken::PackV2DirectoryEntry entry{};
    entry.id = {0xcacc73dc2e344d29, 0x894f3ed9db63e6fa};
    entry.relationOrdinal = 123;
    entry.offset = 512;
    entry.size = 256;
    entry.payloadChecksum = 0x1122334455667788;
    std::array<uint8_t, harken::kPackV2DirectoryEntryBytes + 2> bytes{};
    bytes.front() = bytes.back() = 0xa5;
    harken::SerializePackV2DirectoryEntry(entry, bytes.data() + 1);
    const auto decoded = harken::ParsePackV2DirectoryEntry(bytes.data() + 1);
    EXPECT_EQ(decoded.id, entry.id);
    EXPECT_EQ(decoded.relationOrdinal, entry.relationOrdinal);
    EXPECT_EQ(decoded.offset, entry.offset);
    EXPECT_EQ(decoded.size, entry.size);
    EXPECT_EQ(decoded.payloadChecksum, entry.payloadChecksum);
    EXPECT_EQ(bytes.front(), 0xa5);
    EXPECT_EQ(bytes.back(), 0xa5);
}

TEST(ObjectId, StreamTocPreservesUuidAndFollowingFields)
{
    harken::StreamTocEntry entry{};
    entry.soundId = {0xcacc73dc2e344d29, 0x894f3ed9db63e6fa};
    entry.sampleRate = 48000;
    entry.channelCount = 2;
    entry.frameCount = 1000;
    entry.payloadOffset = 512;
    entry.payloadBytes = 8000;
    entry.chunkChecksumOffset = 9000;
    entry.chunkCount = 10;
    entry.loopStartFrame = 3;
    entry.loopEndFrame = 999;
    std::array<uint8_t, harken::kStreamTocEntryBytes + 2> bytes{};
    bytes.front() = bytes.back() = 0xa5;
    harken::SerializeStreamTocEntry(entry, bytes.data() + 1);
    const auto decoded = harken::ParseStreamTocEntry(bytes.data() + 1);
    EXPECT_EQ(decoded.soundId, entry.soundId);
    EXPECT_EQ(decoded.sampleRate, entry.sampleRate);
    EXPECT_EQ(decoded.channelCount, entry.channelCount);
    EXPECT_EQ(decoded.frameCount, entry.frameCount);
    EXPECT_EQ(decoded.payloadOffset, entry.payloadOffset);
    EXPECT_EQ(decoded.payloadBytes, entry.payloadBytes);
    EXPECT_EQ(decoded.chunkChecksumOffset, entry.chunkChecksumOffset);
    EXPECT_EQ(decoded.chunkCount, entry.chunkCount);
    EXPECT_EQ(decoded.loopStartFrame, entry.loopStartFrame);
    EXPECT_EQ(decoded.loopEndFrame, entry.loopEndFrame);
    EXPECT_EQ(bytes.front(), 0xa5);
    EXPECT_EQ(bytes.back(), 0xa5);
}

TEST(ObjectId, RemoteCommandsDistinguishObjectIdentityFromInstanceHandles)
{
    harken::remote::Command command;
    command.operation = harken::remote::Operation::StopVoice;
    command.target = {0xcacc73dc2e344d29, 0x894f3ed9db63e6fa};
    command.instanceHandle = 0xf123456789abcdef;
    command.expectedGeneration = 17;
    command.durationMs = 50;
    harken::remote::Writer writer;
    harken::remote::Encode(writer, command);
    harken::remote::Reader reader(writer.bytes);
    harken::remote::Command decoded;
    ASSERT_TRUE(harken::remote::Decode(reader, decoded));
    EXPECT_EQ(decoded.target, command.target);
    EXPECT_EQ(decoded.instanceHandle, command.instanceHandle);
    EXPECT_EQ(decoded.expectedGeneration, command.expectedGeneration);
    EXPECT_EQ(decoded.durationMs, command.durationMs);
}

TEST(ObjectId, WorldBodyRetainsUuidAndRefusesPreviousFormats)
{
    harken::VariableDefinitionV1 variable{};
    variable.variableId = {0xcacc73dc2e344d29, 0x894f3ed9db63e6fa};
    variable.kind = harken::VariableKindV1::Smooth;
    variable.rangeMax = 1.0f;
    harken::ContainerWorldInstallViewV1 world{};
    world.variableCount = 1;
    world.variables = &variable;
    world.identity.capabilityMask = harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(world);
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(world, bytes));
    harken::CurrentWorldBodyImage image;
    ASSERT_TRUE(image.Decode(bytes.data(), bytes.size(), harken::kCurrentWorldShippingBodyVintage,
                            harken::CurrentWorldDecodeMode::StrictArtifact));
    ASSERT_NE(image.ProductionView(), nullptr);
    ASSERT_EQ(image.ProductionView()->variableCount, 1u);
    EXPECT_EQ(image.ProductionView()->variables[0].variableId, variable.variableId);
    for (uint32_t version = 2; version < harken::kCurrentWorldShippingBodyVintage; ++version) {
        for (const auto mode : {harken::CurrentWorldDecodeMode::StrictArtifact,
                               harken::CurrentWorldDecodeMode::ReplayRawAttempt,
                               harken::CurrentWorldDecodeMode::ReplayCurrentAttempt})
            EXPECT_FALSE(image.Decode(bytes.data(), bytes.size(), version, mode));
    }
    EXPECT_EQ(image.ProductionView()->variables[0].variableId, variable.variableId);
    std::vector<uint8_t> encoded;
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(*image.ProductionView(), encoded));
    EXPECT_EQ(encoded, bytes);
}

TEST(ObjectId, GameplayCatalogUsesOneUuidAndPreservesFollowingFields)
{
    const VnObjectId first{0xcacc73dc2e344d29, 0x894f3ed9db63e6fa};
    const VnObjectId second{0xbacc73dc2e344d29, first.low};
    harken::PackGameplayCatalogHeader header{};
    header.projectGuid = harken::ObjectIdBytes({0x76656e7573746573, 1});
    header.initPackStableId = 91;
    header.initContentRevision = 7;
    harken::PackGameplayCatalogActiveDraft active{};
    active.actionStableId = first;
    active.name = "Play";
    active.nameBindingRevision = 3;
    active.callableFingerprint = harken::GameplayCallableFingerprint(first, 3, active.name);
    active.ownerPackStableId = 92;
    active.providerClosureFingerprint = 123;
    harken::PackGameplayCatalogRetiredDraft retired{};
    retired.lastActionStableId = second;
    retired.lastNameBindingRevision = 2;
    retired.reason = harken::kGameplayCatalogRetiredReasonDeleted;
    retired.name = "Previous";
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(harken::EncodeGameplayCatalog(header, {active}, {retired}, &bytes));
    harken::PackGameplayCatalogActiveRow decoded{};
    ASSERT_TRUE(harken::ParsePackGameplayCatalogActiveRow(
        bytes.data() + harken::kGameplayCatalogHeaderBytes, &decoded));
    EXPECT_EQ(decoded.actionStableId, first);
    EXPECT_EQ(decoded.nameBindingRevision, 3u);
    EXPECT_EQ(decoded.ownerPackStableId, 92u);
    EXPECT_EQ(decoded.providerClosureFingerprint, 123u);
    EXPECT_EQ(decoded.callableFingerprint, active.callableFingerprint);
    EXPECT_NE(harken::GameplayCallableFingerprint(second, 3, active.name), decoded.callableFingerprint);
    harken::PackGameplayCatalogRetiredRow old{};
    ASSERT_TRUE(harken::ParsePackGameplayCatalogRetiredRow(bytes.data() +
        harken::kGameplayCatalogHeaderBytes + harken::kGameplayCatalogActiveRowBytes, &old));
    EXPECT_EQ(old.lastActionStableId, second);
    EXPECT_EQ(old.lastNameBindingRevision, 2u);
    EXPECT_EQ(old.reason, retired.reason);
}

#if defined(HARKEN_HOST_PATH)
TEST(ObjectId, RecordedPublicCallsReplayInASeparateProcess)
{
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    const auto path = std::string(HARKEN_TEMP_DIR) + "/uuid_identity_" +
        std::to_string(_getpid()) + ".vtrc";
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    config.sampleRate = 48000;
    config.blockFrames = 256;
    ASSERT_EQ(Vn_Init(&config), VN_OK);
    struct Terminate { ~Terminate() { Vn_Term(); } } terminate;
    ASSERT_EQ(Vn_TraceStart(path.c_str()), VN_OK);
    const VnObjectId first{0xcacc73dc2e344d29, 0x894f3ed9db63e6fa};
    const VnObjectId second{0xbacc73dc2e344d29, first.low};
    ASSERT_EQ(Vn_CreateBus(first, VN_BUS_MASTER), VN_OK);
    ASSERT_EQ(Vn_CreateBus(second, VN_BUS_MASTER), VN_OK);
    EXPECT_EQ(Vn_SetBusVolume(first, -6, 0), VN_OK);
    EXPECT_EQ(Vn_SetBusVolume(second, -12, 0), VN_OK);
    std::array<float, 256> samples{};
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(pcm);
    pcm.sampleRate = 48000;
    pcm.channels = 1;
    pcm.frames = samples.size();
    pcm.samplesInterleaved = samples.data();
    EXPECT_EQ(Vn_RegisterPCM(first, &pcm), VN_OK);
    EXPECT_EQ(Vn_RegisterPCM(second, &pcm), VN_OK);
    EXPECT_EQ(Vn_UnregisterPCM(first), VN_OK);
    EXPECT_EQ(Vn_UnregisterPCM(second), VN_OK);
    EXPECT_EQ(Vn_TraceStop(), VN_OK);
    Vn_Term();
    EXPECT_EQ(_spawnl(_P_WAIT, HARKEN_HOST_PATH, HARKEN_HOST_PATH,
        "trace-replay", path.c_str(), "--null", static_cast<char*>(nullptr)), 0);
}
#endif
