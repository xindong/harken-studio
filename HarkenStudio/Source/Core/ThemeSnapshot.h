#pragma once

#include "ComponentRecipeRegistry.h"
#include "ParameterPresenterRegistry.h"

#include <QColor>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <array>
#include <memory>

namespace VNX {

enum class ThemeColorScheme
{
    Dark,
    Light
};

enum class ThemeStateLayer
{
    Hover,
    Pressed,
    Emphasis,
    Highlight
};

inline constexpr int ThemeSoftTintAlpha = 36;

inline QColor themeStateColor(const QColor& base, const QColor& overlay, ThemeStateLayer layer)
{
    if (layer == ThemeStateLayer::Highlight)
        return overlay.lightnessF() > base.lightnessF() ? base.lighter(135) : base.darker(135);
    const qreal opacity = layer == ThemeStateLayer::Hover ? 0.08
        : layer == ThemeStateLayer::Pressed ? 0.12 : 0.16;
    const auto mix = [opacity](qreal from, qreal to) { return from + (to - from) * opacity; };
    return QColor::fromRgbF(mix(base.redF(), overlay.redF()), mix(base.greenF(), overlay.greenF()),
                            mix(base.blueF(), overlay.blueF()), base.alphaF());
}

struct ThemeColors
{
    QColor accent;
    QColor accentHover;
    QColor accentText;

    std::array<QColor, 7> categoricalPalette;

    QColor bgPrimary;
    QColor bgSecondary;
    QColor bgTertiary;
    QColor bgHover;
    QColor bgSelected;

    QColor borderPrimary;
    QColor borderSecondary;
    QColor borderTick;

    QColor textPrimary;
    QColor textSecondary;
    QColor textTitle;
    QColor textDisabled;

    QColor success;
    QColor warning;
    QColor error;
    QColor info;

    QColor trackBackground;
    QColor trackBackgroundAlt;

    QColor sliderTrack;

    QColor panelBackground;
    QColor headerBackground;

    QColor shellCloseHover;
    QColor shellClosePressed;

    QColor categoricalColor(int index) const
    {
        Q_ASSERT(index >= 0);
        return categoricalPalette[static_cast<size_t>(index) % categoricalPalette.size()];
    }
    QColor stateColor(const QColor& base, ThemeStateLayer layer) const
    {
        return themeStateColor(base, textPrimary, layer);
    }
    bool operator==(const ThemeColors&) const = default;
};

struct ThemeMetrics
{
    int spacingCompact = 0;
    int spacingXSmall = 0;
    int spacingSmall = 0;
    int spacingMedium = 0;
    int spacingLarge = 0;
    int spacingXLarge = 0;

    int radiusCompact = 0;
    int radiusSmall = 0;
    int radiusMedium = 0;
    int radiusLarge = 0;

    int borderWidth = 0;
    int accentBorderWidth = 0;

    int controlHeight = 0;
    int iconExtent = 0;

    struct Layout {
        int panelPadding = 0;
        int sectionGap = 0;
        int controlGap = 0;
        int tightGap = 0;
        int fieldHeight = 0;
        int rowInset = 0;
        int rowHeight = 0;
        int sectionHeaderHeight = 0;
        bool operator==(const Layout&) const = default;
    } layout;

    struct Control {
        int compactHeight = 0;
        int largeHeight = 0;
        int smallIconExtent = 0;
        int largeIconExtent = 0;
        int textPaddingX = 0;
        int textPaddingY = 0;
        int checkboxMarkExtent = 0;
        int scrollbarExtent = 0;
        int canvasScrollbarExtent = 0;
        int scrollbarMinimumLength = 0;
        int sliderHandleExtent = 0;
        int sliderGrooveThickness = 0;
        int splitterExtent = 0;
        int buttonMinimumWidth = 0;
        int fieldMinimumWidth = 0;
        int searchMinimumWidth = 0;
        bool operator==(const Control&) const = default;
    } control;

    struct Popup {
        int minimumWidth = 0;
        int maximumHeight = 0;
        int rowPadding = 0;
        int iconColumnWidth = 0;
        int shortcutSpacing = 0;
        bool operator==(const Popup&) const = default;
    } popup;

    struct Toast {
        int minimumWidth = 0;
        int maximumWidth = 0;
        bool operator==(const Toast&) const = default;
    } toast;

    struct Connection {
        qreal lineWidth = 0;
        qreal hitWidth = 0;
        qreal maximumOverviewWidth = 0;
        bool operator==(const Connection&) const = default;
    } connection;

