#include "GridEditorWidget.h"
#include "Widgets/Common/ElidedLabel.h"

#include "MixingEditorWidget.h"
#include "DiscreteGridWidget.h"
#include "SelectionListWidget.h"
#include "TransitionTableWidget.h"
#include "Core/Components/VSelection.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/TransitionSemantics.h"
#include "Widgets/Common/PaneSection.h"
#include "Widgets/Common/PaneSectionStack.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSet>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace VNX {

namespace {

class GridCrumbButton : public QToolButton
{
public:
    explicit GridCrumbButton(QWidget* parent) : QToolButton(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    QSize minimumSizeHint() const override
    {
        return {ThemeManager::instance().metrics().controlHeight,
                QToolButton::minimumSizeHint().height()};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);
        option.text = fontMetrics().elidedText(
            text(), Qt::ElideRight,
            width() - 2 * ThemeManager::instance().metrics().spacingSmall);
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
    }
};

bool isOrDescendsFrom(const ProjectItem* item, const ProjectItem* ancestor)
{
    for (const ProjectItem* it = item; it; it = it->parent)
        if (it == ancestor)
            return true;
    return false;
}

}

GridEditorWidget::GridEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("GridEditorWidget"));
    setFocusPolicy(Qt::StrongFocus);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    crumbBar_ = new QWidget(this);
    crumbBar_->setObjectName(QStringLiteral("gridEditorCrumbBar"));
    crumbBar_->setAttribute(Qt::WA_StyledBackground);
    crumbBar_->setProperty("surfaceRole", QStringLiteral("secondary"));
    auto* crumbBarLayout = new QHBoxLayout(crumbBar_);
    crumbBarLayout->setContentsMargins(0, 0, 0, 0);

    crumbsHost_ = new QWidget(crumbBar_);
    crumbsHost_->setObjectName(QStringLiteral("gridEditorCrumbsHost"));
    crumbsHost_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    crumbLayout_ = new QHBoxLayout(crumbsHost_);
    crumbLayout_->setContentsMargins(0, 0, 0, 0);
    crumbBarLayout->addWidget(crumbsHost_, 1);

    root->addWidget(crumbBar_);

    stack_ = new QStackedWidget(this);
    root->addWidget(stack_, 1);

    emptyLabel_ = new QLabel(stack_);
    emptyLabel_->setObjectName(QStringLiteral("gridEditorEmptyLabel"));
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setWordWrap(true);
    stack_->addWidget(emptyLabel_);

    gridPage_ = new PaneSectionStack(QStringLiteral("SelectionEditor"), stack_);
    gridPage_->setObjectName(QStringLiteral("selectionEditorPage"));
    const auto fill = [](PaneSection* section, QWidget* content) {
        auto* layout = new QVBoxLayout(section->content());
        layout->setSpacing(0);
        layout->addWidget(content, 1);
        ThemeManager::instance().bindMetrics(content, [layout](const ThemeMetrics& metrics) {
            const int padding = metrics.layout.panelPadding;
            layout->setContentsMargins(padding, padding, padding, padding);
        });
    };
    contentSection_ = gridPage_->addSection(tr("Content"), QStringLiteral("content"), 300);
    contentSection_->setObjectName(QStringLiteral("selectionContentSection"));
    editorStack_ = new QStackedWidget(contentSection_->content());
    editorStack_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    fill(contentSection_, editorStack_);
    transitionSection_ = gridPage_->addSection(tr("Transition"), QStringLiteral("transition"), 200);
    transitionSection_->setObjectName(QStringLiteral("selectionTransitionSection"));
    transitionTable_ = new TransitionTableWidget(transitionSection_->content());
    fill(transitionSection_, transitionTable_);

    discreteGrid_ = new DiscreteGridWidget(editorStack_);
    editorStack_->addWidget(discreteGrid_);
    mixingEditor_ = new MixingEditorWidget(editorStack_);
    editorStack_->addWidget(mixingEditor_);
    selectionList_ = new SelectionListWidget(editorStack_);
    editorStack_->addWidget(selectionList_);

    stack_->addWidget(gridPage_);

    QtControlSkin::setSemanticProperty(emptyLabel_, "textRole", QStringLiteral("muted"));

    connect(mixingEditor_, &MixingEditorWidget::drillRequested,
            this, &GridEditorWidget::drillInto);
    connect(discreteGrid_, &DiscreteGridWidget::drillRequested,
            this, &GridEditorWidget::drillInto);
    connect(discreteGrid_, &DiscreteGridWidget::auditionTargetRequested,
            this, &GridEditorWidget::auditionTargetRequested);
    connect(discreteGrid_, &DiscreteGridWidget::navigateToItem,
            this, &GridEditorWidget::navigateToItem);
    connect(discreteGrid_, &DiscreteGridWidget::locateItem,
            this, &GridEditorWidget::locateItem);
    connect(mixingEditor_, &MixingEditorWidget::navigateToItem,
            this, &GridEditorWidget::navigateToItem);
    connect(mixingEditor_, &MixingEditorWidget::locateItem,
            this, &GridEditorWidget::locateItem);
    connect(selectionList_, &SelectionListWidget::drillRequested,
            this, &GridEditorWidget::drillInto);
    connect(selectionList_, &SelectionListWidget::auditionTargetRequested,
            this, &GridEditorWidget::auditionTargetRequested);
    connect(selectionList_, &SelectionListWidget::navigateToItem,
            this, &GridEditorWidget::navigateToItem);
    connect(selectionList_, &SelectionListWidget::locateItem,
            this, &GridEditorWidget::locateItem);
    connect(selectionList_, &SelectionListWidget::sourceEditorRequested,
            this, &GridEditorWidget::sourceEditorRequested);
    connect(discreteGrid_, &DiscreteGridWidget::sourceEditorRequested,
            this, &GridEditorWidget::sourceEditorRequested);
    connect(mixingEditor_, &MixingEditorWidget::sourceEditorRequested,
            this, &GridEditorWidget::sourceEditorRequested);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this]() { refreshChrome(); });

    auto& pm = ProjectManager::instance();
    // Queued: grid commits land while a cell delegate or header drag is still
    // on the stack; rebuilding the table there would destroy the live editor.
    connect(&pm, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& prop, const QObject*) {
        if (!boundItem_)
            return;
        if (prop == QStringLiteral("mode")
            && (item == boundItem_ || item == displayItem_)) {
            refreshFromModel();
            return;
        }
        if (item == displayItem_ && (prop == QStringLiteral("playType")
                                     || prop == QStringLiteral("variableResponse"))) {
            refreshTransitionSection();
            return;
        }
        if (item == displayItem_ && prop == QStringLiteral("selections")) {
            discreteGrid_->reload();
            refreshTransitionSection();
            return;
        }
        static const QSet<QString> kDeclarationProps = {
            QStringLiteral("kind"), QStringLiteral("rangeMin"), QStringLiteral("rangeMax"),
            QStringLiteral("defaultValue"), QStringLiteral("labels")};
        if (isShowingGrid() && kDeclarationProps.contains(prop)) {
            discreteGrid_->reload();
        }
    }, Qt::QueuedConnection);
    connect(&pm, &ProjectManager::itemRenamed, this,
            [this](ProjectItem* item, const QString&) {
        if (!boundItem_)
            return;
        if (item == boundItem_ || item == displayItem_) {
            refreshFromModel();
        } else if (isShowingGrid()) {
            discreteGrid_->reload();
            mixingEditor_->reload();
        }
    }, Qt::QueuedConnection);
    connect(&pm, &ProjectManager::itemDeleted, this, [this](ProjectItem* item) {
        if (!boundItem_)
            return;
        if (isOrDescendsFrom(boundItem_, item)) {
            clearEditor();
            return;
        }
        if (isShowingGrid())
            refreshFromModel();
    });
    connect(&pm, &ProjectManager::projectClosed, this, [this]() { clearEditor(); });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this](const PlatformContext&) { refreshTransitionSection(); });

    clearEditor();
}

