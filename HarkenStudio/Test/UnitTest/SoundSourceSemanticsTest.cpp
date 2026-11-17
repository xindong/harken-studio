#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetImporter.h"
#include "Core/AssetPipeline/CacheArtifactWriter.h"
#include "Core/AssetPipeline/MediaBuildSnapshot.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/Build/PackagingBridge.h"
#include "Core/ConversionResolution.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/ProjectTypes.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "Tooling/Harkenizer/Core/VorbisEncoder.h"
#include "Core/SoundSourceSemantics.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <vorbis/codec.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cmath>
#include <future>
#include <limits>
#include <stdexcept>

using namespace VNX;

namespace {

SoundSourceDef source(uint32_t sourceId)
{
    SoundSourceDef result;
    result.entryId = QUuid::createUuid();
    result.sourceId = sourceId;
    return result;
}

std::optional<std::vector<float>> decodeVorbisReference(
    const VorbisEncoder::EncodedMedia& media, const VorbisEncoder::SeekEntry* seek = nullptr)
{
    struct Decoder
    {
        vorbis_info info{};
        vorbis_comment comment{};
        vorbis_dsp_state state{};
        vorbis_block block{};
        bool stateReady = false;
        bool blockReady = false;

        Decoder()
        {
            vorbis_info_init(&info);
            vorbis_comment_init(&comment);
        }

        ~Decoder()
        {
            if (blockReady) vorbis_block_clear(&block);
            if (stateReady) vorbis_dsp_clear(&state);
            vorbis_comment_clear(&comment);
            vorbis_info_clear(&info);
        }
    } decoder;
    QByteArray comment("\x03vorbis", 7);
    comment.append(QByteArray(8, '\0'));
    comment.append('\x01');
    const QByteArray* headers[]{&media.identificationHeader, &comment, &media.setupHeader};
    for (int index = 0; index < 3; ++index) {
        ogg_packet packet{};
        packet.packet = reinterpret_cast<unsigned char*>(const_cast<char*>(headers[index]->constData()));
        packet.bytes = static_cast<long>(headers[index]->size());
        packet.b_o_s = index == 0;
        packet.packetno = index;
        if (vorbis_synthesis_headerin(&decoder.info, &decoder.comment, &packet) != 0)
            return std::nullopt;
    }
    if (vorbis_synthesis_init(&decoder.state, &decoder.info) != 0) return std::nullopt;
    decoder.stateReady = true;
    if (vorbis_block_init(&decoder.state, &decoder.block) != 0) return std::nullopt;
    decoder.blockReady = true;
    std::vector<float> result;
    uint64_t discard = seek ? seek->discardFrames : 0;
    const uint64_t wanted = seek ? media.frameCount - seek->outputFrame
                                : std::numeric_limits<uint64_t>::max();
    for (size_t index = seek ? seek->packetIndex : 0; index < media.packets.size(); ++index) {
        const auto& encoded = media.packets[index];
        ogg_packet packet{};
        packet.packet = reinterpret_cast<unsigned char*>(const_cast<char*>(encoded.bytes.constData()));
        packet.bytes = static_cast<long>(encoded.bytes.size());
        packet.granulepos = seek ? -1 : encoded.granuleFrame;
        packet.packetno = static_cast<ogg_int64_t>(index + 3);
        packet.e_o_s = !seek && index + 1 == media.packets.size();
        if (vorbis_synthesis(&decoder.block, &packet) != 0
            || vorbis_synthesis_blockin(&decoder.state, &decoder.block) != 0)
            return std::nullopt;
        float** channels = nullptr;
        int available = 0;
        while ((available = vorbis_synthesis_pcmout(&decoder.state, &channels)) > 0) {
            for (int frame = 0; frame < available; ++frame) {
                if (discard != 0) {
                    --discard;
                } else if (result.size() / media.channels < wanted) {
                    for (uint16_t channel = 0; channel < media.channels; ++channel)
                        result.push_back(channels[channel][frame]);
                }
            }
            if (vorbis_synthesis_read(&decoder.state, available) != 0) return std::nullopt;
        }
        if (available < 0) return std::nullopt;
    }
    if (discard != 0) return std::nullopt;
    return result;
}

std::vector<float> channelTones(uint64_t frames, uint16_t channels, uint32_t sampleRate)
{
    std::vector<float> samples(frames * channels);
    for (uint64_t frame = 0; frame < frames; ++frame) {
        for (uint16_t channel = 0; channel < channels; ++channel) {
            const double frequency = channels >= 6 && channel == 3 ? 61.0 : 173.0 + 137.0 * channel;
            samples[frame * channels + channel] = static_cast<float>(
                0.2 * std::sin(6.283185307179586 * frequency * frame / sampleRate));
        }
    }
    return samples;
}

}

TEST(SoundSourceSemanticsTest, FrameLedgerRoundTripsSampleExact)
{
    std::vector<SoundSourceDef> sources{source(1), source(2)};
    sources[0].trimStartFrame = 1;
    sources[0].trimEndFrame = 4294967297ull;
    sources[0].loopRegion = SoundLoopRegion{2147483649ull, 4294967296ull};
    sources[0].fadeIn.durationFrames = 2147483649ull;
    sources[0].fadeOut.durationFrames = 2147483647ull;
    sources[1].trimStartFrame = 4409;
    sources[1].trimEndFrame = 4410;
    sources[1].fadeIn.handles = {{0.1f, 0.7f, 0.8f, 1.0f}};

    const auto reloaded = SoundSourceSemantics::entriesFromJson(
        SoundSourceSemantics::entriesToJson(sources));
    ASSERT_TRUE(reloaded);
    ASSERT_EQ(reloaded->size(), sources.size());
    for (size_t index = 0; index < sources.size(); ++index) {
        EXPECT_EQ((*reloaded)[index].trimStartFrame, sources[index].trimStartFrame);
        ASSERT_TRUE((*reloaded)[index].trimEndFrame);
        EXPECT_EQ(*(*reloaded)[index].trimEndFrame, *sources[index].trimEndFrame);
        EXPECT_EQ((*reloaded)[index].loopRegion, sources[index].loopRegion);
    }
    EXPECT_EQ(*reloaded, sources);
}

TEST(SoundSourceSemanticsTest, FrameLedgerSurvivesTheSerializedDocument)
{
    std::vector<SoundSourceDef> sources{source(1)};
    sources[0].trimStartFrame = 123456789;
    sources[0].trimEndFrame = 987654321;
    sources[0].loopRegion = SoundLoopRegion{234567890, 876543210};
    sources[0].fadeIn.durationFrames = 123456789;
    sources[0].fadeIn.handles = {{0.125f, 0.75f, 0.625f, 1.0f}};
    sources[0].fadeOut.durationFrames = 34567890;
    sources[0].fadeOut.handles = {{0.375f, 0.875f, 0.75f, 0.25f}};

    const QJsonDocument document(QJsonObject{
        {QStringLiteral("sources"), SoundSourceSemantics::entriesToJson(sources)}});
    const QJsonArray parsed = QJsonDocument::fromJson(document.toJson(QJsonDocument::Compact))
                                  .object()
                                  .value(QStringLiteral("sources"))
                                  .toArray();
    const auto reloaded = SoundSourceSemantics::entriesFromJson(parsed);
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(*reloaded, sources);
}

