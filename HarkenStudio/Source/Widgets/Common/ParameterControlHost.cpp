#include "ParameterControlHost.h"

#include "LinearSliderPresenter.h"
#include "ParameterBinding.h"
#include "ParameterPresenter.h"
#include "RotaryKnobPresenter.h"
#include "VerticalFaderPresenter.h"
#include "Core/NumericFormat.h"
#include "Core/ThemeManager.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>

namespace VNX {

ParameterControlHost::ParameterControlHost(
    const QString& name,
    ParameterPresenterContext context,
    QWidget* parent,
    ThemeManager* themeManager)
    : QWidget(parent)
    , binding_(new ParameterBinding(this))
    , themeManager_(themeManager ? themeManager : &ThemeManager::instance())
    , context_(context)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    binding_->setName(name);
    setAccessibleName(name);
    installPresenter(FocusRole::None);

    connect(binding_, &ParameterBinding::metadataChanged, this, [this]() {
        if (presenter_ && presenter_->presenterId() != selectedPresenterId())
            rebuildPresenter();
        setAccessibleName(binding_->name());
        syncToolTips();
    });
    connect(binding_, &ParameterBinding::valueUpdated, this,
            &ParameterControlHost::syncToolTips);
    connect(themeManager_, &ThemeManager::themeChanged, this,
            [this]() { rebuildPresenter(); });
}

ParameterControlHost::~ParameterControlHost()
{
    if (presenter_)
        presenter_->primaryEditor()->removeEventFilter(this);
}

bool ParameterControlHost::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTipChange)
        syncToolTips();
    return QWidget::event(event);
}

bool ParameterControlHost::eventFilter(QObject* watched, QEvent* event)
{
    if (!presenter_ || watched != presenter_->primaryEditor() || !isEnabled())
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton
                && mouse->buttons() == Qt::LeftButton
                && mouse->modifiers() == Qt::ControlModifier
                && !presenter_->isGestureActive()) {
                resetClick_ = true;
                event->accept();
                binding_->commitValue(binding_->defaultValue());
                return true;
            }
            break;
        }
        case QEvent::MouseMove:
            if (resetClick_) {
                event->accept();
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            if (resetClick_ && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
                resetClick_ = false;
                event->accept();
                return true;
            }
            break;
        case QEvent::UngrabMouse:
        case QEvent::Hide:
        case QEvent::WindowDeactivate:
            resetClick_ = false;
            break;
        case QEvent::ContextMenu: {
            auto* menu = new QMenu(presenter_->primaryEditor());
            connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
            auto* reset = menu->addAction(tr("Reset"));
            reset->setEnabled(!binding_->isEditing()
                && (binding_->mixedValue()
                    || !qFuzzyCompare(binding_->value() + 1.0, binding_->defaultValue() + 1.0)));
            connect(reset, &QAction::triggered, this, [this] {
                if (isEnabled())
                    binding_->commitValue(binding_->defaultValue());
            });
            menu->popup(static_cast<QContextMenuEvent*>(event)->globalPos());
            event->accept();
            return true;
        }
        default:
            break;
    }
    return QWidget::eventFilter(watched, event);
}

void ParameterControlHost::syncToolTips()
{
    if (!presenter_) return;
    presenter_->setToolTip(toolTip());
    const QString unit = binding_->unit().trimmed();
    QString text = toolTip();
    if (!binding_->mixedValue()) {
        if (!text.isEmpty()) text += QLatin1Char('\n');
        text += QStringLiteral("%1 %2").arg(
            formatDecimal(binding_->toDisplayValue(binding_->value()), binding_->decimals()), unit).trimmed();
    }
    presenter_->primaryEditor()->setToolTip(text);
    if (auto* editor = presenter_->valueEditor())
        editor->setToolTip(text);
    if (auto* editor = presenter_->choiceEditor())
        editor->setToolTip(text);
}

ParameterPresenterId ParameterControlHost::presenterId() const
{
    return presenter_ ? presenter_->presenterId() : selectedPresenterId();
}

