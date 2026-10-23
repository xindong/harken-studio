#include "TimelineEditorPanel.h"
#include "TimelineCanvas.h"
#include "TimelineHeaderColumn.h"
#include "Widgets/Common/TableWidget.h"
#include "TimelineRuler.h"
#include "TimelineTicks.h"

#include "Core/ActionManager.h"
#include "Core/AuthoringPolicy.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/Components/VAudio.h"
#include "Core/Components/VMixNode.h"
#include "Core/BindingSemantics.h"
#include "Core/Components/VPiece.h"
#include "Core/Components/VSound.h"
#include "Core/Components/VTrack.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/LanguageContextService.h"
#include "Core/NumericFormat.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/QtControlSkin.h"
#include "Core/SoundSourceEditing.h"
#include "Core/ThemeManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/FontManager.h"
#include "Core/Timeline/TimelineCoordinates.h"
#include "Core/Timeline/TimelineEditing.h"
#include "Core/Timeline/TimelineSemantics.h"
#include "Core/AssetPipeline/AudioImportGate.h"
#include "Core/AssetPipeline/ImportAssetsCommand.h"
#include "Core/UndoCommands.h"
#include "Widgets/Common/DecimalSpinBox.h"
#include "Widgets/Common/FocusFilter.h"

#include <QEvent>
#include <QCoreApplication>
#include <QJsonArray>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QDialog>
#include <QGuiApplication>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QShortcut>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QStackedWidget>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace VNX {

namespace {

constexpr int kScrollSteps = 1000000;
constexpr double kMinimumViewNanoseconds = 1000.0;

int laneIndexFromWidget(const TimelineEditorPanel* panel, const QWidget* widget)
{
    for (const QWidget* current = widget; current; current = current->parentWidget()) {
        const QVariant guidValue = current->property("trackGuid");
        if (!guidValue.isValid())
            continue;
        const QUuid guid = QUuid::fromString(guidValue.toString());
        for (int index = 0; index < panel->laneCount(); ++index)
            if (panel->laneGuid(index) == guid)
                return index;
    }
    return -1;
}

bool isOrDescendsFrom(const ProjectItem* item, const ProjectItem* ancestor)
{
    for (const ProjectItem* current = item; current; current = current->parent) {
        if (current == ancestor)
            return true;
    }
    return false;
}

void clearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

int64_t coordinateUnit(const PieceTimeSpec& spec)
{
    return timelineCoordinateUnit(spec);
}

std::optional<int64_t> sourceSpan(const ProjectItem* item, QSet<QUuid>& visiting)
{
    if (!item || visiting.contains(item->guid))
        return std::nullopt;
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    const auto changesSpan = [](const PlayablePropertyOccurrenceV1& properties) {
        return properties.presence.Has(Property::Pitch)
            || properties.presence.Has(Property::LoopCount);
    };
    if (item->type != ProjectItem::Type::Sound || !visiting.isEmpty()) {
        if (const auto* audio = item->comp<VAudio>(); audio && audio->loopCount != 1)
            return std::nullopt;
        if (const auto* mix = item->comp<VMixNode>(); mix &&
            (mix->pitch != 0.0f
             || mix->bindings.findFirst(BindingSemantics::pitchAddress(item->type))))
            return std::nullopt;
        if (std::any_of(item->platformOverrides.rows.begin(), item->platformOverrides.rows.end(),
                        [&changesSpan](const auto& row) { return changesSpan(row.properties); }))
            return std::nullopt;
        if (const auto* sound = item->comp<VSound>(); sound && sound->scheduleOffsetSec != 0.0)
            return std::nullopt;
    }
    if (item->type == ProjectItem::Type::Sound) {
        const auto seconds = SoundSourceEditing::activeTrimmedDuration(
            *item, PlatformContextService::instance().context().primary,
            SoundSourceEditing::previewLanguage(
                *item, LanguageContextService::instance().language()));
        if (!seconds) return std::nullopt;
        const CoordinateResult duration = TimelineCoordinates::fromSeconds(*seconds, {});
        return duration && duration.value > 0
            ? std::optional<int64_t>(duration.value) : std::nullopt;
    }
    visiting.insert(item->guid);
    int64_t end = 0;
    bool known = true;
    if (const auto* track = item->comp<VTrack>()) {
        for (const Clip& clip : track->clips.items) {
            const auto duration = sourceSpan(
                ProjectManager::instance().findItemByGuid(clip.targetGuid), visiting);
            if (!duration || clip.start > std::numeric_limits<int64_t>::max() - *duration) {
                known = false;
                break;
            }
            end = std::max(end, clip.start + *duration);
        }
    } else if (const auto* piece = item->comp<VPiece>();
               piece && AuthoringPolicy::validatePieceTime(piece->pieceTime)) {
        for (const auto& track : item->children) {
            const auto duration = sourceSpan(track.get(), visiting);
            if (!duration) {
                known = false;
                break;
            }
            end = std::max(end, *duration);
        }
    } else {
        known = false;
    }
    visiting.remove(item->guid);
    return known && end > 0 ? std::optional<int64_t>(end) : std::nullopt;
}

}

TimelineEditorPanel::TimelineEditorPanel(QWidget* parent,
                                         ThemeManager* themeManager)
    : QWidget(parent)
    , themeManager_(themeManager ? themeManager : &ThemeManager::instance())
{
    setObjectName(QStringLiteral("TimelineEditorPanel"));
    setFocusPolicy(Qt::StrongFocus);
    cursorEdgeScroll_ = new QTimer(this);
    cursorEdgeScroll_->setInterval(16);
    connect(cursorEdgeScroll_, &QTimer::timeout, this, &TimelineEditorPanel::scrollCursorDrag);

    auto* root = new QVBoxLayout(this);
    const auto& metrics = themeManager_->metrics();
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    toolbar_ = new QWidget(this);
    toolbar_->setObjectName(QStringLiteral("timelineToolbar"));
    auto* tools = new QHBoxLayout(toolbar_);
    tools->setContentsMargins(0, 0, 0, 0);
    breadcrumbBar_ = new QWidget(toolbar_);
    breadcrumbBar_->setObjectName(QStringLiteral("timelineBreadcrumbs"));
    breadcrumbLayout_ = new QHBoxLayout(breadcrumbBar_);
    breadcrumbLayout_->setContentsMargins(0, 0, 0, 0);
    breadcrumbLayout_->setSpacing(metrics.spacingXSmall);
    tools->addWidget(breadcrumbBar_, 1);
    root->addWidget(toolbar_);

    stack_ = new QStackedWidget(this);
    root->addWidget(stack_, 1);
    stateLabel_ = new QLabel(stack_);
    stateLabel_->setObjectName(QStringLiteral("timelineState"));
    stateLabel_->setAlignment(Qt::AlignCenter);
    stateLabel_->setWordWrap(true);
    stack_->addWidget(stateLabel_);
    timelinePage_ = new QFrame(stack_);
    timelinePage_->setObjectName(QStringLiteral("timelineViewportPage"));
    timelinePage_->setFrameShape(QFrame::NoFrame);
    auto* timelineLayout = new QVBoxLayout(timelinePage_);
    timelineLayout->setContentsMargins(0, 0, 0, 0);
    timelineLayout->setSpacing(0);
    ruler_ = new TimelineRuler(this, timelinePage_);
    ruler_->installEventFilter(this);
    timelineLayout->addWidget(ruler_);

    addTrackButton_ = new QToolButton(ruler_);
    addTrackButton_->setObjectName(QStringLiteral("timelineAddTrackButton"));
    addTrackButton_->setToolTip(tr("Add Track"));
    addTrackButton_->setAccessibleName(addTrackButton_->toolTip());
    addTrackButton_->setAutoRaise(true);
    addTrackButton_->setProperty("flatIconButton", true);
    addTrackButton_->setCursor(Qt::PointingHandCursor);
    connect(addTrackButton_, &QToolButton::clicked, this, [this] {
        if (viewKind_ != ViewKind::Piece) return;
        auto* track = ProjectManager::instance().createItem(
            boundItem_, QStringLiteral("Track"), ProjectItem::Type::Track);
        if (track) selectLane(track->guid, true);
    });

    scrollArea_ = new QScrollArea(timelinePage_);
    scrollArea_->setObjectName(QStringLiteral("timelineViewport"));
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea_->viewport()->installEventFilter(this);
    laneHost_ = new QWidget(scrollArea_);
    laneHost_->setObjectName(QStringLiteral("timelineLaneHost"));
    auto* laneRow = new QHBoxLayout(laneHost_);
    laneRow->setContentsMargins(0, 0, 0, 0);
    laneRow->setSpacing(0);
    headerColumn_ = new TimelineHeaderColumn(this, laneHost_);
    canvas_ = new TimelineCanvas(this, laneHost_);
    laneRow->addWidget(headerColumn_);
    laneRow->addWidget(canvas_, 1);
    laneLayout_ = headerColumn_->laneLayout();
    scrollArea_->setWidget(laneHost_);
    auto* contentRow = new QHBoxLayout;
    contentRow->setContentsMargins(0, 0, 0, 0);
    contentRow->setSpacing(0);
    contentRow->addWidget(scrollArea_, 1);
    verticalTools_ = new QFrame(timelinePage_);
    verticalTools_->setObjectName(QStringLiteral("timelineVerticalTools"));
    verticalTools_->setFrameShape(QFrame::NoFrame);
    auto* verticalLayout = new QVBoxLayout(verticalTools_);
    verticalLayout->setContentsMargins(0, 0, 0, 0);
    verticalLayout->setSpacing(0);
    verticalZoomInButton_ = new QToolButton(verticalTools_);
    verticalZoomOutButton_ = new QToolButton(verticalTools_);
    verticalZoomInButton_->setObjectName(QStringLiteral("timelineVerticalZoomIn"));
    verticalZoomOutButton_->setObjectName(QStringLiteral("timelineVerticalZoomOut"));
    verticalZoomInButton_->setToolTip(tr("Zoom in vertically"));
    verticalZoomOutButton_->setToolTip(tr("Zoom out vertically"));
    for (auto* button : {verticalZoomInButton_, verticalZoomOutButton_}) {
        button->setAccessibleName(button->toolTip());
        button->setAutoRaise(true);
        button->setProperty("flatIconButton", true);
        button->setCursor(Qt::PointingHandCursor);
    }
    connect(verticalZoomInButton_, &QToolButton::clicked, this,
        [this] { zoomVertically(trackWaveformRect().center().y(), 1.25); });
    connect(verticalZoomOutButton_, &QToolButton::clicked, this,
        [this] { zoomVertically(trackWaveformRect().center().y(), 0.8); });
    verticalScrollBar_ = new QScrollBar(Qt::Vertical, verticalTools_);
    verticalScrollBar_->setObjectName(QStringLiteral("timelineVerticalScroll"));
    QtControlSkin::setSemanticProperty(verticalScrollBar_, "scrollRole", QStringLiteral("canvas"));
    connect(verticalScrollBar_, &QScrollBar::valueChanged, this, [this](int value) {
        trackVerticalRatio_ = verticalScrollBar_->maximum() > 0
            ? double(value) / verticalScrollBar_->maximum() : 0.0;
        refreshTimelinePaint();
    });
    verticalLayout->addWidget(verticalZoomInButton_);
    verticalLayout->addWidget(verticalScrollBar_, 1);
    verticalLayout->addWidget(verticalZoomOutButton_);
    contentRow->addWidget(verticalTools_);
    timelineLayout->addLayout(contentRow, 1);
    connect(scrollArea_->verticalScrollBar(), &QScrollBar::valueChanged,
        this, [this] { refreshTimelinePaint(); });

    scrollRow_ = new QHBoxLayout;
    scrollRow_->setContentsMargins(0, 0, 0, 0);
    scrollRow_->setSpacing(0);
    horizontalSpacer_ = new QWidget(timelinePage_);
    scrollRow_->addWidget(horizontalSpacer_);
    auto* scrollControls = new QFrame(timelinePage_);
    scrollControls->setObjectName(QStringLiteral("timelineScrollControls"));
    scrollControls->setFrameShape(QFrame::NoFrame);
    auto* scrollControlsLayout = new QHBoxLayout(scrollControls);
    scrollControlsLayout->setContentsMargins(0, 0, 0, 0);
    scrollControlsLayout->setSpacing(0);
    scrollRow_->addWidget(scrollControls, 1);
    const auto zoomButton = [this, scrollControls](const QString& name, const QString& text, double factor) {
        auto* button = new QToolButton(scrollControls);
        button->setObjectName(name);
        button->setToolTip(text);
        button->setAccessibleName(text);
        button->setAutoRaise(true);
        button->setProperty("flatIconButton", true);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QToolButton::clicked, this, [this, factor] {
            zoomAroundPixel(canvas_->width() / 2, factor);
        });
        return button;
    };
    zoomInButton_ = zoomButton(QStringLiteral("timelineZoomIn"), tr("Zoom in time"), 2.0);
    zoomOutButton_ = zoomButton(QStringLiteral("timelineZoomOut"), tr("Zoom out time"), 0.5);
    horizontalScrollBar_ = new QScrollBar(Qt::Horizontal, scrollControls);
    horizontalScrollBar_->setObjectName(QStringLiteral("timelineHorizontalScroll"));
    QtControlSkin::setSemanticProperty(horizontalScrollBar_, "scrollRole", QStringLiteral("canvas"));
    scrollControlsLayout->addWidget(zoomInButton_);
    scrollControlsLayout->addWidget(horizontalScrollBar_, 1);
    scrollControlsLayout->addWidget(zoomOutButton_);
    scrollCorner_ = new QFrame(timelinePage_);
    scrollCorner_->setObjectName(QStringLiteral("timelineScrollCorner"));
    scrollCorner_->setFrameShape(QFrame::NoFrame);
    scrollRow_->addWidget(scrollCorner_);
    timelineLayout->addLayout(scrollRow_);
    connect(horizontalScrollBar_, &QScrollBar::valueChanged, this, [this](int value) {
        setHorizontalRatio(horizontalScrollBar_->maximum() > 0
            ? double(value) / horizontalScrollBar_->maximum() : 0.0);
    });
    laneHeaderResizeBar_ = new QFrame(timelinePage_);
    laneHeaderResizeBar_->setObjectName(QStringLiteral("timelineHeaderResizeBar"));
    laneHeaderResizeBar_->setCursor(Qt::SplitHCursor);
    laneHeaderResizeBar_->setFocusPolicy(Qt::ClickFocus);
    laneHeaderResizeBar_->setAccessibleName(tr("Resize track headers"));
    laneHeaderDivider_ = new QFrame(timelinePage_);
    laneHeaderDivider_->setObjectName(QStringLiteral("timelineHeaderDivider"));
    laneHeaderDivider_->setAttribute(Qt::WA_TransparentForMouseEvents);
    stack_->addWidget(timelinePage_);

    connect(themeManager_, &ThemeManager::themeChanged, this, [this] {
        applyThemeMetrics();
        refreshTimelinePaint();
    });
    connect(&FontManager::instance(), &FontManager::fontChanged,
        this, &TimelineEditorPanel::applyThemeMetrics);
    refreshStatusBar();

    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::itemCreated, this, [this](ProjectItem*) {
        if (suppressSelectionPush_)
            return;
        rebuild();
    });
    connect(&manager, &ProjectManager::itemMoved, this,
            [this](ProjectItem*, ProjectItem*, ProjectItem*) { rebuild(); });
    connect(&manager, &ProjectManager::itemRenamed, this,
            [this](ProjectItem*, const QString&) { refreshLaneChrome(); });
    connect(&manager, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& field, const QObject*) {
        if (item == boundItem_ && field == QLatin1String("childOrder")) rebuild();
        else refreshLaneChrome();
    }, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemDeleted, this, [this](ProjectItem* item) {
        if (item && isOrDescendsFrom(boundItem_, item)) {
            clearEditor();
            return;
        }
        rebuild();
    });
    connect(&manager, &ProjectManager::projectClosed, this, [this] {
        boundItem_ = nullptr;
        lanes_.clear();
        showState(ViewKind::Empty, {});
    });
    connect(&manager, &ProjectManager::projectOpened, this, [this](ProjectData*) {
        if (boundGuid_.isNull()) return;
        const auto state = pendingRestoreState_.isEmpty() ? snapshotState() : pendingRestoreState_;
        restoreState(state);
    });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this] { refreshLaneChrome(); });
    connect(&manager, &ProjectManager::targetPlatformsChanged,
            this, [this] { refreshLaneChrome(); });
    connect(&LanguageContextService::instance(), &LanguageContextService::languageChanged,
            this, [this](const QString&) {
                refreshLaneChrome();
            }, Qt::QueuedConnection);
    auto& assets = AssetRegistry::instance();
    connect(&assets, &AssetRegistry::assetRegistered, this, [this] { refreshLaneChrome(); });
    connect(&assets, &AssetRegistry::assetModified, this, [this] {
        canvas_->invalidateWaveforms();
        refreshLaneChrome();
    });
    connect(&assets, &AssetRegistry::assetUnregistered, this, [this] {
        canvas_->invalidateWaveforms();
        refreshLaneChrome();
    });

    applyThemeMetrics();
    FocusFilter::installRecursively(this, this);
    showState(ViewKind::Empty, {});
}

