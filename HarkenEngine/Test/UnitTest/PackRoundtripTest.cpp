// Pack round-trip fidelity, driven through the real compiler tool: the
// test synthesizes sources, writes them as IEEE float32 wav files, runs
// packc as a subprocess to build a .pack, and then renders every
// entry twice on the null backend — once played from the loaded pack,
// once from the same source registered individually — asserting the two
// renders are bit-identical (tolerance zero). Float32 wav keeps the whole
// chain lossless: the file holds the exact synth floats, the decoder
// returns them unchanged, and both renders push identical PCM through an
// identical engine, so exact equality is the honest expectation.
//
// Also covered here because they need the real tool: byte-stable output
// for identical input (the determinism contract that makes packs diffable
// and cacheable) and the tool's refusal of malformed manifests.
//
// The wav writer below is test-local on purpose: tests never include host
// code (baseline-regression precedent), and the host's writer is 16-bit
// PCM, whose quantization would break exactness anyway.
#include "Common/ObjectIdTestSupport.h"
#include "Common/StreamBuilder.h"
#include "Common/TestUtil.h"

#include "harken/media/PackFormat.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef HARKEN_PACKC_PATH
#error "HARKEN_PACKC_PATH must point at the packc executable"
#endif
#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint32_t kRate = 48000;

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

// Runs packc on the given manifest/output pair and returns the process
// exit code. Paths from the build system arrive with forward slashes;
// cmd.exe wants backslashes for the program itself, and the doubled outer
// quotes survive cmd's quote-stripping rule for the /C payload.
int RunPackc(const std::string& manifestPath) {
    std::string exe = HARKEN_PACKC_PATH;
    for (char& c : exe) {
        if (c == '/') {
            c = '\\';
        }
    }
    const std::string command = "\"\"" + exe + "\" \"" + manifestPath + "\"\"";
    return std::system(command.c_str());
}

std::string ManifestPrefix(const std::string& outputPath) {
    return "{\"manifestVersion\":3,\"output\":\"" + outputPath +
           "\",\"options\":{\"cache\":false},\"entries\":[";
}

// Minimal IEEE float32 (format tag 3) RIFF/WAVE writer: 18-byte fmt chunk
// (cbSize 0) plus the fact chunk the float format prescribes. Samples are
// written verbatim — no conversion, no dithering — which is the point.
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

// Fixed-seed xorshift32 noise mapped through an exactly representable
// 24-bit slice (baseline-regression precedent for deterministic
// broadband material).
std::vector<float> MakeSeededNoise(std::size_t frames, float amplitude) {
    std::vector<float> s(frames);
    std::uint32_t rng = 0x2468ace1u;
    for (std::size_t n = 0; n < frames; ++n) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const float uniform =
            static_cast<float>(rng >> 8) * (1.0f / 8388608.0f) - 1.0f;
        s[n] = amplitude * uniform;
    }
    return s;
}

// Plays one registered sound on the master bus and captures its whole
// render, padded past the source length so the resampler tail drains
// inside the capture.
void CaptureSound(harken::Engine* engine, VnSoundId soundId,
                  std::uint64_t sourceFrames, Capture* out) {
    const VnPlayDesc play = MakePlayDesc(soundId);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const std::uint32_t blocks = static_cast<std::uint32_t>(
        sourceFrames / kBlockFrames + 2);
    harkentest::PumpAndCapture(engine, blocks, *out);
    // The voice must have ended inside the capture window; a still-live
    // voice would mean the two capture windows compared different spans.
    ASSERT_EQ(nullptr, engine->voices.Resolve(harkentest::RenderHandle(engine,
        voice)));
}

struct SourceSpec {
    VnSoundId id;
    std::uint32_t channels;
    std::vector<float> interleaved;
    std::string wavName;
};