bool GridEditorWidget::acceptsItem(const ProjectItem* item)
{
    if (!item || !acceptsItemType(item->type))
        return false;
    return item->comp<VSelection>() != nullptr;
}

bool GridEditorWidget::acceptsItemType(ProjectItem::Type type)
{
    return type == ProjectItem::Type::Container;
}

QUuid GridEditorWidget::currentItemGuid() const
{
    return boundItem_ ? boundItem_->guid : QUuid();
}

QUuid GridEditorWidget::displayedItemGuid() const
{
    return displayItem_ ? displayItem_->guid : QUuid();
}

bool GridEditorWidget::isShowingGrid() const
{
    return stack_->currentWidget() == gridPage_;
}

QString GridEditorWidget::statusText() const
{
    const auto* selection = displayItem_ ? displayItem_->comp<VSelection>() : nullptr;
    return selection && selection->mode == SelectionMode::Blend
        ? mixingEditor_->statusText() : QString();
}

void GridEditorWidget::setPinned(bool pinned)
{
    if (pinned_ == pinned)
        return;
    pinned_ = pinned;
    emit pinToggled(pinned_);
}

void GridEditorWidget::bindItem(ProjectItem* item)
{
    if (item != boundItem_) {
        drillPath_.clear();
        placement_.reset();
    }
    boundItem_ = item;
    refreshFromModel();
}

void GridEditorWidget::setPlacementContext(const std::optional<PlacementContext>& placement)
{
    placement_ = placement;
    transitionTable_->setPlacementContext(displayItem_ == boundItem_ ? placement_ : std::nullopt);
}

void GridEditorWidget::clearEditor()
{
    drillPath_.clear();
    bindItem(nullptr);
}

void GridEditorWidget::drillInto(const QUuid& target)
{
    ProjectItem* inner = ProjectManager::instance().findItemByGuid(target);
    if (!acceptsItem(inner))
        return;
    drillPath_.push_back(target);
    refreshFromModel();
    emit auditionTargetRequested(displayedItemGuid());
}

