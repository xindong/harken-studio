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
    QColor viewBorder;
    QColor dropHighlight;
    QColor dropAccent;

    QColor textPrimary;
    QColor textSecondary;
    QColor textTitle;
    QColor textMuted;
    QColor textDisabled;

    QColor success;
    QColor warning;
    QColor error;
    QColor info;

    QColor trackBackground;
    QColor trackBackgroundAlt;
    QColor busBackground;
    QColor busBackgroundActive;

    QColor sliderTrack;
    QColor knobIndicator;

    QColor panelBackground;
    QColor sectionSeparator;
    QColor headerBackground;

    QColor auditionBg;
    QColor auditionPlayActive;
    QColor auditionStopActive;

    QColor shellMenuText;
    QColor shellMenuHoverText;
    QColor shellButtonText;
    QColor shellButtonHoverText;
    QColor shellCloseHover;
    QColor shellClosePressed;

    QColor categoricalColor(int index) const
    {
        Q_ASSERT(index >= 0);
        return categoricalPalette[static_cast<size_t>(index) % categoricalPalette.size()];
    }
    bool operator==(const ThemeColors&) const = default;
};

struct ThemeMetrics
{
    int radiusSmall = 0;
    int radiusMedium = 0;
    int radiusLarge = 0;

    int spacingXSmall = 0;
    int spacingSmall = 0;
    int spacingMedium = 0;
    int spacingLarge = 0;

    int controlHeight = 0;
    int sectionHeaderHeight = 0;
    int navigationChevronExtent = 0;
    int objectIconExtent = 0;

    struct Table {
        int headerHeight = 0;
        int rowHeight = 0;
        int footerHeight = 0;
        int cellPaddingX = 0;
        int cellPaddingY = 0;
        int buttonWidth = 0;
        int buttonHeight = 0;
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
        int rowHeight = 0;
        int labelColumnShare = 0;
        int valueColumnShare = 0;
        int formLabelWidth = 0;
        int valueColumnWidth = 0;
        int linkSlotWidth = 0;
        int sourceSlotWidth = 0;
        int platformSlotWidth = 0;
        int bindingSlotWidth = 0;
        int slotHeight = 0;
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
        int knobExtent = 0;
        int faderMinimumHeight = 0;
        int labelMinimumWidth = 0;
        int valueMinimumWidth = 0;
        int compactValueMinimumWidth = 0;
        int sliderGrooveThickness = 0;
        int sliderHandleThickness = 0;
        int sliderHandleLength = 0;
        bool operator==(const Parameter&) const = default;
    } parameter;

    struct Timeline {
        int laneHeaderWidth = 0;
        int laneHeaderMinimumWidth = 0;
        int laneHeight = 0;
        int blankRowHeight = 0;
        int laneSpacing = 0;
        int rulerHeight = 0;
        int markerLaneHeight = 0;
        int tickLength = 0;
        int majorTickLength = 0;
        int minLabelSpacing = 0;
        int minTickSpacing = 0;
        int clipVerticalInset = 0;
        int clipRadius = 0;
        int clipMinimumWidth = 0;
        int resizeHandleWidth = 0;
        int markerHitRadius = 0;
        int statusBarHeight = 0;
        int compactHeaderWidth = 0;
        int queueLaneLabelWidth = 0;
        int headerGripWidth = 0;
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
    ThemeFontStyle heading;
    ThemeFontStyle title;
    ThemeFontStyle body;
    ThemeFontStyle caption;
    ThemeFontStyle axis;
    ThemeFontStyle mono;
    ThemeFontStyle label;
    bool operator==(const ThemeTypography&) const = default;
};

struct ParameterPresentationStyle
{
    ParameterPresenterId inspectorFloat =
        ParameterPresenterId::SliderWithValue;
    ParameterPresenterId effectParameter =
        ParameterPresenterId::RotaryKnob;

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
    QColor inactiveTabText;
    QColor closeButtonHover;
    QColor closeButtonPressed;
    QColor lightHoverOverlay;

    qreal activeTabAccentOpacity = 0.0;
    qreal dropOverlayOpacity = 0.0;

    struct Metrics {
        int borderWidth = 0;
        int titleBarMinimumHeight = 0;
        int titleBarButtonPadding = 0;
        int titleBarButtonRadius = 0;
        int titleBarIconSize = 0;
        int closeButtonPadding = 0;
        int closeButtonExtent = 0;
        int closeButtonRadius = 0;
        int tabBarMinimumHeight = 0;
        int tabAccentBorderWidth = 0;
        int tabGapWidth = 0;
        int tabPaddingTop = 0;
        int tabPaddingHorizontal = 0;
        int tabPaddingBottom = 0;
        int tabMinimumHeight = 0;
        int tabFontSize = 0;
        int tabIconSize = 0;
        int tabCloseMarginTop = 0;
        int tabClosePaddingRight = 0;
        int overlayBorderWidth = 0;
        int splitterHandleExtent = 0;
        int autoHideTabIconSize = 0;
        int autoHideTabPaddingLeft = 0;
        int autoHideTabPaddingRight = 0;
        int autoHideTabMinimumHeight = 0;
        int autoHideSideBarSpacing = 0;
        int autoHideTitleBorderWidth = 0;
        int autoHideTitlePaddingLeft = 0;
        bool operator==(const Metrics&) const = default;
    } metrics;
    bool operator==(const QtAdsStyle&) const = default;
};

struct ThirdPartyStyles
{
    QtAdsStyle qtAds;
    bool operator==(const ThirdPartyStyles&) const = default;
};

struct WaveformStyle
{
    QColor line;
    QColor fill;
    QColor selectionHighlight;
    QColor cursor;
    QColor regionMarker;
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
        int labelPointSize = 0;
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
        int labelPointSize = 0;
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