// The round-trip material: two mono sources with different spectra plus
// one true stereo source (distinct channel content), all at the engine
// rate and none block-aligned, so the comparison spans partial final
// blocks too.
std::vector<SourceSpec> MakeRoundtripSources() {
    std::vector<SourceSpec> sources;
    sources.push_back({harkentest::ObjectId(9001), 1,
                       harkentest::MakeSine(700, 997.0, kRate, 0.6f),
                       "roundtrip_sine.wav"});
    sources.push_back({harkentest::ObjectId(9002), 1, MakeSeededNoise(1000, 0.5f),
                       "roundtrip_noise.wav"});
    SourceSpec stereo;
    stereo.id = harkentest::ObjectId(9003);
    stereo.channels = 2;
    stereo.wavName = "roundtrip_stereo.wav";
    const std::vector<float> left =
        harkentest::MakeSine(900, 443.0, kRate, 0.4f);
    const std::vector<float> right =
        harkentest::MakeSine(900, 331.0, kRate, 0.3f);
    stereo.interleaved.resize(900 * 2);
    for (std::size_t n = 0; n < 900; ++n) {
        stereo.interleaved[n * 2 + 0] = left[n];
        stereo.interleaved[n * 2 + 1] = right[n];
    }
    sources.push_back(std::move(stereo));
    return sources;
}

TEST(PackRoundtrip, PackPlaybackBitIdenticalToIndividual) {
    const std::vector<SourceSpec> sources = MakeRoundtripSources();

    // Wav files and manifest (explicit ids so both sides address the
    // same sounds without reading the TOC back).
    const std::string packPath = TempPath("roundtrip.pack");
    std::string manifest = ManifestPrefix(packPath);
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const SourceSpec& source = sources[i];
        const std::string wavPath = TempPath(source.wavName);
        WriteWavF32(wavPath, source.interleaved, source.channels, kRate);
        manifest += "    { \"path\": \"" + wavPath + "\", \"id\": " +
                    "\"" + harken::ObjectIdString(source.id) + "\"" + " }";
        manifest += (i + 1 < sources.size()) ? ",\n" : "\n";
    }
    manifest += "  ]\n}\n";
    const std::string manifestPath = TempPath("roundtrip_manifest.json");
    WriteTextFile(manifestPath, manifest);
    ASSERT_EQ(0, RunPackc(manifestPath));

    // Side 1: every entry rendered from the loaded pack.
    std::vector<Capture> fromPack(sources.size());
    {
        EngineSession session(MakeNullConfig(0, kBlockFrames, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        VnPackDesc desc{};
        desc.structSize = sizeof(VnPackDesc);
        desc.utf8Path = packPath.c_str();
        VnPackHandle pack = 0;
        ASSERT_EQ(VN_OK, Vn_LoadPack(&desc, &pack));
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const std::uint64_t frames =
                sources[i].interleaved.size() / sources[i].channels;
            CaptureSound(session.Engine(), sources[i].id, frames,
                         &fromPack[i]);
        }
        ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
        EXPECT_EQ(0u, harken::memguard::HitCount());
    }

    // Side 2: the identical script with each source registered
    // individually from the original synth buffers.
    std::vector<Capture> fromIndividual(sources.size());
    {
        EngineSession session(MakeNullConfig(0, kBlockFrames, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        for (const SourceSpec& source : sources) {
            VnPCMDesc desc{};
            desc.structSize = sizeof(VnPCMDesc);
            desc.sampleRate = kRate;
            desc.channels = source.channels;
            desc.layout = VN_PCM_LAYOUT_INTERLEAVED;
            desc.policy = VN_PCM_POLICY_COPY;
            desc.frames = source.interleaved.size() / source.channels;
            desc.samplesInterleaved = source.interleaved.data();
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(source.id, &desc));
        }
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const std::uint64_t frames =
                sources[i].interleaved.size() / sources[i].channels;
            CaptureSound(session.Engine(), sources[i].id, frames,
                         &fromIndividual[i]);
        }
        EXPECT_EQ(0u, harken::memguard::HitCount());
    }

    // Bit-identity, tolerance zero, both sides proven non-silent.
    for (std::size_t i = 0; i < sources.size(); ++i) {
        HARKENTEST_ASSERT_NON_SILENT(fromPack[i].left);
        HARKENTEST_ASSERT_NON_SILENT(fromIndividual[i].left);
        ASSERT_EQ(fromIndividual[i].left.size(), fromPack[i].left.size());
        for (std::size_t n = 0; n < fromPack[i].left.size(); ++n) {
            ASSERT_EQ(fromIndividual[i].left[n], fromPack[i].left[n])
                << "entry " << i << " left sample " << n;
            ASSERT_EQ(fromIndividual[i].right[n], fromPack[i].right[n])
                << "entry " << i << " right sample " << n;
        }
    }
}