TEST(SoundSourceSemanticsTest, DerivativeCachePreservesMediaAcrossInputChanges)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("source.wav"));
    const QString referenceDir = directory.filePath(QStringLiteral("reference"));
    const QString cachedDir = directory.filePath(QStringLiteral("cached"));
    const QString snapshotDir = directory.filePath(QStringLiteral("snapshot"));
    const auto platform = QUuid::createUuid();
    SoundSourceDerivative::MaterializationCache cache;
    SoundSourceDerivative::MaterializationCache snapshotCache;
    const auto expectEquivalent = [](const auto& reference, const auto& candidate) {
        EXPECT_EQ(candidate.cacheKey, reference.cacheKey);
        EXPECT_EQ(candidate.sampleRate, reference.sampleRate);
        EXPECT_EQ(candidate.channels, reference.channels);
        EXPECT_EQ(candidate.loopStartFrame, reference.loopStartFrame);
        EXPECT_EQ(candidate.loopEndFrame, reference.loopEndFrame);
        EXPECT_EQ(candidate.loopable, reference.loopable);
        EXPECT_EQ(candidate.contentHash, reference.contentHash);
        ASSERT_TRUE(candidate.samples && reference.samples);
        EXPECT_EQ(*candidate.samples, *reference.samples);
    };
    for (const uint16_t channels : {1, 2}) {
        qint64 sourceSize = 0;
        std::vector<QString> previousKeys;
        for (int revision = 0; revision < 2; ++revision) {
            std::vector<float> samples(257 * channels);
            for (size_t index = 0; index < samples.size(); ++index)
                samples[index] = static_cast<float>(static_cast<int>(index % 17) - 8)
                    / (revision == 0 ? 16.0f : -16.0f);
            ASSERT_TRUE(AssetConverter::writeFloatWav(inputPath, samples, channels, 44100));
            QFile input(inputPath);
            ASSERT_TRUE(input.open(QIODevice::ReadWrite));
            ASSERT_TRUE(input.setFileTime(QDateTime::fromSecsSinceEpoch(1500000000),
                                         QFileDevice::FileModificationTime));
            input.close();
            if (revision == 0) sourceSize = QFileInfo(inputPath).size();
            else EXPECT_EQ(QFileInfo(inputPath).size(), sourceSize);
            BuildSourceInputs inputs;
            AssetInfo asset;
            asset.sourceID = 1;
            asset.absolutePath = inputPath;
            QString captureError;
            const auto captured = inputs.capture(asset, &captureError);
            ASSERT_TRUE(captured) << captureError.toStdString();
            size_t requestIndex = 0;
            for (const uint32_t rate : {0u, 22050u, 32000u}) {
                std::vector<float> uneditedSamples, previousFadeSamples;
                QString uneditedKey, previousFadeKey;
                for (int variant = 0; variant < 7; ++variant) {
                    SCOPED_TRACE(::testing::Message() << channels << "/" << revision
                        << "/" << rate << "/" << variant);
                    auto definition = source(1);
                    double offset = 0.0;
                    if (variant == 1) {
                        definition.trimStartFrame = 31;
                        definition.trimEndFrame = 231;
                        definition.loopRegion = SoundLoopRegion{43, 180};
                        offset = 0.00125;
                    } else if (variant == 2) {
                        definition.trimEndFrame = 1000;
                        definition.loopRegion = SoundLoopRegion{0, 1000};
                        offset = 0.0001;
                    } else if (variant == 3) {
                        definition.trimStartFrame = 73;
                        definition.loopRegion = SoundLoopRegion{51, 141};
                        offset = 0.003;
                    } else if (variant == 4 || variant == 5) {
                        definition.fadeIn.durationFrames = 83;
                        definition.fadeOut.durationFrames = 109;
                        if (variant == 5) {
                            definition.fadeIn.handles = {{0.1f, 0.8f, 0.6f, 1.0f}};
                            definition.fadeOut.handles = {{0.3f, 0.9f, 0.8f, 0.1f}};
                        }
                    }
                    QString error;
                    const auto uncached = SoundSourceDerivative::prepare(
                        inputPath, definition, offset, ResolvedConversion{.sampleRate = rate}, &error);
                    ASSERT_TRUE(uncached) << error.toStdString();
                    const auto reference = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, referenceDir, ResolvedConversion{.sampleRate = rate}, &error);
                    ASSERT_TRUE(reference) << error.toStdString();
                    EXPECT_EQ(*reference->samples, uncached->samples);
                    if (variant == 0) {
                        uneditedSamples = *reference->samples;
                        uneditedKey = reference->cacheKey;
                    } else if (variant == 4 || variant == 5) {
                        EXPECT_NE(*reference->samples, variant == 4 ? uneditedSamples : previousFadeSamples);
                        EXPECT_NE(reference->cacheKey, variant == 4 ? uneditedKey : previousFadeKey);
                        previousFadeSamples = *reference->samples;
                        previousFadeKey = reference->cacheKey;
                    } else if (variant == 6) {
                        EXPECT_EQ(*reference->samples, uneditedSamples);
                        EXPECT_EQ(reference->cacheKey, uneditedKey);
                    }
                    const auto fromSnapshot = SoundSourceDerivative::materialize(
                        captured->inputPath, definition, offset, platform, snapshotDir,
                        ResolvedConversion{.sampleRate = rate}, &error, &snapshotCache, captured.get());
                    ASSERT_TRUE(fromSnapshot) << error.toStdString();
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *fromSnapshot));
                    const auto first = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, cachedDir, ResolvedConversion{.sampleRate = rate}, &error, &cache);
                    ASSERT_TRUE(first) << error.toStdString();
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *first));
                    const auto warm = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, cachedDir, ResolvedConversion{.sampleRate = rate}, &error, &cache);
                    ASSERT_TRUE(warm) << error.toStdString();
                    EXPECT_TRUE(warm->cacheHit);
                    EXPECT_EQ(warm->path, first->path);
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *warm));
                    SoundSourceDerivative::MaterializationCache restartedCache;
                    const auto restarted = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, cachedDir, ResolvedConversion{.sampleRate = rate}, &error, &restartedCache);
                    ASSERT_TRUE(restarted) << error.toStdString();
                    EXPECT_TRUE(restarted->cacheHit);
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *restarted));
                    const auto relocated = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, referenceDir, ResolvedConversion{.sampleRate = rate}, &error, &cache);
                    ASSERT_TRUE(relocated) << error.toStdString();
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *relocated));
                    ASSERT_TRUE(QFile::remove(warm->path));
                    const auto retained = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, cachedDir, ResolvedConversion{.sampleRate = rate}, &error, &cache);
                    ASSERT_TRUE(retained) << error.toStdString();
                    EXPECT_TRUE(retained->cacheHit);
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *retained));
                    SoundSourceDerivative::MaterializationCache missingCache;
                    const auto regenerated = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, QFileInfo(warm->path).absolutePath(),
                        ResolvedConversion{.sampleRate = rate}, &error, &missingCache);
                    ASSERT_TRUE(regenerated) << error.toStdString();
                    EXPECT_FALSE(regenerated->cacheHit);
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *regenerated));
                    if (revision == 0) previousKeys.push_back(reference->cacheKey);
                    else EXPECT_NE(reference->cacheKey, previousKeys[requestIndex]);
                    ++requestIndex;
                }
            }
        }
    }
}

