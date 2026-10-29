// End-to-end double-build golden for v2 pack logic entries
// (the load half of acceptance 13):
// the same sidechain scene is built twice — once through direct API calls
// on a fresh engine, once by packc-compiling a resident manifest plus a
// content manifest and loading both — and the identical drive sequence
// (same seed, same call order, same pump cadence) must render
// byte-identical output on the null backend. The comparison is
// self-relative; no baseline file exists or is wanted.
//
// Two variants: the state-flip sidechain (an Action's SET_VALUE Step flips a
// discrete Variable that a BusVolume binding maps to an attenuation), and the
// meter-wiring sidechain (a detector bus's meter Variable drives the music
// bus's BusVolume), both entirely deterministic with instant ballistics
// and fixed-seed configs. A third case covers the section-19.5 staged
// replace: music must keep rendering across the activation tick with
// only the expected level change (no dropout block).
#include "Common/ObjectIdTestSupport.h"
#include "Common/ResourceCompilerTestUtil.h"
#include "Common/TestUtil.h"

#include "harken/param/ParamTargets.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;
using harkentest::PumpAndCapture;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kSeed = 0xC0FFEEu;

VnConfig SceneConfig() {
    VnConfig config = MakeNullConfig(/*maxVoices=*/8, kBlock, kRate);
    config.randomSeed = kSeed;
    config.maxActions = 8;
    return config;
}

std::string TempPath(const std::string& name) {
    return std::string(HARKEN_TEMP_DIR "/") + name;
}

void WriteTextFile(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

bool ReadFileBytes(const std::string& path,
                   std::vector<std::uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return true;
}


// Minimal IEEE float32 RIFF/WAVE writer: the pack build must carry the
// exact floats the direct build registers, so the wav travels samples
// verbatim.
void WriteWavF32(const std::string& path,
                 const std::vector<float>& interleaved,
                 std::uint32_t channels, std::uint32_t sampleRate) {
    ASSERT_NE(0u, channels);
    ASSERT_EQ(0u, interleaved.size() % channels);
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;

    const std::uint32_t bytesPerSample = 4;
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(channels * bytesPerSample);
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(interleaved.size() * bytesPerSample);
    const std::uint32_t byteRate = sampleRate * blockAlign;
    const std::uint32_t fmtSize = 18;
    const std::uint32_t factSize = 4;
    const std::uint32_t riffSize =
        4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);
    const std::uint16_t formatIeeeFloat = 3;
    const std::uint16_t bitsPerSample = 32;
    const std::uint16_t cbSize = 0;
    const std::uint16_t channels16 = static_cast<std::uint16_t>(channels);
    const std::uint32_t sampleFrames =
        static_cast<std::uint32_t>(interleaved.size() / channels);

    auto write = [&](const void* data, std::size_t bytes) {
        out.write(static_cast<const char*>(data),
                  static_cast<std::streamsize>(bytes));
    };
    write("RIFF", 4);
    write(&riffSize, 4);
    write("WAVE", 4);
    write("fmt ", 4);
    write(&fmtSize, 4);
    write(&formatIeeeFloat, 2);
    write(&channels16, 2);
    write(&sampleRate, 4);
    write(&byteRate, 4);
    write(&blockAlign, 2);
    write(&bitsPerSample, 2);
    write(&cbSize, 2);
    write("fact", 4);
    write(&factSize, 4);
    write(&sampleFrames, 4);
    write("data", 4);
    write(&dataBytes, 4);
    write(interleaved.data(), interleaved.size() * sizeof(float));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

[[nodiscard]] bool BuildPackFile(const std::string& name,
                                 const std::string& manifestJson) {
    const std::string manifestPath = TempPath(name + "_manifest.json");
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    {
        std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return false;
        }
        out.write(manifestJson.data(),
                  static_cast<std::streamsize>(manifestJson.size()));
        if (!out.good()) {
            return false;
        }
    }
    return harkentest::CompileResources({manifestPath}) == 0;
}

