// .stream round-trip fidelity through the real compiler tool: the
// test synthesizes sources, writes IEEE float32 wav files, runs packc in
// --stream mode as a subprocess, registers the container as a stream,
// and renders it on the null backend against the same source registered
// as resident PCM — asserting the two renders are bit-identical
// (tolerance zero). Float32 wav keeps the chain lossless end to end, and
// a streamed voice whose whole file fits in the default ring is
// prebuffered synchronously at raw voice start, so both renders start on the
// same block and exact equality is the honest expectation.
//
// Also covered here because they need the real tool: byte-stable output
// with a full checksum audit of the produced image, the manifest
// chunkFrames key, and the stream-specific manifest refusals.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestPackWriter.h"
#include "Common/TestUtil.h"
#include "UnitTest/JsonTestUtil.h"

#include "JsonReader.h"
#include "harken/media/StreamFormat.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkenhost::JsonValue;
using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::Member;
using harkentest::MemberU64;
using harkentest::ParseJsonFile;

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

int CompileContainer(const std::string& manifestPath, bool stream) {
    if (stream) {
        return harkentest::BuildTestPack({manifestPath, "--stream"});
    }
    return harkentest::BuildTestPack({manifestPath});
}

std::string ManifestPrefix(const std::string& outputPath) {
    return "{\"manifestVersion\":3,\"output\":\"" + outputPath + "\",";
}

// Minimal IEEE float32 (format tag 3) RIFF/WAVE writer: 18-byte fmt chunk
// (cbSize 0) plus the fact chunk the float format prescribes. Samples are
// written verbatim — no conversion, no dithering — which is the point.
// Test-local on purpose: tests never include host code, and the host's
// writer is 16-bit PCM, whose quantization would break exactness anyway.
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

struct SpatialCaptureSettings {
    float mix;
    float width;
};

// Plays one registered sound on the master bus and captures its whole
// render, padded past the source length so the end-of-data drain and the
// resampler tail land inside the capture.
void CaptureSound(harken::Engine* engine, VnSoundId soundId,
                  std::uint64_t sourceFrames, Capture* out,
                  const SpatialCaptureSettings* spatial = nullptr) {
    VnPlayDesc play = MakePlayDesc(soundId);
    if (spatial != nullptr) {
        play.spatialized = 1;
        play.positionZ = -10.0f;
    }
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    if (spatial != nullptr) {
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, Vn_SetVoiceWidth(voice, spatial->width));
        ASSERT_EQ(VN_OK, Vn_SetVoiceSpatializationMix(voice, spatial->mix));
    }
    const std::uint32_t blocks = static_cast<std::uint32_t>(
        sourceFrames / kBlockFrames + 4);
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
    std::string baseName;
    std::uint32_t chunkFrames; // manifest value; 0 = tool default
};