TimelineEditorPanel::~TimelineEditorPanel()
{
    finishLaneResize(false);
    if (cursorValueEditor_)
        cursorValueEditor_->reject();
    endCursorDrag();
}

bool TimelineEditorPanel::acceptsItem(const ProjectItem* item)
{
    return item && (item->type == ProjectItem::Type::Piece || item->type == ProjectItem::Type::Track)
        && ItemTypeCatalog::isUserFacingType(item->type);
}

ProjectItem* TimelineEditorPanel::timelineOwner(ProjectItem* item)
{
    if (item && item->type == ProjectItem::Type::Track
        && item->parent && item->parent->type == ProjectItem::Type::Piece)
        item = item->parent;
    return acceptsItem(item) ? item : nullptr;
}

const ThemeMetrics::Timeline& TimelineEditorPanel::timelineMetrics() const
{
    return themeManager_->metrics().timeline;
}

int TimelineEditorPanel::laneHeaderWidth() const
{
    if (viewKind_ == ViewKind::Track) {
        const QFontMetrics metrics(FontManager::instance().getFont(FontRole::Axis));
        return std::max(metrics.horizontalAdvance(QStringLiteral("-96.00")) + 8,
            themeManager_->metrics().navigationChevronExtent);
    }
    const int desired = std::max(timelineMetrics().laneHeaderMinimumWidth,
        laneHeaderWidth_ > 0 ? laneHeaderWidth_ : timelineMetrics().laneHeaderWidth);
    const int available = scrollArea_ && scrollArea_->isVisible()
        ? scrollArea_->viewport()->width() : width();
    return std::min(desired, std::max(timelineMetrics().laneHeaderMinimumWidth, available / 2));
}

int TimelineEditorPanel::laneHeaderResizeWidth() const
{
    return viewKind_ == ViewKind::Piece ? 2 : 0;
}

QRect TimelineEditorPanel::laneRect(int index) const
{
    if (viewKind_ == ViewKind::Track)
        return index == 0 ? canvas_->rect() : QRect();
    const QRect header = headerColumn_->laneRect(index);
    return QRect(0, header.top(), canvas_->width(), header.height());
}

int TimelineEditorPanel::laneHeight(int index) const
{
    const auto& metrics = themeManager_->metrics();
    const int minimum = std::max(metrics.table.rowHeight,
        headerColumn_->fontMetrics().height() + std::max(metrics.table.cellPaddingY,
            metrics.timeline.clipVerticalInset) * 2 + 1);
    return std::max(minimum, laneHeights_.value(laneGuid(index), metrics.timeline.laneHeight));
}

void TimelineEditorPanel::setLaneHeight(int index, int height)
{
    if (viewKind_ != ViewKind::Piece || index < 0 || index >= laneCount())
        return;
    const auto guid = laneGuid(index);
    if (laneHeights_.value(guid) == height)
        return;
    if (height == 0)
        laneHeights_.remove(guid);
    else
        laneHeights_.insert(guid, std::max(1, height));
    syncLaneGeometry();
    refreshTimelinePaint();
}

int TimelineEditorPanel::headerSelectedLane() const
{
    return canvas_ ? canvas_->selectedLaneIndex() : -1;
}

void TimelineEditorPanel::setLaneHeaderWidth(int width)
{
    const int clamped = std::max(timelineMetrics().laneHeaderMinimumWidth, width);
    if (laneHeaderWidth_ == clamped)
        return;
    laneHeaderWidth_ = clamped;
    applyLaneHeaderWidth();
}

void TimelineEditorPanel::resetLaneHeaderWidth()
{
    if (laneHeaderWidth_ == 0)
        return;
    laneHeaderWidth_ = 0;
    applyLaneHeaderWidth();
}

void TimelineEditorPanel::setLaneHeaderResizeHighlighted(bool highlighted)
{
    QtControlSkin::setSemanticProperty(laneHeaderResizeBar_, "highlighted", highlighted);
    QtControlSkin::setSemanticProperty(laneHeaderDivider_, "highlighted", highlighted);
}

void TimelineEditorPanel::applyLaneHeaderWidth()
{
    const int width = laneHeaderWidth();
    if (headerColumn_)
        headerColumn_->setFixedWidth(width);
    if (horizontalSpacer_)
        horizontalSpacer_->setFixedWidth(width);
    elideLaneHeaderText();
    if (ruler_) {
        ruler_->refresh();
        const int margin = (ruler_->timeRulerHeight() - addTrackButton_->height()) / 2;
        addTrackButton_->move(width - laneHeaderResizeWidth()
            - addTrackButton_->width() - 1 - margin, margin);
    }
    if (laneHeaderDivider_) {
        const QRect contents = timelinePage_->contentsRect();
        laneHeaderResizeBar_->setGeometry(contents.left() + width - laneHeaderResizeWidth() - 1,
            contents.top(), laneHeaderResizeWidth(), contents.height());
        laneHeaderResizeBar_->raise();
        laneHeaderDivider_->setGeometry(contents.left() + width - 1,
            contents.top(), 1, contents.height());
        laneHeaderDivider_->raise();
    }
    refreshHorizontalRange();
    refreshTimelinePaint();
}

void TimelineEditorPanel::elideLaneHeaderText()
{
    for (auto* name : findChildren<QLabel*>(QStringLiteral("timelineLaneName"))) {
        const int index = laneIndexFromWidget(this, name);
        if (index < 0)
            continue;
        const QString full = lanes_[index].name;
        name->setToolTip(full);
        if (name->isVisible() && name->width() > 24)
            name->setText(QFontMetrics(name->font()).elidedText(
                full, Qt::ElideRight, name->contentsRect().width()));
        else
            name->setText(full);
    }
}

void TimelineEditorPanel::refreshLaneSelectionChrome()
{
    const int selected = headerSelectedLane();
    for (auto* row : findChildren<QWidget*>(QStringLiteral("timelineLane"))) {
        const int index = laneIndexFromWidget(this, row);
        QtControlSkin::setSemanticProperty(row, "selected", index == selected);
    }
    if (headerColumn_)
        headerColumn_->update();
}

void TimelineEditorPanel::applyThemeMetrics()
{
    const auto& metrics = themeManager_->metrics();
    toolbar_->setFixedHeight(metrics.timeline.rulerHeight);
    const int extent = horizontalScrollBar_->sizeHint().height();
    const ThemeAssetVariant variant{QSize(extent, extent), devicePixelRatioF(), 0,
        themeManager_->snapshot()->ui.colors.textSecondary};
    for (auto* button : {addTrackButton_, zoomInButton_, zoomOutButton_,
                         verticalZoomInButton_, verticalZoomOutButton_}) {
        button->setFixedSize(extent, extent);
        button->setIconSize(QSize(extent, extent));
    }
    addTrackButton_->setIcon(themeManager_->resolveIcon(
        QString::fromLatin1(ThemeAssetIds::TableAdd), variant).icon);
    zoomInButton_->setIcon(addTrackButton_->icon());
    zoomOutButton_->setIcon(themeManager_->resolveIcon(
        QString::fromLatin1(ThemeAssetIds::TableRemove), variant).icon);
    verticalZoomInButton_->setIcon(zoomInButton_->icon());
    verticalZoomOutButton_->setIcon(zoomOutButton_->icon());
    verticalTools_->setFixedWidth(extent + 1);
    scrollCorner_->setFixedWidth(verticalTools_->width());
    headerColumn_->refreshRows();
    applyLaneHeaderWidth();
    syncLaneGeometry();
    refreshHorizontalRange();
    refreshTimelinePaint();
}

void TimelineEditorPanel::applyPresentation()
{
    layout()->setContentsMargins(8, 8, 8, 8);
    laneHeaderResizeBar_->setVisible(viewKind_ == ViewKind::Piece);
    verticalTools_->setVisible(viewKind_ == ViewKind::Track);
    scrollCorner_->setVisible(viewKind_ == ViewKind::Track);
    scrollArea_->setVerticalScrollBarPolicy(viewKind_ == ViewKind::Track
        ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
}

void TimelineEditorPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned)
        return;
    pinned_ = pinned;
    emit pinToggled(pinned_);
}

QString TimelineEditorPanel::stateText() const
{
    return stateLabel_->text();
}

QUuid TimelineEditorPanel::laneGuid(int index) const
{
    return index >= 0 && index < laneCount() ? lanes_[index].guid : QUuid();
}

QString TimelineEditorPanel::laneName(int index) const
{
    return index >= 0 && index < laneCount() ? lanes_[index].name : QString();
}

bool TimelineEditorPanel::laneHasExternalReferences(int index) const
{
    return index >= 0 && index < laneCount() && lanes_[index].external;
}

int TimelineEditorPanel::clipCount(int laneIndex) const
{
    const ProjectItem* track = laneItem(laneIndex);
    const auto* timeline = track ? track->comp<VTrack>() : nullptr;
    return timeline ? static_cast<int>(timeline->clips.items.size()) : 0;
}

QUuid TimelineEditorPanel::clipGuid(int laneIndex, int clipIndex) const
{
    const ProjectItem* track = laneItem(laneIndex);
    const auto* timeline = track ? track->comp<VTrack>() : nullptr;
    return timeline && clipIndex >= 0 && clipIndex < static_cast<int>(timeline->clips.items.size())
        ? timeline->clips.items[clipIndex].clipGuid : QUuid();
}

QUuid TimelineEditorPanel::clipTargetGuid(int laneIndex, int clipIndex) const
{
    const ProjectItem* track = laneItem(laneIndex);
    const auto* timeline = track ? track->comp<VTrack>() : nullptr;
    return timeline && clipIndex >= 0 && clipIndex < static_cast<int>(timeline->clips.items.size())
        ? timeline->clips.items[clipIndex].targetGuid : QUuid();
}

