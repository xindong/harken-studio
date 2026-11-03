#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Widgets/Timeline/TimelineCanvas.h"
#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/Components/VSound.h"
#include "Core/Components/VTrack.h"
#include "Core/HarkenStyle.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include <harken_buildinfo.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSet>
#include <QSettings>
#include <QStyleFactory>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace {

class MeasuredApplication : public QApplication
{
public:
    using QApplication::QApplication;
    QObject* measuredCanvas = nullptr;
    qint64 paintNanoseconds = 0;
    int paintCount = 0;
    int tooltipCreates = 0;
    QSet<QObject*> tooltipObjects;

    bool notify(QObject* receiver, QEvent* event) override
    {
        if (event->type() == QEvent::Destroy) {
            tooltipObjects.remove(receiver);
        } else if (measuredCanvas && qstrcmp(receiver->metaObject()->className(), "QTipLabel") == 0
            && !tooltipObjects.contains(receiver)) {
            tooltipObjects.insert(receiver);
            ++tooltipCreates;
        }
        if (receiver != measuredCanvas || event->type() != QEvent::Paint)
            return QApplication::notify(receiver, event);
        QElapsedTimer timer;
        timer.start();
        const bool result = QApplication::notify(receiver, event);
        paintNanoseconds += timer.nsecsElapsed();
        ++paintCount;
        return result;
    }
};

QJsonObject summary(const std::vector<double>& samples)
{
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](double p) {
        return sorted[std::min(sorted.size() - 1, size_t(std::ceil(p * sorted.size()) - 1))];
    };
    return {{"mean", std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size()},
            {"p50", percentile(0.50)}, {"p95", percentile(0.95)}, {"max", sorted.back()}};
}

void settle()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

bool writeWaveform(const QString& path)
{
    constexpr quint32 frames = 288000;
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + frames * 2);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << quint32(48000)
           << quint32(96000) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(frames * 2);
    for (quint32 i = 0; i < frames; ++i) {
        const double t = i / 48000.0;
        const double envelope = 0.18 + 0.5 * (0.5 + 0.5 * std::sin(t * 3.7))
            * (0.65 + 0.35 * std::sin(t * 11.0));
        const double value = envelope * (std::sin(t * 2 * 3.141592653589793 * 110)
            + 0.3 * std::sin(t * 2 * 3.141592653589793 * 241)) / 1.3;
        stream << qint16(value * 30000);
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

}

