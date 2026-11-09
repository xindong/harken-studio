#include "TransitionFadeEditor.h"
#include "TransitionFadeTimeline.h"

#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeLayout.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/DecimalSpinBox.h"
#include "Widgets/Common/NumericToolTip.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace VNX {

namespace {

constexpr double kMaximumSeconds = 600.0;
constexpr double kScrollUnitsPerSecond = 1000.0;

}

TransitionFadeEditor::TransitionFadeEditor(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("transitionFadeEditor"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    ThemeLayout::spacing(layout, ThemeLayout::Space::Section);

    auto* viewport = new QFrame(this);
    viewport->setObjectName(QStringLiteral("transitionFadeViewport"));
    viewport->setFrameShape(QFrame::NoFrame);
    auto* viewportLayout = new QVBoxLayout(viewport);
    viewportLayout->setContentsMargins(0, 0, 0, 0);
    viewportLayout->setSpacing(0);
    timeline_ = new TransitionFadeTimeline(viewport);
    viewportLayout->addWidget(timeline_, 1);

    auto* scrollControls = new QFrame(viewport);
    scrollControls->setObjectName(QStringLiteral("transitionFadeScrollControls"));
    scrollControls->setFrameShape(QFrame::NoFrame);
    auto* scrollLayout = new QHBoxLayout(scrollControls);
    scrollLayout->setContentsMargins(0, 0, 0, 0);
    scrollLayout->setSpacing(0);
    const auto zoomButton = [this, scrollControls](const QString& name, const QString& text, double factor) {
        auto* button = new QToolButton(scrollControls);
        button->setObjectName(name);
        button->setToolTip(text);
        button->setAccessibleName(text);
        button->setAutoRaise(true);
        button->setProperty("flatIconButton", true);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QToolButton::clicked, this, [this, factor] { timeline_->zoomBy(factor); });
        return button;
    };
    zoomIn_ = zoomButton(QStringLiteral("transitionFadeZoomIn"), tr("Zoom in time"), 2.0);
    zoomOut_ = zoomButton(QStringLiteral("transitionFadeZoomOut"), tr("Zoom out time"), 0.5);
    scroll_ = new QScrollBar(Qt::Horizontal, scrollControls);
    scroll_->setObjectName(QStringLiteral("transitionFadeHorizontalScroll"));
    QtControlSkin::setSemanticProperty(scroll_, "scrollRole", QStringLiteral("canvas"));
    scrollLayout->addWidget(zoomIn_);
    scrollLayout->addWidget(scroll_, 1);
    scrollLayout->addWidget(zoomOut_);
    viewportLayout->addWidget(scrollControls);
    layout->addWidget(viewport, 1);

    auto* fields = new QWidget(this);
    auto* grid = new QGridLayout(fields);
    grid->setContentsMargins(0, 0, 0, 0);
    ThemeLayout::spacing(grid, ThemeLayout::Space::Control);
    outStart_ = createSpin(QStringLiteral("transitionFadeOutStart"), -kMaximumSeconds);
    outDuration_ = createSpin(QStringLiteral("transitionFadeOutDuration"), 0.0);
    inStart_ = createSpin(QStringLiteral("transitionFadeInStart"), -kMaximumSeconds);
    inDuration_ = createSpin(QStringLiteral("transitionFadeInDuration"), 0.0);
    const struct { QDoubleSpinBox* spin; QString label; int row; int column; } rows[] = {
        {outStart_, tr("Fade Out Start"), 0, 0},
        {outDuration_, tr("Fade Out Duration"), 1, 0},
        {inStart_, tr("Fade In Start"), 0, 2},
        {inDuration_, tr("Fade In Duration"), 1, 2}};
    for (const auto& row : rows) {
        auto* label = new QLabel(row.label, fields);
        label->setToolTip(row.label);
        QtControlSkin::setSemanticProperty(label, "textRole", QStringLiteral("formLabel"));
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        row.spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        grid->addWidget(label, row.row, row.column);
        grid->addWidget(row.spin, row.row, row.column + 1);
    }
    for (int column = 0; column < 4; ++column)
        grid->setColumnStretch(column, 1);
    outStart_->setToolTip(tr("Fade-out start relative to the transition point"));
    inStart_->setToolTip(tr("Fade-in start relative to the transition point"));
    layout->addWidget(fields);

    connect(timeline_, &TransitionFadeTimeline::fadesPreviewed, this, &TransitionFadeEditor::refreshFields);
    connect(timeline_, &TransitionFadeTimeline::fadesCommitted, this, &TransitionFadeEditor::commit);
    connect(timeline_, &TransitionFadeTimeline::viewChanged, this, &TransitionFadeEditor::refreshScroll);
    connect(scroll_, &QScrollBar::valueChanged, this, [this](int value) {
        timeline_->setViewStart(value / kScrollUnitsPerSecond);
    });
    const auto commitFields = [this] {
        const TransitionRuleDef* rule = TransitionRuleEditing::find(container(), endpoints_);
        if (!rule)
            return;
        TransitionFadeDef fadeOut = rule->sourceFadeOut;
        TransitionFadeDef fadeIn = rule->targetFadeIn;
        fadeOut.offsetSeconds = float(outStart_->value());
        fadeOut.durationSeconds = float(outDuration_->value());
        fadeIn.offsetSeconds = float(inStart_->value());
        fadeIn.durationSeconds = float(inDuration_->value());
        commit(fadeOut, fadeIn);
    };
    for (QDoubleSpinBox* spin : {outStart_, outDuration_, inStart_, inDuration_})
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, commitFields);

    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, this, &TransitionFadeEditor::refreshTheme);
    refreshTheme();

    auto& manager = ProjectManager::instance();
    connect(&manager, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property, const QObject*) {
        if (!hasRule())
            return;
        if ((item && item->guid == containerGuid_ && property == QStringLiteral("transitions"))
            || property == QStringLiteral("segmentTime")
            || property == QStringLiteral("markers")
            || property == QStringLiteral("clips")
            || property == QStringLiteral("sources")
            || property == QStringLiteral("loopCount"))
            refresh();
    }, Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemRenamed, this, [this] { if (hasRule()) refresh(); },
            Qt::QueuedConnection);
    connect(&manager, &ProjectManager::itemDeleted, this, [this](ProjectItem* item) {
        if (item && item->guid == containerGuid_)
            clear();
    });
}

