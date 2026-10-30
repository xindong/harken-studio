#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetTypes.h"
#include "AssetPipeline/MediaPool.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/SourceFileSystem.h"
#include "AssetPipeline/WaveformCache.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrent>

#include <vector>

namespace {

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xffu));
    out.append(static_cast<char>((value >> 8) & 0xffu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xffu));
    out.append(static_cast<char>((value >> 8) & 0xffu));
    out.append(static_cast<char>((value >> 16) & 0xffu));
    out.append(static_cast<char>((value >> 24) & 0xffu));
}

void writeWav(const QString& path, uint32_t frames, float value, uint16_t channels = 1)
{
    const std::vector<float> samples(frames * channels, value);
    constexpr uint32_t sampleRate = 48000;
    const uint16_t blockAlign = channels * sizeof(float);
    constexpr uint32_t fmtSize = 18;
    constexpr uint32_t factSize = 4;
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);
    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVEfmt ", 8);
    appendU32(bytes, fmtSize);
    appendU16(bytes, 3);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * blockAlign);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 32);
    appendU16(bytes, 0);
    bytes.append("fact", 4);
    appendU32(bytes, factSize);
    appendU32(bytes, frames);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(samples.data()), dataBytes);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

}

TEST(MediaPoolRefreshTest, SharedWaveformsRefreshAfterSourceAndCacheChanges)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("shared.wav"));
    writeWav(path, 480, 0.2f);
    ASSERT_TRUE(WaveformCache::generate(path, 128));
    const auto original = WaveformCache::loadShared(path);
    ASSERT_NE(original, nullptr);
    ASSERT_TRUE(original->isValid());
    EXPECT_FLOAT_EQ(original->peaks[1], 0.2f);
    ASSERT_TRUE(WaveformCache::generate(path, 64));
    const auto rebuilt = WaveformCache::loadShared(path);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_EQ(rebuilt->numPeaks, 64u);
    EXPECT_EQ(original->numPeaks, 128u);

    const QDateTime changedTime = QFileInfo(path).lastModified().addSecs(1);
    writeWav(path, 480, 0.6f);
    QFile audio(path);
    ASSERT_TRUE(audio.open(QIODevice::ReadWrite));
    ASSERT_TRUE(audio.setFileTime(changedTime, QFileDevice::FileModificationTime));
    audio.close();
    ASSERT_TRUE(WaveformCache::generate(path, 128));
    const auto updated = WaveformCache::loadShared(path);
    ASSERT_NE(updated, nullptr);
    EXPECT_FLOAT_EQ(updated->peaks[1], 0.6f);
    EXPECT_FLOAT_EQ(original->peaks[1], 0.2f);
}

TEST(MediaPoolRefreshTest, CapturedWaveformsShareCacheAndRejectStaleContentWithUnchangedMetadata)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString audioDir = dir.filePath(QStringLiteral("AudioSources"));
    ASSERT_TRUE(QDir().mkpath(audioDir));
    const QString path = QDir(audioDir).filePath(QStringLiteral("source.wav"));
    const QString captured = dir.filePath(QStringLiteral("captured.wav"));
    writeWav(path, 480, 0.2f, 2);
    const auto sourceTime = QFileInfo(path).lastModified();
    const auto capture = [&] {
        QFile::remove(captured);
        if (!QFile::copy(path, captured))
            return false;
        QFile file(captured);
        return file.open(QIODevice::ReadWrite)
            && file.setFileTime(sourceTime, QFileDevice::FileModificationTime);
    };
    ASSERT_TRUE(capture());
    ASSERT_TRUE(WaveformCache::generate(path, 128));
    WaveformPeakData midSide;
    ASSERT_TRUE(WaveformCache::generateMidSide(path, midSide));
    const auto original = WaveformCache::loadShared(path);
    ASSERT_NE(original, nullptr);
    const auto cacheTime = QFileInfo(WaveformCache::cachePath(path)).lastModified();
    QByteArray bytes;
    ASSERT_TRUE(WaveformCache::readForBuild(captured, path,
        AssetImporter::computeContentHash(captured), bytes));
    EXPECT_EQ(QFileInfo(WaveformCache::cachePath(path)).lastModified(), cacheTime);
    EXPECT_FALSE(QFileInfo::exists(WaveformCache::cachePath(captured)));
    const auto originalBytes = bytes;
    const auto originalHash = AssetImporter::computeContentHash(captured);
    ASSERT_TRUE(QFile::remove(WaveformCache::cachePath(path) + QStringLiteral(".source.json")));
    ASSERT_TRUE(WaveformCache::readForBuild(captured, path, originalHash, bytes));
    EXPECT_EQ(bytes, originalBytes);

    writeWav(path, 480, 0.6f, 2);
    QFile source(path);
    ASSERT_TRUE(source.open(QIODevice::ReadWrite));
    ASSERT_TRUE(source.setFileTime(sourceTime, QFileDevice::FileModificationTime));
    source.close();
    ASSERT_TRUE(capture());
    const auto updatedHash = AssetImporter::computeContentHash(captured);
    ASSERT_NE(updatedHash, originalHash);
    ASSERT_TRUE(WaveformCache::readForBuild(captured, path, updatedHash, bytes));
    EXPECT_NE(bytes, originalBytes);
    const auto updated = WaveformCache::loadShared(path);
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->numPeaks, original->numPeaks);
    EXPECT_TRUE(updated->hasMidSideData());
    EXPECT_FLOAT_EQ(updated->peaks[1], 0.6f);
    EXPECT_FLOAT_EQ(original->peaks[1], 0.2f);

    QFile cache(WaveformCache::cachePath(path));
    ASSERT_TRUE(cache.open(QIODevice::WriteOnly));
    ASSERT_EQ(cache.write(originalBytes), originalBytes.size());
    cache.close();
    const auto updatedBytes = bytes;
    ASSERT_TRUE(WaveformCache::readForBuild(captured, path, updatedHash, bytes));
    EXPECT_EQ(bytes, updatedBytes);
    ASSERT_TRUE(cache.open(QIODevice::ReadOnly));
    EXPECT_EQ(cache.readAll(), bytes);
}

