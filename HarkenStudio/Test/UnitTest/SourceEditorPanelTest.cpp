#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/AudioEngine.h"
#include "Core/Components/VSound.h"
#include "Core/ProjectManager.h"
#include "Core/AuditionManager.h"
#include "RuntimeBridge/PlaybackBridge.h"
#include "Core/SoundSourceSemantics.h"
#include "Core/UndoCommands.h"
#include "Widgets/SourceEditor/SourceEditorPanel.h"
#include "Widgets/SourceEditor/SourceEditorWaveformView.h"
#include "Widgets/SourceEditor/SourceEditorWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWheelEvent>

#include <chrono>
#include <thread>
#include <vector>

namespace {

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
    out.append(static_cast<char>((value >> 16) & 0xFFu));
    out.append(static_cast<char>((value >> 24) & 0xFFu));
}

void writeWavF32(const QString& path, const std::vector<float>& interleaved,
                 uint16_t channels, uint32_t sampleRate)
{
    ASSERT_NE(channels, 0);
    ASSERT_EQ(interleaved.size() % channels, 0u);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));

    const uint32_t frames = static_cast<uint32_t>(interleaved.size() / channels);
    const uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * sizeof(float));
    const uint16_t blockAlign = static_cast<uint16_t>(channels * sizeof(float));
    const uint32_t byteRate = sampleRate * blockAlign;
    constexpr uint32_t fmtSize = 18;
    constexpr uint32_t factSize = 4;
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);

    QByteArray bytes;
    bytes.reserve(static_cast<int>(riffSize + 8));
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVE", 4);
    bytes.append("fmt ", 4);
    appendU32(bytes, fmtSize);
    appendU16(bytes, 3);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, byteRate);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 32);
    appendU16(bytes, 0);
    bytes.append("fact", 4);
    appendU32(bytes, factSize);
    appendU32(bytes, frames);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(interleaved.data()),
                 static_cast<qsizetype>(dataBytes));

    ASSERT_EQ(file.write(bytes), bytes.size());
}

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

bool ensureNullEngine()
{
    auto& engine = VNX::AudioEngine::instance();
    if (engine.isInitialized() && !engine.isSimulationMode())
        return true;
    if (engine.isInitialized())
        engine.shutdown();
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 256;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    return engine.initialize(config) && !engine.isSimulationMode();
}

}

class SourceEditorPanelTest : public ::testing::Test
{
protected:
    static void TearDownTestSuite()
    {
        VNX::AudioEngine::instance().shutdown();
    }
};

TEST_F(SourceEditorPanelTest, WaveformDetailReadsOnlyRequestedFramesAndPreservesChannels)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("detail.wav"));
    std::vector<float> samples;
    for (int frame = 0; frame < 20; ++frame) {
        samples.push_back(float(frame) / 20);
        samples.push_back(-float(frame) / 20);
        samples.push_back(0.125f);
    }
    writeWavF32(path, samples, 3, 48000);
    auto detail = VNX::WaveformCache::readRange(path, 7, 9, 3);
    ASSERT_TRUE(detail.isValid());
    EXPECT_EQ(detail.numChannels, 3u);
    EXPECT_EQ(detail.numPeaks, 3u);
    EXPECT_EQ(detail.samplesPerPeak, 3u);
    EXPECT_FLOAT_EQ(detail.peaks[0], 7.0f / 20);
    EXPECT_FLOAT_EQ(detail.peaks[1], 9.0f / 20);
    EXPECT_FLOAT_EQ(detail.peaks[6], -9.0f / 20);
    EXPECT_FLOAT_EQ(detail.peaks[7], -7.0f / 20);
    EXPECT_FLOAT_EQ(detail.peaks[12], 0.125f);
    detail = VNX::WaveformCache::readRange(path, 17, 20, 10);
    ASSERT_TRUE(detail.isValid());
    EXPECT_EQ(detail.numPeaks, 3u);
    EXPECT_EQ(detail.samplesPerPeak, 1u);
    EXPECT_FLOAT_EQ(detail.peaks[4], 19.0f / 20);
    EXPECT_FLOAT_EQ(detail.peaks[5], 19.0f / 20);
    detail = VNX::WaveformCache::readRange(path, 0, 20, 6);
    ASSERT_TRUE(detail.isValid());
    EXPECT_LE(detail.numPeaks, 6u);
    EXPECT_FLOAT_EQ(detail.peaks[(detail.numPeaks - 1) * 2 + 1], 19.0f / 20);
    EXPECT_FALSE(VNX::WaveformCache::readRange(path, 20, 1, 1).isValid());
    EXPECT_FALSE(VNX::WaveformCache::readRange(path, 0, 0, 1).isValid());
    EXPECT_FALSE(VNX::WaveformCache::readRange(path, 0, 1, 0).isValid());
}