TEST(SoundSourceSemanticsTest, MediaEditsPreserveSourceCoordinatesAcrossSlicesAndConversion)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto definition = source(1);
    definition.trimStartFrame = 17;
    definition.trimEndFrame = 241;
    definition.fadeIn.durationFrames = 79;
    definition.fadeOut.durationFrames = 103;
    definition.fadeIn.handles = {{0.1f, 0.7f, 0.8f, 1.0f}};
    definition.fadeOut.handles = {{0.3f, 0.9f, 0.7f, 0.2f}};
    definition.loopRegion = SoundLoopRegion{53, 209};
    auto bakedDefinition = definition;
    bakedDefinition.fadeIn.durationFrames = 0;
    bakedDefinition.fadeOut.durationFrames = 0;
    constexpr uint64_t totalFrames = 257;
    for (const uint16_t channels : {1, 2}) {
        std::vector<float> original(totalFrames * channels);
        for (size_t index = 0; index < original.size(); ++index)
            original[index] = static_cast<float>(index % 31 + 1) / 64.0f;
        auto baked = original;
        SoundSourceSemantics::applyFadeEnvelope(baked, channels, definition, totalFrames);
        EXPECT_NE(baked, original);
        const auto canonicalSource = SoundSourceSemantics::canonicalized(definition, totalFrames);
        for (uint64_t frame = 0; frame < totalFrames; ++frame) {
            const float gain = static_cast<float>(SoundSourceSemantics::fadeGainAtFrame(
                canonicalSource, double(frame)));
            for (uint16_t channel = 0; channel < channels; ++channel)
                EXPECT_FLOAT_EQ(baked[frame * channels + channel], original[frame * channels + channel] * gain);
        }
        for (const auto range : {SoundLoopRegion{0, 17}, SoundLoopRegion{17, 241},
                                 SoundLoopRegion{241, 257}, SoundLoopRegion{42, 88},
                                 *definition.loopRegion, SoundLoopRegion{193, 233}}) {
            SCOPED_TRACE(::testing::Message() << channels << "/" << range.startFrame);
            const auto begin = static_cast<std::ptrdiff_t>(range.startFrame * channels);
            const auto end = static_cast<std::ptrdiff_t>(range.endFrame * channels);
            std::vector<float> sliced(original.begin() + begin, original.begin() + end);
            SoundSourceSemantics::applyFadeEnvelope(
                sliced, channels, definition, totalFrames, range.startFrame);
            EXPECT_EQ(sliced, std::vector<float>(baked.begin() + begin, baked.begin() + end));
        }
        const auto originalPath = directory.filePath(QStringLiteral("original.wav"));
        const auto bakedPath = directory.filePath(QStringLiteral("baked.wav"));
        ASSERT_TRUE(AssetConverter::writeFloatWav(originalPath, original, channels, 44100));
        ASSERT_TRUE(AssetConverter::writeFloatWav(bakedPath, baked, channels, 44100));
        for (const uint32_t rate : {44100u, 22050u, 32000u}) {
            for (const double offset : {0.0, 0.00125}) {
                QString error;
                const auto prepared = SoundSourceDerivative::prepare(
                    originalPath, definition, offset, ResolvedConversion{.sampleRate = rate}, &error);
                ASSERT_TRUE(prepared) << error.toStdString();
                const auto reference = SoundSourceDerivative::prepare(
                    bakedPath, bakedDefinition, offset, ResolvedConversion{.sampleRate = rate}, &error);
                ASSERT_TRUE(reference) << error.toStdString();
                EXPECT_EQ(prepared->samples, reference->samples);
                EXPECT_EQ(prepared->loopStartFrame, reference->loopStartFrame);
                EXPECT_EQ(prepared->loopEndFrame, reference->loopEndFrame);
            }
        }
    }
}

TEST(SoundSourceSemanticsTest, BuildInputsDetectContentChangesWithoutRelyingOnTimestamps)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("source.wav"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(path, std::vector<float>(257, 0.25f), 1, 44100));
    const auto originalTime = QFileInfo(path).lastModified();
    const auto originalSize = QFileInfo(path).size();
    AssetImporter importer;
    AssetInfo asset;
    asset.sourceID = 1;
    asset.absolutePath = path;
    asset.contentHash = AssetImporter::computeContentHash(path);
    asset.metadata = importer.analyzeFile(path);
    MediaBuildSnapshot snapshot;
    snapshot.platforms = ProjectPlatformSemantics::defaultPlatforms();
    const auto platform = snapshot.platforms.front().id;
    snapshot.assets.insert(asset.sourceID, asset);
    AssetInfo missing;
    missing.sourceID = 2;
    missing.absolutePath = directory.filePath(QStringLiteral("missing.wav"));
    snapshot.assets.insert(missing.sourceID, missing);
    snapshot.sourceInputs = std::make_shared<BuildSourceInputs>();
    QString error;
    ProjectData project;
    project.platforms = snapshot.platforms;
    auto bus = std::make_shared<ProjectItem>();
    bus->guid = QUuid::createUuid();
    bus->name = QStringLiteral("Master");
    bus->type = ProjectItem::Type::Bus;
    bus->attachDefaultComponents();
    project.busses.push_back({bus});
    auto sound = std::make_shared<ProjectItem>();
    sound->guid = QUuid::createUuid();
    sound->name = QStringLiteral("Sound");
    sound->type = ProjectItem::Type::Sound;
    sound->attachDefaultComponents();
    sound->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(bus->guid));
    project.layers.push_back({sound});
    ASSERT_TRUE(snapshot.captureConversions(project, &error)) << error.toStdString();
    const auto initial = snapshot.inspectRequest(sound->guid, 1, platform, &error);
    ASSERT_TRUE(initial) << error.toStdString();
    EXPECT_EQ(initial->asset.contentHash, asset.contentHash);
    ASSERT_TRUE(AssetConverter::writeFloatWav(path, std::vector<float>(257, -0.25f), 1, 48000));
    QFile changed(path);
    ASSERT_TRUE(changed.open(QIODevice::ReadWrite));
    ASSERT_TRUE(changed.setFileTime(originalTime, QFileDevice::FileModificationTime));
    changed.close();
    ASSERT_EQ(QFileInfo(path).size(), originalSize);
    ASSERT_EQ(QFileInfo(path).lastModified(), originalTime);
    const auto retained = snapshot.request(sound->guid, 1, platform, &error);
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->asset.contentHash, asset.contentHash);
    snapshot.sourceInputs = std::make_shared<BuildSourceInputs>();
    const auto current = snapshot.request(sound->guid, 1, platform, &error);
    ASSERT_TRUE(current) << error.toStdString();
    EXPECT_NE(current->asset.contentHash, asset.contentHash);
    EXPECT_EQ(current->asset.contentHash, AssetImporter::computeContentHash(path));
    EXPECT_EQ(current->asset.metadata.sampleRate, 48000u);
    EXPECT_EQ(current->asset.metadata.totalSamples, 257u);
    EXPECT_EQ(snapshot.assets.value(1).contentHash, asset.contentHash);
}