int64_t TimelineEditorPanel::clipStart(int laneIndex, int clipIndex) const
{
    const ProjectItem* track = laneItem(laneIndex);
    const auto* timeline = track ? track->comp<VTrack>() : nullptr;
    return timeline && clipIndex >= 0 && clipIndex < static_cast<int>(timeline->clips.items.size())
        ? timeline->clips.items[clipIndex].start : 0;
}

int64_t TimelineEditorPanel::clipDuration(int laneIndex, int clipIndex) const
{
    const ProjectItem* track = laneItem(laneIndex);
    const auto* timeline = track ? track->comp<VTrack>() : nullptr;
    return timeline && clipIndex >= 0 && clipIndex < static_cast<int>(timeline->clips.items.size())
        ? timeline->clips.items[clipIndex].duration : 0;
}

std::optional<int64_t> TimelineEditorPanel::clipSourceDuration(int laneIndex, int clipIndex) const
{
    QSet<QUuid> visiting;
    return sourceSpan(ProjectManager::instance().findItemByGuid(
        clipTargetGuid(laneIndex, clipIndex)), visiting);
}

bool TimelineEditorPanel::clipIsExternal(int laneIndex, int clipIndex) const
{
    const ProjectItem* track = laneItem(laneIndex);
    ProjectItem* target = ProjectManager::instance().findItemByGuid(
        clipTargetGuid(laneIndex, clipIndex));
    return track && target && target->parent != track;
}

QSet<QUuid> TimelineEditorPanel::selectedClipGuids() const
{
    return canvas_ ? canvas_->selectedClipGuids() : QSet<QUuid>();
}

QUuid TimelineEditorPanel::selectedClipAnchor() const
{
    return canvas_ ? canvas_->selectedClipAnchor() : QUuid();
}

int TimelineEditorPanel::findLaneOfClip(const QUuid& guid) const
{
    if (guid.isNull())
        return -1;
    for (int lane = 0; lane < laneCount(); ++lane) {
        if (findClipIndex(lane, guid) >= 0)
            return lane;
    }
    return -1;
}

int TimelineEditorPanel::findClipIndex(int laneIndex, const QUuid& guid) const
{
    if (guid.isNull())
        return -1;
    const int count = clipCount(laneIndex);
    for (int index = 0; index < count; ++index) {
        if (clipGuid(laneIndex, index) == guid)
            return index;
    }
    return -1;
}

int TimelineEditorPanel::minimumZoomPercent()
{
    return 100;
}

double TimelineEditorPanel::maximumZoomPercent() const
{
    return NanosecondPosition{viewOriginCoordinate()}.distanceTo(contentEndCoordinate())
        / kMinimumViewNanoseconds * 100.0;
}

double TimelineEditorPanel::horizontalScrollMaximum() const
{
    return std::max(0.0, (canvas_->width() - 1) * (zoomPercent_ / 100.0 - 1.0));
}

double TimelineEditorPanel::viewSpan() const
{
    if (zoomPercent_ >= maximumZoomPercent())
        return kMinimumViewNanoseconds;
    return NanosecondPosition{viewOriginCoordinate()}.distanceTo(contentEndCoordinate())
        / (zoomPercent_ / 100.0);
}

NanosecondPosition TimelineEditorPanel::positionAtPixel(double pixel) const
{
    const int width = std::max(1, canvas_->width() - 1);
    if (zoomPercent_ <= 100.0 && pixel > width / 2.0)
        return NanosecondPosition{contentEndCoordinate()}.shifted((pixel - width) * viewSpan() / width);
    return viewStart_.shifted(pixel * viewSpan() / width);
}

double TimelineEditorPanel::horizontalRatio() const
{
    const int64_t origin = viewOriginCoordinate();
    const double maximum = NanosecondPosition{origin}.distanceTo(contentEndCoordinate()) - viewSpan();
    return maximum > 0.0 ? std::clamp(-viewStart_.distanceTo(origin) / maximum, 0.0, 1.0) : 0.0;
}

void TimelineEditorPanel::setHorizontalRatio(double ratio)
{
    const NanosecondPosition origin{viewOriginCoordinate()};
    const NanosecondPosition end{contentEndCoordinate()};
    const double maximum = std::max(0.0, origin.distanceTo(end.nanoseconds) - viewSpan());
    ratio = std::clamp(ratio, 0.0, 1.0);
    setViewStart(ratio <= 0.5 ? origin.shifted(ratio * maximum)
        : end.shifted(-viewSpan()).shifted(-(1.0 - ratio) * maximum));
}

void TimelineEditorPanel::setViewStart(NanosecondPosition position)
{
    const int64_t origin = viewOriginCoordinate();
    const NanosecondPosition latest = zoomPercent_ <= 100.0
        ? NanosecondPosition{origin} : NanosecondPosition{contentEndCoordinate()}.shifted(-viewSpan());
    if (position.nanoseconds > latest.nanoseconds
        || (position.nanoseconds == latest.nanoseconds && position.fraction > latest.fraction))
        position = latest;
    if (position.nanoseconds < origin)
        position = {origin};
    viewStart_ = position;
    const QSignalBlocker blocker(horizontalScrollBar_);
    horizontalScrollBar_->setValue(int(std::lround(
        horizontalRatio() * horizontalScrollBar_->maximum())));
    refreshTimelinePaint();
}

double TimelineEditorPanel::zoomPercent() const
{
    return zoomPercent_;
}

int TimelineEditorPanel::pixelOrigin() const
{
    return 0;
}

int TimelineEditorPanel::timelineViewportWidth() const
{
    return canvas_ ? canvas_->width() : 0;
}

double TimelineEditorPanel::pixelsPerUnit() const
{
    return std::max(1, canvas_->width() - 1) * 1e9 / viewSpan();
}

double TimelineEditorPanel::horizontalScroll() const
{
    return std::round(-viewStart_.distanceTo(viewOriginCoordinate())
        / viewSpan() * std::max(1, canvas_->width() - 1));
}

TimeAxisTicks TimelineEditorPanel::timeTicks() const
{
    return buildNanosecondTimeAxisTicks(viewStart_, viewSpan(), canvas_->width(),
        QFontMetrics(FontManager::instance().getFont(FontRole::Axis), canvas_));
}

void TimelineEditorPanel::resetView()
{
    finishLaneResize(true);
    laneHeights_.clear();
    if (cursorValueEditor_) cursorValueEditor_->reject();
    endCursorDrag();
    zoomPercent_ = 100.0;
    viewStart_ = {viewOriginCoordinate()};
    pendingHorizontalRatio_.reset();
    trackVerticalZoom_ = 1.0;
    trackVerticalRatio_ = 0.0;
    selectedLaneGuid_ = QUuid();
    scrollArea_->verticalScrollBar()->setValue(0);
}

QVariantMap TimelineEditorPanel::snapshotState() const
{
    QVariantMap state{
        {QStringLiteral("itemGuid"), boundGuid_.toString(QUuid::WithoutBraces)},
        {QStringLiteral("zoom"), zoomPercent()},
        {QStringLiteral("cursor"), QString::number(editCursor_)},
        {QStringLiteral("laneHeaderWidth"), laneHeaderWidth_},
        {QStringLiteral("selectedLaneGuid"), selectedLaneGuid_.toString(QUuid::WithoutBraces)},
        {QStringLiteral("verticalScroll"), scrollArea_->verticalScrollBar()->value()},
        {QStringLiteral("timelineViewport"), QVariantMap{
            {QStringLiteral("startNanoseconds"), QString::number(viewStart_.nanoseconds)},
            {QStringLiteral("startFraction"), viewStart_.fraction},
            {QStringLiteral("verticalZoom"), trackVerticalZoom_},
            {QStringLiteral("verticalRatio"), trackVerticalRatio_}}}
    };
    QVariantList selected;
    QVariantMap heights;
    for (auto it = laneHeights_.cbegin(); it != laneHeights_.cend(); ++it)
        heights.insert(it.key().toString(QUuid::WithoutBraces), it.value());
    state.insert(QStringLiteral("laneHeights"), heights);
    for (const auto& guid : selectedClipGuids()) selected.append(guid.toString(QUuid::WithoutBraces));
    state.insert(QStringLiteral("selectedClips"), selected);
    state.insert(QStringLiteral("selectionAnchor"), selectedClipAnchor().toString(QUuid::WithoutBraces));
    QVariantList frames;
    for (const DrillFrame& frame : drillPath_) {
        frames.append(QVariantMap{
            {QStringLiteral("timelineGuid"), frame.timelineGuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("ownerTrackGuid"), frame.ownerTrackGuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("clipGuid"), frame.clipGuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("targetGuid"), frame.targetGuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("viewport"), frame.viewport}
        });
    }
    if (!frames.isEmpty()) state.insert(QStringLiteral("drillFrames"), frames);
    return state;
}

void TimelineEditorPanel::bindItem(ProjectItem* item)
{
    if (suppressSelectionPush_) return;
    const QUuid track = item && item->type == ProjectItem::Type::Track ? item->guid : QUuid();
    item = timelineOwner(item);
    if (!item) {
        clearEditor();
        return;
    }
    pendingRestoreState_.clear();
    if (boundItem_ != item || boundGuid_ != item->guid) {
        drillPath_.clear();
        selectedMarkerGuid_ = QUuid();
        canvas_->restoreSelectedClips({}, {});
        boundItem_ = item;
        boundGuid_ = item->guid;
        resetView();
        rebuild();
    }
    if (!track.isNull()) selectLane(track);
}

void TimelineEditorPanel::bindGuid(const QUuid& guid)
{
    pendingRestoreState_.clear();
    if (auto* item = ProjectManager::instance().findItemByGuid(guid)) {
        bindItem(item);
        return;
    }
    clearEditor();
    boundGuid_ = guid;
    if (!guid.isNull())
        showState(ViewKind::Empty, {});
}

void TimelineEditorPanel::clearEditor()
{
    finishLaneResize(true);
    laneHeights_.clear();
    drillPath_.clear();
    selectedMarkerGuid_ = QUuid();
    if (canvas_)
        canvas_->restoreSelectedClips({}, QUuid());
    boundGuid_ = QUuid();
    boundItem_ = nullptr;
    selectedLaneGuid_ = QUuid();
    pendingRestoreState_.clear();
    lanes_.clear();
    showState(ViewKind::Empty, {});
}

void TimelineEditorPanel::setZoomPercent(double percent)
{
    if (!std::isfinite(percent)) return;
    const double clamped = std::clamp(percent, 100.0, maximumZoomPercent());
    if (zoomPercent_ == clamped) return;
    const double ratio = horizontalRatio();
    zoomPercent_ = clamped;
    setHorizontalRatio(ratio);
    refreshHorizontalRange();
    refreshTimelinePaint();
    refreshStatusBar();
}

void TimelineEditorPanel::setHorizontalScroll(double value)
{
    const double maximum = horizontalScrollMaximum();
    setHorizontalRatio(maximum > 0.0 ? value / maximum : 0.0);
}

void TimelineEditorPanel::panHorizontally(double pixels)
{
    setViewStart(viewStart_.shifted(pixels * viewSpan()
        / std::max(1, canvas_->width() - 1)));
}

void TimelineEditorPanel::scrollTracks(QWheelEvent* event)
{
    auto* viewport = scrollArea_->viewport();
    QWheelEvent wheel(viewport->mapFromGlobal(event->globalPosition()), event->globalPosition(),
        event->pixelDelta(), event->angleDelta(), event->buttons(), event->modifiers(),
        event->phase(), event->inverted(), event->source(), event->pointingDevice());
    QCoreApplication::sendEvent(viewport, &wheel);
    event->setAccepted(wheel.isAccepted());
}

QRect TimelineEditorPanel::trackWaveformRect() const
{
    return canvas_->rect();
}

int TimelineEditorPanel::verticalScroll() const
{
    return int(std::lround(trackVerticalRatio_
        * trackWaveformRect().height() * (trackVerticalZoom_ - 1.0)));
}

void TimelineEditorPanel::setVerticalScroll(int value)
{
    verticalScrollBar_->setValue(value);
}

void TimelineEditorPanel::setAmplitudeExponent(double exponent)
{
    amplitudeExponent_ = std::clamp(exponent, 0.0, 1.0);
    refreshTimelinePaint();
}

void TimelineEditorPanel::zoomVertically(int canvasY, double factor)
{
    if (viewKind_ != ViewKind::Track || !std::isfinite(factor) || factor <= 0.0)
        return;
    const QRect wave = trackWaveformRect();
    const int anchor = std::clamp(canvasY - wave.top(), 0, wave.height());
    const double focus = (verticalScroll() + anchor) / trackVerticalZoom_;
    trackVerticalZoom_ = std::clamp(trackVerticalZoom_ * factor, 1.0, 32.0);
    refreshVerticalRange();
    setVerticalScroll(int(std::lround(focus * trackVerticalZoom_ - anchor)));
    refreshTimelinePaint();
}

void TimelineEditorPanel::refreshVerticalRange()
{
    const int viewport = trackWaveformRect().height();
    const int maximum = int(std::lround(viewport * (trackVerticalZoom_ - 1.0)));
    const QSignalBlocker blocker(verticalScrollBar_);
    verticalScrollBar_->setRange(0, maximum);
    verticalScrollBar_->setPageStep(viewport);
    verticalScrollBar_->setSingleStep(std::max(1, viewport / 10));
    verticalScrollBar_->setValue(verticalScroll());
    verticalScrollBar_->setEnabled(maximum > 0);
    verticalZoomInButton_->setEnabled(trackVerticalZoom_ < 32.0);
    verticalZoomOutButton_->setEnabled(trackVerticalZoom_ > 1.0);
}

