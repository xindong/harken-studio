#include "ThemeScale.h"

namespace VNX {
namespace {

template<typename... Values>
void scalePixels(qreal factor, Values&... values)
{
    ((values = values > 0 ? qMax(1, qRound(values * factor)) : qRound(values * factor)), ...);
}

template<typename... Values>
void scaleLengths(qreal factor, Values&... values)
{
    ((values = static_cast<Values>(values * factor)), ...);
}

void scaleMetrics(ThemeMetrics& metrics, qreal factor)
{
    scalePixels(factor,
        metrics.radiusSmall, metrics.radiusMedium, metrics.radiusLarge,
        metrics.spacingXSmall, metrics.spacingSmall, metrics.spacingMedium,
        metrics.spacingLarge, metrics.spacingCompact, metrics.spacingXLarge,
        metrics.borderWidth, metrics.accentBorderWidth, metrics.radiusCompact,
        metrics.controlHeight, metrics.sectionHeaderHeight,
        metrics.navigationChevronExtent, metrics.objectIconExtent);

    auto& control = metrics.control;
    scalePixels(factor,
        control.compactHeight, control.largeHeight, control.smallIconExtent,
        control.largeIconExtent, control.textPaddingX, control.textPaddingY,
        control.checkboxExtent, control.checkboxMarkExtent,
        control.scrollbarExtent, control.canvasScrollbarExtent,
        control.scrollbarMinimumLength, control.sliderHandleExtent,
        control.sliderGrooveThickness, control.splitterExtent,
        control.buttonMinimumWidth, control.fieldMinimumWidth,
        control.searchMinimumWidth);

    auto& popup = metrics.popup;
    scalePixels(factor,
        popup.minimumWidth, popup.maximumHeight, popup.textPadding,
        popup.rowPadding, popup.iconColumnWidth, popup.shortcutSpacing);

    auto& toast = metrics.toast;
    scalePixels(factor, toast.minimumWidth, toast.maximumWidth);

    auto& connection = metrics.connection;
    scaleLengths(factor,
        connection.lineWidth, connection.hitWidth,
        connection.maximumOverviewWidth);

    auto& preview = metrics.colorPreview;
    scalePixels(factor,
        preview.swatchWidth, preview.swatchHeight, preview.alphaHeight,
        preview.paletteWidth, preview.paletteHeight, preview.hueWidth,
        preview.markerRadius);

    scalePixels(factor, metrics.form.rowSpacing, metrics.form.columnSpacing);

    auto& chrome = metrics.chrome;
    scalePixels(factor,
        chrome.titleBarHeight, chrome.windowButtonWidth,
        chrome.windowMinimumWidth, chrome.windowMinimumHeight,
        chrome.progressWidth, chrome.progressHeight);

    auto& toggle = metrics.toggle;
    scalePixels(factor,
        toggle.width, toggle.height, toggle.thumbPadding,
        toggle.compactWidth, toggle.compactHeight, toggle.compactThumbPadding);
    toggle.thumbPadding = qMin(toggle.thumbPadding, (toggle.height - 1) / 2);
    toggle.compactThumbPadding = qMin(toggle.compactThumbPadding, (toggle.compactHeight - 1) / 2);

    auto& panel = metrics.panel;
    scalePixels(factor,
        panel.minimumWidth, panel.minimumHeight, panel.preferredWidth,
        panel.preferredHeight, panel.sidebarWidth, panel.previewHeight);

    auto& dialog = metrics.dialog;
    scalePixels(factor,
        dialog.minimumWidth, dialog.minimumHeight,
        dialog.compactWidth, dialog.compactHeight,
        dialog.preferredWidth, dialog.preferredHeight,
        dialog.wideWidth, dialog.wideHeight);

    scalePixels(factor, metrics.card.minimumWidth, metrics.card.minimumHeight);

    auto& startup = metrics.startup;
    scalePixels(factor,
        startup.width, startup.artworkHeight, startup.logoWidth,
        startup.progressHeight);

    auto& node = metrics.node;
    scaleLengths(factor,
        node.minimumWidth, node.headerHeight, node.bodyPadding, node.rowHeight,
        node.waveformHeight, node.expandedWaveformHeight, node.waveformSpacing,
        node.routePadding, node.routeMinimumHeight, node.pinColumnWidth,
        node.pinSpacing, node.pinRadius, node.pinHitRadius, node.badgeExtent,
        node.columnSpacing, node.rowSpacing,
        node.pathColumnSpacing, node.pathRowSpacing);
    scalePixels(factor, node.sliderWidth, node.valueWidth);

    auto& axis = metrics.timeAxis;
    scalePixels(factor,
        axis.handleBandHeight, axis.handleWidth, axis.handleHitRadius,
        axis.cursorWidth, axis.cursorHeight, axis.labelPadding,
        axis.minorTickLength, axis.majorTickLength, axis.waveformInset,
        axis.channelHeight);

    auto& table = metrics.table;
    scalePixels(factor,
        table.headerHeight, table.rowHeight, table.footerHeight,
        table.cellPaddingX, table.cellPaddingY, table.buttonWidth,
        table.buttonHeight, table.buttonSpacing, table.compactColumnWidth,
        table.minimumColumnWidth, table.resizeHandleRadius);

    auto& inspector = metrics.inspector;
    scalePixels(factor,
        inspector.pageMargin, inspector.sectionSpacing, inspector.rowSpacing,
        inspector.columnPadding, inspector.rowHeight, inspector.formLabelWidth,
        inspector.valueColumnWidth, inspector.linkSlotWidth,
        inspector.sourceSlotWidth, inspector.platformSlotWidth,
        inspector.bindingSlotWidth, inspector.slotHeight, inspector.slotSpacing,
        inspector.separatorHeight, inspector.rangeSeparatorWidth);

    auto& parameter = metrics.parameter;
    scalePixels(factor,
        parameter.faderMinimumHeight, parameter.labelMinimumWidth,
        parameter.valueMinimumWidth, parameter.compactValueMinimumWidth,
        parameter.sliderGrooveThickness, parameter.sliderHandleThickness,
        parameter.sliderHandleLength, parameter.compactSliderHeight,
        parameter.compactSliderHandleWidth);

    auto& timeline = metrics.timeline;
    scalePixels(factor,
        timeline.laneHeaderWidth, timeline.laneHeaderMinimumWidth,
        timeline.laneHeight, timeline.blankRowHeight, timeline.laneSpacing,
        timeline.rulerHeight, timeline.markerLaneHeight, timeline.tickLength,
        timeline.majorTickLength, timeline.minLabelSpacing,
        timeline.minTickSpacing, timeline.clipVerticalInset, timeline.clipRadius,
        timeline.clipMinimumWidth, timeline.resizeHandleWidth,
        timeline.markerHitRadius, timeline.statusBarHeight,
        timeline.compactHeaderWidth, timeline.queueLaneLabelWidth,
        timeline.headerGripWidth);
}

void scaleDockMetrics(QtAdsStyle::Metrics& metrics, qreal factor)
{
    scalePixels(factor,
        metrics.borderWidth, metrics.titleBarMinimumHeight,
        metrics.titleBarButtonPadding, metrics.titleBarButtonRadius,
        metrics.titleBarIconSize, metrics.closeButtonPadding,
        metrics.closeButtonExtent, metrics.closeButtonRadius,
        metrics.tabBarMinimumHeight, metrics.tabAccentBorderWidth,
        metrics.tabGapWidth, metrics.tabPaddingTop, metrics.tabPaddingHorizontal,
        metrics.tabPaddingBottom, metrics.tabMinimumHeight, metrics.tabIconSize,
        metrics.tabCloseMarginTop, metrics.tabClosePaddingRight,
        metrics.overlayBorderWidth, metrics.splitterHandleExtent,
        metrics.autoHideTabIconSize, metrics.autoHideTabPaddingLeft,
        metrics.autoHideTabPaddingRight, metrics.autoHideTabMinimumHeight,
        metrics.autoHideSideBarSpacing, metrics.autoHideTitleBorderWidth,
        metrics.autoHideTitlePaddingLeft);
}

void scaleCurveMetrics(CurveStyle::Metrics& metrics, qreal factor)
{
    scalePixels(factor,
        metrics.linearGraphLeftInset, metrics.amplitudeGraphLeftInset,
        metrics.graphTopInset, metrics.graphRightInset, metrics.graphBottomInset);
    scaleLengths(factor,
        metrics.amplitudeGridTargetRowSpacing, metrics.gridLineWidth,
        metrics.plotBorderWidth, metrics.curveLineWidth,
        metrics.discreteGuideLineWidth, metrics.ghostLineWidth,
        metrics.lassoBorderWidth, metrics.handleLineWidth,
        metrics.handleBorderWidth, metrics.pointBorderWidth,
        metrics.crosshairLineWidth, metrics.ghostRadius, metrics.handleRadius,
        metrics.pointRadius, metrics.pointSelectedRadius,
        metrics.xAxisLabelTopOffset, metrics.xAxisLabelHeight,
        metrics.yAxisLabelCenterX, metrics.yAxisLabelWidth,
        metrics.yAxisLabelHeight, metrics.amplitudeTickLabelLeft,
        metrics.amplitudeTickLabelRightGap, metrics.amplitudeTickLabelHalfHeight,
        metrics.amplitudeTickLabelHeight, metrics.linearYTickLeftOffset,
        metrics.linearYTickBaselineOffset, metrics.xMinTickLeftOffset,
        metrics.xTickBaselineOffset, metrics.xMaxLabelWidth,
        metrics.xMaxLabelTopOffset, metrics.xMaxLabelHeight,
        metrics.xMaxEditorVerticalExpansion, metrics.readoutHorizontalPadding,
        metrics.readoutHeight, metrics.readoutHorizontalOffset,
        metrics.readoutAboveOffset, metrics.readoutBelowOffset,
        metrics.readoutRadius);
}

void scaleMeterMetrics(MeterStyle& meter, qreal factor)
{
    auto& interaction = meter.interaction.metrics;
    scaleLengths(factor,
        interaction.crosshairLineWidth, interaction.accentGuideLineWidth,
        interaction.horizontalPadding, interaction.lineHeight,
        interaction.extraHeight, interaction.horizontalOffset,
        interaction.aboveGap, interaction.belowOffset, interaction.radius);

    auto& peak = meter.peakMeter.metrics;
    scalePixels(factor,
        peak.outerInsetLeft, peak.outerInsetTop,
        peak.outerInsetRight, peak.outerInsetBottom,
        peak.channelGap, peak.reservedWidth, peak.tickLeftOffset, peak.tickWidth,
        peak.clipHeight, peak.channelLabelBottomOffset,
        peak.valueLabelBottomOffset, peak.labelHeight);
    scaleLengths(factor,
        peak.barHorizontalInset, peak.holdHaloTopOffset, peak.holdHaloHeight,
        peak.holdLineTopOffset, peak.holdLineHeight, peak.gridLineWidth,
        peak.tickHalfHeight, peak.tickHeight);

    auto& spectrum = meter.spectrumMeter.metrics;
    scalePixels(factor,
        spectrum.plotInsetLeft, spectrum.plotInsetTop,
        spectrum.plotInsetRight, spectrum.plotInsetBottom);
    scaleLengths(factor,
        spectrum.lineGlowWideWidth, spectrum.lineGlowMediumWidth,
        spectrum.lineGlowNarrowWidth, spectrum.lineHighlightWidth,
        spectrum.peakLineWidth, spectrum.gridLineWidth,
        spectrum.dbLabelLeft, spectrum.dbLabelHalfHeight,
        spectrum.dbLabelWidth, spectrum.dbLabelHeight,
        spectrum.frequencyLabelHalfWidth, spectrum.frequencyLabelTopOffset,
        spectrum.frequencyLabelWidth, spectrum.frequencyLabelHeight);

    auto& spectrogram = meter.spectrogramMeter.metrics;
    scalePixels(factor,
        spectrogram.plotInsetLeft, spectrogram.plotInsetTop,
        spectrogram.plotInsetRight, spectrogram.plotInsetBottom);
    scaleLengths(factor,
        spectrogram.gridLineWidth, spectrogram.frequencyLabelLeft,
        spectrogram.frequencyLabelHalfHeight, spectrogram.frequencyLabelWidth,
        spectrogram.frequencyLabelHeight, spectrogram.timeLabelTopOffset,
        spectrogram.timeLabelHeight);
}

void scaleWaveformMetrics(WaveformStyle& waveform, qreal factor)
{
    scalePixels(factor,
        waveform.splitStereo.labelRightInset,
        waveform.splitStereo.labelBaselineOffset,
        waveform.dbScale.labelLeftInset,
        waveform.dbScale.labelBaselineOffset);
    scaleLengths(factor,
        waveform.splitStereo.dividerWidth,
        waveform.rawPeaks.cornerRadius, waveform.rawPeaks.centerLineWidth,
        waveform.dbScale.gridWidth);
}

}

ThemeSnapshot scaledThemeSnapshot(const ThemeSnapshot& base, qreal factor)
{
    Q_ASSERT(qIsFinite(factor) && factor > 0.0);
    ThemeSnapshot scaled = base;
    scaled.uiScale = factor;
    scaleMetrics(scaled.ui.metrics, factor);
    scaleDockMetrics(scaled.thirdParty.qtAds.metrics, factor);
    scaleCurveMetrics(scaled.domains.curve.metrics, factor);
    scaleMeterMetrics(scaled.domains.meter, factor);
    scaleWaveformMetrics(scaled.domains.waveform, factor);
    for (auto& recipe : scaled.ui.recipes.entries)
    {
        for (auto& slot : recipe.items)
        {
            if (slot.minimum >= 0) scalePixels(factor, slot.minimum);
            if (slot.preferred >= 0) scalePixels(factor, slot.preferred);
            if (slot.maximum >= 0) scalePixels(factor, slot.maximum);
        }
    }
    return scaled;
}

}
