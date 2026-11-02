
#include "BlueprintPanel.h"
#include "Core/ContextInfo.h"

#include "BlueprintCanvasWidget.h"
#include "BlueprintVisibilityMenu.h"
#include "Core/FontManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "Widgets/Meter/MeterPanel.h"

#include <QAction>
#include <QButtonGroup>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {

BlueprintPanel::BlueprintPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("BlueprintPanel"));
    canvas_ = new BlueprintCanvasWidget(this);

    auto* toolbar = new QFrame(this);
    toolbar_ = toolbar;
    toolbar->setObjectName(QStringLiteral("BlueprintToolBar"));
    toolbar->setFont(FontManager::instance().getFont(FontRole::Caption, 9));

    auto* modes = new QButtonGroup(toolbar);
    modes->setExclusive(true);
    selectButton_ = new QToolButton(toolbar);
    panButton_ = new QToolButton(toolbar);
    modes->addButton(selectButton_, 0);
    modes->addButton(panButton_, 1);
    selectButton_->setObjectName(QStringLiteral("BlueprintSelectButton"));
    panButton_->setObjectName(QStringLiteral("BlueprintPanButton"));
    selectButton_->setText(tr("Select"));
    panButton_->setText(tr("Pan"));
    for (auto* button : {selectButton_, panButton_}) {
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(20, 20));
        button->setFixedSize(22, 22);
        button->setProperty("flatIconButton", true);
        button->setAutoRaise(true);
        button->setCheckable(true);
        button->setToolTip(button->text());
    }
    const auto syncMode = [this](BlueprintCanvasView::InteractionMode mode) {
        selectButton_->setChecked(mode == BlueprintCanvasView::InteractionMode::Select);
        panButton_->setChecked(mode == BlueprintCanvasView::InteractionMode::Pan);
    };
    syncMode(canvas_->interactionMode());
    connect(modes, &QButtonGroup::idClicked, canvas_, [this](int index) {
        canvas_->setInteractionMode(index == 0 ? BlueprintCanvasView::InteractionMode::Select
                                             : BlueprintCanvasView::InteractionMode::Pan);
    });
    connect(canvas_, &BlueprintCanvasView::interactionModeChanged, this, syncMode);

    snapButton_ = new QToolButton(toolbar);
    snapButton_->setObjectName(QStringLiteral("BlueprintSnapButton"));
    snapButton_->setText(tr("Snap to Grid"));
    snapButton_->setToolTip(tr("Snap to Grid"));
    snapButton_->setIconSize(QSize(20, 20));
    snapButton_->setFixedSize(22, 22);
    snapButton_->setProperty("flatIconButton", true);
    snapButton_->setAutoRaise(true);
    snapButton_->setCheckable(true);
    snapButton_->setChecked(canvas_->gridSnapEnabled());
    connect(snapButton_, &QToolButton::toggled, canvas_, &BlueprintCanvasWidget::setGridSnapEnabled);
    connect(canvas_, &BlueprintCanvasWidget::gridSnapEnabledChanged, snapButton_, &QToolButton::setChecked);

    filterMenu_ = new QMenu(tr("Viewport Filter"), this);
    filterMenu_->setObjectName(QStringLiteral("BlueprintViewportFilterMenu"));
    filterMenu_->installEventFilter(this);
    filterSoundsAction_ = filterMenu_->addAction(tr("Filter Out All Sounds"));
    filterSoundsAction_->setObjectName(QStringLiteral("BlueprintFilterSoundsAction"));
    filterSoundsAction_->setCheckable(true);
    connect(filterSoundsAction_, &QAction::toggled, canvas_, &BlueprintCanvasWidget::setSoundsFiltered);
    filterButton_ = new QToolButton(toolbar);
    filterButton_->setObjectName(QStringLiteral("BlueprintViewportFilterButton"));
    filterButton_->setText(tr("Viewport Filter"));
    connect(filterButton_, &QToolButton::clicked, this, [this]() {
        openMenu_(filterMenu_, filterButton_);
    });
    connect(filterMenu_, &QMenu::aboutToShow, this, [this]() { filterButton_->setChecked(true); });
    connect(filterMenu_, &QMenu::aboutToHide, this, [this]() {
        filterButton_->setChecked(canvas_->soundsFiltered());
    });
    connect(canvas_, &BlueprintCanvasWidget::viewportFilterChanged, this, &BlueprintPanel::syncFilter_);

    visibilityMenu_ = new BlueprintVisibilityMenu(this);
    visibilityMenu_->installEventFilter(this);
    visibilityButton_ = new QToolButton(toolbar);
    visibilityButton_->setObjectName(QStringLiteral("BlueprintViewportVisibilityButton"));
    visibilityButton_->setText(tr("Viewport Visibility"));
    connect(visibilityButton_, &QToolButton::clicked, this, [this]() {
        openMenu_(visibilityMenu_, visibilityButton_);
    });
    connect(visibilityMenu_, &QMenu::aboutToShow, this, [this]() { visibilityButton_->setChecked(true); });
    connect(visibilityMenu_, &QMenu::aboutToHide, this, [this]() {
        visibilityButton_->setChecked(canvas_->nodeContentFields() != kAllNodeContentFields);
    });
    connect(visibilityMenu_, &BlueprintVisibilityMenu::fieldsToggled, canvas_,
            [this](NodeContentFields fields) {
        const auto current = canvas_->nodeContentFields();
        canvas_->setNodeContentFields((current & fields) == fields
            ? current & ~fields : current | fields);
    });
    canvas_->installEventFilter(this);
    for (auto* button : {filterButton_, visibilityButton_}) {
        button->setAccessibleName(button->text());
        button->setToolTip(button->text());
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(20, 20));
        button->setFixedSize(22, 22);
        button->setProperty("flatIconButton", true);
        button->setAutoRaise(true);
        button->setCheckable(true);
    }
    syncFilter_();
    syncVisibility_();
    connect(canvas_, &BlueprintCanvasWidget::nodeContentFieldsChanged, this, &BlueprintPanel::syncVisibility_);
    auto* separator = new QFrame(toolbar);
    separator->setObjectName(QStringLiteral("BlueprintToolSeparator"));
    separator->setFixedSize(1, 16);
    auto* toolLayout = new QHBoxLayout(toolbar);
    toolLayout->setContentsMargins(6, 2, 6, 2);
    toolLayout->setSpacing(2);
    toolLayout->addWidget(selectButton_);
    toolLayout->addWidget(panButton_);
    toolLayout->addSpacing(5);
    toolLayout->addWidget(separator);
    toolLayout->addSpacing(5);
    toolLayout->addWidget(snapButton_);
    toolLayout->addStretch();
    toolLayout->addWidget(filterButton_);
    toolLayout->addWidget(visibilityButton_);
    for (auto* button : {selectButton_, panButton_, snapButton_, filterButton_, visibilityButton_})
        connect(button, &QToolButton::toggled, this, &BlueprintPanel::refreshIcons_);
    refreshIcons_();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &BlueprintPanel::refreshIcons_);

    auto* statusBar = new QFrame(this);
    statusBar->setObjectName(QStringLiteral("BlueprintStatusBar"));
    statusBar->setFixedHeight(20);
    statusBar->setFont(FontManager::instance().getFont(FontRole::Caption, 9));

    resetButton_ = new QToolButton(statusBar);
    resetButton_->setText(tr("Reset Layout"));
    resetButton_->setToolTip(tr("Reset node positions to auto layout"));
    resetButton_->setCursor(Qt::PointingHandCursor);
    connect(resetButton_, &QToolButton::clicked,
            canvas_, &BlueprintCanvasWidget::resetNodeLayout);

    zoomButton_ = new QToolButton(statusBar);
    zoomButton_->setObjectName(QStringLiteral("BlueprintZoomButton"));
    zoomButton_->setText(QStringLiteral("100%"));

    VNX::ContextInfo::tag(selectButton_, VNX::ContextTopic::forPanel(QStringLiteral("blueprint.select")));
    VNX::ContextInfo::tag(panButton_, VNX::ContextTopic::forPanel(QStringLiteral("blueprint.pan")));
    VNX::ContextInfo::tag(snapButton_, VNX::ContextTopic::forPanel(QStringLiteral("blueprint.snap")));
    VNX::ContextInfo::tag(filterButton_, VNX::ContextTopic::forPanel(QStringLiteral("blueprint.viewportFilter")));
    VNX::ContextInfo::tag(visibilityButton_, VNX::ContextTopic::forPanel(QStringLiteral("blueprint.viewportVisibility")));
    VNX::ContextInfo::tag(resetButton_, VNX::ContextTopic::forPanel(QStringLiteral("blueprint.resetLayout")));
    VNX::ContextInfo::tag(zoomButton_, VNX::ContextTopic::forPanel(QStringLiteral("blueprint.zoom")));
    zoomButton_->setCursor(Qt::PointingHandCursor);
    zoomButton_->setPopupMode(QToolButton::InstantPopup);

    auto* zoomMenu = new QMenu(zoomButton_);
    QAction* fitAct = zoomMenu->addAction(tr("Zoom to Fit"));
    connect(fitAct, &QAction::triggered,
            canvas_, &BlueprintCanvasWidget::zoomToFit);
    zoomMenu->addSeparator();
    for (int pct : {10, 25, 50, 75, 100, 125, 150, 200, 400}) {
        QAction* act = zoomMenu->addAction(QStringLiteral("%1%").arg(pct));
        connect(act, &QAction::triggered, canvas_, [this, pct]() {
            canvas_->setZoomFactor(pct / 100.0);
        });
    }
    zoomButton_->setMenu(zoomMenu);
    zoomButton_->setText(QStringLiteral("400%"));
    zoomButton_->ensurePolished();
    zoomButton_->setFixedWidth(zoomButton_->sizeHint().width());
    zoomButton_->setText(QStringLiteral("100%"));

    auto* barLayout = new QHBoxLayout(statusBar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(6);
    barLayout->addWidget(resetButton_);
    barLayout->addStretch(1);
    barLayout->addWidget(zoomButton_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addSpacing(0);
    layout->addWidget(canvas_, 1);
    layout->addWidget(statusBar, 0);
    meter_ = new MeterPanel(this, MeterPanel::Presentation::Embedded);
    meter_->setObjectName(QStringLiteral("BlueprintPeakMeter"));
    connect(meter_, &MeterPanel::detachRequested, this, &BlueprintPanel::meterDetachRequested);
    meterResizeHandle_ = new QWidget(this);
    meterResizeHandle_->setObjectName(QStringLiteral("BlueprintMeterResizeHandle"));
    meterResizeHandle_->setCursor(Qt::SplitHCursor);
    meterResizeHandle_->hide();
    meterResizeHandle_->installEventFilter(this);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            meterResizeHandle_, qOverload<>(&QWidget::update));
    meter_->installEventFilter(this);
    connect(meter_, &MeterPanel::expandedChanged, this, [this](bool expanded) {
        resizingMeter_ = false;
        meterResizeHandle_->setVisible(expanded);
        positionOverlays_();
    });
    setMinimumHeight(meter_->minimumHeight() + statusBar->height());
    updateHeaderHeight_();
    connect(meter_, &MeterPanel::embeddedHeaderHeightChanged, this, &BlueprintPanel::updateHeaderHeight_);
    positionOverlays_();

    connect(canvas_, &BlueprintCanvasView::zoomChanged,
            this, &BlueprintPanel::updateZoomText_);
}