    struct ColorPreview {
        int swatchWidth = 0;
        int swatchHeight = 0;
        int alphaHeight = 0;
        int paletteWidth = 0;
        int paletteHeight = 0;
        int hueWidth = 0;
        int markerRadius = 0;
        bool operator==(const ColorPreview&) const = default;
    } colorPreview;

    struct Form {
        int rowSpacing = 0;
        int columnSpacing = 0;
        int labelColumnShare = 0;
        int valueColumnShare = 0;
        bool operator==(const Form&) const = default;
    } form;

    struct Chrome {
        int titleBarHeight = 0;
        int windowButtonWidth = 0;
        int windowMinimumWidth = 0;
        int windowMinimumHeight = 0;
        int progressWidth = 0;
        int progressHeight = 0;
        bool operator==(const Chrome&) const = default;
    } chrome;

    struct Switch {
        int width = 0;
        int height = 0;
        int thumbPadding = 0;
        int compactWidth = 0;
        int compactHeight = 0;
        int compactThumbPadding = 0;
        bool operator==(const Switch&) const = default;
    } toggle;

    struct Panel {
        int minimumWidth = 0;
        int minimumHeight = 0;
        int preferredWidth = 0;
        int preferredHeight = 0;
        int sidebarWidth = 0;
        int previewHeight = 0;
        bool operator==(const Panel&) const = default;
    } panel;

    struct Dialog {
        int minimumWidth = 0;
        int minimumHeight = 0;
        int compactWidth = 0;
        int compactHeight = 0;
        int preferredWidth = 0;
        int preferredHeight = 0;
        int wideWidth = 0;
        int wideHeight = 0;
        bool operator==(const Dialog&) const = default;
    } dialog;

    struct Card {
        int minimumWidth = 0;
        int minimumHeight = 0;
        bool operator==(const Card&) const = default;
    } card;

    struct Startup {
        int width = 0;
        int artworkHeight = 0;
        int logoWidth = 0;
        int progressHeight = 0;
        bool operator==(const Startup&) const = default;
    } startup;

    struct Node {
        float minimumWidth = 0;
        float headerHeight = 0;
        float bodyPadding = 0;
        float rowHeight = 0;
        float waveformHeight = 0;
        float expandedWaveformHeight = 0;
        float waveformSpacing = 0;
        float routePadding = 0;
        float routeMinimumHeight = 0;
        float pinColumnWidth = 0;
        float pinSpacing = 0;
        float pinRadius = 0;
        float pinHitRadius = 0;
        float badgeExtent = 0;
        float columnSpacing = 0;
        float rowSpacing = 0;
        float pathColumnSpacing = 0;
        float pathRowSpacing = 0;
        int sliderWidth = 0;
        int valueWidth = 0;
        bool operator==(const Node&) const = default;
    } node;

    struct TimeAxis {
        int handleBandHeight = 0;
        int handleWidth = 0;
        int handleHitRadius = 0;
        int cursorWidth = 0;
        int cursorHeight = 0;
        int labelPadding = 0;
        int minorTickLength = 0;
        int majorTickLength = 0;
        int waveformInset = 0;
        int channelHeight = 0;
        bool operator==(const TimeAxis&) const = default;
    } timeAxis;

    struct Table {
        int rowHeight = 0;
        int footerHeight = 0;
        int cellPaddingX = 0;
        int cellPaddingY = 0;
        int buttonWidth = 0;
        int buttonSpacing = 0;
        int compactColumnWidth = 0;
        int minimumColumnWidth = 0;
        int resizeHandleRadius = 0;
        bool operator==(const Table&) const = default;
    } table;

    struct Inspector {
        int pageMargin = 0;
        int sectionSpacing = 0;
        int rowSpacing = 0;
        int columnPadding = 0;
        int valueColumnWidth = 0;
        int linkSlotWidth = 0;
        int bindingSlotWidth = 0;
        int slotSpacing = 0;
        int separatorHeight = 0;
        int rangeSeparatorWidth = 0;

        int slotWidthFor(int occupiedSlots) const
        {
            if (occupiedSlots <= 0) return 0;
            const int cell = occupiedSlots == 1
                ? (linkSlotWidth > 0 ? linkSlotWidth : bindingSlotWidth)
                : bindingSlotWidth;
            return occupiedSlots * (cell + slotSpacing);
        }
        bool operator==(const Inspector&) const = default;
    } inspector;

