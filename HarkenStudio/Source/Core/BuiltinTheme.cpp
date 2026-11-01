#include "BuiltinTheme.h"
#include "ThemeColorResolver.h"
#include "ThemePackageLoader.h"

#include <QFont>

namespace VNX {

ThemeSnapshot createBuiltinDarkTheme()
{
    ThemeSnapshot theme;
    auto& colors = theme.ui.colors;

    colors.accent = QColor("#00F0FF");
    colors.accentHover = QColor("#33F5FF");
    colors.accentText = QColor("#000000");

    colors.categoricalPalette = {{
        colors.accent,
        QColor("#B69CFF"),
        QColor("#F4C66A"),
        QColor("#6AAEFF"),
        QColor("#FF8A78"),
        QColor("#72D6A0"),
        QColor("#EE8FCB"),
    }};

    colors.bgPrimary = QColor("#0D111A");
    colors.bgSecondary = QColor("#171C28");
    colors.bgTertiary = QColor("#222836");
    colors.bgHover = QColor("#2A3140");
    colors.bgSelected = QColor("#143C48");

    colors.borderPrimary = QColor("#313848");
    colors.borderSecondary = QColor("#3D4554");
    colors.viewBorder = QColor("#272E3C");
    colors.borderTick = colors.viewBorder;
    colors.dropHighlight = QColor(0x30, 0x90, 0xFF, 0x60);
    colors.dropAccent = QColor(0x00, 0xF0, 0xFF, 0xC0);

    colors.textPrimary = QColor("#E0E0E0");
    colors.textSecondary = QColor("#AAAAAA");
    colors.textTitle = colors.textPrimary;
    colors.textMuted = QColor("#666666");
    colors.textDisabled = QColor("#555555");

    colors.success = QColor("#4CAF50");
    colors.warning = QColor("#FF9800");
    colors.error = QColor("#F44336");
    colors.info = QColor("#2196F3");

    colors.trackBackground = QColor("#0F1318");
    colors.trackBackgroundAlt = QColor("#131820");
    colors.busBackground = QColor("#12161E");
    colors.busBackgroundActive = QColor(0, 240, 255, 25);

    colors.sliderTrack = QColor("#2A3140");

    colors.panelBackground = QColor("#10151F");
    colors.sectionSeparator = QColor("#2A3140");
    colors.headerBackground = QColor("#1B2130");

    colors.auditionBg = QColor("#0E1118");
    colors.auditionPlayActive = QColor("#4CAF50");
    colors.auditionStopActive = QColor("#F44336");

    colors.shellMenuText = QColor("#C0C0C0");
    colors.shellMenuHoverText = QColor("#FFFFFF");
    colors.shellButtonText = QColor("#AAB0BA");
    colors.shellButtonHoverText = QColor("#FFFFFF");
    colors.shellCloseHover = QColor("#E81123");
    colors.shellClosePressed = QColor("#F1707A");

    auto& metrics = theme.ui.metrics;
    metrics.radiusSmall = 4;
    metrics.radiusMedium = 6;
    metrics.radiusLarge = 8;
    metrics.spacingXSmall = 3;
    metrics.spacingSmall = 6;
    metrics.spacingMedium = 10;
    metrics.spacingLarge = 14;
    metrics.controlHeight = 24;
    metrics.sectionHeaderHeight = 24;
    metrics.navigationChevronExtent = 20;
    metrics.objectIconExtent = 20;
    metrics.table.headerHeight = 24;
    metrics.table.rowHeight = 30;
    metrics.table.footerHeight = 26;
    metrics.table.cellPaddingX = 4;
    metrics.table.cellPaddingY = 2;
    metrics.table.buttonWidth = 26;
    metrics.table.buttonHeight = 20;
    metrics.table.buttonSpacing = 2;
    metrics.table.compactColumnWidth = 22;
    metrics.table.minimumColumnWidth = 40;
    metrics.table.resizeHandleRadius = 4;
    metrics.inspector.pageMargin = 8;
    metrics.inspector.sectionSpacing = 2;
    metrics.inspector.rowSpacing = 6;
    metrics.inspector.columnPadding = 4;
    metrics.inspector.rowHeight = 24;
    metrics.inspector.labelColumnShare = 300;
    metrics.inspector.valueColumnShare = 700;
    metrics.inspector.formLabelWidth = 80;
    metrics.inspector.valueColumnWidth = 80;
    metrics.inspector.linkSlotWidth = 18;
    metrics.inspector.sourceSlotWidth = 14;
    metrics.inspector.platformSlotWidth = 14;
    metrics.inspector.bindingSlotWidth = 14;
    metrics.inspector.slotHeight = 20;
    metrics.inspector.slotSpacing = 2;
    metrics.inspector.separatorHeight = 1;
    metrics.inspector.rangeSeparatorWidth = 12;
    metrics.parameter.faderMinimumHeight = 96;
    metrics.parameter.labelMinimumWidth = 100;
    metrics.parameter.valueMinimumWidth = 72;
    metrics.parameter.compactValueMinimumWidth = 80;
    metrics.parameter.sliderGrooveThickness = 4;
    metrics.parameter.sliderHandleThickness = 6;
    metrics.parameter.sliderHandleLength = 14;
    metrics.timeline.laneHeaderWidth = 200;
    metrics.timeline.laneHeaderMinimumWidth = 140;
    metrics.timeline.laneHeight = 52;
    metrics.timeline.blankRowHeight = 42;
    metrics.timeline.laneSpacing = 1;
    metrics.timeline.rulerHeight = 28;
    metrics.timeline.markerLaneHeight = 14;
    metrics.timeline.tickLength = 5;
    metrics.timeline.majorTickLength = 10;
    metrics.timeline.minLabelSpacing = 48;
    metrics.timeline.minTickSpacing = 8;
    metrics.timeline.clipVerticalInset = 4;
    metrics.timeline.clipRadius = 2;
    metrics.timeline.clipMinimumWidth = 14;
    metrics.timeline.resizeHandleWidth = 7;
    metrics.timeline.markerHitRadius = 7;
    metrics.timeline.statusBarHeight = 28;
    metrics.timeline.compactHeaderWidth = 180;
    metrics.timeline.queueLaneLabelWidth = 88;
    metrics.timeline.headerGripWidth = 4;

    auto& typography = theme.ui.typography;
    typography.heading.pointSize = 14;
    typography.heading.weight = QFont::Bold;
    typography.title.pointSize = 12;
    typography.title.weight = QFont::DemiBold;
    typography.body.pointSize = 10;
    typography.body.weight = QFont::Normal;
    typography.caption.pointSize = 9;
    typography.caption.weight = QFont::Normal;
    typography.axis.pointSize = 8;
    typography.axis.weight = QFont::Normal;
    typography.mono.pointSize = 10;
    typography.mono.weight = QFont::Normal;
    typography.label.pointSize = 9;
    typography.label.weight = QFont::Medium;

    auto& recipes = theme.ui.recipes.entries;
    const auto makeTrackHeader = [](ComponentRecipeBreakpoint breakpoint) {
        ComponentRecipe track;
        track.host = ComponentRecipeHostId::TrackHeader;
        track.breakpoint = breakpoint;
        track.variant = ComponentRecipeVariant::TrackExpanded;
        const bool compact = breakpoint == ComponentRecipeBreakpoint::Compact;
        const auto add = [&](ComponentRecipeSlotId id,
                             ComponentRecipeAlignment alignment,
                             int minimum,
                             int preferred,
                             int maximum,
                             bool visible = true) {
            ComponentRecipeSlot item;
            item.id = id;
            item.alignment = alignment;
            item.minimum = minimum;
            item.preferred = preferred;
            item.maximum = maximum;
            item.visible = visible;
            track.items.append(item);
        };
        add(ComponentRecipeSlotId::Color, ComponentRecipeAlignment::Fill, 4, 4, 4);
        add(ComponentRecipeSlotId::Identity, ComponentRecipeAlignment::Fill, 48, -1, -1);
        add(ComponentRecipeSlotId::Controls, ComponentRecipeAlignment::Start, 48, 48, 48);
        add(ComponentRecipeSlotId::Routing, ComponentRecipeAlignment::Fill, 48, -1, -1, !compact);
        add(ComponentRecipeSlotId::Params, ComponentRecipeAlignment::Start, 104, 104, 104, !compact);
        add(ComponentRecipeSlotId::Meter, ComponentRecipeAlignment::Fill, -1, -1, -1);
        return track;
    };
    recipes.insert(componentRecipeKey(ComponentRecipeHostId::TrackHeader,
                                      ComponentRecipeBreakpoint::Default),
                   makeTrackHeader(ComponentRecipeBreakpoint::Default));
    recipes.insert(componentRecipeKey(ComponentRecipeHostId::TrackHeader,
                                      ComponentRecipeBreakpoint::Compact),
                   makeTrackHeader(ComponentRecipeBreakpoint::Compact));

    auto& palette = theme.qtPalette;
    palette.window = QColor(0x0B, 0x0E, 0x14);
    palette.base = QColor(0x08, 0x0A, 0x0F);
    palette.alternateBase = QColor(0x10, 0x14, 0x1C);
    palette.toolTipBase = QColor(0x15, 0x19, 0x22);
    palette.button = QColor(0x25, 0x2A, 0x35);
    palette.brightText = QColor(0xFF, 0xFF, 0xFF);
    palette.light = QColor(0xE0, 0xE0, 0xE0);
    palette.dark = QColor(0x88, 0x88, 0x88);
    palette.mid = QColor(0x2A, 0x2E, 0x35);
    palette.disabledHighlight = QColor(0x3C, 0x3C, 0x3C);

    auto& qtAds = theme.thirdParty.qtAds;
    qtAds.chromeBackground = QColor(0x06, 0x08, 0x0D);
    qtAds.inactiveTabText = QColor(0x88, 0x88, 0x88);
    qtAds.closeButtonHover = QColor(0xFF, 0x00, 0x55);
    qtAds.closeButtonPressed = QColor(0xCC, 0x00, 0x44);
    qtAds.lightHoverOverlay = QColor(0xFF, 0xFF, 0xFF, 48);
    qtAds.activeTabAccentOpacity = 0.35;
    qtAds.dropOverlayOpacity = 0.08;

    auto& qtAdsMetrics = qtAds.metrics;
    qtAdsMetrics.borderWidth = 1;
    qtAdsMetrics.titleBarMinimumHeight = 32;
    qtAdsMetrics.titleBarButtonPadding = 3;
    qtAdsMetrics.titleBarButtonRadius = 3;
    qtAdsMetrics.titleBarIconSize = 20;
    qtAdsMetrics.closeButtonPadding = 1;
    qtAdsMetrics.closeButtonExtent = 22;
    qtAdsMetrics.closeButtonRadius = 2;
    qtAdsMetrics.tabBarMinimumHeight = 32;
    qtAdsMetrics.tabAccentBorderWidth = 2;
    qtAdsMetrics.tabGapWidth = 2;
    qtAdsMetrics.tabPaddingTop = 2;
    qtAdsMetrics.tabPaddingHorizontal = 4;
    qtAdsMetrics.tabPaddingBottom = 4;
    qtAdsMetrics.tabMinimumHeight = 24;
    qtAdsMetrics.tabFontSize = 11;
    qtAdsMetrics.tabIconSize = 16;
    qtAdsMetrics.tabCloseMarginTop = 2;
    qtAdsMetrics.tabClosePaddingRight = -2;
    qtAdsMetrics.overlayBorderWidth = 2;
    qtAdsMetrics.splitterHandleExtent = 3;
    qtAdsMetrics.autoHideTabIconSize = 16;
    qtAdsMetrics.autoHideTabPaddingLeft = 2;
    qtAdsMetrics.autoHideTabPaddingRight = 0;
    qtAdsMetrics.autoHideTabMinimumHeight = 20;
    qtAdsMetrics.autoHideSideBarSpacing = 12;
    qtAdsMetrics.autoHideTitleBorderWidth = 2;
    qtAdsMetrics.autoHideTitlePaddingLeft = 4;

    auto& domains = theme.domains;

    domains.startup.wavePalette = {{
        QColor("#06121F"),
        QColor("#173D5C"),
        QColor("#5F9EC7"),
        QColor("#E4F3FF"),
    }};
    domains.startup.logoPalette = {{
        QColor(181, 181, 181), QColor(211, 211, 213), QColor(255, 255, 255),
    }};

    domains.meter.green = QColor(0x00, 0xC8, 0x53);
    domains.meter.yellow = QColor(0xFF, 0xD6, 0x00);
    domains.meter.red = QColor(0xFF, 0x17, 0x44);
    domains.meter.peak = QColor(0xFF, 0x57, 0x22);
    domains.meter.rms = QColor(0x4C, 0xAF, 0x50);
    domains.meter.clipOn = QColor(0xFF, 0x00, 0x00);
    domains.meter.clipOff = QColor(0x66, 0x33, 0x33);
    domains.meter.background = QColor(0x1A, 0x1A, 0x1A);
    domains.meter.oscilloscopeGrid = QColor(
        colors.textPrimary.red(), colors.textPrimary.green(),
        colors.textPrimary.blue(), 15);
    domains.meter.oscilloscopeGlow = QColor(
        colors.textPrimary.red(), colors.textPrimary.green(),
        colors.textPrimary.blue(), 60);
    domains.meter.waveformFill = QColor(
        colors.textPrimary.red(), colors.textPrimary.green(),
        colors.textPrimary.blue(), 180);
    domains.meter.waveformGrid = QColor(
        colors.textPrimary.red(), colors.textPrimary.green(),
        colors.textPrimary.blue(), 40);

    domains.meter.stereo.trailOuter = QColor(255, 220, 100, 12);
    domains.meter.stereo.trailMiddle = QColor(255, 220, 100, 30);
    domains.meter.stereo.trailCore = QColor(255, 240, 180, 55);
    domains.meter.stereo.gridPrimary = QColor(colors.textPrimary.red(), colors.textPrimary.green(),
                                               colors.textPrimary.blue(), 25);
    domains.meter.stereo.gridSecondary = QColor(colors.textPrimary.red(), colors.textPrimary.green(),
                                                 colors.textPrimary.blue(), 12);
    domains.meter.stereo.liveGlowWide = QColor(255, 220, 100, 38);
    domains.meter.stereo.liveGlowMedium = QColor(255, 220, 100, 107);
    domains.meter.stereo.liveHighlight = QColor(255, 240, 200, 217);
    domains.meter.stereo.correlationPositive = QColor(0x00, 0xCC, 0x66, 180);
    domains.meter.stereo.correlationNegative = QColor(0xFF, 0x44, 0x44, 180);

    domains.meter.layout.dividerHover = QColor(0x00, 0x8C, 0xFF, 140);
    domains.meter.layout.dividerDragging = QColor(0x00, 0xB4, 0xFF, 200);
    domains.meter.layout.dividerSnapped = QColor(0x00, 0xFF, 0xC8, 220);
    domains.meter.layout.dropBorder = QColor(0x00, 0xC8, 0xFF, 200);
    domains.meter.layout.dropFill = QColor(0x00, 0xC8, 0xFF, 30);

    auto& meterInteractionColors = domains.meter.interaction.colors;
    meterInteractionColors.accentGuide = QColor(0xFF, 0x88, 0x00);

    auto& meterInteractionMetrics = domains.meter.interaction.metrics;
    meterInteractionMetrics.crosshairLineWidth = 1.0;
    meterInteractionMetrics.accentGuideLineWidth = 1.0;
    meterInteractionMetrics.horizontalPadding = 16.0;
    meterInteractionMetrics.lineHeight = 22.0;
    meterInteractionMetrics.extraHeight = 6.0;
    meterInteractionMetrics.horizontalOffset = 10.0;
    meterInteractionMetrics.aboveGap = 6.0;
    meterInteractionMetrics.belowOffset = 10.0;
    meterInteractionMetrics.radius = 4.0;

    auto& peakColors = domains.meter.peakMeter.colors;
    peakColors.gradientError = colors.error;
    peakColors.gradientWarning = domains.meter.yellow;
    peakColors.gradientYellow = domains.meter.yellow;
    peakColors.gradientAccent = colors.accent;
    peakColors.rmsOverlay = QColor(0xFF, 0xFF, 0xFF, 0x3C);
    peakColors.holdHalo = QColor(0xE0, 0xE0, 0xE0, 0x19);
    peakColors.holdLine = QColor(0xE0, 0xE0, 0xE0, 0xEA);
    peakColors.clipBackground = colors.error;
    peakColors.clipText = QColor(0xFF, 0xFF, 0xFF);

    auto& peakMetrics = domains.meter.peakMeter.metrics;
    peakMetrics.outerInsetLeft = 4;
    peakMetrics.outerInsetTop = 3;
    peakMetrics.outerInsetRight = 4;
    peakMetrics.outerInsetBottom = 3;
    peakMetrics.channelGap = 1;
    peakMetrics.reservedWidth = 1;
    peakMetrics.barHorizontalInset = 0.0;
    peakMetrics.gradientErrorStop = 6.0 / 102.0;
    peakMetrics.gradientWarningStop = 12.0 / 102.0;
    peakMetrics.gradientYellowStop = 18.0 / 102.0;
    peakMetrics.gradientAccentStop = 30.0 / 102.0;
    peakMetrics.rmsLeftInsetRatio = 0.2;
    peakMetrics.rmsWidthRatio = 0.6;
    peakMetrics.holdHaloTopOffset = -2.5;
    peakMetrics.holdHaloHeight = 6.0;
    peakMetrics.holdLineTopOffset = -0.5;
    peakMetrics.holdLineHeight = 2.0;
    peakMetrics.gridLineWidth = 1.0;
    peakMetrics.tickLeftOffset = 28;
    peakMetrics.tickWidth = 26;
    peakMetrics.tickHalfHeight = 6.0;
    peakMetrics.tickHeight = 12.0;
    peakMetrics.clipHeight = 18;
    peakMetrics.channelLabelBottomOffset = 30;
    peakMetrics.valueLabelBottomOffset = 16;
    peakMetrics.labelHeight = 14;

    auto& spectrumColors = domains.meter.spectrumMeter.colors;
    spectrumColors.fillTop = colors.accent;
    spectrumColors.fillTop.setAlpha(160);
    spectrumColors.fillUpperMiddle = colors.info;
    spectrumColors.fillUpperMiddle.setAlpha(120);
    spectrumColors.fillLowerMiddle = colors.accent;
    spectrumColors.fillLowerMiddle.setAlpha(80);
    spectrumColors.fillBottom = colors.accent;
    spectrumColors.fillBottom.setAlpha(24);
    spectrumColors.lineGlowWide = colors.accent;
    spectrumColors.lineGlowWide.setAlpha(15);
    spectrumColors.lineGlowMedium = colors.accent;
    spectrumColors.lineGlowMedium.setAlpha(40);
    spectrumColors.lineGlowNarrow = colors.accent;
    spectrumColors.lineGlowNarrow.setAlpha(120);
    spectrumColors.lineHighlight = colors.accentHover;
    spectrumColors.lineHighlight.setAlpha(220);
    spectrumColors.peakLine = QColor(0xFF, 0xFF, 0xFF);
    spectrumColors.peakLine.setAlpha(128);

    auto& spectrumMetrics = domains.meter.spectrumMeter.metrics;
    spectrumMetrics.plotInsetLeft = 30;
    spectrumMetrics.plotInsetTop = 10;
    spectrumMetrics.plotInsetRight = 10;
    spectrumMetrics.plotInsetBottom = 20;
    spectrumMetrics.fillTopStop = 0.0;
    spectrumMetrics.fillUpperMiddleStop = 0.3;
    spectrumMetrics.fillLowerMiddleStop = 0.6;
    spectrumMetrics.fillBottomStop = 1.0;
    spectrumMetrics.curveControlRatio = 0.4f;
    spectrumMetrics.lineGlowWideWidth = 6.0;
    spectrumMetrics.lineGlowMediumWidth = 3.5;
    spectrumMetrics.lineGlowNarrowWidth = 1.8;
    spectrumMetrics.lineHighlightWidth = 0.8;
    spectrumMetrics.peakLineWidth = 1.0;
    spectrumMetrics.gridLineWidth = 1.0;
    spectrumMetrics.dbLabelLeft = 0.0;
    spectrumMetrics.dbLabelHalfHeight = 6.0;
    spectrumMetrics.dbLabelWidth = 28.0;
    spectrumMetrics.dbLabelHeight = 12.0;
    spectrumMetrics.frequencyLabelHalfWidth = 20.0;
    spectrumMetrics.frequencyLabelTopOffset = 2.0;
    spectrumMetrics.frequencyLabelWidth = 40.0;
    spectrumMetrics.frequencyLabelHeight = 14.0;

    auto& spectrogramColors = domains.meter.spectrogramMeter.colors;
    spectrogramColors.emptyHistory = colors.bgPrimary;

    auto& spectrogramMetrics = domains.meter.spectrogramMeter.metrics;
    spectrogramMetrics.plotInsetLeft = 30;
    spectrogramMetrics.plotInsetTop = 10;
    spectrogramMetrics.plotInsetRight = 10;
    spectrogramMetrics.plotInsetBottom = 10;
    spectrogramMetrics.gridLineWidth = 1.0;
    spectrogramMetrics.frequencyLabelLeft = 0.0;
    spectrogramMetrics.frequencyLabelHalfHeight = 6.0;
    spectrogramMetrics.frequencyLabelWidth = 28.0;
    spectrogramMetrics.frequencyLabelHeight = 12.0;
    spectrogramMetrics.timeLabelTopOffset = 2.0;
    spectrogramMetrics.timeLabelHeight = 16.0;

    domains.feedback.overlayBackground = QColor(0x10, 0x18, 0x28, 0xF0);
    domains.feedback.overlayErrorBackground = QColor(0x28, 0x10, 0x10, 0xF0);

    domains.waveform.line = QColor(0x00, 0xE6, 0x76);
    domains.waveform.fill = QColor(0x00, 0xE6, 0x76, 0x40);
    domains.waveform.selectionHighlight = QColor(0x00, 0xF0, 0xFF, 0x50);
    domains.waveform.cursor = QColor(0xFF, 0xFF, 0xFF);
    domains.waveform.regionMarker = QColor(0xFF, 0xD6, 0x00);
    domains.waveform.trimHandle = QColor(0x3E, 0x8E, 0xF7);
    domains.waveform.loopHandle = QColor(0x4A, 0xBD, 0x80);
    domains.waveform.staticMode.quiet = QColor(60, 140, 255, 180);
    domains.waveform.staticMode.peak = QColor(60, 255, 255, 180);
    domains.waveform.staticMode.accentQuietAlpha = 100;
    domains.waveform.staticMode.accentPeakAlpha = 255;
    domains.waveform.multiBandMode.colors = {
        QColor(0xFF, 0x00, 0x00),
        QColor(0x00, 0xFF, 0x00),
        QColor(0x00, 0x00, 0xFF),
    };
    domains.waveform.multiBandMode.gains = {1.7f, 0.8f, 2.0f};
    domains.waveform.multiBandMode.alpha = 0.85f;
    domains.waveform.colorMapMode.offset = {0.4f, 0.0f, 0.67f, 0.10f};
    domains.waveform.colorMapMode.slope = {0.6f, 0.27f, -0.34f, 0.75f};
    domains.waveform.splitStereo.divider = QColor(80, 80, 90);
    domains.waveform.splitStereo.label = QColor(120, 120, 140);
    domains.waveform.splitStereo.dividerWidth = 1.0;
    domains.waveform.splitStereo.labelPointSize = 8;
    domains.waveform.splitStereo.labelRightInset = 18;
    domains.waveform.splitStereo.labelBaselineOffset = 12;
    domains.waveform.rawPeaks.background = QColor(11, 14, 20, 120);
    domains.waveform.rawPeaks.centerLine = QColor(60, 60, 60, 100);
    domains.waveform.rawPeaks.cornerRadius = 2.0;
    domains.waveform.rawPeaks.centerLineWidth = 0.5;
    domains.waveform.dbScale.gridWidth = 1.0;
    domains.waveform.dbScale.labelPointSize = 8;
    domains.waveform.dbScale.amplitudeScale = 0.9;
    domains.waveform.dbScale.labelLeftInset = 2;
    domains.waveform.dbScale.labelBaselineOffset = -1;
    domains.waveform.amplitudeScale = 0.9;

    auto& curveColors = domains.curve.colors;
    curveColors.attenuationVolume = colors.accent;
    curveColors.attenuationPitch = QColor(0x4A, 0x9D, 0xE0);
    curveColors.parameterBinding = colors.accent;
    curveColors.gridFade = colors.accent;
    curveColors.crosshair = colors.borderTick;
    domains.meter.interaction.colors.crosshair = colors.borderTick;
    curveColors.ghost = QColor(0xFF, 0xFF, 0xFF, 90);
    curveColors.lassoBorder = QColor(0xFF, 0xFF, 0xFF, 150);
    curveColors.lassoFill = QColor(0xFF, 0xFF, 0xFF, 28);
    curveColors.playhead = QColor(0xC8, 0xD2, 0xDC, 90);

    auto& curveMetrics = domains.curve.metrics;
    curveMetrics.linearGraphLeftInset = 40;
    curveMetrics.amplitudeGraphLeftInset = 40;
    curveMetrics.graphTopInset = 10;
    curveMetrics.graphRightInset = 10;
    curveMetrics.graphBottomInset = 26;
    curveMetrics.gridColumnCount = 10;
    curveMetrics.linearGridRowCount = 10;
    curveMetrics.amplitudeGridTargetRowSpacing = 32.0;
    curveMetrics.amplitudeGridMinimumRows = 8;
    curveMetrics.amplitudeGridMaximumRows = 32;
    curveMetrics.gridLineWidth = 1.0;
    curveMetrics.plotBorderWidth = 1.0;
    curveMetrics.curveLineWidth = 1.5;
    curveMetrics.discreteGuideLineWidth = 1.0;
    curveMetrics.discreteGuideAlpha = 75;
    curveMetrics.ghostLineWidth = 1.0;
    curveMetrics.lassoBorderWidth = 1.0;
    curveMetrics.handleLineWidth = 1.0;
    curveMetrics.handleBorderWidth = 1.5;
    curveMetrics.pointBorderWidth = 1.5;
    curveMetrics.crosshairLineWidth = 1.0;
    curveMetrics.ghostRadius = 2.5;
    curveMetrics.handleRadius = 3.0;
    curveMetrics.pointRadius = 4.0;
    curveMetrics.pointSelectedRadius = 5.0;
    curveMetrics.crosshairDashLength = 2.0;
    curveMetrics.crosshairDashGap = 3.0;
    curveMetrics.xAxisLabelTopOffset = 8.0;
    curveMetrics.xAxisLabelHeight = 18.0;
    curveMetrics.yAxisLabelCenterX = 12.0;
    curveMetrics.yAxisLabelWidth = 120.0;
    curveMetrics.yAxisLabelHeight = 18.0;
    curveMetrics.amplitudeTickLabelLeft = 30.0;
    curveMetrics.amplitudeTickLabelRightGap = 6.0;
    curveMetrics.amplitudeTickLabelHalfHeight = 8.0;
    curveMetrics.amplitudeTickLabelHeight = 16.0;
    curveMetrics.linearYTickLeftOffset = 40.0;
    curveMetrics.linearYTickBaselineOffset = 4.0;
    curveMetrics.xMinTickLeftOffset = 4.0;
    curveMetrics.xTickBaselineOffset = 18.0;
    curveMetrics.xMaxLabelWidth = 96.0;
    curveMetrics.xMaxLabelTopOffset = 6.0;
    curveMetrics.xMaxLabelHeight = 18.0;
    curveMetrics.xMaxEditorVerticalExpansion = 3.0;
    curveMetrics.readoutFontPixelSize = 12;
    curveMetrics.readoutHorizontalPadding = 8.0;
    curveMetrics.readoutHeight = 20.0;
    curveMetrics.readoutHorizontalOffset = 6.0;
    curveMetrics.readoutAboveOffset = 21.0;
    curveMetrics.readoutBelowOffset = 9.0;
    curveMetrics.readoutRadius = 2.0;

    domains.pinDomain.audio = QColor(0x00, 0xF0, 0xFF);
    domains.pinDomain.control = QColor(0x9B, 0x6E, 0xFF);
    domains.pinDomain.value = QColor(0xFF, 0xB8, 0x00);
    domains.pinDomain.ring = QColor(0x0B, 0x0E, 0x14);
    domains.pinDomain.targetOk = QColor(0x3D, 0xBA, 0x6E);
    domains.pinDomain.targetBad = QColor(0xDC, 0x3C, 0x3C);

    domains.nodeGraphNode.borderNormal = QColor(0x10, 0x11, 0x14);
    domains.nodeGraphNode.borderHoverMix = QColor(0xFF, 0xFF, 0xFF);
    domains.nodeGraphNode.dropTarget = QColor(0x3D, 0xBA, 0x6E);
    domains.nodeGraphNode.shadow = QColor(0x00, 0x00, 0x00, 0x3C);
    domains.nodeGraphNode.sandboxBadgeBg = QColor(0xDC, 0x3C, 0x3C, 0xC8);
    domains.nodeGraphNode.sandboxBadgeText = QColor(0xFF, 0xFF, 0xFF);
    domains.nodeGraphNode.badgeOverride = QColor(0xFF, 0x98, 0x00);
    domains.nodeGraphNode.badgeChild = QColor(0x9B, 0x6E, 0xFF);
    domains.nodeGraphNode.badgeReceive = QColor(0xFF, 0x4D, 0x8F);
    domains.nodeGraphNode.waveformBg = QColor(0x16, 0x17, 0x1B);
    domains.nodeGraphNode.waveformCenter = QColor(0x3C, 0x3C, 0x3C, 0x64);
    domains.nodeGraphNode.blueprintWaveform = QColor(0x0F, 0xB5, 0xA8, 0xCC);

    domains.blueprint.canvasBackground = QColor(0x0B, 0x0E, 0x14);
    domains.blueprint.gridDot = QColor(0x33, 0x3C, 0x4D);
    domains.blueprint.edgeLabelBg = QColor(0x0B, 0x0E, 0x14, 0xE6);
    domains.blueprint.placementChain = QColor(0x00, 0xF0, 0xFF);
    domains.blueprint.organizationChain = QColor(0x9B, 0x6E, 0xFF);
    domains.blueprint.containment = QColor(0x64, 0x64, 0x64);
    domains.blueprint.busRoute = QColor(0x3D, 0xBA, 0x6E);
    domains.blueprint.send = QColor(0xFF, 0x4D, 0x8F);
    domains.blueprint.overrideRoute = QColor(0xFF, 0x98, 0x00);

    domains.timeline.entry = QColor(74, 189, 128);
    domains.timeline.exit = QColor(228, 112, 97);
    domains.timeline.transition = QColor(232, 184, 74);
    domains.timeline.clip = QColor(54, 105, 168);
    domains.timeline.clipSelected = QColor(62, 142, 247);
    domains.timeline.clipBorder = QColor(91, 153, 215);
    domains.timeline.clipSelectedBorder = QColor(190, 222, 255);
    domains.timeline.clipText = QColor(230, 240, 255);
    domains.timeline.badge = QColor(181, 92, 35);
    domains.timeline.emergenceMarker = QColor(255, 190, 64);
    domains.timeline.unavailableOverlay = QColor(128, 128, 128, 28);
    domains.timeline.unavailableText = QColor(255, 255, 255, 70);
    domains.timeline.outOfSpanVeil = QColor(0x0B, 0x0E, 0x14, 0x66);

    domains.music.trackColors = {{
        QColor(0x42, 0xA5, 0xF5),
        QColor(0xEF, 0x53, 0x50),
        QColor(0x66, 0xBB, 0x6A),
        QColor(0xFF, 0xCA, 0x28),
        QColor(0xAB, 0x47, 0xBC),
        QColor(0x26, 0xC6, 0xDA),
        QColor(0xFF, 0x70, 0x43),
        QColor(0x78, 0x90, 0x9C),
        QColor(0xEC, 0x40, 0x7A),
        QColor(0x9C, 0xCC, 0x65),
        QColor(0x5C, 0x6B, 0xC0),
        QColor(0xFF, 0xEE, 0x58),
    }};
    domains.music.beatGridMajor = QColor(0xFF, 0xFF, 0xFF, 0x30);
    domains.music.beatGridMinor = QColor(0xFF, 0xFF, 0xFF, 0x12);
    domains.music.cursor = QColor(0xFF, 0xFF, 0xFF);
    domains.music.loopRegion = QColor(0x00, 0xF0, 0xFF, 0x28);

    domains.log.debug = QColor(0x80, 0x80, 0x80);
    domains.log.info = QColor(0xBB, 0xBB, 0xBB);
    domains.log.warning = QColor(0xFF, 0xB7, 0x4D);
    domains.log.error = QColor(0xFF, 0x44, 0x44);
    domains.log.critical = QColor(0xFF, 0x00, 0x00);

    domains.monitor.stateStolen = QColor(0xFF, 0x50, 0x50);
    domains.monitor.statePlaying = QColor(0x64, 0xC8, 0x64);
    domains.monitor.stateStarting = QColor(0xB4, 0xDC, 0x64);
    domains.monitor.stateVirtual = QColor(0x64, 0x64, 0xC8);
    domains.monitor.stateReleasing = QColor(0xFF, 0xC8, 0x64);
    domains.monitor.stateStopping = QColor(0xC8, 0x64, 0x64);
    domains.monitor.statePaused = QColor(0xA0, 0x8C, 0xC8);
    domains.monitor.stateDefault = QColor(0x96, 0x96, 0x96);
    domains.monitor.virtualDim = QColor(0x80, 0x80, 0x80);
    domains.monitor.mutedText = QColor(0xB4, 0x64, 0x64);
    domains.monitor.lowVolume = QColor(0x64, 0x64, 0x64);
    domains.monitor.busIdText = QColor(0x96, 0x96, 0xB4);
    domains.monitor.peakClip = QColor(0xF4, 0x43, 0x36);
    domains.monitor.peakCaution = QColor(0xFF, 0xC8, 0x64);
    domains.monitor.peakSafe = QColor(0x64, 0xC8, 0x64);
    domains.monitor.pitchHigh = QColor(0xFF, 0x64, 0x64);
    domains.monitor.pitchLow = QColor(0x64, 0x64, 0xFF);
    domains.monitor.cpuCritical = QColor(0xF4, 0x43, 0x36);
    domains.monitor.cpuWarning = QColor(0xFF, 0x98, 0x00);
    domains.monitor.cpuSafe = QColor(0x4C, 0xAF, 0x50);
    domains.monitor.bufferWarning = QColor(0xFF, 0x98, 0x00);
    domains.monitor.bufferNormal = QColor(0x21, 0x96, 0xF3);
    domains.monitor.underrunFlash = QColor(0xFF, 0x3C, 0x3C);
    domains.monitor.underrunIdle = QColor(0xC8, 0x78, 0x3C);
    domains.monitor.underrunNone = QColor(0x78, 0x78, 0x78);
    domains.monitor.spatialGainHigh = QColor(0x64, 0xC8, 0x64);
    domains.monitor.spatialGainLow = QColor(0xC8, 0x96, 0x32);
    domains.monitor.spatialLpf = QColor(0x64, 0x96, 0xC8);
    domains.monitor.spatialWidth = QColor(0xC8, 0x64, 0xC8);
    domains.monitor.spatialDistance = QColor(0xC8, 0xC8, 0x64);
    domains.monitor.spatialReverb = QColor(0x64, 0xC8, 0xC8);
    domains.monitor.spatialDopplerHi = QColor(0xFF, 0x64, 0x64);
    domains.monitor.spatialDopplerLo = QColor(0x64, 0x64, 0xFF);
    domains.monitor.spatialDot = QColor(0x00, 0xF0, 0xFF);

    domains.ai.brandAccent = QColor(0x00, 0xF0, 0xFF);
    domains.ai.userAccent = QColor(0xFF, 0x6B, 0x35);

    domains.scene3d.listener = QColor(61, 173, 232);
    domains.scene3d.listenerCone = QColor(102, 204, 255);
    domains.scene3d.room = QColor(102, 102, 153);
    domains.scene3d.emitterMin = QColor(0, 255, 0);
    domains.scene3d.emitterMax = QColor(255, 0, 0);
    domains.scene3d.hudBackground = QColor(30, 30, 35, 200);
    domains.scene3d.hudTitle = QColor(200, 200, 200);
    domains.scene3d.hudLabel = QColor(150, 150, 150);
    domains.scene3d.hudGain = QColor(100, 200, 100);
    domains.scene3d.hudGainLow = QColor(200, 150, 50);
    domains.scene3d.hudLpf = QColor(100, 150, 200);
    domains.scene3d.hudWidth = QColor(200, 100, 200);
    domains.scene3d.dopplerIdle = QColor(100, 100, 100);

    domains.spatial.background = QColor(26, 26, 26);
    domains.spatial.grid = QColor(40, 40, 40);
    domains.spatial.axis = QColor(80, 80, 80);
    domains.spatial.axisLabel = QColor(160, 160, 160);
    domains.spatial.tangentLine = QColor(100, 100, 100);
    domains.spatial.tangentHover = QColor(150, 150, 150);
    domains.spatial.tangentActive = QColor(255, 200, 100);
    domains.spatial.pointSelected = QColor(255, 255, 100);
    domains.spatial.previewMarker = QColor(200, 200, 200, 150);
    domains.spatial.infoText = QColor(180, 180, 180);
    domains.spatial.legendBg = QColor(80, 80, 80);
    domains.spatial.layerVolume = QColor(231, 76, 60);
    domains.spatial.layerLpf = QColor(52, 152, 219);
    domains.spatial.layerWidth = QColor(46, 204, 113);
    domains.spatial.layerReverb = QColor(241, 196, 15);

    ThemeColorOverrides roleColors;
    for (const auto& role : studioDefaultColorRoles())
        roleColors.insert(role.key, readThemeColor(theme, role.key));
    resolveThemeColors(theme, studioDefaultColorRoles(), roleColors);
    return theme;
}

ThemeSnapshot createBuiltinLightTheme()
{
    ThemeSnapshot theme = createBuiltinDarkTheme();
    theme.colorScheme = ThemeColorScheme::Light;
    auto& colors = theme.ui.colors;
    colors.accent = QColor("#007B8B");
    colors.accentHover = QColor("#006674");
    colors.accentText = QColor("#FFFFFF");
    colors.categoricalPalette = {{
        colors.accent, QColor("#7753B8"), QColor("#A87312"), QColor("#326EB4"),
        QColor("#C05A48"), QColor("#2B8352"), QColor("#AD4D86"),
    }};
    colors.bgPrimary = QColor("#F4F6F9");
    colors.bgSecondary = QColor("#FFFFFF");
    colors.bgTertiary = QColor("#E8EDF3");
    colors.bgHover = QColor("#DCE5EE");
    colors.bgSelected = QColor("#D0E8EE");
    colors.borderPrimary = QColor("#BCC7D3");
    colors.borderSecondary = QColor("#8D9EAF");
    colors.viewBorder = QColor("#B9C5D1");
    colors.borderTick = colors.viewBorder;
    colors.dropHighlight = QColor("#40326EB4");
    colors.dropAccent = QColor("#C0007B8B");
    colors.textPrimary = QColor("#202B38");
    colors.textSecondary = QColor("#506174");
    colors.textTitle = colors.textPrimary;
    colors.textMuted = QColor("#697889");
    colors.textDisabled = QColor("#98A4B1");
    colors.success = QColor("#217A45");
    colors.warning = QColor("#A76400");
    colors.error = QColor("#C53642");
    colors.info = QColor("#176DB0");
    colors.trackBackground = QColor("#F8FAFC");
    colors.trackBackgroundAlt = QColor("#EEF3F7");
    colors.busBackground = QColor("#E7EEF4");
    colors.busBackgroundActive = QColor("#20007B8B");
    colors.sliderTrack = QColor("#B7C5D3");
    colors.panelBackground = QColor("#EEF2F6");
    colors.sectionSeparator = QColor("#C5CFDA");
    colors.headerBackground = QColor("#E2E8EF");
    colors.auditionBg = QColor("#E8EEF4");
    colors.auditionPlayActive = colors.success;
    colors.auditionStopActive = colors.error;
    colors.shellMenuText = colors.textSecondary;
    colors.shellMenuHoverText = colors.textPrimary;
    colors.shellButtonText = colors.textSecondary;
    colors.shellButtonHoverText = colors.textPrimary;
    colors.shellCloseHover = QColor("#C93042");
    colors.shellClosePressed = QColor("#A92234");

    auto& palette = theme.qtPalette;
    palette.window = QColor("#EEF2F6");
    palette.base = QColor("#FFFFFF");
    palette.alternateBase = QColor("#F1F5F9");
    palette.toolTipBase = QColor("#FAFCFE");
    palette.button = QColor("#E8EDF3");
    palette.brightText = QColor("#FFFFFF");
    palette.light = QColor("#FFFFFF");
    palette.dark = QColor("#8696A7");
    palette.mid = QColor("#B6C2D0");
    palette.disabledHighlight = QColor("#CAD5E0");

    auto& qtAds = theme.thirdParty.qtAds;
    qtAds.chromeBackground = QColor("#DCE4EC");
    qtAds.inactiveTabText = QColor("#66788A");
    qtAds.closeButtonHover = QColor("#C93042");
    qtAds.closeButtonPressed = QColor("#A92234");
    qtAds.lightHoverOverlay = QColor("#1834485C");
    qtAds.activeTabAccentOpacity = 0.16;
    qtAds.dropOverlayOpacity = 0.10;

    auto& domains = theme.domains;
    domains.startup.wavePalette = {{
        QColor("#BACFDF"), QColor("#92B6CD"), QColor("#578BAA"), QColor("#256585"),
    }};
    domains.startup.logoPalette = {{
        QColor("#344B60"), QColor("#286B83"), QColor("#007B8B"),
    }};

    auto& meter = domains.meter;
    meter.green = QColor("#23814E");
    meter.yellow = QColor("#B48712");
    meter.red = QColor("#C33047");
    meter.peak = QColor("#BF5930");
    meter.rms = QColor("#397D54");
    meter.clipOn = QColor("#C3263C");
    meter.clipOff = QColor("#D8B4BA");
    meter.background = QColor("#E8EEF4");
    meter.oscilloscopeGrid = QColor("#28202B38");
    meter.oscilloscopeGlow = QColor("#50202B38");
    meter.waveformFill = QColor("#B4202B38");
    meter.waveformGrid = QColor("#30202B38");
    meter.stereo.trailOuter = QColor("#18A36B12");
    meter.stereo.trailMiddle = QColor("#30A36B12");
    meter.stereo.trailCore = QColor("#508B5810");
    meter.stereo.gridPrimary = QColor("#30202B38");
    meter.stereo.gridSecondary = QColor("#18202B38");
    meter.stereo.liveGlowWide = QColor("#28A36B12");
    meter.stereo.liveGlowMedium = QColor("#70A36B12");
    meter.stereo.liveHighlight = QColor("#E08B5810");
    meter.stereo.correlationPositive = QColor("#C0217A45");
    meter.stereo.correlationNegative = QColor("#C0C53642");
    meter.layout.dividerHover = QColor("#A0326EB4");
    meter.layout.dividerDragging = QColor("#D0007B8B");
    meter.layout.dividerSnapped = QColor("#E0217A45");
    meter.layout.dropBorder = QColor("#D0007B8B");
    meter.layout.dropFill = QColor("#22007B8B");
    meter.interaction.colors.accentGuide = QColor("#A76400");

    auto& peak = meter.peakMeter.colors;
    peak.gradientAccent = QColor("#008E94");
    peak.gradientYellow = QColor("#A99216");
    peak.gradientWarning = QColor("#C17B13");
    peak.gradientError = colors.error;
    peak.rmsOverlay = QColor("#60203040");
    peak.holdHalo = QColor("#30334557");
    peak.holdLine = QColor("#E0334557");
    peak.clipBackground = colors.error;
    peak.clipText = QColor("#FFFFFF");

    auto& spectrum = meter.spectrumMeter.colors;
    spectrum.fillTop = QColor("#80007B8B");
    spectrum.fillUpperMiddle = QColor("#60326EB4");
    spectrum.fillLowerMiddle = QColor("#40007B8B");
    spectrum.fillBottom = QColor("#12007B8B");
    spectrum.lineGlowWide = QColor("#18007B8B");
    spectrum.lineGlowMedium = QColor("#30007B8B");
    spectrum.lineGlowNarrow = QColor("#70007B8B");
    spectrum.lineHighlight = QColor("#E0006374");
    spectrum.peakLine = QColor("#A0506174");
    meter.spectrogramMeter.colors.emptyHistory = colors.bgPrimary;

    domains.feedback.overlayBackground = QColor("#F0E8F0F8");
    domains.feedback.overlayErrorBackground = QColor("#F0F8E7E9");

    auto& waveform = domains.waveform;
    waveform.line = QColor("#23814E");
    waveform.fill = QColor("#4023814E");
    waveform.selectionHighlight = QColor("#35007B8B");
    waveform.cursor = colors.textPrimary;
    waveform.regionMarker = QColor("#A87312");
    waveform.trimHandle = QColor("#326EB4");
    waveform.loopHandle = QColor("#278250");
    waveform.staticMode.quiet = QColor("#B43078AD");
    waveform.staticMode.peak = QColor("#C0007E8A");
    waveform.multiBandMode.colors = {
        QColor("#C64B4B"), QColor("#388C4F"), QColor("#3B64B7"),
    };
    waveform.colorMapMode.offset = {0.05f, 0.25f, 0.50f, 0.18f};
    waveform.colorMapMode.slope = {0.0f, 0.35f, 0.15f, 0.75f};
    waveform.splitStereo.divider = QColor("#AEBCCA");
    waveform.splitStereo.label = colors.textSecondary;
    waveform.rawPeaks.background = QColor("#90E1E8F0");
    waveform.rawPeaks.centerLine = QColor("#60607488");

    auto& curve = domains.curve.colors;
    curve.attenuationVolume = colors.accent;
    curve.attenuationPitch = QColor("#326EB4");
    curve.parameterBinding = QColor("#7753B8");
    curve.gridFade = colors.accent;
    curve.crosshair = colors.borderTick;
    meter.interaction.colors.crosshair = colors.borderTick;
    curve.ghost = QColor("#70506174");
    curve.lassoBorder = QColor("#A0326EB4");
    curve.lassoFill = QColor("#20326EB4");
    curve.playhead = QColor("#80202B38");

    domains.pinDomain.audio = colors.accent;
    domains.pinDomain.control = QColor("#7753B8");
    domains.pinDomain.value = QColor("#A87312");
    domains.pinDomain.ring = QColor("#F8FAFC");
    domains.pinDomain.targetOk = colors.success;
    domains.pinDomain.targetBad = colors.error;

    auto& node = domains.nodeGraphNode;
    node.borderNormal = QColor("#A9B7C6");
    node.borderHoverMix = QColor("#34485C");
    node.dropTarget = colors.success;
    node.shadow = QColor("#22304458");
    node.sandboxBadgeBg = QColor("#E0C53642");
    node.sandboxBadgeText = QColor("#FFFFFF");
    node.badgeOverride = QColor("#B36C00");
    node.badgeChild = QColor("#7753B8");
    node.badgeReceive = QColor("#B54C80");
    node.waveformBg = QColor("#F8FAFC");
    node.waveformCenter = QColor("#60A0AFBE");
    node.blueprintWaveform = QColor("#D0007B80");

    auto& blueprint = domains.blueprint;
    blueprint.canvasBackground = QColor("#F5F8FB");
    blueprint.gridDot = QColor("#C1CEDA");
    blueprint.edgeLabelBg = QColor("#EBF5F8FB");
    blueprint.placementChain = colors.accent;
    blueprint.organizationChain = QColor("#7753B8");
    blueprint.containment = QColor("#74859A");
    blueprint.busRoute = colors.success;
    blueprint.send = QColor("#B54C80");
    blueprint.overrideRoute = QColor("#B36C00");

    auto& timeline = domains.timeline;
    timeline.entry = QColor("#278250");
    timeline.exit = QColor("#C05242");
    timeline.transition = QColor("#A87312");
    timeline.clip = QColor("#C6DDF0");
    timeline.clipSelected = QColor("#77AED8");
    timeline.clipBorder = QColor("#7CA4C7");
    timeline.clipSelectedBorder = QColor("#1C639C");
    timeline.clipText = QColor("#24425F");
    timeline.badge = QColor("#A45C22");
    timeline.emergenceMarker = QColor("#AF7915");
    timeline.unavailableOverlay = QColor("#30758595");
    timeline.unavailableText = QColor("#90607080");
    timeline.outOfSpanVeil = QColor("#80EDF2F7");

    domains.music.trackColors = {{
        QColor("#337CB3"), QColor("#C14E4B"), QColor("#39834D"), QColor("#A88214"),
        QColor("#9152A1"), QColor("#228592"), QColor("#BC6639"), QColor("#657F8C"),
        QColor("#B74775"), QColor("#718B3C"), QColor("#596BA6"), QColor("#948417"),
    }};
    domains.music.beatGridMajor = QColor("#40506174");
    domains.music.beatGridMinor = QColor("#20506174");
    domains.music.cursor = colors.textPrimary;
    domains.music.loopRegion = QColor("#28007B8B");

    domains.log.debug = colors.textMuted;
    domains.log.info = colors.textPrimary;
    domains.log.warning = colors.warning;
    domains.log.error = colors.error;
    domains.log.critical = QColor("#A91930");

    auto& monitor = domains.monitor;
    monitor.stateStolen = QColor("#B92F42");
    monitor.statePlaying = QColor("#278250");
    monitor.stateStarting = QColor("#6C851B");
    monitor.stateVirtual = QColor("#6266AE");
    monitor.stateReleasing = QColor("#AA7518");
    monitor.stateStopping = QColor("#A14C50");
    monitor.statePaused = QColor("#80619D");
    monitor.stateDefault = QColor("#6B7B8C");
    monitor.virtualDim = QColor("#8795A4");
    monitor.mutedText = QColor("#A65D63");
    monitor.lowVolume = QColor("#6D7B89");
    monitor.busIdText = QColor("#65748D");
    monitor.peakClip = colors.error;
    monitor.peakCaution = colors.warning;
    monitor.peakSafe = colors.success;
    monitor.pitchHigh = QColor("#BB4E52");
    monitor.pitchLow = QColor("#545AB1");
    monitor.cpuCritical = colors.error;
    monitor.cpuWarning = colors.warning;
    monitor.cpuSafe = colors.success;
    monitor.bufferWarning = colors.warning;
    monitor.bufferNormal = colors.info;
    monitor.underrunFlash = QColor("#C0273D");
    monitor.underrunIdle = QColor("#A96830");
    monitor.underrunNone = colors.textMuted;
    monitor.spatialGainHigh = colors.success;
    monitor.spatialGainLow = QColor("#A17A23");
    monitor.spatialLpf = QColor("#396F9D");
    monitor.spatialWidth = QColor("#A051A0");
    monitor.spatialDistance = QColor("#858426");
    monitor.spatialReverb = QColor("#25868A");
    monitor.spatialDopplerHi = monitor.pitchHigh;
    monitor.spatialDopplerLo = monitor.pitchLow;
    monitor.spatialDot = colors.accent;

    domains.ai.brandAccent = colors.accent;
    domains.ai.userAccent = QColor("#B4582E");

    auto& scene = domains.scene3d;
    scene.listener = QColor("#217BA7");
    scene.listenerCone = QColor("#54A0C8");
    scene.room = QColor("#7D81A9");
    scene.emitterMin = QColor("#27934C");
    scene.emitterMax = QColor("#C53C43");
    scene.hudBackground = QColor("#E8F3F6FA");
    scene.hudTitle = colors.textPrimary;
    scene.hudLabel = colors.textSecondary;
    scene.hudGain = colors.success;
    scene.hudGainLow = QColor("#A17A23");
    scene.hudLpf = QColor("#396F9D");
    scene.hudWidth = QColor("#A051A0");
    scene.dopplerIdle = colors.textMuted;

    auto& spatial = domains.spatial;
    spatial.background = QColor("#F6F8FB");
    spatial.grid = QColor("#DCE4EC");
    spatial.axis = QColor("#A3B2C2");
    spatial.axisLabel = colors.textSecondary;
    spatial.tangentLine = QColor("#8797A9");
    spatial.tangentHover = QColor("#5F758E");
    spatial.tangentActive = QColor("#B47915");
    spatial.pointSelected = QColor("#9F7500");
    spatial.previewMarker = QColor("#B034485C");
    spatial.infoText = colors.textPrimary;
    spatial.legendBg = QColor("#DEE6EF");
    spatial.layerVolume = QColor("#BE4E43");
    spatial.layerLpf = QColor("#327BAE");
    spatial.layerWidth = QColor("#288357");
    spatial.layerReverb = QColor("#9B7F10");

    ThemeColorOverrides roleColors;
    for (const auto& role : studioDefaultColorRoles())
        roleColors.insert(role.key, readThemeColor(theme, role.key));
    resolveThemeColors(theme, studioDefaultColorRoles(), roleColors);
    return theme;
}

}
