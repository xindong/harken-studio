
#include "QtControlSkin.h"

#include "FontManager.h"
#include "ThemeManager.h"
#include "ThemeAssetCatalog.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QIconEngine>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QTemporaryDir>
#include <QWidget>
#include <QtGlobal>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

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

int QtControlSkin::fieldHeight() const
{
    const QFont font = FontManager::instance().getFont(snapshot_, FontRole::Body);
    return std::max({metrics().controlHeight, metrics().objectIconExtent + 4,
                     metrics().navigationChevronExtent + 2,
                     QFontMetrics(font).height() + 6});
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
    palette.setColor(QPalette::Button, p.button);
    palette.setColor(QPalette::ButtonText, p.buttonText);
    palette.setColor(QPalette::BrightText, p.brightText);
    palette.setColor(QPalette::Link, p.link);
    palette.setColor(QPalette::Highlight, p.highlight);
    palette.setColor(QPalette::HighlightedText, p.highlightedText);
    palette.setColor(QPalette::Light, p.light);
    palette.setColor(QPalette::Dark, p.dark);
    palette.setColor(QPalette::Mid, p.mid);
    palette.setColor(QPalette::Disabled, QPalette::Text, p.disabledText);
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
                      metrics().navigationChevronExtent, colors().textSecondary));
    replace(QStringLiteral("@chevronDownDisabled@"),
            imagePath(ThemeAssetIds::NavigationChevronDown,
                      metrics().navigationChevronExtent, colors().textDisabled));
    replace(QStringLiteral("@checkboxIndeterminate@"),
            imagePath(ThemeAssetIds::CheckboxIndeterminate, 14, colors().textPrimary));
    replace(QStringLiteral("@checkboxIndeterminateDisabled@"),
            imagePath(ThemeAssetIds::CheckboxIndeterminate, 14, colors().textDisabled));
    const auto color = [](const QColor& value) {
        return value.name(QColor::HexArgb);
    };
    QColor errorOpaque = colors().error;
    errorOpaque.setAlphaF(1.0);

    replace(QStringLiteral("@accent@"), color(colors().accent));
    replace(QStringLiteral("@accentHover@"), color(colors().accentHover));
    replace(QStringLiteral("@accentText@"), color(colors().accentText));
    replace(QStringLiteral("@bgPrimary@"), color(colors().bgPrimary));
    replace(QStringLiteral("@bgSecondary@"), color(colors().bgSecondary));
    replace(QStringLiteral("@bgTertiary@"), color(colors().bgTertiary));
    replace(QStringLiteral("@bgHover@"), color(colors().bgHover));
    replace(QStringLiteral("@bgSelected@"), color(colors().bgSelected));
    replace(QStringLiteral("@panelBackground@"), color(colors().panelBackground));
    replace(QStringLiteral("@panelBorder@"), color(colors().panelBorder));
    replace(QStringLiteral("@dropHighlight@"), color(colors().dropHighlight));
    replace(QStringLiteral("@borderPrimary@"), color(colors().borderPrimary));
    replace(QStringLiteral("@borderSecondary@"), color(colors().borderSecondary));
    replace(QStringLiteral("@viewBorder@"), color(colors().viewBorder));
    replace(QStringLiteral("@textPrimary@"), color(colors().textPrimary));
    replace(QStringLiteral("@textSecondary@"), color(colors().textSecondary));
    replace(QStringLiteral("@textMuted@"), color(colors().textMuted));
    replace(QStringLiteral("@textDisabled@"), color(colors().textDisabled));
    replace(QStringLiteral("@error@"), color(colors().error));
    replace(QStringLiteral("@errorOpaque@"), color(errorOpaque));
    replace(QStringLiteral("@errorText@"), color(contrastTextColor(errorOpaque)));
    replace(QStringLiteral("@window@"), color(palette.window));
    replace(QStringLiteral("@base@"), color(palette.base));
    replace(QStringLiteral("@alternateBase@"), color(palette.alternateBase));
    replace(QStringLiteral("@button@"), color(palette.button));
    replace(QStringLiteral("@brightText@"), color(palette.brightText));
    replace(QStringLiteral("@mid@"), color(palette.mid));
    replace(QStringLiteral("@disabledHighlight@"), color(palette.disabledHighlight));
    replace(QStringLiteral("@iconButtonHover@"), rgba(colors().textPrimary, 24));
    replace(QStringLiteral("@auditionBg@"), color(colors().auditionBg));
    replace(QStringLiteral("@auditionPlayOverlay@"), rgba(colors().auditionPlayActive, 40));
    replace(QStringLiteral("@shellMenuText@"), color(colors().shellMenuText));
    replace(QStringLiteral("@shellMenuHover@"), color(colors().shellMenuHover));
    replace(QStringLiteral("@shellMenuHoverText@"), color(colors().shellMenuHoverText));
    replace(QStringLiteral("@shellMenuPressed@"), color(colors().shellMenuPressed));
    replace(QStringLiteral("@shellMenuPressedText@"), color(colors().shellMenuPressedText));
    replace(QStringLiteral("@shellButtonText@"), color(colors().shellButtonText));
    replace(QStringLiteral("@shellButtonHoverText@"), color(colors().shellButtonHoverText));
    replace(QStringLiteral("@shellButtonPressed@"), color(colors().shellButtonPressed));
    replace(QStringLiteral("@shellCloseHover@"), color(colors().shellCloseHover));
    replace(QStringLiteral("@shellClosePressed@"), color(colors().shellClosePressed));
    replace(QStringLiteral("@success@"), color(colors().success));
    replace(QStringLiteral("@warning@"), color(colors().warning));
    replace(QStringLiteral("@warningSoft@"), rgba(colors().warning, 36));
    replace(QStringLiteral("@info@"), color(colors().info));
    replace(QStringLiteral("@sectionSeparator@"), color(colors().sectionSeparator));
    replace(QStringLiteral("@headerBackground@"), color(colors().headerBackground));
    replace(QStringLiteral("@borderAccent@"), color(colors().borderAccent));
    replace(QStringLiteral("@accentSoft@"), rgba(colors().accent, 38));
    replace(QStringLiteral("@toolbarRemoteActive@"), color(colors().warning.lighter(125)));
    replace(QStringLiteral("@toolbarRemoteHover@"), color(colors().warning.lighter(145)));
    replace(QStringLiteral("@toolbarMonitorActive@"), color(colors().error));
    replace(QStringLiteral("@toolbarMonitorHover@"), color(colors().error.lighter(115)));
    replace(QStringLiteral("@toolbarAllPlatformsActive@"), color(colors().success));
    replace(QStringLiteral("@toolbarAllPlatformsHover@"), color(colors().success.lighter(115)));
    replace(QStringLiteral("@compactSliderFill@"), rgba(colors().accent, 85));
    replace(QStringLiteral("@nodeContentLabel@"), color(snapshot_->domains.nodeGraphNode.contentLabel));
    replace(QStringLiteral("@nodeContentValue@"), color(snapshot_->domains.nodeGraphNode.contentValue));
    replace(QStringLiteral("@aiBrandAccent@"), color(snapshot_->domains.ai.brandAccent));
    replace(QStringLiteral("@aiBrandAccentSoft@"), rgba(snapshot_->domains.ai.brandAccent, 32));
    replace(QStringLiteral("@aiMessageSeparator@"), rgba(colors().textPrimary, 16));
    replace(QStringLiteral("@aiUserAccent@"), color(snapshot_->domains.ai.userAccent));
    replace(QStringLiteral("@monitorCpuSafe@"), color(snapshot_->domains.monitor.cpuSafe));
    replace(QStringLiteral("@monitorCpuWarning@"), color(snapshot_->domains.monitor.cpuWarning));
    replace(QStringLiteral("@monitorCpuCritical@"), color(snapshot_->domains.monitor.cpuCritical));
    replace(QStringLiteral("@monitorBufferNormal@"), color(snapshot_->domains.monitor.bufferNormal));
    replace(QStringLiteral("@monitorBufferWarning@"), color(snapshot_->domains.monitor.bufferWarning));
    replace(QStringLiteral("@radiusSmall@"), QString::number(metrics().radiusSmall));
    replace(QStringLiteral("@radiusMedium@"), QString::number(metrics().radiusMedium));
    replace(QStringLiteral("@radiusLarge@"), QString::number(metrics().radiusLarge));
    replace(QStringLiteral("@spacingXSmall@"), QString::number(metrics().spacingXSmall));
    replace(QStringLiteral("@spacingSmall@"), QString::number(metrics().spacingSmall));
    replace(QStringLiteral("@spacingMedium@"), QString::number(metrics().spacingMedium));
    replace(QStringLiteral("@fieldContentHeight@"), QString::number(fieldHeight() - 2));
    replace(QStringLiteral("@tableCellPaddingX@"), QString::number(metrics().table.cellPaddingX));
    replace(QStringLiteral("@tableCellPaddingY@"), QString::number(metrics().table.cellPaddingY));
    replace(QStringLiteral("@chevronExtent@"),
            QString::number(metrics().navigationChevronExtent));
    replace(QStringLiteral("@inspectorColumnPadding@"),
            QString::number(metrics().inspector.columnPadding));
    const auto& parameterMetrics = metrics().parameter;
    replace(QStringLiteral("@parameterSliderGroove@"),
            QString::number(parameterMetrics.sliderGrooveThickness));
    replace(QStringLiteral("@parameterSliderHandleThickness@"),
            QString::number(parameterMetrics.sliderHandleThickness));
    replace(QStringLiteral("@parameterSliderHandleLength@"),
            QString::number(parameterMetrics.sliderHandleLength));
    replace(QStringLiteral("@parameterSliderHandleOverhang@"),
            QString::number((parameterMetrics.sliderHandleLength
                             - parameterMetrics.sliderGrooveThickness) / 2));
    replace(QStringLiteral("@bodyPointSize@"), QString::number(snapshot_->ui.typography.body.pointSize));
    replace(QStringLiteral("@captionPointSize@"), QString::number(snapshot_->ui.typography.caption.pointSize));
    replace(QStringLiteral("@titlePointSize@"), QString::number(snapshot_->ui.typography.title.pointSize));
    replace(QStringLiteral("@headingPointSize@"), QString::number(snapshot_->ui.typography.heading.pointSize));
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