void TimelineEditorPanel::setEditCursorVisible(bool visible)
{
    if (editCursorVisible_ == visible)
        return;
    editCursorVisible_ = visible;
    if (!visible) {
        endCursorDrag(true);
        if (cursorValueEditor_)
            cursorValueEditor_->reject();
        if (auto* menu = findChild<QMenu*>(QStringLiteral("timelineCursorContextMenu")))
            menu->close();
    }
    refreshTimelinePaint();
}

void TimelineEditorPanel::setEditCursor(int64_t coordinate, bool disableSnap)
{
    bool ok = true;
    editCursor_ = snapCoordinate(0, coordinate, disableSnap, &ok);
    if (!ok)
        editCursor_ = coordinate;
    refreshTimelinePaint();
    refreshStatusBar();
}

void TimelineEditorPanel::beginCursorDrag(int canvasX, bool disableSnap, bool locate)
{
    if (!isEditCursorVisible())
        return;
    cursorEdgeScroll_->stop();
    const int offset = locate ? 0 : canvasX - viewportPixelAtCoordinate(0, editCursor_);
    cursorDrag_ = CursorDrag{canvasX, offset, disableSnap, false, editCursor_};
    if (locate)
        applyCursorDrag();
    else
        refreshTimelinePaint();
}

void TimelineEditorPanel::updateCursorDrag(int canvasX, bool disableSnap)
{
    if (!cursorDrag_)
        return;
    cursorDrag_->moved = cursorDrag_->moved || canvasX != cursorDrag_->pointerX;
    cursorDrag_->pointerX = canvasX;
    cursorDrag_->disableSnap = disableSnap;
    if (cursorDrag_->moved)
        applyCursorDrag();
}

void TimelineEditorPanel::applyCursorDrag()
{
    const int width = std::max(1, timelineViewportWidth());
    const int intendedX = cursorDrag_->pointerX - cursorDrag_->grabOffset;
    const int x = std::clamp(intendedX, 0, width - 1);
    const int64_t coordinate = snapCoordinate(0,
        coordinateAtViewportPixel(0, x), cursorDrag_->disableSnap);
    setEditCursor(std::clamp(coordinate, viewOriginCoordinate(), contentEndCoordinate()), true);
    const bool scrollLeft = intendedX < 0 && viewStart_.distanceTo(viewOriginCoordinate()) < 0.0;
    const bool scrollRight = intendedX >= width && viewStart_.distanceTo(contentEndCoordinate()) > viewSpan();
    if (scrollLeft || scrollRight) {
        if (!cursorEdgeScroll_->isActive())
            cursorEdgeScroll_->start();
    } else {
        cursorEdgeScroll_->stop();
    }
}

void TimelineEditorPanel::scrollCursorDrag()
{
    const int width = std::max(1, timelineViewportWidth());
    const int x = cursorDrag_->pointerX - cursorDrag_->grabOffset;
    const int outside = x < 0 ? x : x >= width ? x - width + 1 : 0;
    const int maximumStep = std::max(1, width * 8 / 100);
    panHorizontally(std::clamp(outside, -maximumStep, maximumStep));
    applyCursorDrag();
}

void TimelineEditorPanel::endCursorDrag(bool cancel)
{
    if (!cursorDrag_)
        return;
    const int64_t original = cursorDrag_->originalCoordinate;
    cursorEdgeScroll_->stop();
    cursorDrag_.reset();
    emit cursorDragFinished();
    if (cancel)
        setEditCursor(original, true);
    else
        refreshTimelinePaint();
}

void TimelineEditorPanel::setPlayhead(std::optional<int64_t> coordinate)
{
    playhead_ = coordinate;
    refreshTimelinePaint();
}