        int readoutFontPixelSize = 0;
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
    QColor green;
    QColor yellow;
    QColor red;
    QColor peak;
    QColor rms;
    QColor clipOn;
    QColor clipOff;
    QColor background;

    QColor lufsMomentary;
    QColor lufsShortTerm;
    QColor lufsIntegrated;
    QColor oscilloscopeGrid;
    QColor oscilloscopeGlow;
    QColor waveformFill;
    QColor waveformGrid;

    struct Stereo {
        QColor trailOuter;
        QColor trailMiddle;
        QColor trailCore;
        QColor gridPrimary;
        QColor gridSecondary;
        QColor liveGlowWide;
        QColor liveGlowMedium;
        QColor liveHighlight;
        QColor correlationPositive;
        QColor correlationNegative;
        bool operator==(const Stereo&) const = default;
    } stereo;

    struct Layout {
        QColor dividerHover;
        QColor dividerDragging;
        QColor dividerSnapped;
        QColor dropBorder;
        QColor dropFill;
        bool operator==(const Layout&) const = default;
    } layout;

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
            QColor clipBackground;
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

    struct Loudness {
        struct Colors {
            QColor canvasBackground;
            QColor trackBackground;
            QColor targetLine;
            QColor targetText;
            QColor labelText;
            QColor valueText;
            bool operator==(const Colors&) const = default;
        } colors;

        struct Metrics {
            int outerInsetLeft = 0;
            int outerInsetTop = 0;
            int outerInsetRight = 0;
            int outerInsetBottom = 0;
            int reservedWidth = 0;
            int barGap = 0;
            int minimumBarWidth = 0;
            qreal gradientTopStop = 0.0;
            qreal gradientBottomStop = 0.0;
            int gradientTopLighterFactor = 0;
            int gradientBottomDarkerFactor = 0;
            int highlightLighterFactor = 0;
            int highlightAlpha = 0;
            qreal highlightMinimumBarHeight = 0.0;
            qreal highlightHeight = 0.0;
            qreal targetLineWidth = 0.0;
            qreal targetLabelHorizontalInset = 0.0;
            qreal targetLabelTopOffset = 0.0;
            qreal targetLabelHeight = 0.0;
            qreal channelLabelHeight = 0.0;
            qreal valueLabelBottomOffset = 0.0;
            qreal valueLabelHeight = 0.0;
            bool operator==(const Metrics&) const = default;
        } metrics;
        bool operator==(const Loudness&) const = default;
    } loudnessMeter;

    struct VU {
        struct Colors {
            QColor canvasBackground;
            QColor titleText;
            QColor trackBackground;
            QColor analogArc;
            QColor analogNegativeTick;
            QColor analogPositiveTick;
            QColor analogOverloadArc;
            QColor analogNeedleGlow;
            QColor analogNeedle;
            QColor analogPivot;
            QColor analogValueText;
            QColor analogLabelText;
            QColor barWarning;
            QColor barGrid;
            QColor barLabelText;
            QColor barValueText;
            bool operator==(const Colors&) const = default;
        } colors;