TEST(MediaPoolRefreshTest, CacheRemovalInvalidatesPendingWaveformPublication)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("pending.wav"));
    writeWav(path, 480, 0.1f);
    ASSERT_TRUE(WaveformCache::generate(path));

    auto* pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threadCount = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore started;
    QSemaphore resume;
    auto blocker = QtConcurrent::run([&] {
        started.release();
        resume.acquire();
    });
    const auto cleanup = qScopeGuard([&] {
        resume.release();
        blocker.waitForFinished();
        pool->waitForDone();
        pool->setMaxThreadCount(threadCount);
    });
    started.acquire();
    auto waveform = WaveformCache::generateAsync(path);
    ASSERT_TRUE(WaveformCache::remove(path));
    resume.release();
    waveform.waitForFinished();
    EXPECT_FALSE(waveform.result());
    EXPECT_FALSE(WaveformCache::isValid(path));
}

TEST(MediaPoolRefreshTest, SourceChangesInvalidateQueuedReadsAndResumeDeferredWaveforms)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = AssetRegistry::instance();
    auto& media = MediaPool::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto clearRegistry = qScopeGuard([&] { registry.clear(); });
    const QString path = QDir(PathUtils::getAudioSourcesDir(dir.path()))
        .filePath(QStringLiteral("pending.wav"));
    const QString replacement = dir.filePath(QStringLiteral("replacement.wav"));
    writeWav(path, 480, 0.1f);
    writeWav(replacement, 240, 0.7f);
    AssetImporter importer;
    importer.setProjectRoot(dir.path());
    AssetInfo asset;
    asset.sourceID = 8699;
    asset.relativePath = QStringLiteral("pending.wav");
    asset.absolutePath = path;
    asset.metadata = importer.analyzeFile(path);
    asset.contentHash = AssetImporter::computeContentHash(path);
    ASSERT_TRUE(registry.registerAsset(asset, false));

    auto* pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threadCount = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore started;
    QSemaphore resume;
    auto blocker = QtConcurrent::run([&] {
        started.release();
        resume.acquire();
    });
    const auto restorePool = qScopeGuard([&] {
        resume.release();
        blocker.waitForFinished();
        pool->waitForDone();
        pool->setMaxThreadCount(threadCount);
    });
    started.acquire();
    auto queued = WaveformCache::generateAsync(path, 64);
    {
        media.beginSourceFileChange();
        const auto finishChange = qScopeGuard([&] { media.endSourceFileChange(); });
        ASSERT_TRUE(SourceFileSystem::copyFile(replacement, path));
    }
    resume.release();
    queued.waitForFinished();
    EXPECT_FALSE(queued.result());
    pool->waitForDone();
    const auto resumed = WaveformCache::loadShared(path);
    ASSERT_NE(resumed, nullptr);
    EXPECT_FLOAT_EQ(resumed->peaks[1], 0.7f);
    EXPECT_EQ(AssetImporter::computeContentHash(path), AssetImporter::computeContentHash(replacement));
}