    struct Parameter {
        int faderMinimumHeight = 0;
        int labelMinimumWidth = 0;
        int valueMinimumWidth = 0;
        int compactValueMinimumWidth = 0;
        int sliderHandleThickness = 0;
        int sliderHandleLength = 0;
        int compactSliderHeight = 0;
        int compactSliderHandleWidth = 0;
        bool operator==(const Parameter&) const = default;
    } parameter;

    struct Timeline {
        int laneHeaderWidth = 0;
        int laneHeaderMinimumWidth = 0;
        int laneHeight = 0;
        int rulerHeight = 0;
        int minLabelSpacing = 0;
        int minTickSpacing = 0;
        int clipBottomInset = 0;
        int clipMinimumWidth = 0;
        int resizeHandleWidth = 0;
        bool operator==(const Timeline&) const = default;
    } timeline;
    bool operator==(const ThemeMetrics&) const = default;
};

struct ThemeFontStyle
{
    QString family;
    int pointSize = 0;
    int weight = 0;
    bool operator==(const ThemeFontStyle&) const = default;
};

struct ThemeTypography
{
    ThemeFontStyle title{{}, 12, 600};
    ThemeFontStyle body{{}, 10, 400};
    ThemeFontStyle caption{{}, 9, 0};
    ThemeFontStyle mono;
    bool operator==(const ThemeTypography&) const = default;
};

struct ParameterPresentationStyle
{
    ParameterPresenterId inspectorFloat =
        ParameterPresenterId::SliderWithValue;
    ParameterPresenterId effectParameter =
        ParameterPresenterId::SliderWithValue;

    ParameterPresenterId presenterFor(
        ParameterPresenterContext context) const
    {
        switch (context)
        {
            case ParameterPresenterContext::InspectorFloat:
                return inspectorFloat;
            case ParameterPresenterContext::EffectParameter:
                return effectParameter;
        }
        return ParameterPresenterId::SliderWithValue;
    }
    bool operator==(const ParameterPresentationStyle&) const = default;
};

struct ComponentRecipeSlot
{
    ComponentRecipeSlotId id = ComponentRecipeSlotId::Identity;
    bool visible = true;
    int minimum = -1;
    int preferred = -1;
    int maximum = -1;
    ComponentRecipeAlignment alignment = ComponentRecipeAlignment::Fill;
    std::optional<ParameterPresenterId> presenter;
    QString asset;
    bool operator==(const ComponentRecipeSlot&) const = default;
};

struct ComponentRecipe
{
    ComponentRecipeHostId host = ComponentRecipeHostId::TrackHeader;
    ComponentRecipeBreakpoint breakpoint = ComponentRecipeBreakpoint::Default;
    ComponentRecipeVariant variant = ComponentRecipeVariant::TrackCompact;
    QVector<ComponentRecipeSlot> items;
    bool operator==(const ComponentRecipe&) const = default;
};

struct PresentationRecipes
{
    QMap<QString, ComponentRecipe> entries;

    const ComponentRecipe* recipeFor(
        ComponentRecipeHostId host,
        ComponentRecipeBreakpoint breakpoint =
            ComponentRecipeBreakpoint::Default) const
    {
        const auto it = entries.constFind(componentRecipeKey(host, breakpoint));
        return it == entries.constEnd() ? nullptr : &it.value();
    }
    bool operator==(const PresentationRecipes&) const = default;
};

struct UiTokens
{
    ThemeColors colors;
    ThemeMetrics metrics;
    ThemeTypography typography;
    ParameterPresentationStyle parameterPresenters;
    PresentationRecipes recipes;
    bool operator==(const UiTokens&) const = default;
};

struct QtPaletteColors
{
    QColor window;
    QColor windowText;
    QColor base;
    QColor alternateBase;
    QColor toolTipBase;
    QColor toolTipText;
    QColor text;
    QColor placeholderText;
    QColor button;
    QColor buttonText;
    QColor brightText;
    QColor link;
    QColor highlight;
    QColor highlightedText;
    QColor light;
    QColor dark;
    QColor mid;
    QColor disabledText;
    QColor disabledButtonText;
    QColor disabledHighlight;
    bool operator==(const QtPaletteColors&) const = default;
};

struct QtAdsStyle
{
    QColor chromeBackground;

    qreal activeTabAccentOpacity = 0.0;
    qreal dropOverlayOpacity = 0.0;

