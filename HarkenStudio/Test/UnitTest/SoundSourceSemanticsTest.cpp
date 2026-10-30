#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetImporter.h"
#include "Core/AssetPipeline/CacheArtifactWriter.h"
#include "Core/AssetPipeline/MediaBuildSnapshot.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "Core/SoundSourceSemantics.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <limits>

using namespace VNX;

namespace {

SoundSourceDef source(uint32_t sourceId)
{
    SoundSourceDef result;
    result.entryId = QUuid::createUuid();
    result.sourceId = sourceId;
    return result;
}

}

TEST(SoundSourceSemanticsTest, FrameLedgerRoundTripsSampleExact)
{
    std::vector<SoundSourceDef> sources{source(1), source(2)};
    sources[0].trimStartFrame = 1;
    sources[0].trimEndFrame = 4294967297ull;
    sources[0].loopRegion = SoundLoopRegion{2147483649ull, 4294967296ull};
    sources[1].trimStartFrame = 4409;
    sources[1].trimEndFrame = 4410;

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
    const auto expectEquivalent = [](const auto& reference, const auto& candidate) {
        EXPECT_EQ(candidate.cacheKey, reference.cacheKey);
        EXPECT_EQ(candidate.sampleRate, reference.sampleRate);
        EXPECT_EQ(candidate.channels, reference.channels);
        EXPECT_EQ(candidate.loopStartFrame, reference.loopStartFrame);
        EXPECT_EQ(candidate.loopEndFrame, reference.loopEndFrame);
        EXPECT_EQ(candidate.loopable, reference.loopable);
        EXPECT_EQ(candidate.contentHash, reference.contentHash);
        QFile expected(reference.path);
        QFile actual(candidate.path);
        ASSERT_TRUE(expected.open(QIODevice::ReadOnly));
        ASSERT_TRUE(actual.open(QIODevice::ReadOnly));
        EXPECT_EQ(actual.readAll(), expected.readAll());
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
            for (const uint32_t rate : {0u, 22050u, 48000u}) {
                ConversionSettings settings;
                settings.performResample = rate != 0;
                if (rate != 0) settings.targetSampleRate = rate;
                for (int variant = 0; variant < 4; ++variant) {
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
                    }
                    QString error;
                    const auto reference = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, referenceDir, settings, &error);
                    ASSERT_TRUE(reference) << error.toStdString();
                    const auto fromSnapshot = SoundSourceDerivative::materialize(
                        captured->inputPath, definition, offset, platform, snapshotDir,
                        settings, &error, &cache, captured.get());
                    ASSERT_TRUE(fromSnapshot) << error.toStdString();
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *fromSnapshot));
                    const auto first = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, cachedDir, settings, &error, &cache);
                    ASSERT_TRUE(first) << error.toStdString();
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *first));
                    const auto warm = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, cachedDir, settings, &error, &cache);
                    ASSERT_TRUE(warm) << error.toStdString();
                    EXPECT_TRUE(warm->cacheHit);
                    EXPECT_EQ(warm->path, first->path);
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *warm));
                    SoundSourceDerivative::MaterializationCache restartedCache;
                    const auto restarted = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, cachedDir, settings, &error, &restartedCache);
                    ASSERT_TRUE(restarted) << error.toStdString();
                    EXPECT_TRUE(restarted->cacheHit);
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *restarted));
                    const auto relocated = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, referenceDir, settings, &error, &cache);
                    ASSERT_TRUE(relocated) << error.toStdString();
                    EXPECT_EQ(relocated->path, reference->path);
                    ASSERT_NO_FATAL_FAILURE(expectEquivalent(*reference, *relocated));
                    ASSERT_TRUE(QFile::remove(warm->path));
                    const auto regenerated = SoundSourceDerivative::materialize(
                        inputPath, definition, offset, platform, cachedDir, settings, &error, &cache);
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
    EXPECT_EQ(snapshot.sourceFingerprint(1, &error), asset.contentHash);
    ASSERT_TRUE(AssetConverter::writeFloatWav(path, std::vector<float>(257, -0.25f), 1, 48000));
    QFile changed(path);
    ASSERT_TRUE(changed.open(QIODevice::ReadWrite));
    ASSERT_TRUE(changed.setFileTime(originalTime, QFileDevice::FileModificationTime));
    changed.close();
    ASSERT_EQ(QFileInfo(path).size(), originalSize);
    ASSERT_EQ(QFileInfo(path).lastModified(), originalTime);
    EXPECT_FALSE(snapshot.request(1, platform, &error));
    EXPECT_FALSE(snapshot.sourceInputs->verifyUnchanged(&error));
    snapshot.sourceInputs = std::make_shared<BuildSourceInputs>();
    const auto current = snapshot.request(1, platform, &error);
    ASSERT_TRUE(current) << error.toStdString();
    EXPECT_NE(current->asset.contentHash, asset.contentHash);
    EXPECT_EQ(current->asset.contentHash, AssetImporter::computeContentHash(path));
    EXPECT_EQ(current->asset.metadata.sampleRate, 48000u);
    EXPECT_EQ(current->asset.metadata.totalSamples, 257u);
    EXPECT_EQ(snapshot.assets.value(1).contentHash, asset.contentHash);
    EXPECT_TRUE(snapshot.sourceInputs->verifyUnchanged(&error)) << error.toStdString();
}