QDoubleSpinBox* TransitionFadeEditor::createSpin(const QString& name, double minimum)
{
    auto* spin = new DecimalSpinBox(this);
    spin->setObjectName(name);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setDecimals(3);
    spin->setSingleStep(0.01);
    spin->setRange(minimum, kMaximumSeconds);
    spin->setKeyboardTracking(false);
    new NumericToolTip(spin, QStringLiteral("s"));
    return spin;
}

void TransitionFadeEditor::bindRule(const QUuid& containerGuid, const TransitionEndpoints& endpoints)
{
    const bool changed = containerGuid_ != containerGuid || endpoints_ != endpoints;
    containerGuid_ = containerGuid;
    endpoints_ = endpoints;
    refresh();
    if (changed)
        timeline_->fitView();
}

void TransitionFadeEditor::clear()
{
    containerGuid_ = QUuid();
    endpoints_ = {};
    refresh();
}

bool TransitionFadeEditor::hasRule() const
{
    return TransitionRuleEditing::find(container(), endpoints_) != nullptr;
}

ProjectItem* TransitionFadeEditor::container() const
{
    return containerGuid_.isNull() ? nullptr : ProjectManager::instance().findItemByGuid(containerGuid_);
}

void TransitionFadeEditor::refresh()
{
    const ProjectItem* owner = container();
    const TransitionRuleDef* rule = TransitionRuleEditing::find(owner, endpoints_);
    setEnabled(rule != nullptr);
    const TransitionPreview preview = rule ? buildTransitionPreview(*owner, *rule) : TransitionPreview{};
    timeline_->setPreview(preview);
    refreshFields(preview.fadeOut, preview.fadeIn);
}

void TransitionFadeEditor::refreshFields(const TransitionFadeDef& fadeOut, const TransitionFadeDef& fadeIn)
{
    const std::pair<QDoubleSpinBox*, float> values[] = {
        {outStart_, fadeOut.offsetSeconds}, {outDuration_, fadeOut.durationSeconds},
        {inStart_, fadeIn.offsetSeconds}, {inDuration_, fadeIn.durationSeconds}};
    for (const auto& [spin, value] : values) {
        if (spin->hasFocus())
            continue;
        const QSignalBlocker blocker(spin);
        spin->setValue(value);
    }
}

void TransitionFadeEditor::refreshScroll()
{
    const QSignalBlocker blocker(scroll_);
    const double span = timeline_->viewSpan();
    scroll_->setRange(int(std::floor(timeline_->contentStart() * kScrollUnitsPerSecond)),
                      int(std::ceil((timeline_->contentEnd() - span) * kScrollUnitsPerSecond)));
    scroll_->setPageStep(int(std::lround(span * kScrollUnitsPerSecond)));
    scroll_->setSingleStep(std::max(1, scroll_->pageStep() / 10));
    scroll_->setValue(int(std::lround(timeline_->viewStart() * kScrollUnitsPerSecond)));
}

void TransitionFadeEditor::refreshTheme()
{
    auto& theme = ThemeManager::instance();
    const int extent = scroll_->sizeHint().height();
    const ThemeAssetVariant variant{QSize(extent, extent), devicePixelRatioF(), 0, theme.colors().textPrimary};
    zoomIn_->setIcon(theme.resolveIcon(QString::fromLatin1(ThemeAssetIds::TableAdd), variant).icon);
    zoomOut_->setIcon(theme.resolveIcon(QString::fromLatin1(ThemeAssetIds::TableRemove), variant).icon);
    for (QToolButton* button : {zoomIn_, zoomOut_}) {
        button->setFixedSize(extent, extent);
        button->setIconSize(QSize(extent, extent));
    }
}

void TransitionFadeEditor::commit(const TransitionFadeDef& fadeOut, const TransitionFadeDef& fadeIn)
{
    TransitionRuleEditing::update(container(), endpoints_, [&](TransitionRuleDef& rule) {
        rule.sourceFadeOut = fadeOut;
        rule.targetFadeIn = fadeIn;
    }, tr("Set Transition Fade"), this);
    refresh();
}

}