void TimelineEditorPanel::editCursorValue()
{
    if (!isEditCursorVisible())
        return;
    endCursorDrag(true);
    if (cursorValueEditor_)
        cursorValueEditor_->reject();
    QToolTip::hideText();
    const int64_t initial = editCursor_;
    const QUuid owner = boundGuid_;
    auto* dialog = new QDialog(this, Qt::Popup);
    cursorValueEditor_ = dialog;
    dialog->setObjectName(QStringLiteral("timelineCursorValueEditor"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QHBoxLayout(dialog);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(8);
    layout->addWidget(new QLabel(tr("Cursor"), dialog));
    auto* spin = new DecimalSpinBox(dialog);
    spin->setObjectName(QStringLiteral("timelineCursorValueInput"));
    spin->setKeyboardTracking(false);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setDecimals(9);
    const double divisor = TimelineCoordinates::kNanosecondsPerSecond;
    spin->setRange(double(std::min(initial, viewOriginCoordinate())) / divisor,
        double(std::max(initial, contentEndCoordinate())) / divisor);
    spin->setSingleStep(0.001);
    spin->setSuffix(QStringLiteral(" s"));
    spin->setValue(double(initial) / divisor);
    const double initialValue = spin->value();
    spin->setMinimumWidth(158);
    layout->addWidget(spin);
    for (int key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto* shortcut = new QShortcut(QKeySequence(key), dialog);
        connect(shortcut, &QShortcut::activated, dialog, [dialog, spin] {
            spin->interpretText();
            dialog->accept();
        });
    }
    connect(dialog, &QDialog::accepted, this, [this, owner, initial, initialValue, spin] {
        if (!hasTimeline() || boundGuid_ != owner
            || editCursor_ != initial || spin->value() == initialValue)
            return;
        const auto coordinate = TimelineCoordinates::fromSeconds(spin->value(), {});
        if (coordinate)
            setEditCursor(std::clamp(coordinate.value, viewOriginCoordinate(), contentEndCoordinate()), true);
    });
    connect(dialog, &QDialog::finished, this, [this] { cursorValueEditor_.clear(); });
    dialog->adjustSize();
    const QRect axis = ruler_->timeAxisRect();
    const int x = axis.left() + std::clamp(viewportPixelAtCoordinate(0, initial), 0, axis.width() - 1);
    const QPoint anchor = ruler_->mapToGlobal(QPoint(x, axis.top()));
    QPoint position(anchor.x() - dialog->width() / 2, anchor.y() - dialog->height() - 6);
    if (const auto* screen = QGuiApplication::screenAt(anchor)) {
        const QRect available = screen->availableGeometry();
        position.setX(std::clamp(position.x(), available.left(),
            std::max(available.left(), available.right() - dialog->width() + 1)));
        position.setY(std::max(available.top(), position.y()));
    }
    dialog->move(position);
    dialog->show();
    spin->setFocus();
    spin->selectAll();
}

void TimelineEditorPanel::showCursorContextMenu(const QPoint& position)
{
    if (!isEditCursorVisible())
        return;
    endCursorDrag(true);
    if (cursorValueEditor_)
        cursorValueEditor_->reject();
    QToolTip::hideText();
    const QUuid owner = boundGuid_;
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("timelineCursorContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto* enter = menu->addAction(tr("Enter Value..."), this, [this, owner] {
        if (boundGuid_ == owner)
            editCursorValue();
    });
    enter->setObjectName(QStringLiteral("timelineCursorEnterValue"));
    menu->popup(position);
}

void TimelineEditorPanel::restoreViewport(const QVariantMap& state)
{
    finishLaneResize(true);
    laneHeights_.clear();
    const auto heights = state.value(QStringLiteral("laneHeights")).toMap();
    for (auto it = heights.cbegin(); it != heights.cend(); ++it) {
        const auto guid = QUuid::fromString(it.key());
        if (!guid.isNull() && it.value().toInt() > 0)
            laneHeights_.insert(guid, it.value().toInt());
    }
    syncLaneGeometry();
    laneHeaderWidth_ = std::max(0, state.value(QStringLiteral("laneHeaderWidth")).toInt());
    applyLaneHeaderWidth();
    const auto viewport = state.value(QStringLiteral("timelineViewport"),
        state.value(QStringLiteral("trackViewport"))).toMap();
    if (!viewport.isEmpty()) {
        zoomPercent_ = std::max(100.0, state.value(QStringLiteral("zoom"), 100).toDouble());
        if (viewKind_ == ViewKind::Track) {
            trackVerticalZoom_ = std::clamp(viewport.value(QStringLiteral("verticalZoom"), 1.0).toDouble(), 1.0, 32.0);
            trackVerticalRatio_ = std::clamp(viewport.value(QStringLiteral("verticalRatio")).toDouble(), 0.0, 1.0);
            refreshVerticalRange();
        }
        if (viewport.contains(QStringLiteral("startNanoseconds"))) {
            viewStart_ = NanosecondPosition{viewport.value(QStringLiteral("startNanoseconds")).toLongLong()}
                .shifted(std::clamp(viewport.value(QStringLiteral("startFraction")).toDouble(), 0.0, 1.0));
        } else {
            pendingHorizontalRatio_ = std::clamp(viewport.value(QStringLiteral("horizontalRatio")).toDouble(), 0.0, 1.0);
        }
    } else if (const auto* item = ProjectManager::instance().findItemByGuid(
                   QUuid::fromString(state.value(QStringLiteral("itemGuid")).toString()));
               item && item->type == ProjectItem::Type::Track) {
        zoomPercent_ = 100.0;
        viewStart_ = {viewOriginCoordinate()};
    } else {
        const double scale = std::clamp(state.value(QStringLiteral("zoom"), 100).toDouble(), 4.0, 800.0);
        const double span = std::max(1, timelineViewportWidth() - 1) / scale * 1e9;
        zoomPercent_ = std::max(100.0,
            NanosecondPosition{viewOriginCoordinate()}.distanceTo(contentEndCoordinate()) / span * 100.0);
        viewStart_ = NanosecondPosition{viewOriginCoordinate()}.shifted(
            (state.value(QStringLiteral("horizontalScroll")).toDouble() - kTimelinePixelOrigin) / scale * 1e9);
    }
    refreshHorizontalRange();
    setEditCursor(state.value(QStringLiteral("cursor")).toLongLong(), true);
    QSet<QUuid> clips;
    for (const auto& value : state.value(QStringLiteral("selectedClips")).toList())
        clips.insert(QUuid::fromString(value.toString()));
    canvas_->restoreSelectedClips(clips,
        QUuid::fromString(state.value(QStringLiteral("selectionAnchor")).toString()));
    selectedLaneGuid_ = laneGuid(headerSelectedLane());
    if (canvas_->selectedClipGuids().isEmpty())
        selectLane(QUuid::fromString(state.value(QStringLiteral("selectedLaneGuid")).toString()));
    scrollArea_->verticalScrollBar()->setValue(state.value(QStringLiteral("verticalScroll")).toInt());
    refreshLaneSelectionChrome();
    refreshTimelinePaint();
}

void TimelineEditorPanel::restoreState(const QVariantMap& state)
{
    const QUuid guid = QUuid::fromString(state.value(QStringLiteral("itemGuid")).toString());
    bindGuid(guid);
    if (!boundItem_) {
        if (!guid.isNull() && !ProjectManager::instance().findItemByGuid(guid)) pendingRestoreState_ = state;
        return;
    }
    restoreViewport(state);
    drillPath_.clear();
    const QVariantList frames = state.value(QStringLiteral("drillFrames")).toList();
    for (const QVariant& value : frames) {
        const QVariantMap stored = value.toMap();
        DrillFrame frame{
            QUuid::fromString(stored.value(QStringLiteral("timelineGuid")).toString()),
            QUuid::fromString(stored.value(QStringLiteral("ownerTrackGuid")).toString()),
            QUuid::fromString(stored.value(QStringLiteral("clipGuid")).toString()),
            QUuid::fromString(stored.value(QStringLiteral("targetGuid")).toString()),
            stored.value(QStringLiteral("viewport")).toMap(),
        };
        const auto* timelineItem = ProjectManager::instance().findItemByGuid(frame.timelineGuid);
        const auto* track = ProjectManager::instance().findItemByGuid(frame.ownerTrackGuid);
        const auto* timeline = track ? track->comp<VTrack>() : nullptr;
        const bool valid = acceptsItem(timelineItem) && timeline
            && (timelineItem == track || track->parent == timelineItem)
            && std::any_of(timeline->clips.items.begin(), timeline->clips.items.end(),
                           [&frame](const Clip& clip) {
                               return clip.clipGuid == frame.clipGuid
                                   && clip.targetGuid == frame.targetGuid;
                           })
            && (drillPath_.empty() || drillPath_.back().targetGuid == frame.timelineGuid);
        if (!valid) {
            drillPath_.clear();
            break;
        }
        drillPath_.push_back(std::move(frame));
    }
    if (!drillPath_.empty() && drillPath_.back().targetGuid != boundGuid_)
        drillPath_.clear();
    refreshBreadcrumbs();
}

PieceTimeSpec TimelineEditorPanel::effectiveTimeSpec(int laneIndex) const
{
    ProjectItem* track = laneItem(laneIndex);
    return TimelineEditing::timeSpecForTrackParent(track ? track->parent : boundItem_);
}

QString TimelineEditorPanel::formatCoordinate(int laneIndex, int64_t coordinate) const
{
    if (viewKind_ == ViewKind::Unsupported || !AuthoringPolicy::validatePieceTime(effectiveTimeSpec(laneIndex)))
        return tr("Unsupported arrangement");
    return QStringLiteral("%1 s").arg(formatTimeNanoseconds(coordinate, 0,
        axisReadoutDecimalsForStep(timeTicks().majorStep)));
}

int64_t TimelineEditorPanel::coordinateAtPixel(int, double pixel) const
{
    if (viewKind_ == ViewKind::Unsupported) return 0;
    return NanosecondPosition{viewOriginCoordinate()}.shifted(
        pixel * viewSpan() / std::max(1, canvas_->width() - 1)).rounded();
}

double TimelineEditorPanel::pixelAtCoordinate(int, int64_t coordinate) const
{
    if (viewKind_ == ViewKind::Unsupported) return 0.0;
    return NanosecondPosition{viewOriginCoordinate()}.distanceTo(coordinate)
        / viewSpan() * std::max(1, canvas_->width() - 1);
}

int64_t TimelineEditorPanel::coordinateAtViewportPixel(int, double pixel) const
{
    return viewKind_ == ViewKind::Unsupported ? 0 : positionAtPixel(pixel).rounded();
}

double TimelineEditorPanel::viewportPositionAtCoordinate(int, int64_t coordinate) const
{
    return viewStart_.distanceTo(coordinate) / viewSpan() * std::max(1, canvas_->width() - 1);
}

int TimelineEditorPanel::viewportPixelAtCoordinate(int laneIndex, int64_t coordinate) const
{
    return int(std::clamp(std::round(viewportPositionAtCoordinate(laneIndex, coordinate)),
        -double(QWIDGETSIZE_MAX), double(QWIDGETSIZE_MAX)));
}

std::optional<int64_t> TimelineEditorPanel::emergenceCoordinate(
    int laneIndex, int clipIndex) const
{
    ProjectItem* target = ProjectManager::instance().findItemByGuid(
        clipTargetGuid(laneIndex, clipIndex));
    const auto* sound = target ? target->comp<VSound>() : nullptr;
    if (!sound || sound->scheduleOffsetSec <= 0.0) return std::nullopt;
    const PieceTimeSpec spec = effectiveTimeSpec(laneIndex);
    const CoordinateResult offset = TimelineCoordinates::fromSeconds(
        sound->scheduleOffsetSec, spec);
    if (!offset) return std::nullopt;
    const int64_t start = clipStart(laneIndex, clipIndex);
    if (offset.value > 0 && start > std::numeric_limits<int64_t>::max() - offset.value)
        return std::numeric_limits<int64_t>::max();
    return start + offset.value;
}

TimelineDomain TimelineEditorPanel::timelineDomain() const
{
    return effectiveTimeSpec(0).domain;
}

int64_t TimelineEditorPanel::tempoMicrosecondsPerQuarter() const
{
    const auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    return timeline && !timeline->pieceTime.tempoMap.empty()
        ? timeline->pieceTime.tempoMap.front().microsecondsPerQuarter : 0;
}

int TimelineEditorPanel::meterNumerator() const
{
    const auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    return timeline && !timeline->pieceTime.meterMap.empty()
        ? timeline->pieceTime.meterMap.front().numerator : 0;
}

int TimelineEditorPanel::meterDenominator() const
{
    const auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    return timeline && !timeline->pieceTime.meterMap.empty()
        ? timeline->pieceTime.meterMap.front().denominator : 0;
}

int TimelineEditorPanel::markerCount() const
{
    const auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    return timeline ? static_cast<int>(timeline->markers.items.size()) : 0;
}

QUuid TimelineEditorPanel::markerGuid(int index) const
{
    const auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    return timeline && index >= 0 && index < static_cast<int>(timeline->markers.items.size())
        ? timeline->markers.items[index].markerGuid : QUuid{};
}

MarkerKind TimelineEditorPanel::markerKind(int index) const
{
    const auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    return timeline && index >= 0 && index < static_cast<int>(timeline->markers.items.size())
        ? timeline->markers.items[index].kind : MarkerKind::Custom;
}

QString TimelineEditorPanel::markerName(int index) const
{
    const auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    return timeline && index >= 0 && index < static_cast<int>(timeline->markers.items.size())
        ? timeline->markers.items[index].name : QString{};
}

int64_t TimelineEditorPanel::markerPosition(int index) const
{
    const auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    return timeline && index >= 0 && index < static_cast<int>(timeline->markers.items.size())
        ? timeline->markers.items[index].position : 0;
}

int64_t TimelineEditorPanel::markerDisplayPosition(int index) const
{
    return markerPosition(index);
}

void TimelineEditorPanel::setSelectedMarker(const QUuid& guid)
{
    if (selectedMarkerGuid_ == guid)
        return;
    selectedMarkerGuid_ = guid;
    refreshTimelinePaint();
}

bool TimelineEditorPanel::hasMarkerKind(MarkerKind kind) const
{
    const auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    if (!timeline)
        return false;
    return std::any_of(timeline->markers.items.begin(), timeline->markers.items.end(),
                       [kind](const Marker& marker) { return marker.kind == kind; });
}

TimelineSemanticsResult TimelineEditorPanel::timelineSemantics() const
{
    if (ProjectManager::instance().isClosing())
        return TimelineSemanticsResult::unknown(
            TimelineError::MissingTimeline, QStringLiteral("The project is closing"));
    if (viewKind_ == ViewKind::Unsupported)
        return TimelineSemanticsResult::unknown(
            TimelineError::AuthoringRestricted, AuthoringPolicy::musicalTimeRestriction());
    auto resolver = [](const QUuid& guid) -> const ProjectItem* {
        return ProjectManager::instance().findItemByGuid(guid);
    };
    const ProjectItem* bound = resolver(boundGuid_);
    if (!bound)
        return TimelineSemanticsResult::unknown(
            TimelineError::MissingTimeline, QStringLiteral("Timeline item is unavailable"));
    const auto displayTrack = [](const ProjectItem& track) {
        auto display = std::make_shared<ProjectItem>(track.name, ProjectItem::Type::Track);
        display->guid = track.guid;
        display->compOrCreate<VTrack>()->clips = track.comp<VTrack>()->clips;
        return display;
    };
    if (bound->type == ProjectItem::Type::Piece) {
        ProjectItem display(bound->name, ProjectItem::Type::Piece);
        display.guid = bound->guid;
        auto* timeline = display.compOrCreate<VPiece>();
        const auto* original = bound->comp<VPiece>();
        timeline->pieceTime = original->pieceTime;
        timeline->markers = original->markers;
        for (const auto& child : bound->children) {
            if (!child || child->type != ProjectItem::Type::Track)
                continue;
            auto track = displayTrack(*child);
            track->parent = &display;
            display.children.push_back(std::move(track));
        }
        return TimelineSemantics::pieceNominalSpan(display, resolver);
    }
    if (bound->type == ProjectItem::Type::Track) {
        PieceTimeSpec time;
        if (const auto* piece = bound->parent ? bound->parent->comp<VPiece>() : nullptr)
            time = piece->pieceTime;
        return TimelineSemantics::trackNominalSpan(*displayTrack(*bound), resolver, time);
    }
    return TimelineSemanticsResult::unknown(
        TimelineError::InvalidTargetType,
        QStringLiteral("Nominal span is defined only for Track and Piece"));
}

QString TimelineEditorPanel::nominalSummary() const
{
    return TimelineSemantics::formatNominalSummary(timelineSemantics());
}

std::vector<TimelineSpan> TimelineEditorPanel::overlapRegions(int laneIndex) const
{
    ClipLedger clips;
    for (int index = 0; index < clipCount(laneIndex); ++index) {
        Clip clip{clipGuid(laneIndex, index), clipTargetGuid(laneIndex, index),
                  clipStart(laneIndex, index), clipDuration(laneIndex, index)};
        if (!clips.mintOccurrenceOrdinal(&clip)) return {};
        clips.items.push_back(std::move(clip));
    }
    return TimelineSemantics::overlapRegions(clips);
}

int64_t TimelineEditorPanel::snapCoordinate(int laneIndex, int64_t coordinate,
    bool disableSnap, bool* ok, const QSet<QUuid>& excludedClips) const
{
    if (viewKind_ == ViewKind::Unsupported
        || !AuthoringPolicy::validatePieceTime(effectiveTimeSpec(laneIndex))) {
        if (ok) *ok = false;
        return coordinate;
    }
    if (disableSnap) {
        if (ok) *ok = true;
        return coordinate;
    }
    const auto grid = TimelineCoordinates::fromSeconds(timeTicks().minorStep, {});
    const auto snapped = grid ? TimelineCoordinates::snap(coordinate, grid.value) : grid;
    if (ok) *ok = snapped.ok();
    if (!snapped) return coordinate;
    int64_t best = snapped.value;
    const auto distance = [coordinate](int64_t candidate) {
        return candidate > coordinate ? uint64_t(candidate) - uint64_t(coordinate)
            : uint64_t(coordinate) - uint64_t(candidate);
    };
    const auto consider = [&](int64_t candidate) {
        const double pixels = std::abs(NanosecondPosition{coordinate}.distanceTo(candidate))
            / viewSpan() * std::max(1, timelineViewportWidth() - 1);
        if (pixels <= 6.0 && (distance(candidate) < distance(best)
            || (distance(candidate) == distance(best) && candidate < best))) best = candidate;
    };
    const int top = scrollArea_->verticalScrollBar()->value();
    const int bottom = top + scrollArea_->viewport()->height();
    for (int lane = 0; lane < laneCount(); ++lane) {
        const auto row = laneRect(lane);
        if (row.bottom() < top || row.top() > bottom) continue;
        for (int clip = 0; clip < clipCount(lane); ++clip) {
            if (excludedClips.contains(clipGuid(lane, clip))) continue;
            const int64_t start = clipStart(lane, clip);
            const int64_t duration = clipDuration(lane, clip);
            consider(start);
            if (start <= std::numeric_limits<int64_t>::max() - duration) consider(start + duration);
        }
    }
    return best;
}

int64_t TimelineEditorPanel::authoredDuration(ProjectItem* target, int) const
{
    QSet<QUuid> visiting;
    return sourceSpan(target, visiting).value_or(1000000000LL);
}

int64_t TimelineEditorPanel::viewOriginCoordinate() const
{
    if (viewKind_ == ViewKind::Unsupported)
        return 0;
    int64_t earliest = 0;
    for (int lane = 0; lane < laneCount(); ++lane)
        for (int clip = 0; clip < clipCount(lane); ++clip)
            earliest = std::min(earliest, clipStart(lane, clip));
    if (earliest >= 0)
        return 0;
    ProjectItem* track = laneItem(0);
    const PieceTimeSpec spec = TimelineEditing::timeSpecForTrackParent(
        track ? track->parent : boundItem_);
    const int64_t remainder = earliest % coordinateUnit(spec);
    const int64_t offset = remainder < 0 ? coordinateUnit(spec) + remainder : 0;
    return earliest < std::numeric_limits<int64_t>::min() + offset
        ? std::numeric_limits<int64_t>::min() : earliest - offset;
}

bool TimelineEditorPanel::validateClipTarget(ProjectItem* target, int laneIndex,
                                             QString* error) const
{
    ProjectItem* track = laneItem(laneIndex);
    if (!track) {
        if (error) *error = tr("Drop onto the object's owning Track lane.");
        return false;
    }
    if (!target || ProjectManager::instance().findItemByGuid(target->guid) != target) {
        if (error) *error = tr("The dropped target no longer exists.");
        return false;
    }
    ClipLedger clips = track->comp<VTrack>()->clips;
    Clip clip{QUuid::createUuid(), target->guid, 0, 1};
    if (!clips.mintOccurrenceOrdinal(&clip)) {
        if (error) *error = tr("The Track occurrence ordinal space is exhausted.");
        return false;
    }
    const auto resolve = [](const QUuid& guid) {
        return ProjectManager::instance().findItemByGuid(guid);
    };
    const TimelineValidation authoring = AuthoringPolicy::validateNewSubtree(
        *target, nullptr, resolve);
    if (!authoring) {
        if (error) *error = authoring.message;
        return false;
    }
    if (boundItem_) {
        const TimelineValidation destination = AuthoringPolicy::validateTimeline(*boundItem_, resolve);
        if (!destination) {
            if (error) *error = destination.message;
            return false;
        }
    }
    clips.items.push_back(std::move(clip));
    const TimelineValidation placement = TimelineEditing::validateClipPlacement(
        &ProjectManager::instance(), track, clips);
    if (!placement && error) *error = placement.message;
    return placement.ok();
}

bool TimelineEditorPanel::commitTimelineAction(ActionResult result)
{
    if (!result.success) {
        rejectInteraction(result.errorMessage);
        return false;
    }
    lastInteractionError_.clear();
    if (result.undoFn) {
        UndoRedoManager::instance().push(new RuntimeActionCommand(
            result.undoText, std::move(result.undoFn), std::move(result.redoFn)));
    }
    setStatusMessage({}, false);
    refreshStatusBar();
    return true;
}

bool TimelineEditorPanel::commitTimelineActions(std::vector<ActionResult> results,
                                                const QString& undoText)
{
    if (results.empty())
        return false;
    for (const ActionResult& result : results) {
        if (!result.success) {
            for (auto it = results.rbegin(); it != results.rend(); ++it) {
                if (it->success && it->undoFn)
                    it->undoFn();
            }
            rejectInteraction(result.errorMessage);
            return false;
        }
    }
    std::vector<std::function<void()>> undos;
    std::vector<std::function<void()>> redos;
    undos.reserve(results.size());
    redos.reserve(results.size());
    for (ActionResult& result : results) {
        if (!result.undoFn)
            continue;
        undos.push_back(std::move(result.undoFn));
        redos.push_back(std::move(result.redoFn));
    }
    if (!undos.empty()) {
        UndoRedoManager::instance().push(new RuntimeActionCommand(
            undoText,
            [undos]() {
                for (auto it = undos.rbegin(); it != undos.rend(); ++it)
                    (*it)();
            },
            [redos]() {
                for (const auto& redo : redos)
                    redo();
            }));
    }
    lastInteractionError_.clear();
    setStatusMessage({}, false);
    refreshStatusBar();
    return true;
}

void TimelineEditorPanel::rejectInteraction(const QString& error)
{
    lastInteractionError_ = error.isEmpty() ? tr("Timeline edit was rejected.") : error;
    setStatusMessage(lastInteractionError_, true);
}

bool TimelineEditorPanel::placeItem(ProjectItem* target, int laneIndex, int64_t start,
                                    bool disableSnap)
{
    QString error;
    ProjectItem* track = laneItem(laneIndex);
    if (!validateClipTarget(target, laneIndex, &error)) {
        rejectInteraction(error);
        return false;
    }
    bool snapped = false;
    start = snapCoordinate(laneIndex, start, disableSnap, &snapped);
    if (!snapped) {
        rejectInteraction(tr("The Clip position cannot be represented in this timeline."));
        return false;
    }
    Clip clip{QUuid::createUuid(), target->guid, start, authoredDuration(target, laneIndex)};
    return commitTimelineAction(TimelineEditing::addClip(
        &ProjectManager::instance(), track, clip, this));
}

bool TimelineEditorPanel::moveClip(int sourceLaneIndex, const QUuid& guid,
                                   int destinationLaneIndex, int64_t start,
                                   bool disableSnap)
{
    ProjectItem* source = laneItem(sourceLaneIndex);
    ProjectItem* destination = laneItem(destinationLaneIndex);
    if (!source || !destination || guid.isNull()) {
        rejectInteraction(tr("The Clip or destination Track is unavailable."));
        return false;
    }
    bool snapped = false;
    start = snapCoordinate(destinationLaneIndex, start, disableSnap, &snapped, {guid});
    if (!snapped) {
        rejectInteraction(tr("The Clip position cannot be represented in the destination Track."));
        return false;
    }
    ActionResult result = source == destination
        ? TimelineEditing::moveClip(&ProjectManager::instance(), source, guid, start, this)
        : TimelineEditing::moveClipAcrossTracks(&ProjectManager::instance(), source,
                                                destination, guid, start, this);
    return commitTimelineAction(std::move(result));
}

bool TimelineEditorPanel::resizeClip(int laneIndex, const QUuid& guid,
                                     int64_t start, int64_t duration,
                                     bool disableSnap)
{
    ProjectItem* track = laneItem(laneIndex);
    if (!track || guid.isNull()) {
        rejectInteraction(tr("The Clip is unavailable."));
        return false;
    }
    bool snapped = false;
    start = snapCoordinate(laneIndex, start, disableSnap, &snapped, {guid});
    if (!snapped) {
        rejectInteraction(tr("The Clip position cannot be represented in this Track."));
        return false;
    }
    return commitTimelineAction(TimelineEditing::resizeClip(
        &ProjectManager::instance(), track, guid, start, duration, this));
}

bool TimelineEditorPanel::deleteClip(int laneIndex, const QUuid& guid)
{
    ProjectItem* track = laneItem(laneIndex);
    if (!track || guid.isNull()) {
        rejectInteraction(tr("The Clip is unavailable."));
        return false;
    }
    return commitTimelineAction(TimelineEditing::removeClip(
        &ProjectManager::instance(), track, guid, this));
}

bool TimelineEditorPanel::setTimelineDomain(TimelineDomain domain)
{
    auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    if (!timeline) {
        rejectInteraction(tr("Clock editing requires a Piece."));
        return false;
    }
    PieceTimeSpec spec = domain == TimelineDomain::Musical
        ? (timeline->pieceTime.domain == TimelineDomain::Musical
               ? timeline->pieceTime : PieceTimeSpec::musical())
        : PieceTimeSpec{};
    return commitTimelineAction(TimelineEditing::replacePieceTime(
        &ProjectManager::instance(), boundItem_, spec, this, tr("Change Piece clock")));
}

bool TimelineEditorPanel::setTempoMicrosecondsPerQuarter(int64_t microsecondsPerQuarter)
{
    auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    if (!timeline) {
        rejectInteraction(tr("Time settings require a Piece."));
        return false;
    }
    PieceTimeSpec spec = timeline->pieceTime;
    spec.tempoMap = {{0, microsecondsPerQuarter}};
    return commitTimelineAction(TimelineEditing::replacePieceTime(
        &ProjectManager::instance(), boundItem_, spec, this, tr("Change Piece tempo")));
}

bool TimelineEditorPanel::setMeter(int numerator, int denominator)
{
    auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    if (!timeline) {
        rejectInteraction(tr("Time settings require a Piece."));
        return false;
    }
    PieceTimeSpec spec = timeline->pieceTime;
    spec.meterMap = {{0, numerator, denominator}};
    return commitTimelineAction(TimelineEditing::replacePieceTime(
        &ProjectManager::instance(), boundItem_, spec, this, tr("Change Piece meter")));
}

bool TimelineEditorPanel::addMarker(MarkerKind kind, int64_t position, const QString& name)
{
    if (!boundItem_ || boundItem_->type != ProjectItem::Type::Piece) {
        rejectInteraction(tr("Marker creation requires a Piece."));
        return false;
    }
    Marker marker{QUuid::createUuid(), kind, kind == MarkerKind::Custom ? name : QString{}, position};
    const QUuid guid = marker.markerGuid;
    if (!commitTimelineAction(TimelineEditing::addMarker(
            &ProjectManager::instance(), boundItem_, std::move(marker), this)))
        return false;
    setSelectedMarker(guid);
    return true;
}

bool TimelineEditorPanel::moveMarker(const QUuid& guid, int64_t position, bool disableSnap)
{
    auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    if (!timeline || guid.isNull()) {
        rejectInteraction(tr("The Marker is unavailable."));
        return false;
    }
    const auto it = std::find_if(timeline->markers.items.begin(), timeline->markers.items.end(),
                                 [&guid](const Marker& marker) { return marker.markerGuid == guid; });
    if (it == timeline->markers.items.end()) {
        rejectInteraction(tr("The Marker is unavailable."));
        return false;
    }
    bool snapped = false;
    position = snapCoordinate(-1, position, disableSnap, &snapped);
    if (!snapped) {
        rejectInteraction(tr("The Marker position cannot be represented in this Piece."));
        return false;
    }
    Marker marker = *it;
    marker.position = position;
    return commitTimelineAction(TimelineEditing::updateMarker(
        &ProjectManager::instance(), boundItem_, marker, this));
}

bool TimelineEditorPanel::renameMarker(const QUuid& guid, const QString& name)
{
    auto* timeline = boundItem_ ? boundItem_->comp<VPiece>() : nullptr;
    if (!timeline || guid.isNull()) {
        rejectInteraction(tr("The Marker is unavailable."));
        return false;
    }
    const auto it = std::find_if(timeline->markers.items.begin(), timeline->markers.items.end(),
                                 [&guid](const Marker& marker) { return marker.markerGuid == guid; });
    if (it == timeline->markers.items.end() || it->kind != MarkerKind::Custom) {
        rejectInteraction(tr("Only Custom Marker names can be edited."));
        return false;
    }
    Marker marker = *it;
    marker.name = name;
    return commitTimelineAction(TimelineEditing::updateMarker(
        &ProjectManager::instance(), boundItem_, marker, this));
}

bool TimelineEditorPanel::deleteMarker(const QUuid& guid)
{
    if (!boundItem_ || boundItem_->type != ProjectItem::Type::Piece || guid.isNull()) {
        rejectInteraction(tr("The Marker is unavailable."));
        return false;
    }
    return commitTimelineAction(TimelineEditing::removeMarker(
        &ProjectManager::instance(), boundItem_, guid, this));
}

bool TimelineEditorPanel::drillIntoClip(int laneIndex, const QUuid& guid)
{
    ProjectItem* ownerTrack = laneItem(laneIndex);
    const int clipIndex = findClipIndex(laneIndex, guid);
    ProjectItem* target = clipIndex >= 0
        ? ProjectManager::instance().findItemByGuid(clipTargetGuid(laneIndex, clipIndex))
        : nullptr;
    if (!ownerTrack || !target || target->type != ProjectItem::Type::Piece) {
        rejectInteraction(tr("Only Piece Clips can be opened in Timeline."));
        return false;
    }
    auto viewport = snapshotState();
    viewport.remove(QStringLiteral("drillFrames"));
    drillPath_.push_back({boundGuid_, ownerTrack->guid, guid, target->guid, viewport});
    bindDrillItem(target);
    return true;
}

bool TimelineEditorPanel::drillBack()
{
    return drillToDepth(static_cast<int>(drillPath_.size()) - 1);
}

bool TimelineEditorPanel::drillToDepth(int depth)
{
    if (depth < 0 || depth > static_cast<int>(drillPath_.size()))
        return false;
    if (depth == static_cast<int>(drillPath_.size()))
        return true;
    const auto viewport = drillPath_[static_cast<size_t>(depth)].viewport;
    const QUuid targetGuid = drillPath_[static_cast<size_t>(depth)].timelineGuid;
    drillPath_.resize(static_cast<size_t>(depth));
    ProjectItem* target = ProjectManager::instance().findItemByGuid(targetGuid);
    if (!target) {
        drillPath_.clear();
        rejectInteraction(tr("The previous Timeline item no longer exists."));
        return false;
    }
    bindDrillItem(target);
    if (!viewport.isEmpty()) restoreViewport(viewport);
    return true;
}

PlacementContext TimelineEditorPanel::drillPlacementContext() const
{
    PlacementContext context;
    for (const DrillFrame& frame : drillPath_) {
        context = context.withNearestOccurrence({
            frame.ownerTrackGuid, frame.clipGuid, frame.targetGuid});
    }
    return context;
}

PlacementContext TimelineEditorPanel::placementContextForClip(
    int laneIndex, const QUuid& guid) const
{
    const ProjectItem* track = laneItem(laneIndex);
    const auto* timeline = track ? track->comp<VTrack>() : nullptr;
    if (!timeline || guid.isNull())
        return {};
    const auto it = std::find_if(
        timeline->clips.items.begin(), timeline->clips.items.end(),
        [&guid](const Clip& clip) { return clip.clipGuid == guid; });
    if (it == timeline->clips.items.end())
        return {};
    return drillPlacementContext().withNearestOccurrence(
        {track->guid, it->clipGuid, it->targetGuid});
}

PlacementContext TimelineEditorPanel::placementContextForLane(int laneIndex) const
{
    return laneItem(laneIndex) ? drillPlacementContext() : PlacementContext{};
}

bool TimelineEditorPanel::requestBlueprintForClip(int laneIndex, const QUuid& guid)
{
    const PlacementContext context = placementContextForClip(laneIndex, guid);
    if (context.empty()) {
        rejectInteraction(tr("The Clip is unavailable."));
        return false;
    }
    ProjectItem* subject = ProjectManager::instance().findItemByGuid(
        context.occurrences().front().targetGuid);
    if (!subject) {
        rejectInteraction(tr("The Clip target is unavailable."));
        return false;
    }
    lastInteractionError_.clear();
    QScopedValueRollback<bool> selectionGuard(suppressSelectionPush_, true);
    emit blueprintRequested(subject, context);
    return true;
}

bool TimelineEditorPanel::requestBlueprintForLane(int laneIndex)
{
    ProjectItem* track = laneItem(laneIndex);
    if (!track) {
        rejectInteraction(tr("The Track is unavailable."));
        return false;
    }
    lastInteractionError_.clear();
    QScopedValueRollback<bool> selectionGuard(suppressSelectionPush_, true);
    emit blueprintRequested(track, placementContextForLane(laneIndex));
    return true;
}

bool TimelineEditorPanel::inspectClip(int laneIndex, const QUuid& guid)
{
    const PlacementContext context = placementContextForClip(laneIndex, guid);
    if (context.empty()) {
        rejectInteraction(tr("The Clip is unavailable."));
        return false;
    }
    ProjectItem* subject = ProjectManager::instance().findItemByGuid(
        context.occurrences().front().targetGuid);
    return inspectItem(subject, context);
}

bool TimelineEditorPanel::selectLane(const QUuid& guid, bool publish)
{
    if (guid.isNull()) {
        selectedLaneGuid_ = QUuid();
        canvas_->setSelectedClip(-1, {});
        refreshLaneSelectionChrome();
        return true;
    }
    for (int lane = 0; lane < laneCount(); ++lane) {
        if (laneGuid(lane) != guid) continue;
        selectedLaneGuid_ = guid;
        if (headerSelectedLane() != lane || !selectedClipGuids().isEmpty())
            canvas_->setSelectedClip(lane, {});
        const auto row = laneRect(lane);
        scrollArea_->ensureVisible(0, row.center().y(), 0, row.height() / 2);
        refreshLaneSelectionChrome();
        if (publish) {
            QScopedValueRollback<bool> guard(suppressSelectionPush_, true);
            emit laneSelectionRequested(laneItem(lane), placementContextForLane(lane));
        }
        return true;
    }
    return false;
}

bool TimelineEditorPanel::reorderLane(int from, int to)
{
    if (viewKind_ != ViewKind::Piece || from < 0 || to < 0
        || from >= laneCount() || to >= laneCount() || from == to) return false;
    auto order = boundItem_->children;
    const auto moving = std::find_if(order.begin(), order.end(), [this, from](const auto& child) {
        return child->guid == laneGuid(from);
    });
    const auto destination = std::find_if(order.begin(), order.end(), [this, to](const auto& child) {
        return child->guid == laneGuid(to);
    });
    const auto target = std::distance(order.begin(), destination);
    const auto track = *moving;
    order.erase(moving);
    order.insert(order.begin() + target, track);
    QJsonArray guids;
    for (const auto& child : order) guids.append(child->guid.toString());
    const auto result = ActionManager::instance().execute(QStringLiteral("reorder_container_children"),
        {{QStringLiteral("container_path"), boundGuid_.toString()},
         {QStringLiteral("child_order"), guids}}, ActionSource::Internal, this);
    if (!result.success) {
        rejectInteraction(result.errorMessage);
        return false;
    }
    selectedLaneGuid_ = track->guid;
    rebuild();
    selectLane(track->guid, true);
    return true;
}

bool TimelineEditorPanel::revealClip(const QUuid& guid)
{
    const int lane = findLaneOfClip(guid);
    const int clip = findClipIndex(lane, guid);
    if (lane < 0 || clip < 0 || !canvas_) return false;
    setSelectedMarker({});
    canvas_->setSelectedClip(lane, guid);
    panHorizontally(viewportPositionAtCoordinate(lane, clipStart(lane, clip))
                    - timelineViewportWidth() * 0.25);
    if (viewKind_ == ViewKind::Piece && scrollArea_) {
        const auto row = laneRect(lane);
        scrollArea_->ensureVisible(0, row.center().y(), 0, row.height() / 2);
    }
    inspectClip(lane, guid);
    canvas_->setFocus(Qt::OtherFocusReason);
    return true;
}

bool TimelineEditorPanel::revealMarker(const QUuid& guid)
{
    for (int index = 0; index < markerCount(); ++index) {
        if (markerGuid(index) != guid) continue;
        if (canvas_) canvas_->setSelectedClip(-1, {});
        setSelectedMarker(guid);
        panHorizontally(viewportPositionAtCoordinate(0, markerDisplayPosition(index))
                        - timelineViewportWidth() * 0.25);
        if (canvas_) canvas_->setFocus(Qt::OtherFocusReason);
        return true;
    }
    return false;
}

bool TimelineEditorPanel::inspectItem(ProjectItem* item, const PlacementContext& context)
{
    if (!item) {
        rejectInteraction(tr("The object is unavailable."));
        return false;
    }
    lastInteractionError_.clear();
    QScopedValueRollback<bool> selectionGuard(suppressSelectionPush_, true);
    emit clipSelectionRequested(item, context);
    return true;
}

bool TimelineEditorPanel::deleteSelectedClips()
{
    if (!canvas_)
        return false;
    const QSet<QUuid> guids = canvas_->selectedClipGuids();
    if (guids.isEmpty())
        return false;
    std::vector<ActionResult> results;
    results.reserve(static_cast<size_t>(guids.size()));
    for (const QUuid& guid : guids) {
        const int lane = findLaneOfClip(guid);
        ProjectItem* track = laneItem(lane);
        if (!track) {
            rejectInteraction(tr("The Clip is unavailable."));
            return false;
        }
        results.push_back(TimelineEditing::removeClip(
            &ProjectManager::instance(), track, guid, this));
    }
    if (!commitTimelineActions(std::move(results), tr("Delete Clips")))
        return false;
    canvas_->restoreSelectedClips({}, QUuid());
    return true;
}

bool TimelineEditorPanel::importAudioFiles(const QStringList& files, int laneIndex,
                                           int64_t start, bool disableSnap)
{
    ProjectItem* track = laneItem(laneIndex);
    if (!track) {
        rejectInteraction(tr("The destination Track is unavailable."));
        return false;
    }
    const ImportPlacement placement = AudioImportGate::resolveImportParent(track);
    if (placement.kind != ImportPlacementKind::Parent || !placement.parent) {
        rejectInteraction(tr("Audio cannot be imported into this Track."));
        return false;
    }
    ProjectData* project = ProjectManager::instance().currentProject();
    ProjectFileBinding* binding = project
        ? findFileBindingForItem(*project, placement.parent) : nullptr;
    if (!binding) {
        rejectInteraction(tr("Could not resolve an import target for the dropped files."));
        return false;
    }
    auto* command = ImportAssetsCommand::pushImport(files, binding, placement.parent);
    if (!command || command->successCount() <= 0) {
        rejectInteraction(tr("Audio import failed."));
        return false;
    }
    bool placed = false;
    for (const QUuid& guid : command->importedItemGuids()) {
        ProjectItem* sound = ProjectManager::instance().findItemByGuid(guid);
        if (!sound)
            continue;
        const auto& clips = track->comp<VTrack>()->clips.items;
        const auto clip = std::find_if(clips.begin(), clips.end(),
            [sound](const Clip& entry) { return entry.targetGuid == sound->guid; });
        if (clip == clips.end()
            || !moveClip(laneIndex, clip->clipGuid, laneIndex, start, disableSnap))
            return false;
        placed = true;
    }
    if (!placed) {
        rejectInteraction(tr("Imported audio could not be placed as a Clip."));
        return false;
    }
    setStatusMessage(tr("Imported into %1").arg(track->name), false);
    return true;
}

void TimelineEditorPanel::zoomAroundPixel(int canvasX, double factor)
{
    canvasX = std::clamp(canvasX, 0, std::max(0, canvas_->width() - 1));
    if (!std::isfinite(factor) || factor <= 0.0) return;
    const double anchor = double(canvasX) / std::max(1, canvas_->width() - 1);
    const auto focus = positionAtPixel(canvasX);
    const double previous = zoomPercent_;
    setZoomPercent(zoomPercent_ * factor);
    if (previous != zoomPercent_)
        setViewStart(focus.shifted(-anchor * viewSpan()));
}

void TimelineEditorPanel::setStatusMessage(const QString& text, bool error)
{
    statusMessage_ = error ? QString() : text;
    refreshStatusBar();
}

void TimelineEditorPanel::refreshStatusBar()
{
    const bool active = hasTimeline();
    addTrackButton_->setVisible(viewKind_ == ViewKind::Piece);
    zoomOutButton_->setEnabled(active && zoomPercent() > 100.0);
    zoomInButton_->setEnabled(active && zoomPercent() < maximumZoomPercent());
}

void TimelineEditorPanel::fitClips()
{
    setZoomPercent(100.0);
    setViewStart({viewOriginCoordinate()});
}

void TimelineEditorPanel::publishClipSelection()
{
    selectedLaneGuid_ = laneGuid(headerSelectedLane());
    refreshLaneSelectionChrome();
    refreshStatusBar();
    if (!canvas_)
        return;
    const QSet<QUuid> selected = canvas_->selectedClipGuids();
    if (selected.size() != 1)
        return;
    const QUuid guid = *selected.constBegin();
    const int lane = findLaneOfClip(guid);
    inspectClip(lane, guid);
}

void TimelineEditorPanel::bindDrillItem(ProjectItem* item)
{
    boundGuid_ = item ? item->guid : QUuid();
    boundItem_ = item;
    resetView();
    rebuild();
}

void TimelineEditorPanel::refreshBreadcrumbs()
{
    if (!breadcrumbLayout_)
        return;
    toolbar_->setVisible(!drillPath_.empty());
    while (QLayoutItem* item = breadcrumbLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    titleLabel_ = nullptr;

    struct Crumb {
        QString name;
        int depth = 0;
        bool current = false;
    };
    std::vector<Crumb> crumbs;
    auto& manager = ProjectManager::instance();
    QUuid rootGuid;
    if (!drillPath_.empty())
        rootGuid = drillPath_.front().timelineGuid;
    else if (boundItem_)
        rootGuid = boundItem_->guid;
    if (const ProjectItem* root = manager.findItemByGuid(rootGuid))
        crumbs.push_back({root->name, 0, drillPath_.empty()});
    else if (boundItem_ && drillPath_.empty())
        crumbs.push_back({boundItem_->name, 0, true});
    for (size_t i = 0; i < drillPath_.size(); ++i) {
        const ProjectItem* item = manager.findItemByGuid(drillPath_[i].targetGuid);
        crumbs.push_back({item ? item->name : tr("Missing"),
                          static_cast<int>(i + 1),
                          i + 1 == drillPath_.size()});
    }
    if (crumbs.empty())
        crumbs.push_back({boundItem_ ? boundItem_->name : tr("Timeline"), 0, true});

    const bool collapse = crumbs.size() > 4
        || (crumbs.size() > 2 && breadcrumbBar_->width() < 320);
    auto addSeparator = [this] {
        auto* separator = new QLabel(QStringLiteral("›"), breadcrumbBar_);
        separator->setObjectName(QStringLiteral("timelineBreadcrumbSeparator"));
        QtControlSkin::setSemanticProperty(separator, "textRole",
                                           QStringLiteral("muted"));
        breadcrumbLayout_->addWidget(separator);
    };
    auto addCrumb = [this](const Crumb& crumb) {
        if (crumb.current) {
            auto* label = new QLabel(crumb.name, breadcrumbBar_);
            label->setObjectName(QStringLiteral("timelineBreadcrumbCurrent"));
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            label->setToolTip(crumb.name);
            QtControlSkin::setSemanticProperty(label, "textRole",
                                               QStringLiteral("primary"));
            breadcrumbLayout_->addWidget(label, 1);
            titleLabel_ = label;
            return;
        }
        auto* button = new QToolButton(breadcrumbBar_);
        button->setObjectName(QStringLiteral("timelineBreadcrumb"));
        button->setText(crumb.name);
        button->setToolTip(crumb.name);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setProperty("drillDepth", crumb.depth);
        QtControlSkin::setSemanticProperty(button, "textRole",
                                           QStringLiteral("accent"));
        connect(button, &QToolButton::clicked, this, [this, depth = crumb.depth] {
            drillToDepth(depth);
        });
        breadcrumbLayout_->addWidget(button, 1);
    };

    QStringList fullPath;
    for (const Crumb& crumb : crumbs)
        fullPath << crumb.name;
    if (collapse) {
        addCrumb(crumbs.front());
        addSeparator();
        auto* ellipsis = new QLabel(QStringLiteral("…"), breadcrumbBar_);
        ellipsis->setObjectName(QStringLiteral("timelineBreadcrumbEllipsis"));
        ellipsis->setToolTip(fullPath.join(QStringLiteral(" › ")));
        QtControlSkin::setSemanticProperty(ellipsis, "textRole",
                                           QStringLiteral("muted"));
        breadcrumbLayout_->addWidget(ellipsis);
        addSeparator();
        addCrumb(crumbs.back());
    } else {
        for (size_t i = 0; i < crumbs.size(); ++i) {
            if (i > 0)
                addSeparator();
            addCrumb(crumbs[i]);
        }
    }
    if (!titleLabel_) {
        titleLabel_ = new QLabel(tr("Timeline"), breadcrumbBar_);
        titleLabel_->setObjectName(QStringLiteral("timelineTitle"));
        breadcrumbLayout_->addWidget(titleLabel_);
    }
    breadcrumbBar_->setToolTip(fullPath.join(QStringLiteral(" › ")));
}

bool TimelineEditorPanel::eventFilter(QObject* watched, QEvent* event)
{
    auto* laneResizeBar = qobject_cast<QFrame*>(watched);
    if (laneResizeBar && laneResizeBar->objectName() != QLatin1String("timelineLaneResizeBar"))
        laneResizeBar = nullptr;
    if (laneResizeBar) {
        const int lane = laneResizeBar->property("laneIndex").toInt();
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                laneResizeBar->setFocus(Qt::MouseFocusReason);
                laneResizeDrag_ = LaneResizeDrag{lane, mouse->globalPosition().toPoint().y(),
                    laneRect(lane).height(), laneHeights_.value(laneGuid(lane))};
                laneResizeBar->grabMouse();
                QtControlSkin::setSemanticProperty(laneResizeBar, "highlighted", true);
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const bool released = event->type() == QEvent::MouseButtonRelease
                && mouse->button() == Qt::LeftButton;
            if (laneResizeDrag_ && (released || mouse->buttons().testFlag(Qt::LeftButton))) {
                setLaneHeight(laneResizeDrag_->lane, std::max(1, laneResizeDrag_->height
                    + mouse->globalPosition().toPoint().y() - laneResizeDrag_->pointerY));
                if (released) {
                    finishLaneResize(false);
                    QtControlSkin::setSemanticProperty(laneResizeBar, "highlighted",
                        laneResizeBar->rect().contains(laneResizeBar->mapFromGlobal(
                            mouse->globalPosition().toPoint())));
                }
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonDblClick:
            if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
                finishLaneResize(false);
                setLaneHeight(lane, 0);
                QtControlSkin::setSemanticProperty(laneResizeBar, "highlighted",
                    laneResizeBar->rect().contains(laneResizeBar->mapFromGlobal(
                        static_cast<QMouseEvent*>(event)->globalPosition().toPoint())));
                event->accept();
                return true;
            }
            break;
        case QEvent::KeyPress:
            if (laneResizeDrag_ && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
                finishLaneResize(true);
                return true;
            }
            break;
        case QEvent::Enter:
            canvas_->clearHover();
            QtControlSkin::setSemanticProperty(laneResizeBar, "highlighted", true);
            break;
        case QEvent::Leave:
            QtControlSkin::setSemanticProperty(laneResizeBar, "highlighted",
                laneResizeDrag_ && laneResizeDrag_->lane == lane);
            break;
        case QEvent::Hide:
            finishLaneResize(true);
            QtControlSkin::setSemanticProperty(laneResizeBar, "highlighted", false);
            break;
        case QEvent::Wheel:
            scrollTracks(static_cast<QWheelEvent*>(event));
            return true;
        default:
            break;
        }
    }
    if ((watched == laneHost_ || watched == headerColumn_
            || watched->objectName() == QLatin1String("timelineLane"))
        && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        updateLaneResizeBars();
        canvas_->update();
    }
    if (watched == laneHeaderResizeBar_) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                laneHeaderResizeBar_->setFocus(Qt::MouseFocusReason);
                headerResizeDrag_ = HeaderResizeDrag{mouse->globalPosition().toPoint().x(), laneHeaderWidth()};
                setLaneHeaderResizeHighlighted(true);
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const bool released = event->type() == QEvent::MouseButtonRelease
                && mouse->button() == Qt::LeftButton;
            if (headerResizeDrag_ && (released || mouse->buttons().testFlag(Qt::LeftButton))) {
                setLaneHeaderWidth(headerResizeDrag_->width
                    + mouse->globalPosition().toPoint().x() - headerResizeDrag_->pointerX);
                if (released) {
                    headerResizeDrag_.reset();
                    setLaneHeaderResizeHighlighted(laneHeaderResizeBar_->rect().contains(
                        laneHeaderResizeBar_->mapFromGlobal(mouse->globalPosition().toPoint())));
                }
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonDblClick: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                headerResizeDrag_.reset();
                resetLaneHeaderWidth();
                setLaneHeaderResizeHighlighted(laneHeaderResizeBar_->rect().contains(
                    laneHeaderResizeBar_->mapFromGlobal(mouse->globalPosition().toPoint())));
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::Enter:
            canvas_->clearHover();
            setLaneHeaderResizeHighlighted(true);
            break;
        case QEvent::Leave:
            setLaneHeaderResizeHighlighted(headerResizeDrag_.has_value());
            break;
        case QEvent::Hide:
            headerResizeDrag_.reset();
            setLaneHeaderResizeHighlighted(false);
            break;
        default:
            break;
        }
    }
    if (watched == ruler_ && event->type() == QEvent::Hide && cursorValueEditor_)
        cursorValueEditor_->reject();
    if (watched == canvas_ && event->type() == QEvent::Resize)
        refreshVerticalRange();
    const bool navigationControl = watched == ruler_ || watched == horizontalScrollBar_
        || watched == zoomInButton_ || watched == zoomOutButton_
        || (viewKind_ == ViewKind::Track && (watched == headerColumn_
            || watched == verticalScrollBar_ || watched == verticalTools_
            || watched == verticalZoomInButton_ || watched == verticalZoomOutButton_));
    if (hasTimeline() && navigationControl && event->type() == QEvent::Wheel) {
        const auto* source = static_cast<QWheelEvent*>(event);
        QWheelEvent wheel(canvas_->mapFromGlobal(source->globalPosition()), source->globalPosition(),
            source->pixelDelta(), source->angleDelta(), source->buttons(), source->modifiers(),
            source->phase(), source->inverted(), source->source());
        QCoreApplication::sendEvent(canvas_, &wheel);
        event->setAccepted(wheel.isAccepted());
        return wheel.isAccepted();
    }
    if (hasTimeline() && navigationControl && event->type() == QEvent::Enter)
        canvas_->clearHover();
    if ((watched == timelinePage_ || (scrollArea_ && watched == scrollArea_->viewport()))
        && event->type() == QEvent::Resize)
        applyLaneHeaderWidth();
    if (event->type() == QEvent::Resize || event->type() == QEvent::FontChange) {
        if (auto* label = qobject_cast<QLabel*>(watched);
            label && (label == titleLabel_ || label->objectName() == QLatin1String("timelineLaneName"))) {
            label->setText(label->fontMetrics().elidedText(
                label->toolTip(), Qt::ElideRight, label->contentsRect().width()));
        } else if (auto* button = qobject_cast<QToolButton*>(watched);
                   button && button->objectName() == QLatin1String("timelineBreadcrumb")) {
            button->setText(button->fontMetrics().elidedText(
                button->toolTip(), Qt::ElideRight,
                std::max(0, button->width() - themeManager_->metrics().spacingSmall * 2)));
        }
    }
    if (FocusFilter::dispatch(this, watched, event) && event->type() == QEvent::FocusIn)
        emit panelFocused(panelId_);
    return QWidget::eventFilter(watched, event);
}

