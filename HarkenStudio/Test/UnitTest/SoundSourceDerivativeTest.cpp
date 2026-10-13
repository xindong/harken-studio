#include "Support/ProjectPlatformTestUtils.h"
#include "Core/SoundSourceDerivative.h"

#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetImporter.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "Core/SoundSourceSemantics.h"

#include <QCryptographicHash>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace VNX;

TEST(SoundSourceDerivativeTest, MapsTrimFramesToPackFrames)
{
    SoundSourceDef source;
    source.sourceId = 1;
    source.trimStartFrame = 12000;
    source.trimEndFrame = 36000;
    const auto range = SoundSourceDerivative::mapFrameRange(source, 48000);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->start, 12000u);
    EXPECT_EQ(range->end, 36000u);
}

TEST(SoundSourceDerivativeTest, MapFrameRangeClampsToTheDecodedLength)
{
    SoundSourceDef source;
    source.trimStartFrame = 100;
    source.trimEndFrame = 90000;
    const auto range = SoundSourceDerivative::mapFrameRange(source, 48000);
    ASSERT_TRUE(range);
    EXPECT_EQ(range->start, 100u);
    EXPECT_EQ(range->end, 48000u);
    EXPECT_FALSE(SoundSourceDerivative::mapFrameRange(source, 0));
}

TEST(SoundSourceDerivativeTest, CacheKeyIncludesInputPlatformAndDeepSemantics)
{
    SoundSourceDerivative::DerivativeKey first;
    first.sourceId = 1;
    first.inputFingerprint = QStringLiteral("input-a");
    first.platform = HarkenTest::platformId("Windows");
    first.trimEndFrame = 48000;
    first.sourceChannels = 2;
    auto second = first;
    second.inputFingerprint = QStringLiteral("input-b");
    EXPECT_NE(SoundSourceDerivative::cacheKey(first), SoundSourceDerivative::cacheKey(second));
    second = first;
    second.platform = HarkenTest::platformId("macOS");
    EXPECT_NE(SoundSourceDerivative::cacheKey(first), SoundSourceDerivative::cacheKey(second));
    second = first;
    second.scheduleOffsetSec = 0.25;
    EXPECT_NE(SoundSourceDerivative::cacheKey(first), SoundSourceDerivative::cacheKey(second));
    second = first;
    second.trimEndFrame = 48001;
    EXPECT_NE(SoundSourceDerivative::cacheKey(first), SoundSourceDerivative::cacheKey(second));
    second = first;
    second.loopRegion = SoundLoopRegion{100, 200};
    EXPECT_NE(SoundSourceDerivative::cacheKey(first), SoundSourceDerivative::cacheKey(second));
    second = first;
    second.sourceChannels = 1;
    EXPECT_NE(SoundSourceDerivative::cacheKey(first), SoundSourceDerivative::cacheKey(second));
}

TEST(SoundSourceDerivativeTest, PreservesStereoAndIgnoresOldSpatialMonoCache)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("source.wav"));
    const std::vector<float> samples{
        0.25f, -0.125f, 0.5f, 0.25f, -0.75f, 0.125f, 0.0f, 0.5f};
    ASSERT_TRUE(AssetConverter::writeFloatWav(input, samples, 2, 48000));

    SoundSourceDef source;
    source.sourceId = 1;
    source.trimStartFrame = 1;
    source.trimEndFrame = 3;
    const auto platform = HarkenTest::platformId("Windows");
    const ConversionSettings settings;

    QByteArray oldIdentity;
    oldIdentity += QByteArray::number(source.sourceId) + '\0';
    oldIdentity += QByteArray::number(settings.hash()) + '\0';
    oldIdentity += QByteArray::number(true) + '\0';
    oldIdentity += AssetImporter::computeContentHash(input).toUtf8() + '\0';
    oldIdentity += ProjectPlatformSemantics::idString(platform).toUtf8() + '\0';
    oldIdentity += QByteArray::number(1) + '\0';
    oldIdentity += QByteArray::number(3) + '\0';
    oldIdentity += QByteArray::number(0.0, 'g', 17) + '\0';
    const QString oldPath = directory.filePath(QString::fromLatin1(
        QCryptographicHash::hash(oldIdentity, QCryptographicHash::Sha256).toHex())
        + QStringLiteral(".wav"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(
        oldPath, {0.375f, -0.3125f}, 1, 48000));
    const QString oldHash = AssetImporter::computeContentHash(oldPath);

    QString error;
    const auto artifact = SoundSourceDerivative::materialize(
        input, source, 0.0, platform, directory.path(), settings, &error);
    ASSERT_TRUE(artifact) << error.toStdString();
    EXPECT_FALSE(artifact->cacheHit);
    EXPECT_NE(artifact->path, oldPath);
    EXPECT_EQ(artifact->channels, 2);
    std::vector<float> decoded;
    int channels = 0;
    int sampleRate = 0;
    ASSERT_TRUE(HarkenEditor::Bridge::decodeAudioFileInterleaved(
        artifact->path, decoded, channels, sampleRate));
    EXPECT_EQ(channels, 2);
    EXPECT_EQ(sampleRate, 48000);
    EXPECT_EQ(decoded, (std::vector<float>{0.5f, 0.25f, -0.75f, 0.125f}));
    EXPECT_EQ(AssetImporter::computeContentHash(oldPath), oldHash);

    const auto cached = SoundSourceDerivative::materialize(
        input, source, 0.0, platform, directory.path(), settings, &error);
    ASSERT_TRUE(cached) << error.toStdString();
    EXPECT_TRUE(cached->cacheHit);
    EXPECT_EQ(cached->path, artifact->path);
    EXPECT_EQ(cached->channels, 2);
}

