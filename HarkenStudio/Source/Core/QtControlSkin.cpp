
#include "QtControlSkin.h"

#include "FontManager.h"
#include "ThemeManager.h"
#include "ThemeAssetCatalog.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QIconEngine>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionMenuItem>
#include <QStyleOptionToolButton>
#include <QTemporaryDir>
#include <QWidget>
#include <QtGlobal>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <typeinfo>
#include <utility>

namespace {

constexpr int LineEditHorizontalMargin = 2;
int popupTextMargin(const QStyleOptionMenuItem& item, const QWidget* widget,
                    const VNX::ThemeMetrics& metrics)
{
    QVariant mode;
    for (auto* owner = widget; owner && !mode.isValid(); owner = owner->parentWidget())
        mode = owner->property("popupIconColumn");
    const bool reserved = mode.isValid()
        ? mode.toInt() == static_cast<int>(VNX::PopupIconColumn::Reserved)
        : qobject_cast<const QMenu*>(widget) != nullptr;
    return reserved || item.menuHasCheckableItems
        ? qMax(metrics.popup.iconColumnWidth, item.maxIconWidth) : metrics.control.textPaddingX;
}

class PopupItemDelegate final : public QStyledItemDelegate
{
public:
    explicit PopupItemDelegate(QComboBox* combo) : QStyledItemDelegate(combo), combo_(combo) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        const QVariant alignment = index.data(Qt::TextAlignmentRole);
        VNX::QtControlSkin::current().paintPopupItem(*painter, menuOption(option, index), combo_,
            alignment.isValid() ? Qt::Alignment(alignment.toInt()) : Qt::AlignLeft);
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const auto menu = menuOption(option, index);
        const QSize contents = menu.fontMetrics.size(Qt::TextSingleLine | Qt::TextShowMnemonic,
                                                     menu.text);
        return VNX::QtControlSkin::current().popupItemSize(menu, contents, combo_);
    }

private:
    QStyleOptionMenuItem menuOption(const QStyleOptionViewItem& option,
                                   const QModelIndex& index) const
    {
        QStyleOptionViewItem item = option;
        initStyleOption(&item, index);
        QStyleOptionMenuItem menu;
        menu.rect = item.rect;
        menu.state = item.state;
        menu.direction = item.direction;
        menu.palette = item.palette;
        menu.font = item.font;
        menu.fontMetrics = item.fontMetrics;
        menu.menuItemType = QStyleOptionMenuItem::Normal;
        menu.icon = item.icon;
        menu.maxIconWidth = item.icon.isNull() ? 0 : combo_->iconSize().width();
        const bool checkCurrent = combo_->property("popupCurrentItemChecked").toBool();
        const bool checkable = item.features.testFlag(QStyleOptionViewItem::HasCheckIndicator);
        menu.menuHasCheckableItems = checkCurrent
            || combo_->property("popupHasCheckableItems").toBool();
        menu.checkType = checkable ? QStyleOptionMenuItem::NonExclusive
            : checkCurrent ? QStyleOptionMenuItem::Exclusive : QStyleOptionMenuItem::NotCheckable;
        menu.checked = checkable ? item.checkState == Qt::Checked
            : checkCurrent && index == combo_->model()->index(
                combo_->currentIndex(), combo_->modelColumn(), combo_->rootModelIndex());
        menu.text = item.text.replace(QLatin1Char('&'), QStringLiteral("&&"));
        return menu;
    }

    QComboBox* combo_;
};

class IconButtonEngine final : public QIconEngine
{
public:
    explicit IconButtonEngine(QIcon icon) : icon_(std::move(icon)) {}
    QIconEngine* clone() const override { return new IconButtonEngine(icon_); }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        icon_.paint(painter, rect, Qt::AlignCenter, neutralMode(mode), state);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return icon_.pixmap(size, neutralMode(mode), state);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        return icon_.pixmap(size, scale, neutralMode(mode), state);
    }
    QSize actualSize(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return icon_.actualSize(size, neutralMode(mode), state);
    }

private:
    static QIcon::Mode neutralMode(QIcon::Mode mode)
    {
        return mode == QIcon::Active ? QIcon::Normal : mode;
    }
    QIcon icon_;
};

