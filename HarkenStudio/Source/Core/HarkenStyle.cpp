#include "HarkenStyle.h"

#include "QtControlSkin.h"
#include "ThemeAssetCatalog.h"
#include "ThemeManager.h"

#include <QAbstractSpinBox>
#include <QComboBox>
#include <QEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QStyleOption>
#include <QTabBar>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolButton>
#include <QWidget>

namespace VNX {

namespace {

class InputValidation : public QObject
{
public:
    explicit InputValidation(QLineEdit* editor) : QObject(editor), editor_(editor)
    {
        setObjectName(QStringLiteral("_vnInputValidation"));
        connect(editor, &QLineEdit::textChanged, this, [this] { refresh(); });
        connect(editor, &QLineEdit::inputRejected, this, [this] { refresh(true); });
        connect(editor, &QLineEdit::editingFinished, this, [this] { refresh(); });
        editor->installEventFilter(this);
        refresh();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Show || event->type() == QEvent::FocusIn)
            refresh();
        return QObject::eventFilter(watched, event);
    }

private:
    void refresh(bool rejected = false)
    {
        QWidget* field = editor_;
        bool acceptable = editor_->hasAcceptableInput();
        if (auto* spin = qobject_cast<QAbstractSpinBox*>(editor_->parentWidget())) {
            field = spin;
            acceptable = spin->hasAcceptableInput();
        } else if (auto* combo = qobject_cast<QComboBox*>(editor_->parentWidget())) {
            field = combo;
        }
        const bool emptyMixedValue = field->property("mixedAcrossPlatforms").toBool()
            && editor_->text().isEmpty();
        QtControlSkin::setSemanticProperty(field, "inputInvalid",
                                           rejected || (!acceptable && !emptyMixedValue));
    }

    QLineEdit* editor_;
};

}

HarkenStyle::HarkenStyle(QStyle* baseStyle, ThemeManager* themeManager)
    : QProxyStyle(baseStyle), themeManager_(themeManager)
{
}

ThemeManager& HarkenStyle::theme() const
{
    return themeManager_ ? *themeManager_ : ThemeManager::instance();
}

void HarkenStyle::polish(QWidget* widget)
{
    if (qobject_cast<QAbstractButton*>(widget)
        && (widget->inherits("ads::CTitleBarButton")
            || (widget->objectName() == QLatin1String("tabCloseButton")
                && widget->parentWidget()
                && widget->parentWidget()->inherits("ads::CDockWidgetTab"))
            || widget->objectName() == QLatin1String("qt_toolbar_ext_button")
            || (qobject_cast<QTabBar*>(widget->parentWidget())
                && (widget->inherits("CloseButton")
                    || widget->objectName() == QLatin1String("ScrollLeftButton")
                    || widget->objectName() == QLatin1String("ScrollRightButton")))
            || widget->inherits("QLineEditIconButton")))
        widget->setProperty("flatIconButton", true);
    QProxyStyle::polish(widget);
    auto* comboBox = qobject_cast<QComboBox*>(widget);
    if (comboBox)
        QtControlSkin::applyPopupStyle(comboBox);
    const auto* plainText = qobject_cast<QPlainTextEdit*>(widget);
    const auto* richText = qobject_cast<QTextEdit*>(widget);
    if (plainText)
        plainText->document()->setDocumentMargin(0);
    if (richText)
        richText->document()->setDocumentMargin(0);
    if (widget->contextMenuPolicy() == Qt::DefaultContextMenu
        && (qobject_cast<QLineEdit*>(widget)
            || qobject_cast<QAbstractSpinBox*>(widget)
            || (comboBox && comboBox->isEditable())
            || (plainText && !plainText->isReadOnly())
            || (richText && !richText->isReadOnly())))
        widget->setContextMenuPolicy(Qt::PreventContextMenu);
    auto* editor = qobject_cast<QLineEdit*>(widget);
    if (auto* spin = qobject_cast<QAbstractSpinBox*>(widget)) {
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        editor = spin->findChild<QLineEdit*>();
    } else if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        editor = combo->lineEdit();
    }
    if (editor && !editor->findChild<QObject*>(QStringLiteral("_vnInputValidation"),
                                              Qt::FindDirectChildrenOnly))
        new InputValidation(editor);
    if (widget->property("flatIconButton").toBool())
        widget->installEventFilter(this);
}

