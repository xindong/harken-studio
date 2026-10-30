#include "HarkenStyle.h"

#include "QtControlSkin.h"
#include "ThemeAssetCatalog.h"
#include "ThemeManager.h"
#include "WidgetAppearance.h"

#include <QAbstractSpinBox>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDynamicPropertyChangeEvent>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QMdiSubWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QSplitter>
#include <QStyleOptionSlider>
#include <QTabWidget>
#include <QToolBar>
#include <QTimer>
#include <algorithm>
#include <utility>
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

QStringList appearanceStates(const QStyleOption* option)
{
    QStringList states;
    if (const auto* slider = qstyleoption_cast<const QStyleOptionSlider*>(option))
        states.append(slider->orientation == Qt::Horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical"));
    if (const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option)) {
        states.append(header->orientation == Qt::Horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical"));
        if (header->position == QStyleOptionHeader::Beginning) states.append(QStringLiteral("first"));
        if (header->position == QStyleOptionHeader::End) states.append(QStringLiteral("last"));
        if (header->position == QStyleOptionHeader::OnlyOneSection) states.append(QStringLiteral("only-one"));
    }
    if (const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option))
        if (item->features & QStyleOptionViewItem::Alternate) states.append(QStringLiteral("alternate"));
    return states;
}

QStyle::State widgetState(const QWidget* widget)
{
    QStyleOption option;
    option.initFrom(widget);
    if (const auto* button = qobject_cast<const QAbstractButton*>(widget)) {
        if (button->isDown()) option.state |= QStyle::State_Sunken;
        if (button->isChecked()) option.state |= QStyle::State_On;
    }
    return option.state;
}

std::array<QSizeF, 4> normalizedRadii(const QRectF& rect, std::array<QSizeF, 4> radii)
{
    for (auto& radius : radii) radius = radius.expandedTo(QSizeF(0, 0));
    if (radii[0].width() + radii[1].width() > rect.width()) radii[0] = radii[1] = QSizeF(0, 0);
    if (radii[3].width() + radii[2].width() > rect.width()) radii[3] = radii[2] = QSizeF(0, 0);
    if (radii[0].height() + radii[3].height() > rect.height()) radii[0] = radii[3] = QSizeF(0, 0);
    if (radii[1].height() + radii[2].height() > rect.height()) radii[1] = radii[2] = QSizeF(0, 0);
    return radii;
}

QPainterPath roundedPath(const QRectF& outer, const std::array<QSizeF, 4>& input,
                        const QMarginsF& inset = {})
{
    const QRectF rect = outer.marginsRemoved(inset);
    const auto normalized = normalizedRadii(outer, input);
    const std::array<QPointF, 4> offsets = {QPointF(inset.left(), inset.top()),
        QPointF(inset.right(), inset.top()), QPointF(inset.right(), inset.bottom()),
        QPointF(inset.left(), inset.bottom())};
    std::array<QSizeF, 4> radius;
    for (size_t index = 0; index < radius.size(); ++index) {
        const auto value = normalized[index];
        radius[index] = QSizeF(std::max(qreal(0), value.width() - offsets[index].x()),
                              std::max(qreal(0), value.height() - offsets[index].y()));
    }
    QPainterPath path;
    path.moveTo(rect.left() + radius[0].width(), rect.top());
    path.lineTo(rect.right() - radius[1].width(), rect.top());
    if (!radius[1].isEmpty()) path.arcTo(QRectF(QPointF(rect.right() - 2 * radius[1].width(), rect.top()), 2 * radius[1]), 90, -90);
    else path.lineTo(rect.topRight());
    path.lineTo(rect.right(), rect.bottom() - radius[2].height());
    if (!radius[2].isEmpty()) path.arcTo(QRectF(QPointF(rect.right() - 2 * radius[2].width(), rect.bottom() - 2 * radius[2].height()), 2 * radius[2]), 0, -90);
    else path.lineTo(rect.bottomRight());
    path.lineTo(rect.left() + radius[3].width(), rect.bottom());
    if (!radius[3].isEmpty()) path.arcTo(QRectF(QPointF(rect.left(), rect.bottom() - 2 * radius[3].height()), 2 * radius[3]), 270, -90);
    else path.lineTo(rect.bottomLeft());
    path.lineTo(rect.left(), rect.top() + radius[0].height());
    if (!radius[0].isEmpty()) path.arcTo(QRectF(rect.topLeft(), 2 * radius[0]), 180, -90);
    else path.lineTo(rect.topLeft());
    path.closeSubpath();
    return path;
}