QColor contrastTextColor(const QColor& background)
{
    const auto linear = [](qreal channel) {
        return channel <= 0.04045
            ? channel / 12.92
            : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    const qreal luminance = 0.2126 * linear(background.redF())
        + 0.7152 * linear(background.greenF())
        + 0.0722 * linear(background.blueF());
    const qreal foreground = luminance > 0.179 ? 0.0 : 1.0;
    return QColor::fromRgbF(foreground, foreground, foreground);
}

QString styleSheetImagePath(const VNX::ThemeImageResource& resource)
{
    static QTemporaryDir directory(
        QDir::tempPath() + QStringLiteral("/harken-theme-icons-XXXXXX"));
    if (!directory.isValid())
        return {};
    QByteArray bytes = resource.svgData;
    const bool svg = !bytes.isEmpty();
    if (!svg) {
        QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::WriteOnly)
            || !resource.image.save(&buffer, "PNG"))
            return {};
    }
    const QString name = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QString path = directory.filePath(
        name + (svg ? QStringLiteral(".svg") : QStringLiteral("@2x.png")));
    if (!QFile::exists(path)) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            return {};
    }
    return path;
}

}

namespace VNX {

QtControlSkin::QtControlSkin(ThemeSnapshotPtr snapshot,
                             ThemeAssetResolver* assetResolver)
    : snapshot_(std::move(snapshot))
    , assetResolver_(assetResolver)
{
}

QtControlSkin QtControlSkin::current()
{
    auto& manager = ThemeManager::instance();
    return QtControlSkin(manager.snapshot(), &manager.assetResolver());
}

QIcon QtControlSkin::iconButtonIcon(const QIcon& icon)
{
    return icon.isNull() ? icon : QIcon(new IconButtonEngine(icon));
}

void QtControlSkin::setPopupIconColumn(QWidget* widget, PopupIconColumn mode)
{
    setSemanticProperty(widget, "popupIconColumn", static_cast<int>(mode));
    widget->updateGeometry();
}

void QtControlSkin::setPopupCurrentItemChecked(QComboBox* combo, bool checked)
{
    setSemanticProperty(combo, "popupCurrentItemChecked", checked);
    combo->view()->doItemsLayout();
}

void QtControlSkin::applyPopupStyle(QComboBox* combo)
{
    const QByteArray name = combo->itemDelegate()->metaObject()->className();
    if (name == "QComboBoxDelegate" || name == "QComboMenuDelegate"
        || typeid(*combo->itemDelegate()) == typeid(QStyledItemDelegate))
        combo->setItemDelegate(new PopupItemDelegate(combo));
}

int QtControlSkin::popupRowHeight(const QFontMetrics& fontMetrics) const
{
    return std::max({fontMetrics.height(), metrics().iconExtent, metrics().iconExtent})
        + 2 * metrics().popup.rowPadding;
}

QSize QtControlSkin::popupItemSize(const QStyleOptionMenuItem& item,
                                 const QSize& contentsSize, const QWidget* widget) const
{
    const int spacing = item.text.contains(QLatin1Char('\t')) ? metrics().popup.shortcutSpacing : 0;
    const int margin = popupTextMargin(item, widget, metrics());
    const int trailingMargin = item.menuItemType == QStyleOptionMenuItem::SubMenu
        ? qMax(margin, metrics().iconExtent + metrics().spacingXSmall) : margin;
    const int contentWidth = qobject_cast<const QMenu*>(widget)
        ? qMax(metrics().popup.minimumWidth, contentsSize.width() + spacing) : contentsSize.width() + spacing;
    return QSize(contentWidth + margin + trailingMargin, popupRowHeight(item.fontMetrics));
}

void QtControlSkin::paintPopupItem(QPainter& painter, const QStyleOptionMenuItem& item,
                                  const QWidget* widget, Qt::Alignment alignment) const
{
    const QStyle* style = widget ? widget->style() : QApplication::style();
    painter.save();
    const bool enabled = item.state & QStyle::State_Enabled;
    const bool selected = enabled && (item.state & QStyle::State_Selected);
    if (selected)
        painter.fillRect(item.rect, item.palette.brush(QPalette::Highlight));
    const QColor textColor = item.palette.color(
        enabled ? QPalette::Active : QPalette::Disabled,
        selected ? QPalette::HighlightedText : QPalette::ButtonText);
    painter.setFont(item.font);
    painter.setPen(textColor);

    const int margin = popupTextMargin(item, widget, metrics());
    const int trailingMargin = item.menuItemType == QStyleOptionMenuItem::SubMenu
        ? qMax(margin, metrics().iconExtent + metrics().spacingXSmall) : margin;
    QRect textRect = item.rect.adjusted(margin, 0, -trailingMargin, 0);
    const QStringView text(item.text);
    const qsizetype tab = text.indexOf(QLatin1Char('\t'));
    const int textFlags = Qt::AlignVCenter | Qt::TextSingleLine | Qt::TextHideMnemonic;
    if (tab >= 0) {
        const QString shortcut = text.mid(tab + 1).toString();
        const int shortcutWidth = qMax(item.reservedShortcutWidth,
                                       painter.fontMetrics().horizontalAdvance(shortcut));
        QRect shortcutRect = textRect;
        shortcutRect.setLeft(shortcutRect.right() - shortcutWidth + 1);
        painter.drawText(QStyle::visualRect(item.direction, item.rect, shortcutRect),
                         textFlags | QStyle::visualAlignment(item.direction, Qt::AlignRight),
                         shortcut);
        textRect.setRight(shortcutRect.left() - metrics().popup.shortcutSpacing - 1);
    }
    painter.drawText(QStyle::visualRect(item.direction, item.rect, textRect),
                     textFlags | QStyle::visualAlignment(item.direction, alignment),
                     text.left(tab >= 0 ? tab : text.size()).toString());

    if (margin >= metrics().popup.iconColumnWidth && !item.icon.isNull()) {
        const int extent = qMin(metrics().iconExtent, item.rect.height());
        const QRect iconRect(item.rect.left(), item.rect.top(), margin, item.rect.height());
        const auto state = item.checked ? QIcon::On : QIcon::Off;
        QPixmap glyph = item.icon.pixmap(QSize(extent, extent),
            painter.device()->devicePixelRatioF(), QIcon::Normal, state);
        if (!glyph.isNull()) {
            QPainter tint(&glyph);
            tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(glyph.rect(), textColor);
            tint.end();
            painter.drawPixmap(QStyle::visualRect(item.direction, item.rect,
                QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter,
                                    glyph.deviceIndependentSize().toSize(), iconRect)), glyph);
        }
    } else if (margin >= metrics().popup.iconColumnWidth
               && item.checkType != QStyleOptionMenuItem::NotCheckable && item.checked) {
        QStyleOptionMenuItem check = item;
        check.rect = QStyle::visualRect(item.direction, item.rect,
            QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, QSize(metrics().control.smallIconExtent, metrics().control.smallIconExtent),
                                QRect(item.rect.topLeft(), QSize(margin, item.rect.height()))));
        check.state |= QStyle::State_On;
        check.palette.setColor(QPalette::Text, textColor);
        check.palette.setColor(QPalette::HighlightedText, textColor);
        style->drawPrimitive(QStyle::PE_IndicatorMenuCheckMark, &check, &painter, widget);
    }

    if (item.menuItemType == QStyleOptionMenuItem::SubMenu) {
        const int extent = metrics().iconExtent;
        QStyleOptionMenuItem arrow = item;
        arrow.rect = QStyle::visualRect(item.direction, item.rect,
            QStyle::alignedRect(Qt::LeftToRight, Qt::AlignRight | Qt::AlignVCenter,
                                QSize(extent, extent), item.rect));
        style->drawPrimitive(item.direction == Qt::RightToLeft
            ? QStyle::PE_IndicatorArrowLeft : QStyle::PE_IndicatorArrowRight,
            &arrow, &painter, widget);
    }
    painter.restore();
}

