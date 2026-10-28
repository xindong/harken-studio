#include "ThemeColorResolver.h"

#include "ThemeSnapshot.h"

namespace VNX {

void resolveThemeColors(ThemeSnapshot& theme)
{
    const auto& colors = theme.ui.colors;
    auto& palette = theme.qtPalette;
    palette.windowText = colors.textPrimary;
    palette.toolTipText = colors.textPrimary;
    palette.text = colors.textPrimary;
    palette.buttonText = colors.textPrimary;
    palette.link = colors.accent;
    palette.highlight = colors.accent;
    palette.highlightedText = colors.accentText;
    palette.disabledText = colors.textDisabled;
    palette.disabledButtonText = colors.textDisabled;

    auto& domains = theme.domains;
    auto& curve = domains.curve.colors;
    curve.canvasBackground = colors.panelBackground;
    curve.canvasBorder = colors.viewBorder;
    curve.plotBackground = colors.bgPrimary;
    curve.grid = colors.viewBorder;
    curve.grid.setAlpha(100);
    curve.plotBorder = colors.borderPrimary;
    curve.axisText = colors.textSecondary;
    curve.handleLine = colors.textSecondary;
    curve.handleLine.setAlpha(110);
    curve.handleBorder = colors.textSecondary;
    curve.handleActiveBorder = colors.accent;
    curve.handleFill = colors.bgPrimary;
    curve.handleActiveFill = colors.accent;
    curve.pointBorder = colors.accent;
    curve.pointSelectedBorder = colors.textPrimary;
    curve.pointSelectedFill = colors.accent;
    curve.crosshairContrast = colors.bgPrimary;
    curve.crosshairContrast.setAlpha(153);
    curve.readoutBackground = colors.bgSecondary;
    curve.readoutText = colors.textPrimary;

    domains.waveform.dbScale.grid = colors.viewBorder;
    domains.waveform.dbScale.grid.setAlpha(100);
    domains.waveform.dbScale.label = colors.textSecondary;

    auto& meterInteraction = domains.meter.interaction.colors;
    meterInteraction.crosshair = colors.textPrimary;
    meterInteraction.crosshair.setAlpha(89);
    meterInteraction.readoutBackground = colors.bgPrimary;
    meterInteraction.readoutBackground.setAlpha(221);
    meterInteraction.readoutText = colors.textPrimary;
    meterInteraction.readoutText.setAlpha(229);

    auto& peak = domains.meter.peakMeter.colors;
    peak.canvasBackground = colors.bgPrimary;
    peak.trackBackground = colors.bgPrimary;
    peak.grid = colors.viewBorder;
    peak.tickText = colors.textSecondary;
    peak.label = colors.textPrimary;
    peak.label.setAlpha(204);

    auto& loudness = domains.meter.loudnessMeter.colors;
    loudness.canvasBackground = colors.panelBackground;
    loudness.trackBackground = colors.trackBackground;
    loudness.targetText = colors.textSecondary;
    loudness.labelText = colors.textPrimary;
    loudness.labelText.setAlpha(204);
    loudness.valueText = colors.textPrimary;
    loudness.valueText.setAlpha(204);

    auto& vu = domains.meter.vuMeter.colors;
    vu.canvasBackground = colors.panelBackground;
    vu.titleText = colors.textPrimary;
    vu.trackBackground = colors.trackBackground;
    vu.analogArc = colors.borderPrimary;
    vu.analogNegativeTick = colors.textSecondary;
    vu.analogValueText = colors.textMuted;
    vu.analogLabelText = colors.textPrimary;
    vu.barGrid = colors.borderPrimary;
    vu.barLabelText = colors.textPrimary;
    vu.barValueText = colors.textMuted;

    auto& spectrum = domains.meter.spectrumMeter.colors;
    spectrum.canvasBackground = colors.bgPrimary;
    spectrum.horizontalGrid = colors.viewBorder;
    spectrum.verticalGrid = colors.viewBorder;
    spectrum.axisText = colors.textSecondary;

    auto& spectrogram = domains.meter.spectrogramMeter.colors;
    spectrogram.canvasBackground = colors.bgPrimary;
    spectrogram.grid = colors.viewBorder;
    spectrogram.axisText = colors.textSecondary;
    spectrogram.timeText = colors.textSecondary;

    domains.nodeGraphNode.bodyNormal = colors.bgTertiary;
    domains.nodeGraphNode.bodySelected = colors.bgHover;
    domains.nodeGraphNode.titleText = colors.textPrimary;
    domains.nodeGraphNode.contentLabel = colors.textSecondary;
    domains.nodeGraphNode.contentValue = colors.textPrimary;
    domains.blueprint.edgeLabelText = colors.textSecondary;

    domains.feedback.success = colors.success;
    domains.feedback.warning = colors.warning;
    domains.feedback.error = colors.error;
    domains.feedback.info = colors.info;
    domains.feedback.neutral = colors.textMuted;
    domains.feedback.overlayHighlight = colors.accent;
}

}
