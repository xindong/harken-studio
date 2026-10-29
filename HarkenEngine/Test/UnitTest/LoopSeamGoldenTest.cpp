// Seamless-loop goldens on the 1:1 unresampled path, where the interpolation
// kernel returns each source sample exactly at integer read positions, so a
// correctly wrapped loop is bit-identical to the concatenation of its passes
// (seam tolerance 0). Two region sources are covered as
// the acceptance list requires: a whole-file loop on an individually
// registered sound, and a sub-file region loop on a loopable pack entry
// (built through the real packc tool, like the round-trip golden). A third
// case proves the pack's loop metadata is never served for an id the caller
// has re-registered with content of their own (the cookie gate).
//
// The wav writer and packc runner are test-local on purpose (round-trip
// golden precedent): tests never include host code, and float32 wav keeps
// the whole chain lossless so exact equality is the honest expectation.
#include "Common/ObjectIdTestSupport.h"
#include "Common/ResourceCompilerTestUtil.h"
#include "Common/TestUtil.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
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
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
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

int CompilePack(const std::string& manifestPath) {
    return harkentest::CompileResources({manifestPath});
}

// Minimal IEEE float32 (format tag 3) mono/stereo RIFF writer (round-trip
// golden precedent): samples written verbatim, no quantization.
void WriteWavF32(const std::string& path,
                 const std::vector<float>& interleaved,
                 std::uint32_t channels, std::uint32_t sampleRate) {
    ASSERT_NE(0u, channels);
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

TEST(LoopSeamGolden, WholeFileLoopIsExactConcatenationIndividual) {
    // An individually registered sound loops over the whole file: three
    // passes are exactly three concatenated copies, with the seam bit-exact.
    EngineSession session(MakeNullConfig(0, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(80, 1200.0, kRate, 0.7f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 3;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 2, out);
    const std::size_t total = sine.size() * 3;
    ASSERT_GE(out.left.size(), total);
    for (std::size_t i = 0; i < total; ++i) {
        ASSERT_EQ(sine[i % sine.size()], out.left[i]) << "sample " << i;
    }
    for (std::size_t i = total; i < out.left.size(); ++i) {
        ASSERT_EQ(0.0f, out.left[i]) << "post-loop sample " << i;
    }
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

// Builds a one-entry pack with a sub-file loop region and returns its path.
// The source is 300 frames; the region is [100, 200).
std::string BuildSubRegionPack(const std::vector<float>& source,
                               VnSoundId id, const std::string& tag) {
    const std::string wavPath = TempPath("loopseam_" + tag + ".wav");
    const std::string packPath = TempPath("loopseam_" + tag + ".pack");
    WriteWavF32(wavPath, source, 1, kRate);
    const std::string manifest =
        "{\"manifestVersion\":3,\"output\":\"" + packPath +
        "\", \"entries\": [ { \"path\": \"" + wavPath + "\", \"id\": " +
        harkentest::JsonObjectId(id) +
        ", \"loopStartFrame\": 100, \"loopEndFrame\": 200 } ] }\n";
    const std::string manifestPath =
        TempPath("loopseam_" + tag + "_manifest.json");
    WriteTextFile(manifestPath, manifest);
    EXPECT_EQ(0, CompilePack(manifestPath));
    return packPath;
}

TEST(LoopSeamGolden, PackSubRegionLoopPlaysIntroRegionOutroExact) {
    // A loopable pack entry: intro [0,100) once, region [100,200) twice,
    // outro [200,300) — i.e. [0,200) ++ [100,300), bit-exact, seam included.
    const std::vector<float> source =
        harkentest::MakeSine(300, 950.0, kRate, 0.6f);
    HARKENTEST_ASSERT_NON_SILENT(source);
    constexpr VnSoundId kId = harkentest::ObjectId(7000);
    const std::string packPath = BuildSubRegionPack(source, kId, "region");

    EngineSession session(MakeNullConfig(0, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnPackDesc packDesc{};
    packDesc.structSize = sizeof(VnPackDesc);
    packDesc.utf8Path = packPath.c_str();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));

    VnPlayDesc play = MakePlayDesc(kId);
    play.loopCount = 2;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 3, out);

    std::vector<float> expected;
    for (std::size_t i = 0; i < 200; ++i) {
        expected.push_back(source[i]); // intro + first region traversal
    }
    for (std::size_t i = 100; i < source.size(); ++i) {
        expected.push_back(source[i]); // second region traversal + outro
    }
    ASSERT_GE(out.left.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(expected[i], out.left[i]) << "sample " << i;
    }
    ASSERT_EQ(source[100], out.left[200]) << "seam sample";
    for (std::size_t i = expected.size(); i < out.left.size(); ++i) {
        ASSERT_EQ(0.0f, out.left[i]) << "post-loop sample " << i;
    }
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(LoopSeamGolden, ReRegisteredIdLoopsWholeFileNotPackRegion) {
    // The pack registers id 7001 with a sub-region [100,200). If the caller
    // unregisters that id and re-registers it with content of their own, the
    // pack's loop metadata must NOT be served (cookie gate): the new
    // registration loops over its WHOLE file. With 2 passes over the full
    // 300-frame content that is 600 output frames, versus the 400 a stale
    // sub-region loop would produce — length alone separates the two.
    const std::vector<float> source =
        harkentest::MakeSine(300, 700.0, kRate, 0.6f);
    HARKENTEST_ASSERT_NON_SILENT(source);
    constexpr VnSoundId kId = harkentest::ObjectId(7001);
    const std::string packPath = BuildSubRegionPack(source, kId, "cookie");

    EngineSession session(MakeNullConfig(0, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnPackDesc packDesc{};
    packDesc.structSize = sizeof(VnPackDesc);
    packDesc.utf8Path = packPath.c_str();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));

    // Take the id away from the pack and re-register it individually with the
    // caller's own content (same buffer here — the point is the provenance
    // change, not the bytes).
    ASSERT_EQ(VN_OK, Vn_UnregisterPCM(kId));
    const VnPCMDesc mine = MonoDesc(source, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kId, &mine));

    VnPlayDesc play = MakePlayDesc(kId);
    play.loopCount = 2;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 4, out);

    const std::size_t wholeFileTotal = source.size() * 2; // 600
    ASSERT_GE(out.left.size(), wholeFileTotal);
    for (std::size_t i = 0; i < wholeFileTotal; ++i) {
        ASSERT_EQ(source[i % source.size()], out.left[i])
            << "whole-file loop sample " << i;
    }
    for (std::size_t i = wholeFileTotal; i < out.left.size(); ++i) {
        ASSERT_EQ(0.0f, out.left[i]) << "post-loop sample " << i;
    }
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

// --- resampled-path seam (pitchRatio != 1) ---
//
// The 1:1 goldens above cannot expose a broken seam feed: the kernel at
// frac == 0 returns the exact current sample, so padding zeros / resetting
// the resampler / corrupting kernel history at the wrap would still pass at
// unity, yet click at a non-unity ratio. These cases render the looped voice
// at a non-unity pitch and compare it against a reference voice playing the
// EXPLICITLY CONCATENATED source (intro + region * N + outro as one buffer)
// through the same pipeline at the same ratio. The wrap only reconstructs
// that concatenated input stream frame by frame — same frames at the same
// ring indices, same ratio, same input-end index — so the two renders are
// bit-identical (tolerance 0). Any difference is a real seam-feed defect,
// not tolerance noise.

// Renders one sound (registered fresh in its own session) at a pitch ratio
// and loop count, returning the deinterleaved capture. `content` is copied
// into the registry during the call, so a local buffer is fine.
Capture RenderResampled(VnSoundId id, const std::vector<float>& content,
                        std::uint32_t loopCount, float ratio,
                        std::uint32_t blocks) {
    EngineSession session(MakeNullConfig(0, kBlock, kRate));
    EXPECT_EQ(VN_OK, session.InitResult());
    const VnPCMDesc desc = MonoDesc(content, kRate);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
    VnPlayDesc play = MakePlayDesc(id, 0.0f, ratio);
    play.loopCount = loopCount;
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    Capture out;
    harkentest::PumpAndCapture(session.Engine(), blocks, out);
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), handle)))
        << "capture window must outlast the voice";
    return out;
}