TEST_F(SourceEditorPanelTest, TrimTimelineLoadsSampleDetailAfterZooming)
{
    ensureApp();
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("zoom.wav"));
    std::vector<float> samples;
    for (int frame = 0; frame < 4800; ++frame) {
        samples.push_back(frame % 2 ? 0.5f : -0.5f);
        samples.push_back(0.0f);
    }
    writeWavF32(path, samples, 2, 48000);
    ASSERT_TRUE(VNX::WaveformCache::generate(path));
    VNX::AssetInfo asset;
    asset.sourceID = 41;
    asset.absolutePath = path;
    asset.metadata.sampleRate = 48000;
    asset.metadata.totalSamples = 4800;
    asset.metadata.channels = 2;
    VNX::SourceEditorWaveformView waveform(
        nullptr, VNX::SourceEditorWaveformView::Presentation::TrimEditor);
    waveform.resize(640, 360);
    waveform.setAsset(&asset);
    waveform.setSource({});
    waveform.show();
    QApplication::processEvents();
    ASSERT_TRUE(waveform.hasWaveform());
    waveform.setViewRange(2400, 8);
    const auto renderWave = [&] {
        QImage image(waveform.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        waveform.render(&image);
        return image.copy(waveform.waveformRect().adjusted(15, 40, -15, -20));
    };
    const QImage overview = renderWave();
    bool detailed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (renderWave() != overview) {
            detailed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(detailed);
    EXPECT_DOUBLE_EQ(waveform.viewStartFrame(), 2400);
    EXPECT_DOUBLE_EQ(waveform.viewFrameCount(), 8);
    waveform.setCommitHandler([](const VNX::SoundSourceDef&) {
        ADD_FAILURE() << "View zoom committed a source edit";
    });
    auto* vertical = waveform.findChild<QScrollBar*>(QStringLiteral("sourceWaveChannelScroll"));
    ASSERT_NE(vertical, nullptr);
    ASSERT_EQ(vertical->maximum(), 0);
    const QImage fitted = renderWave();
    const QPoint position = waveform.waveformRect().center();
    QWheelEvent wheel(position, waveform.mapToGlobal(position), QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&waveform, &wheel);
    EXPECT_TRUE(wheel.isAccepted());
    const QImage zoomed = renderWave();
    EXPECT_NE(zoomed, fitted);
    EXPECT_TRUE(vertical->isEnabled());
    EXPECT_GT(vertical->maximum(), 0);
    EXPECT_DOUBLE_EQ(waveform.viewStartFrame(), 2400);
    EXPECT_DOUBLE_EQ(waveform.viewFrameCount(), 8);
    vertical->setValue(vertical->maximum());
    EXPECT_NE(renderWave(), zoomed);
    waveform.findChild<QToolButton*>(QStringLiteral("sourceWaveVerticalZoomOut"))->click();
    EXPECT_EQ(vertical->maximum(), 0);
    EXPECT_EQ(renderWave(), fitted);
}

TEST_F(SourceEditorPanelTest, WaveformCrosshairTracksTimeViewAndClearsOnExit)
{
    ensureApp();
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("crosshair.wav"));
    writeWavF32(path, std::vector<float>(64, 0.5f), 1, 64);
    ASSERT_TRUE(VNX::WaveformCache::generate(path));
    VNX::AssetInfo asset;
    asset.sourceID = 42;
    asset.absolutePath = path;
    asset.metadata.sampleRate = 64;
    asset.metadata.totalSamples = 64;
    asset.metadata.channels = 1;
    for (auto presentation : {VNX::SourceEditorWaveformView::Presentation::Preview,
                             VNX::SourceEditorWaveformView::Presentation::TrimEditor}) {
        VNX::SourceEditorWaveformView waveform(nullptr, presentation);
        waveform.resize(640, 360);
        waveform.setAsset(&asset);
        waveform.setSource({});
        waveform.setAmplitudeExponent(1.0);
        waveform.show();
        QApplication::processEvents();
        ASSERT_TRUE(waveform.hasWaveform());
        const QPoint point = waveform.waveformRect().center();
        QMouseEvent move(QEvent::MouseMove, QPointF(point),
            QPointF(waveform.mapToGlobal(point)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&waveform, &move);
        ASSERT_TRUE(waveform.crosshair().visible);
        const auto readoutSeconds = [&] {
            return waveform.crosshair().label.section(QLatin1Char(' '), 0, 0).toDouble();
        };
        EXPECT_NEAR(readoutSeconds(), 0.5, 0.01);
        const QPoint amplitudePoint(point.x(), waveform.waveformRect().top() + waveform.waveformRect().height() / 4);
        QMouseEvent amplitudeMove(QEvent::MouseMove, QPointF(amplitudePoint),
            QPointF(waveform.mapToGlobal(amplitudePoint)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&waveform, &amplitudeMove);
        const QString linearLabel = waveform.crosshair().label;
        const auto readoutDb = [&] {
            return waveform.crosshair().label.section(QStringLiteral(" | "), 1, 1)
                .section(QLatin1Char(' '), 0, 0).toDouble();
        };
        const double linearDb = readoutDb();
        waveform.setCommitHandler([](const VNX::SoundSourceDef&) {
            ADD_FAILURE() << "Amplitude curve changed source data";
        });
        waveform.setAmplitudeExponent(0.3);
        ASSERT_TRUE(waveform.crosshair().visible);
        EXPECT_LT(readoutDb(), linearDb);
        EXPECT_EQ(waveform.crosshair().label.section(QStringLiteral(" | "), 0, 0),
            linearLabel.section(QStringLiteral(" | "), 0, 0));
        const double powerDb = readoutDb();
        waveform.setAmplitudeExponent(0.0);
        EXPECT_LT(readoutDb(), powerDb);
        waveform.setAmplitudeExponent(1.0);
        EXPECT_EQ(waveform.crosshair().label, linearLabel);
        QApplication::sendEvent(&waveform, &move);
        if (presentation == VNX::SourceEditorWaveformView::Presentation::TrimEditor) {
            waveform.setViewRange(0, 32);
            EXPECT_NEAR(readoutSeconds(), 0.25, 0.01);
            const QString beforeZoom = waveform.crosshair().label;
            auto* zoom = waveform.findChild<QToolButton*>(QStringLiteral("sourceWaveVerticalZoomIn"));
            zoom->click();
            ASSERT_TRUE(waveform.crosshair().visible);
            auto* scroll = waveform.findChild<QScrollBar*>(QStringLiteral("sourceWaveChannelScroll"));
            scroll->setValue(scroll->maximum());
            EXPECT_NE(waveform.crosshair().label, beforeZoom);
            QEvent enter(QEvent::Enter);
            QApplication::sendEvent(zoom, &enter);
            EXPECT_FALSE(waveform.crosshair().visible);
            QApplication::sendEvent(&waveform, &move);
            EXPECT_TRUE(waveform.crosshair().visible);
        }
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&waveform, &leave);
        EXPECT_FALSE(waveform.crosshair().visible);
        QApplication::sendEvent(&waveform, &move);
        waveform.setAsset(nullptr);
        EXPECT_FALSE(waveform.crosshair().visible);
    }
}

TEST_F(SourceEditorPanelTest, WidgetGeneratesMissingWaveformCacheAndReusesIt)
{
    ensureApp();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    constexpr uint32_t kSampleRate = 48000;
    constexpr uint16_t kChannels = 2;
    const QString path = QDir(directory.path()).filePath(QStringLiteral("editor.wav"));
    std::vector<float> samples(kSampleRate * kChannels / 10);
    for (size_t frame = 0; frame < samples.size() / kChannels; ++frame) {
        const float phase = static_cast<float>(frame % 128) / 127.0f;
        samples[frame * kChannels] = phase * 2.0f - 1.0f;
        samples[frame * kChannels + 1] = 1.0f - phase * 2.0f;
    }
    writeWavF32(path, samples, kChannels, kSampleRate);
    ASSERT_FALSE(QFile::exists(VNX::WaveformCache::cachePath(path)));

    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();
    VNX::AssetInfo asset;
    asset.sourceID = 71;
    asset.relativePath = QStringLiteral("editor.wav");
    asset.absolutePath = path;
    asset.metadata.sampleRate = kSampleRate;
    asset.metadata.channels = kChannels;
    asset.metadata.totalSamples = samples.size() / kChannels;
    ASSERT_TRUE(assets.registerAsset(asset));

    VNX::SourceEditorWidget widget;
    widget.resize(720, 360);
    widget.bindSource(asset.sourceID);
    auto* waveform = widget.findChild<VNX::SourceEditorWaveformView*>(
        QStringLiteral("sourceEditorWaveform"));
    ASSERT_NE(waveform, nullptr);
    EXPECT_TRUE(waveform->isGenerating());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!waveform->hasWaveform() && std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(waveform->hasWaveform());
    EXPECT_TRUE(waveform->hasBandData());
    EXPECT_TRUE(QFile::exists(VNX::WaveformCache::cachePath(path)));

    VNX::SourceEditorWidget reopened;
    reopened.bindSource(asset.sourceID);
    auto* cached = reopened.findChild<VNX::SourceEditorWaveformView*>(
        QStringLiteral("sourceEditorWaveform"));
    ASSERT_NE(cached, nullptr);
    EXPECT_TRUE(cached->hasWaveform());
    EXPECT_FALSE(cached->isGenerating());

    assets.clear();
}

TEST_F(SourceEditorPanelTest, SamePathContentChangeReloadsWaveform)
{
    ensureApp();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = QDir(directory.path()).filePath(QStringLiteral("refreshed.wav"));
    writeWavF32(path, std::vector<float>(1024, 0.25f), 1, 48000);
    ASSERT_TRUE(VNX::WaveformCache::generate(path, 64));

    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();
    VNX::AssetInfo asset;
    asset.sourceID = 80;
    asset.relativePath = QStringLiteral("refreshed.wav");
    asset.absolutePath = path;
    asset.contentHash = QStringLiteral("first-hash");
    asset.metadata.sampleRate = 48000;
    asset.metadata.channels = 1;
    asset.metadata.fileSize = QFileInfo(path).size();
    asset.metadata.totalSamples = 1024;
    ASSERT_TRUE(assets.registerAsset(asset));

    VNX::SourceEditorWidget widget;
    widget.bindSource(asset.sourceID);
    auto* waveform = widget.findChild<VNX::SourceEditorWaveformView*>(
        QStringLiteral("sourceEditorWaveform"));
    ASSERT_NE(waveform, nullptr);
    ASSERT_TRUE(waveform->hasWaveform());

    writeWavF32(path, std::vector<float>(2048, -0.5f), 1, 48000);
    VNX::AudioMetadata updated = asset.metadata;
    updated.fileSize = QFileInfo(path).size();
    updated.totalSamples = 2048;
    ASSERT_TRUE(assets.updateSourceFile(asset.sourceID, updated, path,
                                        QStringLiteral("second-hash"), 1));
    EXPECT_FALSE(waveform->hasWaveform());
    EXPECT_TRUE(waveform->isGenerating());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!waveform->hasWaveform() && std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(waveform->hasWaveform());

    assets.clear();
}

TEST_F(SourceEditorPanelTest, WaveformHandlesKeepRangesOrderedAndInsideTrimWindow)
{
    ensureApp();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = QDir(directory.path()).filePath(QStringLiteral("ordered.wav"));
    writeWavF32(path, std::vector<float>(1000, 0.25f), 1, 1000);
    ASSERT_TRUE(VNX::WaveformCache::generate(path, 64));

    VNX::AssetInfo asset;
    asset.sourceID = 81;
    asset.absolutePath = path;
    asset.metadata.sampleRate = 1000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 1000;

    VNX::SoundSourceDef source;
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 100;
    source.trimEndFrame = 900;
    source.loopRegion = VNX::SoundLoopRegion{300, 700};

    VNX::SourceEditorWaveformView waveform;
    waveform.resize(720, 240);
    waveform.setSource(source);
    waveform.setAsset(&asset);
    waveform.setEditable(true);
    waveform.show();
    QApplication::processEvents(QEventLoop::AllEvents);

    VNX::SoundSourceDef committed;
    waveform.setCommitHandler([&committed](const VNX::SoundSourceDef& value) {
        committed = value;
    });
    const QPoint trimStart = waveform.handleHitPoint(0);
    const QPointF pastTrimEnd(waveform.width() - 8.0, trimStart.y());
    QMouseEvent trimPress(QEvent::MouseButtonPress, trimStart,
                          waveform.mapToGlobal(trimStart), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&waveform, &trimPress);
    QMouseEvent trimRelease(QEvent::MouseButtonRelease, pastTrimEnd,
                            waveform.mapToGlobal(pastTrimEnd), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&waveform, &trimRelease);

    ASSERT_TRUE(committed.trimEndFrame.has_value());
    EXPECT_LT(committed.trimStartFrame, *committed.trimEndFrame);
    ASSERT_TRUE(committed.loopRegion.has_value());
    EXPECT_GE(committed.loopRegion->startFrame, committed.trimStartFrame);
    EXPECT_LT(committed.loopRegion->startFrame, committed.loopRegion->endFrame);
    EXPECT_LE(committed.loopRegion->endFrame, *committed.trimEndFrame);

    source.loopRegion = VNX::SoundLoopRegion{0, 0};
    waveform.setSource(source);
    const QPoint loopStart = waveform.handleHitPoint(1);
    QMouseEvent loopPress(QEvent::MouseButtonPress, loopStart,
                          waveform.mapToGlobal(loopStart), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&waveform, &loopPress);
    QMouseEvent loopRelease(QEvent::MouseButtonRelease, loopStart,
                            waveform.mapToGlobal(loopStart), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&waveform, &loopRelease);

    ASSERT_TRUE(committed.loopRegion.has_value());
    EXPECT_GE(committed.loopRegion->startFrame, committed.trimStartFrame);
    EXPECT_LT(committed.loopRegion->startFrame, committed.loopRegion->endFrame);
    EXPECT_LE(committed.loopRegion->endFrame, *committed.trimEndFrame);
}

TEST_F(SourceEditorPanelTest, ViewerWaveformDoesNotCommitReferenceRange)
{
    ensureApp();
    VNX::ActionManager::instance().clearAllForTesting();
    VNX::registerProjectActions(VNX::ActionManager::instance(),
                                &VNX::ProjectManager::instance(), nullptr, {});
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("SourceEditorRangeProject");
    config.location = directory.path();
    ASSERT_TRUE(VNX::ProjectManager::instance().createProject(config));

    const QString path = QDir(directory.path()).filePath(QStringLiteral("range.wav"));
    writeWavF32(path, std::vector<float>(1000, 0.25f), 1, 1000);
    ASSERT_TRUE(VNX::WaveformCache::generate(path, 64));

    auto& assets = VNX::AssetRegistry::instance();
    VNX::AssetInfo asset;
    asset.sourceID = 73;
    asset.relativePath = QStringLiteral("range.wav");
    asset.absolutePath = path;
    asset.contentHash = QStringLiteral("range-hash");
    asset.metadata.sampleRate = 1000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 1000;
    asset.metadata.loopInfo.enabled = true;
    asset.metadata.loopInfo.loopStart = 300;
    asset.metadata.loopInfo.loopEnd = 699;
    ASSERT_TRUE(assets.registerAsset(asset));

    VNX::ProjectItem* soundItem = VNX::ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(soundItem, nullptr);
    auto* sound = soundItem->comp<VNX::VSound>();
    ASSERT_NE(sound, nullptr);
    VNX::SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 100;
    source.trimEndFrame = 900;
    source.rangeContentHash = asset.contentHash;
    soundItem->sourceID = asset.sourceID;
    soundItem->sourcePath = asset.relativePath;

    for (const auto& loop : {VNX::SoundLoopRegion{300, 700}, VNX::SoundLoopRegion{100, 900}}) {
        source.loopRegion = loop;
        sound->sources.plain.entries = {source};
        VNX::SourceEditorWidget widget;
        widget.resize(720, 360);
        widget.bindSource(asset.sourceID, soundItem->guid, source.entryId);
        widget.show();
        QApplication::processEvents(QEventLoop::AllEvents);
        auto* waveform = widget.findChild<VNX::SourceEditorWaveformView*>(
            QStringLiteral("sourceEditorWaveform"));
        ASSERT_NE(waveform, nullptr);
        ASSERT_EQ(waveform->previewSource(), source);

        for (bool drag : {false, true}) {
            VNX::UndoRedoManager::instance().clear();
            const QPointF start = drag ? waveform->handleHitPoint(0) : waveform->waveformRect().center();
            const QPointF end = drag ? start + QPointF(40, 0) : start;
            QMouseEvent press(QEvent::MouseButtonPress, start, waveform->mapToGlobal(start),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(waveform, &press);
            QMouseEvent move(QEvent::MouseMove, end, waveform->mapToGlobal(end),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(waveform, &move);
            QMouseEvent release(QEvent::MouseButtonRelease, end, waveform->mapToGlobal(end),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(waveform, &release);
            QApplication::processEvents(QEventLoop::AllEvents);

            EXPECT_EQ(sound->sources.plain.entries.front(), source);
            EXPECT_EQ(waveform->previewSource(), source);
            EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 0);
        }
    }

    VNX::ProjectManager::instance().closeProject();
    assets.clear();
    VNX::ActionManager::instance().clearAllForTesting();
}

TEST_F(SourceEditorPanelTest, PanelRestoresSourceContextAndClearsMissingState)
{
    ensureApp();
    VNX::SourceEditorPanel panel;
    const QUuid soundGuid = QUuid::createUuid();
    const QUuid entryId = QUuid::createUuid();
    panel.bindSource(93, soundGuid, entryId);

    const QVariantMap state = panel.snapshotState();

    VNX::SourceEditorPanel restored;
    restored.restoreState(state);

    EXPECT_EQ(restored.sourceId(), 93u);
    EXPECT_EQ(restored.soundGuid(), soundGuid);
    EXPECT_EQ(restored.entryId(), entryId);
    restored.restoreState({});
    EXPECT_EQ(restored.sourceId(), 0u);
    EXPECT_TRUE(restored.soundGuid().isNull());
    EXPECT_TRUE(restored.entryId().isNull());
}

TEST_F(SourceEditorPanelTest, WidgetExtrapolatesALocalPlayheadWithoutEnginePosition)
{
    ensureApp();
    ASSERT_TRUE(ensureNullEngine());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = QDir(directory.path()).filePath(QStringLiteral("playhead.wav"));
    writeWavF32(path, std::vector<float>(1000, 0.25f), 1, 1000);
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();

    VNX::AssetInfo asset;
    asset.sourceID = 93;
    asset.absolutePath = path;
    asset.metadata.sampleRate = 1000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 1000;
    ASSERT_TRUE(assets.registerAsset(asset));

    VNX::SourceEditorWidget widget;
    EXPECT_FALSE(widget.startPreviewPlayhead());
    widget.bindSource(asset.sourceID);
    const auto startedAt = std::chrono::steady_clock::now();
    ASSERT_TRUE(widget.startPreviewPlayhead());
    EXPECT_TRUE(widget.isPreviewPlayheadActive());
    EXPECT_NE(widget.previewSoundId(), harkentest::StudioObjectId(0u));
    EXPECT_NE(widget.previewPlaybackHandle(), 0u);
    const auto frame = widget.previewPlayheadFrame();
    ASSERT_TRUE(frame.has_value());
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    EXPECT_LE(*frame, static_cast<uint64_t>(elapsedMs));

    auto* waveform = widget.findChild<VNX::SourceEditorWaveformView*>(
        QStringLiteral("sourceEditorWaveform"));
    ASSERT_NE(waveform, nullptr);
    EXPECT_GE(waveform->playheadPosition(), 0);

    widget.stopPreviewPlayhead();
    EXPECT_FALSE(widget.isPreviewPlayheadActive());
    EXPECT_EQ(widget.previewSoundId(), harkentest::StudioObjectId(0u));
    EXPECT_EQ(widget.previewPlaybackHandle(), 0u);
    EXPECT_FALSE(widget.previewPlayheadFrame());
    EXPECT_EQ(waveform->playheadPosition(), -1);
    widget.bindSource(0);
    EXPECT_FALSE(widget.isPreviewPlayheadActive());

    assets.clear();
}

TEST_F(SourceEditorPanelTest, WidgetPlayheadFollowsALoopWindowAndClearsOnRebind)
{
    ensureApp();
    ASSERT_TRUE(ensureNullEngine());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = QDir(directory.path()).filePath(QStringLiteral("loop-playhead.wav"));
    writeWavF32(path, std::vector<float>(1000, 0.25f), 1, 1000);
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();

    VNX::AssetInfo asset;
    asset.sourceID = 94;
    asset.absolutePath = path;
    asset.metadata.sampleRate = 1000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 1000;
    asset.metadata.loopInfo.enabled = true;
    asset.metadata.loopInfo.loopStart = 200;
    asset.metadata.loopInfo.loopEnd = 399;
    ASSERT_TRUE(assets.registerAsset(asset));

    VNX::SourceEditorWidget widget;
    widget.bindSource(asset.sourceID);
    ASSERT_TRUE(widget.startPreviewPlayhead(350));
    EXPECT_TRUE(widget.isPreviewPlayheadActive());
    EXPECT_NE(widget.previewPlaybackHandle(), 0u);
    EXPECT_EQ(widget.previewPlayheadFrame(), 350u);

    widget.bindSource(0);
    EXPECT_FALSE(widget.isPreviewPlayheadActive());
    EXPECT_EQ(widget.previewPlaybackHandle(), 0u);
    EXPECT_FALSE(widget.previewPlayheadFrame());

    assets.clear();
}

TEST_F(SourceEditorPanelTest, WaveformClickLocatesPreviewFromThatFrame)
{
    ensureApp();
    ASSERT_TRUE(ensureNullEngine());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = QDir(directory.path()).filePath(QStringLiteral("locate-click.wav"));
    writeWavF32(path, std::vector<float>(1000, 0.25f), 1, 1000);
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();

    VNX::AssetInfo asset;
    asset.sourceID = 103;
    asset.absolutePath = path;
    asset.metadata.sampleRate = 1000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 1000;
    ASSERT_TRUE(assets.registerAsset(asset));

    VNX::SourceEditorWidget widget;
    widget.resize(720, 360);
    widget.bindSource(asset.sourceID);
    widget.show();
    QApplication::processEvents(QEventLoop::AllEvents);
    auto* waveform = widget.findChild<VNX::SourceEditorWaveformView*>(
        QStringLiteral("sourceEditorWaveform"));
    ASSERT_NE(waveform, nullptr);

    const int x = waveform->handlePosition(0)
        + (waveform->handlePosition(3) - waveform->handlePosition(0)) / 2;
    const QPointF point(x, waveform->height() / 2.0);
    QMouseEvent press(QEvent::MouseButtonPress, point, waveform->mapToGlobal(point),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(waveform, &press);
    QApplication::processEvents(QEventLoop::AllEvents);

    EXPECT_TRUE(widget.isPreviewPlayheadActive());
    const auto frame = widget.previewPlayheadFrame();
    ASSERT_TRUE(frame);
    EXPECT_GE(*frame, 400u);
    EXPECT_LE(*frame, 600u);

    widget.stopPreviewPlayhead();
    assets.clear();
}

TEST_F(SourceEditorPanelTest, WidgetRegistersRawPlaybackWithAuditionManager)
{
    ensureApp();
    ASSERT_TRUE(ensureNullEngine());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = QDir(directory.path()).filePath(QStringLiteral("ledger.wav"));
    writeWavF32(path, std::vector<float>(1000, 0.25f), 1, 1000);
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();

    VNX::AssetInfo asset;
    asset.sourceID = 96;
    asset.absolutePath = path;
    asset.relativePath = QStringLiteral("ledger.wav");
    asset.metadata.sampleRate = 1000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 1000;
    ASSERT_TRUE(assets.registerAsset(asset));

    HarkenEditor::AuditionManager audition(&VNX::AudioEngine::instance());
    VNX::SourceEditorWidget widget;
    widget.setAuditionManager(&audition);
    widget.bindSource(asset.sourceID);

    ASSERT_TRUE(widget.startPreviewPlayhead());
    EXPECT_TRUE(audition.hasActiveAudition());
    EXPECT_EQ(audition.activeAuditionCount(), 1);
    const uint32_t playingId = widget.auditionPlayingId();
    ASSERT_NE(playingId, 0u);
    const auto started = audition.getAudition(playingId);
    EXPECT_EQ(started.mode, HarkenEditor::AuditionMode::RawFile);
    EXPECT_EQ(started.playbackHandle, static_cast<quintptr>(widget.previewPlaybackHandle()));
    EXPECT_TRUE(started.bIsPlaying);

    widget.stopPreviewPlayhead();
    EXPECT_FALSE(widget.isPreviewPlayheadActive());
    EXPECT_EQ(widget.auditionPlayingId(), 0u);
    EXPECT_FALSE(audition.hasActiveAudition());
    EXPECT_EQ(audition.getAudition(playingId).playingId, 0u);

    ASSERT_TRUE(widget.startPreviewPlayhead());
    const uint32_t secondId = widget.auditionPlayingId();
    ASSERT_NE(secondId, 0u);
    audition.stop(secondId);
    EXPECT_FALSE(widget.isPreviewPlayheadActive());
    EXPECT_EQ(widget.previewPlaybackHandle(), 0u);
    EXPECT_EQ(widget.auditionPlayingId(), 0u);
    EXPECT_FALSE(audition.hasActiveAudition());

    assets.clear();
}

TEST_F(SourceEditorPanelTest, WidgetRollsBackRawPlaybackWhenAuditionIsFull)
{
    ensureApp();
    ASSERT_TRUE(ensureNullEngine());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();

    VNX::AssetInfo asset;
    asset.sourceID = 103;
    asset.absolutePath = QDir(directory.path()).filePath(QStringLiteral("full-ledger.wav"));
    asset.relativePath = QStringLiteral("full-ledger.wav");
    asset.metadata.sampleRate = 1000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 1000;
    writeWavF32(asset.absolutePath, std::vector<float>(1000, 0.25f), 1, 1000);
    ASSERT_TRUE(assets.registerAsset(asset));

    HarkenEditor::AuditionManager audition(&VNX::AudioEngine::instance());
    uint32_t firstFillerId = 0;
    for (int i = 0; i < HarkenEditor::AuditionManager::MAX_AUDITIONS; ++i) {
        const uint32_t fillerId = audition.registerRawFilePlayback(
            QStringLiteral("filler/%1").arg(i),
            static_cast<quintptr>(2000 + i));
        ASSERT_NE(fillerId, 0u);
        if (i == 0)
            firstFillerId = fillerId;
    }
    EXPECT_EQ(audition.activeAuditionCount(),
              HarkenEditor::AuditionManager::MAX_AUDITIONS);

    VNX::SourceEditorWidget widget;
    widget.setAuditionManager(&audition);
    widget.bindSource(asset.sourceID);

    EXPECT_FALSE(widget.startPreviewPlayhead());
    EXPECT_FALSE(widget.isPreviewPlayheadActive());
    EXPECT_EQ(widget.previewPlaybackHandle(), 0u);
    EXPECT_EQ(widget.previewSoundId(), harkentest::StudioObjectId(0u));
    EXPECT_EQ(widget.auditionPlayingId(), 0u);
    EXPECT_EQ(audition.activeAuditionCount(),
              HarkenEditor::AuditionManager::MAX_AUDITIONS);
    EXPECT_EQ(audition.getAuditionForObject(asset.relativePath), 0u);

    audition.stop(firstFillerId);
    ASSERT_TRUE(widget.startPreviewPlayhead());
    EXPECT_TRUE(widget.isPreviewPlayheadActive());
    EXPECT_NE(widget.previewPlaybackHandle(), 0u);
    EXPECT_NE(widget.auditionPlayingId(), 0u);
    EXPECT_EQ(audition.activeAuditionCount(),
              HarkenEditor::AuditionManager::MAX_AUDITIONS);

    widget.stopPreviewPlayhead();
    assets.clear();
}

TEST_F(SourceEditorPanelTest, StopAllLeavesRawFilePlaybackRunning)
{
    ensureApp();
    ASSERT_TRUE(ensureNullEngine());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();

    VNX::AssetInfo asset;
    asset.sourceID = 99;
    asset.absolutePath = QDir(directory.path()).filePath(QStringLiteral("keep-raw.wav"));
    asset.relativePath = QStringLiteral("keep-raw.wav");
    asset.metadata.sampleRate = 1000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 1000;
    writeWavF32(asset.absolutePath, std::vector<float>(1000, 0.25f), 1, 1000);
    ASSERT_TRUE(assets.registerAsset(asset));

    HarkenEditor::AuditionManager audition(&VNX::AudioEngine::instance());
    VNX::SourceEditorWidget widget;
    widget.setAuditionManager(&audition);
    widget.bindSource(asset.sourceID);
    ASSERT_TRUE(widget.startPreviewPlayhead());
    const uint32_t rawId = widget.auditionPlayingId();
    ASSERT_NE(rawId, 0u);

    const uint32_t eventId = audition.registerExternalPlayback(
        QStringLiteral("event"), 4242);
    ASSERT_NE(eventId, 0u);
    EXPECT_EQ(audition.activeAuditionCount(), 2);

    audition.stopAll();

    EXPECT_TRUE(widget.isPreviewPlayheadActive());
    EXPECT_EQ(widget.auditionPlayingId(), rawId);
    EXPECT_NE(widget.previewPlaybackHandle(), 0u);
    EXPECT_EQ(audition.getAudition(rawId).mode, HarkenEditor::AuditionMode::RawFile);
    EXPECT_EQ(audition.getAudition(eventId).playingId, 0u);
    EXPECT_EQ(audition.activeAuditionCount(), 1);
    EXPECT_TRUE(audition.hasActiveAudition());
    EXPECT_FALSE(audition.hasActiveNonRawAudition());

    widget.stopPreviewPlayhead();
    assets.clear();
}

TEST_F(SourceEditorPanelTest, WidgetResolvesFileDefaultPcmSlices)
{
    ensureApp();
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();

    VNX::AssetInfo asset;
    asset.sourceID = 91;
    asset.absolutePath = QStringLiteral("slice.wav");
    asset.metadata.sampleRate = 48000;
    asset.metadata.channels = 2;
    asset.metadata.totalSamples = 1000;
    asset.metadata.loopInfo.enabled = true;
    asset.metadata.loopInfo.loopStart = 200;
    asset.metadata.loopInfo.loopEnd = 599;
    ASSERT_TRUE(assets.registerAsset(asset));

    VNX::SourceEditorWidget widget;
    EXPECT_FALSE(widget.previewPcmSlice());
    widget.bindSource(asset.sourceID);
    const auto fileSlice = widget.previewPcmSlice();
    ASSERT_TRUE(fileSlice);
    EXPECT_EQ(fileSlice->startFrame, 200u);
    EXPECT_EQ(fileSlice->frameCount, 400u);
    EXPECT_EQ(fileSlice->windowStartFrame, 200u);
    EXPECT_EQ(fileSlice->windowFrameCount, 400u);
    EXPECT_TRUE(fileSlice->loops);
    EXPECT_EQ(fileSlice->channels, 2u);
    EXPECT_EQ(fileSlice->sampleOffset(), 400u);

    const auto fromLater = widget.previewPcmSlice(350);
    ASSERT_TRUE(fromLater);
    EXPECT_EQ(fromLater->startFrame, 350u);
    EXPECT_EQ(fromLater->frameCount, 400u);
    EXPECT_TRUE(fromLater->loops);
    const auto loopSlice = widget.previewLoopPcmSlice(350);
    ASSERT_TRUE(loopSlice);
    EXPECT_EQ(loopSlice->windowFrameCount, 400u);
    EXPECT_FALSE(widget.previewPcmSlice(600));

    assets.clear();
}

TEST_F(SourceEditorPanelTest, SpaceOnFocusedEditorTogglesOnlyThatRawPlayback)
{
    ensureApp();
    ASSERT_TRUE(ensureNullEngine());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();

    VNX::AssetInfo firstAsset;
    firstAsset.sourceID = 101;
    firstAsset.absolutePath = QDir(directory.path()).filePath(QStringLiteral("focus-one.wav"));
    firstAsset.relativePath = QStringLiteral("focus-one.wav");
    firstAsset.metadata.sampleRate = 1000;
    firstAsset.metadata.channels = 1;
    firstAsset.metadata.totalSamples = 1000;
    writeWavF32(firstAsset.absolutePath, std::vector<float>(1000, 0.25f), 1, 1000);
    ASSERT_TRUE(assets.registerAsset(firstAsset));

    VNX::AssetInfo secondAsset;
    secondAsset.sourceID = 102;
    secondAsset.absolutePath = QDir(directory.path()).filePath(QStringLiteral("focus-two.wav"));
    secondAsset.relativePath = QStringLiteral("focus-two.wav");
    secondAsset.metadata.sampleRate = 1000;
    secondAsset.metadata.channels = 1;
    secondAsset.metadata.totalSamples = 1000;
    writeWavF32(secondAsset.absolutePath, std::vector<float>(1000, 0.25f), 1, 1000);
    ASSERT_TRUE(assets.registerAsset(secondAsset));

    HarkenEditor::AuditionManager audition(&VNX::AudioEngine::instance());
    VNX::SourceEditorPanel first;
    VNX::SourceEditorPanel second;
    first.setAuditionManager(&audition);
    second.setAuditionManager(&audition);
    first.bindSource(firstAsset.sourceID);
    second.bindSource(secondAsset.sourceID);

    ASSERT_TRUE(first.handlePlayStopShortcut());
    EXPECT_TRUE(first.isPreviewPlayheadActive());
    EXPECT_FALSE(second.isPreviewPlayheadActive());
    EXPECT_TRUE(audition.hasActiveAudition());
    EXPECT_EQ(audition.activeAuditionCount(), 1);

    ASSERT_TRUE(second.handlePlayStopShortcut());
    EXPECT_TRUE(first.isPreviewPlayheadActive());
    EXPECT_TRUE(second.isPreviewPlayheadActive());
    EXPECT_EQ(audition.activeAuditionCount(), 2);

    EXPECT_TRUE(first.handlePlayStopShortcut());
    EXPECT_FALSE(first.isPreviewPlayheadActive());
    EXPECT_TRUE(second.isPreviewPlayheadActive());
    EXPECT_EQ(audition.activeAuditionCount(), 1);
    EXPECT_TRUE(audition.hasActiveAudition());

    EXPECT_TRUE(second.handlePlayStopShortcut());
    EXPECT_FALSE(audition.hasActiveAudition());

    assets.clear();
}

TEST_F(SourceEditorPanelTest, ReferenceLoopSliceRegistersTheLoopWindow)
{
    ensureApp();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("SourceEditorLoopSliceProject");
    config.location = directory.path();
    ASSERT_TRUE(VNX::ProjectManager::instance().createProject(config));

    auto& assets = VNX::AssetRegistry::instance();
    VNX::AssetInfo asset;
    asset.sourceID = 92;
    asset.absolutePath = QDir(directory.path()).filePath(QStringLiteral("loop-slice.wav"));
    asset.metadata.sampleRate = 1000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 1000;
    ASSERT_TRUE(assets.registerAsset(asset));

    VNX::ProjectItem* soundItem = VNX::ProjectManager::instance().createItem(
        nullptr, QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(soundItem, nullptr);
    auto* sound = soundItem->comp<VNX::VSound>();
    ASSERT_NE(sound, nullptr);
    VNX::SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 50;
    source.trimEndFrame = 800;
    source.loopRegion = VNX::SoundLoopRegion{200, 500};
    sound->sources.plain.entries = {source};

    VNX::SourceEditorWidget widget;
    widget.bindSource(asset.sourceID, soundItem->guid, source.entryId);

    const auto looped = widget.previewPcmSlice();
    ASSERT_TRUE(looped);
    EXPECT_EQ(looped->startFrame, 200u);
    EXPECT_EQ(looped->frameCount, 300u);
    EXPECT_EQ(looped->windowStartFrame, 200u);
    EXPECT_EQ(looped->windowFrameCount, 300u);
    EXPECT_TRUE(looped->loops);

    const auto fromLater = widget.previewLoopPcmSlice(350);
    ASSERT_TRUE(fromLater);
    EXPECT_EQ(fromLater->startFrame, 350u);
    EXPECT_EQ(fromLater->frameCount, 300u);
    EXPECT_TRUE(fromLater->loops);
    EXPECT_FALSE(widget.previewLoopPcmSlice(500));

    source.loopRegion.reset();
    sound->sources.plain.entries.front() = source;
    widget.bindSource(asset.sourceID, soundItem->guid, source.entryId);
    EXPECT_FALSE(widget.previewLoopPcmSlice());
    const auto trimmed = widget.previewPcmSlice(100);
    ASSERT_TRUE(trimmed);
    EXPECT_EQ(trimmed->startFrame, 100u);
    EXPECT_EQ(trimmed->frameCount, 700u);
    EXPECT_FALSE(trimmed->loops);

    VNX::ProjectManager::instance().closeProject();
    assets.clear();
}
