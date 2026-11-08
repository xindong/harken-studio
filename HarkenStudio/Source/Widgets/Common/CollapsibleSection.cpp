
#include "CollapsibleSection.h"
#include "LayoutDebugTint.h"
#include "../../Core/ThemeManager.h"
#include "../../Core/QtControlSkin.h"
#include "../../Core/ThemeAssetCatalog.h"
#include "../../Core/HarkenStyle.h"
#include <QPropertyAnimation>
#include <QMargins>
#include <QMoveEvent>
#include <QShowEvent>
#include <QHBoxLayout>
#include <utility>

namespace VNX {

CollapsibleSection::CollapsibleSection(const QString& title,
                                       int animationDuration,
                                       QWidget* parent)
    : QWidget(parent)
    , animationDuration_(animationDuration)
{
    setObjectName("CollapsibleSection");
    auto& theme = ThemeManager::instance();
    indicatorColor_ = theme.colors().accent;

    toggleButton_ = new QToolButton(this);
    toggleButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggleButton_->setText(title);
    toggleButton_->setCheckable(true);
    toggleButton_->setChecked(false);
    toggleButton_->setCursor(Qt::PointingHandCursor);
    toggleButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    headerLine_ = new QFrame(this);
    headerLine_->setFrameShape(QFrame::HLine);
    headerLine_->setFrameShadow(QFrame::Sunken);
    headerLine_->hide();

    contentArea_ = new QWidget(this);
    contentArea_->setObjectName(QStringLiteral("CollapsibleContent"));
    contentArea_->setAttribute(Qt::WA_StyledBackground, true);
    contentArea_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    contentArea_->setMaximumHeight(0);
    contentArea_->setMinimumHeight(0);
    
    mainLayout_ = new QVBoxLayout(this);
    mainLayout_->setSpacing(0);
    mainLayout_->setContentsMargins(0, 0, 0, 0);
    headerLayout_ = new QGridLayout();
    headerLayout_->setContentsMargins(0, 0, 0, 0);
    headerLayout_->setSpacing(0);
    headerLayout_->setColumnStretch(0, 1);
    headerLayout_->addWidget(toggleButton_, 0, 0, 1, 3);
    mainLayout_->addLayout(headerLayout_);
    mainLayout_->addWidget(headerLine_);
    mainLayout_->addWidget(contentArea_);
    
    toggleAnimation_ = new QParallelAnimationGroup(this);
    
    auto* contentAnimation = new QPropertyAnimation(contentArea_, "maximumHeight");
    contentAnimation->setDuration(animationDuration_);
    contentAnimation->setEasingCurve(QEasingCurve::InOutQuad);
    toggleAnimation_->addAnimation(contentAnimation);

    connect(toggleAnimation_, &QParallelAnimationGroup::finished, this, [this]() {
        if (expanded_ && !dynamicContent_)
            contentArea_->setMaximumHeight(QWIDGETSIZE_MAX);
    });

    connect(toggleButton_, &QToolButton::clicked, this, &CollapsibleSection::toggle);
    connect(&theme, &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });
    connect(&theme, &ThemeManager::appearanceChanged, this, &CollapsibleSection::refreshAppearance);

    applyTheme();
}

CollapsibleSection::~CollapsibleSection() = default;

void CollapsibleSection::setHeaderWidget(QWidget* widget)
{
    if (headerWidget_ == widget)
        return;
    if (headerWidget_) {
        headerLayout_->removeWidget(headerWidget_);
        delete headerWidget_;
    }
    headerWidget_ = widget;
    if (widget) {
        widget->setParent(this);
        headerLayout_->addWidget(widget, 0, 1, Qt::AlignVCenter);
    }
    applyTheme();
}

void CollapsibleSection::setContentLayout(QLayout* contentLayout)
{
    delete contentArea_->layout();

    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    railFrame_ = new QFrame(contentArea_);
    railFrame_->setVisible(false);
    row->addWidget(railFrame_);

    innerContent_ = new QWidget(contentArea_);
    innerContent_->setLayout(contentLayout);
    DebugTint::tintContainer(innerContent_, DebugTint::Role::Section);
    row->addWidget(innerContent_, 1);

    contentArea_->setLayout(row);
    applyTheme();
}

void CollapsibleSection::setDynamicContent(bool dynamic)
{
    dynamicContent_ = dynamic;
    if (dynamic && expanded_)
        contentArea_->setMaximumHeight(QWIDGETSIZE_MAX);
}

void CollapsibleSection::setFillsHeight(bool fills)
{
    fillsHeight_ = fills;
    refreshFill();
}