void BlueprintPanel::refreshIcons_()
{
    auto& theme = ThemeManager::instance();
    const auto refresh = [&theme](QToolButton* button, const char* asset) {
        const ThemeAssetVariant variant{button->iconSize(), button->devicePixelRatioF(), 0,
            button->isChecked() ? theme.colors().accent : QColor()};
        button->setIcon(theme.resolveIcon(QString::fromLatin1(asset), variant).icon);
    };
    refresh(selectButton_, ThemeAssetIds::BlueprintSelect);
    refresh(panButton_, ThemeAssetIds::BlueprintPan);
    refresh(snapButton_, ThemeAssetIds::BlueprintGridSnap);
    refresh(filterButton_, ThemeAssetIds::BlueprintChainFilter);
    refresh(visibilityButton_, ThemeAssetIds::BlueprintDisplayVisibility);
}

void BlueprintPanel::syncVisibility_()
{
    const auto fields = canvas_->nodeContentFields();
    visibilityMenu_->setFields(fields);
    visibilityButton_->setChecked(fields != kAllNodeContentFields || visibilityMenu_->isVisible());
}

void BlueprintPanel::syncFilter_()
{
    filterSoundsAction_->setChecked(canvas_->soundsFiltered());
    filterButton_->setChecked(canvas_->soundsFiltered() || filterMenu_->isVisible());
}