void TimelineEditorPanel::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

void TimelineEditorPanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    applyLaneHeaderWidth();
    refreshHorizontalRange();
    refreshTimelinePaint();
    refreshStatusBar();
}

void TimelineEditorPanel::rebuild()
{
    finishLaneResize(true);
    if (!boundItem_) {
        lanes_.clear();
        showState(ViewKind::Empty, {});
        return;
    }
    if (timelineOwner(boundItem_) != boundItem_) {
        const auto viewport = snapshotState();
        bindItem(boundItem_);
        restoreViewport(viewport);
        return;
    }
    lanes_.clear();

    const TimelineValidation authoring = AuthoringPolicy::validateTimeline(
        *boundItem_, [](const QUuid& guid) {
            return ProjectManager::instance().findItemByGuid(guid);
        });
    if (!authoring) {
        showState(ViewKind::Unsupported,
                  tr("Unsupported arrangement. This timeline is read-only. %1")
                      .arg(authoring.message));
        return;
    }

    if (boundItem_->type == ProjectItem::Type::Track) {
        lanes_.push_back({boundItem_->guid, boundItem_->objectID, boundItem_->name,
                          hasExternalReferences(boundItem_)});
        viewKind_ = ViewKind::Track;
        rebuildTimelinePage();
        return;
    }

    if (boundItem_->type == ProjectItem::Type::Piece) {
        for (const auto& child : boundItem_->children) {
            if (child && child->type == ProjectItem::Type::Track)
                lanes_.push_back({child->guid, child->objectID, child->name,
                                  hasExternalReferences(child.get())});
        }
        viewKind_ = ViewKind::Piece;
        rebuildTimelinePage();
        return;
    }

    showState(ViewKind::Unsupported,
              tr("%1 cannot be edited in Timeline. Select a Piece or Track.")
                  .arg(boundItem_->name));
}

