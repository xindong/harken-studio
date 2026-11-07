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
    colors.borderTick = QColor("#272E3C");

    colors.textPrimary = QColor("#E0E0E0");
    colors.textSecondary = QColor("#AAAAAA");
    colors.textTitle = colors.textPrimary;
    colors.textDisabled = QColor("#555555");

    colors.success = QColor("#4CAF50");
    colors.warning = QColor("#FF9800");
    colors.error = QColor("#F44336");
    colors.info = QColor("#2196F3");

    colors.trackBackground = QColor("#0F1318");
    colors.trackBackgroundAlt = QColor("#131820");

    colors.sliderTrack = QColor("#2A3140");

    colors.panelBackground = QColor("#10151F");
    colors.headerBackground = QColor("#1B2130");

    colors.shellCloseHover = QColor("#E81123");
    colors.shellClosePressed = QColor("#F1707A");

    auto& metrics = theme.ui.metrics;
    metrics.radiusSmall = 4;
    metrics.radiusMedium = 6;
    metrics.radiusLarge = 8;
    metrics.spacingXSmall = 4;
    metrics.spacingSmall = 8;
    metrics.spacingMedium = 12;
    metrics.spacingLarge = 16;
    metrics.spacingCompact = 2;
    metrics.spacingXLarge = 24;
    metrics.borderWidth = 1;
    metrics.accentBorderWidth = 2;
    metrics.radiusCompact = 2;
    metrics.controlHeight = 24;
    metrics.sectionHeaderHeight = 24;
    metrics.navigationChevronExtent = 20;
    metrics.objectIconExtent = 20;
    metrics.control.compactHeight = 20;
    metrics.control.largeHeight = 32;
    metrics.control.smallIconExtent = 16;
    metrics.control.largeIconExtent = 24;
    metrics.control.textPaddingX = 8;
    metrics.control.textPaddingY = 2;
    metrics.control.checkboxExtent = 16;
    metrics.control.checkboxMarkExtent = 12;
    metrics.control.scrollbarExtent = 8;
    metrics.control.canvasScrollbarExtent = 16;
    metrics.control.sliderHandleExtent = 12;
    metrics.control.sliderGrooveThickness = 4;
    metrics.control.scrollbarMinimumLength = 24;
    metrics.control.splitterExtent = 4;
    metrics.control.buttonMinimumWidth = 80;
    metrics.control.fieldMinimumWidth = 80;
    metrics.control.searchMinimumWidth = 160;
    metrics.popup.minimumWidth = 160;
    metrics.popup.maximumHeight = 240;
    metrics.popup.textPadding = 8;
    metrics.popup.rowPadding = 4;
    metrics.popup.iconColumnWidth = 24;
    metrics.popup.shortcutSpacing = 12;
    metrics.toast.minimumWidth = 200;
    metrics.toast.maximumWidth = 480;
    metrics.connection.lineWidth = 2;
    metrics.connection.hitWidth = 12;
    metrics.connection.maximumOverviewWidth = 12;
    metrics.colorPreview.swatchWidth = 48;
    metrics.colorPreview.swatchHeight = 16;
    metrics.colorPreview.alphaHeight = 4;
    metrics.colorPreview.paletteWidth = 160;
    metrics.colorPreview.paletteHeight = 8;
    metrics.colorPreview.hueWidth = 20;
    metrics.colorPreview.markerRadius = 4;
    metrics.form.rowSpacing = 12;
    metrics.form.columnSpacing = 16;
    metrics.form.labelColumnShare = 3;
    metrics.form.valueColumnShare = 7;
    metrics.chrome.titleBarHeight = 36;
    metrics.chrome.windowButtonWidth = 48;
    metrics.chrome.windowMinimumWidth = 800;
    metrics.chrome.windowMinimumHeight = 600;
    metrics.chrome.progressWidth = 160;
    metrics.chrome.progressHeight = 16;
    metrics.card.minimumWidth = 96;
    metrics.card.minimumHeight = 80;
    metrics.startup.width = 768;
    metrics.startup.artworkHeight = 416;
    metrics.startup.logoWidth = 272;
    metrics.startup.progressHeight = 4;
    metrics.toggle.width = 44;
    metrics.toggle.height = 24;
    metrics.toggle.thumbPadding = 4;
    metrics.toggle.compactWidth = 24;
    metrics.toggle.compactHeight = 16;
    metrics.toggle.compactThumbPadding = 4;
    metrics.panel.minimumWidth = 160;
    metrics.panel.minimumHeight = 120;
    metrics.panel.preferredWidth = 320;
    metrics.panel.preferredHeight = 480;
    metrics.panel.sidebarWidth = 160;
    metrics.panel.previewHeight = 256;
    metrics.dialog.minimumWidth = 416;
    metrics.dialog.minimumHeight = 280;
    metrics.dialog.compactWidth = 400;
    metrics.dialog.compactHeight = 280;
    metrics.dialog.preferredWidth = 640;
    metrics.dialog.preferredHeight = 480;
    metrics.dialog.wideWidth = 800;
    metrics.dialog.wideHeight = 560;
    metrics.node.minimumWidth = 160;
    metrics.node.headerHeight = 32;
    metrics.node.bodyPadding = 12;
    metrics.node.rowHeight = 24;
    metrics.node.waveformHeight = 24;
    metrics.node.expandedWaveformHeight = 28;
    metrics.node.waveformSpacing = 8;
    metrics.node.routePadding = 8;
    metrics.node.routeMinimumHeight = 32;
    metrics.node.pinColumnWidth = 28;
    metrics.node.pinSpacing = 32;
    metrics.node.pinRadius = 4;
    metrics.node.pinHitRadius = 8;
    metrics.node.badgeExtent = 24;
    metrics.node.columnSpacing = 56;
    metrics.node.rowSpacing = 16;
    metrics.node.pathColumnSpacing = 96;
    metrics.node.pathRowSpacing = 40;
    metrics.node.sliderWidth = 100;
    metrics.node.valueWidth = 48;
    metrics.timeAxis.handleBandHeight = 12;
    metrics.timeAxis.handleWidth = 12;
    metrics.timeAxis.handleHitRadius = 8;
    metrics.timeAxis.cursorWidth = 16;
    metrics.timeAxis.cursorHeight = 12;
    metrics.timeAxis.labelPadding = 4;
    metrics.timeAxis.minorTickLength = 4;
    metrics.timeAxis.majorTickLength = 8;
    metrics.timeAxis.waveformInset = 8;
    metrics.timeAxis.channelHeight = 88;
    metrics.table.headerHeight = 24;
    metrics.table.rowHeight = 32;
    metrics.table.footerHeight = 28;
    metrics.table.cellPaddingX = 4;
    metrics.table.cellPaddingY = 2;
    metrics.table.buttonWidth = 28;
    metrics.table.buttonHeight = 20;
    metrics.table.buttonSpacing = 2;
    metrics.table.compactColumnWidth = 24;
    metrics.table.minimumColumnWidth = 40;
    metrics.table.resizeHandleRadius = 4;
    metrics.inspector.pageMargin = 8;
    metrics.inspector.sectionSpacing = 2;
    metrics.inspector.rowSpacing = 8;
    metrics.inspector.columnPadding = 4;
    metrics.inspector.rowHeight = 24;
    metrics.inspector.labelColumnShare = 300;
    metrics.inspector.valueColumnShare = 700;
    metrics.inspector.formLabelWidth = 80;
    metrics.inspector.valueColumnWidth = 80;
    metrics.inspector.linkSlotWidth = 20;
    metrics.inspector.sourceSlotWidth = 16;
    metrics.inspector.platformSlotWidth = 16;
    metrics.inspector.bindingSlotWidth = 16;
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
    metrics.parameter.compactSliderHeight = 8;
    metrics.parameter.compactSliderHandleWidth = 4;
    metrics.timeline.laneHeaderWidth = 200;
    metrics.timeline.laneHeaderMinimumWidth = 140;
    metrics.timeline.laneHeight = 112;
    metrics.timeline.blankRowHeight = 42;
    metrics.timeline.laneSpacing = 1;
    metrics.timeline.rulerHeight = 28;
    metrics.timeline.markerLaneHeight = 12;
    metrics.timeline.tickLength = 5;
    metrics.timeline.majorTickLength = 10;
    metrics.timeline.minLabelSpacing = 48;
    metrics.timeline.minTickSpacing = 8;
    metrics.timeline.clipVerticalInset = 1;
    metrics.timeline.clipRadius = 2;
    metrics.timeline.clipMinimumWidth = 14;
    metrics.timeline.resizeHandleWidth = 7;
    metrics.timeline.markerHitRadius = 7;
    metrics.timeline.statusBarHeight = 28;
    metrics.timeline.compactHeaderWidth = 180;
    metrics.timeline.queueLaneLabelWidth = 88;
    metrics.timeline.headerGripWidth = 4;

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
    peakColors.gradientWarning = QColor(0xFF, 0xD6, 0x00);
    peakColors.gradientYellow = QColor(0xFF, 0xD6, 0x00);
    peakColors.gradientAccent = colors.accent;
    peakColors.rmsOverlay = QColor(0xFF, 0xFF, 0xFF, 0x3C);
    peakColors.holdHalo = QColor(0xE0, 0xE0, 0xE0, 0x19);
    peakColors.holdLine = QColor(0xE0, 0xE0, 0xE0, 0xEA);
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
    domains.waveform.splitStereo.labelRightInset = 18;
    domains.waveform.splitStereo.labelBaselineOffset = 12;
    domains.waveform.rawPeaks.background = QColor(11, 14, 20, 120);
    domains.waveform.rawPeaks.centerLine = QColor(60, 60, 60, 100);
    domains.waveform.rawPeaks.cornerRadius = 2.0;
    domains.waveform.rawPeaks.centerLineWidth = 0.5;
    domains.waveform.dbScale.gridWidth = 1.0;
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
    domains.nodeGraphNode.shadow = QColor(0x00, 0x00, 0x00, 0x3C);
    domains.nodeGraphNode.sandboxBadgeBg = QColor(0xDC, 0x3C, 0x3C, 0xC8);
    domains.nodeGraphNode.sandboxBadgeText = QColor(0xFF, 0xFF, 0xFF);
    domains.nodeGraphNode.badgeOverride = QColor(0xFF, 0x98, 0x00);
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
    domains.blueprint.send = QColor(0xFF, 0x4D, 0x8F);

    domains.timeline.entry = QColor(0xFF, 0xCA, 0x28);
    domains.timeline.exit = colors.error;
    domains.timeline.transition = QColor(232, 184, 74);
    domains.timeline.clip = QColor(54, 105, 168);
    domains.timeline.clipSelected = QColor(62, 142, 247);
    domains.timeline.clipText = QColor(230, 240, 255);
    domains.timeline.emergenceMarker = QColor(255, 190, 64);
    domains.timeline.unavailableOverlay = QColor(128, 128, 128, 28);
    domains.timeline.unavailableText = QColor(255, 255, 255, 70);
    domains.timeline.outOfSpanVeil = QColor(0x0B, 0x0E, 0x14, 0x66);

    domains.music.beatGridMajor = QColor(0xFF, 0xFF, 0xFF, 0x30);
    domains.music.beatGridMinor = QColor(0xFF, 0xFF, 0xFF, 0x12);
    domains.music.cursor = colors.success;

    domains.log.debug = QColor(0x80, 0x80, 0x80);
    domains.log.info = QColor(0xBB, 0xBB, 0xBB);
    domains.log.warning = QColor(0xFF, 0xB7, 0x4D);
    domains.log.error = QColor(0xFF, 0x44, 0x44);
    domains.log.critical = QColor(0xFF, 0x00, 0x00);

    domains.ai.brandAccent = QColor(0x00, 0xF0, 0xFF);

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
    colors.borderTick = QColor("#B9C5D1");
    colors.textPrimary = QColor("#202B38");
    colors.textSecondary = QColor("#506174");
    colors.textTitle = colors.textPrimary;
    colors.textDisabled = QColor("#98A4B1");
    colors.success = QColor("#217A45");
    colors.warning = QColor("#A76400");
    colors.error = QColor("#C53642");
    colors.info = QColor("#176DB0");
    colors.trackBackground = QColor("#F8FAFC");
    colors.trackBackgroundAlt = QColor("#EEF3F7");
    colors.sliderTrack = QColor("#B7C5D3");
    colors.panelBackground = QColor("#EEF2F6");
    colors.headerBackground = QColor("#E2E8EF");
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
    meter.interaction.colors.accentGuide = QColor("#A76400");

    auto& peak = meter.peakMeter.colors;
    peak.gradientAccent = QColor("#008E94");
    peak.gradientYellow = QColor("#A99216");
    peak.gradientWarning = QColor("#C17B13");
    peak.gradientError = colors.error;
    peak.rmsOverlay = QColor("#60203040");
    peak.holdHalo = QColor("#30334557");
    peak.holdLine = QColor("#E0334557");
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

    auto& waveform = domains.waveform;
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
    node.shadow = QColor("#22304458");
    node.sandboxBadgeBg = QColor("#E0C53642");
    node.sandboxBadgeText = QColor("#FFFFFF");
    node.badgeOverride = QColor("#B36C00");
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
    blueprint.send = QColor("#B54C80");

    auto& timeline = domains.timeline;
    timeline.entry = QColor("#A88214");
    timeline.exit = colors.error;
    timeline.transition = QColor("#A87312");
    timeline.clip = QColor("#C6DDF0");
    timeline.clipSelected = QColor("#77AED8");
    timeline.clipText = QColor("#24425F");
    timeline.emergenceMarker = QColor("#AF7915");
    timeline.unavailableOverlay = QColor("#30758595");
    timeline.unavailableText = QColor("#90607080");
    timeline.outOfSpanVeil = QColor("#80EDF2F7");

    domains.music.beatGridMajor = QColor("#40506174");
    domains.music.beatGridMinor = QColor("#20506174");
    domains.music.cursor = colors.success;

    domains.log.debug = colors.textSecondary;
    domains.log.info = colors.textPrimary;
    domains.log.warning = colors.warning;
    domains.log.error = colors.error;
    domains.log.critical = QColor("#A91930");

    domains.ai.brandAccent = colors.accent;

    ThemeColorOverrides roleColors;
    for (const auto& role : studioDefaultColorRoles())
        roleColors.insert(role.key, readThemeColor(theme, role.key));
    resolveThemeColors(theme, studioDefaultColorRoles(), roleColors);
    return theme;
}

}