// The round-trip material: one mono source small enough to cross several
// 256-frame chunks with a ragged tail, and one true stereo source left at
// the default chunk size (a single zero-padded chunk), so both the
// multi-chunk walk and the padded-tail path feed the comparison. Neither
// is block-aligned.
std::vector<SourceSpec> MakeRoundtripSources() {
    std::vector<SourceSpec> sources;
    sources.push_back({harkentest::ObjectId(9101), 1,
                       harkentest::MakeSine(700, 997.0, kRate, 0.6f),
                       "sd_roundtrip_sine", 256});
    SourceSpec stereo;
    stereo.id = harkentest::ObjectId(9102);
    stereo.channels = 2;
    stereo.baseName = "sd_roundtrip_stereo";
    stereo.chunkFrames = 0;
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

// Builds the one-sound manifest and runs packc --stream for it.
void BuildContainer(const SourceSpec& source, std::string* outPath) {
    const std::string wavPath = TempPath(source.baseName + ".wav");
    WriteWavF32(wavPath, source.interleaved, source.channels, kRate);
    *outPath = TempPath(source.baseName + ".stream");
    std::string manifest = ManifestPrefix(*outPath);
    if (source.chunkFrames != 0) {
        manifest +=
            "  \"chunkFrames\": " + std::to_string(source.chunkFrames) +
            ",\n";
    }
    manifest += "  \"entries\": [ { \"path\": \"" + wavPath +
                "\", \"id\": " + "\"" + harken::ObjectIdString(source.id) + "\"" + " } ]\n}\n";
    const std::string manifestPath =
        TempPath(source.baseName + "_manifest.json");
    WriteTextFile(manifestPath, manifest);
    ASSERT_EQ(0, CompileContainer(manifestPath, true));
}

TEST(StreamRoundtrip, StreamPlaybackBitIdenticalToResident) {
    const std::vector<SourceSpec> sources = MakeRoundtripSources();
    std::vector<std::string> containers(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        BuildContainer(sources[i], &containers[i]);
        if (HasFatalFailure()) {
            return;
        }
    }

    // Side 1: every source streamed from its container.
    std::vector<Capture> fromStream(sources.size());
    {
        EngineSession session(MakeNullConfig(0, kBlockFrames, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        for (std::size_t i = 0; i < sources.size(); ++i) {
            ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(sources[i].id,
                                                   containers[i].c_str()));
        }
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const std::uint64_t frames =
                sources[i].interleaved.size() / sources[i].channels;
            CaptureSound(session.Engine(), sources[i].id, frames,
                         &fromStream[i]);
        }
        EXPECT_EQ(0u, harken::memguard::HitCount());
    }

    // Side 2: the identical script with each source registered as
    // resident PCM from the original synth buffers.
    std::vector<Capture> fromResident(sources.size());
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
                         &fromResident[i]);
        }
        EXPECT_EQ(0u, harken::memguard::HitCount());
    }

    // Bit-identity, tolerance zero, both sides proven non-silent.
    for (std::size_t i = 0; i < sources.size(); ++i) {
        HARKENTEST_ASSERT_NON_SILENT(fromStream[i].left);
        HARKENTEST_ASSERT_NON_SILENT(fromResident[i].left);
        ASSERT_EQ(fromResident[i].left.size(), fromStream[i].left.size());
        for (std::size_t n = 0; n < fromStream[i].left.size(); ++n) {
            ASSERT_EQ(fromResident[i].left[n], fromStream[i].left[n])
                << "source " << i << " left sample " << n;
            ASSERT_EQ(fromResident[i].right[n], fromStream[i].right[n])
                << "source " << i << " right sample " << n;
        }
    }
}

