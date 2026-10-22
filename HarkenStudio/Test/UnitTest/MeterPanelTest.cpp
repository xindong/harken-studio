#include "Engine.h"
#include "Core/AudioEngine.h"
#include "Widgets/Meter/MeterPanel.h"
#include "Widgets/Meter/MeterBallistics.h"
#include "Widgets/Common/AmplitudeAxis.h"
#include "MonitorCaptureHubTestHooks.h"
#include "MonitorCaptureHub.h"
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/TestUtil.h"
#include "RuntimeBridge/PlaybackBridge.h"
#include "RuntimeBridge/RuntimeLink.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QThread>
#include <QTimer>
#include <QScopeGuard>
#include <QtEndian>

#include <limits>
#include <algorithm>
#include <bit>
#include <cmath>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void refresh(VNX::MeterPanel& panel)
{
    ASSERT_TRUE(QMetaObject::invokeMethod(&panel, "refresh", Qt::DirectConnection));
}

}

TEST(MeterBridge, MeasurementDomainsKeepSilenceDistinctFromMissingData)
{
    VNX::Monitor::MonitorRecordHeader header{};
    header.recordKind = VN_MONITOR_RECORD_METER;
    header.recordVersion = 1;
    header.streamKind = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    header.validityFlags = VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC;
    std::array<uint8_t, sizeof(VnMonitorMeterPayload)> payload{};
    const auto word = [&](size_t offset, uint32_t value) {
        qToLittleEndian(value, payload.data() + offset);
    };
    word(offsetof(VnMonitorMeterPayload, structSize), payload.size());
    word(offsetof(VnMonitorMeterPayload, algorithmVersion), 1);
    word(offsetof(VnMonitorMeterPayload, rmsUnitId), 1);
    word(offsetof(VnMonitorMeterPayload, lufsSupported), 1);
    for (bool lufs : {false, true}) {
        word(offsetof(VnMonitorMeterPayload, algorithmId),
            lufs ? VN_MONITOR_MEASURE_LUFS_MOMENTARY : VN_MONITOR_MEASURE_RMS);
        const auto offset = lufs ? offsetof(VnMonitorMeterPayload, lufsMomentaryBits)
            : offsetof(VnMonitorMeterPayload, rmsBits);
        for (float value : lufs ? std::vector<float>{-23.0f, 3.0f, -std::numeric_limits<float>::infinity()}
                               : std::vector<float>{0.5f, 2.0f, 0.0f}) {
            word(offset, std::bit_cast<uint32_t>(value));
            HarkenEditor::Bridge::MeterMeasurementFrame frame;
            frame.rmsAvailable = frame.lufsAvailable = true;
            frame.spectrumDb = {-1.0f};
            ASSERT_TRUE(HarkenEditor::Bridge::decodeMeterMeasurement(header, payload, frame));
            EXPECT_EQ(frame.rmsAvailable, !lufs);
            EXPECT_EQ(frame.lufsAvailable, lufs);
            EXPECT_FLOAT_EQ(lufs ? frame.lufs : frame.rms, value);
            EXPECT_TRUE(frame.spectrumDb.empty());
            EXPECT_FALSE(frame.invalidated);
        }
        for (float value : {std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity()}) {
            word(offset, std::bit_cast<uint32_t>(value));
            HarkenEditor::Bridge::MeterMeasurementFrame frame;
            EXPECT_FALSE(HarkenEditor::Bridge::decodeMeterMeasurement(header, payload, frame));
        }
        word(offset, std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN()));
        for (uint32_t validity : {uint32_t(VN_MONITOR_VALID_ARMED),
                uint32_t(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_WARMUP),
                uint32_t(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC | VN_MONITOR_VALID_UNSUPPORTED)}) {
            auto unavailable = header;
            unavailable.validityFlags = validity;
            HarkenEditor::Bridge::MeterMeasurementFrame frame;
            frame.rmsAvailable = frame.lufsAvailable = true;
            ASSERT_TRUE(HarkenEditor::Bridge::decodeMeterMeasurement(unavailable, payload, frame));
            EXPECT_TRUE(frame.unavailable);
            EXPECT_FALSE(frame.invalidated);
            EXPECT_FALSE(frame.rmsAvailable);
            EXPECT_FALSE(frame.lufsAvailable);
        }
    }
    auto unknown = header;
    unknown.validityFlags = VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_WARMUP;
    word(offsetof(VnMonitorMeterPayload, algorithmId), 999);
    HarkenEditor::Bridge::MeterMeasurementFrame rejected;
    EXPECT_FALSE(HarkenEditor::Bridge::decodeMeterMeasurement(unknown, payload, rejected));
    EXPECT_FALSE(HarkenEditor::Bridge::decodeMeterMeasurement(unknown, {}, rejected));
    for (auto kind : {VN_MONITOR_RECORD_TAP_BINDING_CHANGED, VN_MONITOR_RECORD_MEASUREMENT_SEGMENT_RESET}) {
        header.recordKind = kind;
        HarkenEditor::Bridge::MeterMeasurementFrame frame;
        frame.rmsAvailable = frame.lufsAvailable = true;
        ASSERT_TRUE(HarkenEditor::Bridge::decodeMeterMeasurement(header, {}, frame));
        EXPECT_TRUE(frame.invalidated);
        EXPECT_FALSE(frame.rmsAvailable);
        EXPECT_FALSE(frame.lufsAvailable);
    }
}

TEST(MeterPanel, StereoPeaksRemainIndependentWhilePressingThePlot)
{
    ensureApp();
    VNX::MeterPanel panel;
    panel.resize(240, 360);
    panel.show();
    panel.acceptPeak(1.25f, 0.25f);
    auto* canvas = panel.findChild<QWidget*>(QStringLiteral("meterCanvas"));
    const QPoint point = panel.plotBounds().center();
    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonDblClick}) {
        QMouseEvent press(type, point, canvas->mapToGlobal(point),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &press);
        panel.acceptPeak(0.125f, 0.5f);
        refresh(panel);
        EXPECT_NEAR(panel.levelDb(0), -18.0618f, 0.001f);
        EXPECT_NEAR(panel.levelDb(1), -6.0206f, 0.001f);
        EXPECT_NEAR(panel.heldPeakDb(0), 1.9382f, 0.001f);
        EXPECT_NEAR(panel.heldPeakDb(1), -6.0206f, 0.001f);
        EXPECT_TRUE(panel.clipped(0));
        EXPECT_FALSE(panel.clipped(1));
    }
    panel.resetPeakHold();
    for (int channel : {0, 1}) {
        EXPECT_FALSE(panel.clipped(channel));
        EXPECT_FLOAT_EQ(panel.heldPeakDb(channel), panel.levelDb(channel));
    }
}