void CollapsibleSection::refreshFill()
{
    const bool fill = fillsHeight_ && expanded_;
    contentArea_->setSizePolicy(QSizePolicy::Expanding, fill ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    setSizePolicy(sizePolicy().horizontalPolicy(), fill ? QSizePolicy::Expanding : QSizePolicy::Preferred);
}

void CollapsibleSection::setIndicatorColor(const QColor& color)
{
    indicatorColor_ = color;
    hasCustomIndicator_ = true;
    applyTheme();
}

void CollapsibleSection::setHeaderTintColor(const QColor& color)
{
    headerTintColor_ = color;
    refreshAppearance();
}

void CollapsibleSection::setExpanded(bool expanded)
{
    if (expanded_ == expanded)
        return;
    
    expanded_ = expanded;
    toggleButton_->setChecked(expanded);

    if (dynamicContent_)
    {
        contentArea_->setMaximumHeight(expanded ? QWIDGETSIZE_MAX : 0);
    }
    else
    {
        auto* contentAnimation = static_cast<QPropertyAnimation*>(toggleAnimation_->animationAt(0));
        const int target = contentArea_->layout() ? contentArea_->layout()->sizeHint().height() : 0;
        contentAnimation->setStartValue(expanded ? 0 : contentArea_->height());
        contentAnimation->setEndValue(expanded ? target : 0);

        toggleAnimation_->start();
    }

    headerLine_->setVisible(expanded && hasCustomIndicator_);
    refreshFill();
    applyTheme();

    emit expandedChanged(expanded_);
}

void CollapsibleSection::toggle()
{
    setExpanded(!expanded_);
}

void CollapsibleSection::expand()
{
    setExpanded(true);
}

void CollapsibleSection::collapse()
{
    setExpanded(false);
}

int CollapsibleSection::detectNestingLevel() const
{
    int depth = 0;
    for (QWidget* p = parentWidget(); p; p = p->parentWidget())
        if (qobject_cast<CollapsibleSection*>(p))
            ++depth;
    return qMin(depth, 2);
}

void CollapsibleSection::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    const int detected = detectNestingLevel();
    if (detected != level_)
    {
        level_ = detected;
        applyTheme();
    }
}

void CollapsibleSection::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
    const auto* parentLayout = parentWidget() ? parentWidget()->layout() : nullptr;
    const bool topSeparator = parentLayout && y() > parentLayout->contentsMargins().top();
    if (hasTopSeparator_ == topSeparator)
        return;
    hasTopSeparator_ = topSeparator;
    refreshAppearance();
}

void CollapsibleSection::applyTheme()
{
    auto& theme = ThemeManager::instance();
    const auto& metrics = theme.metrics();
    const int lvl = level_ < 0 ? 0 : level_;
    const auto& layout = metrics.layout;
    const int headerSpacing = layout.sectionGap;
    headerLayout_->setColumnMinimumWidth(2, headerWidget_ ? headerSpacing : 0);
    headerLine_->setMaximumHeight(metrics.accentBorderWidth);
    if (railFrame_) {
        railFrame_->setFixedWidth(metrics.accentBorderWidth);
        railFrame_->setVisible(lvl >= 1);
    }

    if (contentArea_->layout())
    {
        QMargins rm = contentArea_->layout()->contentsMargins();
        rm.setLeft(lvl >= 1 ? layout.panelPadding : 0);
        contentArea_->layout()->setContentsMargins(rm);
    }

    if (innerContent_ && innerContent_->layout())
    {
        innerContent_->layout()->setContentsMargins(layout.indent, layout.controlGap,
                                                    0, layout.controlGap);
    }

    const int chevronExtent = theme.metrics().iconExtent;
    toggleButton_->setIconSize(QSize(chevronExtent, chevronExtent));
    refreshAppearance();
}

void CollapsibleSection::refreshAppearance()
{
    auto& theme = ThemeManager::instance();
    const QtControlSkin skin(theme.snapshot());
    const auto& colors = theme.colors();
    const int lvl = level_ < 0 ? 0 : level_;
    const auto applyStyle = [&theme](QWidget* widget, QString source) {
        const auto& native = theme.widgetAppearance().updateWidgetStyle(widget, std::move(source));
        if (widget->styleSheet() != native) widget->setStyleSheet(native);
        HarkenStyle::refreshWidgetAppearance(widget, theme);
        widget->update();
    };
    QString headerStyle = skin.collapsibleHeaderStyle(
        indicatorColor_, hasCustomIndicator_, lvl, hasTopSeparator_, headerTintColor_);
    if (headerWidget_)
        headerStyle += QStringLiteral("QToolButton { padding-right: %1px; }")
            .arg(headerWidget_->sizeHint().width() + 2 * theme.metrics().layout.sectionGap);
    applyStyle(toggleButton_, std::move(headerStyle));
    applyStyle(headerLine_, skin.collapsibleIndicatorStyle(indicatorColor_, 180, false));
    if (railFrame_ && lvl >= 1) {
        const QColor color = hasCustomIndicator_ ? indicatorColor_ : colors.accent;
        applyStyle(railFrame_, skin.collapsibleIndicatorStyle(color, lvl == 1 ? 150 : 100, true));
    }
    toggleButton_->setIcon(theme.resolveIcon(
        QString::fromLatin1(expanded_ ? ThemeAssetIds::NavigationChevronDown
                                     : ThemeAssetIds::NavigationChevronRight),
        ThemeAssetVariant{QSize(), 1.0, 0, colors.textPrimary}).icon);
}

}