void ExpectWholeFileResampledSeamlessLoop(float ratio, std::uint32_t blocks) {
    const std::vector<float> source =
        harkentest::MakeSine(160, 1100.0, kRate, 0.7f);
    HARKENTEST_ASSERT_NON_SILENT(source);
    constexpr std::uint32_t kPasses = 3;
    std::vector<float> concatenated;
    for (std::uint32_t p = 0; p < kPasses; ++p) {
        concatenated.insert(concatenated.end(), source.begin(), source.end());
    }
    const Capture looped = RenderResampled(harkentest::ObjectId(1), source, kPasses, ratio, blocks);
    const Capture reference =
        RenderResampled(harkentest::ObjectId(2), concatenated, 1, ratio, blocks);
    HARKENTEST_ASSERT_NON_SILENT(looped.left);
    ASSERT_EQ(reference.left.size(), looped.left.size());
    for (std::size_t i = 0; i < reference.left.size(); ++i) {
        ASSERT_EQ(reference.left[i], looped.left[i]) << "left sample " << i;
        ASSERT_EQ(reference.right[i], looped.right[i]) << "right sample " << i;
    }
}

TEST(LoopSeamGolden, ResampledWholeFileLoopAboveUnityMatchesConcatenation) {
    ExpectWholeFileResampledSeamlessLoop(/*ratio=*/1.5f, /*blocks=*/6);
}