int QtControlSkin::fieldHeight() const
{
    const QFont font = FontManager::instance().getFont(snapshot_, FontRole::Body);
    return std::max({metrics().layout.fieldHeight, metrics().iconExtent + metrics().control.textPaddingY * 2,
                     metrics().iconExtent + metrics().borderWidth * 2,
                     QFontMetrics(font).height() + 2 * (metrics().control.textPaddingY + metrics().borderWidth)});
}

QMargins QtControlSkin::fieldTextMargins() const
{
    const QFont font = FontManager::instance().getFont(snapshot_, FontRole::Body);
    const int vertical = std::max(0, (fieldHeight() - QFontMetrics(font).height()) / 2 - metrics().borderWidth);
    return QMargins(metrics().control.textPaddingX, vertical, metrics().control.textPaddingX, vertical);
}

QIcon QtControlSkin::toolBarIcon(const QStyleOptionToolButton& option, qreal devicePixelRatio) const
{
    const auto state = option.state & QStyle::State_On ? QIcon::On : QIcon::Off;
    QPixmap glyph = option.icon.pixmap(option.iconSize, devicePixelRatio, QIcon::Normal, state);
    if (glyph.isNull())
        return {};
    const QColor color = !(option.state & QStyle::State_Enabled) ? colors().textDisabled
        : (option.state & QStyle::State_On) ? colors().accentText
        : colors().textPrimary;
    QPainter painter(&glyph);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(glyph.rect(), color);
    painter.end();
    QIcon icon;
    for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled})
        icon.addPixmap(glyph, mode, state);
    return icon;
}