    bool operator==(const QtAdsStyle&) const = default;
};

struct ThirdPartyStyles
{
    QtAdsStyle qtAds;
    bool operator==(const ThirdPartyStyles&) const = default;
};

struct WaveformStyle
{
    QColor trimHandle;
    QColor loopHandle;

    struct StaticMode {
        QColor quiet;
        QColor peak;
        int accentQuietAlpha = 0;
        int accentPeakAlpha = 0;
        bool operator==(const StaticMode&) const = default;
    } staticMode;

    struct MultiBandMode {
        std::array<QColor, 3> colors;
        std::array<float, 3> gains{};
        float alpha = 0.0f;
        bool operator==(const MultiBandMode&) const = default;
    } multiBandMode;

    struct ColorMapMode {
        std::array<float, 4> offset{};
        std::array<float, 4> slope{};
        bool operator==(const ColorMapMode&) const = default;
    } colorMapMode;

    struct SplitStereo {
        QColor divider;
        QColor label;
        qreal dividerWidth = 0.0;
        int labelRightInset = 0;
        int labelBaselineOffset = 0;
        bool operator==(const SplitStereo&) const = default;
    } splitStereo;

    struct RawPeaks {
        QColor background;
        QColor centerLine;
        qreal cornerRadius = 0.0;
        qreal centerLineWidth = 0.0;
        bool operator==(const RawPeaks&) const = default;
    } rawPeaks;

    struct DbScale {
        QColor grid;
        QColor label;
        qreal gridWidth = 0.0;
        qreal amplitudeScale = 0.0;
        int labelLeftInset = 0;
        int labelBaselineOffset = 0;
        bool operator==(const DbScale&) const = default;
    } dbScale;

    qreal amplitudeScale = 0.0;
    bool operator==(const WaveformStyle&) const = default;
};

enum class CurveColorRole
{
    AttenuationVolume,
    AttenuationPitch,
    ParameterBinding,
    GridFade
};

struct CurveStyle
{
    struct Colors {
        QColor canvasBackground;
        QColor canvasBorder;
        QColor plotBackground;
        QColor grid;
        QColor plotBorder;
        QColor axisText;
        QColor attenuationVolume;
        QColor attenuationPitch;
        QColor parameterBinding;
        QColor gridFade;
        QColor ghost;
        QColor lassoBorder;
        QColor lassoFill;
        QColor handleLine;
        QColor handleBorder;
        QColor handleActiveBorder;
        QColor handleFill;
        QColor handleActiveFill;
        QColor pointBorder;
        QColor pointSelectedBorder;
        QColor pointSelectedFill;
        QColor crosshair;
        QColor crosshairContrast;
        QColor playhead;
        QColor readoutBackground;
        QColor readoutText;
        bool operator==(const Colors&) const = default;
    } colors;

    struct Metrics {
        int linearGraphLeftInset = 0;
        int amplitudeGraphLeftInset = 0;
        int graphTopInset = 0;
        int graphRightInset = 0;
        int graphBottomInset = 0;

        int gridColumnCount = 0;
        int linearGridRowCount = 0;
        qreal amplitudeGridTargetRowSpacing = 0.0;
        int amplitudeGridMinimumRows = 0;
        int amplitudeGridMaximumRows = 0;

        qreal gridLineWidth = 0.0;
        qreal plotBorderWidth = 0.0;
        qreal curveLineWidth = 0.0;
        qreal discreteGuideLineWidth = 0.0;
        int discreteGuideAlpha = 0;
        qreal ghostLineWidth = 0.0;
        qreal lassoBorderWidth = 0.0;
        qreal handleLineWidth = 0.0;
        qreal handleBorderWidth = 0.0;
        qreal pointBorderWidth = 0.0;
        qreal crosshairLineWidth = 0.0;

        qreal ghostRadius = 0.0;
        qreal handleRadius = 0.0;
        qreal pointRadius = 0.0;
        qreal pointSelectedRadius = 0.0;
        qreal crosshairDashLength = 0.0;
        qreal crosshairDashGap = 0.0;

        qreal xAxisLabelTopOffset = 0.0;
        qreal xAxisLabelHeight = 0.0;
        qreal yAxisLabelCenterX = 0.0;
        qreal yAxisLabelWidth = 0.0;
        qreal yAxisLabelHeight = 0.0;
        qreal amplitudeTickLabelLeft = 0.0;
        qreal amplitudeTickLabelRightGap = 0.0;
        qreal amplitudeTickLabelHalfHeight = 0.0;
        qreal amplitudeTickLabelHeight = 0.0;
        qreal linearYTickLeftOffset = 0.0;
        qreal linearYTickBaselineOffset = 0.0;
        qreal xMinTickLeftOffset = 0.0;
        qreal xTickBaselineOffset = 0.0;
        qreal xMaxLabelWidth = 0.0;
        qreal xMaxLabelTopOffset = 0.0;
        qreal xMaxLabelHeight = 0.0;
        qreal xMaxEditorVerticalExpansion = 0.0;

