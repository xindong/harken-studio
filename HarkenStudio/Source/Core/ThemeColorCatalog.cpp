#include "ThemeColorCatalog.h"

#include "ThemePackageLoader.h"

namespace VNX {

const QVector<ThemeColorRole>& themeColorRoles()
{
#define COLOR_ROLE(key, group, label, description, field)                       \
    {key, group, label, description,                                            \
     [](const ThemeSnapshot& theme) { return theme.field; },                    \
     [](ThemeSnapshot& theme, const QColor& color) {                            \
         applyThemeColor(theme, QStringLiteral(key), color);                   \
     }}
    static const QVector<ThemeColorRole> roles{
        COLOR_ROLE("text.primary", "Text", "Primary text",
                   "Field values, valid read-only values, body text and node values.", ui.colors.textPrimary),
        COLOR_ROLE("text.secondary", "Text", "Secondary text",
                   "Field names, axis labels, inherited references and supporting labels.", ui.colors.textSecondary),
        COLOR_ROLE("text.muted", "Text", "Muted text",
                   "Hints, empty states and secondary metadata.", ui.colors.textMuted),
        COLOR_ROLE("text.disabled", "Text", "Disabled text",
                   "Text in unavailable controls, including disabled values and labels.", ui.colors.textDisabled),

        COLOR_ROLE("background.primary", "Surfaces", "Main background",
                   "Main work areas, curve plots and meter canvases.", ui.colors.bgPrimary),
        COLOR_ROLE("background.secondary", "Surfaces", "Control background",
                   "Input fields, secondary surfaces and readout backgrounds.", ui.colors.bgSecondary),
        COLOR_ROLE("background.tertiary", "Surfaces", "Raised background",
                   "Raised controls, node bodies and knob faces.", ui.colors.bgTertiary),
        COLOR_ROLE("panel.background", "Surfaces", "Panel background",
                   "Panel surfaces, the outer canvas of curve editors and the startup logo backdrop.", ui.colors.panelBackground),
        COLOR_ROLE("header.background", "Surfaces", "Header background",
                   "Table headers and Blueprint node headers.", ui.colors.headerBackground),
        COLOR_ROLE("track.background", "Surfaces", "Track background",
                   "Track surfaces and loudness meter tracks.", ui.colors.trackBackground),
        COLOR_ROLE("track.backgroundAlternate", "Surfaces", "Alternate track background",
                   "Alternating track surfaces.", ui.colors.trackBackgroundAlt),
        COLOR_ROLE("bus.background", "Surfaces", "Bus background",
                   "Bus surfaces.", ui.colors.busBackground),

        COLOR_ROLE("accent.primary", "Interaction", "Accent",
                   "Active controls, focus borders, slider fill and curve editing handles.", ui.colors.accent),
        COLOR_ROLE("accent.hover", "Interaction", "Accent hover",
                   "Hovered accent controls.", ui.colors.accentHover),
        COLOR_ROLE("accent.text", "Interaction", "Text on accent",
                   "Text on accent-filled selections and highlighted menus.", ui.colors.accentText),
        COLOR_ROLE("background.hover", "Interaction", "Hover background",
                   "Hovered controls and selected node bodies.", ui.colors.bgHover),
        COLOR_ROLE("background.selected", "Interaction", "Selected background",
                   "Selected rows, tabs and segmented options.", ui.colors.bgSelected),
        COLOR_ROLE("border.primary", "Interaction", "Standard border",
                   "Control boundaries, panel outlines and curve plot borders.", ui.colors.borderPrimary),
        COLOR_ROLE("border.secondary", "Interaction", "Strong border",
                   "Input field boundaries and stronger control outlines.", ui.colors.borderSecondary),
        COLOR_ROLE("border.view", "Interaction", "Canvas border and grid",
                   "Canvas outlines and shared curve, waveform and meter grids.", ui.colors.viewBorder),
        COLOR_ROLE("slider.track", "Interaction", "Slider track",
                   "The unfilled track of sliders and faders.", ui.colors.sliderTrack),
        COLOR_ROLE("knob.indicator", "Interaction", "Knob indicator",
                   "The position indicator on rotary knobs.", ui.colors.knobIndicator),

        COLOR_ROLE("status.success", "Feedback", "Success",
                   "Success feedback and shared playback position indicators.", ui.colors.success),
        COLOR_ROLE("status.warning", "Feedback", "Warning",
                   "Warnings and attention states.", ui.colors.warning),
        COLOR_ROLE("status.error", "Feedback", "Error",
                   "Errors, invalid references and validation feedback.", ui.colors.error),
        COLOR_ROLE("status.info", "Feedback", "Information",
                   "Informational feedback.", ui.colors.info),

        COLOR_ROLE("palette.categorical0", "Categorical palette", "Category 1",
                   "First color for distinguishing multiple items.", ui.colors.categoricalPalette[0]),
        COLOR_ROLE("palette.categorical1", "Categorical palette", "Category 2",
                   "Second color for distinguishing multiple items.", ui.colors.categoricalPalette[1]),
        COLOR_ROLE("palette.categorical2", "Categorical palette", "Category 3",
                   "Third color for distinguishing multiple items.", ui.colors.categoricalPalette[2]),
        COLOR_ROLE("palette.categorical3", "Categorical palette", "Category 4",
                   "Fourth color for distinguishing multiple items.", ui.colors.categoricalPalette[3]),
        COLOR_ROLE("palette.categorical4", "Categorical palette", "Category 5",
                   "Fifth color for distinguishing multiple items.", ui.colors.categoricalPalette[4]),
        COLOR_ROLE("palette.categorical5", "Categorical palette", "Category 6",
                   "Sixth color for distinguishing multiple items.", ui.colors.categoricalPalette[5]),
        COLOR_ROLE("palette.categorical6", "Categorical palette", "Category 7",
                   "Seventh color for distinguishing multiple items.", ui.colors.categoricalPalette[6]),

        COLOR_ROLE("waveform.static.quiet", "Waveform", "Quiet waveform",
                   "Low-amplitude portions of waveforms in static color mode.", domains.waveform.staticMode.quiet),
        COLOR_ROLE("waveform.static.peak", "Waveform", "Peak waveform",
                   "High-amplitude portions of waveforms in static color mode.", domains.waveform.staticMode.peak),
        COLOR_ROLE("waveform.multiBand.color0", "Waveform", "Low frequencies",
                   "Low-frequency contribution in multiband waveforms.", domains.waveform.multiBandMode.colors[0]),
        COLOR_ROLE("waveform.multiBand.color1", "Waveform", "Mid frequencies",
                   "Mid-frequency contribution in multiband waveforms.", domains.waveform.multiBandMode.colors[1]),
        COLOR_ROLE("waveform.multiBand.color2", "Waveform", "High frequencies",
                   "High-frequency contribution in multiband waveforms.", domains.waveform.multiBandMode.colors[2]),
        COLOR_ROLE("waveform.trimHandle", "Waveform", "Trim markers",
                   "Trim boundaries, handles and source range thumbnails.", domains.waveform.trimHandle),
        COLOR_ROLE("waveform.loopHandle", "Waveform", "Loop markers",
                   "Loop boundaries, handles and loop regions in waveform previews.", domains.waveform.loopHandle),

        COLOR_ROLE("curve.attenuationVolume", "Curves", "Volume attenuation",
                   "Volume attenuation curves.", domains.curve.colors.attenuationVolume),
        COLOR_ROLE("curve.attenuationPitch", "Curves", "Pitch attenuation",
                   "Pitch attenuation curves.", domains.curve.colors.attenuationPitch),
        COLOR_ROLE("curve.parameterBinding", "Curves", "Parameter binding",
                   "Parameter modulation and binding curves.", domains.curve.colors.parameterBinding),
        COLOR_ROLE("curve.gridFade", "Curves", "Grid fade",
                   "Fade curves in the grid editor.", domains.curve.colors.gridFade),

        COLOR_ROLE("meter.peakMeter.gradientAccent", "Meters", "Peak normal range",
                   "The lower range of the peak meter gradient.", domains.meter.peakMeter.colors.gradientAccent),
        COLOR_ROLE("meter.peakMeter.gradientYellow", "Meters", "Peak upper range",
                   "The upper range of the peak meter gradient.", domains.meter.peakMeter.colors.gradientYellow),
        COLOR_ROLE("meter.peakMeter.gradientWarning", "Meters", "Peak warning range",
                   "The warning range of the peak meter gradient.", domains.meter.peakMeter.colors.gradientWarning),
        COLOR_ROLE("meter.peakMeter.gradientError", "Meters", "Peak overload range",
                   "The highest range of the peak meter gradient.", domains.meter.peakMeter.colors.gradientError),
        COLOR_ROLE("meter.peakMeter.rmsOverlay", "Meters", "RMS overlay",
                   "RMS level displayed over peak meters.", domains.meter.peakMeter.colors.rmsOverlay),
        COLOR_ROLE("meter.peakMeter.holdLine", "Meters", "Peak hold line",
                   "The retained peak level indicator.", domains.meter.peakMeter.colors.holdLine),
        COLOR_ROLE("meter.spectrum.lineHighlight", "Meters", "Spectrum line",
                   "The foreground line of the frequency spectrum.", domains.meter.spectrumMeter.colors.lineHighlight),
        COLOR_ROLE("meter.spectrum.peakLine", "Meters", "Spectrum peak line",
                   "Retained peak levels in the frequency spectrum.", domains.meter.spectrumMeter.colors.peakLine),
        COLOR_ROLE("meter.lufsMomentary", "Meters", "Momentary loudness",
                   "Momentary loudness measurements.", domains.meter.lufsMomentary),
        COLOR_ROLE("meter.lufsShortTerm", "Meters", "Short-term loudness",
                   "Short-term loudness measurements.", domains.meter.lufsShortTerm),
        COLOR_ROLE("meter.lufsIntegrated", "Meters", "Integrated loudness",
                   "Integrated loudness measurements.", domains.meter.lufsIntegrated),

        COLOR_ROLE("timeline.clip", "Timeline", "Clip background",
                   "The default fill of timeline clips.", domains.timeline.clip),
        COLOR_ROLE("timeline.clipSelected", "Timeline", "Context band highlight",
                   "The nominal range tint of timeline context bands.", domains.timeline.clipSelected),
        COLOR_ROLE("timeline.clipText", "Timeline", "Clip text",
                   "Names and labels drawn on timeline clips.", domains.timeline.clipText),
        COLOR_ROLE("timeline.clipBorder", "Timeline", "Clip border",
                   "The outline of unselected timeline clips.", domains.timeline.clipBorder),
        COLOR_ROLE("timeline.clipSelectedBorder", "Timeline", "Selected clip border",
                   "The outline of selected timeline clips.", domains.timeline.clipSelectedBorder),
        COLOR_ROLE("timeline.entry", "Timeline", "Entry marker",
                   "Entry points and nominal span indicators.", domains.timeline.entry),
        COLOR_ROLE("timeline.exit", "Timeline", "Exit marker",
                   "Exit points in the timeline.", domains.timeline.exit),
        COLOR_ROLE("timeline.transition", "Timeline", "Transition",
                   "Transition markers and regions.", domains.timeline.transition),

        COLOR_ROLE("blueprint.canvasBackground", "Blueprint", "Canvas background",
                   "The Blueprint editing canvas.", domains.blueprint.canvasBackground),
        COLOR_ROLE("blueprint.gridDot", "Blueprint", "Grid dots",
                   "The Blueprint positioning grid.", domains.blueprint.gridDot),
        COLOR_ROLE("nodeGraph.pin.audio", "Blueprint", "Audio ports",
                   "Audio connection ports.", domains.pinDomain.audio),
        COLOR_ROLE("nodeGraph.pin.control", "Blueprint", "Control ports",
                   "Control connection ports.", domains.pinDomain.control),
        COLOR_ROLE("nodeGraph.pin.value", "Blueprint", "Value ports",
                   "Value connection ports.", domains.pinDomain.value),
        COLOR_ROLE("blueprint.placementChain", "Blueprint", "Placement connections",
                   "Connections representing placement relationships.", domains.blueprint.placementChain),
        COLOR_ROLE("blueprint.organizationChain", "Blueprint", "Organization connections",
                   "Connections representing organization relationships.", domains.blueprint.organizationChain),
        COLOR_ROLE("blueprint.containment", "Blueprint", "Containment connections",
                   "Connections representing containment relationships.", domains.blueprint.containment),
        COLOR_ROLE("nodeGraph.node.blueprintWaveform", "Blueprint", "Node waveform",
                   "Waveform previews inside Blueprint nodes.", domains.nodeGraphNode.blueprintWaveform),

        COLOR_ROLE("startup.wave.shadow", "Startup", "Wave shadows",
                   "The darkest shade of the startup wave animation. Applies on next startup.", domains.startup.wavePalette[0]),
        COLOR_ROLE("startup.wave.low", "Startup", "Wave low tones",
                   "The lower middle shade of the startup wave animation. Applies on next startup.", domains.startup.wavePalette[1]),
        COLOR_ROLE("startup.wave.high", "Startup", "Wave high tones",
                   "The upper middle shade of the startup wave animation. Applies on next startup.", domains.startup.wavePalette[2]),
        COLOR_ROLE("startup.wave.highlight", "Startup", "Wave highlights",
                   "The brightest shade of the startup wave animation. Applies on next startup.", domains.startup.wavePalette[3]),
    };
#undef COLOR_ROLE
    return roles;
}

const ThemeColorRole* findThemeColorRole(const QString& key)
{
    for (const ThemeColorRole& role : themeColorRoles())
        if (key == QLatin1String(role.key))
            return &role;
    return nullptr;
}

}
