#include "Core/FontManager.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/QtAdsThemeAdapter.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/WaveformRenderer.h"
#include "Widgets/CurveEditor/CurveRenderer.h"
#include "Widgets/Meter/PeakMeterRenderer.h"
#include "Widgets/Meter/SpectrogramMeterRenderer.h"
#include "Widgets/Meter/SpectrumMeterRenderer.h"
#include "Widgets/Timeline/TimelineRenderer.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QStyleHints>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <memory>
#include <optional>

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

void expectPlatformColorScheme(Qt::ColorScheme expected)
{
    if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
        return;
    EXPECT_EQ(qApp->styleHints()->colorScheme(), expected);
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

bool writeImage(const QString& path, const QColor& color)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage image(12, 6, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    return image.save(path);
}

bool writePackage(const QString& themesRoot,
                  const QString& directory,
                  const QString& id,
                  const std::optional<QString>& baseTheme,
                  const QByteArray& tokens,
                  const QByteArray& curve,
                  const QByteArray& meter,
                  const QByteArray& waveform,
                  const QByteArray& assets,
                  const std::optional<QString>& colorScheme = std::nullopt)
{
    const QString root = QDir(themesRoot).filePath(directory);
    const QByteArray base = baseTheme
        ? QByteArray(",\"baseThemeId\":\"") + baseTheme->toUtf8() + '"'
        : QByteArray();
    const QByteArray scheme = colorScheme
        ? QByteArray(",\"colorScheme\":\"") + colorScheme->toUtf8() + '"'
        : QByteArray();
    const QByteArray manifest = QByteArray("{")
        + "\"schemaVersion\":1,\"id\":\"" + id.toUtf8()
        + "\",\"displayName\":\"" + id.toUtf8()
        + "\",\"version\":\"1.0.0\"" + scheme + base
        + ",\"compatibility\":{\"harkenVersion\":\">=0.1.0 <1.0.0\","
          "\"capabilities\":[\"curve-v1\",\"meter-v1\",\"waveform-v1\"]},"
          "\"entries\":{\"tokens\":\"tokens.json\",\"visuals\":{"
          "\"curve\":\"visuals/curve.json\","
          "\"meter\":\"visuals/meter.json\","
          "\"waveform\":\"visuals/waveform.json\"},"
          "\"assetMap\":\"assets.json\"}}";
    return writeFile(QDir(root).filePath(QStringLiteral("manifest.json")),
                     manifest)
        && writeFile(QDir(root).filePath(QStringLiteral("tokens.json")), tokens)
        && writeFile(QDir(root).filePath(QStringLiteral("visuals/curve.json")),
                     curve)
        && writeFile(QDir(root).filePath(QStringLiteral("visuals/meter.json")),
                     meter)
        && writeFile(QDir(root).filePath(QStringLiteral("visuals/waveform.json")),
                     waveform)
        && writeFile(QDir(root).filePath(QStringLiteral("assets.json")), assets);
}

std::unique_ptr<VNX::ISettingsStorage> memoryStorage()
{
    return std::make_unique<VNX::MemorySettingsStorage>();
}

QImage renderCurve(const VNX::ThemeSnapshotPtr& theme)
{
    QImage image(180, 120, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    VNX::CurveRenderData data;
    data.canvasRect = QRectF(0.0, 0.0, 180.0, 120.0);
    data.graphRect = QRectF(24.0, 10.0, 146.0, 92.0);
    data.gridLines = {
        QLineF(24.0, 56.0, 170.0, 56.0),
        QLineF(72.0, 10.0, 72.0, 102.0),
        QLineF(120.0, 10.0, 120.0, 102.0)};
    data.continuousPath.moveTo(24.0, 90.0);
    data.continuousPath.cubicTo(62.0, 82.0, 116.0, 22.0, 170.0, 18.0);
    data.points = {{QPointF(24.0, 90.0), false},
                   {QPointF(170.0, 18.0), true}};
    QPainter painter(&image);
    VNX::CurveRenderer::render(
        painter, theme->domains.curve,
        VNX::CurveColorRole::AttenuationVolume, data);
    return image;
}

QImage renderSpectrum(const VNX::ThemeSnapshotPtr& theme)
{
    QImage image(180, 120, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    VNX::SpectrumMeterRenderFrame frame;
    frame.canvasBounds = image.rect();
    frame.plotBounds = QRect(24, 8, 148, 98);
    frame.hasPlot = true;
    for (int index = 0; index < VNX::kSpectrumMeterDisplayPoints; ++index)
    {
        const float phase = static_cast<float>(index)
            / (VNX::kSpectrumMeterDisplayPoints - 1);
        frame.smoothedLevels[index] = 0.15f + phase * 0.65f;
        frame.peakLevels[index] = 0.25f + phase * 0.65f;
    }
    QPainter painter(&image);
    VNX::SpectrumMeterRenderer::render(
        painter, theme->domains.meter, frame);
    return image;
}

QImage renderSpectrogram(const VNX::ThemeSnapshotPtr& theme)
{
    QImage image(180, 120, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    VNX::SpectrogramMeterRenderFrame frame;
    frame.canvasBounds = image.rect();
    frame.plotBounds = QRect(24, 8, 148, 98);
    frame.hasPlot = true;
    frame.waterfallImage = QImage(160, 98, QImage::Format_ARGB32);
    frame.waterfallImage.fill(0);
    frame.frequencyTicks = {
        {24.0, QStringLiteral("8k")},
        {54.0, QStringLiteral("2k")},
        {84.0, QStringLiteral("500")},
    };
    QPainter painter(&image);
    VNX::SpectrogramMeterRenderer::render(
        painter, theme->domains.meter, frame);
    return image;
}

QImage renderPeak(VNX::ThemeManager& manager,
                  const VNX::ThemeSnapshotPtr& theme)
{
    QImage image(80, 120, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    VNX::PeakMeterRenderFrame frame;
    frame.canvasBounds = image.rect();
    frame.bars[0].bounds = QRect(5, 5, 28, 108);
    frame.bars[0].showLevel = true;
    frame.bars[0].levelTop = 38.0;
    frame.bars[0].showHold = true;
    frame.bars[0].holdY = 25.0;
    frame.bars[1].bounds = QRect(45, 5, 28, 108);
    frame.bars[1].showLevel = true;
    frame.bars[1].levelTop = 52.0;
    frame.peakCap = manager.assetResolver().resolveImage(
        theme, QStringLiteral("meter.peak.cap"),
        VNX::ThemeAssetVariant{QSize(20, 3), 1.0, 0, QColor()}).resource;
    QPainter painter(&image);
    VNX::PeakMeterRenderer::render(painter, theme->domains.meter, frame);
    return image;
}

QImage renderWaveform(const VNX::ThemeSnapshotPtr& theme)
{
    QImage image(160, 64, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const float peaks[]{-0.8f, 0.7f, -0.3f, 0.4f,
                        -1.0f, 0.9f, -0.2f, 0.2f};
    QPainter painter(&image);
    VNX::WaveformRenderer::renderRawPeaks(
        painter, image.rect(), peaks, 4, theme->domains.waveform,
        theme->ui.colors.accent);
    return image;
}

struct RenderedTheme
{
    QImage curve;
    QImage spectrum;
    QImage spectrogram;
    QImage peak;
    QImage waveform;
    QImage play;
    QPalette palette;
    QString applicationStyleSheet;
    QString qtAdsStyleSheet;
    QFont bodyFont;
    VNX::ThemeMetrics metrics;
};

RenderedTheme capture(VNX::ThemeManager& manager)
{
    const auto theme = manager.snapshot();
    const auto play = manager.assetResolver().resolveImage(
        theme, QStringLiteral("audition.play"));
    EXPECT_TRUE(play.ok());
    return {renderCurve(theme),
            renderSpectrum(theme),
            renderSpectrogram(theme),
            renderPeak(manager, theme),
            renderWaveform(theme),
            play.resource ? play.resource->image : QImage(),
            qApp->palette(),
            qApp->styleSheet(),
            VNX::QtAdsThemeAdapter(theme).styleSheet(),
            VNX::FontManager::instance().getFont(VNX::FontRole::Body),
            theme->ui.metrics};
}

}

TEST(ThemeEndToEnd, PublishesDomainVisualsToTimelineRenderer)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString package = QDir(root.path()).filePath(QStringLiteral("domain"));
    const QByteArray manifest = R"json({
        "schemaVersion":4,
        "id":"test.theme.domain",
        "displayName":"Domain Theme",
        "version":"1.0.0",
        "compatibility":{"harkenVersion":">=0.1.0 <1.0.0"},
        "entries":{
            "tokens":"tokens.json",
            "visuals":{
                "curve":"visuals/curve.json",
                "meter":"visuals/meter.json",
                "waveform":"visuals/waveform.json",
                "timeline":"visuals/timeline.json"
            },
            "assetMap":"assets.json"
        }
    })json";
    ASSERT_TRUE(writeFile(QDir(package).filePath(QStringLiteral("manifest.json")),
                          manifest));
    ASSERT_TRUE(writeFile(QDir(package).filePath(QStringLiteral("tokens.json")),
                          R"json({"colors":{"accent.primary":"#ff00f0ff"}})json"));
    ASSERT_TRUE(writeFile(
        QDir(package).filePath(QStringLiteral("visuals/curve.json")),
        R"json({"colors":{"curve.line":"#ff112233"}})json"));
    ASSERT_TRUE(writeFile(
        QDir(package).filePath(QStringLiteral("visuals/meter.json")),
        R"json({"colors":{"meter.green":"#ff22aa44"}})json"));
    ASSERT_TRUE(writeFile(
        QDir(package).filePath(QStringLiteral("visuals/waveform.json")),
        R"json({"colors":{"waveform.line":"#ff334455"}})json"));
    ASSERT_TRUE(writeFile(
        QDir(package).filePath(QStringLiteral("visuals/timeline.json")),
        R"json({"colors":{"timeline.clip":"#ff123456","timeline.clipBorder":"#ff654321","timeline.clipText":"#ffffffff"}})json"));
    ASSERT_TRUE(writeFile(QDir(package).filePath(QStringLiteral("assets.json")),
                          R"json({"assets":{}})json"));

    VNX::ThemeManager manager(root.path(), memoryStorage());
    const auto render = [](const VNX::ThemeSnapshotPtr& theme) {
        QImage image(80, 32, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        VNX::TimelineRenderer::drawClip(
            painter, QRect(4, 4, 72, 24), theme->domains.timeline, false,
            QStringLiteral("Clip"));
        return image;
    };
    const QImage builtin = render(manager.snapshot());

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("test.theme.domain")).ok());
    const auto selected = manager.snapshot();
    const QImage themed = render(selected);

    EXPECT_NE(themed, builtin);
}
TEST(ThemeEndToEnd, SwitchesHighDifferenceThemesAtomicallyAndRestoresBuiltin)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());

    const QByteArray lightTokens = R"json({
        "colors": {"accent.primary":"#ffcc2200","background.primary":"#fffff8e8",
            "background.secondary":"#fff2e3c4","text.primary":"#ff201000"},
        "metrics": {"control.height":19},
        "fonts": {"body":{"family":"Serif","pointSize":15,"weight":700}}
    })json";
    const QByteArray lightCurve = R"json({"colors":{"curve.canvasBackground":"#fffff8e8",
        "curve.plotBackground":"#ffffe7b8","curve.attenuationVolume":"#ffdf1600"}})json";
    const QByteArray lightMeter = R"json({"colors":{
        "meter.peakMeter.canvasBackground":"#ffffedc2",
        "meter.peakMeter.gradientGreen":"#ff1122ff",
        "meter.spectrum.canvasBackground":"#fffff0cc",
        "meter.spectrum.lineHighlight":"#ff2200ff",
        "meter.spectrogram.canvasBackground":"#ffffd7a8",
        "meter.spectrogram.grid":"#ffff0077"}})json";
    const QByteArray lightWaveform = R"json({"colors":{"waveform.rawPeaks.background":"#ffffe2a3",
        "waveform.rawPeaks.centerLine":"#ff6b2c00","waveform.line":"#ffcc2200"}})json";
    const QByteArray lightAssets = R"json({"assets":{
        "audition.play":{"type":"image","sources":[{"path":"assets/play.png"}]},
        "meter.peak.cap":{"type":"image","sources":[{"path":"assets/cap.png"}]}
    }})json";
    ASSERT_TRUE(writePackage(
        root.path(), QStringLiteral("light"), QStringLiteral("test.theme.light"),
        VNX::builtinThemeId(), lightTokens, lightCurve, lightMeter,
        lightWaveform, lightAssets, QStringLiteral("light")));
    ASSERT_TRUE(writeImage(QDir(root.path()).filePath(
        QStringLiteral("light/assets/play.png")), QColor(QStringLiteral("#ffff2200"))));
    ASSERT_TRUE(writeImage(QDir(root.path()).filePath(
        QStringLiteral("light/assets/cap.png")), QColor(QStringLiteral("#ff0000ff"))));

    const QByteArray darkTokens = R"json({
        "colors": {"accent.primary":"#ff00ff88","background.primary":"#ff020409",
            "background.secondary":"#ff08101d","text.primary":"#fff4fbff"},
        "metrics": {"control.height":34},
        "fonts": {"body":{"family":"Monospace","pointSize":9,"weight":300}}
    })json";
    const QByteArray darkCurve = R"json({"colors":{"curve.canvasBackground":"#ff020409",
        "curve.plotBackground":"#ff061321","curve.attenuationVolume":"#ff00ff88"}})json";
    const QByteArray darkMeter = R"json({"colors":{
        "meter.peakMeter.canvasBackground":"#ff020409",
        "meter.peakMeter.gradientGreen":"#ffff00ff",
        "meter.spectrum.canvasBackground":"#ff020409",
        "meter.spectrum.lineHighlight":"#ff00ff88",
        "meter.spectrogram.canvasBackground":"#ff020409",
        "meter.spectrogram.grid":"#ff004466"}})json";
    const QByteArray darkWaveform = R"json({"colors":{"waveform.rawPeaks.background":"#ff061321",
        "waveform.rawPeaks.centerLine":"#ff00ff88","waveform.line":"#ff00ff88"}})json";
    const QByteArray darkAssets = R"json({"assets":{
        "audition.play":{"type":"image","sources":[{"path":"assets/play.png"}]},
        "meter.peak.cap":{"type":"image","sources":[{"path":"assets/cap.png"}]}
    }})json";
    ASSERT_TRUE(writePackage(
        root.path(), QStringLiteral("dark"), QStringLiteral("test.theme.dark"),
        QStringLiteral("test.theme.light"), darkTokens, darkCurve, darkMeter,
        darkWaveform, darkAssets, QStringLiteral("dark")));
    ASSERT_TRUE(writeImage(QDir(root.path()).filePath(
        QStringLiteral("dark/assets/play.png")), QColor(QStringLiteral("#ff00ff88"))));
    ASSERT_TRUE(writeImage(QDir(root.path()).filePath(
        QStringLiteral("dark/assets/cap.png")), QColor(QStringLiteral("#ffff00ff"))));

    VNX::ThemeManager manager(root.path(), memoryStorage());
    const RenderedTheme builtin = capture(manager);
    int notifications = 0;
    int fontNotifications = 0;
    bool applicationSurfacesReadyForFontSignal = true;
    QObject::connect(&manager, &VNX::ThemeManager::themeChanged,
                     &manager, [&] { ++notifications; });
    QObject::connect(&VNX::FontManager::instance(),
                     &VNX::FontManager::fontChanged,
                     &manager, [&] {
        ++fontNotifications;
        applicationSurfacesReadyForFontSignal =
            applicationSurfacesReadyForFontSignal
            && qApp->palette().color(QPalette::Highlight)
                == manager.snapshot()->ui.colors.accent
            && qApp->font()
                == VNX::FontManager::instance().getFont(VNX::FontRole::Body);
    });

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("test.theme.light")).ok());
    expectPlatformColorScheme(Qt::ColorScheme::Light);
    const RenderedTheme light = capture(manager);
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("test.theme.dark")).ok());
    expectPlatformColorScheme(Qt::ColorScheme::Dark);
    const RenderedTheme dark = capture(manager);

    EXPECT_NE(light.curve, dark.curve);
    EXPECT_NE(light.spectrum, dark.spectrum);
    EXPECT_NE(light.spectrogram, dark.spectrogram);
    EXPECT_NE(light.peak, dark.peak);
    EXPECT_NE(light.waveform, dark.waveform);
    EXPECT_NE(light.play, dark.play);
    EXPECT_NE(light.palette, dark.palette);
    EXPECT_NE(light.applicationStyleSheet, dark.applicationStyleSheet);
    EXPECT_NE(light.qtAdsStyleSheet, dark.qtAdsStyleSheet);
    EXPECT_NE(light.bodyFont, dark.bodyFont);
    EXPECT_NE(light.metrics.controlHeight, dark.metrics.controlHeight);

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("test.theme.light")).ok());
    const RenderedTheme lightAgain = capture(manager);
    EXPECT_EQ(light.curve, lightAgain.curve);
    EXPECT_EQ(light.spectrum, lightAgain.spectrum);
    EXPECT_EQ(light.spectrogram, lightAgain.spectrogram);
    EXPECT_EQ(light.peak, lightAgain.peak);
    EXPECT_EQ(light.waveform, lightAgain.waveform);
    EXPECT_EQ(light.play, lightAgain.play);
    EXPECT_EQ(light.metrics.controlHeight, lightAgain.metrics.controlHeight);

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("test.theme.dark")).ok());
    const auto beforeFailure = manager.snapshot();
    const QPalette paletteBeforeFailure = qApp->palette();
    const QString styleBeforeFailure = qApp->styleSheet();
    const int notificationsBeforeFailure = notifications;
    ASSERT_TRUE(writeFile(QDir(root.path()).filePath(
        QStringLiteral("dark/tokens.json")),
        R"json({"colors":{"unsupported.color":"#ffffffff"}})json"));

    const auto failed = manager.reloadTheme();

    EXPECT_FALSE(failed.ok());
    EXPECT_FALSE(failed.published);
    EXPECT_EQ(manager.snapshot(), beforeFailure);
    EXPECT_EQ(manager.currentThemeId(), QStringLiteral("test.theme.dark"));
    EXPECT_EQ(qApp->palette(), paletteBeforeFailure);
    EXPECT_EQ(qApp->styleSheet(), styleBeforeFailure);
    EXPECT_EQ(notifications, notificationsBeforeFailure);
    EXPECT_EQ(fontNotifications, notificationsBeforeFailure);

    ASSERT_TRUE(manager.selectTheme(VNX::builtinThemeId()).ok());
    expectPlatformColorScheme(Qt::ColorScheme::Dark);
    const RenderedTheme restored = capture(manager);
    EXPECT_EQ(restored.curve, builtin.curve);
    EXPECT_EQ(restored.spectrum, builtin.spectrum);
    EXPECT_EQ(restored.spectrogram, builtin.spectrogram);
    EXPECT_EQ(restored.peak, builtin.peak);
    EXPECT_EQ(restored.waveform, builtin.waveform);
    EXPECT_EQ(restored.play, builtin.play);
    EXPECT_EQ(restored.palette, builtin.palette);
    EXPECT_EQ(restored.applicationStyleSheet, builtin.applicationStyleSheet);
    EXPECT_EQ(restored.qtAdsStyleSheet, builtin.qtAdsStyleSheet);
    EXPECT_EQ(restored.bodyFont, builtin.bodyFont);
    EXPECT_EQ(restored.metrics.controlHeight, builtin.metrics.controlHeight);
    EXPECT_TRUE(applicationSurfacesReadyForFontSignal);
    EXPECT_EQ(fontNotifications, notifications);
}
