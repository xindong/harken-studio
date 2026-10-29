#include "Core/AssetPipeline/AssetConverter.h"
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
    const auto platform = QUuid::createUuid();
    SoundSourceDerivative::MaterializationCache cache;
    const auto expectEquivalent = [](const auto& reference, const auto& candidate) {
        EXPECT_EQ(candidate.cacheKey, reference.cacheKey);
        EXPECT_EQ(candidate.sampleRate, reference.sampleRate);
        EXPECT_EQ(candidate.channels, reference.channels);
        EXPECT_EQ(candidate.loopStartFrame, reference.loopStartFrame);
        EXPECT_EQ(candidate.loopEndFrame, reference.loopEndFrame);
        EXPECT_EQ(candidate.loopable, reference.loopable);
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