QPalette QtControlSkin::applicationPalette() const
{
    const auto& p = snapshot_->qtPalette;
    QPalette palette;
    palette.setColor(QPalette::Window, p.window);
    palette.setColor(QPalette::WindowText, p.windowText);
    palette.setColor(QPalette::Base, p.base);
    palette.setColor(QPalette::AlternateBase, p.alternateBase);
    palette.setColor(QPalette::ToolTipBase, p.toolTipBase);
    palette.setColor(QPalette::ToolTipText, p.toolTipText);
    palette.setColor(QPalette::Text, p.text);
    palette.setColor(QPalette::PlaceholderText, p.placeholderText);
    palette.setColor(QPalette::Button, p.button);
    palette.setColor(QPalette::ButtonText, p.buttonText);
    palette.setColor(QPalette::BrightText, p.brightText);
    palette.setColor(QPalette::Link, p.link);
    palette.setColor(QPalette::Highlight, p.highlight);
    palette.setColor(QPalette::HighlightedText, p.highlightedText);
    palette.setColor(QPalette::Light, p.light);
    palette.setColor(QPalette::Dark, p.dark);
    palette.setColor(QPalette::Mid, p.mid);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, p.disabledText);
    palette.setColor(QPalette::Disabled, QPalette::Text, p.disabledText);
    palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, p.disabledText);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, p.disabledButtonText);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, p.disabledHighlight);
    return palette;
}

void QtControlSkin::setSemanticProperty(QWidget* widget, const char* name,
                                        const QVariant& value)
{
    if (!widget || widget->property(name) == value)
        return;
    widget->setProperty(name, value);
    if (widget->style())
    {
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
    }
    widget->update();
}

int QtControlSkin::tableResizeGripWidth(const QWidget* widget) const
{
    return std::max(metrics().table.resizeHandleRadius,
        widget->style()->pixelMetric(QStyle::PM_HeaderGripMargin, nullptr, widget));
}

QtControlSkin::TableHandleLayout QtControlSkin::tableHandleLayout(
    const QRect& rect, const QWidget* widget, bool hasHandle) const
{
    const auto& table = metrics().table;
    const int grip = tableResizeGripWidth(widget);
    const int handle = hasHandle
        ? std::min(table.compactColumnWidth, std::max(0, rect.width() - grip * 2)) : 0;
    const int padding = std::max(table.cellPaddingX, grip);
    return {
        QRect(rect.x() + grip, rect.y(), handle, rect.height()),
        QRect(rect.x() + padding + handle, rect.y() + table.cellPaddingY,
              std::max(0, rect.width() - padding * 2 - handle),
              std::max(0, rect.height() - table.cellPaddingY * 2))};
}

void QtControlSkin::paintDragHandle(QPainter& painter, const QRect& rect, bool enabled) const
{
    if (rect.width() < 6) return;
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(enabled ? colors().textPrimary : colors().textDisabled);
    const auto center = rect.center();
    for (int x : {-2, 2})
        for (int y : {-4, 0, 4})
            painter.drawEllipse(QPointF(center.x() + x, center.y() + y), 1, 1);
    painter.restore();
}