TEST(LoopSeamGolden, ResampledWholeFileLoopBelowUnityMatchesConcatenation) {
    ExpectWholeFileResampledSeamlessLoop(/*ratio=*/0.5f, /*blocks=*/12);
}

TEST(LoopSeamGolden, ResampledPackSubRegionLoopMatchesConcatenation) {
    // The sub-region wrap path (wrap to loopStart, with intro/outro) at a
    // non-unity ratio, against the explicit intro + region * N + outro
    // concatenation.
    const std::vector<float> source =
        harkentest::MakeSine(300, 850.0, kRate, 0.6f);
    HARKENTEST_ASSERT_NON_SILENT(source);
    constexpr VnSoundId kPackId = harkentest::ObjectId(7100);
    constexpr std::uint32_t kPasses = 2;
    constexpr float kRatio = 1.5f;
    const std::string packPath = BuildSubRegionPack(source, kPackId, "resamp");

    Capture looped;
    {
        EngineSession session(MakeNullConfig(0, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        VnPackDesc packDesc{};
        packDesc.structSize = sizeof(VnPackDesc);
        packDesc.utf8Path = packPath.c_str();
        VnPackHandle pack = 0;
        ASSERT_EQ(VN_OK, Vn_LoadPack(&packDesc, &pack));
        VnPlayDesc play = MakePlayDesc(kPackId, 0.0f, kRatio);
        play.loopCount = kPasses;
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        harkentest::PumpAndCapture(session.Engine(), 4, looped);
        ASSERT_EQ(nullptr, session.Engine()->voices.Resolve(
            harkentest::RenderHandle(session.Engine(), handle)));
        ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
    }

    // Region [100,200): intro [0,100), region twice, outro [200,300).
    std::vector<float> concatenated;
    for (std::size_t i = 0; i < 100; ++i) {
        concatenated.push_back(source[i]);
    }
    for (std::uint32_t p = 0; p < kPasses; ++p) {
        for (std::size_t i = 100; i < 200; ++i) {
            concatenated.push_back(source[i]);
        }
    }
    for (std::size_t i = 200; i < source.size(); ++i) {
        concatenated.push_back(source[i]);
    }
    const Capture reference =
        RenderResampled(harkentest::ObjectId(7101), concatenated, 1, kRatio, 4);

    HARKENTEST_ASSERT_NON_SILENT(looped.left);
    ASSERT_EQ(reference.left.size(), looped.left.size());
    for (std::size_t i = 0; i < reference.left.size(); ++i) {
        ASSERT_EQ(reference.left[i], looped.left[i]) << "left sample " << i;
        ASSERT_EQ(reference.right[i], looped.right[i]) << "right sample " << i;
    }
}

} // namespace