void HarkenStyle::unpolish(QWidget* widget)
{
    widget->removeEventFilter(this);
    QProxyStyle::unpolish(widget);
}

bool HarkenStyle::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Paint) {
        auto* button = qobject_cast<QAbstractButton*>(watched);
        if (button && button->property("flatIconButton").toBool()) {
            if (!button->icon().isNull()
                && button->property("_vnIconButtonCacheKey").toLongLong() != button->icon().cacheKey()) {
                const QIcon icon = QtControlSkin::iconButtonIcon(button->icon());
                button->setProperty("_vnIconButtonCacheKey", QVariant::fromValue(icon.cacheKey()));
                button->setIcon(icon);
            }
            const bool tabClose = button->inherits("CloseButton")
                && qobject_cast<QTabBar*>(button->parentWidget());
            if (!tabClose && !button->inherits("QLineEditIconButton"))
                return QProxyStyle::eventFilter(watched, event);
            QStyleOptionToolButton option;
            option.initFrom(button);
            if (button->isDown()) option.state |= State_Sunken;
            QPainter painter(button);
            button->style()->drawPrimitive(PE_PanelButtonTool, &option, &painter, button);
            if (tabClose) {
                const QRect rect = alignedRect(option.direction, Qt::AlignCenter,
                                               QSize(20, 20), option.rect);
                theme().resolveIcon(QString::fromLatin1(ThemeAssetIds::DockClose)).icon.paint(
                    &painter, rect, Qt::AlignCenter,
                    button->isEnabled() ? QIcon::Normal : QIcon::Disabled);
                return true;
            }
        }
    }
    return QProxyStyle::eventFilter(watched, event);
}

int HarkenStyle::styleHint(StyleHint hint, const QStyleOption* option,
                          const QWidget* widget,
                          QStyleHintReturn* returnData) const
{
    if (hint == SH_UnderlineShortcut || hint == SH_ComboBox_Popup)
        return 0;
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

int HarkenStyle::pixelMetric(PixelMetric metric, const QStyleOption* option,
                            const QWidget* widget) const
{
    if (metric == PM_LineEditIconSize || metric == PM_SmallIconSize)
        return theme().metrics().objectIconExtent;
    if (metric == PM_MenuButtonIndicator || metric == PM_HeaderMarkSize
        || metric == PM_TreeViewIndentation || metric == PM_TabBarScrollButtonWidth)
        return theme().metrics().navigationChevronExtent;
    return QProxyStyle::pixelMetric(metric, option, widget);
}

QSize HarkenStyle::sizeFromContents(ContentsType type, const QStyleOption* option,
                                  const QSize& contentsSize, const QWidget* widget) const
{
    if (type == CT_MenuItem) {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (item && item->menuItemType != QStyleOptionMenuItem::Separator)
            return QtControlSkin(theme().snapshot()).popupItemSize(*item, contentsSize, widget);
    }
    return QProxyStyle::sizeFromContents(type, option, contentsSize, widget);
}

QRect HarkenStyle::subElementRect(SubElement element, const QStyleOption* option,
                                  const QWidget* widget) const
{
    if (element == SE_HeaderArrow || element == SE_HeaderLabel) {
        const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option);
        if (header && header->sortIndicator != QStyleOptionHeader::None) {
            const int extent = theme().metrics().navigationChevronExtent;
            const int margin = pixelMetric(PM_HeaderMargin, option, widget);
            QRect rect = header->rect.adjusted(margin, margin, -margin, -margin);
            if (element == SE_HeaderArrow) {
                const auto alignment = header->orientation == Qt::Horizontal
                    ? Qt::AlignRight | Qt::AlignVCenter : Qt::AlignHCenter | Qt::AlignBottom;
                rect = alignedRect(Qt::LeftToRight, alignment,
                                   QSize(extent, extent), rect);
            } else if (header->orientation == Qt::Horizontal) {
                rect.adjust(0, 0, -extent - margin, 0);
            } else {
                rect.adjust(0, 0, 0, -extent - margin);
            }
            return visualRect(header->direction, header->rect, rect);
        }
    }
    return QProxyStyle::subElementRect(element, option, widget);
}