        qreal readoutHorizontalPadding = 0.0;
        qreal readoutHeight = 0.0;
        qreal readoutHorizontalOffset = 0.0;
        qreal readoutAboveOffset = 0.0;
        qreal readoutBelowOffset = 0.0;
        qreal readoutRadius = 0.0;
        bool operator==(const Metrics&) const = default;
    } metrics;
    bool operator==(const CurveStyle&) const = default;
};

struct MeterStyle
{
    struct Interaction {
        struct Colors {
            QColor crosshair;
            QColor accentGuide;
            QColor readoutBackground;
            QColor readoutText;
            bool operator==(const Colors&) const = default;
        } colors;

        struct Metrics {
            qreal crosshairLineWidth = 0.0;
            qreal accentGuideLineWidth = 0.0;
            qreal horizontalPadding = 0.0;
            qreal lineHeight = 0.0;
            qreal extraHeight = 0.0;
            qreal horizontalOffset = 0.0;
            qreal aboveGap = 0.0;
            qreal belowOffset = 0.0;
            qreal radius = 0.0;
            bool operator==(const Metrics&) const = default;
        } metrics;
        bool operator==(const Interaction&) const = default;
    } interaction;

    struct Peak {
        struct Colors {
            QColor canvasBackground;
            QColor trackBackground;
            QColor gradientError;
            QColor gradientWarning;
            QColor gradientYellow;
            QColor gradientAccent;
            QColor rmsOverlay;
            QColor holdHalo;
            QColor holdLine;
            QColor grid;
            QColor tickText;
            QColor clipText;
            QColor label;
            bool operator==(const Colors&) const = default;
        } colors;

        struct Metrics {
            int outerInsetLeft = 0;
            int outerInsetTop = 0;
            int outerInsetRight = 0;
            int outerInsetBottom = 0;
            int channelGap = 0;
            int reservedWidth = 0;
            qreal barHorizontalInset = 0.0;
            qreal gradientErrorStop = 0.0;
            qreal gradientWarningStop = 0.0;
            qreal gradientYellowStop = 0.0;
            qreal gradientAccentStop = 0.0;
            qreal rmsLeftInsetRatio = 0.0;
            qreal rmsWidthRatio = 0.0;
            qreal holdHaloTopOffset = 0.0;
            qreal holdHaloHeight = 0.0;
            qreal holdLineTopOffset = 0.0;
            qreal holdLineHeight = 0.0;
            qreal gridLineWidth = 0.0;
            int tickLeftOffset = 0;
            int tickWidth = 0;
            qreal tickHalfHeight = 0.0;
            qreal tickHeight = 0.0;
            int clipHeight = 0;
            int channelLabelBottomOffset = 0;
            int valueLabelBottomOffset = 0;
            int labelHeight = 0;
            bool operator==(const Metrics&) const = default;
        } metrics;
        bool operator==(const Peak&) const = default;
    } peakMeter;

    struct Spectrum {
        struct Colors {
            QColor canvasBackground;
            QColor fillTop;
            QColor fillUpperMiddle;
            QColor fillLowerMiddle;
            QColor fillBottom;
            QColor lineGlowWide;
            QColor lineGlowMedium;
            QColor lineGlowNarrow;
            QColor lineHighlight;
            QColor peakLine;
            QColor horizontalGrid;
            QColor verticalGrid;
            QColor axisText;
            bool operator==(const Colors&) const = default;
        } colors;