TEST(SoundSourceSemanticsTest, BuildInputsKeepCapturedMediaStableAndRejectLaterSourceChanges)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("source.wav"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(path, std::vector<float>(257, 0.25f), 1, 44100));
    AssetInfo asset;
    asset.sourceID = 1;
    asset.absolutePath = path;
    BuildSourceInputs inputs;
    QString error;
    const auto captured = inputs.capture(asset, &error);
    ASSERT_TRUE(captured) << error.toStdString();
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
    const auto reused = inputs.capture(asset, &error);
    ASSERT_TRUE(reused) << error.toStdString();
    EXPECT_EQ(AssetImporter::computeContentHash(reused->inputPath), fingerprint);
    EXPECT_NE(AssetImporter::computeContentHash(path), fingerprint);
    EXPECT_FALSE(inputs.verifyUnchanged(&error));
    BuildSourceInputs nextBuild;
    EXPECT_NE(nextBuild.sourceFingerprint(asset, &error), fingerprint);
}

TEST(SoundSourceSemanticsTest, BuildInputDependencyAdoptionIsAtomicAndScopesPreserveBuildValidation)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    AssetInfo first;
    first.sourceID = 1;
    first.absolutePath = directory.filePath(QStringLiteral("first.wav"));
    AssetInfo second;
    second.sourceID = 2;
    second.absolutePath = directory.filePath(QStringLiteral("second.wav"));
    const auto write = [](const AssetInfo& asset, float sample) {
        return AssetConverter::writeFloatWav(asset.absolutePath, std::vector<float>(257, sample), 1, 44100);
    };
    ASSERT_TRUE(write(first, 0.25f));
    ASSERT_TRUE(write(second, 0.5f));
    BuildSourceInputs inputs;
    QString error;
    const auto firstHash = inputs.sourceFingerprint(first, &error);
    ASSERT_FALSE(firstHash.isEmpty());
    BuildSourceInputs rejected;
    ASSERT_FALSE(rejected.sourceFingerprint(second, &error).isEmpty());
    ASSERT_TRUE(write(first, -0.25f));
    ASSERT_NE(rejected.sourceFingerprint(first, &error), firstHash);
    EXPECT_FALSE(inputs.adopt(rejected, &error));
    EXPECT_EQ(inputs.fingerprints().size(), 1);
    ASSERT_TRUE(write(first, 0.25f));
    ASSERT_TRUE(QFile::remove(second.absolutePath));
    EXPECT_TRUE(inputs.verifyUnchanged(&error));
    ASSERT_TRUE(write(second, 0.5f));
    BuildSourceInputs accepted;
    const auto secondHash = accepted.sourceFingerprint(second, &error);
    ASSERT_FALSE(secondHash.isEmpty());
    EXPECT_TRUE(inputs.adopt(accepted, &error));
    EXPECT_EQ(inputs.fingerprints().size(), 2);
    inputs.beginDependencyScope();
    EXPECT_TRUE(inputs.fingerprints().isEmpty());
    EXPECT_EQ(inputs.sourceFingerprint(first, &error), firstHash);
    EXPECT_EQ(inputs.fingerprints().size(), 1);
    EXPECT_TRUE(inputs.verifyUnchanged(&error));
    ASSERT_TRUE(QFile::remove(second.absolutePath));
    EXPECT_FALSE(inputs.verifyUnchanged(&error));
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
    EXPECT_FALSE(cache.verifyUnchanged(&error));
    SoundSourceDerivative::MaterializationCache recoveryCache;
    const auto restored = SoundSourceDerivative::materialize(
        inputPath, definition, 0.0, platform, outputDir, {}, &error, &recoveryCache);
    ASSERT_TRUE(restored) << error.toStdString();
    EXPECT_FALSE(restored->cacheHit);
    EXPECT_EQ(restored->contentHash, expectedHash);
    EXPECT_EQ(AssetImporter::computeContentHash(restored->path), expectedHash);
    EXPECT_TRUE(recoveryCache.verifyUnchanged(&error));
    const auto receipts = QDir(outputDir).entryList({QStringLiteral("*.json")}, QDir::Files);
    ASSERT_EQ(receipts.size(), 1);
    QFile receipt(QDir(outputDir).filePath(receipts.front()));
    ASSERT_TRUE(receipt.open(QIODevice::ReadOnly));
    auto body = QJsonDocument::fromJson(receipt.readAll()).object();
    receipt.close();
    auto artifact = body.value(QStringLiteral("artifact")).toObject();
    artifact.insert(QStringLiteral("loopEnd"), QStringLiteral("32"));
    body.insert(QStringLiteral("artifact"), artifact);
    ASSERT_TRUE(receipt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const auto corruptedReceipt = QJsonDocument(body).toJson();
    ASSERT_EQ(receipt.write(corruptedReceipt), corruptedReceipt.size());
    receipt.close();
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
        desc.sourcePath = media->path;
        desc.format = HarkenEditor::Bridge::PackMediaFormat::PcmFloat;
        desc.loopable = media->loopable;
        desc.loopStartFrame = media->loopStartFrame;
        desc.loopEndFrame = media->loopEndFrame;
        uint64_t reference = 0;
        ASSERT_TRUE(HarkenEditor::Bridge::computePackMediaContentRevision(desc, &reference));
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
    for (int mode = 0; mode < 7; ++mode) {
        SCOPED_TRACE(mode);
        auto invalidSource = definition;
        auto invalidPlatform = platform;
        QString invalidPath = inputPath;
        double offset = 0.0;
        ConversionSettings settings;
        if (mode == 0) invalidSource.trimStartFrame = 1000;
        else if (mode == 1) invalidSource.trimEndFrame = 0;
        else if (mode == 2) offset = -1.0;
        else if (mode == 3) offset = std::numeric_limits<double>::quiet_NaN();
        else if (mode == 4) {
            settings.performResample = true;
            settings.targetSampleRate = 0;
        } else if (mode == 5) invalidPlatform = QUuid{};
        else invalidPath = directory.filePath(QStringLiteral("missing.wav"));
        QString referenceError;
        QString cachedError;
        const auto reference = SoundSourceDerivative::materialize(
            invalidPath, invalidSource, offset, invalidPlatform, outputDir, settings, &referenceError);
        const auto cached = SoundSourceDerivative::materialize(
            invalidPath, invalidSource, offset, invalidPlatform, outputDir, settings, &cachedError, &cache);
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