void ParameterControlHost::setPresenterContext(
    ParameterPresenterContext context)
{
    if (context_ == context)
        return;
    context_ = context;
    presenterOverride_.reset();
    rebuildPresenter();
}

void ParameterControlHost::setPresenterOverride(
    std::optional<ParameterPresenterId> presenter)
{
    if (presenter == presenterOverride_)
        return;
    if (presenter && !parameterPresenterSupportsContext(*presenter, context_))
        presenter = defaultParameterPresenter(context_);
    presenterOverride_ = presenter;
    rebuildPresenter();
}

void ParameterControlHost::setCompactValueColumn(int width)
{
    compactValueColumnWidth_ = width;
    primaryColumnStretch_ = 0;
    valueColumnStretch_ = 0;
    if (presenter_)
        presenter_->setCompactValueColumn(width);
}

void ParameterControlHost::setValueColumnStretch(int primaryStretch, int valueStretch)
{
    primaryColumnStretch_ = primaryStretch;
    valueColumnStretch_ = valueStretch;
    if (presenter_)
        presenter_->setValueColumnStretch(primaryStretch, valueStretch);
}

void ParameterControlHost::rebuildPresenter()
{
    const FocusRole focusRole = currentFocusRole();
    if (presenter_ && presenter_->isGestureActive())
        binding_->commitEdit();
    if (presenter_)
        presenter_->primaryEditor()->removeEventFilter(this);
    delete presenter_;
    presenter_ = nullptr;
    installPresenter(focusRole);
}

ParameterPresenterId ParameterControlHost::selectedPresenterId() const
{
    if (binding_->discreteSelection() && presenterOverride_ != ParameterPresenterId::HorizontalSlider)
        return ParameterPresenterId::SliderWithValue;
    const ParameterPresenterId requested = presenterOverride_.value_or(
        themeManager_->snapshot()->ui.parameterPresenters
            .presenterFor(context_));
    return parameterPresenterSupportsContext(requested, context_)
        ? requested
        : defaultParameterPresenter(context_);
}

ParameterControlHost::FocusRole ParameterControlHost::currentFocusRole() const
{
    if (!presenter_)
        return FocusRole::None;
    QWidget* focused = QApplication::focusWidget();
    if (focused == presenter_->primaryEditor())
        return FocusRole::PrimaryEditor;
    if (focused && (focused == presenter_->valueEditor() || focused == presenter_->choiceEditor()))
        return FocusRole::ValueEditor;
    return FocusRole::None;
}

void ParameterControlHost::installPresenter(FocusRole focusRole)
{
    resetClick_ = false;
    switch (selectedPresenterId())
    {
        case ParameterPresenterId::SliderWithValue:
            presenter_ = new LinearSliderPresenter(
                binding_, true, themeManager_, this);
            break;
        case ParameterPresenterId::HorizontalSlider:
            presenter_ = new LinearSliderPresenter(
                binding_, false, themeManager_, this);
            break;
        case ParameterPresenterId::VerticalFader:
            presenter_ = new VerticalFaderPresenter(
                binding_, themeManager_, this);
            break;
        case ParameterPresenterId::RotaryKnob:
            presenter_ = new RotaryKnobPresenter(
                binding_, themeManager_, this);
            break;
    }
    layout()->addWidget(presenter_);
    presenter_->primaryEditor()->installEventFilter(this);
    if (compactValueColumnWidth_ > 0)
        presenter_->setCompactValueColumn(compactValueColumnWidth_);
    if (valueColumnStretch_ > 0)
        presenter_->setValueColumnStretch(primaryColumnStretch_, valueColumnStretch_);

    if (focusRole == FocusRole::PrimaryEditor)
        presenter_->primaryEditor()->setFocus(Qt::OtherFocusReason);
    else if (focusRole == FocusRole::ValueEditor)
    {
        QWidget* target = presenter_->valueEditor();
        if (!target)
            target = presenter_->choiceEditor();
        if (!target)
            target = presenter_->primaryEditor();
        target->setFocus(Qt::OtherFocusReason);
    }
    syncToolTips();
    presenterInstalled();
}

}