void BlueprintPanel::openMenu_(QMenu* menu, QToolButton* button)
{
    if (menu->isVisible()) {
        menu->close();
        return;
    }
    if (menu != filterMenu_) filterMenu_->close();
    if (menu != visibilityMenu_) visibilityMenu_->close();
    const QSize size = menu->sizeHint();
    menu->popup(button->mapToGlobal(QPoint(button->width() - size.width(), button->height())));
}

void BlueprintPanel::updateHeaderHeight_()
{
    const int height = meter_->embeddedHeaderHeight();
    toolbar_->setFixedHeight(height);
    layout()->itemAt(0)->spacerItem()->changeSize(0, height, QSizePolicy::Minimum, QSizePolicy::Fixed);
    layout()->invalidate();
}

void BlueprintPanel::positionOverlays_()
{
    if (!meter_ || !meterResizeHandle_)
        return;
    const int toolbarMinimumWidth = toolbar_->minimumSizeHint().width();
    setMinimumWidth(meter_->minimumWidth() + toolbarMinimumWidth);
    const int meterWidth = std::clamp(meter_->sizeHint().width(), meter_->minimumWidth(),
        std::max(meter_->minimumWidth(), width() - toolbarMinimumWidth));
    meter_->setGeometry(0, 0, meterWidth, canvas_->geometry().bottom() + 1);
    toolbar_->setGeometry(meterWidth, 0, width() - meterWidth, toolbar_->height());
    meterResizeHandle_->setGeometry(meterWidth - 3, 0, 6, meter_->height());
    meterResizeHandle_->raise();
    const QPoint viewportOrigin = canvas_->viewport()->mapTo(this, QPoint());
    canvas_->setNavigationViewportMargins(QMargins(
        std::max(0, meter_->geometry().right() + 1 - viewportOrigin.x())
            + (meterResizeHandle_->isHidden() ? 0 : 3), 0, 0, 0));
}

void BlueprintPanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    positionOverlays_();
}

bool BlueprintPanel::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == visibilityMenu_ || watched == filterMenu_) && event->type() == QEvent::MouseButtonPress) {
        const QPoint position = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
        auto* button = watched == visibilityMenu_ ? visibilityButton_ : filterButton_;
        if (button->rect().contains(button->mapFromGlobal(position)))
            static_cast<QMenu*>(watched)->setAttribute(Qt::WA_NoMouseReplay);
    }
    if ((watched == canvas_ || watched == meter_) && (event->type() == QEvent::Resize || event->type() == QEvent::Move
        || event->type() == QEvent::Show))
        positionOverlays_();
    if (watched == meterResizeHandle_) {
        if (event->type() == QEvent::Paint) {
            if (meterResizeHandle_->isEnabled() && meterResizeHandle_->underMouse()) {
                QPainter painter(meterResizeHandle_);
                const int x = meter_->geometry().right() - meterResizeHandle_->x();
                painter.fillRect(QRect(x, 0, 1, meterResizeHandle_->height()),
                                 ThemeManager::instance().colors().accent);
            }
            return true;
        } else if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
            meterResizeHandle_->update();
        } else if (event->type() == QEvent::MouseButtonPress) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                resizingMeter_ = true;
                meterPressX_ = mouse->globalPosition().toPoint().x();
                meterPressWidth_ = meter_->width();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove && resizingMeter_) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const int delta = mouse->globalPosition().toPoint().x() - meterPressX_;
            const int meterWidth = std::clamp(meterPressWidth_ + delta, meter_->minimumWidth(),
                std::max(meter_->minimumWidth(), width() - toolbar_->minimumSizeHint().width()));
            meter_->setEmbeddedPreferredWidth(meterWidth);
            return true;
        } else if (event->type() == QEvent::MouseButtonRelease
                   && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            resizingMeter_ = false;
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void BlueprintPanel::updateZoomText_(double factor)
{
    if (zoomButton_ != nullptr) {
        zoomButton_->setText(QStringLiteral("%1%").arg(qRound(factor * 100.0)));
    }
}

}