TEST(MeterBridge, DisplayWindowIsSharedAndAuxiliaryUpdatesDoNotConsumePeaks)
{
    ensureApp();
    VNX::AudioEngine::instance().shutdown();
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    config.bufferSize = 128;
    auto session = HarkenEditor::Bridge::EngineSession::create(config);
    ASSERT_NE(nullptr, session);
    ASSERT_EQ(HarkenEditor::Bridge::kEngineOk, session->startOutput());
    const auto busId = harkentest::StudioObjectId(8302);
    const auto nativeBusId = harkentest::ToRuntime(busId);
    ASSERT_EQ(VN_OK, Vn_CreateBus(nativeBusId, VN_BUS_MASTER));
    std::vector<float> samples(128 * 8 * 2, 0.05f);
    for (std::size_t frame = 0; frame < 128; ++frame) {
        samples[2 * frame] = 0.8f;
        samples[2 * frame + 1] = 0.2f;
    }
    const auto pcm = harkentest::InterleavedDesc(samples, 2, 48000);
    const auto soundId = harkentest::ToRuntime(harkentest::StudioObjectId(8303));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(soundId, &pcm));
    auto play = harkentest::MakePlayDesc(soundId);
    play.busId = nativeBusId;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    session->pumpNullMixer(2);
    HarkenEditor::Bridge::Detail::runtimeUpdate();
    HarkenEditor::Bridge::Detail::runtimeUpdate();
    session->update();
    session->update();
    session->publishMeterPeaks();
    for (int reader = 0; reader < 3; ++reader) {
        float left = 0.0f, right = 0.0f;
        session->masterMeterPeak(left, right);
        EXPECT_NEAR(0.8f, left, 1e-5f);
        EXPECT_NEAR(0.2f, right, 1e-5f);
        ASSERT_TRUE(session->busMeterPeakById(busId, left, right));
        EXPECT_NEAR(0.8f, left, 1e-5f);
        EXPECT_NEAR(0.2f, right, 1e-5f);
    }
    session->pumpNullMixer(1);
    float left = 0.0f, right = 0.0f;
    session->masterMeterPeak(left, right);
    EXPECT_NEAR(0.8f, left, 1e-5f);
    session->update();
    session->publishMeterPeaks();
    session->masterMeterPeak(left, right);
    EXPECT_NEAR(0.05f, left, 1e-5f);
    EXPECT_NEAR(0.05f, right, 1e-5f);
    session->update();
    session->publishMeterPeaks();
    ASSERT_TRUE(session->busMeterPeakById(busId, left, right));
    EXPECT_NEAR(0.05f, left, 1e-5f);
    EXPECT_NEAR(0.05f, right, 1e-5f);
    session->pumpNullMixer(1);
    session->stopAll(0);
    session->update();
    session->update();
    session->publishMeterPeaks();
    session->masterMeterPeak(left, right);
    EXPECT_NEAR(0.05f, left, 1e-5f);
    session->publishMeterPeaks();
    session->masterMeterPeak(left, right);
    EXPECT_NEAR(0.05f, left, 1e-5f);
    EXPECT_NEAR(0.05f, right, 1e-5f);
    session->stopOutput();
    session->masterMeterPeak(left, right);
    EXPECT_FLOAT_EQ(0.0f, left);
    EXPECT_FLOAT_EQ(0.0f, right);
    ASSERT_EQ(HarkenEditor::Bridge::kEngineOk, session->startOutput());
    session->pumpNullMixer(1);
    session->update();
    session->publishMeterPeaks();
    session->masterMeterPeak(left, right);
    EXPECT_FLOAT_EQ(0.0f, left);
    EXPECT_FLOAT_EQ(0.0f, right);
    std::fill(samples.begin(), samples.end(), 0.02f);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(soundId, &pcm));
    play.busId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    session->pumpNullMixer(2);
    session->update();
    session->publishMeterPeaks();
    session->masterMeterPeak(left, right);
    EXPECT_NEAR(0.02f, left, 1e-5f);
    EXPECT_NEAR(0.02f, right, 1e-5f);
}

TEST(MeterPanel, DisplayPublicationsPreservePeaksAcrossServiceUpdatesAndSharedReaders)
{
    ensureApp();
    auto& engine = VNX::AudioEngine::instance();
    engine.shutdown();
    const auto shutdown = qScopeGuard([&] { engine.shutdown(); });
    HarkenEditor::Bridge::EngineSessionConfig config;
    config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    config.sampleRate = 48000;
    config.bufferSize = 128;
    ASSERT_TRUE(engine.initialize(config));
    ASSERT_FALSE(engine.isSimulationMode());
    ASSERT_TRUE(engine.startOutput());
    for (auto* timer : engine.findChildren<QTimer*>())
        timer->stop();
    VNX::MeterPanel first;
    VNX::MeterPanel second;
    first.show();
    second.show();
    for (auto* panel : {&first, &second}) {
        for (auto* timer : panel->findChildren<QTimer*>())
            timer->stop();
    }
    auto* runtime = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, runtime);
    std::vector<float> samples(128 * 8 * 2, 0.05f);
    for (std::size_t frame = 0; frame < 128; ++frame) {
        samples[2 * frame] = 0.8f;
        samples[2 * frame + 1] = 0.2f;
    }
    const auto pcm = harkentest::InterleavedDesc(samples, 2, 48000);
    const auto soundId = harkentest::ToRuntime(harkentest::StudioObjectId(8304));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(soundId, &pcm));
    auto play = harkentest::MakePlayDesc(soundId);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    runtime->nullBackend.Pump(2);
    for (int step = 0; step < 3; ++step) {
        ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        runtime->nullBackend.Pump(1);
    }
    const auto expectLevels = [&](float left, float right) {
        for (auto* panel : {&first, &second}) {
            EXPECT_NEAR(left, panel->levelDb(0), 0.001f);
            EXPECT_NEAR(right, panel->levelDb(1), 0.001f);
        }
    };
    expectLevels(-96.0f, -96.0f);
    ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "onMeterTimer", Qt::DirectConnection));
    expectLevels(20.0f * std::log10(0.8f), 20.0f * std::log10(0.2f));
    for (int step = 0; step < 3; ++step) {
        ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection));
        ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "onMeterTimer", Qt::DirectConnection));
        expectLevels(20.0f * std::log10(0.8f), 20.0f * std::log10(0.2f));
    }
    runtime->nullBackend.Pump(1);
    ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "onMeterTimer", Qt::DirectConnection));
    expectLevels(20.0f * std::log10(0.05f), 20.0f * std::log10(0.05f));
    engine.stopAll(0.0f);
    ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection));
    runtime->nullBackend.Pump(2);
    ASSERT_TRUE(QMetaObject::invokeMethod(&engine, "onMeterTimer", Qt::DirectConnection));
    expectLevels(-96.0f, -96.0f);
}