void QtControlSkin::paintTableCellSelection(QPainter& painter, const QRect& rect) const
{
    painter.fillRect(rect, colors().bgSelected);
    painter.fillRect(QRect(rect.left(), rect.top(), 2, rect.height()), colors().accent);
}

void QtControlSkin::paintFieldFrame(QPainter& painter, const QWidget* widget)
{
    QStyleOption option;
    option.initFrom(widget);
    widget->style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, widget);
}

QString QtControlSkin::applicationStyleSheet() const
{
    QFile file(QStringLiteral(":/Resources/styles/dark_theme.qss"));
    if (!file.open(QFile::ReadOnly | QFile::Text))
        return {};

    QTextStream stream(&file);
    QString styleSheet = stream.readAll();
    ThemeAssetResolver builtinAssets;
    if (!assetResolver_)
        builtinAssets.registerSnapshot(snapshot_, {}, {});
    auto& assets = assetResolver_ ? *assetResolver_ : builtinAssets;
    const auto imagePath = [&](const char* logicalId, int extent, const QColor& tint) {
        const auto image = assets.resolveImage(
            snapshot_, QString::fromLatin1(logicalId),
            ThemeAssetVariant{QSize(extent, extent), 2.0, 0, tint});
        return image.ok() ? styleSheetImagePath(*image.resource) : QString();
    };
    const auto& palette = snapshot_->qtPalette;
    const auto replace = [&styleSheet](const QString& token, const QString& value) {
        if (!value.isEmpty())
            styleSheet.replace(token, value);
    };
    replace(QStringLiteral("@chevronDown@"),
            imagePath(ThemeAssetIds::NavigationChevronDown,
                      metrics().iconExtent, colors().textPrimary));
    replace(QStringLiteral("@chevronDownDisabled@"),
            imagePath(ThemeAssetIds::NavigationChevronDown,
                      metrics().iconExtent, colors().textDisabled));
    replace(QStringLiteral("@checkboxChecked@"),
            imagePath(ThemeAssetIds::CheckboxChecked, metrics().control.checkboxMarkExtent, colors().textPrimary));
    replace(QStringLiteral("@checkboxCheckedDisabled@"),
            imagePath(ThemeAssetIds::CheckboxChecked, metrics().control.checkboxMarkExtent, colors().textDisabled));
    replace(QStringLiteral("@checkboxIndeterminate@"),
            imagePath(ThemeAssetIds::CheckboxIndeterminate, metrics().control.smallIconExtent, colors().textPrimary));
    replace(QStringLiteral("@checkboxIndeterminateDisabled@"),
            imagePath(ThemeAssetIds::CheckboxIndeterminate, metrics().control.smallIconExtent, colors().textDisabled));
    const auto color = [](const QColor& value) {
        return value.name(QColor::HexArgb);
    };

    const auto soft = [](const QColor& value) { return rgba(value, ThemeSoftTintAlpha); };
    const auto hover = [&](const QColor& value) {
        return color(colors().stateColor(value, ThemeStateLayer::Hover));
    };

    replace(QStringLiteral("@accent@"), color(colors().accent));
    replace(QStringLiteral("@accentHover@"), color(colors().accentHover));
    replace(QStringLiteral("@accentText@"), color(colors().accentText));
    replace(QStringLiteral("@bgPrimary@"), color(colors().bgPrimary));
    replace(QStringLiteral("@bgSecondary@"), color(colors().bgSecondary));
    replace(QStringLiteral("@bgTertiary@"), color(colors().bgTertiary));
    replace(QStringLiteral("@bgHover@"), color(colors().bgHover));
    replace(QStringLiteral("@bgSelected@"), color(colors().bgSelected));
    replace(QStringLiteral("@panelBackground@"), color(colors().panelBackground));
    replace(QStringLiteral("@headerBackground@"), color(colors().headerBackground));
    replace(QStringLiteral("@borderPrimary@"), color(colors().borderPrimary));
    replace(QStringLiteral("@borderSecondary@"), color(colors().borderSecondary));
    replace(QStringLiteral("@textPrimary@"), color(colors().textPrimary));
    replace(QStringLiteral("@textSecondary@"), color(colors().textSecondary));
    replace(QStringLiteral("@textTitle@"), color(colors().textTitle));
    replace(QStringLiteral("@textDisabled@"), color(colors().textDisabled));
    replace(QStringLiteral("@sliderTrack@"), color(colors().sliderTrack));
    replace(QStringLiteral("@hoverOverlay@"),
            rgba(colors().textPrimary, qRound(255 * 0.08)));
    replace(QStringLiteral("@success@"), color(colors().success));
    replace(QStringLiteral("@successHover@"), hover(colors().success));
    replace(QStringLiteral("@successSoft@"), soft(colors().success));
    replace(QStringLiteral("@warning@"), color(colors().warning));
    replace(QStringLiteral("@warningHover@"), hover(colors().warning));
    replace(QStringLiteral("@warningSoft@"), soft(colors().warning));
    replace(QStringLiteral("@error@"), color(colors().error));
    replace(QStringLiteral("@errorHover@"), hover(colors().error));
    replace(QStringLiteral("@errorText@"), color(contrastTextColor(colors().error)));
    replace(QStringLiteral("@info@"), color(colors().info));
    replace(QStringLiteral("@compactSliderFill@"), rgba(colors().accent, 85));
    replace(QStringLiteral("@shellCloseHover@"), color(colors().shellCloseHover));
    replace(QStringLiteral("@shellClosePressed@"), color(colors().shellClosePressed));
    replace(QStringLiteral("@shellCloseText@"),
            color(contrastTextColor(colors().shellClosePressed)));
    replace(QStringLiteral("@window@"), color(palette.window));
    replace(QStringLiteral("@base@"), color(palette.base));
    replace(QStringLiteral("@alternateBase@"), color(palette.alternateBase));
    replace(QStringLiteral("@button@"), color(palette.button));
    replace(QStringLiteral("@brightText@"), color(palette.brightText));
    replace(QStringLiteral("@disabledHighlight@"), color(palette.disabledHighlight));
    replace(QStringLiteral("@nodeContentLabel@"), color(snapshot_->domains.nodeGraphNode.contentLabel));
    replace(QStringLiteral("@nodeContentValue@"), color(snapshot_->domains.nodeGraphNode.contentValue));
    replace(QStringLiteral("@logDebug@"), color(snapshot_->domains.log.debug));
    replace(QStringLiteral("@logInfo@"), color(snapshot_->domains.log.info));
    replace(QStringLiteral("@logWarning@"), color(snapshot_->domains.log.warning));
    replace(QStringLiteral("@logError@"), color(snapshot_->domains.log.error));
    replace(QStringLiteral("@logCritical@"), color(snapshot_->domains.log.critical));
    replace(QStringLiteral("@aiBrandAccent@"), color(snapshot_->domains.ai.brandAccent));
    replace(QStringLiteral("@aiBrandAccentSoft@"), soft(snapshot_->domains.ai.brandAccent));
    replace(QStringLiteral("@borderWidth@"), QString::number(metrics().borderWidth));
    replace(QStringLiteral("@accentBorderWidth@"), QString::number(metrics().accentBorderWidth));
    replace(QStringLiteral("@radiusCompact@"), QString::number(metrics().radiusCompact));
    replace(QStringLiteral("@controlHeight@"), QString::number(metrics().controlHeight));
    replace(QStringLiteral("@smallIconExtent@"), QString::number(metrics().control.smallIconExtent));
    replace(QStringLiteral("@iconExtent@"), QString::number(metrics().iconExtent));
    replace(QStringLiteral("@scrollbarExtent@"), QString::number(metrics().control.scrollbarExtent));
    replace(QStringLiteral("@canvasScrollbarExtent@"), QString::number(metrics().control.canvasScrollbarExtent));
    replace(QStringLiteral("@sliderHandleExtent@"), QString::number(metrics().control.sliderHandleExtent));
    replace(QStringLiteral("@sliderGrooveThickness@"), QString::number(metrics().control.sliderGrooveThickness));
    replace(QStringLiteral("@sliderHandleOverhang@"), QString::number((metrics().control.sliderHandleExtent - metrics().control.sliderGrooveThickness) / 2));
    replace(QStringLiteral("@startupProgressHeight@"), QString::number(metrics().startup.progressHeight));
    replace(QStringLiteral("@scrollbarMinimumLength@"), QString::number(metrics().control.scrollbarMinimumLength));
    replace(QStringLiteral("@splitterExtent@"), QString::number(metrics().control.splitterExtent));
    replace(QStringLiteral("@windowButtonWidth@"), QString::number(metrics().chrome.windowButtonWidth));
    replace(QStringLiteral("@titleBarHeight@"), QString::number(metrics().chrome.titleBarHeight));
    replace(QStringLiteral("@compactSliderHeight@"), QString::number(metrics().parameter.compactSliderHeight));
    replace(QStringLiteral("@compactSliderHandleWidth@"), QString::number(metrics().parameter.compactSliderHandleWidth));
    replace(QStringLiteral("@buttonMinimumWidth@"), QString::number(metrics().control.buttonMinimumWidth));
    replace(QStringLiteral("@cardMinimumWidth@"), QString::number(metrics().card.minimumWidth));
    replace(QStringLiteral("@cardMinimumHeight@"), QString::number(metrics().card.minimumHeight));
    replace(QStringLiteral("@radiusSmall@"), QString::number(metrics().radiusSmall));
    replace(QStringLiteral("@radiusMedium@"), QString::number(metrics().radiusMedium));
    replace(QStringLiteral("@radiusLarge@"), QString::number(metrics().radiusLarge));
    replace(QStringLiteral("@popupPaddingY@"), QString::number(metrics().popup.rowPadding));
    replace(QStringLiteral("@fieldHeight@"), QString::number(fieldHeight()));
    const auto& layout = metrics().layout;
    replace(QStringLiteral("@panelPadding@"), QString::number(layout.panelPadding));
    replace(QStringLiteral("@sectionGap@"), QString::number(layout.sectionGap));
    replace(QStringLiteral("@tightGap@"), QString::number(layout.tightGap));
    replace(QStringLiteral("@sectionHeaderHeight@"), QString::number(layout.sectionHeaderHeight));
    replace(QStringLiteral("@textPadding@"), QString::number(metrics().control.textPaddingX));
    replace(QStringLiteral("@rowInset@"), QString::number(metrics().layout.rowInset));
    replace(QStringLiteral("@controlGap@"), QString::number(metrics().layout.controlGap));
    replace(QStringLiteral("@fieldContentHeight@"), QString::number(fieldHeight() - metrics().borderWidth * 2));
    replace(QStringLiteral("@singleLineContentHeight@"), QString::number(fieldHeight() - metrics().borderWidth * 3));
    const QMargins fieldMargins = fieldTextMargins();
    replace(QStringLiteral("@fieldTextPaddingX@"), QString::number(fieldMargins.left()));
    replace(QStringLiteral("@fieldTextPaddingY@"), QString::number(fieldMargins.top()));
    replace(QStringLiteral("@lineEditPaddingX@"),
            QString::number(std::max(0, fieldMargins.left() - LineEditHorizontalMargin)));
    replace(QStringLiteral("@tableCellPaddingX@"), QString::number(metrics().table.cellPaddingX));
    replace(QStringLiteral("@tableCellPaddingY@"), QString::number(metrics().table.cellPaddingY));
    const auto& parameterMetrics = metrics().parameter;
    replace(QStringLiteral("@parameterSliderHandleThickness@"),
            QString::number(parameterMetrics.sliderHandleThickness));
    replace(QStringLiteral("@parameterSliderHandleLength@"),
            QString::number(parameterMetrics.sliderHandleLength));
    replace(QStringLiteral("@parameterSliderHandleOverhang@"),
            QString::number((parameterMetrics.sliderHandleLength
                             - metrics().control.sliderGrooveThickness) / 2));
    const auto& fonts = FontManager::instance();
    replace(QStringLiteral("@bodyFont@"), fonts.styleSheet(snapshot_, FontRole::Body));
    replace(QStringLiteral("@captionFont@"), fonts.styleSheet(snapshot_, FontRole::Caption));
    replace(QStringLiteral("@titleFont@"), fonts.styleSheet(snapshot_, FontRole::Title));
    replace(QStringLiteral("@regularWeight@"), QString::number(fonts.getFont(snapshot_, FontRole::Body).weight()));
    replace(QStringLiteral("@emphasisWeight@"), QString::number(fonts.getFont(snapshot_, FontRole::Body, FontEmphasis::Strong).weight()));
    return styleSheet;
}

