#include "ColorPalette.h"

#include "BuiltinTheme.h"
#include "ThemePackageLoader.h"

#include <utility>

namespace VNX {

QList<ColorPalette> createStudioDefaultPalettes()
{
    QList<ColorPalette> palettes;
    const auto add = [&](const QString& id, const QString& name, ThemeSnapshot theme) {
        ColorPalette palette{id, name, {},
                             std::make_shared<const ThemeSnapshot>(std::move(theme))};
        for (const auto& role : studioDefaultColorRoles())
            palette.colors.insert(role.key, readThemeColor(*palette.snapshot, role.key));
        palettes.append(std::move(palette));
    };
    add(QStringLiteral("dark"), QStringLiteral("Dark"), createBuiltinDarkTheme());
    add(QStringLiteral("light"), QStringLiteral("Light"), createBuiltinLightTheme());
    return palettes;
}

void copyThemeColors(ThemeSnapshot& target, const ThemeSnapshot& source)
{
    target.colorScheme = source.colorScheme;
    target.ui.colors = source.ui.colors;
    target.qtPalette = source.qtPalette;
    target.thirdParty.qtAds.chromeBackground = source.thirdParty.qtAds.chromeBackground;
    target.thirdParty.qtAds.activeTabAccentOpacity = source.thirdParty.qtAds.activeTabAccentOpacity;
    target.thirdParty.qtAds.dropOverlayOpacity = source.thirdParty.qtAds.dropOverlayOpacity;

    auto& waveform = target.domains.waveform;
    const auto& sourceWaveform = source.domains.waveform;
    waveform.trimHandle = sourceWaveform.trimHandle;
    waveform.loopHandle = sourceWaveform.loopHandle;
    waveform.staticMode = sourceWaveform.staticMode;
    waveform.multiBandMode = sourceWaveform.multiBandMode;
    waveform.colorMapMode = sourceWaveform.colorMapMode;
    waveform.splitStereo.divider = sourceWaveform.splitStereo.divider;
    waveform.splitStereo.label = sourceWaveform.splitStereo.label;
    waveform.rawPeaks.background = sourceWaveform.rawPeaks.background;
    waveform.rawPeaks.centerLine = sourceWaveform.rawPeaks.centerLine;
    waveform.dbScale.grid = sourceWaveform.dbScale.grid;
    waveform.dbScale.label = sourceWaveform.dbScale.label;

    auto& meter = target.domains.meter;
    const auto& sourceMeter = source.domains.meter;
    meter.interaction.colors = sourceMeter.interaction.colors;
    meter.peakMeter.colors = sourceMeter.peakMeter.colors;
    meter.spectrumMeter.colors = sourceMeter.spectrumMeter.colors;
    meter.spectrogramMeter.colors = sourceMeter.spectrogramMeter.colors;
    meter.peakMeter.metrics.gradientErrorStop = sourceMeter.peakMeter.metrics.gradientErrorStop;
    meter.peakMeter.metrics.gradientWarningStop = sourceMeter.peakMeter.metrics.gradientWarningStop;
    meter.peakMeter.metrics.gradientYellowStop = sourceMeter.peakMeter.metrics.gradientYellowStop;
    meter.peakMeter.metrics.gradientAccentStop = sourceMeter.peakMeter.metrics.gradientAccentStop;
    meter.spectrumMeter.metrics.fillTopStop = sourceMeter.spectrumMeter.metrics.fillTopStop;
    meter.spectrumMeter.metrics.fillUpperMiddleStop = sourceMeter.spectrumMeter.metrics.fillUpperMiddleStop;
    meter.spectrumMeter.metrics.fillLowerMiddleStop = sourceMeter.spectrumMeter.metrics.fillLowerMiddleStop;
    meter.spectrumMeter.metrics.fillBottomStop = sourceMeter.spectrumMeter.metrics.fillBottomStop;

    target.domains.curve.colors = source.domains.curve.colors;
    target.domains.curve.metrics.discreteGuideAlpha = source.domains.curve.metrics.discreteGuideAlpha;
    target.domains.startup = source.domains.startup;
    target.domains.pinDomain = source.domains.pinDomain;
    target.domains.nodeGraphNode = source.domains.nodeGraphNode;
    target.domains.blueprint = source.domains.blueprint;
    target.domains.timeline = source.domains.timeline;
    target.domains.music = source.domains.music;
    target.domains.log = source.domains.log;
    target.domains.ai = source.domains.ai;
}

}