void TimelineEditorPanel::rebuildTimelinePage()
{
    if (!hasTimeline()) return;
    applyPresentation();
    refreshBreadcrumbs();
    const auto selectedClips = canvas_->selectedClipGuids();
    const auto selectedAnchor = canvas_->selectedClipAnchor();
    qDeleteAll(laneResizeBars_);
    laneResizeBars_.clear();
    headerColumn_->rebuildRows();
    if (viewKind_ == ViewKind::Piece) {
        for (int lane = 0; lane < laneCount(); ++lane) {
            auto* bar = new QFrame(laneHost_);
            bar->setObjectName(QStringLiteral("timelineLaneResizeBar"));
            bar->setProperty("laneIndex", lane);
            bar->setCursor(Qt::SplitVCursor);
            bar->setFocusPolicy(Qt::ClickFocus);
            bar->setAccessibleName(tr("Resize track height"));
            bar->installEventFilter(this);
            laneResizeBars_.push_back(bar);
        }
    }
    canvas_->restoreSelectedClips(selectedClips, selectedAnchor);
    if (selectedClips.isEmpty()) selectLane(selectedLaneGuid_);
    applyThemeMetrics();
    refreshLaneSelectionChrome();
    stack_->setCurrentWidget(timelinePage_);
}

void TimelineEditorPanel::refreshTimelinePaint()
{
    if (ruler_)
        ruler_->refresh();
    if (canvas_)
        canvas_->update();
    if (headerColumn_)
        headerColumn_->update();
    refreshStatusBar();
}