void paintVisual(QPainter* painter, QRect rect, const ControlVisual& visual,
                 bool margins = false)
{
    if (margins) rect = rect.marginsRemoved(visual.margin);
    if (rect.isEmpty()) return;
    painter->save();
    const QRectF bounds(rect);
    const auto radii = normalizedRadii(bounds, visual.radii);
    const bool rounded = std::any_of(radii.begin(), radii.end(), [](const QSizeF& radius) { return !radius.isNull(); });
    painter->setRenderHint(QPainter::Antialiasing, rounded);
    const QPainterPath outer = roundedPath(bounds, radii);
    if (visual.background.isValid()) {
        const QMarginsF inset(visual.borders.left() / 2.0, visual.borders.top() / 2.0,
                              visual.borders.right() / 2.0, visual.borders.bottom() / 2.0);
        painter->fillPath(rounded ? roundedPath(bounds, radii, inset) : outer, visual.background);
    }
    if (!visual.borders.isNull()) {
        painter->setRenderHint(QPainter::Antialiasing);
        const std::array<int, 4> widths = {visual.borders.left(), visual.borders.top(),
                                          visual.borders.right(), visual.borders.bottom()};
        const std::array<std::array<int, 2>, 4> corners = {{{0, 3}, {0, 1}, {1, 2}, {3, 2}}};
        const std::array<std::array<int, 2>, 4> starts = {{{135, 180}, {135, 45}, {45, 315}, {-90, -90}}};
        const std::array<std::array<int, 2>, 4> spans = {{{45, 45}, {-45, 45}, {-45, 45}, {-45, 45}}};
        for (const auto side : {3, 2, 0, 1}) {
            const qreal width = widths[side];
            const auto color = visual.borderColors[side];
            if (width <= 0 || !color.isValid()) continue;
            const bool vertical = side == 0 || side == 2;
            const auto firstRadius = radii[corners[side][0]];
            const auto secondRadius = radii[corners[side][1]];
            QRectF strip = bounds;
            if (vertical) {
                strip.setTop(bounds.top() + firstRadius.height());
                strip.setBottom(bounds.bottom() - secondRadius.height());
                if (side == 0) strip.setRight(bounds.left() + width);
                else strip.setLeft(bounds.right() - width);
            } else {
                strip.setLeft(bounds.left() + firstRadius.width());
                strip.setRight(bounds.right() - secondRadius.width());
                if (side == 1) strip.setBottom(bounds.top() + width);
                else strip.setTop(bounds.bottom() - width);
            }
            const auto joinInset = [&](int neighbor, qreal radius) -> qreal {
                const auto adjacent = visual.borderColors[neighbor];
                if (radius > 0 || !adjacent.isValid() || adjacent == QColor(Qt::transparent)
                    || (adjacent == color && color.alpha() == 255)) return 0;
                return widths[neighbor];
            };
            const qreal first = joinInset(vertical ? 1 : 0,
                                           vertical ? firstRadius.height() : firstRadius.width());
            const qreal second = joinInset(vertical ? 3 : 2,
                                            vertical ? secondRadius.height() : secondRadius.width());
            painter->setPen(Qt::NoPen);
            painter->setBrush(color);
            if (width == 1 || (first == 0 && second == 0)) {
                painter->drawRect(strip);
            } else {
                QPolygonF polygon{strip.topLeft(), strip.topRight(), strip.bottomRight(), strip.bottomLeft()};
                if (side == 0) {
                    polygon[1].ry() += first;
                    polygon[2].ry() -= second;
                } else if (side == 1) {
                    polygon[3].rx() += first;
                    polygon[2].rx() -= second;
                } else if (side == 2) {
                    polygon[0].ry() += first;
                    polygon[3].ry() -= second;
                } else {
                    polygon[0].rx() += first;
                    polygon[1].rx() -= second;
                }
                painter->drawConvexPolygon(polygon);
            }
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(color, width, Qt::SolidLine, Qt::SquareCap));
            for (int end = 0; end < 2; ++end) {
                const int corner = corners[side][end];
                const auto radius = radii[corner];
                if (radius.isEmpty()) continue;
                const qreal x = (corner == 0 || corner == 3)
                    ? bounds.left() : bounds.right() - 2 * radius.width();
                const qreal y = corner < 2 ? bounds.top() : bounds.bottom() - 2 * radius.height();
                painter->drawArc(QRectF(x + width / 2, y + width / 2,
                                        2 * radius.width() - width, 2 * radius.height() - width),
                                 starts[side][end] * 16, spans[side][end] * 16);
            }
        }
    }
    painter->restore();
    if (!visual.image.isEmpty()) {
        const QPixmap pixmap(visual.image);
        if (!pixmap.isNull()) {
            QSize size = pixmap.deviceIndependentSize().toSize();
            size.scale(size.boundedTo(rect.size()), Qt::KeepAspectRatio);
            painter->drawPixmap(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, size, rect), pixmap);
        }
    }
}

void configurePalette(QPalette& palette, const ControlVisual& visual)
{
    if (visual.foreground.isValid()) {
        for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            palette.setColor(role, visual.foreground);
        QColor placeholder = visual.foreground;
        placeholder.setAlpha((placeholder.alpha() + 1) / 2);
        palette.setColor(QPalette::PlaceholderText, placeholder);
    }
    if (visual.alternateBackground.isValid()) palette.setColor(QPalette::AlternateBase, visual.alternateBackground);
    if (visual.selectionBackground.isValid()) palette.setColor(QPalette::Highlight, visual.selectionBackground);
    if (visual.selectionForeground.isValid()) palette.setColor(QPalette::HighlightedText, visual.selectionForeground);
}

bool paintsControlSurface(const QWidget* widget)
{
    if (const auto* toolbar = qobject_cast<const QToolBar*>(widget->parentWidget()))
        for (auto* action : toolbar->actions())
            if (action->isSeparator() && toolbar->widgetForAction(action) == widget) return true;
    return qobject_cast<const QAbstractButton*>(widget) || qobject_cast<const QLineEdit*>(widget)
        || qobject_cast<const QComboBox*>(widget) || qobject_cast<const QAbstractSpinBox*>(widget)
        || qobject_cast<const QAbstractSlider*>(widget) || qobject_cast<const QAbstractScrollArea*>(widget)
        || qobject_cast<const QProgressBar*>(widget) || qobject_cast<const QGroupBox*>(widget)
        || qobject_cast<const QMenu*>(widget) || qobject_cast<const QTabWidget*>(widget);
}