QString QtControlSkin::collapsibleHeaderStyle(const QColor& indicator, bool expanded, bool custom, int level) const
{
    const QString indicatorColor = indicator.isValid() ? indicator.name() : accent();

    QString bg, border, font, color, hoverBg, hoverColor, pad;
    int minH;
    if (level <= 0)
    {
        bg       = bgSecondary();
        border   = custom ? QString("border: none; border-bottom: 1px solid %1; border-left: 2px solid %2;").arg(borderPrimary(), indicatorColor)
                          : QString("border: none; border-bottom: 1px solid %1;").arg(borderPrimary());
        font     = "font-size: 11px; font-weight: 700; letter-spacing: 0.5px;";
        color    = custom ? indicatorColor : (expanded ? accent() : textSecondary());
        hoverBg  = bgTertiary();
        hoverColor = color;
        pad      = "padding: 3px 6px 3px 4px;";
        minH     = 22;
    }
    else if (level == 1)
    {
        bg       = custom ? rgba(indicator, 20) : QStringLiteral("transparent");
        border   = custom ? QString("border: none; border-left: 3px solid %1; border-bottom: 1px solid %2;").arg(indicatorColor, borderPrimary())
                          : QString("border: none; border-bottom: 1px solid %1;").arg(borderPrimary());
        font     = "font-size: 11px; font-weight: 600; letter-spacing: 0.3px;";
        color    = custom ? indicatorColor : accent();
        hoverBg  = bgTertiary();
        hoverColor = custom ? indicatorColor : accent();
        pad      = "padding: 2px 8px 2px 6px;";
        minH     = 20;
    }
    else
    {
        bg       = custom ? rgba(indicator, 16) : QStringLiteral("transparent");
        border   = custom ? QString("border: none; border-left: 2px solid %1;").arg(indicatorColor)
                          : QStringLiteral("border: none;");
        font     = "font-size: 11px; font-weight: 600;";
        color    = textMuted();
        hoverBg  = QStringLiteral("transparent");
        hoverColor = textSecondary();
        pad      = "padding: 1px 8px 1px 6px;";
        minH     = 18;
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
        .arg(color)
        .arg(minH)
        .arg(hoverBg)
        .arg(hoverColor)
        .arg(textDisabled());
}

QString QtControlSkin::rgba(const QColor& color, int alpha)
{
    const int clampedAlpha = qBound(0, alpha, 255);
    return QString("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(clampedAlpha);
}

}