TEST(MeterSource, CaptureChangesAndTapRetirementStayIsolatedToTheirConsumers)
{
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);
    const auto terminate = qScopeGuard([] { Vn_Term(); });
    const auto bus = harkentest::StudioObjectId(8301);
    ASSERT_EQ(Vn_CreateBus(harkentest::ToRuntime(bus), VN_BUS_MASTER), VN_OK);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::detail::ActiveEngine()->nullBackend.Pump(1);
    VNX::MeterSource first;
    VNX::MeterSource second;
    VNX::MeterSource otherBus;
    const auto spectrum = HarkenEditor::Bridge::MeterMeasurement::Spectrum;
    ASSERT_TRUE(first.start(harkenstudio::kMasterBusObjectId, spectrum)) << first.result();
    ASSERT_TRUE(second.start(harkenstudio::kMasterBusObjectId, spectrum)) << second.result();
    first.poll();
    second.poll();
    harken::monitor::AdvanceExecuteFrameForTests(1);
    uint64_t sourceFrame = 0;
    const auto publish = [&](bool includeOtherBus) {
        auto* runtime = harken::detail::ActiveEngine();
        for (bool master : {true, false}) {
            if (!master && !includeOtherBus)
                continue;
            std::array<float, 2048> samples;
            const float amplitude = master ? 0.5f : 0.0625f;
            for (size_t index = 0; index < samples.size(); ++index)
                samples[index] = amplitude * std::sin(2.0 * 3.141592653589793 * 64 * index / samples.size());
            float* planes[] = {samples.data(), samples.data()};
            const uint32_t busIndex = runtime->buses.FindIndex(master ? VN_BUS_MASTER : harkentest::ToRuntime(bus));
            harken::monitor::PublishPlanarAudioChunk(master ? VN_MONITOR_TAP_MASTER_OUTPUT : VN_MONITOR_TAP_BUS_POST_EFFECTS,
                busIndex + 1u, runtime->buses.GenerationAt(busIndex), 0, planes, samples.size(), 2, 0,
                VN_CHANNEL_LAYOUT_STEREO, 48000, sourceFrame);
        }
        sourceFrame += 2048;
        harken::monitor::DrainOnceForTests();
    };
    const auto receive = [](VNX::MeterSource& source, float expected) {
        std::vector<HarkenEditor::Bridge::MeterMeasurementFrame> frames;
        for (int attempt = 0; attempt < 100 && frames.empty(); ++attempt) {
            auto update = source.poll();
            EXPECT_FALSE(update.interrupted);
            frames = std::move(update.frames);
            if (frames.empty())
                QThread::msleep(5);
        }
        ASSERT_FALSE(frames.empty());
        for (const auto& frame : frames) {
            ASSERT_FALSE(frame.spectrumDb.empty());
            EXPECT_NEAR(*std::max_element(frame.spectrumDb.begin(), frame.spectrumDb.end()), expected, 0.1f);
        }
    };
    publish(false);
    receive(first, -6.0206f);
    ASSERT_TRUE(otherBus.start(bus, spectrum)) << otherBus.result();
    receive(second, -6.0206f);
    otherBus.poll();
    harken::monitor::AdvanceExecuteFrameForTests(1);
    publish(true);
    receive(first, -6.0206f);
    receive(second, -6.0206f);
    receive(otherBus, -24.0824f);
    first.stop();
    otherBus.stop();
    EXPECT_FALSE(second.poll().interrupted);
    harken::monitor::AdvanceExecuteFrameForTests(1);
    publish(false);
    receive(second, -6.0206f);
    ASSERT_TRUE(otherBus.start(bus, spectrum));
    otherBus.stop();
    harken::monitor::AdvanceExecuteFrameForTests(1);
    EXPECT_FALSE(second.poll().interrupted);
    harken::monitor::AdvanceExecuteFrameForTests(1);
    publish(false);
    receive(second, -6.0206f);

    ASSERT_TRUE(otherBus.start(bus, spectrum));
    otherBus.poll();
    harken::monitor::AdvanceExecuteFrameForTests(1);
    publish(true);
    receive(otherBus, -24.0824f);
    const auto* runtime = harken::detail::ActiveEngine();
    const auto busIndex = runtime->buses.FindIndex(harkentest::ToRuntime(bus));
    harken::monitor::NoteExactInstanceRetired(VN_MONITOR_TAP_BUS_POST_EFFECTS,
        busIndex + 1u, runtime->buses.GenerationAt(busIndex));
    for (int attempt = 0; attempt < 100 && otherBus.active(); ++attempt) {
        EXPECT_FALSE(second.poll().interrupted);
        if (otherBus.active())
            QThread::msleep(5);
    }
    EXPECT_FALSE(otherBus.active());
    EXPECT_TRUE(otherBus.poll().interrupted);
    EXPECT_TRUE(second.active());
    EXPECT_FALSE(second.poll().interrupted);
    harken::monitor::AdvanceExecuteFrameForTests(1);
    publish(false);
    receive(second, -6.0206f);
}