void refreshPalette(QWidget* widget, ThemeManager& manager, QStyle::State state)
{
    const QWidget* owner = widget;
    if (const auto* scrollArea = qobject_cast<const QAbstractScrollArea*>(widget->parentWidget()))
        if (scrollArea->viewport() == widget) owner = scrollArea;
    QPalette palette = widget->palette();
    const auto visual = manager.widgetAppearance().visual(owner, state);
    if (qobject_cast<QComboBox*>(widget))
        manager.widgetAppearance().updateWidgetGeometry(widget,
            manager.widgetAppearance().visual(widget, QStyle::State_Enabled));
    configurePalette(palette, visual);
    if (auto* progress = qobject_cast<QProgressBar*>(widget))
        if (visual.textAlignment != Qt::Alignment() && progress->alignment() != visual.textAlignment)
            progress->setAlignment(visual.textAlignment);
    if (qobject_cast<const QHeaderView*>(owner)) {
        const auto section = manager.widgetAppearance().visual(owner, state, u"section");
        configurePalette(palette, section);
        const QFont font = section.font.resolve(widget->font());
        if (font != widget->font()) widget->setFont(font);
    }
    if (qobject_cast<const QGroupBox*>(owner)) {
        const auto title = manager.widgetAppearance().visual(owner, state, u"title");
        if (title.foreground.isValid()) palette.setColor(QPalette::WindowText, title.foreground);
    }
    if (visual.background.isValid()) {
        palette.setColor(QPalette::Window, visual.background);
        palette.setColor(QPalette::Base, visual.background);
        palette.setColor(QPalette::Button, visual.background);
    }
    const auto disabled = manager.widgetAppearance().visual(owner, state & ~QStyle::State_Enabled);
    if (disabled.foreground.isValid())
        for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            palette.setColor(QPalette::Disabled, role, disabled.foreground);
    if (disabled.background.isValid()) {
        palette.setColor(QPalette::Disabled, QPalette::Base, disabled.background);
        palette.setColor(QPalette::Disabled, QPalette::Window, disabled.background);
        palette.setColor(QPalette::Disabled, QPalette::Button, disabled.background);
    }
    if (palette != widget->palette()) widget->setPalette(palette);
}

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
        } else {
            if (!editor_->validator() && editor_->inputMask().isEmpty()) return;
            if (auto* combo = qobject_cast<QComboBox*>(editor_->parentWidget()))
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

void HarkenStyle::refreshWidgetAppearance(QWidget* widget, ThemeManager& manager)
{
    refreshPalette(widget, manager, widgetState(widget));
}

void HarkenStyle::scheduleAppearanceRefresh(QWidget* widget)
{
    const bool scheduled = !pendingRefreshes_.isEmpty();
    pendingRefreshes_.insert(widget, widget);
    if (scheduled) return;
    QTimer::singleShot(0, this, [this] {
        const auto pending = std::exchange(pendingRefreshes_, {});
        for (const auto& root : pending) {
            if (!root) continue;
            bool covered = false;
            for (auto* parent = root->parentWidget(); parent; parent = parent->parentWidget())
                if (pending.value(parent) == parent) {
                    covered = true;
                    break;
                }
            if (covered) continue;
            auto widgets = root->findChildren<QWidget*>();
            widgets.prepend(root);
            for (auto* widget : widgets)
                if (widget->testAttribute(Qt::WA_WState_Polished)) {
                    refreshWidgetAppearance(widget, theme());
                    widget->update();
                }
        }
    });
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
    if (widget->metaObject() == &QWidget::staticMetaObject
        || qobject_cast<QHeaderView*>(widget) || qobject_cast<QTabBar*>(widget)
        || qobject_cast<QFrame*>(widget) || qobject_cast<QMainWindow*>(widget)
        || qobject_cast<QMdiSubWindow*>(widget) || qobject_cast<QMenuBar*>(widget)
        || qobject_cast<QDialog*>(widget) || qobject_cast<QToolBar*>(widget))
        widget->setAttribute(Qt::WA_StyledBackground);
    widget->setAttribute(Qt::WA_Hover);
    widget->installEventFilter(this);
    if (theme().widgetAppearance().synchronize(widget))
        scheduleAppearanceRefresh(widget);
    if (!observedWidgets_.contains(widget)) {
        observedWidgets_.insert(widget);
        connect(widget, &QObject::objectNameChanged, this, [this, widget] {
            scheduleAppearanceRefresh(widget);
        });
        connect(widget, &QObject::destroyed, this, [this, widget] { observedWidgets_.remove(widget); });
        if (auto* button = qobject_cast<QAbstractButton*>(widget))
            connect(button, &QAbstractButton::toggled, this, [this, widget] {
                refreshWidgetAppearance(widget, theme());
                widget->update();
            });
    }
    refreshWidgetAppearance(widget, theme());
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
}

void HarkenStyle::unpolish(QWidget* widget)
{
    widget->removeEventFilter(this);
    QProxyStyle::unpolish(widget);
}

bool HarkenStyle::eventFilter(QObject* watched, QEvent* event)
{
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget) {
        bool refresh = false;
        QStyle::State state = widgetState(widget);
        switch (event->type()) {
        case QEvent::DynamicPropertyChange:
            if (const auto name = static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName();
                !theme().widgetAppearance().usesProperty(widget, name)) break;
            [[fallthrough]];
        case QEvent::ParentChange:
            scheduleAppearanceRefresh(widget);
            break;
        case QEvent::StyleChange:
            if (theme().widgetAppearance().takeDependencyChanges(widget))
                scheduleAppearanceRefresh(widget);
            refresh = true;
            break;
        case QEvent::Enter:
        case QEvent::HoverEnter:
            state |= State_MouseOver;
            refresh = true;
            break;
        case QEvent::Leave:
        case QEvent::HoverLeave:
            state &= ~State_MouseOver;
            refresh = true;
            break;
        case QEvent::FocusIn:
            state |= State_HasFocus;
            refresh = true;
            break;
        case QEvent::FocusOut:
            state &= ~State_HasFocus;
            refresh = true;
            break;
        case QEvent::MouseButtonPress:
            if (qobject_cast<QAbstractButton*>(widget)) { state |= State_Sunken; refresh = true; }
            break;
        case QEvent::MouseButtonRelease:
            if (qobject_cast<QAbstractButton*>(widget)) { state &= ~State_Sunken; refresh = true; }
            break;
        case QEvent::EnabledChange:
        case QEvent::Show:
            refresh = true;
            break;
        default:
            break;
        }
        if (refresh) {
            refreshPalette(widget, theme(), state);
            widget->update();
        }
    }
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
    if (hint == SH_Table_GridLineColor && widget) {
        const auto visual = theme().widgetAppearance().visual(widget, option ? option->state : widgetState(widget));
        if (visual.gridline.isValid()) return static_cast<int>(visual.gridline.rgba());
    }
    if (hint == SH_UnderlineShortcut || hint == SH_ComboBox_Popup)
        return 0;
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

int HarkenStyle::pixelMetric(PixelMetric metric, const QStyleOption* option,
                            const QWidget* widget) const
{
    if (metric == PM_DefaultFrameWidth && widget) {
        const auto visual = theme().widgetAppearance().visual(widget, option ? option->state : widgetState(widget));
        if (visual.borderSpecified) return 0;
    }
    if (metric == PM_ButtonDefaultIndicator || metric == PM_ButtonShiftHorizontal || metric == PM_ButtonShiftVertical)
        return 0;
    if (metric == PM_TabBarTabHSpace || metric == PM_TabBarTabVSpace
        || metric == PM_TabBarTabShiftHorizontal || metric == PM_TabBarTabShiftVertical)
        return 0;
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
    if (type == CT_ComboBox && widget) {
        if (const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option)) {
            const auto visual = theme().widgetAppearance().visual(widget, option->state);
            QSize size = contentsSize;
            if (visual.size.width() >= 0) size.setWidth(visual.size.width());
            if (visual.size.height() >= 0) size.setHeight(visual.size.height());
            if (visual.maximum.width() >= 0) size.setWidth(std::min(size.width(), visual.maximum.width()));
            if (visual.maximum.height() >= 0) size.setHeight(std::min(size.height(), visual.maximum.height()));
            size = size.expandedTo(visual.minimum);
            const auto insets = visual.margin + visual.borders + visual.padding;
            return size + QSize(insets.left() + insets.right()
                                  + subControlRect(CC_ComboBox, combo, SC_ComboBoxArrow, widget).width() + 2,
                                insets.top() + insets.bottom());
        }
    }
    if (type == CT_HeaderSection && widget) {
        if (const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option)) {
            QStyleOptionHeader copy = *header;
            const auto section = theme().widgetAppearance().visual(widget, option->state, u"section", appearanceStates(option));
            copy.fontMetrics = QFontMetrics(section.font.resolve(widget->font()));
            return QProxyStyle::sizeFromContents(type, &copy, contentsSize, widget);
        }
    }
    if (type == CT_TabBarTab && widget) {
        const auto visual = theme().widgetAppearance().visual(widget, option->state, u"tab");
        const auto margins = visual.padding + visual.borders + visual.margin;
        QSize size = contentsSize + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
        return size.expandedTo(visual.minimum);
    }
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
    if (element == SE_ProgressBarContents && widget) {
        const auto visual = theme().widgetAppearance().visual(widget, option->state);
        const auto chunk = theme().widgetAppearance().visual(widget, option->state, u"chunk");
        if (visual.borderSpecified || chunk.background.isValid())
            return option->rect.marginsRemoved(visual.margin + visual.borders + visual.padding);
    }
    if (widget && (element == SE_LineEditContents || element == SE_FrameContents || element == SE_ShapedFrameContents)) {
        const auto visual = theme().widgetAppearance().visual(widget, option ? option->state : widgetState(widget));
        if (visual.borderSpecified)
            return visualRect(option->direction, option->rect, option->rect.marginsRemoved(visual.borders + visual.padding));
    }
    if (element == SE_TabBarTabText && widget) {
        const auto visual = theme().widgetAppearance().visual(widget, option->state, u"tab");
        if (const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option)) {
            QStyleOptionTab copy = *tab;
            copy.rect = option->rect.marginsRemoved(visual.padding + visual.borders + visual.margin);
            return QProxyStyle::subElementRect(element, &copy, widget);
        }
    }
    if (widget && (element == SE_ItemViewItemText || element == SE_ItemViewItemDecoration
        || element == SE_ItemViewItemCheckIndicator || element == SE_ItemViewItemFocusRect)) {
        if (const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option)) {
            const auto visual = theme().widgetAppearance().visual(widget, option->state, u"item", appearanceStates(option));
            QStyleOptionViewItem copy = *item;
            copy.rect = option->rect.marginsRemoved(visual.padding + visual.borders);
            return QProxyStyle::subElementRect(element, &copy, widget);
        }
    }
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
    if (control == CC_Slider && widget) {
        if (const auto* slider = qstyleoption_cast<const QStyleOptionSlider*>(option)) {
            const auto states = appearanceStates(option);
            const auto groove = theme().widgetAppearance().visual(widget, option->state, u"groove", states);
            const auto handle = theme().widgetAppearance().visual(widget, option->state, u"handle", states);
            if (groove.size.width() >= 0 || groove.size.height() >= 0) {
                const bool horizontal = slider->orientation == Qt::Horizontal;
                const QRect area = option->rect;
                const QSize grooveSize(groove.size.width() >= 0 ? groove.size.width() : area.width(),
                                       groove.size.height() >= 0 ? groove.size.height() : area.height());
                const QRect grooveRect = alignedRect(option->direction, Qt::AlignCenter, grooveSize, area);
                if (subControl == SC_SliderGroove) return grooveRect;
                if (subControl == SC_SliderHandle) {
                    const QSize handleSize(handle.size.width() >= 0 ? handle.size.width() : grooveRect.width(),
                                           handle.size.height() >= 0 ? handle.size.height() : grooveRect.height());
                    const int length = horizontal ? handleSize.width() : handleSize.height();
                    const int available = (horizontal ? area.width() : area.height()) - length;
                    const int position = sliderPositionFromValue(slider->minimum, slider->maximum, slider->sliderPosition,
                                                                 std::max(0, available), slider->upsideDown);
                    return horizontal
                        ? QRect(area.left() + position, grooveRect.center().y() - handleSize.height() / 2 + 1,
                                handleSize.width(), handleSize.height())
                        : QRect(grooveRect.center().x() - handleSize.width() / 2 + 1, area.top() + position,
                                handleSize.width(), handleSize.height());
                }
            }
        }
    }
    if (control == CC_ComboBox) {
        const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
        if (combo && widget) {
            const auto visual = theme().widgetAppearance().visual(widget, option->state);
            if (subControl == SC_ComboBoxFrame)
                return combo->rect.marginsRemoved(visual.margin);
            if (subControl == SC_ComboBoxArrow || subControl == SC_ComboBoxEditField) {
                const auto dropdown = theme().widgetAppearance().visual(widget, option->state, u"drop-down");
                const QRect inner = combo->rect.marginsRemoved(visual.margin + visual.borders);
                const auto insets = dropdown.margin + dropdown.borders + dropdown.padding;
                const QSize size = QSize(dropdown.size.width() >= 0
                                           ? dropdown.size.width() + insets.left() + insets.right() : 16,
                                         dropdown.size.height() >= 0
                                           ? dropdown.size.height() + insets.top() + insets.bottom() : inner.height())
                    .expandedTo(dropdown.minimum);
                if (subControl == SC_ComboBoxArrow)
                    return alignedRect(combo->direction, Qt::AlignRight | Qt::AlignTop, size, inner);
                const QRect contents = inner.marginsRemoved(visual.padding);
                return visualRect(combo->direction, contents, contents.adjusted(0, 0, -size.width(), 0));
            }
        }
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

QStyle::SubControl HarkenStyle::hitTestComplexControl(ComplexControl control,
                                                     const QStyleOptionComplex* option,
                                                     const QPoint& point, const QWidget* widget) const
{
    if (control == CC_ComboBox && widget) {
        if (qstyleoption_cast<const QStyleOptionComboBox*>(option)) {
            for (const auto part : {SC_ComboBoxArrow, SC_ComboBoxEditField, SC_ComboBoxFrame})
                if (subControlRect(control, option, part, widget).contains(point)) return part;
            return SC_None;
        }
    }
    return QProxyStyle::hitTestComplexControl(control, option, point, widget);
}

void HarkenStyle::drawPrimitive(PrimitiveElement element,
                                const QStyleOption* option, QPainter* painter,
                                const QWidget* widget) const
{
    if (widget) {
        QStringView part;
        bool surface = false;
        bool margins = false;
        switch (element) {
        case PE_Widget:
            if (const auto* scroll = qobject_cast<const QAbstractScrollArea*>(widget->parentWidget());
                scroll && scroll->viewport() == widget) {
                if (widget->autoFillBackground()) return;
                auto visual = theme().widgetAppearance().visual(scroll, option->state);
                visual.borders = {};
                visual.radii.fill(QSizeF(0, 0));
                paintVisual(painter, option->rect, visual);
                return;
            }
            if (paintsControlSurface(widget)) return;
            surface = true;
            margins = true;
            break;
        case PE_PanelButtonCommand:
        case PE_PanelButtonTool:
        case PE_PanelLineEdit:
        case PE_FrameLineEdit:
        case PE_FrameGroupBox:
        case PE_PanelMenu:
        case PE_FrameWindow:
            surface = true;
            break;
        case PE_PanelMenuBar:
            if (widget->testAttribute(Qt::WA_StyledBackground)) return;
            surface = true;
            break;
        case PE_Frame:
        case PE_FrameDockWidget:
            if (!theme().widgetAppearance().visual(widget, option->state).borderSpecified) break;
            if (widget->testAttribute(Qt::WA_StyledBackground) && !paintsControlSurface(widget)) return;
            surface = true;
            break;
        case PE_IndicatorDockWidgetResizeHandle:
            part = u"separator";
            surface = true;
            break;
        case PE_FrameTabWidget:
            part = u"pane";
            surface = true;
            break;
        case PE_FrameFocusRect:
        case PE_FrameDefaultButton:
        case PE_FrameTabBarBase:
        case PE_FrameStatusBarItem:
            return;
        case PE_FrameMenu:
            if (qobject_cast<const QToolBar*>(widget)) {
                auto visual = theme().widgetAppearance().visual(widget, option->state);
                visual.background = QColor();
                paintVisual(painter, option->rect, visual);
            }
            return;
        case PE_IndicatorCheckBox:
        case PE_IndicatorItemViewItemCheck:
            part = u"indicator";
            surface = true;
            break;
        case PE_PanelItemViewItem:
        case PE_PanelItemViewRow: {
            const auto visual = theme().widgetAppearance().visual(widget, option->state, u"item", appearanceStates(option));
            if (visual.background.isValid()) paintVisual(painter, option->rect, visual);
            else if (const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option)) {
                if (item->state & State_Selected) painter->fillRect(item->rect, item->palette.brush(QPalette::Highlight));
                else if (item->backgroundBrush.style() != Qt::NoBrush) painter->fillRect(item->rect, item->backgroundBrush);
                else if (item->features & QStyleOptionViewItem::Alternate) painter->fillRect(item->rect, item->palette.brush(QPalette::AlternateBase));
            }
            return;
        }
        default:
            break;
        }
        if (surface) {
            const auto visual = theme().widgetAppearance().visual(widget, option->state, part, appearanceStates(option));
            paintVisual(painter, option->rect, visual, margins);
            return;
        }
    }
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
    if (!widget) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }
    const auto visual = theme().widgetAppearance().visual(widget, option->state, {}, appearanceStates(option));
    switch (element) {
    case CE_CheckBox:
    case CE_RadioButton:
        paintVisual(painter, option->rect, visual);
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    case CE_ShapedFrame:
        if (!visual.borderSpecified) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }
        if (const auto* scroll = qobject_cast<const QAbstractScrollArea*>(widget)) {
            painter->save();
            painter->setClipRegion(QRegion(option->rect).subtracted(QRegion(scroll->viewport()->geometry())), Qt::IntersectClip);
            paintVisual(painter, option->rect, visual);
            painter->restore();
        }
        else if (!widget->testAttribute(Qt::WA_StyledBackground)) paintVisual(painter, option->rect, visual);
        return;
    case CE_PushButtonBevel:
        paintVisual(painter, option->rect, visual);
        if (const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option)) {
            if (button->features & QStyleOptionButton::HasMenu) {
                QStyleOption arrow = *option;
                const int extent = theme().metrics().navigationChevronExtent;
                arrow.rect = QRect(option->rect.right() - extent, option->rect.center().y() - extent / 2, extent, extent);
                drawPrimitive(PE_IndicatorArrowDown, &arrow, painter, widget);
            }
        }
        return;
    case CE_PushButtonLabel:
    case CE_CheckBoxLabel:
    case CE_RadioButtonLabel:
        if (const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option)) {
            QStyleOptionButton copy = *button;
            configurePalette(copy.palette, visual);
            if (element == CE_PushButtonLabel && visual.textAlignment != Qt::Alignment()) {
                QRect textRect = copy.rect;
                if (!copy.icon.isNull()) {
                    const QRect iconRect = alignedRect(copy.direction, Qt::AlignLeft | Qt::AlignVCenter,
                                                        copy.iconSize, textRect);
                    copy.icon.paint(painter, iconRect, Qt::AlignCenter,
                                    copy.state & State_Enabled ? QIcon::Normal : QIcon::Disabled,
                                    copy.state & State_On ? QIcon::On : QIcon::Off);
                    textRect.adjust(copy.iconSize.width() + 4, 0, 0, 0);
                }
                drawItemText(painter, textRect, visual.textAlignment | Qt::AlignVCenter | Qt::TextHideMnemonic,
                             copy.palette, copy.state & State_Enabled, copy.text, QPalette::ButtonText);
            } else {
                QProxyStyle::drawControl(element, &copy, painter, widget);
            }
            return;
        }
        break;
    case CE_ToolButtonLabel:
        if (const auto* button = qstyleoption_cast<const QStyleOptionToolButton*>(option)) {
            QStyleOptionToolButton copy = *button;
            configurePalette(copy.palette, visual);
            if (visual.textAlignment != Qt::Alignment() && copy.toolButtonStyle == Qt::ToolButtonTextOnly) {
                drawItemText(painter, copy.rect, visual.textAlignment | Qt::AlignVCenter | Qt::TextHideMnemonic,
                             copy.palette, copy.state & State_Enabled, copy.text, QPalette::ButtonText);
            } else {
                QProxyStyle::drawControl(element, &copy, painter, widget);
            }
            return;
        }
        break;
    case CE_ComboBoxLabel:
        if (const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option)) {
            QStyleOptionComboBox copy = *combo;
            configurePalette(copy.palette, visual);
            QProxyStyle::drawControl(element, &copy, painter, widget);
            return;
        }
        break;
    case CE_HeaderSection: {
        const auto section = theme().widgetAppearance().visual(widget, option->state, u"section", appearanceStates(option));
        if (section.background.isValid() || section.borderSpecified) paintVisual(painter, option->rect, section);
        else QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }
    case CE_HeaderLabel:
        if (const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option)) {
            QStyleOptionHeader copy = *header;
            const auto section = theme().widgetAppearance().visual(widget, option->state, u"section", appearanceStates(option));
            configurePalette(copy.palette, section);
            QFont font = section.font.resolve(painter->font());
            font.setResolveMask(font.resolveMask() | section.font.resolveMask());
            copy.fontMetrics = QFontMetrics(font);
            painter->save();
            painter->setFont(font);
            QProxyStyle::drawControl(element, &copy, painter, widget);
            painter->restore();
            return;
        }
        break;
    case CE_HeaderEmptyArea:
        paintVisual(painter, option->rect, theme().widgetAppearance().visual(widget, option->state, u"section", {QStringLiteral("last")}));
        return;
    case CE_ItemViewItem:
        if (const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option)) {
            QStyleOptionViewItem copy = *item;
            const auto itemVisual = theme().widgetAppearance().visual(widget, option->state, u"item", appearanceStates(option));
            if (itemVisual.foreground.isValid())
                copy.palette.setColor(copy.state & State_Selected ? QPalette::HighlightedText : QPalette::Text,
                                      itemVisual.foreground);
            QProxyStyle::drawControl(element, &copy, painter, widget);
            return;
        }
        break;
    case CE_TabBarTab:
        drawControl(CE_TabBarTabShape, option, painter, widget);
        drawControl(CE_TabBarTabLabel, option, painter, widget);
        return;
    case CE_TabBarTabShape:
        paintVisual(painter, option->rect, theme().widgetAppearance().visual(widget, option->state, u"tab"), true);
        return;
    case CE_TabBarTabLabel:
        if (const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option)) {
            QStyleOptionTab copy = *tab;
            configurePalette(copy.palette, theme().widgetAppearance().visual(widget, option->state, u"tab"));
            QProxyStyle::drawControl(element, &copy, painter, widget);
            return;
        }
        break;
    case CE_MenuItem:
        if (const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option)) {
            const bool separator = item->menuItemType == QStyleOptionMenuItem::Separator;
            const auto itemVisual = theme().widgetAppearance().visual(widget, option->state,
                                                                       separator ? u"separator" : u"item");
            if (separator) {
                paintVisual(painter, option->rect, itemVisual);
            } else {
                QStyleOptionMenuItem copy = *item;
                configurePalette(copy.palette, itemVisual);
                if ((copy.state & State_Selected) && itemVisual.background.isValid())
                    copy.palette.setColor(QPalette::Highlight, itemVisual.background);
                if ((copy.state & State_Selected) && itemVisual.foreground.isValid())
                    copy.palette.setColor(QPalette::HighlightedText, itemVisual.foreground);
                QtControlSkin(theme().snapshot()).paintPopupItem(*painter, copy, widget);
            }
            return;
        }
        break;
    case CE_MenuBarItem:
        if (const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option)) {
            const auto itemVisual = theme().widgetAppearance().visual(widget, option->state, u"item");
            paintVisual(painter, option->rect.marginsAdded(itemVisual.padding + itemVisual.borders), itemVisual);
            QStyleOptionMenuItem copy = *item;
            configurePalette(copy.palette, itemVisual);
            QCommonStyle::drawControl(element, &copy, painter, widget);
            return;
        }
        break;
    case CE_ToolBar:
        if (!widget->testAttribute(Qt::WA_StyledBackground)) paintVisual(painter, option->rect, visual);
        return;
    case CE_MenuBarEmptyArea:
    case CE_MenuEmptyArea:
        return;
    case CE_SizeGrip:
        if (visual.background.isValid() || visual.borderSpecified || !visual.image.isNull()) {
            paintVisual(painter, option->rect, visual);
            return;
        }
        break;
    case CE_Splitter: {
        const QWidget* owner = qobject_cast<const QSplitterHandle*>(widget) ? widget->parentWidget() : widget;
        paintVisual(painter, option->rect, theme().widgetAppearance().visual(owner, option->state, u"handle"));
        return;
    }
    case CE_ProgressBarGroove:
        if (widget->property("shellRole") == QLatin1String("statusProgress")) paintVisual(painter, option->rect, visual);
        else QProxyStyle::drawControl(element, option, painter, widget);
        return;
    case CE_ProgressBarContents:
        if (const auto* progress = qstyleoption_cast<const QStyleOptionProgressBar*>(option)) {
            const auto chunk = theme().widgetAppearance().visual(widget, option->state, u"chunk");
            if (!chunk.background.isValid() || (progress->minimum == 0 && progress->maximum == 0)) {
                QStyleOptionProgressBar copy = *progress;
                if (chunk.background.isValid()) copy.palette.setColor(QPalette::Highlight, chunk.background);
                QProxyStyle::drawControl(element, &copy, painter, widget);
                return;
            }
            QRect rect = progress->rect;
            const qint64 range = qint64(progress->maximum) - progress->minimum;
            const qreal fraction = range > 0
                ? std::clamp(qreal(qint64(progress->progress) - progress->minimum) / range, qreal(0), qreal(1)) : 0.5;
            const bool horizontal = progress->state & State_Horizontal;
            if (horizontal) {
                const int extent = static_cast<int>(rect.width() * fraction);
                const bool reverse = progress->invertedAppearance != (progress->direction == Qt::RightToLeft);
                if (reverse) rect.setLeft(rect.right() - extent + 1); else rect.setWidth(extent);
            } else {
                const int extent = static_cast<int>(rect.height() * fraction);
                if (progress->invertedAppearance) rect.setHeight(extent); else rect.setTop(rect.bottom() - extent + 1);
            }
            paintVisual(painter, rect, chunk);
            return;
        }
        break;
    case CE_ProgressBarLabel:
        if (const auto* progress = qstyleoption_cast<const QStyleOptionProgressBar*>(option)) {
            QStyleOptionProgressBar copy = *progress;
            configurePalette(copy.palette, visual);
            if (visual.textAlignment != Qt::Alignment())
                drawItemText(painter, copy.rect, visual.textAlignment | Qt::TextSingleLine, copy.palette,
                             copy.state & State_Enabled, copy.text, QPalette::Text);
            else QProxyStyle::drawControl(element, &copy, painter, widget);
            return;
        }
        break;
    case CE_ScrollBarSlider:
    case CE_ScrollBarAddPage:
    case CE_ScrollBarSubPage:
    case CE_ScrollBarAddLine:
    case CE_ScrollBarSubLine: {
        const QStringView part = element == CE_ScrollBarSlider ? u"handle" : element == CE_ScrollBarAddPage ? u"add-page"
            : element == CE_ScrollBarSubPage ? u"sub-page" : element == CE_ScrollBarAddLine ? u"add-line" : u"sub-line";
        paintVisual(painter, option->rect, theme().widgetAppearance().visual(widget, option->state, part, appearanceStates(option)), true);
        return;
    }
    default:
        break;
    }
    QProxyStyle::drawControl(element, option, painter, widget);
}

