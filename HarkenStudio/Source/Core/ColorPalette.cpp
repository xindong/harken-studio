#include "ColorPalette.h"

#include "BuiltinTheme.h"
#include "ThemeColorResolver.h"
#include "ThemePackageLoader.h"

#include <utility>

namespace VNX {

QString defaultPaletteId(const QString& themeId)
{
    return QStringLiteral("theme:") + themeId;
}

QString builtinAmberPaletteId()
{
    return QStringLiteral("harken.palette.amber");
}

ColorPalette colorPaletteFromTheme(const QString& themeId,
                                   const QString& displayName,
                                   const ThemeSnapshot& theme)
{
    ColorPalette palette{defaultPaletteId(themeId), displayName, {},
                         std::make_shared<const ThemeSnapshot>(theme)};
    for (const auto& role : themeColorRoles())
        palette.colors.insert(QString::fromLatin1(role.key), role.read(theme));
    return palette;
}

ColorPalette createBuiltinAmberPalette()
{
    ThemeSnapshot theme = createBuiltinDarkTheme();
    constexpr std::pair<const char*, const char*> overrides[]{
        {"accent.primary", "#E5B567"},
        {"accent.hover", "#F2CE8D"},
        {"accent.text", "#201A11"},
        {"text.primary", "#E8E1D5"},
        {"text.secondary", "#B8AD9D"},
        {"text.muted", "#817665"},
        {"text.disabled", "#655D53"},
        {"background.primary", "#171513"},
        {"background.secondary", "#211E1A"},
        {"background.tertiary", "#2C2721"},
        {"background.hover", "#383127"},
        {"background.selected", "#493A24"},
        {"border.primary", "#453D32"},
        {"border.secondary", "#594D3D"},
        {"border.view", "#342E26"},
        {"panel.background", "#1B1815"},
        {"header.background", "#27221C"},
        {"track.background", "#191613"},
        {"track.backgroundAlternate", "#1F1B17"},
        {"bus.background", "#1D1915"},
        {"slider.track", "#3C3429"},
        {"knob.indicator", "#F4E8D4"},
        {"status.success", "#86B889"},
        {"status.warning", "#E1A456"},
        {"status.error", "#DD756A"},
        {"status.info", "#7EACCA"},
        {"palette.categorical0", "#E5B567"},
        {"palette.categorical1", "#B6A0D8"},
        {"palette.categorical2", "#7DBEAB"},
        {"palette.categorical3", "#80ACD0"},
        {"palette.categorical4", "#D98D77"},
        {"palette.categorical5", "#A7BD7E"},
        {"palette.categorical6", "#D59AB4"},
        {"waveform.static.quiet", "#B4AD794B"},
        {"waveform.static.peak", "#B4F3C77C"},
        {"waveform.trimHandle", "#80ACD0"},
        {"waveform.loopHandle", "#86B889"},
        {"curve.attenuationVolume", "#E5B567"},
        {"curve.attenuationPitch", "#80ACD0"},
        {"curve.parameterBinding", "#B6A0D8"},
        {"curve.gridFade", "#E5B567"},
        {"meter.peakMeter.gradientAccent", "#E5B567"},
        {"meter.peakMeter.gradientYellow", "#E9CF83"},
        {"meter.peakMeter.gradientWarning", "#E49A54"},
        {"meter.peakMeter.gradientError", "#DD756A"},
        {"meter.lufsMomentary", "#80ACD0"},
        {"meter.lufsShortTerm", "#86B889"},
        {"meter.lufsIntegrated", "#E5B567"},
        {"timeline.clip", "#8A6640"},
        {"timeline.clipSelected", "#BC8C4C"},
        {"timeline.clipBorder", "#B99460"},
        {"timeline.clipSelectedBorder", "#EAD1AA"},
        {"timeline.clipText", "#F5E6CB"},
        {"timeline.entry", "#86B889"},
        {"timeline.exit", "#DD756A"},
        {"timeline.transition", "#E5B567"},
        {"blueprint.canvasBackground", "#141210"},
        {"blueprint.gridDot", "#443A2E"},
        {"blueprint.placementChain", "#E5B567"},
        {"blueprint.organizationChain", "#B6A0D8"},
        {"blueprint.containment", "#827766"},
        {"nodeGraph.pin.audio", "#E5B567"},
        {"nodeGraph.pin.control", "#B6A0D8"},
        {"nodeGraph.pin.value", "#7DBEAB"},
        {"nodeGraph.node.blueprintWaveform", "#CCDBAD69"},
    };
    for (const auto& [key, value] : overrides)
        applyThemeColor(theme, QString::fromLatin1(key), QColor(QString::fromLatin1(value)));

    const auto withAlpha = [](QColor color, int alpha) {
        color.setAlpha(alpha);
        return color;
    };
    auto& colors = theme.ui.colors;
    colors.dropHighlight = withAlpha(colors.accent, 96);
    colors.dropAccent = withAlpha(colors.accent, 192);
    colors.busBackgroundActive = withAlpha(colors.accent, 25);
    colors.sectionSeparator = colors.bgHover;
    colors.auditionBg = colors.bgPrimary;
    colors.auditionPlayActive = colors.success;
    colors.auditionStopActive = colors.error;
    colors.shellMenuText = colors.textSecondary;
    colors.shellMenuHoverText = colors.textPrimary;
    colors.shellButtonText = colors.textSecondary;
    colors.shellButtonHoverText = colors.textPrimary;

    auto& palette = theme.qtPalette;
    palette.brightText = colors.knobIndicator;
    palette.light = colors.textPrimary;
    palette.disabledHighlight = colors.borderPrimary;

    auto& qtAds = theme.thirdParty.qtAds;
    qtAds.chromeBackground = QColor("#110F0D");
    qtAds.inactiveTabText = colors.textMuted;

    auto& domains = theme.domains;
    domains.feedback.overlayBackground = withAlpha(colors.bgSecondary, 240);
    domains.feedback.overlayErrorBackground = QColor("#F02C1B18");
    domains.waveform.selectionHighlight = withAlpha(colors.accent, 80);
    domains.waveform.cursor = colors.textPrimary;
    domains.waveform.splitStereo.divider = colors.borderSecondary;
    domains.waveform.splitStereo.label = colors.textMuted;
    domains.waveform.rawPeaks.background = withAlpha(colors.bgPrimary, 120);
    domains.waveform.rawPeaks.centerLine = withAlpha(colors.borderSecondary, 100);

    auto& meter = domains.meter;
    meter.background = colors.bgPrimary;
    meter.oscilloscopeGrid = withAlpha(colors.textPrimary, 15);
    meter.oscilloscopeGlow = withAlpha(colors.textPrimary, 60);
    meter.waveformFill = withAlpha(colors.textPrimary, 180);
    meter.waveformGrid = withAlpha(colors.textPrimary, 40);
    meter.stereo.gridPrimary = withAlpha(colors.textPrimary, 25);
    meter.stereo.gridSecondary = withAlpha(colors.textPrimary, 12);
    meter.layout.dividerHover = withAlpha(colors.accent, 140);
    meter.layout.dividerDragging = withAlpha(colors.accent, 200);
    meter.layout.dividerSnapped = withAlpha(colors.success, 220);
    meter.layout.dropBorder = withAlpha(colors.accent, 200);
    meter.layout.dropFill = withAlpha(colors.accent, 30);
    meter.vuMeter.colors.analogNeedleGlow = withAlpha(colors.accent, 38);
    meter.vuMeter.colors.analogNeedle = withAlpha(colors.textPrimary, 229);
    meter.vuMeter.colors.analogPivot = colors.accent;
    meter.vuMeter.colors.barWarning = colors.warning;
    meter.peakMeter.colors.rmsOverlay = withAlpha(colors.textPrimary, 60);
    meter.peakMeter.colors.holdHalo = withAlpha(colors.textPrimary, 25);
    meter.peakMeter.colors.holdLine = colors.textPrimary;
    meter.spectrogramMeter.colors.emptyHistory = colors.bgPrimary;
    auto& spectrum = meter.spectrumMeter.colors;
    spectrum.fillTop = withAlpha(colors.accent, 160);
    spectrum.fillUpperMiddle = withAlpha(QColor("#B7864E"), 120);
    spectrum.fillLowerMiddle = withAlpha(colors.accent, 80);
    spectrum.fillBottom = withAlpha(colors.accent, 24);
    spectrum.lineGlowWide = withAlpha(colors.accent, 15);
    spectrum.lineGlowMedium = withAlpha(colors.accent, 40);
    spectrum.lineGlowNarrow = withAlpha(colors.accent, 120);
    spectrum.lineHighlight = withAlpha(colors.accentHover, 220);
    spectrum.peakLine = withAlpha(colors.textPrimary, 128);

    domains.pinDomain.ring = domains.blueprint.canvasBackground;
    domains.pinDomain.targetOk = colors.success;
    domains.pinDomain.targetBad = colors.error;
    domains.nodeGraphNode.borderNormal = colors.bgPrimary;
    domains.nodeGraphNode.dropTarget = colors.success;
    domains.nodeGraphNode.waveformBg = colors.bgSecondary;
    domains.nodeGraphNode.waveformCenter = withAlpha(colors.borderSecondary, 100);
    domains.blueprint.edgeLabelBg = withAlpha(domains.blueprint.canvasBackground, 230);
    domains.timeline.outOfSpanVeil = withAlpha(domains.blueprint.canvasBackground, 102);
    domains.music.loopRegion = withAlpha(colors.accent, 40);
    domains.monitor.spatialDot = colors.accent;
    domains.ai.brandAccent = colors.accent;
    domains.scene3d.hudBackground = withAlpha(colors.bgSecondary, 200);
    domains.scene3d.hudTitle = colors.textPrimary;
    domains.scene3d.hudLabel = colors.textSecondary;
    domains.spatial.background = colors.bgSecondary;
    domains.spatial.grid = colors.viewBorder;
    domains.spatial.axis = colors.borderSecondary;
    domains.spatial.axisLabel = colors.textSecondary;
    domains.spatial.infoText = colors.textSecondary;
    domains.spatial.legendBg = colors.bgTertiary;

    resolveThemeColors(theme);
    ColorPalette result{builtinAmberPaletteId(), QStringLiteral("Harken Amber"), {},
                        std::make_shared<const ThemeSnapshot>(std::move(theme))};
    for (const auto& role : themeColorRoles())
        result.colors.insert(QString::fromLatin1(role.key), role.read(*result.snapshot));
    return result;
}

void applyColorPalette(ThemeSnapshot& target, const ColorPalette& palette)
{
    const auto& source = *palette.snapshot;
    target.colorScheme = source.colorScheme;
    target.ui.colors = source.ui.colors;
    target.qtPalette = source.qtPalette;
    target.thirdParty.qtAds.chromeBackground = source.thirdParty.qtAds.chromeBackground;
    target.thirdParty.qtAds.inactiveTabText = source.thirdParty.qtAds.inactiveTabText;
    target.thirdParty.qtAds.closeButtonHover = source.thirdParty.qtAds.closeButtonHover;
    target.thirdParty.qtAds.closeButtonPressed = source.thirdParty.qtAds.closeButtonPressed;
    target.thirdParty.qtAds.lightHoverOverlay = source.thirdParty.qtAds.lightHoverOverlay;
    target.thirdParty.qtAds.activeTabAccentOpacity = source.thirdParty.qtAds.activeTabAccentOpacity;
    target.thirdParty.qtAds.dropOverlayOpacity = source.thirdParty.qtAds.dropOverlayOpacity;

    auto& waveform = target.domains.waveform;
    const auto& sourceWaveform = source.domains.waveform;
    waveform.line = sourceWaveform.line;
    waveform.fill = sourceWaveform.fill;
    waveform.selectionHighlight = sourceWaveform.selectionHighlight;
    waveform.cursor = sourceWaveform.cursor;
    waveform.regionMarker = sourceWaveform.regionMarker;
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
    meter.green = sourceMeter.green;
    meter.yellow = sourceMeter.yellow;
    meter.red = sourceMeter.red;
    meter.peak = sourceMeter.peak;
    meter.rms = sourceMeter.rms;
    meter.clipOn = sourceMeter.clipOn;
    meter.clipOff = sourceMeter.clipOff;
    meter.background = sourceMeter.background;
    meter.lufsMomentary = sourceMeter.lufsMomentary;
    meter.lufsShortTerm = sourceMeter.lufsShortTerm;
    meter.lufsIntegrated = sourceMeter.lufsIntegrated;
    meter.oscilloscopeGrid = sourceMeter.oscilloscopeGrid;
    meter.oscilloscopeGlow = sourceMeter.oscilloscopeGlow;
    meter.waveformFill = sourceMeter.waveformFill;
    meter.waveformGrid = sourceMeter.waveformGrid;
    meter.stereo = sourceMeter.stereo;
    meter.layout = sourceMeter.layout;
    meter.interaction.colors = sourceMeter.interaction.colors;
    meter.peakMeter.colors = sourceMeter.peakMeter.colors;
    meter.loudnessMeter.colors = sourceMeter.loudnessMeter.colors;
    meter.vuMeter.colors = sourceMeter.vuMeter.colors;
    meter.spectrumMeter.colors = sourceMeter.spectrumMeter.colors;
    meter.spectrogramMeter.colors = sourceMeter.spectrogramMeter.colors;
    meter.peakMeter.metrics.gradientErrorStop = sourceMeter.peakMeter.metrics.gradientErrorStop;
    meter.peakMeter.metrics.gradientWarningStop = sourceMeter.peakMeter.metrics.gradientWarningStop;
    meter.peakMeter.metrics.gradientYellowStop = sourceMeter.peakMeter.metrics.gradientYellowStop;
    meter.peakMeter.metrics.gradientAccentStop = sourceMeter.peakMeter.metrics.gradientAccentStop;
    meter.loudnessMeter.metrics.gradientTopStop = sourceMeter.loudnessMeter.metrics.gradientTopStop;
    meter.loudnessMeter.metrics.gradientBottomStop = sourceMeter.loudnessMeter.metrics.gradientBottomStop;
    meter.loudnessMeter.metrics.gradientTopLighterFactor = sourceMeter.loudnessMeter.metrics.gradientTopLighterFactor;
    meter.loudnessMeter.metrics.gradientBottomDarkerFactor = sourceMeter.loudnessMeter.metrics.gradientBottomDarkerFactor;
    meter.loudnessMeter.metrics.highlightLighterFactor = sourceMeter.loudnessMeter.metrics.highlightLighterFactor;
    meter.loudnessMeter.metrics.highlightAlpha = sourceMeter.loudnessMeter.metrics.highlightAlpha;
    meter.vuMeter.metrics.barOverloadStop = sourceMeter.vuMeter.metrics.barOverloadStop;
    meter.vuMeter.metrics.barWarningStop = sourceMeter.vuMeter.metrics.barWarningStop;
    meter.vuMeter.metrics.barYellowStop = sourceMeter.vuMeter.metrics.barYellowStop;
    meter.vuMeter.metrics.barBrightGreenStop = sourceMeter.vuMeter.metrics.barBrightGreenStop;
    meter.vuMeter.metrics.barGreenStop = sourceMeter.vuMeter.metrics.barGreenStop;
    meter.vuMeter.metrics.barOverloadAlpha = sourceMeter.vuMeter.metrics.barOverloadAlpha;
    meter.vuMeter.metrics.barGreenLighterFactor = sourceMeter.vuMeter.metrics.barGreenLighterFactor;
    meter.spectrumMeter.metrics.fillTopStop = sourceMeter.spectrumMeter.metrics.fillTopStop;
    meter.spectrumMeter.metrics.fillUpperMiddleStop = sourceMeter.spectrumMeter.metrics.fillUpperMiddleStop;
    meter.spectrumMeter.metrics.fillLowerMiddleStop = sourceMeter.spectrumMeter.metrics.fillLowerMiddleStop;
    meter.spectrumMeter.metrics.fillBottomStop = sourceMeter.spectrumMeter.metrics.fillBottomStop;

    target.domains.curve.colors = source.domains.curve.colors;
    target.domains.curve.metrics.discreteGuideAlpha = source.domains.curve.metrics.discreteGuideAlpha;
    target.domains.feedback = source.domains.feedback;
    target.domains.startup = source.domains.startup;
    target.domains.pinDomain = source.domains.pinDomain;
    target.domains.nodeGraphNode = source.domains.nodeGraphNode;
    target.domains.blueprint = source.domains.blueprint;
    target.domains.timeline = source.domains.timeline;
    target.domains.music = source.domains.music;
    target.domains.log = source.domains.log;
    target.domains.monitor = source.domains.monitor;
    target.domains.ai = source.domains.ai;
    target.domains.scene3d = source.domains.scene3d;
    target.domains.spatial = source.domains.spatial;
}

}
