#include "ComponentRecipeHost.h"

#include "ParameterControlHost.h"
#include "Core/ThemeManager.h"

#include <QBoxLayout>
#include <QPainter>
#include <QSizePolicy>

namespace VNX {

class ComponentRecipeHost::SlotContainer : public QWidget
{
public:
    SlotContainer(QWidget* parent, ThemeManager* themeManager)
        : QWidget(parent)
        , layout_(new QBoxLayout(QBoxLayout::LeftToRight, this))
        , themeManager_(themeManager)
    {
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(0);
    }

    void setContent(QWidget* widget)
    {
        if (content_ == widget)
            return;
        if (content_)
            content_->setParent(nullptr);
        content_ = widget;
        if (content_)
            layout_->addWidget(content_);
    }

    QWidget* content() const { return content_; }

    void setContentAlignment(Qt::Alignment alignment)
    {
        if (content_)
            layout_->setAlignment(content_, alignment);
    }

    void setBackgroundAsset(const QString& logicalId)
    {
        backgroundId_ = logicalId;
        background_.reset();
        backgroundRatio_ = 0.0;
        update();
    }

    QString backgroundAsset() const { return backgroundId_; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QBoxLayout* layout_;
    ThemeManager* themeManager_;
    QWidget* content_ = nullptr;
    QString backgroundId_;
    ThemeImageResourcePtr background_;
    qreal backgroundRatio_ = 0.0;
};

namespace {

void drawSlotBackground(QPainter& painter, const QRect& target,
                        const ThemeImageResource& resource)
{
    const QImage& image = resource.image;
    const QRectF whole(0.0, 0.0, image.width(), image.height());
    if (!resource.insets
        || resource.insets->left + resource.insets->right > target.width()
        || resource.insets->top + resource.insets->bottom > target.height())
    {
        painter.drawImage(QRectF(target), image, whole);
        return;
    }

    const ThemeAssetInsetsDto& insets = *resource.insets;
    const qreal ratio = image.devicePixelRatio();
    const qreal left = insets.left;
    const qreal top = insets.top;
    const qreal right = insets.right;
    const qreal bottom = insets.bottom;
    const QRectF box(target);
    const qreal targetX[4] = {box.left(), box.left() + left,
                              box.right() - right, box.right()};
    const qreal targetY[4] = {box.top(), box.top() + top,
                              box.bottom() - bottom, box.bottom()};
    const qreal sourceX[4] = {0.0, left * ratio,
                              whole.width() - right * ratio, whole.width()};
    const qreal sourceY[4] = {0.0, top * ratio,
                              whole.height() - bottom * ratio, whole.height()};

    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            const QRectF cell(QPointF(targetX[column], targetY[row]),
                              QPointF(targetX[column + 1], targetY[row + 1]));
            const QRectF source(QPointF(sourceX[column], sourceY[row]),
                                QPointF(sourceX[column + 1], sourceY[row + 1]));
            if (cell.isEmpty() || source.isEmpty())
                continue;
            painter.drawImage(cell, image, source);
        }
    }
}

}

void ComponentRecipeHost::SlotContainer::paintEvent(QPaintEvent*)
{
    if (backgroundId_.isEmpty())
        return;
    const qreal ratio = devicePixelRatioF();
    if (backgroundRatio_ != ratio)
    {
        background_ = themeManager_
                          ->resolveImage(backgroundId_,
                                         ThemeAssetVariant{QSize(), ratio, 0,
                                                           QColor()})
                          .resource;
        backgroundRatio_ = ratio;
    }
    if (!background_)
        return;
    QPainter painter(this);
    drawSlotBackground(painter, rect(), *background_);
}

namespace {

Qt::Alignment qtAlignment(ComponentRecipeAlignment alignment, bool vertical)
{
    switch (alignment)
    {
        case ComponentRecipeAlignment::Start:
            return vertical ? Qt::AlignTop | Qt::AlignHCenter
                            : Qt::AlignLeft | Qt::AlignVCenter;
        case ComponentRecipeAlignment::Center:
            return Qt::AlignCenter;
        case ComponentRecipeAlignment::End:
            return vertical ? Qt::AlignBottom | Qt::AlignHCenter
                            : Qt::AlignRight | Qt::AlignVCenter;
        case ComponentRecipeAlignment::Fill:
            return {};
    }
    return {};
}

void applySlotSize(QWidget* widget,
                   const ComponentRecipeSlot& slot,
                   bool vertical)
{
    const int unlimited = QWIDGETSIZE_MAX;
    widget->setMinimumSize(0, 0);
    widget->setMaximumSize(unlimited, unlimited);
    const int minimum = slot.minimum >= 0 ? slot.minimum : 0;
    const int maximum = slot.maximum >= 0 ? slot.maximum : unlimited;
    if (vertical)
    {
        widget->setMinimumHeight(minimum);
        widget->setMaximumHeight(maximum);
        if (slot.preferred >= 0)
        {
            widget->setMinimumHeight(slot.preferred);
            widget->setMaximumHeight(slot.preferred);
        }
    }
    else
    {
        widget->setMinimumWidth(minimum);
        widget->setMaximumWidth(maximum);
        if (slot.preferred >= 0)
        {
            widget->setMinimumWidth(slot.preferred);
            widget->setMaximumWidth(slot.preferred);
        }
    }
    const bool fill = slot.alignment == ComponentRecipeAlignment::Fill;
    widget->setSizePolicy(
        vertical ? QSizePolicy::Preferred
                 : (fill ? QSizePolicy::Expanding : QSizePolicy::Preferred),
        vertical ? (fill ? QSizePolicy::Expanding : QSizePolicy::Preferred)
                 : QSizePolicy::Preferred);
}

}