void GridEditorWidget::drillBackTo(int depth)
{
    if (depth < 0 || depth >= static_cast<int>(drillPath_.size()))
        return;
    drillPath_.resize(depth);
    refreshFromModel();
    emit auditionTargetRequested(displayedItemGuid());
}

void GridEditorWidget::refreshFromModel()
{
    if (!acceptsItem(boundItem_)) {
        drillPath_.clear();
        displayItem_ = nullptr;
        discreteGrid_->setBoundItem(nullptr);
        mixingEditor_->setBoundItem(nullptr);
        selectionList_->setBoundItem(nullptr);
        transitionTable_->setBoundItem(nullptr);
        rebuildCrumbs({});
        showEmptyState({});
        return;
    }

    std::vector<ProjectItem*> chain{boundItem_};
    size_t depth = 0;
    for (; depth < drillPath_.size(); ++depth) {
        ProjectItem* inner = ProjectManager::instance().findItemByGuid(drillPath_[depth]);
        if (!acceptsItem(inner))
            break;
        chain.push_back(inner);
    }
    drillPath_.resize(depth);

    displayItem_ = chain.back();
    rebuildCrumbs(chain);
    const auto mode = displayItem_->comp<VSelection>()->mode;
    const bool match = mode == SelectionMode::Match;
    const bool blend = mode == SelectionMode::Blend;
    crumbBar_->setVisible(!drillPath_.empty());
    discreteGrid_->setBoundItem(match ? displayItem_ : nullptr);
    mixingEditor_->setBoundItem(blend ? displayItem_ : nullptr);
    selectionList_->setBoundItem(match || blend ? nullptr : displayItem_);
    editorStack_->setCurrentWidget(match ? static_cast<QWidget*>(discreteGrid_)
                                 : blend ? static_cast<QWidget*>(mixingEditor_)
                                         : static_cast<QWidget*>(selectionList_));
    transitionTable_->setBoundItem(displayItem_);
    transitionTable_->setPlacementContext(displayItem_ == boundItem_ ? placement_ : std::nullopt);
    refreshTransitionSection();
    stack_->setCurrentWidget(gridPage_);
    refreshChrome();
}

void GridEditorWidget::refreshTransitionSection()
{
    gridPage_->setSectionVisible(transitionSection_, displayItem_
        && TransitionSemantics::transitionMountState(*displayItem_,
               currentWorldPlatformScope(PlatformContextService::instance().context()))
            == TransitionMountState::Editable);
}

void GridEditorWidget::rebuildCrumbs(const std::vector<ProjectItem*>& chain)
{
    while (QLayoutItem* it = crumbLayout_->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    if (chain.empty())
        return;
    for (size_t i = 0; i + 1 < chain.size(); ++i) {
        auto* crumb = new GridCrumbButton(crumbsHost_);
        crumb->setObjectName(QStringLiteral("gridEditorCrumb"));
        crumb->setText(chain[i]->name);
        crumb->setAutoRaise(true);
        crumb->setCursor(Qt::PointingHandCursor);
        crumb->setToolTip(tr("Back to %1").arg(chain[i]->name));
        const int depth = static_cast<int>(i);
        connect(crumb, &QToolButton::clicked, this,
                [this, depth]() { drillBackTo(depth); });
        crumbLayout_->addWidget(crumb);
        auto* sep = new QLabel(QStringLiteral("›"), crumbsHost_);
        QtControlSkin::setSemanticProperty(sep, "textRole", QStringLiteral("muted"));
        crumbLayout_->addWidget(sep);
    }
    auto* last = new ElidedLabel(chain.back()->name, crumbsHost_);
    last->setObjectName(QStringLiteral("gridEditorTitle"));
    last->setToolTip(chain.back()->name);
    QtControlSkin::setSemanticProperty(last, "textRole", QStringLiteral("secondary"));
    QtControlSkin::setSemanticProperty(last, "fontEmphasis", QStringLiteral("strong"));
    crumbLayout_->addWidget(last, 1);
}

void GridEditorWidget::showEmptyState(const QString& message)
{
    crumbBar_->hide();
    emptyLabel_->setText(message);
    stack_->setCurrentWidget(emptyLabel_);
    refreshChrome();
}

void GridEditorWidget::refreshChrome()
{
    const auto& metrics = ThemeManager::instance().metrics();
    const auto* selection = displayItem_ ? displayItem_->comp<VSelection>() : nullptr;
    const int margin = selection ? metrics.layout.panelPadding : 0;
    layout()->setContentsMargins(margin, margin, margin, margin);
    layout()->setSpacing(margin);
    crumbBar_->setFixedHeight(metrics.timeline.rulerHeight);
    crumbBar_->layout()->setContentsMargins(metrics.layout.panelPadding, 0, metrics.layout.panelPadding, 0);
    crumbBar_->layout()->setSpacing(metrics.layout.controlGap);
    crumbLayout_->setSpacing(metrics.layout.controlGap);
}

void GridEditorWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refreshChrome();
}

}