        struct Metrics {
            int outerInsetLeft = 0;
            int outerInsetTop = 0;
            int outerInsetRight = 0;
            int outerInsetBottom = 0;
            int titleHeight = 0;
            int contentTopInset = 0;
            int analogGap = 0;
            qreal analogCornerRadius = 0.0;
            int analogInnerInsetLeft = 0;
            int analogInnerInsetTop = 0;
            int analogInnerInsetRight = 0;
            int analogInnerInsetBottom = 0;
            int analogLabelStripBottomOffset = 0;
            int analogLabelStripHeight = 0;
            qreal analogPivotBottomOffset = 0.0;
            qreal analogRadiusScale = 0.0;
            qreal analogArcStartPiFactor = 0.0;
            qreal analogArcEndPiFactor = 0.0;
            int analogArcSegments = 0;
            qreal analogArcLineWidth = 0.0;
            qreal analogTickInnerRadiusScale = 0.0;
            qreal analogTickOuterRadiusScale = 0.0;
            qreal analogTickLineWidth = 0.0;
            qreal analogTickLabelRadiusScale = 0.0;
            qreal analogTickLabelHalfWidth = 0.0;
            qreal analogTickLabelTopOffset = 0.0;
            qreal analogTickLabelWidth = 0.0;
            qreal analogTickLabelHeight = 0.0;
            qreal analogOverloadRadiusScale = 0.0;
            int analogOverloadSegments = 0;
            qreal analogOverloadLineWidth = 0.0;
            qreal analogNeedleLengthScale = 0.0;
            qreal analogNeedleGlowWidth = 0.0;
            qreal analogNeedleWidth = 0.0;
            qreal analogPivotRadius = 0.0;
            int analogValueHeight = 0;
            int analogLabelTopOffset = 0;
            int analogLabelHeight = 0;
            int barGap = 0;
            qreal barCornerRadius = 0.0;
            qreal barHorizontalInset = 0.0;
            qreal barOverloadStop = 0.0;
            qreal barWarningStop = 0.0;
            qreal barYellowStop = 0.0;
            qreal barBrightGreenStop = 0.0;
            qreal barGreenStop = 0.0;
            int barOverloadAlpha = 0;
            int barGreenLighterFactor = 0;
            qreal barGridLineWidth = 0.0;
            int barChannelLabelBottomOffset = 0;
            int barChannelLabelHeight = 0;
            int barValueLabelBottomOffset = 0;
            int barValueLabelHeight = 0;
            bool operator==(const Metrics&) const = default;
        } metrics;
        bool operator==(const VU&) const = default;
    } vuMeter;

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
            QColor timeText;
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

struct FeedbackStyle
{
    QColor success;
    QColor warning;
    QColor error;
    QColor info;
    QColor neutral;
    QColor overlayBackground;
    QColor overlayErrorBackground;
    QColor overlayHighlight;
    bool operator==(const FeedbackStyle&) const = default;
};

struct DomainColors
{
    MeterStyle meter;
    FeedbackStyle feedback;
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
        QColor dropTarget;

        QColor shadow;

        QColor titleText;
        QColor contentLabel;
        QColor contentValue;

        QColor sandboxBadgeBg;
        QColor sandboxBadgeText;
        QColor badgeOverride;
        QColor badgeChild;
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
        QColor busRoute;
        QColor send;
        QColor overrideRoute;
        bool operator==(const Blueprint&) const = default;
    } blueprint;

    struct Timeline {
        QColor entry;
        QColor exit;
        QColor transition;
        QColor clip;
        QColor clipSelected;
        QColor clipBorder;
        QColor clipSelectedBorder;
        QColor clipText;
        QColor badge;
        QColor emergenceMarker;
        QColor unavailableOverlay;
        QColor unavailableText;
        QColor outOfSpanVeil;
        bool operator==(const Timeline&) const = default;
    } timeline;

    struct Music {
        static constexpr int kTrackColorCount = 12;
        std::array<QColor, kTrackColorCount> trackColors;

        QColor beatGridMajor;
        QColor beatGridMinor;
        QColor cursor;
        QColor loopRegion;
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

    struct Monitor {
        QColor stateStolen;
        QColor statePlaying;
        QColor stateStarting;
        QColor stateVirtual;
        QColor stateReleasing;
        QColor stateStopping;
        QColor statePaused;
        QColor stateDefault;

        QColor virtualDim;
        QColor mutedText;
        QColor lowVolume;
        QColor busIdText;

        QColor peakClip;
        QColor peakCaution;
        QColor peakSafe;

        QColor pitchHigh;
        QColor pitchLow;

        QColor cpuCritical;
        QColor cpuWarning;
        QColor cpuSafe;
        QColor bufferWarning;
        QColor bufferNormal;

        QColor underrunFlash;
        QColor underrunIdle;
        QColor underrunNone;

        QColor spatialGainHigh;
        QColor spatialGainLow;
        QColor spatialLpf;
        QColor spatialWidth;
        QColor spatialDistance;
        QColor spatialReverb;
        QColor spatialDopplerHi;
        QColor spatialDopplerLo;
        QColor spatialDot;
        bool operator==(const Monitor&) const = default;
    } monitor;

    struct AI {
        QColor brandAccent;
        QColor userAccent;
        bool operator==(const AI&) const = default;
    } ai;

    struct Scene3D {
        QColor listener;
        QColor listenerCone;
        QColor room;
        QColor emitterMin;
        QColor emitterMax;
        QColor hudBackground;
        QColor hudTitle;
        QColor hudLabel;
        QColor hudGain;
        QColor hudGainLow;
        QColor hudLpf;
        QColor hudWidth;
        QColor dopplerIdle;
        bool operator==(const Scene3D&) const = default;
    } scene3d;

    struct Spatial {
        QColor background;
        QColor grid;
        QColor axis;
        QColor axisLabel;
        QColor tangentLine;
        QColor tangentHover;
        QColor tangentActive;
        QColor pointSelected;
        QColor previewMarker;
        QColor infoText;
        QColor legendBg;
        QColor layerVolume;
        QColor layerLpf;
        QColor layerWidth;
        QColor layerReverb;
        bool operator==(const Spatial&) const = default;
    } spatial;
    bool operator==(const DomainColors&) const = default;
};

struct ThemeSnapshot
{
    quint64 generation = 0;
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