VnResult LoadPack(const std::string& path, VnPackHandle* outHandle) {
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = path.c_str();
    return Vn_LoadPack(&desc, outHandle);
}

VnResult ReplacePack(VnPackHandle handle, const std::string& path) {
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = path.c_str();
    return Vn_ReplacePack(handle, &desc);
}

// Both builds must produce the same number of samples and the same bit
// patterns; memcmp is the whole point (exact equality, no tolerance).
void ExpectBitIdentical(const Capture& direct, const Capture& fromPacks) {
    ASSERT_EQ(direct.left.size(), fromPacks.left.size());
    ASSERT_EQ(direct.right.size(), fromPacks.right.size());
    EXPECT_EQ(0, std::memcmp(direct.left.data(), fromPacks.left.data(),
                             direct.left.size() * sizeof(float)))
        << "left channel diverged between the direct and pack builds";
    EXPECT_EQ(0, std::memcmp(direct.right.data(), fromPacks.right.data(),
                             direct.right.size() * sizeof(float)))
        << "right channel diverged between the direct and pack builds";
}

float BusPeak(harken::Engine* engine, VnBusId busId) {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    const std::uint32_t index = engine->buses.FindIndex(busId);
    EXPECT_LT(index, static_cast<std::uint32_t>(VN_SNAPSHOT_MAX_BUSES));
    return snapshot.busPeaks[index];
}