TEST(SoundSourceDerivativeTest, EmptyTrimRangeIsRejected)
{
    SoundSourceDef source;
    source.trimStartFrame = 48000;
    source.trimEndFrame = 48000;
    EXPECT_FALSE(SoundSourceDerivative::mapFrameRange(source, 48000));
}

TEST(SoundSourceDerivativeTest, MaterializeRejectsMissingInputAtomically)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    EXPECT_FALSE(SoundSourceDerivative::materialize(
        dir.filePath(QStringLiteral("missing.wav")), {}, 0.0,
        HarkenTest::platformId("Windows"), dir.path(), ConversionSettings{}, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(SoundSourceDerivativeTest, MaterializeRejectsMissingInputWithDeepSemantics)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SoundSourceDef source;
    source.sourceId = 1;
    source.trimStartFrame = 12000;
    source.trimEndFrame = 36000;
    source.loopRegion = SoundLoopRegion{18000, 30000};
    QString error;
    EXPECT_FALSE(SoundSourceDerivative::materialize(
        QStringLiteral("missing.wav"), source, 0.0,
        HarkenTest::platformId("Windows"), dir.path(), ConversionSettings{}, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(SoundSourceDerivativeTest, AFileSeededLoopStaysOutOfTheDerivativeKey)
{
    AssetInfo info;
    info.sourceID = 1;
    info.absolutePath = QStringLiteral("audio.wav");
    info.metadata.sampleRate = 48000;
    info.metadata.totalSamples = 48000;
    info.metadata.loopInfo.enabled = true;
    info.metadata.loopInfo.loopStart = 100;
    info.metadata.loopInfo.loopEnd = 199;

    SoundSourceDef seeded;
    seeded.sourceId = 1;
    seeded.trimEndFrame = 36000;
    seeded.loopRegion = SoundLoopRegion{100, 200};
    seeded.loopProvenance = SoundLoopProvenance::File;
    EXPECT_FALSE(SoundSourceSemantics::authoredView(seeded, &info).loopRegion);

    SoundSourceDerivative::DerivativeKey unlooped;
    unlooped.sourceId = 1;
    unlooped.inputFingerprint = QStringLiteral("input-a");
    unlooped.platform = HarkenTest::platformId("Windows");
    unlooped.trimEndFrame = 36000;
    auto fromSeed = unlooped;
    fromSeed.loopRegion = SoundSourceSemantics::authoredView(seeded, &info).loopRegion;
    EXPECT_EQ(SoundSourceDerivative::cacheKey(unlooped),
              SoundSourceDerivative::cacheKey(fromSeed));

    seeded.loopProvenance = SoundLoopProvenance::Authored;
    auto fromAuthored = unlooped;
    fromAuthored.loopRegion = SoundSourceSemantics::authoredView(seeded, &info).loopRegion;
    ASSERT_TRUE(fromAuthored.loopRegion);
    EXPECT_NE(SoundSourceDerivative::cacheKey(unlooped),
              SoundSourceDerivative::cacheKey(fromAuthored));
}

TEST(SoundSourceDerivativeTest, PreparedAndExportedLoopBoundsStayHalfOpenAcrossRates)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("source.wav"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(path, std::vector<float>(1024, 0.25f), 1, 48000));
    SoundSourceDef source;
    source.sourceId = 1;
    source.trimStartFrame = 128;
    source.trimEndFrame = 896;
    source.loopRegion = SoundLoopRegion{256, 768};
    const struct {
        bool process;
        uint32_t rate;
        uint64_t start;
        uint64_t end;
    } cases[] = {{false, 48000, 608, 1120}, {true, 48000, 608, 1120},
                 {true, 24000, 304, 560}, {true, 96000, 1216, 2240}};
    for (const auto& test : cases) {
        SCOPED_TRACE(test.rate);
        SCOPED_TRACE(test.process);
        ConversionSettings settings;
        settings.performResample = test.process;
        settings.targetSampleRate = test.rate;
        QString error;
        const auto prepared = SoundSourceDerivative::prepare(path, source, 0.01, settings, &error);
        ASSERT_TRUE(prepared) << error.toStdString();
        EXPECT_EQ(prepared->loopStartFrame, test.start);
        EXPECT_EQ(prepared->loopEndFrame, test.end);
        const auto artifact = SoundSourceDerivative::materialize(path, source, 0.01,
            HarkenTest::platformId("Windows"), directory.path(), settings, &error);
        ASSERT_TRUE(artifact) << error.toStdString();
        EXPECT_TRUE(artifact->loopable);
        EXPECT_EQ(artifact->loopStartFrame, test.start);
        EXPECT_EQ(artifact->loopEndFrame, test.end);
    }
}

TEST(SoundSourceDerivativeTest, SingleFrameLoopSurvivesWithoutResampling)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("source.wav"));
    ASSERT_TRUE(AssetConverter::writeFloatWav(path, std::vector<float>(16, 0.25f), 1, 48000));
    SoundSourceDef source;
    source.sourceId = 1;
    source.loopRegion = SoundLoopRegion{4, 5};
    QString error;
    const auto artifact = SoundSourceDerivative::materialize(path, source, 0.0,
        HarkenTest::platformId("Windows"), directory.path(), ConversionSettings{}, &error);
    ASSERT_TRUE(artifact) << error.toStdString();
    EXPECT_TRUE(artifact->loopable);
    EXPECT_EQ(artifact->loopStartFrame, 4u);
    EXPECT_EQ(artifact->loopEndFrame, 5u);
}
