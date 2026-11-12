#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/FontManager.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/WaveformRenderer.h"
#include "Widgets/SourceEditor/SourceEditorWaveformView.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QContextMenuEvent>
#include <QFile>
#include <QFontMetrics>
#include <QMenu>
#include <QTemporaryDir>

#include <vector>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

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

}

TEST(SourceEditorPanelTest, WaveformDetailReadsOnlyRequestedFramesAndPreservesChannels)
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

TEST(SourceEditorPanelTest, FadePresetMenusFollowTheSameRightSegmentRule)
{
    ensureApp();
    VNX::AssetInfo asset;
    asset.sourceID = 5;
    asset.metadata.totalSamples = 1000;
    asset.metadata.sampleRate = 1000;
    asset.metadata.channels = 1;
    VNX::SoundSourceDef source;
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 100;
    source.trimEndFrame = 900;
    source.fadeIn.durationFrames = 200;
    source.fadeOut.durationFrames = 300;
    VNX::SourceEditorWaveformView view(nullptr, VNX::SourceEditorWaveformView::Presentation::TrimEditor);
    view.resize(600, 400);
    view.setAsset(&asset);
    view.setEditable(true);
    view.setSource(source);
    view.setViewRange(0.0, 1000.0);
    view.show();
    int commits = 0;
    VNX::SoundSourceDef committed;
    view.setCommitHandler([&](const VNX::SoundSourceDef& next) {
        committed = next;
        ++commits;
    });
    const QRect plot = view.waveformRect();
    const auto theme = VNX::ThemeManager::instance().snapshot();
    const auto& metrics = theme->ui.metrics.timeAxis;
    const QRect viewport = plot.adjusted(0, metrics.handleBandHeight, 0, 0);
    const QFontMetrics font(VNX::FontManager::instance().getFont(VNX::FontRole::Caption), &view);
    const QRect wave = VNX::waveformAmplitudeBounds(viewport, viewport.height(), 0, font, metrics.labelPadding);
    const auto ticks = VNX::buildAmplitudeAxisTicks(viewport, wave.top() + wave.height() / 2,
        wave.height() / 2, font, theme->domains.curve.metrics.amplitudeGridTargetRowSpacing,
        VNX::WaveformRenderer::amplitudeScale(theme->domains.waveform, view.amplitudeExponent()));
    QVector<int> unityRows;
    for (const auto& tick : ticks)
        if (tick.major) unityRows.append(tick.y);
    ASSERT_EQ(unityRows.size(), 2);
    for (const int fadeIndex : {0, 1}) {
        for (const bool rightAnchor : {false, true}) {
            view.setSource(source);
            commits = 0;
            const int frame = fadeIndex == 0 ? (rightAnchor ? 300 : 100) : (rightAnchor ? 900 : 600);
            const bool upperAnchor = (fadeIndex == 0) == rightAnchor;
            const QPoint position(plot.left() + qRound(frame / 1000.0 * (plot.width() - 1)),
                                  upperAnchor ? unityRows.front() : unityRows.back());
            QContextMenuEvent event(QContextMenuEvent::Mouse, position, view.mapToGlobal(position));
            QApplication::sendEvent(&view, &event);
            auto* menu = view.findChild<QMenu*>(QStringLiteral("sourceFadeContextMenu"));
            ASSERT_NE(menu, nullptr);
            auto* presets = menu->findChild<QMenu*>(QStringLiteral("curvePresetMenu"));
            ASSERT_NE(presets, nullptr);
            EXPECT_EQ(presets->menuAction()->isEnabled(), !rightAnchor);
            if (!rightAnchor) {
                for (auto* action : presets->actions()) {
                    if (action->data().toInt() == static_cast<int>(VNX::CurvePreset::Exp3)) {
                        action->trigger();
                        break;
                    }
                }
                ASSERT_EQ(commits, 1);
                EXPECT_EQ(committed, view.previewSource());
                const auto& changed = fadeIndex == 0 ? committed.fadeIn : committed.fadeOut;
                const auto& original = fadeIndex == 0 ? source.fadeIn : source.fadeOut;
                EXPECT_NE(changed.handles, original.handles);
                auto restored = committed;
                (fadeIndex == 0 ? restored.fadeIn : restored.fadeOut) = original;
                EXPECT_EQ(restored, source);
            } else {
                EXPECT_EQ(commits, 0);
                EXPECT_EQ(view.previewSource(), source);
            }
            menu->close();
            QApplication::sendPostedEvents(menu, QEvent::DeferredDelete);
        }
    }
}