TEST(SoundSourceSemanticsTest, BuildInputsKeepCapturedMediaStableUntilNextRequest)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("source.wav"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(path, std::vector<float>(257, 0.25f), 1, 44100));
    AssetInfo asset;
    asset.sourceID = 1;
    asset.absolutePath = path;
    std::vector<AssetInfo> assets(4, asset);
    for (size_t i = 0; i < assets.size(); ++i)
        assets[i].sourceID = static_cast<uint32_t>(i + 1);
    ASSERT_TRUE(QDir(directory.path()).mkdir(QStringLiteral("nested")));
    assets[2].absolutePath = directory.filePath(QStringLiteral("nested/../source.wav"));
    assets[3].absolutePath = directory.filePath(QStringLiteral("other.wav"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(
        assets[3].absolutePath, std::vector<float>(513, -0.5f), 1, 48000));
    const auto originalHash = AssetImporter::computeContentHash(path);
    const auto otherHash = AssetImporter::computeContentHash(assets[3].absolutePath);
    BuildSourceInputs inputs;
    std::barrier<> start(static_cast<std::ptrdiff_t>(assets.size()));
    std::vector<QString> errors(assets.size());
    std::vector<std::future<std::shared_ptr<const BuildSourceInput>>> readers;
    for (size_t i = 0; i < assets.size(); ++i) {
        readers.push_back(std::async(std::launch::async, [&, i] {
            start.arrive_and_wait();
            return inputs.capture(assets[i], &errors[i]);
        }));
    }
    std::vector<std::shared_ptr<const BuildSourceInput>> snapshots;
    for (size_t i = 0; i < readers.size(); ++i) {
        const auto snapshot = readers[i].get();
        ASSERT_TRUE(snapshot) << errors[i].toStdString();
        const auto& expected = i == 3 ? otherHash : originalHash;
        EXPECT_EQ(snapshot->contentHash, expected);
        EXPECT_EQ(AssetImporter::computeContentHash(snapshot->inputPath), expected);
        EXPECT_EQ(snapshot->metadata.sampleRate, i == 3 ? 48000u : 44100u);
        EXPECT_EQ(snapshot->metadata.totalSamples, i == 3 ? 513u : 257u);
        snapshots.push_back(snapshot);
    }
    QString error;
    const auto& captured = snapshots.front();
    EXPECT_EQ(QFileInfo(captured->inputPath).lastModified(), QFileInfo(path).lastModified());
    ASSERT_TRUE(WaveformCache::generate(path));
    ASSERT_TRUE(WaveformCache::generate(captured->inputPath));
    EXPECT_EQ(AssetImporter::computeContentHash(WaveformCache::cachePath(path)),
              AssetImporter::computeContentHash(WaveformCache::cachePath(captured->inputPath)));
    const auto fingerprint = captured->contentHash;
    const auto originalTime = QFileInfo(path).lastModified();
    ASSERT_TRUE(AssetConverter::writeFloatWav(path, std::vector<float>(257, -0.25f), 1, 44100));
    QFile changed(path);
    ASSERT_TRUE(changed.open(QIODevice::ReadWrite));
    ASSERT_TRUE(changed.setFileTime(originalTime, QFileDevice::FileModificationTime));
    changed.close();
    for (size_t i = 0; i < assets.size(); ++i) {
        const auto reused = inputs.capture(assets[i], &error);
        ASSERT_TRUE(reused) << error.toStdString();
        EXPECT_EQ(AssetImporter::computeContentHash(reused->inputPath), snapshots[i]->contentHash);
        EXPECT_EQ(reused->contentHash, snapshots[i]->contentHash);
    }
    EXPECT_NE(AssetImporter::computeContentHash(path), fingerprint);
    BuildSourceInputs nextBuild;
    const auto next = nextBuild.inspect(asset, &error);
    ASSERT_TRUE(next) << error.toStdString();
    EXPECT_NE(next->contentHash, fingerprint);
}

TEST(SoundSourceSemanticsTest, ReusedInputOwnerObservesEditsOnTheNextRequest)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    AssetInfo asset;
    asset.sourceID = 1;
    asset.absolutePath = directory.filePath(QStringLiteral("source.wav"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(asset.absolutePath, std::vector<float>(257, 0.25f), 1, 44100));
    BuildSourceInputs inputs;
    const auto captured = inputs.capture(asset);
    ASSERT_TRUE(captured);
    const auto original = captured->contentHash;
    ASSERT_TRUE(AssetConverter::writeFloatWav(asset.absolutePath, std::vector<float>(257, -0.25f), 1, 44100));
    const auto unchanged = inputs.inspect(asset);
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->contentHash, original);
    inputs.beginRequest();
    const auto changed = inputs.inspect(asset);
    ASSERT_TRUE(changed);
    EXPECT_NE(changed->contentHash, original);
    EXPECT_EQ(AssetImporter::computeContentHash(captured->inputPath), original);
}

TEST(SoundSourceSemanticsTest, CacheArtifactReplacementPreservesPreviousFileOnIncompleteWrite)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("artifact"));
    ASSERT_TRUE(writeCacheArtifact(path, [](QIODevice& file) { return file.write("original") == 8; }));
    EXPECT_FALSE(writeCacheArtifact(path, [](QIODevice& file) {
        file.write("incomplete");
        return false;
    }));
    QFile original(path);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), QByteArray("original"));
    original.close();
    EXPECT_EQ(QDir(directory.path()).entryList(QDir::Files).size(), 1);
    ASSERT_TRUE(writeCacheArtifact(path, [](QIODevice& file) { return file.write("replaced") == 8; }));
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), QByteArray("replaced"));
    original.close();
    QString contentHash;
    ASSERT_TRUE(AssetConverter::writeFloatWavCache(path, std::vector<float>(257, 0.25f),
                                                   1, 44100, &contentHash));
    EXPECT_EQ(contentHash, AssetImporter::computeContentHash(path));
    const auto previousHash = contentHash;
    EXPECT_FALSE(AssetConverter::writeFloatWavCache(path, {}, 1, 44100, &contentHash));
    EXPECT_TRUE(contentHash.isEmpty());
    EXPECT_EQ(AssetImporter::computeContentHash(path), previousHash);
}

TEST(SoundSourceSemanticsTest, CacheArtifactsCanCreateSharedParentDirectoriesConcurrently)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto parent = directory.filePath(QStringLiteral("cache/media/platform"));
    ASSERT_FALSE(QFileInfo::exists(parent));
    std::barrier<> start(8);
    std::vector<std::future<bool>> writers;
    for (int index = 0; index < 8; ++index) {
        const auto path = QDir(parent).filePath(QStringLiteral("%1.pcm").arg(index));
        const auto content = QByteArray::number(index).repeated(257);
        writers.push_back(std::async(std::launch::async, [&, path, content] {
            start.arrive_and_wait();
            return writeCacheArtifact(path, [&](QIODevice& file) {
                return file.write(content) == content.size();
            });
        }));
    }
    for (auto& writer : writers)
        EXPECT_TRUE(writer.get());
    for (int index = 0; index < 8; ++index) {
        QFile artifact(QDir(parent).filePath(QStringLiteral("%1.pcm").arg(index)));
        ASSERT_TRUE(artifact.open(QIODevice::ReadOnly));
        EXPECT_EQ(artifact.readAll(), QByteArray::number(index).repeated(257));
    }
}

TEST(SoundSourceSemanticsTest, CacheArtifactCreationPreservesFilesBlockingItsParentDirectory)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto parent = directory.filePath(QStringLiteral("cache"));
    QFile blocker(parent);
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    ASSERT_EQ(blocker.write("original"), 8);
    blocker.close();
    const auto path = QDir(parent).filePath(QStringLiteral("artifact.pcm"));
    EXPECT_FALSE(writeCacheArtifact(path, [](QIODevice& file) {
        return file.write("replacement") == 11;
    }));
    EXPECT_TRUE(QFileInfo(parent).isFile());
    ASSERT_TRUE(blocker.open(QIODevice::ReadOnly));
    EXPECT_EQ(blocker.readAll(), QByteArray("original"));
    EXPECT_FALSE(QFileInfo::exists(path));
}