void HarkenStyle::drawComplexControl(ComplexControl control,
                                     const QStyleOptionComplex* option,
                                     QPainter* painter, const QWidget* widget) const
{
    if (!widget) {
        QProxyStyle::drawComplexControl(control, option, painter, widget);
        return;
    }
    const auto visual = theme().widgetAppearance().visual(widget, option->state, {}, appearanceStates(option));
    if (control == CC_ComboBox) {
        if (const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option)) {
            paintVisual(painter, combo->rect, visual, true);
            if (combo->subControls & SC_ComboBoxArrow) {
                QStyleOption arrow = *combo;
                arrow.rect = subControlRect(control, combo, SC_ComboBoxArrow, widget);
                const auto image = theme().widgetAppearance().visual(widget, option->state, u"down-arrow");
                if (!image.image.isEmpty()) paintVisual(painter, arrow.rect, image);
                else drawPrimitive(PE_IndicatorArrowDown, &arrow, painter, widget);
            }
            return;
        }
    }
    if (control == CC_ToolButton) {
        if (const auto* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option)) {
            paintVisual(painter, tool->rect, visual);
            QStyleOptionToolButton label = *tool;
            label.rect = tool->rect.marginsRemoved(visual.padding + visual.borders);
            label.features &= ~(QStyleOptionToolButton::HasMenu | QStyleOptionToolButton::MenuButtonPopup);
            drawControl(CE_ToolButtonLabel, &label, painter, widget);
            if ((tool->features & QStyleOptionToolButton::MenuButtonPopup)
                && (tool->subControls & SC_ToolButtonMenu)) {
                QStyleOption arrow = *tool;
                arrow.rect = widget->style()->subControlRect(control, tool, SC_ToolButtonMenu, widget);
                QStyleOptionToolButton dropdown = *tool;
                dropdown.rect = arrow.rect;
                configurePalette(dropdown.palette, visual);
                if (visual.background.isValid()) {
                    dropdown.palette.setColor(QPalette::Button, visual.background);
                    dropdown.palette.setColor(QPalette::Window, visual.background);
                    dropdown.palette.setColor(QPalette::Light, visual.background.lighter(115));
                    dropdown.palette.setColor(QPalette::Midlight, visual.background.lighter(107));
                    dropdown.palette.setColor(QPalette::Dark, visual.background.darker(150));
                    dropdown.palette.setColor(QPalette::Shadow, visual.background.darker(300));
                }
                QProxyStyle::drawPrimitive(PE_PanelButtonCommand, &dropdown, painter, widget);
                drawPrimitive(PE_IndicatorArrowDown, &arrow, painter, widget);
            } else if (!(tool->features & QStyleOptionToolButton::MenuButtonPopup)
                       && (tool->features & QStyleOptionToolButton::HasMenu)) {
                const auto indicator = theme().widgetAppearance().visual(widget, option->state, u"menu-indicator");
                if (indicator.image.isNull() || !indicator.image.isEmpty()) {
                    QStyleOption arrow = *tool;
                    const QSize size(indicator.size.width() >= 0 ? indicator.size.width() : 13,
                                     indicator.size.height() >= 0 ? indicator.size.height() : 13);
                    arrow.rect = alignedRect(tool->direction, Qt::AlignRight | Qt::AlignBottom,
                                             size, tool->rect.marginsRemoved(visual.borders));
                    if (!indicator.image.isEmpty()) paintVisual(painter, arrow.rect, indicator);
                    else drawPrimitive(PE_IndicatorArrowDown, &arrow, painter, widget);
                }
            }
            return;
        }
    }
    if (control == CC_SpinBox) {
        paintVisual(painter, option->rect, visual);
        return;
    }
    if (control == CC_Slider) {
        if (const auto* slider = qstyleoption_cast<const QStyleOptionSlider*>(option)) {
            const auto states = appearanceStates(option);
            const auto grooveVisual = theme().widgetAppearance().visual(widget, option->state, u"groove", states);
            if (grooveVisual.size.width() < 0 && grooveVisual.size.height() < 0) {
                paintVisual(painter, option->rect, visual);
                QProxyStyle::drawComplexControl(control, option, painter, widget);
                return;
            }
            paintVisual(painter, option->rect, visual);
            const QRect groove = widget->style()->subControlRect(control, option, SC_SliderGroove, widget);
            const QRect handle = widget->style()->subControlRect(control, option, SC_SliderHandle, widget);
            if (slider->subControls & SC_SliderGroove) {
                paintVisual(painter, groove, grooveVisual);
                QRect before = groove;
                QRect after = groove;
                if (slider->orientation == Qt::Horizontal) {
                    before.setRight(handle.x() + handle.width() / 2);
                    after.setLeft(handle.x() + handle.width() / 2 + 1);
                } else {
                    before.setBottom(handle.y() + handle.height() / 2);
                    after.setTop(handle.y() + handle.height() / 2 + 1);
                }
                paintVisual(painter, before, theme().widgetAppearance().visual(widget, option->state, u"sub-page", states));
                paintVisual(painter, after, theme().widgetAppearance().visual(widget, option->state, u"add-page", states));
            }
            if (slider->subControls & SC_SliderHandle) {
                auto state = option->state;
                if (!(slider->activeSubControls & SC_SliderHandle)) state &= ~(State_MouseOver | State_Sunken);
                paintVisual(painter, handle, theme().widgetAppearance().visual(widget, state, u"handle", states));
            }
            return;
        }
    }
    if (control == CC_ScrollBar) {
        if (const auto* scroll = qstyleoption_cast<const QStyleOptionSlider*>(option)) {
            paintVisual(painter, option->rect, visual);
            const std::array<std::pair<SubControl, QStringView>, 5> parts = {{
                {SC_ScrollBarSubPage, u"sub-page"}, {SC_ScrollBarAddPage, u"add-page"},
                {SC_ScrollBarSubLine, u"sub-line"}, {SC_ScrollBarAddLine, u"add-line"},
                {SC_ScrollBarSlider, u"handle"}}};
            for (const auto& [subControl, part] : parts) {
                if (!(scroll->subControls & subControl)) continue;
                auto state = scroll->state;
                if (!(scroll->activeSubControls & subControl)) state &= ~(State_MouseOver | State_Sunken);
                const auto partVisual = theme().widgetAppearance().visual(widget, state, part, appearanceStates(option));
                const QRect rect = widget->style()->subControlRect(control, option, subControl, widget);
                paintVisual(painter, rect, partVisual, true);
            }
            return;
        }
    }
    QProxyStyle::drawComplexControl(control, option, painter, widget);
}

}