TEST(PackLogicRoundtrip, ReplaceActivationKeepsMusicRenderingNoDropout) {
    constexpr VnBusId kContinuityBus = harkentest::ObjectId(54);
    constexpr VnVariableId kContinuityVar = harkentest::ObjectId(7504);
    constexpr VnSoundId kContinuityTone = harkentest::ObjectId(74);

    const std::string residentV1 = TempPath("plr_cont_res_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_cont_res_v1",
        "{\n\"manifestVersion\": 3,\n\"output\": \"" + residentV1 +
            "\",\n\"packKind\": \"resident\",\n" +
            std::string(R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000036", "parent": "00000000-0000-0000-0000-000000000001" } ],
"bindings": [ { "id": "3da91a64-b947-4560-0000-000000000040", "variable": "3da91a64-b947-4560-0000-000000001d50", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -12.0]],
                            "handles": [[0.0, 0.0, 1.0, -12.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-000000000040", "scope": "bus", "object": "3da91a64-b947-4560-0000-000000000036" } ]
})")));
    // The revision's only diff: the sidechain curve relaxes from -12 to -6.
    const std::string residentV2 = TempPath("plr_cont_res_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_cont_res_v2",
        "{\n\"manifestVersion\": 3,\n\"output\": \"" + residentV2 +
            "\",\n\"packKind\": \"resident\",\n" +
            std::string(R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000036", "parent": "00000000-0000-0000-0000-000000000001" } ],
"bindings": [ { "id": "3da91a64-b947-4560-0000-000000000040", "variable": "3da91a64-b947-4560-0000-000000001d50", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                            "handles": [[0.0, 0.0, 1.0, -6.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-000000000040", "scope": "bus", "object": "3da91a64-b947-4560-0000-000000000036" } ]
})")));

    EngineSession session(SceneConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const harken::VariableDefinitionV1 definition =
        harkentest::MakeVariableDefinition(kContinuityVar,
                                         harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f);
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(
                  engine, &definition, 1).status);
    VnPackHandle resident = 0;
    ASSERT_EQ(VN_OK, LoadPack(residentV1, &resident));

    static std::vector<float> music;
    music = harkentest::MakeDC(kBlock * 64, 0.5f);
    const VnPCMDesc pcm = MonoDesc(music, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kContinuityTone, &pcm));
    VnPlayDesc play = MakePlayDesc(kContinuityTone);
    play.busId = kContinuityBus;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    // Engage the attenuation and settle on the old curve's level.
    ASSERT_EQ(VN_OK, Vn_SetValue(kContinuityVar, 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    const float oldLevel = 0.5f * std::pow(10.0f, -12.0f / 20.0f);
    const float newLevel = 0.5f * std::pow(10.0f, -6.0f / 20.0f);
    ASSERT_NEAR(oldLevel, BusPeak(engine, kContinuityBus), 2e-3f);

    // Block-by-block peaks across the whole staged replace: two gap
    // blocks, the activation tick, four post-activation blocks.
    std::vector<float> blockPeaks;
    ASSERT_EQ(VN_OK, ReplacePack(resident, residentV2));
    for (int block = 0; block < 2; ++block) {
        engine->nullBackend.Pump(1);
        blockPeaks.push_back(BusPeak(engine, kContinuityBus));
        // The gap renders the OLD curve's level exactly.
        EXPECT_NEAR(oldLevel, blockPeaks.back(), 2e-3f) << block;
    }
    ASSERT_EQ(VN_OK, Vn_Update()); // the activation tick
    for (int block = 0; block < 4; ++block) {
        engine->nullBackend.Pump(1);
        blockPeaks.push_back(BusPeak(engine, kContinuityBus));
    }

    // Continuity: every block through the swap stays inside the
    // [old attenuation, new attenuation] envelope — the music never dropped out, and
    // nothing overshot past the expected level change.
    for (std::size_t i = 0; i < blockPeaks.size(); ++i) {
        EXPECT_GT(blockPeaks[i], oldLevel * 0.95f) << "block " << i;
        EXPECT_LT(blockPeaks[i], newLevel * 1.05f) << "block " << i;
    }
    // The revision landed: the tail renders the NEW curve's level.
    EXPECT_NEAR(newLevel, blockPeaks.back(), 2e-3f);
}

TEST(PackLogicRoundtrip, EmptyContentPacksCarryNoRuntimeCargo) {
    const std::string firstPath = TempPath("plr_empty_content_first.pack");
    const std::string secondPath = TempPath("plr_empty_content_second.pack");
    ASSERT_TRUE(BuildPackFile(
        "plr_empty_content_first",
        "{\"manifestVersion\":3,\"output\":\"" + firstPath +
            "\",\"packKind\":\"content\",\"options\":{"
            "\"cache\":false}}"));
    ASSERT_TRUE(BuildPackFile(
        "plr_empty_content_second",
        "{\"manifestVersion\":3,\"output\":\"" + secondPath +
            "\",\"packKind\":\"content\",\"options\":{"
            "\"cache\":false}}"));
    std::vector<std::uint8_t> firstBytes;
    std::vector<std::uint8_t> secondBytes;
    ASSERT_TRUE(ReadFileBytes(firstPath, &firstBytes));
    ASSERT_TRUE(ReadFileBytes(secondPath, &secondBytes));
    ASSERT_EQ(harken::kPackV2HeaderBytes, firstBytes.size());
    ASSERT_EQ(firstBytes, secondBytes);

    VnConfig config = SceneConfig();
    config.maxPacks = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::size_t registryBefore = engine->registry.Count();
    VnSnapshot snapshotBefore{};
    snapshotBefore.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshotBefore));

    VnPackHandle first = 0;
    VnPackHandle second = 0;
    ASSERT_EQ(VN_OK, LoadPack(firstPath, &first));
    ASSERT_EQ(VN_OK, LoadPack(secondPath, &second));
    ASSERT_EQ(2u, engine->packs.LoadedCount());
    EXPECT_EQ(registryBefore, engine->registry.Count());

    VnPackHandle overflow = 0;
    EXPECT_EQ(VN_NO_PACK, LoadPack(firstPath, &overflow));
    EXPECT_EQ(2u, engine->packs.LoadedCount());
    EXPECT_EQ(registryBefore, engine->registry.Count());

    const harken::PackLogicRecord* firstRecord =
        engine->logicPacks.RecordFor(first);
    const harken::PackLogicRecord* secondRecord =
        engine->logicPacks.RecordFor(second);
    ASSERT_NE(nullptr, firstRecord);
    ASSERT_NE(nullptr, secondRecord);
    for (const harken::PackLogicRecord* record : {firstRecord, secondRecord}) {
        EXPECT_EQ(harken::kPackKindContent, record->packKind);
        EXPECT_TRUE(record->buses.empty());
        EXPECT_TRUE(record->busSends.empty());
        EXPECT_TRUE(record->busEffects.empty());
        EXPECT_TRUE(record->dynamicRoutes.empty());
        EXPECT_TRUE(record->attenuations.empty());
        EXPECT_TRUE(record->bindingDefinitions.empty());
        EXPECT_TRUE(record->bindingAttachments.empty());
        EXPECT_TRUE(record->voiceRecipes.empty());
        EXPECT_TRUE(record->providers.empty());
        EXPECT_TRUE(record->stamps.empty());
    }

    VnSnapshot loadedSnapshot{};
    loadedSnapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&loadedSnapshot));
    EXPECT_EQ(snapshotBefore.activeVoiceCount, loadedSnapshot.activeVoiceCount);
    EXPECT_EQ(snapshotBefore.busCount, loadedSnapshot.busCount);

    ASSERT_EQ(VN_OK, Vn_UnloadPack(second));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_UnloadPack(second));
    VnPackHandle replacement = 0;
    ASSERT_EQ(VN_OK, LoadPack(secondPath, &replacement));
    EXPECT_NE(second, replacement);
    EXPECT_EQ(VN_STALE_HANDLE, Vn_UnloadPack(second));
    EXPECT_NE(nullptr, engine->logicPacks.RecordFor(replacement));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(replacement));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(first));
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(registryBefore, engine->registry.Count());
    EXPECT_EQ(nullptr, engine->logicPacks.RecordFor(first));
    EXPECT_EQ(nullptr, engine->logicPacks.RecordFor(second));
    EXPECT_EQ(nullptr, engine->logicPacks.RecordFor(replacement));
    VnSnapshot finalSnapshot{};
    finalSnapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&finalSnapshot));
    EXPECT_EQ(snapshotBefore.activeVoiceCount, finalSnapshot.activeVoiceCount);
    EXPECT_EQ(snapshotBefore.busCount, finalSnapshot.busCount);
}