TEST(SoundSourceSemanticsTest, DerivativeCacheRecoversCorruptArtifactsAndPreservesContentRevisions)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto inputPath = directory.filePath(QStringLiteral("source.wav"));
    const auto outputDir = directory.filePath(QStringLiteral("derivatives"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(inputPath, std::vector<float>(257, 0.25f), 1, 44100));
    const auto platform = QUuid::createUuid();
    auto definition = source(1);
    SoundSourceDerivative::MaterializationCache cache;
    QString error;
    const auto initial = SoundSourceDerivative::materialize(
        inputPath, definition, 0.0, platform, outputDir, {}, &error, &cache);
    ASSERT_TRUE(initial) << error.toStdString();
    const auto expectedHash = initial->contentHash;
    const auto expectedTime = QFileInfo(initial->path).lastModified();
    QFile corrupted(initial->path);
    ASSERT_TRUE(corrupted.open(QIODevice::ReadWrite));
    ASSERT_TRUE(corrupted.seek(corrupted.size() - 1));
    const auto last = corrupted.read(1);
    ASSERT_EQ(last.size(), 1);
    ASSERT_TRUE(corrupted.seek(corrupted.size() - 1));
    ASSERT_EQ(corrupted.write(QByteArray(1, last.front() ^ 1)), 1);
    ASSERT_TRUE(corrupted.setFileTime(expectedTime, QFileDevice::FileModificationTime));
    corrupted.close();
    const auto retained = SoundSourceDerivative::materialize(
        inputPath, definition, 0.0, platform, outputDir, {}, &error, &cache);
    ASSERT_TRUE(retained) << error.toStdString();
    EXPECT_EQ(*retained->samples, *initial->samples);
    SoundSourceDerivative::MaterializationCache recoveryCache;
    const auto restored = SoundSourceDerivative::materialize(
        inputPath, definition, 0.0, platform, outputDir, {}, &error, &recoveryCache);
    ASSERT_TRUE(restored) << error.toStdString();
    EXPECT_FALSE(restored->cacheHit);
    EXPECT_EQ(restored->contentHash, expectedHash);
    EXPECT_EQ(*restored->samples, *initial->samples);
    ASSERT_TRUE(corrupted.open(QIODevice::ReadWrite));
    ASSERT_TRUE(corrupted.seek(8));
    const auto sampleRateByte = corrupted.read(1);
    ASSERT_EQ(sampleRateByte.size(), 1);
    ASSERT_TRUE(corrupted.seek(8));
    ASSERT_EQ(corrupted.write(QByteArray(1, sampleRateByte.front() ^ 1)), 1);
    corrupted.close();
    SoundSourceDerivative::MaterializationCache restartedCache;
    const auto rebuilt = SoundSourceDerivative::materialize(
        inputPath, definition, 0.0, platform, outputDir, {}, &error, &restartedCache);
    ASSERT_TRUE(rebuilt) << error.toStdString();
    EXPECT_FALSE(rebuilt->cacheHit);
    EXPECT_EQ(rebuilt->contentHash, expectedHash);
    std::optional<uint64_t> previousRevision;
    for (int variant = 0; variant < 3; ++variant) {
        if (variant == 1) definition.loopRegion = SoundLoopRegion{16, 128};
        if (variant == 2) definition.loopRegion = SoundLoopRegion{32, 128};
        const auto media = SoundSourceDerivative::materialize(
            inputPath, definition, 0.0, platform, outputDir, {}, &error, &cache);
        ASSERT_TRUE(media) << error.toStdString();
        HarkenEditor::Bridge::PackMediaDesc desc;
        desc.interleavedSamples = media->samples;
        desc.sampleRate = media->sampleRate;
        desc.channels = media->channels;
        desc.format = HarkenEditor::Bridge::PackMediaFormat::PcmFloat;
        desc.loopable = media->loopable;
        desc.loopStartFrame = media->loopStartFrame;
        desc.loopEndFrame = media->loopEndFrame;
        uint64_t reference = 0;
        ASSERT_TRUE(HarkenEditor::Bridge::computePackMediaContentRevision(desc, &reference));
        const auto referencePath = directory.filePath(QStringLiteral("reference.wav"));
        ASSERT_TRUE(AssetConverter::writeFloatWav(referencePath, *media->samples,
            media->channels, media->sampleRate));
        desc.interleavedSamples.reset();
        desc.sourcePath = referencePath;
        uint64_t fileRevision = 0;
        ASSERT_TRUE(HarkenEditor::Bridge::computePackMediaContentRevision(desc, &fileRevision));
        EXPECT_EQ(fileRevision, reference);
        const auto revision = cache.contentRevision(*media);
        ASSERT_TRUE(revision);
        EXPECT_EQ(*revision, reference);
        EXPECT_EQ(cache.contentRevision(*media), revision);
        if (previousRevision) EXPECT_NE(revision, previousRevision);
        previousRevision = revision;
    }
}

TEST(SoundSourceSemanticsTest, DerivativeCachePreservesInvalidRequestErrors)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString inputPath = directory.filePath(QStringLiteral("source.wav"));
    const QString outputDir = directory.filePath(QStringLiteral("derivatives"));
    const auto platform = QUuid::createUuid();
    const auto definition = source(1);
    ASSERT_TRUE(AssetConverter::writeFloatWav(inputPath, std::vector<float>(257, 0.25f), 1, 44100));
    SoundSourceDerivative::MaterializationCache cache;
    QString error = QStringLiteral("Retained diagnostic");
    const auto first = SoundSourceDerivative::materialize(
        inputPath, definition, 0.0, platform, outputDir, {}, &error, &cache);
    ASSERT_TRUE(first) << error.toStdString();
    EXPECT_EQ(error, QStringLiteral("Retained diagnostic"));
    for (int mode = 0; mode < 6; ++mode) {
        SCOPED_TRACE(mode);
        auto invalidSource = definition;
        auto invalidPlatform = platform;
        QString invalidPath = inputPath;
        double offset = 0.0;
        if (mode == 0) invalidSource.trimStartFrame = 1000;
        else if (mode == 1) invalidSource.trimEndFrame = 0;
        else if (mode == 2) offset = -1.0;
        else if (mode == 3) offset = std::numeric_limits<double>::quiet_NaN();
        else if (mode == 4) invalidPlatform = QUuid{};
        else invalidPath = directory.filePath(QStringLiteral("missing.wav"));
        QString referenceError;
        QString cachedError;
        const auto reference = SoundSourceDerivative::materialize(
            invalidPath, invalidSource, offset, invalidPlatform, outputDir, {}, &referenceError);
        const auto cached = SoundSourceDerivative::materialize(
            invalidPath, invalidSource, offset, invalidPlatform, outputDir, {}, &cachedError, &cache);
        EXPECT_FALSE(reference);
        EXPECT_FALSE(cached);
        EXPECT_EQ(cachedError, referenceError);
        const auto valid = SoundSourceDerivative::materialize(
            inputPath, definition, 0.0, platform, outputDir, {}, &error, &cache);
        ASSERT_TRUE(valid) << error.toStdString();
        EXPECT_EQ(valid->cacheKey, first->cacheKey);
        EXPECT_TRUE(valid->cacheHit);
        EXPECT_EQ(error, QStringLiteral("Retained diagnostic"));
    }
}