TEST(PackRoundtrip, PackcOutputIsByteStable) {
    const std::vector<float> tone =
        harkentest::MakeSine(600, 523.0, kRate, 0.5f);
    const std::vector<float> noise = MakeSeededNoise(750, 0.4f);
    const std::string tonePath = TempPath("stable_tone.wav");
    const std::string noisePath = TempPath("stable_noise.wav");
    WriteWavF32(tonePath, tone, 1, kRate);
    WriteWavF32(noisePath, noise, 1, kRate);

    const std::string entries =
        "  { \"path\": \"" + tonePath + "\", \"id\": " +
        harkentest::JsonObjectId(harkentest::ObjectId(9001)) + ", \"loopStartFrame\": 100, "
        "\"loopEndFrame\": 500 },\n"
        "  { \"path\": \"" + noisePath + "\", \"id\": " +
        harkentest::JsonObjectId(harkentest::ObjectId(9002)) + " }\n"
        "] }\n";
    const std::string manifestPath = TempPath("stable_manifest.json");

    const std::string outA = TempPath("stable_a.pack");
    const std::string outB = TempPath("stable_b.pack");
    WriteTextFile(manifestPath, ManifestPrefix(outA) + entries);
    ASSERT_EQ(0, RunPackc(manifestPath));
    WriteTextFile(manifestPath, ManifestPrefix(outB) + entries);
    ASSERT_EQ(0, RunPackc(manifestPath));

    std::vector<std::uint8_t> bytesA;
    std::vector<std::uint8_t> bytesB;
    ASSERT_TRUE(ReadFileBytes(outA, &bytesA));
    ASSERT_TRUE(ReadFileBytes(outB, &bytesB));
    ASSERT_GT(bytesA.size(), static_cast<std::size_t>(
                                 harken::kPackHeaderBytes));
    EXPECT_EQ(bytesA, bytesB) << "identical input must produce "
                                 "byte-identical packs";
}

TEST(PackRoundtrip, ExplicitIdentityPreservesFormatFacts) {
    const std::vector<float> samples =
        harkentest::MakeSine(400, 587.0, kRate, 0.5f);
    const std::string pathA = TempPath("hashfacts_48k.wav");
    const std::string pathB = TempPath("hashfacts_44k.wav");
    WriteWavF32(pathA, samples, 1, 48000);
    WriteWavF32(pathB, samples, 1, 44100);

    const std::string packPath = TempPath("hashfacts.pack");
    const std::string manifest = ManifestPrefix(packPath) +
        "  { \"path\": \"" + pathA + "\", \"id\": " +
        harkentest::JsonObjectId(harkentest::ObjectId(9001)) + " },\n"
        "  { \"path\": \"" + pathB + "\", \"id\": " +
        harkentest::JsonObjectId(harkentest::ObjectId(9002)) + " }\n"
        "] }\n";
    const std::string manifestPath = TempPath("hashfacts_manifest.json");
    WriteTextFile(manifestPath, manifest);
    ASSERT_EQ(0, RunPackc(manifestPath));

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &bytes));
    ASSERT_GE(bytes.size(),
              static_cast<std::size_t>(harken::kPackHeaderBytes +
                                       2 * harken::kPackTocEntryBytes));
    harken::PackHeader header;
    ASSERT_TRUE(harken::ParsePackHeader(bytes.data(), &header));
    ASSERT_EQ(2u, header.entryCount);
    const harken::PackTocEntry first =
        harken::ParsePackTocEntry(bytes.data() + header.tocOffset);
    const harken::PackTocEntry second = harken::ParsePackTocEntry(
        bytes.data() + header.tocOffset + harken::kPackTocEntryBytes);
    EXPECT_EQ(48000u, first.sampleRate);
    EXPECT_EQ(44100u, second.sampleRate);
    EXPECT_EQ(harkentest::ObjectId(9001), first.soundId);
    EXPECT_EQ(harkentest::ObjectId(9002), second.soundId);
}