int main(int argc, char* argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    if (qgetenv("QT_QPA_PLATFORM").split(';').contains("offscreen")
        && qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
#endif
    MeasuredApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("HarkenTimelineBenchmark"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Measure Timeline drag event and paint latency."));
    parser.addHelpOption();
    parser.addOptions({
        {"clips", "Total clip count.", "count", "200"},
        {"lanes", "Track count.", "count", "4"},
        {"waveform", "Render a shared audio source: on or off.", "mode", "on"},
        {"hidden-clips", "Place this many trailing clips beyond the visible time range.", "count", "0"},
        {"frames", "Measured mouse moves per gesture.", "count", "40"},
        {"warmup", "Unmeasured mouse moves per gesture.", "count", "5"},
        {"snap", "Snapping mode: on, off, or both.", "mode", "both"},
        {"gesture", "Gesture: move or resize.", "mode", "move"},
        {"interval-ms", "Event loop wait before each sample.", "ms", "0"},
        {"step-px", "Mouse distance between samples.", "pixels", "7"},
        {"tag", "Label included in the report.", "label", "local"},
        {"output", "Write JSON to this file instead of stdout.", "path"}
    });
    parser.process(app);
    bool valid = true;
    const auto integer = [&](const QString& key, int minimum) {
        bool ok = false;
        const int value = parser.value(key).toInt(&ok);
        valid = valid && ok && value >= minimum;
        return value;
    };
    const int clipCount = integer(QStringLiteral("clips"), 1);
    const int lanes = integer(QStringLiteral("lanes"), 1);
    const int hidden = integer(QStringLiteral("hidden-clips"), 0);
    const int frames = integer(QStringLiteral("frames"), 1);
    const int warmup = integer(QStringLiteral("warmup"), 0);
    const int interval = integer(QStringLiteral("interval-ms"), 0);
    const int stepPixels = integer(QStringLiteral("step-px"), 1);
    const QString waveformMode = parser.value(QStringLiteral("waveform"));
    const QString snap = parser.value(QStringLiteral("snap"));
    const QString gesture = parser.value(QStringLiteral("gesture"));
    if (!valid || lanes > clipCount || hidden >= clipCount
        || (waveformMode != "on" && waveformMode != "off")
        || (gesture != "move" && gesture != "resize")
        || (snap != "on" && snap != "off" && snap != "both")) {
        QTextStream(stderr) << "Invalid benchmark parameters.\n";
        return 2;
    }
    const bool waveform = waveformMode == "on";
    const bool resizing = gesture == "resize";
    QTemporaryDir workspace;
    if (!workspace.isValid()) return 3;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, workspace.path() + "/settings");
    QApplication::setStyle(new VNX::HarkenStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
    VNX::ThemeManager::instance();
    app.setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    auto& manager = VNX::ProjectManager::instance();
    VNX::registerProjectActions(VNX::ActionManager::instance(), &manager, nullptr);
    QObject::connect(&manager, &VNX::ProjectManager::errorOccurred, &app,
        [](const QString& error) { QTextStream(stderr) << error << "\n"; });
    VNX::ProjectConfig config;
    config.name = QStringLiteral("TimelineBenchmark");
    config.location = workspace.path();
    if (!manager.createProject(config)) return 3;
    auto* segment = manager.createItem(nullptr, QStringLiteral("Timeline"), VNX::ProjectItem::Type::Segment);
    if (!segment) return 3;
    std::vector<VNX::ProjectItem*> tracks;
    for (int lane = 0; lane < lanes; ++lane) {
        auto* track = manager.createItem(segment, QStringLiteral("Track %1").arg(lane), VNX::ProjectItem::Type::Track);
        if (!track) return 3;
        tracks.push_back(track);
    }
    VNX::AssetInfo asset;
    if (waveform) {
        asset.sourceID = VNX::AssetRegistry::instance().allocateStableID();
        asset.absolutePath = workspace.path() + "/preview.wav";
        asset.relativePath = QStringLiteral("preview.wav");
        asset.metadata.sampleRate = 48000;
        asset.metadata.channels = 1;
        asset.metadata.bitDepth = 16;
        asset.metadata.totalSamples = 288000;
        asset.metadata.durationSec = 6.0;
        if (!writeWaveform(asset.absolutePath)) return 3;
        VNX::AssetRegistry::instance().registerAsset(asset, false);
        if (!VNX::WaveformCache::generate(asset.absolutePath)) return 3;
    }
    std::vector<VNX::ProjectItem*> sounds;
    for (int i = 0; i < clipCount; ++i) {
        auto* sound = manager.createItem(tracks[i % lanes],
            QStringLiteral("Texture %1 long title for overlap elision").arg(i), VNX::ProjectItem::Type::Sound);
        if (!sound) return 3;
        if (waveform) {
            VNX::SoundSourceDef source;
            source.entryId = QUuid::createUuid();
            source.sourceId = asset.sourceID;
            auto* component = sound->comp<VNX::VSound>();
            component->sources.plain.entries = {source};
            component->sources.plain.active = source.entryId;
        }
        sounds.push_back(sound);
    }
    for (auto* track : tracks) track->comp<VNX::VTrack>()->clips = {};
    QUuid dragGuid;
    double end = 10.0;
    const int perLane = clipCount / lanes + (clipCount % lanes != 0);
    for (int i = 0; i < clipCount; ++i) {
        const int lane = i % lanes;
        const int slot = i / lanes;
        double start = i == 0 ? 0.5 : 2.0 + slot * 6.0 / perLane;
        const double duration = i == 0 ? 0.7 : 1.0;
        if (i >= clipCount - hidden) start = 40.0 + slot * 2.0;
        end = std::max(end, start + duration);
        VNX::Clip clip;
        clip.clipGuid = QUuid::createUuid();
        clip.targetGuid = sounds[i]->guid;
        clip.start = int64_t(std::llround(start * 1e9));
        clip.duration = int64_t(std::llround(duration * 1e9));
        auto& ledger = tracks[lane]->comp<VNX::VTrack>()->clips;
        ledger.mintOccurrenceOrdinal(&clip);
        ledger.items.push_back(clip);
        if (i == 0) dragGuid = clip.clipGuid;
    }
    QJsonArray results;
    bool complete = true;
    {
        VNX::TimelineEditorPanel panel;
        panel.bindItem(segment);
        panel.resize(1200, 540);
        panel.setWindowFlags(Qt::Tool | Qt::WindowDoesNotAcceptFocus);
        panel.setAttribute(Qt::WA_ShowWithoutActivating);
        panel.setWindowTitle(QStringLiteral("Harken isolated Timeline benchmark"));
        panel.show();
        settle();
        panel.setLaneHeaderWidth(180);
        for (int lane = 0; lane < lanes; ++lane) panel.setLaneHeight(lane, 72);
        panel.fitClips();
        panel.setZoomPercent(end * 10.0);
        panel.setEditCursorVisible(false);
        auto* canvas = panel.findChild<VNX::TimelineCanvas*>();
        app.measuredCanvas = canvas;
        settle();
        for (bool alt : {false, true}) {
            if ((snap == "on" && alt) || (snap == "off" && !alt)) continue;
            canvas->setSelectedClip(0, dragGuid);
            settle();
            QRect activeRect;
            for (const auto& item : canvas->lastRenderData().clipItems)
                if (item.guid == dragGuid) activeRect = item.rect;
            if (activeRect.isEmpty()) return 4;
            const QPoint press = resizing
                ? QPoint(activeRect.right() - 1, activeRect.center().y()) : activeRect.center();
            const auto modifiers = alt ? Qt::AltModifier : Qt::NoModifier;
            const bool windowActive = panel.isActiveWindow();
            QMouseEvent down(QEvent::MouseButtonPress, press, canvas->mapToGlobal(press),
                Qt::LeftButton, Qt::LeftButton, resizing ? Qt::NoModifier : modifiers);
            QElapsedTimer clock;
            clock.start();
            QCoreApplication::sendEvent(canvas, &down);
            const double pressMs = clock.nsecsElapsed() / 1e6;
            settle();
            std::vector<double> moveTimes, loopTimes, paintTimes, totalTimes;
            int paints = 0;
            int movingFrames = 0;
            int tooltipCreates = 0;
            int maxPreviewStep = 0;
            int64_t lastCoordinate = canvas->previewStart()
                + (resizing ? canvas->previewDuration() : 0);
            int lastPixel = panel.viewportPixelAtCoordinate(canvas->previewLane(), lastCoordinate);
            for (int frame = 0; frame < frames + warmup; ++frame) {
                if (interval > 0) {
                    QEventLoop wait;
                    QTimer::singleShot(interval, &wait, &QEventLoop::quit);
                    wait.exec();
                }
                const int offset = 20 + (frame % 300 * (stepPixels % 300)) % 300;
                const QPoint at = press + QPoint(offset, 0);
                QMouseEvent move(QEvent::MouseMove, at, canvas->mapToGlobal(at),
                    Qt::NoButton, Qt::LeftButton, modifiers);
                const qint64 paintBefore = app.paintNanoseconds;
                const int countBefore = app.paintCount;
                const int tooltipsBefore = app.tooltipCreates;
                clock.restart();
                QCoreApplication::sendEvent(canvas, &move);
                const double moveMs = clock.nsecsElapsed() / 1e6;
                clock.restart();
                settle();
                const double loopMs = clock.nsecsElapsed() / 1e6;
                const double paintMs = (app.paintNanoseconds - paintBefore) / 1e6;
                const int64_t coordinate = canvas->previewStart()
                    + (resizing ? canvas->previewDuration() : 0);
                const int pixel = panel.viewportPixelAtCoordinate(canvas->previewLane(), coordinate);
                if (frame >= warmup) {
                    moveTimes.push_back(moveMs);
                    loopTimes.push_back(loopMs);
                    paintTimes.push_back(paintMs);
                    totalTimes.push_back(moveMs + loopMs);
                    paints += app.paintCount - countBefore;
                    movingFrames += coordinate != lastCoordinate;
                    if (frame > warmup)
                        maxPreviewStep = std::max(maxPreviewStep, std::abs(pixel - lastPixel));
                    tooltipCreates += app.tooltipCreates - tooltipsBefore;
                }
                lastCoordinate = coordinate;
                lastPixel = pixel;
            }
            const bool exercised = canvas->isGestureActive() && paints > 0;
            complete = complete && exercised;
            results.append(QJsonObject{{"snap", !alt}, {"press_ms", pressMs},
                {"move_ms", summary(moveTimes)}, {"event_loop_ms", summary(loopTimes)},
                {"canvas_paint_ms", summary(paintTimes)}, {"frame_ms", summary(totalTimes)},
                {"paint_count", paints}, {"moving_frames", movingFrames}, {"exercised", exercised},
                {"max_preview_step_px", maxPreviewStep}, {"tooltip_creates", tooltipCreates},
                {"window_active_at_start", windowActive},
                {"canvas_width", canvas->width()}, {"canvas_height", canvas->height()},
                {"rendered_clip_items", canvas->lastRenderData().clipItems.size()}});
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(canvas, &escape);
            settle();
        }
        app.measuredCanvas = nullptr;
    }
    VNX::UndoRedoManager::instance().clear();
    manager.closeProject();
    VNX::ActionManager::instance().clearAllForTesting();
    const QJsonObject record{{"benchmark", "timeline-interaction"}, {"schema", 1},
        {"complete", complete}, {"tag", parser.value(QStringLiteral("tag"))},
        {"build_configuration", QStringLiteral(HARKEN_BENCHMARK_CONFIGURATION)},
        {"build_commit", QString::fromUtf8(Harken::BuildInfo::GitCommit.data(), int(Harken::BuildInfo::GitCommit.size()))},
        {"build_dirty", Harken::BuildInfo::GitDirty}, {"qt_version", qVersion()},
        {"os", QSysInfo::prettyProductName()}, {"architecture", QSysInfo::currentCpuArchitecture()},
        {"platform", QGuiApplication::platformName()}, {"clips", clipCount}, {"lanes", lanes},
        {"window_mode", "tool-no-activation"}, {"input", "synthetic-qt-events"},
        {"gesture", gesture}, {"interval_ms", interval}, {"step_px", stepPixels},
        {"waveform", waveform}, {"hidden_clips", hidden}, {"frames", frames}, {"warmup", warmup},
        {"results", results}};
    const QByteArray json = QJsonDocument(record).toJson(QJsonDocument::Indented);
    const QString destination = parser.value(QStringLiteral("output"));
    if (destination.isEmpty()) {
        QTextStream(stdout) << json;
    } else {
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) return 6;
        QFile file(destination);
        if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) return 6;
    }
    return complete ? 0 : 4;
}
