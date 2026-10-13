#pragma once

#include "TimelineTicks.h"

#include "Core/AssetPipeline/WaveformCache.h"
#include "Core/ThemeSnapshot.h"
#include "Core/Timeline/TimelineTypes.h"
#include "Widgets/Common/TimeAxis.h"
#include "Widgets/Common/AmplitudeAxis.h"

#include <QColor>
#include <QIcon>
#include <QLine>
#include <QRect>
#include <QString>
#include <QUuid>
#include <QVector>

#include <cstdint>
#include <optional>

class QPainter;

namespace VNX {

struct TimelineStyleView
{
    QColor canvasBackground;
    QColor canvasBorder;
    QColor placeholderText;
    QColor axisText;
    QColor trackBackground;
    QColor trackBackgroundAlt;
    QColor accent;
    QColor textMuted;
    DomainColors::Timeline timeline;
    DomainColors::Music music;
    WaveformStyle waveform;
    double waveformExponent = 1.0;
    CurveStyle curve;
    ThemeMetrics metrics;
};

struct TimelineLaneRect
{
    QRect rect;
    bool alternate = false;
    bool highlighted = false;
    bool selected = false;
    QColor accent;
};

struct TimelineGridLine
{
    QLine line;
    bool major = false;
};

enum class TimelineSemanticBandKind : uint8_t
{
    Nominal = 0,
    Pickup,
    Tail,
    Unavailable
};

struct TimelineSemanticBand
{
    TimelineSemanticBandKind kind = TimelineSemanticBandKind::Nominal;
    QRect rect;
    QString label;
};

enum class TimelineClipVisualState : uint8_t
{
    Normal = 0,
    Hover,
    Selected,
    Dragging
};

enum class TimelineClipBadgeKind : uint8_t
{
    Mode = 0,
    Clock,
    Loop,
    External
};

struct TimelineClipBadge
{
    TimelineClipBadgeKind kind = TimelineClipBadgeKind::Mode;
    QString text;
    bool accent = false;
};

struct TimelineClipItem
{
    QRect rect;
    QString label;
    QString tooltip;
    QIcon typeIcon;
    QVector<TimelineClipBadge> badges;
    QColor objectColor;
    TimelineClipVisualState state = TimelineClipVisualState::Normal;
    bool foreign = false;
    bool muted = false;
    bool residual = false;
    int laneIndex = -1;
    int clipIndex = -1;
    QUuid guid;
    std::optional<int> emergencePixel;
    std::optional<int64_t> emergenceCoordinate;
    bool emergenceClamped = false;
    const WaveformPeakData* waveform = nullptr;
    QString waveformPath;
    QRect waveformRect;
    QRect waveformViewport;
    double waveformStartFrame = 0.0;
    double waveformFrameCount = 0.0;
};

struct TimelineCrossfadeRegion
{
    QRect rect;
};

enum class TimelineMarkerShape : uint8_t
{
    Line = 0,
    EntryFlag,
    ExitFlag,
    Diamond
};

struct TimelineMarker
{
    int x = 0;
    int height = 0;
    QString label;
    QString tooltip;
    MarkerKind kind = MarkerKind::Custom;
    TimelineMarkerShape shape = TimelineMarkerShape::Line;
    QColor color;
    int labelMaxWidth = 0;
    bool selected = false;
};

struct TimelineNominalBar
{
    bool visible = false;
    QRect rect;
};

struct TimelineCursor
{
    bool visible = false;
    int x = 0;
};

struct TimelinePlayhead
{
    bool visible = false;
    int x = 0;
};

struct TimelineDropIndicator
{
    bool visible = false;
    int x = 0;
};

struct TimelineSnapGuide
{
    bool visible = false;
    int x = 0;
};

struct TimelineLasso
{
    bool visible = false;
    QRect rect;
};

struct TimelineGhost
{
    bool visible = false;
    QRect rect;
    int laneIndex = -1;
    bool illegal = false;
};

struct TimelineRenderData
{
    QRect viewport;
    double viewOffset = 0;
    qreal pxPerUnit = 0.0;
    TimelineDomain domain = TimelineDomain::Absolute;
    QVector<TimelineLaneRect> laneRects;
    QVector<TimelineGridLine> gridLines;
    QVector<AmplitudeAxisTick> amplitudeTicks;
    TimeAxisTicks timeAxis;
    bool frameless = false;
    QVector<TimelineSemanticBand> semanticBands;
    QVector<TimelineClipItem> clipItems;
    QVector<TimelineCrossfadeRegion> crossfadeRegions;
    QVector<TimelineMarker> markers;
    TimelineNominalBar nominalBar;
    TimelineCursor editCursor;
    TimelinePlayhead playhead;
    TimelineDropIndicator dropIndicator;
    TimelineSnapGuide snapGuide;
    TimelineLasso lasso;
    QVector<TimelineGhost> ghosts;
    QString placeholder;
};

class TimelineRenderer
{
public:
    static void render(QPainter& painter,
                       const TimelineStyleView& style,
                       const TimelineRenderData& data);
    static void drawVerticalLine(QPainter& painter, const QColor& color, int x, const QRect& bounds);
    static void drawTickSet(QPainter& painter,
                            const TimelineStyleView& style,
                            const TimelineTickSet& ticks,
                            int originX,
                            int height,
                            int leftBound,
                            int rightBound,
                            int labelTop,
                            int labelHeight);
    static QVector<AmplitudeAxisTick> amplitudeTicks(
        const QRect& viewport, double zoom, int scroll, const ThemeSnapshot& theme,
        double amplitudeExponent = 1.0);

    static void drawMarker(QPainter& painter, int x, int height,
                        const QString& label, const QColor& color);
    static void drawClip(QPainter& painter, const QRect& area,
                         const decltype(DomainColors::timeline)& style,
                         bool selected, const QString& label);
};

}