void TimelineEditorPanel::refreshLaneChrome()
{
    if (ProjectManager::instance().isClosing())
        return;
    if (boundItem_) {
        const bool supported = bool(AuthoringPolicy::validateTimeline(
            *boundItem_, [](const QUuid& guid) {
                return ProjectManager::instance().findItemByGuid(guid);
            }));
        if (supported == (viewKind_ == ViewKind::Unsupported)) {
            rebuild();
            return;
        }
    }
    for (int index = 0; index < laneCount(); ++index) {
        if (index < static_cast<int>(lanes_.size()) && laneItem(index)) {
            ProjectItem* track = laneItem(index);
            lanes_[index].name = track->name;
            lanes_[index].external = hasExternalReferences(track);
        }
    }
    if (hasTimeline())
        refreshBreadcrumbs();
    if (canvas_ && !canvas_->selectedClipGuids().isEmpty()) {
        canvas_->restoreSelectedClips(canvas_->selectedClipGuids(),
                                      canvas_->selectedClipAnchor());
        selectedLaneGuid_ = laneGuid(headerSelectedLane());
    }
    elideLaneHeaderText();
    refreshLaneSelectionChrome();
    refreshHorizontalRange();
    refreshTimelinePaint();
    refreshStatusBar();
}

void TimelineEditorPanel::syncLaneGeometry()
{
    headerColumn_->refreshRows();
    if (viewKind_ == ViewKind::Track) {
        canvas_->setMinimumHeight(0);
        laneHost_->setMinimumHeight(0);
        refreshVerticalRange();
        return;
    }
    const int contentHeight = std::max(timelineMetrics().laneHeight,
        laneLayout_->totalSizeHint().height());
    canvas_->setMinimumHeight(contentHeight);
    laneHost_->setMinimumHeight(contentHeight);
    updateLaneResizeBars();
}

void TimelineEditorPanel::updateLaneResizeBars()
{
    for (int lane = 0; lane < static_cast<int>(laneResizeBars_.size()); ++lane) {
        auto* bar = laneResizeBars_[lane];
        const auto area = laneRect(lane);
        bar->setGeometry(0, area.bottom() - 3, headerColumn_->width() - 1, 4);
        bar->setVisible(!area.isEmpty());
        bar->raise();
    }
}

void TimelineEditorPanel::finishLaneResize(bool cancel)
{
    if (!laneResizeDrag_) return;
    const auto drag = *laneResizeDrag_;
    laneResizeDrag_.reset();
    auto* bar = laneResizeBars_[drag.lane];
    bar->releaseMouse();
    if (cancel) setLaneHeight(drag.lane, drag.storedHeight);
    QtControlSkin::setSemanticProperty(bar, "highlighted", bar->underMouse());
}

int64_t TimelineEditorPanel::contentEndCoordinate() const
{
    int64_t end = 10 * TimelineCoordinates::kNanosecondsPerSecond;
    for (int lane = 0; lane < laneCount(); ++lane) {
        for (int clip = 0; clip < clipCount(lane); ++clip) {
            const int64_t start = clipStart(lane, clip);
            const int64_t duration = clipDuration(lane, clip);
            if (start > std::numeric_limits<int64_t>::max() - duration)
                return std::numeric_limits<int64_t>::max();
            end = std::max(end, start + duration);
        }
    }
    return end;
}

void TimelineEditorPanel::refreshHorizontalRange()
{
    if (!horizontalScrollBar_ || !canvas_)
        return;
    zoomPercent_ = std::clamp(zoomPercent_, 100.0, maximumZoomPercent());
    const int page = std::clamp(int(std::lround(kScrollSteps * 100.0 / zoomPercent_)),
        1, kScrollSteps);
    const QSignalBlocker blocker(horizontalScrollBar_);
    horizontalScrollBar_->setRange(0, kScrollSteps - page);
    horizontalScrollBar_->setPageStep(page);
    horizontalScrollBar_->setSingleStep(std::max(1, page / 10));
    if (pendingHorizontalRatio_) {
        const double ratio = *pendingHorizontalRatio_;
        pendingHorizontalRatio_.reset();
        setHorizontalRatio(ratio);
    } else {
        setViewStart(viewStart_);
    }
    horizontalScrollBar_->show();
    horizontalSpacer_->show();
    horizontalScrollBar_->setEnabled(page < kScrollSteps);
    refreshStatusBar();
}

void TimelineEditorPanel::showState(ViewKind kind, const QString& text)
{
    finishLaneResize(true);
    qDeleteAll(laneResizeBars_);
    laneResizeBars_.clear();
    endCursorDrag();
    if (laneLayout_)
        clearLayout(laneLayout_);
    viewKind_ = kind;
    applyPresentation();
    refreshBreadcrumbs();
    if (titleLabel_ && kind == ViewKind::Unsupported)
        titleLabel_->setText(tr("Timeline unavailable"));
    else if (titleLabel_ && !boundItem_)
        titleLabel_->clear();
    stateLabel_->setText(text);
    stack_->setCurrentWidget(stateLabel_);
    refreshStatusBar();
}

ProjectItem* TimelineEditorPanel::laneItem(int index) const
{
    return index >= 0 && index < laneCount()
        ? ProjectManager::instance().findItemByGuid(lanes_[index].guid) : nullptr;
}

bool TimelineEditorPanel::hasExternalReferences(const ProjectItem* track) const
{
    const auto* timeline = track ? track->comp<VTrack>() : nullptr;
    if (!timeline)
        return false;
    auto& manager = ProjectManager::instance();
    return std::any_of(timeline->clips.items.begin(), timeline->clips.items.end(),
                       [track, &manager](const Clip& clip) {
        ProjectItem* target = manager.findItemByGuid(clip.targetGuid);
        return target && target->parent != track;
    });
}

}