ComponentRecipeHost::ComponentRecipeHost(
    ComponentRecipeHostId host,
    QWidget* parent,
    ThemeManager* themeManager)
    : QWidget(parent)
    , host_(host)
    , appliedVariant_(ComponentRecipeVariant::TrackExpanded)
    , themeManager_(themeManager ? themeManager : &ThemeManager::instance())
{
    connect(themeManager_, &ThemeManager::themeChanged, this,
            [this]() { applyRecipe(); });
    connect(themeManager_, &ThemeManager::appearanceChanged, this, [this]() {
        for (auto* container : slots_)
            container->setBackgroundAsset(container->backgroundAsset());
    });
}

ComponentRecipeHost::~ComponentRecipeHost() = default;

QStringList ComponentRecipeHost::orderedSlotNames() const
{
    return orderedSlotNames_;
}

void ComponentRecipeHost::setBreakpoint(
    ComponentRecipeBreakpoint breakpoint)
{
    if (breakpoint_ == breakpoint)
        return;
    breakpoint_ = breakpoint;
    applyRecipe();
}

bool ComponentRecipeHost::registerSlot(ComponentRecipeSlotId slot,
                                       QWidget* widget)
{
    if (!widget || !componentRecipeHostSupportsSlot(host_, slot)
        || slots_.contains(slot))
        return false;
    auto* container = new SlotContainer(this, themeManager_);
    container->setContent(widget);
    slots_.insert(slot, container);
    applyRecipe();
    return true;
}

QWidget* ComponentRecipeHost::slotWidget(ComponentRecipeSlotId slot) const
{
    const auto it = slots_.constFind(slot);
    if (it == slots_.constEnd())
        return nullptr;
    return it.value()->content();
}

QString ComponentRecipeHost::slotAsset(ComponentRecipeSlotId slot) const
{
    const auto it = slots_.constFind(slot);
    if (it == slots_.constEnd())
        return QString();
    return it.value()->backgroundAsset();
}

const ComponentRecipe* ComponentRecipeHost::selectedRecipe() const
{
    const auto& recipes = themeManager_->snapshot()->ui.recipes;
    if (const ComponentRecipe* exact = recipes.recipeFor(host_, breakpoint_))
        return exact;
    return recipes.recipeFor(host_, ComponentRecipeBreakpoint::Default);
}

void ComponentRecipeHost::applyRecipe()
{
    const ComponentRecipe* recipe = selectedRecipe();
    if (!recipe)
        return;

    delete layout();
    const bool vertical = componentRecipeVariantIsVertical(recipe->variant);
    auto* box = new QBoxLayout(vertical ? QBoxLayout::TopToBottom
                                        : QBoxLayout::LeftToRight);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(themeManager_->snapshot()->ui.metrics.spacingSmall);
    setLayout(box);

    for (auto* container : slots_)
    {
        container->hide();
        container->setBackgroundAsset(QString());
    }
    orderedSlotNames_.clear();
    for (const ComponentRecipeSlot& slot : recipe->items)
    {
        const auto containerIt = slots_.constFind(slot.id);
        if (containerIt == slots_.constEnd())
            continue;
        SlotContainer* container = containerIt.value();
        container->setVisible(slot.visible);
        if (!slot.visible)
            continue;
        applySlotSize(container, slot, vertical);
        container->setContentAlignment(qtAlignment(slot.alignment, vertical));
        box->addWidget(container);
        orderedSlotNames_.append(componentRecipeSlotName(slot.id));
        container->setBackgroundAsset(slot.asset);
        if (auto* parameter = qobject_cast<ParameterControlHost*>(
                slotWidget(slot.id)))
        {
            if (const auto context = componentRecipePresenterContext(
                    host_, slot.id, recipe->variant))
                parameter->setPresenterContext(*context);
            parameter->setPresenterOverride(slot.presenter);
        }
    }
    QWidget* previousFocus = nullptr;
    for (const ComponentRecipeSlot& slot : recipe->items)
    {
        const auto containerIt = slots_.constFind(slot.id);
        if (containerIt == slots_.constEnd() || !slot.visible)
            continue;
        QWidget* focus = containerIt.value()->content();
        if (!focus)
            continue;
        if (previousFocus)
            QWidget::setTabOrder(previousFocus, focus);
        previousFocus = focus;
    }
    appliedVariant_ = recipe->variant;
    emit recipeApplied(appliedVariant_);
}

}