TEST(PackRoundtrip, FullUuidSeparatesObjectsWithIdenticalMedia) {
    const std::vector<float> pair = {0.5f, -0.25f};
    const std::string monoPath = TempPath("hashlayout_mono.wav");
    const std::string stereoPath = TempPath("hashlayout_stereo.wav");
    WriteWavF32(monoPath, pair, 1, kRate);   // 2 frames, 1 channel
    WriteWavF32(stereoPath, pair, 2, kRate); // 1 frame, 2 channels

    const VnObjectId monoId{0x3da91a64b9474560, 9001};
    const VnObjectId stereoId{0x4da91a64b9474560, 9001};
    const std::string packPath = TempPath("hashlayout.pack");
    const std::string manifest = ManifestPrefix(packPath) +
        "  { \"path\": \"" + monoPath + "\", \"id\": " +
        harkentest::JsonObjectId(monoId) + " },\n"
        "  { \"path\": \"" + stereoPath + "\", \"id\": " +
        harkentest::JsonObjectId(stereoId) + " }\n"
        "] }\n";
    const std::string manifestPath =
        TempPath("hashlayout_manifest.json");
    WriteTextFile(manifestPath, manifest);
    ASSERT_EQ(0, RunPackc(manifestPath));

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &bytes));
    harken::PackHeader header;
    ASSERT_TRUE(harken::ParsePackHeader(bytes.data(), &header));
    ASSERT_EQ(2u, header.entryCount);
    const harken::PackTocEntry mono =
        harken::ParsePackTocEntry(bytes.data() + header.tocOffset);
    const harken::PackTocEntry stereo = harken::ParsePackTocEntry(
        bytes.data() + header.tocOffset + harken::kPackTocEntryBytes);
    EXPECT_EQ(1u, mono.channelCount);
    EXPECT_EQ(2u, mono.frameCount);
    EXPECT_EQ(2u, stereo.channelCount);
    EXPECT_EQ(1u, stereo.frameCount);
    ASSERT_EQ(mono.blobBytes, stereo.blobBytes);
    ASSERT_EQ(0, std::memcmp(bytes.data() + mono.blobOffset,
                             bytes.data() + stereo.blobOffset,
                             static_cast<std::size_t>(mono.blobBytes)))
        << "precondition: the two layouts must serialize byte-identical blobs";
    EXPECT_EQ(monoId, mono.soundId);
    EXPECT_EQ(stereoId, stereo.soundId);
}