TEST(PackLogicRoundtrip, ManifestAttenuationBezierMatchesDirectApiGoldens) {
    struct Golden {
        VnCurveHandlePair handle;
        float expectedDb[3];
    };
    constexpr Golden goldens[] = {
        {{0.0f, 0.0f, 100.0f, -96.0f},
         {-2.4987f, -6.0205f, -12.0408f}},
        {{25.0f, 0.0f, 75.0f, -96.0f},
         {-1.7487f, -6.0205f, -14.7810f}},
        {{100.0f, 0.0f, 0.0f, -96.0f},
         {-0.2621f, -6.0204f, -30.5332f}},
    };
    constexpr float distances[] = {25.0f, 50.0f, 75.0f};
    const std::string packPath = TempPath("plr_bezier_resident.pack");
    const std::string manifestPath =
        TempPath("plr_bezier_resident_manifest.json");
    std::string manifest =
        "{\n\"manifestVersion\": 3,\n\"output\": \"" + packPath +
        "\",\n\"packKind\": \"resident\",\n\"options\": {\"cache\": false},\n"
        "\"attenuations\": [";
    for (std::size_t i = 0; i < std::size(goldens); ++i) {
        if (i != 0) {
            manifest += ",";
        }
        const VnCurveHandlePair& h = goldens[i].handle;
        manifest +=
            "{\"id\":" + harkentest::JsonObjectId(harkentest::ObjectId(810 + i)) +
            ",\"rolloff\":\"linear\",\"minDistance\":1.0,"
            "\"maxDistance\":101.0,\"airAbsorptionNearHz\":20000.0,"
            "\"airAbsorptionFarHz\":20000.0,\"volumeCurve\":"
            "{\"points\":[[0.0,0.0],[100.0,-96.0]],\"handles\":[[" +
            std::to_string(h.h1x) + "," + std::to_string(h.h1y) + "," +
            std::to_string(h.h2x) + "," + std::to_string(h.h2y) +
            "]]}}";
    }
    manifest += "]}\n";
    WriteTextFile(manifestPath, manifest);
    ASSERT_EQ(0, harkentest::CompileResources({manifestPath}));

    for (std::size_t group = 0; group < std::size(goldens); ++group) {
        for (std::size_t sample = 0; sample < std::size(distances);
             ++sample) {
            float directDb = 0.0f;
            float packedDb = 0.0f;
            Capture directCapture;
            Capture packedCapture;
            const std::vector<float> dc = harkentest::MakeDC(48000, 0.5f);

            {
                EngineSession session(SceneConfig());
                ASSERT_EQ(VN_OK, session.InitResult());
                harken::Engine* engine = session.Engine();
                const VnPCMDesc pcm = MonoDesc(dc, kRate);
                ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
                VnAttenuationDesc desc{};
                desc.structSize = sizeof(VnAttenuationDesc);
                desc.rolloff = VN_ROLLOFF_LINEAR;
                desc.minDistance = 1.0f;
                desc.maxDistance = 101.0f;
                desc.airAbsorptionNearHz = 20000.0f;
                desc.airAbsorptionFarHz = 20000.0f;
                desc.volumeCurve.enabled = 1;
                desc.volumeCurve.pointCount = 2;
                desc.volumeCurve.points[0] = {0.0f, 0.0f};
                desc.volumeCurve.points[1] = {100.0f, -96.0f};
                desc.volumeCurve.handles[0] = goldens[group].handle;
                ASSERT_EQ(VN_OK,
                          Vn_RegisterAttenuation(harkentest::ObjectId(810 + group), &desc));
                VnPlayDesc play =
                    MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -distances[sample]);
                VnVoiceHandle voice = 0;
                ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
                ASSERT_EQ(VN_OK,
                          Vn_BindVoiceAttenuation(voice, harkentest::ObjectId(810 + group)));
                ASSERT_EQ(VN_OK, Vn_Update());
                directDb = engine->params.ResolvedValue(
                    harkentest::RenderHandle(engine, voice),
                    harken::kTargetVoiceVolume);
                PumpAndCapture(engine, 2, directCapture);
            }

            {
                EngineSession session(SceneConfig());
                ASSERT_EQ(VN_OK, session.InitResult());
                harken::Engine* engine = session.Engine();
                VnPackHandle pack = 0;
                ASSERT_EQ(VN_OK, LoadPack(packPath, &pack));
                const VnPCMDesc pcm = MonoDesc(dc, kRate);
                ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
                VnPlayDesc play =
                    MakePlayDesc3D(harkentest::ObjectId(1), 0.0f, 0.0f, -distances[sample]);
                VnVoiceHandle voice = 0;
                ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
                ASSERT_EQ(VN_OK,
                          Vn_BindVoiceAttenuation(voice, harkentest::ObjectId(810 + group)));
                ASSERT_EQ(VN_OK, Vn_Update());
                packedDb = engine->params.ResolvedValue(
                    harkentest::RenderHandle(engine, voice),
                    harken::kTargetVoiceVolume);
                PumpAndCapture(engine, 2, packedCapture);
            }

            EXPECT_NEAR(goldens[group].expectedDb[sample], directDb, 1e-4f)
                << "direct group " << group << " sample " << sample;
            EXPECT_NEAR(directDb, packedDb, 1e-6f)
                << "pack group " << group << " sample " << sample;
            ExpectBitIdentical(directCapture, packedCapture);
        }
    }
}

} // namespace