QString QtControlSkin::colorSwatchStyle(const QColor& color)
{
    const QColor foreground = contrastTextColor(color);
    return QStringLiteral("QPushButton { background-color: %1; color: %2; }")
        .arg(color.name(QColor::HexArgb), foreground.name(QColor::HexArgb));
}

QString QtControlSkin::collapsibleIndicatorStyle(const QColor& color, int alpha,
                                                  bool rail) const
{
    return QStringLiteral("background-color: %1;%2")
        .arg(rgba(color, alpha), rail ? QStringLiteral(" border: none;")
                                      : QString());
}

QString QtControlSkin::collapsibleHeaderStyle(const QColor& indicator, bool custom, int level,
                                             bool topSeparator, const QColor& backgroundTint) const
{
    const QString indicatorColor = indicator.isValid() ? indicator.name(QColor::HexArgb) : accent();

    const QString titleColor = colors().textTitle.name(QColor::HexArgb);
    const QString font = FontManager::instance().styleSheet(snapshot_, FontRole::Body, FontEmphasis::Strong);
    QString bg, border, hoverBg;
    int verticalBorder = 0;
    if (level <= 0)
    {
        bg       = bgSecondary();
        border   = QString("border: none; border-bottom: %1px solid %2;")
            .arg(metrics().borderWidth).arg(borderPrimary());
        verticalBorder = metrics().borderWidth;
        if (custom)
            border += QString(" border-left: %1px solid %2;").arg(metrics().accentBorderWidth).arg(indicatorColor);
        if (topSeparator) {
            border += QStringLiteral(" border-top: %1px solid %2;").arg(metrics().borderWidth).arg(borderPrimary());
            verticalBorder += metrics().borderWidth;
        }
        hoverBg  = bgTertiary();
    }
    else if (level == 1)
    {
        bg       = custom ? rgba(indicator, 20) : QStringLiteral("transparent");
        border   = QString("border: none; border-bottom: %1px solid %2;")
            .arg(metrics().borderWidth).arg(borderPrimary());
        verticalBorder = metrics().borderWidth;
        if (custom)
            border += QString(" border-left: %1px solid %2;").arg(metrics().accentBorderWidth).arg(indicatorColor);
        hoverBg  = bgTertiary();
    }
    else
    {
        bg       = custom ? rgba(indicator, 16) : QStringLiteral("transparent");
        border   = custom ? QString("border: none; border-left: %1px solid %2;").arg(metrics().accentBorderWidth).arg(indicatorColor)
                          : QStringLiteral("border: none;");
        hoverBg  = QStringLiteral("transparent");
    }
    const QString pad = QString("padding: 0px %1px 0px 0px;").arg(metrics().layout.panelPadding);
    const int contentHeight = metrics().layout.sectionHeaderHeight - verticalBorder;

    if (backgroundTint.isValid()) {
        bg = rgba(backgroundTint, level >= 2 ? 16 : 20);
    }

    return QString(R"(
        QToolButton {
            background-color: %1;
            %2
            border-radius: 0px;
            %3
            color: %4;
            text-align: left;
            min-height: %5px;
            max-height: %5px;
        }
        QToolButton:hover {
            background-color: %6;
            color: %7;
        }
        QToolButton:disabled {
            color: %8;
        }
    )")
        .arg(bg)
        .arg(border)
        .arg(font + pad)
        .arg(titleColor)
        .arg(contentHeight)
        .arg(hoverBg)
        .arg(titleColor)
        .arg(textDisabled());
}

QString QtControlSkin::rgba(const QColor& color, int alpha)
{
    const int clampedAlpha = qBound(0, alpha, 255);
    return QString("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(qRound(color.alphaF() * clampedAlpha));
}

}