TEST(PackRoundtrip, PackcRefusesMalformedInput) {
    const std::vector<float> tone =
        harkentest::MakeSine(300, 660.0, kRate, 0.5f);
    const std::string wavPath = TempPath("refuse_tone.wav");
    WriteWavF32(wavPath, tone, 1, kRate);
    const std::string outPath = TempPath("refuse_out.pack");
    const std::string prefix = ManifestPrefix(outPath);
    const std::string id = ", \"id\": " + harkentest::JsonObjectId(harkentest::ObjectId(9001));

    struct Refusal {
        const char* name;
        std::string manifest;
    };
    const Refusal refusals[] = {
        {"broken JSON", prefix},
        {"retired manifest", "{ \"sounds\": [] }"},
        {"empty entries array", prefix + "]}"},
        {"unknown entry key",
         prefix + "{ \"path\": \"" + wavPath + "\"" + id +
             ", \"bogus\": 1 } ] }"},
        {"missing wav file",
         prefix + "{ \"path\": \"" +
             TempPath("refuse_missing.wav") + "\"" + id + " } ] }"},
        {"loop start not below end",
         prefix + "{ \"path\": \"" + wavPath + "\"" + id +
             ", \"loopStartFrame\": 200, \"loopEndFrame\": 100 } ] }"},
        {"loop end past frame count",
         prefix + "{ \"path\": \"" + wavPath + "\"" + id +
             ", \"loopStartFrame\": 0, \"loopEndFrame\": 100000 } ] }"},
        {"duplicate object UUID",
         prefix + "{ \"path\": \"" + wavPath + "\"" + id +
             " }, { \"path\": \"" + wavPath + "\"" + id + " } ] }"},
        {"missing UUID",
         prefix + "{ \"path\": \"" + wavPath + "\" } ] }"},
        {"numeric id",
         prefix + "{ \"path\": \"" + wavPath +
             "\", \"id\": 1.5 } ] }"},
    };
    for (const Refusal& refusal : refusals) {
        const std::string manifestPath =
            TempPath("refuse_manifest.json");
        WriteTextFile(manifestPath, refusal.manifest);
        EXPECT_NE(0, RunPackc(manifestPath)) << refusal.name;
    }

    // A missing manifest and missing arguments refuse too.
    EXPECT_NE(0, RunPackc(TempPath("refuse_no_such_manifest.json")));
}


TEST(PackRoundtrip, LoadRefusedWhenEntryShadowsStream) {
    // Symmetric to the container case: the flat playable-ID space refuses a
    // pack whose entry id names a registered stream (all-or-nothing), and the
    // stream registration is untouched.
    harkentest::StreamSpec spec;
    spec.soundId = harkentest::ObjectId(8100);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 256;
    const std::vector<float> tone =
        harkentest::MakeSine(600, 440.0, kRate, 0.5f);
    const std::string streamPath = TempPath("shadow_stream.stream");
    harkentest::WriteFileBytes(streamPath,
                              harkentest::BuildStreamBytes(spec, tone));

    // A 2-entry pack whose first entry id (8100) collides with the stream.
    const std::vector<float> a = harkentest::MakeSine(400, 500.0, kRate, 0.5f);
    const std::vector<float> b = harkentest::MakeSine(400, 700.0, kRate, 0.5f);
    const std::string wavA = TempPath("shadow_stream_a.wav");
    const std::string wavB = TempPath("shadow_stream_b.wav");
    WriteWavF32(wavA, a, 1, kRate);
    WriteWavF32(wavB, b, 1, kRate);
    const std::string packPath = TempPath("shadow_stream.pack");
    const std::string manifest =
        ManifestPrefix(packPath) + "    { \"path\": \"" + wavA +
        "\", \"id\": \"3da91a64-b947-4560-0000-000000001fa4\" },\n" + "    { \"path\": \"" + wavB +
        "\", \"id\": \"3da91a64-b947-4560-0000-000000001fa5\" }\n" + "  ]\n}\n";
    const std::string manifestPath = TempPath("shadow_stream_manifest.json");
    WriteTextFile(manifestPath, manifest);
    ASSERT_EQ(0, RunPackc(manifestPath));

    EngineSession session(MakeNullConfig(0, kBlockFrames, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(8100), streamPath.c_str()));

    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = packPath.c_str();
    VnPackHandle pack = 0;
    EXPECT_EQ(VN_IN_USE, Vn_LoadPack(&desc, &pack));

    // All-or-nothing: the non-colliding entry (8101) registered nothing.
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(8101));
    VnVoiceHandle voice = 0;
    EXPECT_EQ(VN_UNKNOWN_ID, harkentest::PlayRaw(&play, &voice));
    // The stream is untouched — still registered, so unregister succeeds.
    EXPECT_EQ(VN_OK, Vn_UnregisterStreamFile(harkentest::ObjectId(8100)));

    // With the stream gone, the same pack loads and 8101 becomes playable.
    ASSERT_EQ(VN_OK, Vn_LoadPack(&desc, &pack));
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
}

} // namespace
