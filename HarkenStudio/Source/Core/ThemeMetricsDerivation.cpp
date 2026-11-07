#include "ThemeMetricsDerivation.h"

#include <QtGlobal>

namespace VNX {
namespace {

struct Scale
{
    qreal factor = 1.0;

    int px(int value) const
    {
        return value > 0 ? qMax(1, qRound(value * factor)) : qRound(value * factor);
    }
    qreal len(qreal value) const { return value * factor; }
};

void deriveUiMetrics(ThemeMetrics& m, const Scale& s)
{
    const int compact = m.spacingCompact;
    const int xSmall = m.spacingXSmall;
    const int small = m.spacingSmall;
    const int medium = m.spacingMedium;
    const int large = m.spacingLarge;
    const int smallIcon = m.control.smallIconExtent;
    const int largeIcon = m.control.largeIconExtent;
    const int rowHeight = m.controlHeight;
    const int largeHeight = m.control.largeHeight;

    auto& control = m.control;
    control.textPaddingX = small;
    control.textPaddingY = compact;
    control.checkboxMarkExtent = smallIcon - xSmall;
    control.scrollbarExtent = small;
    control.canvasScrollbarExtent = smallIcon;
    control.scrollbarMinimumLength = rowHeight;
    control.sliderHandleExtent = medium;
    control.sliderGrooveThickness = xSmall;
    control.splitterExtent = xSmall;
    control.buttonMinimumWidth = s.px(80);
    control.fieldMinimumWidth = s.px(80);
    control.searchMinimumWidth = s.px(160);

    auto& popup = m.popup;
    popup.minimumWidth = s.px(160);
    popup.maximumHeight = s.px(240);
    popup.rowPadding = xSmall;
    popup.iconColumnWidth = largeIcon;
    popup.shortcutSpacing = medium;

    m.toast.minimumWidth = s.px(200);
    m.toast.maximumWidth = s.px(480);

    auto& connection = m.connection;
    connection.lineWidth = m.accentBorderWidth;
    connection.hitWidth = medium;
    connection.maximumOverviewWidth = medium;

    auto& preview = m.colorPreview;
    preview.swatchWidth = largeIcon * 2;
    preview.swatchHeight = smallIcon;
    preview.alphaHeight = xSmall;
    preview.paletteWidth = s.px(160);
    preview.paletteHeight = small;
    preview.hueWidth = m.iconExtent;
    preview.markerRadius = xSmall;

    auto& form = m.form;
    form.rowSpacing = medium;
    form.columnSpacing = large;
    form.labelColumnShare = 3;
    form.valueColumnShare = 7;

    auto& chrome = m.chrome;
    chrome.titleBarHeight = largeHeight + xSmall;
    chrome.windowButtonWidth = rowHeight * 2;
    chrome.windowMinimumWidth = 800;
    chrome.windowMinimumHeight = 600;
    chrome.progressWidth = s.px(160);
    chrome.progressHeight = smallIcon;

    m.card.minimumWidth = s.px(96);
    m.card.minimumHeight = s.px(80);

    auto& startup = m.startup;
    startup.width = s.px(768);
    startup.artworkHeight = s.px(416);
    startup.logoWidth = s.px(272);
    startup.progressHeight = xSmall;

    auto& toggle = m.toggle;
    toggle.height = rowHeight;
    toggle.width = rowHeight * 2 - xSmall;
    toggle.thumbPadding = qMin(xSmall, (toggle.height - 1) / 2);
    toggle.compactHeight = smallIcon;
    toggle.compactWidth = smallIcon + small;
    toggle.compactThumbPadding = qMin(xSmall, (toggle.compactHeight - 1) / 2);

    auto& panel = m.panel;
    panel.minimumWidth = s.px(160);
    panel.minimumHeight = s.px(120);
    panel.preferredWidth = s.px(320);
    panel.preferredHeight = s.px(480);
    panel.sidebarWidth = s.px(160);
    panel.previewHeight = s.px(256);

    auto& dialog = m.dialog;
    dialog.minimumWidth = s.px(416);
    dialog.minimumHeight = s.px(280);
    dialog.compactWidth = s.px(400);
    dialog.compactHeight = s.px(280);
    dialog.preferredWidth = s.px(640);
    dialog.preferredHeight = s.px(480);
    dialog.wideWidth = s.px(800);
    dialog.wideHeight = s.px(560);

    auto& axis = m.timeAxis;
    axis.handleBandHeight = medium;
    axis.handleWidth = medium;
    axis.handleHitRadius = small;
    axis.cursorWidth = smallIcon;
    axis.cursorHeight = medium;
    axis.labelPadding = xSmall;
    axis.minorTickLength = xSmall;
    axis.majorTickLength = small;
    axis.waveformInset = small;
    axis.channelHeight = s.px(88);

    auto& table = m.table;
    table.rowHeight = largeHeight;
    table.footerHeight = rowHeight + xSmall;
    table.cellPaddingX = xSmall;
    table.cellPaddingY = compact;
    table.buttonWidth = rowHeight + xSmall;
    table.buttonSpacing = compact;
    table.compactColumnWidth = largeIcon;
    table.minimumColumnWidth = largeIcon + smallIcon;
    table.resizeHandleRadius = xSmall;

    auto& inspector = m.inspector;
    inspector.pageMargin = small;
    inspector.sectionSpacing = compact;
    inspector.rowSpacing = small;
    inspector.columnPadding = xSmall;
    inspector.valueColumnWidth = s.px(80);
    inspector.linkSlotWidth = m.iconExtent;
    inspector.bindingSlotWidth = smallIcon;
    inspector.slotSpacing = compact;
    inspector.separatorHeight = m.borderWidth;
    inspector.rangeSeparatorWidth = medium;

    auto& parameter = m.parameter;
    parameter.faderMinimumHeight = rowHeight * 4;
    parameter.labelMinimumWidth = s.px(100);
    parameter.valueMinimumWidth = s.px(72);
    parameter.compactValueMinimumWidth = s.px(80);
    parameter.sliderHandleThickness = xSmall + compact;
    parameter.sliderHandleLength = medium + compact;
    parameter.compactSliderHeight = small;
    parameter.compactSliderHandleWidth = xSmall;

    auto& timeline = m.timeline;
    timeline.laneHeaderWidth = s.px(200);
    timeline.laneHeaderMinimumWidth = s.px(140);
    timeline.laneHeight = s.px(112);
    timeline.rulerHeight = rowHeight + xSmall;
    timeline.minLabelSpacing = rowHeight * 2;
    timeline.minTickSpacing = small;
    timeline.clipVerticalInset = m.borderWidth;
    timeline.clipMinimumWidth = medium + compact;
    timeline.resizeHandleWidth = small;
}

void deriveNodeMetrics(ThemeMetrics::Node& node, const ThemeMetrics& w, const Scale& s)
{
    const auto len = [&s](int value) { return static_cast<float>(s.len(value)); };
    const int rowHeight = w.controlHeight;
    const int largeHeight = w.control.largeHeight;
    node.minimumWidth = len(160);
    node.headerHeight = len(largeHeight);
    node.bodyPadding = len(w.spacingMedium);
    node.rowHeight = len(rowHeight);
    node.waveformHeight = len(rowHeight);
    node.expandedWaveformHeight = len(rowHeight + w.spacingXSmall);
    node.waveformSpacing = len(w.spacingSmall);
    node.routePadding = len(w.spacingSmall);
    node.routeMinimumHeight = len(largeHeight);
    node.pinColumnWidth = len(rowHeight + w.spacingXSmall);
    node.pinSpacing = len(largeHeight);
    node.pinRadius = len(w.spacingXSmall);
    node.pinHitRadius = len(w.spacingSmall);
    node.badgeExtent = len(w.control.largeIconExtent);
    node.columnSpacing = len(56);
    node.rowSpacing = len(w.spacingLarge);
    node.pathColumnSpacing = len(96);
    node.pathRowSpacing = len(40);
    node.sliderWidth = s.px(100);
    node.valueWidth = qRound(s.len(rowHeight * 2));
}

void deriveCurveMetrics(CurveStyle::Metrics& c, const ThemeMetrics& m, const Scale& s)
{
    const qreal line = m.borderWidth;
    c.linearGraphLeftInset = s.px(40);
    c.amplitudeGraphLeftInset = s.px(40);
    c.graphTopInset = s.px(10);
    c.graphRightInset = s.px(10);
    c.graphBottomInset = s.px(26);
    c.gridColumnCount = 10;
    c.linearGridRowCount = 10;
    c.amplitudeGridTargetRowSpacing = s.len(32.0);
    c.amplitudeGridMinimumRows = 8;
    c.amplitudeGridMaximumRows = 32;
    c.gridLineWidth = line;
    c.plotBorderWidth = line;
    c.curveLineWidth = s.len(1.5);
    c.discreteGuideLineWidth = line;
    c.ghostLineWidth = line;
    c.lassoBorderWidth = line;
    c.handleLineWidth = line;
    c.handleBorderWidth = s.len(1.5);
    c.pointBorderWidth = s.len(1.5);
    c.crosshairLineWidth = line;
    c.ghostRadius = s.len(2.5);
    c.handleRadius = s.len(3.0);
    c.pointRadius = m.spacingXSmall;
    c.pointSelectedRadius = s.len(5.0);
    c.crosshairDashLength = 2.0;
    c.crosshairDashGap = 3.0;
    c.xAxisLabelTopOffset = m.spacingSmall;
    c.xAxisLabelHeight = s.len(18.0);
    c.yAxisLabelCenterX = m.spacingMedium;
    c.yAxisLabelWidth = s.len(120.0);
    c.yAxisLabelHeight = s.len(18.0);
    c.amplitudeTickLabelLeft = s.len(30.0);
    c.amplitudeTickLabelRightGap = s.len(6.0);
    c.amplitudeTickLabelHalfHeight = m.spacingSmall;
    c.amplitudeTickLabelHeight = m.control.smallIconExtent;
    c.linearYTickLeftOffset = s.len(40.0);
    c.linearYTickBaselineOffset = m.spacingXSmall;
    c.xMinTickLeftOffset = m.spacingXSmall;
    c.xTickBaselineOffset = s.len(18.0);
    c.xMaxLabelWidth = s.len(96.0);
    c.xMaxLabelTopOffset = s.len(6.0);
    c.xMaxLabelHeight = s.len(18.0);
    c.xMaxEditorVerticalExpansion = s.len(3.0);
    c.readoutHorizontalPadding = m.spacingSmall;
    c.readoutHeight = m.control.compactHeight;
    c.readoutHorizontalOffset = s.len(6.0);
    c.readoutAboveOffset = s.len(21.0);
    c.readoutBelowOffset = s.len(9.0);
    c.readoutRadius = m.radiusCompact;
}

void deriveMeterMetrics(MeterStyle& meter, const ThemeMetrics& m, const Scale& s)
{
    const qreal line = m.borderWidth;

    auto& interaction = meter.interaction.metrics;
    interaction.crosshairLineWidth = line;
    interaction.accentGuideLineWidth = line;
    interaction.horizontalPadding = m.spacingLarge;
    interaction.lineHeight = s.len(22.0);
    interaction.extraHeight = s.len(6.0);
    interaction.horizontalOffset = s.len(10.0);
    interaction.aboveGap = s.len(6.0);
    interaction.belowOffset = s.len(10.0);
    interaction.radius = m.radiusSmall;

    auto& peak = meter.peakMeter.metrics;
    peak.outerInsetLeft = m.spacingXSmall;
    peak.outerInsetTop = s.px(3);
    peak.outerInsetRight = m.spacingXSmall;
    peak.outerInsetBottom = s.px(3);
    peak.channelGap = m.borderWidth;
    peak.reservedWidth = m.borderWidth;
    peak.barHorizontalInset = 0.0;
    peak.rmsLeftInsetRatio = 0.2;
    peak.rmsWidthRatio = 0.6;
    peak.holdHaloTopOffset = s.len(-2.5);
    peak.holdHaloHeight = s.len(6.0);
    peak.holdLineTopOffset = s.len(-0.5);
    peak.holdLineHeight = m.accentBorderWidth;
    peak.gridLineWidth = line;
    peak.tickLeftOffset = s.px(28);
    peak.tickWidth = s.px(26);
    peak.tickHalfHeight = s.len(6.0);
    peak.tickHeight = m.spacingMedium;
    peak.clipHeight = s.px(18);
    peak.channelLabelBottomOffset = s.px(30);
    peak.valueLabelBottomOffset = m.control.smallIconExtent;
    peak.labelHeight = s.px(14);

    auto& spectrum = meter.spectrumMeter.metrics;
    spectrum.plotInsetLeft = s.px(30);
    spectrum.plotInsetTop = s.px(10);
    spectrum.plotInsetRight = s.px(10);
    spectrum.plotInsetBottom = s.px(20);
    spectrum.curveControlRatio = 0.4f;
    spectrum.lineGlowWideWidth = s.len(6.0);
    spectrum.lineGlowMediumWidth = s.len(3.5);
    spectrum.lineGlowNarrowWidth = s.len(1.8);
    spectrum.lineHighlightWidth = s.len(0.8);
    spectrum.peakLineWidth = line;
    spectrum.gridLineWidth = line;
    spectrum.dbLabelLeft = 0.0;
    spectrum.dbLabelHalfHeight = s.len(6.0);
    spectrum.dbLabelWidth = s.len(28.0);
    spectrum.dbLabelHeight = m.spacingMedium;
    spectrum.frequencyLabelHalfWidth = s.len(20.0);
    spectrum.frequencyLabelTopOffset = m.spacingCompact;
    spectrum.frequencyLabelWidth = s.len(40.0);
    spectrum.frequencyLabelHeight = s.len(14.0);

    auto& spectrogram = meter.spectrogramMeter.metrics;
    spectrogram.plotInsetLeft = s.px(30);
    spectrogram.plotInsetTop = s.px(10);
    spectrogram.plotInsetRight = s.px(10);
    spectrogram.plotInsetBottom = s.px(10);
    spectrogram.gridLineWidth = line;
    spectrogram.frequencyLabelLeft = 0.0;
    spectrogram.frequencyLabelHalfHeight = s.len(6.0);
    spectrogram.frequencyLabelWidth = s.len(28.0);
    spectrogram.frequencyLabelHeight = m.spacingMedium;
    spectrogram.timeLabelTopOffset = m.spacingCompact;
    spectrogram.timeLabelHeight = m.control.smallIconExtent;
}

void deriveWaveformMetrics(WaveformStyle& waveform, const ThemeMetrics& m, const Scale& s)
{
    waveform.amplitudeScale = 0.9;
    waveform.splitStereo.dividerWidth = m.borderWidth;
    waveform.splitStereo.labelRightInset = s.px(18);
    waveform.splitStereo.labelBaselineOffset = m.spacingMedium;
    waveform.rawPeaks.cornerRadius = m.radiusCompact;
    waveform.rawPeaks.centerLineWidth = s.len(0.5);
    waveform.dbScale.gridWidth = m.borderWidth;
    waveform.dbScale.amplitudeScale = 0.9;
    waveform.dbScale.labelLeftInset = m.spacingCompact;
    waveform.dbScale.labelBaselineOffset = s.px(-1);
}

}

void deriveThemeMetrics(ThemeSnapshot& theme)
{
    deriveThemeMetrics(theme, theme.ui.metrics);
}

void deriveThemeMetrics(ThemeSnapshot& theme, const ThemeMetrics& worldTokens)
{
    const Scale scale{theme.uiScale};
    auto& metrics = theme.ui.metrics;
    deriveUiMetrics(metrics, scale);
    deriveNodeMetrics(metrics.node, worldTokens, scale);
    deriveCurveMetrics(theme.domains.curve.metrics, metrics, scale);
    deriveMeterMetrics(theme.domains.meter, metrics, scale);
    deriveWaveformMetrics(theme.domains.waveform, metrics, scale);
}

}