        struct Metrics {
            int plotInsetLeft = 0;
            int plotInsetTop = 0;
            int plotInsetRight = 0;
            int plotInsetBottom = 0;
            qreal fillTopStop = 0.0;
            qreal fillUpperMiddleStop = 0.0;
            qreal fillLowerMiddleStop = 0.0;
            qreal fillBottomStop = 0.0;
            float curveControlRatio = 0.0f;
            qreal lineGlowWideWidth = 0.0;
            qreal lineGlowMediumWidth = 0.0;
            qreal lineGlowNarrowWidth = 0.0;
            qreal lineHighlightWidth = 0.0;
            qreal peakLineWidth = 0.0;
            qreal gridLineWidth = 0.0;
            qreal dbLabelLeft = 0.0;
            qreal dbLabelHalfHeight = 0.0;
            qreal dbLabelWidth = 0.0;
            qreal dbLabelHeight = 0.0;
            qreal frequencyLabelHalfWidth = 0.0;
            qreal frequencyLabelTopOffset = 0.0;
            qreal frequencyLabelWidth = 0.0;
            qreal frequencyLabelHeight = 0.0;
            bool operator==(const Metrics&) const = default;
        } metrics;
        bool operator==(const Spectrum&) const = default;
    } spectrumMeter;

    struct Spectrogram {
        struct Colors {
            QColor canvasBackground;
            QColor emptyHistory;
            QColor grid;
            QColor axisText;
            bool operator==(const Colors&) const = default;
        } colors;

        struct Metrics {
            int plotInsetLeft = 0;
            int plotInsetTop = 0;
            int plotInsetRight = 0;
            int plotInsetBottom = 0;
            qreal gridLineWidth = 0.0;
            qreal frequencyLabelLeft = 0.0;
            qreal frequencyLabelHalfHeight = 0.0;
            qreal frequencyLabelWidth = 0.0;
            qreal frequencyLabelHeight = 0.0;
            qreal timeLabelTopOffset = 0.0;
            qreal timeLabelHeight = 0.0;
            bool operator==(const Metrics&) const = default;
        } metrics;
        bool operator==(const Spectrogram&) const = default;
    } spectrogramMeter;
    bool operator==(const MeterStyle&) const = default;
};

struct DomainColors
{
    MeterStyle meter;
    WaveformStyle waveform;
    CurveStyle curve;

    struct Startup {
        std::array<QColor, 4> wavePalette;
        std::array<QColor, 3> logoPalette;
        bool operator==(const Startup&) const = default;
    } startup;

    struct PinDomain {
        QColor audio;
        QColor control;
        QColor value;
        QColor ring;
        QColor targetOk;
        QColor targetBad;
        bool operator==(const PinDomain&) const = default;
    } pinDomain;

    struct NodeGraphNode {
        QColor bodyNormal;
        QColor bodySelected;
        QColor borderNormal;
        QColor borderHoverMix;

        QColor shadow;

        QColor titleText;
        QColor contentLabel;
        QColor contentValue;

        QColor sandboxBadgeBg;
        QColor sandboxBadgeText;
        QColor badgeOverride;
        QColor badgeReceive;

        QColor waveformBg;
        QColor waveformCenter;
        QColor blueprintWaveform;
        bool operator==(const NodeGraphNode&) const = default;
    } nodeGraphNode;

    struct Blueprint {
        QColor canvasBackground;
        QColor gridDot;
        QColor edgeLabelBg;
        QColor edgeLabelText;
        QColor placementChain;
        QColor organizationChain;
        QColor containment;
        QColor send;
        bool operator==(const Blueprint&) const = default;
    } blueprint;

    struct Timeline {
        QColor entry;
        QColor exit;
        QColor transition;
        QColor clip;
        QColor clipSelected;
        QColor clipText;
        QColor emergenceMarker;
        QColor unavailableOverlay;
        QColor unavailableText;
        QColor outOfSpanVeil;
        bool operator==(const Timeline&) const = default;
    } timeline;

    struct Music {
        QColor beatGridMajor;
        QColor beatGridMinor;
        QColor cursor;
        bool operator==(const Music&) const = default;
    } music;

    struct Log {
        QColor debug;
        QColor info;
        QColor warning;
        QColor error;
        QColor critical;
        bool operator==(const Log&) const = default;
    } log;

    struct AI {
        QColor brandAccent;
        bool operator==(const AI&) const = default;
    } ai;

    bool operator==(const DomainColors&) const = default;
};

struct ThemeSnapshot
{
    quint64 generation = 0;
    qreal uiScale = 1.0;
    ThemeColorScheme colorScheme = ThemeColorScheme::Dark;
    UiTokens ui;
    QtPaletteColors qtPalette;
    ThirdPartyStyles thirdParty;
    DomainColors domains;
    bool operator==(const ThemeSnapshot&) const = default;
};

using ThemeSnapshotPtr = std::shared_ptr<const ThemeSnapshot>;

}

Q_DECLARE_METATYPE(VNX::ThemeSnapshotPtr)