TEST(StreamRoundtrip, SpatialMonoAndStereoPreserveChannelsAndMatchResident) {
    auto sources = MakeRoundtripSources();
    for (auto& source : sources) {
        source.baseName += "_spatial";
        if (source.channels == 2) {
            const std::size_t frames = source.interleaved.size() / 2;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                source.interleaved[frame * 2] = frame < frames / 3 ? 0.25f
                    : frame < frames * 2 / 3 ? 0.0f : 0.125f;
                source.interleaved[frame * 2 + 1] = frame < frames / 3 ? 0.0f
                    : frame < frames * 2 / 3 ? -0.125f : -0.0625f;
            }
        }
    }
    std::vector<std::string> containers(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        ASSERT_NO_FATAL_FAILURE(BuildContainer(sources[i], &containers[i]));
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(ReadFileBytes(containers[i], &bytes));
        ASSERT_GE(bytes.size(), harken::kStreamHeaderBytes);
        harken::StreamHeader header;
        ASSERT_TRUE(harken::ParseStreamHeader(bytes.data(), &header));
        ASSERT_LE(header.tocOffset, bytes.size());
        ASSERT_LE(harken::kStreamTocEntryBytes, bytes.size() - header.tocOffset);
        const auto toc = harken::ParseStreamTocEntry(bytes.data() + header.tocOffset);
        EXPECT_EQ(sources[i].id, toc.soundId);
        EXPECT_EQ(sources[i].channels, toc.channelCount);
        EXPECT_EQ(kRate, toc.sampleRate);
        EXPECT_EQ(sources[i].interleaved.size() / sources[i].channels, toc.frameCount);
    }

    const std::array<SpatialCaptureSettings, 3> spatialCases{{
        {0.0f, 0.6f}, {0.5f, 0.6f}, {1.0f, 0.6f}}};
    constexpr std::size_t kCases = 4;
    std::array<std::vector<Capture>, kCases> fromStream, fromResident;
    for (std::size_t path = 0; path < 2; ++path) {
        EngineSession session(MakeNullConfig(0, kBlockFrames, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& captures = path == 0 ? fromStream : fromResident;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (path == 0) {
                ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(sources[i].id, containers[i].c_str()));
            } else {
                const auto desc = harkentest::InterleavedDesc(
                    sources[i].interleaved, sources[i].channels, kRate);
                ASSERT_EQ(VN_OK, Vn_RegisterPCM(sources[i].id, &desc));
            }
        }
        for (std::size_t mode = 0; mode < kCases; ++mode) {
            captures[mode].resize(sources.size());
            for (std::size_t i = 0; i < sources.size(); ++i) {
                SCOPED_TRACE(::testing::Message() << "path=" << path
                    << " mode=" << mode << " channels=" << sources[i].channels);
                const auto frames = sources[i].interleaved.size() / sources[i].channels;
                ASSERT_NO_FATAL_FAILURE(CaptureSound(session.Engine(), sources[i].id,
                    frames, &captures[mode][i],
                    mode == 0 ? nullptr : &spatialCases[mode - 1]));
                HARKENTEST_ASSERT_NON_SILENT(captures[mode][i].left);
                HARKENTEST_ASSERT_NON_SILENT(captures[mode][i].right);
                ASSERT_GT(captures[mode][i].left.size(), frames);
            }
        }
        EXPECT_EQ(0u, harken::memguard::HitCount());
    }

    constexpr double kPi = 3.14159265358979323846;
    for (std::size_t mode = 0; mode < kCases; ++mode) {
        const double mix = mode == 0 ? 0.0 : spatialCases[mode - 1].mix;
        const double width = mode == 0 ? 0.0 : spatialCases[mode - 1].width;
        const double leftAngle = (1.0 - width) * kPi / 4.0;
        const double rightAngle = (1.0 + width) * kPi / 4.0;
        const double stereoScale = 1.0 / std::sqrt(1.0 + std::cos(rightAngle - leftAngle));
        for (std::size_t i = 0; i < sources.size(); ++i) {
            SCOPED_TRACE(::testing::Message() << "mode=" << mode
                << " channels=" << sources[i].channels);
            const auto& streamed = fromStream[mode][i];
            const auto& resident = fromResident[mode][i];
            ASSERT_EQ(resident.left.size(), streamed.left.size());
            ASSERT_EQ(resident.right.size(), streamed.right.size());
            ASSERT_EQ(streamed.left.size(), streamed.right.size());
            const auto frames = sources[i].interleaved.size() / sources[i].channels;
            for (std::size_t frame = 0; frame < streamed.left.size(); ++frame) {
                ASSERT_EQ(resident.left[frame], streamed.left[frame]) << frame;
                ASSERT_EQ(resident.right[frame], streamed.right[frame]) << frame;
                double expectedLeft = 0.0;
                double expectedRight = 0.0;
                if (frame < frames) {
                    const double left = sources[i].interleaved[frame * sources[i].channels];
                    if (sources[i].channels == 1) {
                        expectedLeft = expectedRight = left;
                    } else {
                        const double right = sources[i].interleaved[frame * 2 + 1];
                        expectedLeft = (1.0 - mix) * left + mix * stereoScale *
                            (left * std::cos(leftAngle) + right * std::cos(rightAngle));
                        expectedRight = (1.0 - mix) * right + mix * stereoScale *
                            (left * std::sin(leftAngle) + right * std::sin(rightAngle));
                    }
                }
                EXPECT_NEAR(expectedLeft, streamed.left[frame], 2e-6) << frame;
                EXPECT_NEAR(expectedRight, streamed.right[frame], 2e-6) << frame;
                if (mode == 1 && sources[i].channels == 2) {
                    EXPECT_EQ(fromStream[0][i].left[frame], streamed.left[frame]) << frame;
                    EXPECT_EQ(fromStream[0][i].right[frame], streamed.right[frame]) << frame;
                }
            }
        }
    }
}

} // namespace