TEST(SoundSourceSemanticsTest, CacheIdentityTracksEffectiveMediaParameters)
{
    auto definition = source(1);
    AudioMetadata metadata;
    metadata.sampleRate = 48000;
    metadata.channels = 1;
    metadata.totalSamples = 257;
    const auto key = [&](const std::optional<ResolvedConversion>& conversion) {
        return SoundSourceDerivative::cacheKey(QStringLiteral("source-content"),
            definition, 0.001, conversion, metadata);
    };
    ResolvedConversion pcm;
    pcm.presetGuid = QUuid::createUuid();
    const auto original = key(std::nullopt);
    ASSERT_FALSE(original.isEmpty());
    EXPECT_EQ(key(pcm), original);
    pcm.presetGuid = QUuid::createUuid();
    pcm.vorbisQuality = 0.9f;
    EXPECT_EQ(key(pcm), original);
    pcm.sampleRate = 24000;
    const auto resampled = key(pcm);
    EXPECT_NE(resampled, original);
    pcm.sampleRate = 22050;
    EXPECT_NE(key(pcm), resampled);
    auto vorbis = pcm;
    vorbis.format = ConversionFormat::Vorbis;
    const auto encoded = key(vorbis);
    EXPECT_NE(encoded, key(pcm));
    vorbis.presetGuid = QUuid::createUuid();
    EXPECT_EQ(key(vorbis), encoded);
    vorbis.vorbisQuality = 0.4f;
    EXPECT_NE(key(vorbis), encoded);
    vorbis.sampleRate = 0;
    EXPECT_NE(key(vorbis), key(ResolvedConversion{
        .format = ConversionFormat::Vorbis, .vorbisQuality = 0.4f, .sampleRate = 22050}));
    AssetInfo asset;
    asset.metadata = metadata;
    asset.metadata.fileSize = 100;
    ConversionPresetSnapshot preset;
    preset.presetGuid = QUuid::createUuid();
    ConversionPlatformConfig row;
    row.platformId = QUuid::createUuid();
    row.tiers = {{100, 24000}, {std::nullopt, 16000}};
    preset.platforms.push_back(row);
    QString resolutionError;
    const auto selected = resolveConversion(preset, row.platformId, asset, &resolutionError);
    ASSERT_TRUE(selected) << resolutionError.toStdString();
    EXPECT_EQ(key(selected), resampled);
    preset.presetGuid = QUuid::createUuid();
    preset.platforms.front().tiers = {{std::nullopt, 24000}};
    EXPECT_EQ(key(resolveConversion(preset, row.platformId, asset)), resampled);
    asset.metadata.sampleRate = 16000;
    const auto originalRate = resolveConversion(preset, row.platformId, asset, &resolutionError);
    ASSERT_TRUE(originalRate) << resolutionError.toStdString();
    EXPECT_EQ(originalRate->sampleRate, 0u);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto inputPath = directory.filePath(QStringLiteral("source.wav"));
    const auto outputDir = directory.filePath(QStringLiteral("media"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(inputPath, std::vector<float>(257, 0.25f), 1, 48000));
    SoundSourceDerivative::MaterializationCache cache;
    QString error;
    const auto originalMedia = SoundSourceDerivative::materialize(inputPath, definition, 0.0,
        QUuid::createUuid(), outputDir, std::nullopt, &error, &cache);
    ASSERT_TRUE(originalMedia) << error.toStdString();
    const auto encodedMedia = SoundSourceDerivative::materialize(inputPath, definition, 0.0,
        QUuid::createUuid(), outputDir, vorbis, &error, &cache);
    ASSERT_TRUE(encodedMedia) << error.toStdString();
    EXPECT_EQ(encodedMedia->format, ConversionFormat::Vorbis);
    EXPECT_TRUE(encodedMedia->encodedMedia);
    EXPECT_FALSE(encodedMedia->samples);
    EXPECT_NE(encodedMedia->cacheKey, originalMedia->cacheKey);
    EXPECT_EQ(QDir(outputDir).entryList(QDir::Files).size(), 2);
}

TEST(SoundSourceSemanticsTest, EquivalentMediaTransformsShareOneArtifactAcrossPlatforms)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto inputPath = directory.filePath(QStringLiteral("source.wav"));
    const auto outputDir = directory.filePath(QStringLiteral("media"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(inputPath, std::vector<float>(257, 0.25f), 1, 44100));
    SoundSourceDerivative::MaterializationCache cache;
    const auto definition = source(1);
    QString error;
    const auto first = SoundSourceDerivative::materialize(
        inputPath, definition, 0.0, QUuid::createUuid(), outputDir, {}, &error, &cache);
    ASSERT_TRUE(first) << error.toStdString();
    auto equivalent = source(2);
    equivalent.trimEndFrame = 5000;
    cache.clear();
    const auto shared = SoundSourceDerivative::materialize(
        inputPath, equivalent, 0.0, QUuid::createUuid(), outputDir, {}, &error, &cache);
    ASSERT_TRUE(shared) << error.toStdString();
    EXPECT_TRUE(shared->cacheHit);
    EXPECT_EQ(shared->path, first->path);
    EXPECT_EQ(*shared->samples, *first->samples);
    EXPECT_EQ(cache.contentRevision(*shared), cache.contentRevision(*first));
    const auto normalized = SoundSourceDerivative::materialize(
        inputPath, equivalent, 0.1 / 44100.0, QUuid::createUuid(), outputDir,
        ResolvedConversion{.sampleRate = 44100}, &error, &cache);
    ASSERT_TRUE(normalized) << error.toStdString();
    EXPECT_TRUE(normalized->cacheHit);
    EXPECT_EQ(normalized->path, first->path);
    BuildSourceInputs inputs;
    AssetInfo asset;
    asset.sourceID = 1;
    asset.absolutePath = inputPath;
    const auto inspected = inputs.inspect(asset, &error);
    ASSERT_TRUE(inspected) << error.toStdString();
    bool captured = false;
    const auto lazy = SoundSourceDerivative::materialize(
        inputPath, definition, 0.0, QUuid::createUuid(), outputDir, {}, &error, nullptr,
        inspected.get(), [&](QString*) -> std::shared_ptr<const BuildSourceInput> {
            captured = true;
            return {};
        });
    ASSERT_TRUE(lazy) << error.toStdString();
    EXPECT_FALSE(captured);
    EXPECT_EQ(*lazy->samples, *first->samples);
    SoundSourceDerivative::MaterializationCache memory;
    const auto resident = SoundSourceDerivative::materialize(
        inputPath, definition, 0.0, QUuid::createUuid(), {}, {}, &error, &memory);
    ASSERT_TRUE(resident) << error.toStdString();
    EXPECT_TRUE(resident->path.isEmpty());
    EXPECT_EQ(*resident->samples, *first->samples);
    EXPECT_EQ(memory.contentRevision(*resident), cache.contentRevision(*first));
    const auto converted = SoundSourceDerivative::materialize(
        inputPath, equivalent, 0.0, QUuid::createUuid(), outputDir, ResolvedConversion{.sampleRate = 22050}, &error, &cache);
    ASSERT_TRUE(converted) << error.toStdString();
    EXPECT_FALSE(converted->cacheHit);
    EXPECT_NE(converted->path, first->path);
    EXPECT_EQ(converted->sampleRate, 22050);
    EXPECT_EQ(QDir(outputDir).entryList(QDir::Files).size(), 2);
    QFile truncated(converted->path);
    ASSERT_TRUE(truncated.open(QIODevice::ReadWrite));
    ASSERT_TRUE(truncated.resize(truncated.size() - 1));
    truncated.close();
    cache.clear();
    const auto recovered = SoundSourceDerivative::materialize(
        inputPath, equivalent, 0.0, QUuid::createUuid(), outputDir, ResolvedConversion{.sampleRate = 22050}, &error, &cache);
    ASSERT_TRUE(recovered) << error.toStdString();
    EXPECT_FALSE(recovered->cacheHit);
    EXPECT_EQ(*recovered->samples, *converted->samples);
}

TEST(SoundSourceSemanticsTest, EncodedMediaPreservesFrameCoordinatesAndChannelIdentity)
{
    struct Case { uint16_t channels; uint32_t rate; uint64_t frames; };
    const Case cases[]{{1, 48000, 16397}, {2, 44100, 12289}, {6, 48000, 16397},
        {8, 32000, 8193}, {1, 8000, 4097}, {2, 48000, 1}, {1, 48000, 31}, {6, 48000, 127}};
    for (const auto& item : cases) {
        SCOPED_TRACE(::testing::Message() << item.channels << "/" << item.rate << "/" << item.frames);
        const auto samples = channelTones(item.frames, item.channels, item.rate);
        const uint64_t loopStart = item.frames > 2 ? item.frames / 3 : 0;
        const uint64_t loopEnd = item.frames > 2 ? item.frames - 1 : 0;
        QString error;
        const auto encoded = VorbisEncoder::encode(samples, item.rate, item.channels, 0.6f,
            loopStart, loopEnd, &error);
        ASSERT_TRUE(encoded) << error.toStdString();
        const auto linear = decodeVorbisReference(*encoded);
        ASSERT_TRUE(linear);
        ASSERT_EQ(linear->size(), samples.size());
        EXPECT_EQ(encoded->frameCount, item.frames);
        EXPECT_EQ(encoded->loopStartFrame, loopStart);
        EXPECT_EQ(encoded->loopEndFrame, loopEnd);
        if (item.frames > item.rate / 10) {
            const int surround[]{0, 2, 1, 4, 5, 3};
            const int surround71[]{0, 2, 1, 6, 7, 4, 5, 3};
            for (uint16_t channel = 0; channel < item.channels; ++channel) {
                const int originalChannel = item.channels == 6 ? surround[channel]
                    : item.channels == 8 ? surround71[channel] : channel;
                double energy = 0.0;
                double residual = 0.0;
                for (uint64_t frame = 0; frame < item.frames; ++frame) {
                    const double expected = samples[frame * item.channels + originalChannel];
                    const double difference = (*linear)[frame * item.channels + channel] - expected;
                    energy += expected * expected;
                    residual += difference * difference;
                }
                EXPECT_LT(residual, energy * 0.06) << "channel " << channel;
            }
        }
        ASSERT_FALSE(encoded->seekTable.empty());
        EXPECT_EQ(encoded->seekTable.front().outputFrame, 0u);
        if (loopEnd != 0)
            EXPECT_TRUE(std::any_of(encoded->seekTable.begin(), encoded->seekTable.end(),
                [&](const auto& entry) { return entry.outputFrame == loopStart; }));
        for (const auto& seek : encoded->seekTable) {
            SCOPED_TRACE(seek.outputFrame);
            const auto decoded = decodeVorbisReference(*encoded, &seek);
            ASSERT_TRUE(decoded);
            const size_t start = static_cast<size_t>(seek.outputFrame * item.channels);
            ASSERT_EQ(decoded->size(), linear->size() - start);
            float maximumDifference = 0.0f;
            for (size_t index = 0; index < decoded->size(); ++index)
                maximumDifference = std::max(maximumDifference,
                    std::abs((*decoded)[index] - (*linear)[start + index]));
            EXPECT_LT(maximumDifference, 0.00001f);
        }
    }
}

TEST(SoundSourceSemanticsTest, EncodedDerivativeCacheRoundTripsAndRepairsCorruption)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto inputPath = directory.filePath(QStringLiteral("source.wav"));
    const auto outputDir = directory.filePath(QStringLiteral("media"));
    const auto referenceDir = directory.filePath(QStringLiteral("reference"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(inputPath, channelTones(16397, 2, 48000), 2, 48000));
    auto definition = source(1);
    definition.trimStartFrame = 37;
    definition.trimEndFrame = 15991;
    definition.loopRegion = SoundLoopRegion{4133, 15007};
    definition.fadeIn.durationFrames = 263;
    definition.fadeOut.durationFrames = 509;
    const auto platform = QUuid::createUuid();
    const ResolvedConversion conversion{
        .format = ConversionFormat::Vorbis, .vorbisQuality = 0.6f, .sampleRate = 32000};
    SoundSourceDerivative::MaterializationCache cache;
    QString error;
    const auto initial = SoundSourceDerivative::materialize(inputPath, definition, 0.001,
        platform, outputDir, conversion, &error, &cache);
    ASSERT_TRUE(initial) << error.toStdString();
    ASSERT_TRUE(initial->encodedMedia);
    EXPECT_FALSE(initial->samples);
    const auto prepared = SoundSourceDerivative::prepare(inputPath, definition, 0.001, conversion, &error);
    ASSERT_TRUE(prepared) << error.toStdString();
    EXPECT_EQ(initial->encodedMedia->frameCount, prepared->samples.size() / prepared->channels);
    EXPECT_EQ(initial->loopStartFrame, prepared->loopStartFrame);
    EXPECT_EQ(initial->loopEndFrame, prepared->loopEndFrame);
    const auto linear = decodeVorbisReference(*initial->encodedMedia);
    ASSERT_TRUE(linear);
    ASSERT_EQ(linear->size(), prepared->samples.size());
    QFile artifact(initial->path);
    ASSERT_TRUE(artifact.open(QIODevice::ReadOnly));
    const auto expectedBytes = artifact.readAll();
    artifact.close();
    SoundSourceDerivative::MaterializationCache independentCache;
    const auto repeated = SoundSourceDerivative::materialize(inputPath, definition, 0.001,
        platform, referenceDir, conversion, &error, &independentCache);
    ASSERT_TRUE(repeated) << error.toStdString();
    QFile reference(repeated->path);
    ASSERT_TRUE(reference.open(QIODevice::ReadOnly));
    EXPECT_EQ(reference.readAll(), expectedBytes);
    EXPECT_EQ(repeated->contentHash, initial->contentHash);
    EXPECT_EQ(repeated->revision, initial->revision);
    reference.close();
    cache.clear();
    const auto restarted = SoundSourceDerivative::materialize(inputPath, definition, 0.001,
        platform, outputDir, conversion, &error, &cache);
    ASSERT_TRUE(restarted) << error.toStdString();
    ASSERT_TRUE(restarted->encodedMedia);
    EXPECT_TRUE(restarted->cacheHit);
    EXPECT_EQ(decodeVorbisReference(*restarted->encodedMedia), linear);
    EXPECT_EQ(cache.contentRevision(*restarted), cache.contentRevision(*initial));
    for (int corruption = 0; corruption < 3; ++corruption) {
        SCOPED_TRACE(corruption);
        ASSERT_TRUE(artifact.open(QIODevice::ReadWrite));
        if (corruption < 2) {
            const qint64 position = corruption == 0 ? artifact.size() - 1 : 8;
            ASSERT_TRUE(artifact.seek(position));
            const auto original = artifact.read(1);
            ASSERT_EQ(original.size(), 1);
            ASSERT_TRUE(artifact.seek(position));
            ASSERT_EQ(artifact.write(QByteArray(1, original.front() ^ 1)), 1);
        } else {
            ASSERT_TRUE(artifact.resize(artifact.size() - 1));
        }
        artifact.close();
        cache.clear();
        const auto recovered = SoundSourceDerivative::materialize(inputPath, definition, 0.001,
            platform, outputDir, conversion, &error, &cache);
        ASSERT_TRUE(recovered) << error.toStdString();
        ASSERT_TRUE(recovered->encodedMedia);
        EXPECT_FALSE(recovered->cacheHit);
        EXPECT_EQ(recovered->contentHash, initial->contentHash);
        EXPECT_EQ(recovered->revision, initial->revision);
        EXPECT_EQ(decodeVorbisReference(*recovered->encodedMedia), linear);
        ASSERT_TRUE(artifact.open(QIODevice::ReadOnly));
        EXPECT_EQ(artifact.readAll(), expectedBytes);
        artifact.close();
    }
}

TEST(SoundSourceSemanticsTest, ConcurrentMediaRequestsShareOneMaterialization)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto inputPath = directory.filePath(QStringLiteral("source.wav"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(inputPath, channelTones(16397, 2, 48000), 2, 48000));
    BuildSourceInputs inputs;
    AssetInfo asset;
    asset.sourceID = 1;
    asset.absolutePath = inputPath;
    QString error;
    const auto captured = inputs.capture(asset, &error);
    ASSERT_TRUE(captured) << error.toStdString();
    for (const auto format : {ConversionFormat::PCM, ConversionFormat::Vorbis}) {
        SCOPED_TRACE(static_cast<int>(format));
        SoundSourceDerivative::MaterializationCache cache;
        const auto definition = source(1);
        std::atomic<int> captures = 0;
        std::barrier<> start(8);
        std::vector<std::future<std::optional<SoundSourceDerivative::MaterializedArtifact>>> requests;
        for (int index = 0; index < 8; ++index) {
            requests.push_back(std::async(std::launch::async, [&] {
                start.arrive_and_wait();
                return SoundSourceDerivative::materialize(inputPath, definition, 0.0,
                    QUuid::createUuid(), {}, ResolvedConversion{.format = format}, nullptr,
                    &cache, captured.get(), [&](QString*) {
                        captures.fetch_add(1);
                        return captured;
                    });
            }));
        }
        const auto first = requests.front().get();
        ASSERT_TRUE(first);
        for (size_t index = 1; index < requests.size(); ++index) {
            const auto shared = requests[index].get();
            ASSERT_TRUE(shared);
            EXPECT_EQ(shared->contentHash, first->contentHash);
            EXPECT_EQ(shared->samples, first->samples);
            EXPECT_EQ(shared->encodedMedia, first->encodedMedia);
        }
        EXPECT_EQ(captures.load(), 1);
    }
}

TEST(SoundSourceSemanticsTest, FailedMaterializationsCanBeRetriedWithoutRetainingErrors)
{
    SoundSourceDerivative::MaterializationCache cache;
    const QByteArray request("retryable-media");
    int attempts = 0;
    const auto materialize = [&](QString* error) -> std::optional<SoundSourceDerivative::MaterializedArtifact> {
        if (++attempts == 1) {
            if (error) *error = QStringLiteral("source unavailable");
            return std::nullopt;
        }
        SoundSourceDerivative::MaterializedArtifact artifact;
        artifact.samples = std::make_shared<const std::vector<float>>(17, 0.25f);
        return artifact;
    };
    QString error;
    EXPECT_FALSE(cache.getOrCreate(request, materialize, &error));
    EXPECT_EQ(error, QStringLiteral("source unavailable"));
    error = QStringLiteral("caller diagnostic");
    const auto recovered = cache.getOrCreate(request, materialize, &error);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(error, QStringLiteral("caller diagnostic"));
    const auto shared = cache.getOrCreate(request, materialize, &error);
    ASSERT_TRUE(shared);
    EXPECT_EQ(shared->samples, recovered->samples);
    EXPECT_EQ(attempts, 2);
}

TEST(SoundSourceSemanticsTest, ExceptionalMaterializationsReleaseTasksForRetry)
{
    SoundSourceDerivative::MaterializationCache cache;
    const QByteArray key("exceptional-media");
    int attempts = 0;
    const auto create = [&](QString*) -> std::optional<SoundSourceDerivative::MaterializedArtifact> {
        if (++attempts == 1) throw std::runtime_error("producer stopped");
        SoundSourceDerivative::MaterializedArtifact artifact;
        artifact.samples = std::make_shared<const std::vector<float>>(17, 0.25f);
        return artifact;
    };
    EXPECT_THROW(cache.getOrCreate(key, create), std::runtime_error);
    const auto recovered = cache.getOrCreate(key, create);
    ASSERT_TRUE(recovered);
    const auto shared = cache.getOrCreate(key, create);
    ASSERT_TRUE(shared);
    EXPECT_EQ(shared->samples, recovered->samples);
    EXPECT_EQ(attempts, 2);
}

TEST(SoundSourceSemanticsTest, SharedMediaCanBePublishedToIndependentDestinationsAfterWriteFailure)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto inputPath = directory.filePath(QStringLiteral("source.wav"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(inputPath, channelTones(16397, 2, 48000), 2, 48000));
    const auto definition = source(1);
    const auto platform = QUuid::createUuid();
    for (const auto format : {ConversionFormat::PCM, ConversionFormat::Vorbis}) {
        SCOPED_TRACE(static_cast<int>(format));
        SoundSourceDerivative::MaterializationCache cache;
        const ResolvedConversion conversion{.format = format};
        QString error;
        const auto resident = SoundSourceDerivative::materialize(inputPath, definition, 0.0,
            platform, {}, conversion, &error, &cache);
        ASSERT_TRUE(resident) << error.toStdString();
        EXPECT_TRUE(resident->path.isEmpty());
        QByteArray expectedBytes;
        for (int destination = 0; destination < 3; ++destination) {
            SCOPED_TRACE(destination);
            const auto outputDir = directory.filePath(QStringLiteral("%1/%2")
                .arg(static_cast<int>(format)).arg(destination));
            if (destination == 1) {
                QFile blocker(outputDir);
                ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
                ASSERT_EQ(blocker.write("preserved"), 9);
                blocker.close();
                const auto failed = SoundSourceDerivative::materialize(inputPath, definition, 0.0,
                    platform, outputDir, conversion, &error, &cache);
                EXPECT_FALSE(failed);
                EXPECT_FALSE(error.isEmpty());
                ASSERT_TRUE(blocker.open(QIODevice::ReadOnly));
                EXPECT_EQ(blocker.readAll(), QByteArray("preserved"));
                blocker.close();
                ASSERT_TRUE(blocker.remove());
            }
            const auto published = SoundSourceDerivative::materialize(inputPath, definition, 0.0,
                platform, outputDir, conversion, &error, &cache);
            ASSERT_TRUE(published) << error.toStdString();
            EXPECT_TRUE(published->cacheHit);
            EXPECT_EQ(QFileInfo(published->path).absolutePath(), QFileInfo(outputDir).absoluteFilePath());
            EXPECT_EQ(published->samples, resident->samples);
            EXPECT_EQ(published->encodedMedia, resident->encodedMedia);
            EXPECT_EQ(published->revision, resident->revision);
            EXPECT_EQ(published->contentHash, resident->contentHash);
            QFile output(published->path);
            ASSERT_TRUE(output.open(QIODevice::ReadOnly));
            const auto bytes = output.readAll();
            ASSERT_FALSE(bytes.isEmpty());
            if (destination == 0) expectedBytes = bytes;
            else EXPECT_EQ(bytes, expectedBytes);
        }
    }
}