TEST(MeterPanel, RejectsInvalidSamplesAndClearsOnOutputStopAndHide)
{
    ensureApp();
    VNX::MeterPanel panel;
    panel.show();
    auto& engine = VNX::AudioEngine::instance();
    panel.acceptPeak(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN());
    panel.acceptPeak(std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
    panel.acceptPeak(-1.0f, -1.0f);
    refresh(panel);
    EXPECT_FLOAT_EQ(panel.levelDb(), -96.0f);
    EXPECT_FALSE(panel.clipped());
    panel.acceptPeak(1.0f, 1.0f);
    refresh(panel);
    EXPECT_TRUE(panel.clipped());
    engine.playingChanged(false);
    EXPECT_FLOAT_EQ(panel.levelDb(), -96.0f);
    EXPECT_FALSE(panel.clipped());
    panel.hide();
    panel.acceptPeak(2.0f, 2.0f);
    panel.show();
    refresh(panel);
    EXPECT_FLOAT_EQ(panel.levelDb(), -96.0f);
    EXPECT_FALSE(panel.clipped());
}

TEST(MeterPanel, ModeChangesPreserveSpectrumHistoryAndResetMeasurements)
{
    ensureApp();
    VNX::MeterPanel panel;
    panel.show();
    panel.setMode(VNX::MeterPanel::Mode::Spectrum);
    HarkenEditor::Bridge::MeterMeasurementFrame frame;
    frame.fftSize = 2048;
    frame.sampleRate = 48000;
    frame.spectrumDb.assign(1025, -18.0f);
    panel.acceptMeasurement(frame);
    const auto levels = panel.spectrumLevels();
    panel.setMode(VNX::MeterPanel::Mode::Spectrogram);
    EXPECT_TRUE(panel.measurementAvailable());
    EXPECT_EQ(panel.spectrumLevels(), levels);
    EXPECT_EQ(panel.historyColumns(), 1);
    panel.setMode(VNX::MeterPanel::Mode::Loudness);
    EXPECT_FALSE(panel.measurementAvailable());
    EXPECT_EQ(panel.historyColumns(), 0);
    panel.acceptMeasurement(frame);
    EXPECT_FALSE(panel.measurementAvailable());
    frame = {};
    frame.rmsAvailable = true;
    frame.rms = 0.5f;
    panel.acceptMeasurement(frame);
    ASSERT_TRUE(panel.measurementAvailable());
    EXPECT_NEAR(panel.loudnessDb(), -6.0206f, 0.001f);
    panel.setLoudnessMeasurement(VNX::MeterPanel::LoudnessMeasurement::Lufs);
    EXPECT_FALSE(panel.measurementAvailable());
    panel.acceptMeasurement(frame);
    EXPECT_FALSE(panel.measurementAvailable());
    frame = {};
    frame.lufsAvailable = true;
    frame.lufs = -23.0f;
    panel.acceptMeasurement(frame);
    ASSERT_TRUE(panel.measurementAvailable());
    EXPECT_FLOAT_EQ(panel.loudnessDb(), -23.0f);
    frame.lufs = -std::numeric_limits<float>::infinity();
    panel.acceptMeasurement(frame);
    EXPECT_TRUE(panel.measurementAvailable());
    EXPECT_EQ(panel.loudnessDb(), -std::numeric_limits<float>::infinity());
    panel.setMode(VNX::MeterPanel::Mode::Peak);
    EXPECT_FALSE(panel.measurementAvailable());
    EXPECT_EQ(panel.historyColumns(), 0);
}

TEST(MeterPanel, SpectrumDistinguishesMissingFramesFromSilenceWhilePreservingPeaks)
{
    ensureApp();
    VNX::MeterPanel panel;
    panel.setMode(VNX::MeterPanel::Mode::Spectrum);
    panel.show();
    HarkenEditor::Bridge::MeterMeasurementFrame frame;
    frame.fftSize = 2048;
    frame.sampleRate = 16000;
    for (float db : {-6.0f, -36.0f, -18.0f, -54.0f}) {
        frame.spectrumDb.assign(1025, db);
        panel.acceptMeasurement(frame);
        for (float level : panel.spectrumLevels())
            EXPECT_NEAR(level, std::pow(10.0f, db / 20.0f), 0.00001f);
    }
    for (int update = 0; update < 3; ++update) {
        refresh(panel);
        for (float level : panel.spectrumLevels())
            EXPECT_NEAR(level, std::pow(10.0f, -54.0f / 20.0f), 0.00001f);
        EXPECT_EQ(panel.historyColumns(), 4);
    }
    frame.spectrumDb.assign(1025, -96.0f);
    panel.acceptMeasurement(frame);
    refresh(panel);
    for (float level : panel.spectrumLevels())
        EXPECT_FLOAT_EQ(level, 0.0f);
    for (float peak : panel.spectrumPeaks())
        EXPECT_NEAR(peak, std::pow(10.0f, -6.0f / 20.0f), 0.00001f);
    EXPECT_EQ(panel.historyColumns(), 5);
}

TEST(MeterPanel, NewlyReceivedPeaksKeepTheirFullHoldAfterAnIdleRefreshInterval)
{
    ensureApp();
    VNX::MeterPanel peakPanel;
    VNX::MeterPanel spectrumPanel;
    VNX::MeterPanel rmsPanel;
    VNX::MeterPanel lufsPanel;
    lufsPanel.setLoudnessMeasurement(VNX::MeterPanel::LoudnessMeasurement::Lufs);
    const std::array loudnessPanels{&rmsPanel, &lufsPanel};
    for (auto* panel : loudnessPanels) {
        panel->setMode(VNX::MeterPanel::Mode::Loudness);
        panel->show();
        refresh(*panel);
    }
    spectrumPanel.setMode(VNX::MeterPanel::Mode::Spectrum);
    peakPanel.show();
    spectrumPanel.show();
    refresh(peakPanel);
    refresh(spectrumPanel);
    QThread::msleep(1100);
    peakPanel.acceptPeak(0.75f, 0.75f);
    peakPanel.acceptPeak(0.125f, 0.125f);
    refresh(peakPanel);
    EXPECT_NEAR(peakPanel.levelDb(), 20.0f * std::log10(0.125f), 0.001f);
    EXPECT_NEAR(peakPanel.heldPeakDb(), 20.0f * std::log10(0.75f), 0.001f);
    HarkenEditor::Bridge::MeterMeasurementFrame frame;
    frame.fftSize = 2048;
    frame.sampleRate = 48000;
    frame.spectrumDb.assign(1025, -6.0f);
    spectrumPanel.acceptMeasurement(frame);
    frame.spectrumDb.assign(1025, -48.0f);
    spectrumPanel.acceptMeasurement(frame);
    refresh(spectrumPanel);
    for (float level : spectrumPanel.spectrumLevels())
        EXPECT_NEAR(level, std::pow(10.0f, -48.0f / 20.0f), 0.00001f);
    for (float peak : spectrumPanel.spectrumPeaks())
        EXPECT_NEAR(peak, std::pow(10.0f, -6.0f / 20.0f), 0.00001f);
    for (auto* panel : loudnessPanels) {
        frame = {};
        frame.rmsAvailable = panel == &rmsPanel;
        frame.lufsAvailable = panel == &lufsPanel;
        frame.rms = 0.5f;
        frame.lufs = -6.0f;
        panel->acceptMeasurement(frame);
        const float maximum = panel->loudnessDb();
        frame.rms = 0.125f;
        frame.lufs = -18.0f;
        panel->acceptMeasurement(frame);
        refresh(*panel);
        EXPECT_LT(panel->loudnessDb(), maximum);
        EXPECT_FLOAT_EQ(panel->heldLoudnessDb(), maximum);
        panel->resetPeakHold();
        EXPECT_FLOAT_EQ(panel->heldLoudnessDb(), panel->loudnessDb());
    }
}

TEST(MeterPanel, HoldingPlotFreezesDisplayedMeasurementsWhileAcquisitionContinues)
{
    ensureApp();
    VNX::MeterPanel panel;
    panel.resize(480, 360);
    panel.show();
    auto* canvas = panel.findChild<QWidget*>(QStringLiteral("meterCanvas"));
    ASSERT_NE(canvas, nullptr);
    const auto render = [&] {
        QImage image(canvas->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        canvas->render(&image);
        return image;
    };
    for (auto mode : {VNX::MeterPanel::Mode::Spectrum, VNX::MeterPanel::Mode::Spectrogram}) {
        panel.setMode(mode);
        panel.resetLevels();
        panel.acceptPeak(0.25f, 0.25f);
        HarkenEditor::Bridge::MeterMeasurementFrame frame;
        frame.fftSize = 2048;
        frame.sampleRate = 48000;
        frame.spectrumDb.assign(1025, -48.0f);
        panel.acceptMeasurement(frame);
        refresh(panel);
        const int history = panel.historyColumns();
        const QPoint point = panel.plotBounds().center();
        QMouseEvent press(QEvent::MouseButtonPress, point, canvas->mapToGlobal(point),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &press);
        const QImage frozen = render();
        panel.acceptPeak(1.25f, 1.25f);
        frame.spectrumDb.assign(1025, -6.0f);
        for (int column = 0; column < 4; ++column)
            panel.acceptMeasurement(frame);
        refresh(panel);
        EXPECT_EQ(panel.historyColumns(), history);
        EXPECT_EQ(render(), frozen);
        const QPoint moved = point + QPoint(15, 5);
        QMouseEvent move(QEvent::MouseMove, moved, canvas->mapToGlobal(moved),
            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &move);
        EXPECT_TRUE(panel.crosshair().visible);
        EXPECT_EQ(panel.crosshair().position, moved);
        const QPoint outside(-4, -4);
        QMouseEvent release(QEvent::MouseButtonRelease, outside, canvas->mapToGlobal(outside),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &release);
        EXPECT_NEAR(panel.levelDb(), 20.0f * std::log10(1.25f), 0.001f);
        EXPECT_TRUE(panel.clipped());
        EXPECT_EQ(panel.historyColumns(), history + 4);
        EXPECT_NEAR(*std::max_element(panel.spectrumLevels().begin(), panel.spectrumLevels().end()),
            std::pow(10.0f, -6.0f / 20.0f), 0.001f);
    }
}

TEST(MeterPanel, LostMouseGrabAndHideCancelPlotPause)
{
    ensureApp();
    for (auto mode : {VNX::MeterPanel::Mode::Spectrum, VNX::MeterPanel::Mode::Spectrogram}) {
        for (bool hide : {false, true}) {
            VNX::MeterPanel panel;
            panel.resize(480, 360);
            panel.setMode(mode);
            panel.show();
            auto* canvas = panel.findChild<QWidget*>(QStringLiteral("meterCanvas"));
            ASSERT_NE(canvas, nullptr);
            panel.acceptPeak(0.25f, 0.25f);
            refresh(panel);
            const float frozen = panel.levelDb();
            const QPoint point = panel.plotBounds().center();
            QMouseEvent press(QEvent::MouseButtonPress, point, canvas->mapToGlobal(point),
                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas, &press);
            panel.acceptPeak(0.75f, 0.75f);
            refresh(panel);
            EXPECT_FLOAT_EQ(panel.levelDb(), frozen);
            if (hide) {
                panel.hide();
                panel.show();
                EXPECT_FALSE(panel.levelAvailable());
                panel.acceptPeak(0.75f, 0.75f);
                refresh(panel);
            } else {
                QEvent ungrab(QEvent::UngrabMouse);
                QApplication::sendEvent(canvas, &ungrab);
            }
            EXPECT_NEAR(panel.levelDb(), 20.0f * std::log10(0.75f), 0.001f);
            panel.acceptPeak(1.25f, 1.25f);
            refresh(panel);
            EXPECT_NEAR(panel.levelDb(), 20.0f * std::log10(1.25f), 0.001f);
        }
    }
}

TEST(MeterPanel, SpectrogramResizePreservesTimeScaleAndRightAlignedHistory)
{
    ensureApp();
    VNX::MeterPanel panel;
    panel.setMode(VNX::MeterPanel::Mode::Spectrogram);
    panel.resize(360, 360);
    panel.show();
    QApplication::processEvents();
    auto* canvas = panel.findChild<QWidget*>(QStringLiteral("meterCanvas"));
    ASSERT_NE(canvas, nullptr);
    HarkenEditor::Bridge::MeterMeasurementFrame frame;
    frame.fftSize = 2048;
    frame.sampleRate = 48000;
    for (int column = 0; column < 2000; ++column) {
        frame.spectrumDb.assign(1025, -72.0f + 20.0f * ((column / 9) % 4));
        panel.acceptMeasurement(frame);
    }
    const int history = panel.historyColumns();
    for (double speed : {0.25, 2.0, 10.0}) {
        panel.setSpectrogramSpeed(speed);
        for (int pixelRatio : {1, 2}) {
            SCOPED_TRACE(::testing::Message() << "speed=" << speed << " DPR=" << pixelRatio);
            const auto renderPlot = [&] {
                QEvent leave(QEvent::Leave);
                QApplication::sendEvent(canvas, &leave);
                QImage image(canvas->size() * pixelRatio, QImage::Format_ARGB32_Premultiplied);
                image.setDevicePixelRatio(pixelRatio);
                image.fill(Qt::transparent);
                canvas->render(&image);
                const QRect plot = panel.plotBounds();
                return image.copy(QRect(plot.topLeft() * pixelRatio, plot.size() * pixelRatio));
            };
            panel.resize(360, 360);
            QApplication::processEvents();
            const QImage narrow = renderPlot();
            panel.resize(720, 360);
            QApplication::processEvents();
            const QImage wide = renderPlot();
            ASSERT_EQ(narrow.height(), wide.height());
            ASSERT_LT(narrow.width(), wide.width());
            const QRect overlap = narrow.rect().adjusted(3 * pixelRatio, 3 * pixelRatio,
                -3 * pixelRatio, -3 * pixelRatio);
            EXPECT_EQ(narrow.copy(overlap), wide.copy(overlap.translated(wide.width() - narrow.width(), 0)));
            EXPECT_EQ(panel.historyColumns(), history);
        }
    }
}

TEST(MeterScale, TickDensityAdaptsToSizeAndFontWithoutOverlappingLabels)
{
    ensureApp();
    for (int pixels : {11, 18, 30}) {
        QFont font;
        font.setPixelSize(pixels);
        const QFontMetrics metrics(font);
        for (int height : {24, 40, 80, 240, 800}) {
            const auto ticks = VNX::MeterScale::frequencyTicks(20000, height, metrics, true);
            float previous = -1000.0f;
            for (const auto& tick : ticks) {
                if (tick.text.isEmpty())
                    continue;
                const float pixel = tick.normalizedPosition * (height - 1);
                EXPECT_GE(pixel - previous, metrics.height());
                previous = pixel;
            }
        }
    }
}

TEST(MeterBallistics, LevelTracksInputWhilePeakHoldsForOneSecondThenReleases)
{
    VNX::MeterBallistics meter;
    meter.advance(-30.0f, 0.0f);
    EXPECT_FLOAT_EQ(meter.levelDb, -30.0f);
    EXPECT_FLOAT_EQ(meter.peakDb, -30.0f);
    meter.advance(-6.0f, 0.001f);
    EXPECT_FLOAT_EQ(meter.levelDb, -6.0f);
    EXPECT_FLOAT_EQ(meter.peakDb, -6.0f);
    meter.advance(-24.0f, 0.0f);
    EXPECT_FLOAT_EQ(meter.levelDb, -24.0f);
    EXPECT_FLOAT_EQ(meter.peakDb, -6.0f);
    meter.advance(-96.0f, 0.05f);
    EXPECT_FLOAT_EQ(meter.levelDb, -96.0f);
    EXPECT_FLOAT_EQ(meter.peakDb, -6.0f);
    meter.advance(-96.0f, 0.9f);
    EXPECT_FLOAT_EQ(meter.peakDb, -6.0f);
    meter.advance(-96.0f, 0.1f);
    EXPECT_NEAR(meter.peakDb, -9.0f, 0.001f);
    EXPECT_GE(meter.peakDb, meter.levelDb);
    meter.advance(-2.0f, 0.0f);
    EXPECT_FLOAT_EQ(meter.levelDb, -2.0f);
    EXPECT_FLOAT_EQ(meter.peakDb, -2.0f);
    meter.advance(-96.0f, 10.0f);
    EXPECT_FLOAT_EQ(meter.levelDb, -96.0f);
    EXPECT_FLOAT_EQ(meter.peakDb, -96.0f);
    meter = {0.0f, 0.0f, 0.0f};
    meter.advance(-12.0f, 0.5f);
    EXPECT_FLOAT_EQ(meter.peakDb, -12.0f);
    meter.advance(-96.0f, 0.05f);
    EXPECT_FLOAT_EQ(meter.peakDb, -12.0f);
    EXPECT_LT(meter.levelDb, meter.peakDb);
}

TEST(MeterBallistics, ReleaseDependsOnElapsedTimeInsteadOfUpdateFrequency)
{
    for (float totalSeconds : {0.05f, 0.75f, 0.95f, 1.0f, 1.25f, 2.0f}) {
        for (float inputDb : {-96.0f, -24.0f}) {
            VNX::MeterBallistics single;
            single.advance(0.0f, 0.0f);
            single.advance(inputDb, totalSeconds);
            for (int subdivisions : {3, 11, 60}) {
                VNX::MeterBallistics divided;
                divided.advance(0.0f, 0.0f);
                for (int step = 0; step < subdivisions; ++step) {
                    divided.advance(inputDb, totalSeconds / subdivisions);
                    EXPECT_GE(divided.peakDb, divided.levelDb);
                    EXPECT_GE(divided.levelDb, -96.0f);
                }
                EXPECT_NEAR(divided.levelDb, single.levelDb, 0.001f);
                EXPECT_NEAR(divided.peakDb, single.peakDb, 0.001f);
                auto singleAfterSilence = single;
                singleAfterSilence.advance(-96.0f, 0.1f);
                divided.advance(-96.0f, 0.1f);
                EXPECT_NEAR(divided.levelDb, singleAfterSilence.levelDb, 0.001f);
                EXPECT_NEAR(divided.peakDb, singleAfterSilence.peakDb, 0.001f);
            }
        }
    }
}

TEST(AmplitudeScale, PreservesRangeAndInverseAcrossCurveSettings)
{
    for (double maximumDb : {0.0, 6.0, 24.0}) {
        for (double exponent : {0.0, 1e-10, 0.01, 0.3, 1.0}) {
            const VNX::AmplitudeScale axis{-96.0, maximumDb, exponent};
            SCOPED_TRACE(::testing::Message() << "maximum=" << maximumDb << " exponent=" << exponent);
            EXPECT_DOUBLE_EQ(axis.dbToPosition(-120.0), 0.0);
            EXPECT_DOUBLE_EQ(axis.dbToPosition(-96.0), 0.0);
            EXPECT_DOUBLE_EQ(axis.dbToPosition(maximumDb), 1.0);
            EXPECT_DOUBLE_EQ(axis.dbToPosition(maximumDb + 24.0), 1.0);
            EXPECT_NEAR(axis.positionToDb(0.0), -96.0, 1e-12);
            EXPECT_NEAR(axis.positionToDb(1.0), maximumDb, 1e-12);
            EXPECT_DOUBLE_EQ(axis.amplitudeToPosition(0.0), 0.0);
            EXPECT_DOUBLE_EQ(axis.positionToAmplitude(0.0), 0.0);
            double previous = 0.0;
            for (double db : {-95.9, -72.0, -48.0, -24.0, -6.0, 0.0}) {
                const double position = axis.dbToPosition(db);
                EXPECT_GT(position, previous);
                EXPECT_NEAR(axis.positionToDb(position), db, 1e-7);
                const double amplitude = std::pow(10.0, db / 20.0);
                EXPECT_NEAR(axis.amplitudeToPosition(amplitude), position, 1e-10);
                EXPECT_NEAR(axis.positionToAmplitude(position), amplitude, 1e-10);
                previous = position;
            }
        }
    }
}

TEST(AmplitudeScale, CurveInterpolatesContinuouslyBetweenDecibelAndAmplitudeAxes)
{
    const VNX::AmplitudeScale decibel{-96.0, 0.0, 0.0};
    const VNX::AmplitudeScale nearDecibel{-96.0, 0.0, 1e-10};
    const VNX::AmplitudeScale power{-96.0, 0.0, 0.3};
    const VNX::AmplitudeScale amplitude{-96.0, 0.0, 1.0};
    EXPECT_NEAR(decibel.dbToPosition(-48.0), 0.5, 1e-12);
    EXPECT_NEAR(amplitude.amplitudeToPosition(0.5), 0.5, 0.00001);
    for (double db : {-90.0, -60.0, -30.0, -12.0, -3.0}) {
        EXPECT_NEAR(nearDecibel.dbToPosition(db), decibel.dbToPosition(db), 1e-8);
        EXPECT_LT(amplitude.dbToPosition(db), power.dbToPosition(db));
        EXPECT_LT(power.dbToPosition(db), decibel.dbToPosition(db));
    }
}

TEST(AmplitudeScale, IntegralTicksPreserveZeroAndSpacingAcrossCurveSettings)
{
    ensureApp();
    for (int pixels : {11, 18, 30}) {
        QFont font;
        font.setPixelSize(pixels);
        const QFontMetrics metrics(font);
        for (int height : {24, 40, 80, 240, 800, 2400}) {
            for (double exponent : {0.0, 0.3, 1.0}) {
                for (bool bipolar : {false, true}) {
                    const VNX::AmplitudeScale axis{-96.25, 6.25, exponent};
                    const QRect viewport(0, 10, 80, height);
                    const double zeroY = bipolar ? viewport.center().y() : viewport.bottom();
                    const double rangeHeight = bipolar ? (height - 1) / 2.0 : height - 1;
                    const auto ticks = VNX::buildAmplitudeAxisTicks(viewport, zeroY,
                        rangeHeight, metrics, 32.0, axis, bipolar);
                    SCOPED_TRACE(::testing::Message() << "font=" << pixels << " height=" << height
                        << " exponent=" << exponent << " bipolar=" << bipolar);
                    bool zeroVisible = false;
                    int previous = std::numeric_limits<int>::min() / 2;
                    for (const auto& tick : ticks) {
                        if (tick.label.isEmpty())
                            continue;
                        bool integral = false;
                        const int labelDb = tick.label.toInt(&integral);
                        EXPECT_TRUE(integral);
                        EXPECT_DOUBLE_EQ(labelDb, tick.db);
                        EXPECT_GE(tick.db, axis.minimumDb);
                        EXPECT_LE(tick.db, axis.maximumDb);
                        if (tick.db == 0.0) {
                            zeroVisible = true;
                        }
                        EXPECT_GE(tick.y - previous, metrics.height());
                        previous = tick.y;
                    }
                    EXPECT_TRUE(zeroVisible);
                }
            }
        }
    }
}

TEST(MeterPanel, AmplitudeCurveChangesPreserveMeasurementsAndModeSettings)
{
    ensureApp();
    VNX::MeterPanel panel;
    panel.resize(480, 360);
    panel.show();
    auto* canvas = panel.findChild<QWidget*>(QStringLiteral("meterCanvas"));
    ASSERT_NE(canvas, nullptr);
    panel.acceptPeak(0.5f, 0.5f);
    const float measuredDb = panel.levelDb();
    const QPoint point = panel.plotBounds().center();
    QMouseEvent move(QEvent::MouseMove, point, canvas->mapToGlobal(point),
        Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &move);
    const QString originalLabel = panel.crosshair().label;
    panel.setAmplitudeExponent(0.0);
    EXPECT_NE(panel.crosshair().label, originalLabel);
    EXPECT_FLOAT_EQ(panel.levelDb(), measuredDb);
    panel.setMode(VNX::MeterPanel::Mode::Spectrum);
    panel.setAmplitudeExponent(1.0);
    panel.setMode(VNX::MeterPanel::Mode::Peak);
    EXPECT_DOUBLE_EQ(panel.amplitudeExponent(), 0.0);
    panel.setMode(VNX::MeterPanel::Mode::Spectrum);
    EXPECT_DOUBLE_EQ(panel.amplitudeExponent(), 1.0);
    const auto saved = panel.saveState();
    for (const auto [storedMode, mode] : {std::pair{0, VNX::MeterPanel::Mode::Peak},
             std::pair{1, VNX::MeterPanel::Mode::Spectrum}, std::pair{2, VNX::MeterPanel::Mode::Spectrogram}}) {
        auto legacy = saved;
        legacy["mode"] = storedMode;
        panel.restoreState(legacy);
        EXPECT_EQ(panel.mode(), mode);
    }
    panel.setMode(VNX::MeterPanel::Mode::Loudness);
    panel.setLoudnessMeasurement(VNX::MeterPanel::LoudnessMeasurement::Lufs);
    VNX::MeterPanel restored;
    restored.restoreState(panel.saveState());
    EXPECT_EQ(restored.mode(), VNX::MeterPanel::Mode::Loudness);
    EXPECT_EQ(restored.loudnessMeasurement(), VNX::MeterPanel::LoudnessMeasurement::Lufs);
    EXPECT_DOUBLE_EQ(restored.amplitudeExponent(), 0.0);
}

TEST(MeterSource, SharedMeasurementsRewarmAfterDiscontinuitiesWithoutReusingOldValues)
{
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);
    const auto terminate = qScopeGuard([] { Vn_Term(); });
    ASSERT_EQ(Vn_Update(), VN_OK);
    VNX::MeterSource rms;
    VNX::MeterSource rmsCopy;
    VNX::MeterSource lufs;
    ASSERT_TRUE(rms.start(harkenstudio::kMasterBusObjectId, HarkenEditor::Bridge::MeterMeasurement::Rms));
    ASSERT_TRUE(rmsCopy.start(harkenstudio::kMasterBusObjectId, HarkenEditor::Bridge::MeterMeasurement::Rms));
    ASSERT_TRUE(lufs.start(harkenstudio::kMasterBusObjectId, HarkenEditor::Bridge::MeterMeasurement::LufsMomentary));
    harken::monitor::StopDrainForTests();
    const auto restartDrain = qScopeGuard([] { harken::monitor::StartDrainForTests(); });
    const std::array sources{&rms, &rmsCopy, &lufs};
    for (int boundary = 0; boundary < 2; ++boundary) {
        harken::monitor::AdvanceExecuteFrameForTests(1);
        for (auto* source : sources)
            source->poll();
    }
    uint64_t sourceFrame = 0;
    uint32_t sampleRate = 48000;
    const auto publish = [&](int chunks) {
        auto* runtime = harken::detail::ActiveEngine();
        const auto busIndex = runtime->buses.FindIndex(VN_BUS_MASTER);
        const uint32_t frames = sampleRate / 50;
        std::vector<float> samples(frames);
        for (int chunk = 0; chunk < chunks; ++chunk) {
            for (uint32_t index = 0; index < frames; ++index)
                samples[index] = 0.5f * std::sin(2.0 * 3.141592653589793 * 1000 * (sourceFrame + index) / sampleRate);
            float* planes[] = {samples.data(), samples.data()};
            harken::monitor::PublishPlanarAudioChunk(VN_MONITOR_TAP_MASTER_OUTPUT,
                busIndex + 1u, runtime->buses.GenerationAt(busIndex), 0, planes, frames, 2, 0,
                VN_CHANNEL_LAYOUT_STEREO, sampleRate, sourceFrame);
            sourceFrame += frames;
            harken::monitor::DrainOnceForTests();
        }
    };
    const auto verifyNumeric = [&](VNX::MeterSource& source, bool isLufs) {
        SCOPED_TRACE(::testing::Message() << "sourceFrame=" << sourceFrame << " sampleRate=" << sampleRate
            << " lufs=" << isLufs);
        const auto update = source.poll();
        EXPECT_FALSE(update.frames.empty());
        for (const auto& frame : update.frames) {
            EXPECT_EQ(frame.rmsAvailable, !isLufs);
            EXPECT_EQ(frame.lufsAvailable, isLufs);
            EXPECT_FALSE(frame.unavailable);
            EXPECT_FALSE(frame.invalidated);
            EXPECT_TRUE(std::isfinite(isLufs ? frame.lufs : frame.rms));
        }
    };
    publish(19);
    verifyNumeric(rms, false);
    verifyNumeric(rmsCopy, false);
    {
        const auto update = lufs.poll();
        EXPECT_TRUE(update.interrupted);
        EXPECT_TRUE(update.frames.empty());
        EXPECT_TRUE(lufs.active());
    }
    publish(1);
    for (auto* source : sources)
        verifyNumeric(*source, source == &lufs);
    ASSERT_TRUE(rms.start(harkenstudio::kMasterBusObjectId, HarkenEditor::Bridge::MeterMeasurement::Rms));
    EXPECT_FALSE(rms.poll().interrupted);
    publish(1);
    verifyNumeric(rms, false);
    verifyNumeric(lufs, true);
    sourceFrame += 4800;
    publish(1);
    for (auto* source : sources) {
        const auto update = source->poll();
        EXPECT_TRUE(update.interrupted);
        EXPECT_TRUE(update.frames.empty());
        EXPECT_TRUE(source->active());
    }
    publish(19);
    for (auto* source : sources)
        verifyNumeric(*source, source == &lufs);
    sampleRate = 24000;
    publish(1);
    for (auto* source : sources) {
        const auto update = source->poll();
        EXPECT_TRUE(update.interrupted);
        EXPECT_TRUE(update.frames.empty());
        EXPECT_TRUE(source->active());
    }
    publish(19);
    for (auto* source : sources)
        verifyNumeric(*source, source == &lufs);
    rms.stop();
    publish(1);
    verifyNumeric(rmsCopy, false);
    verifyNumeric(lufs, true);
    ASSERT_TRUE(rmsCopy.start(harkenstudio::kMasterBusObjectId, HarkenEditor::Bridge::MeterMeasurement::LufsMomentary));
    ASSERT_TRUE(rmsCopy.start(harkenstudio::kMasterBusObjectId, HarkenEditor::Bridge::MeterMeasurement::Rms));
    for (int boundary = 0; boundary < 2; ++boundary) {
        harken::monitor::AdvanceExecuteFrameForTests(1);
        rmsCopy.poll();
        lufs.poll();
    }
    publish(20);
    verifyNumeric(rmsCopy, false);
    verifyNumeric(lufs, true);
    rmsCopy.stop();
    lufs.stop();
    ASSERT_TRUE(rmsCopy.start(harkenstudio::kMasterBusObjectId, HarkenEditor::Bridge::MeterMeasurement::Rms));
    for (int boundary = 0; boundary < 2; ++boundary) {
        harken::monitor::AdvanceExecuteFrameForTests(1);
        rmsCopy.poll();
    }
    publish(15);
    verifyNumeric(rmsCopy, false);
    ASSERT_TRUE(rms.start(harkenstudio::kMasterBusObjectId, HarkenEditor::Bridge::MeterMeasurement::Rms));
    publish(1);
    verifyNumeric(rms, false);
    EXPECT_FALSE(rmsCopy.start(harkentest::StudioObjectId(8302), HarkenEditor::Bridge::MeterMeasurement::Rms));
    EXPECT_FALSE(rmsCopy.active());
    EXPECT_TRUE(rmsCopy.poll().frames.empty());
}