QRect HarkenStyle::subControlRect(ComplexControl control,
                                  const QStyleOptionComplex* option,
                                  SubControl subControl, const QWidget* widget) const
{
    if (control == CC_ComboBox) {
        const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
        if (combo && (subControl == SC_ComboBoxArrow || subControl == SC_ComboBoxEditField)) {
            const int extent = theme().metrics().navigationChevronExtent;
            const int margin = combo->frame ? pixelMetric(PM_DefaultFrameWidth, option, widget) : 0;
            const QRect inner = combo->rect.adjusted(margin, margin, -margin, -margin);
            QRect rect(inner.right() - extent + 1, inner.top(), extent, inner.height());
            if (subControl == SC_ComboBoxEditField) {
                rect = visualRect(combo->direction, combo->rect,
                                  QProxyStyle::subControlRect(control, option, subControl, widget));
                rect.setRight(inner.right() - extent);
            }
            return visualRect(combo->direction, combo->rect, rect);
        }
    }
    return QProxyStyle::subControlRect(control, option, subControl, widget);
}

void HarkenStyle::drawPrimitive(PrimitiveElement element,
                                const QStyleOption* option, QPainter* painter,
                                const QWidget* widget) const
{
    const bool iconButton = widget && widget->property("flatIconButton").toBool();
    const char* asset = ThemeAssetIds::NavigationChevronDown;
    bool reverse = false;
    switch (element) {
    case PE_IndicatorArrowDown:
        break;
    case PE_IndicatorArrowUp:
        reverse = true;
        break;
    case PE_IndicatorArrowLeft:
        reverse = true;
        [[fallthrough]];
    case PE_IndicatorArrowRight:
        asset = ThemeAssetIds::NavigationChevronRight;
        break;
    case PE_IndicatorBranch:
        if (!(option->state & State_Children))
            return;
        if (!(option->state & State_Open)) {
            asset = ThemeAssetIds::NavigationChevronRight;
            reverse = option->direction == Qt::RightToLeft;
        }
        break;
    case PE_IndicatorHeaderArrow: {
        const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option);
        if (!header || header->sortIndicator == QStyleOptionHeader::None)
            return;
        reverse = header->sortIndicator == QStyleOptionHeader::SortDown;
#ifdef Q_OS_LINUX
        reverse = !reverse;
#endif
        break;
    }
    default:
        QProxyStyle::drawPrimitive(element, option, painter, widget);
        return;
    }

    if (option->rect.isEmpty())
        return;
    const int extent = theme().metrics().navigationChevronExtent;
    const auto& colors = theme().colors();
    const QColor tint = !(option->state & State_Enabled) ? colors.textDisabled
        : (option->state & State_Selected)
            ? option->palette.color(QPalette::HighlightedText)
            : (option->state & State_Sunken) && !iconButton ? colors.accentText : colors.textSecondary;
    const QIcon icon = theme().resolveIcon(
        QString::fromLatin1(asset), ThemeAssetVariant{QSize(), 1.0, 0, tint}).icon;
    const QRect rect = alignedRect(option->direction, Qt::AlignCenter,
                                   QSize(extent, extent), option->rect);
    painter->save();
    if (reverse) {
        const QPointF center = QRectF(rect).center();
        painter->translate(center);
        painter->rotate(180);
        painter->translate(-center);
    }
    icon.paint(painter, rect);
    painter->restore();
}

void HarkenStyle::drawControl(ControlElement element, const QStyleOption* option,
                             QPainter* painter, const QWidget* widget) const
{
    const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
    if (element != CE_MenuItem || !item
        || item->menuItemType == QStyleOptionMenuItem::Separator) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }

    QtControlSkin(theme().snapshot()).paintPopupItem(*painter, *item, widget);
}

void HarkenStyle::drawComplexControl(ComplexControl control,
                                     const QStyleOptionComplex* option,
                                     QPainter* painter, const QWidget* widget) const
{
    if (control == CC_ComboBox) {
        if (const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option)) {
            QStyleOptionComboBox frame = *combo;
            frame.subControls &= ~SC_ComboBoxArrow;
            QProxyStyle::drawComplexControl(control, &frame, painter, widget);
            if (combo->subControls & SC_ComboBoxArrow) {
                QStyleOption arrow = *combo;
                arrow.rect = subControlRect(control, combo, SC_ComboBoxArrow, widget);
                drawPrimitive(PE_IndicatorArrowDown, &arrow, painter, widget);
            }
            return;
        }
    }
    QProxyStyle::drawComplexControl(control, option, painter, widget);
}

}
