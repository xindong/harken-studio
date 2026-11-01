#pragma once

#include "ThemeSnapshot.h"

#include <QPalette>
#include <QIcon>
#include <QMargins>
#include <QRect>
#include <QString>
#include <QVariant>

class QWidget;
class QPainter;
class QComboBox;
class QFontMetrics;
class QStyleOptionMenuItem;
class QStyleOptionToolButton;

namespace VNX {

class ThemeAssetResolver;

enum class PopupIconColumn { Reserved, Automatic };

class QtControlSkin
{
public:
    struct TableHandleLayout {
        QRect handle;
        QRect content;
    };

    explicit QtControlSkin(ThemeSnapshotPtr snapshot,
                           ThemeAssetResolver* assetResolver = nullptr);
    static QtControlSkin current();

    QPalette applicationPalette() const;
    QString applicationStyleSheet() const;
    int fieldHeight() const;
    QMargins fieldTextMargins() const;
    QIcon toolBarIcon(const QStyleOptionToolButton& option, qreal devicePixelRatio) const;
    static QIcon iconButtonIcon(const QIcon& icon);
    static void setPopupIconColumn(QWidget* widget, PopupIconColumn mode);
    static void setPopupCurrentItemChecked(QComboBox* combo, bool checked);
    static void applyPopupStyle(QComboBox* combo);
    static int popupRowHeight(const QFontMetrics& fontMetrics);
    QSize popupItemSize(const QStyleOptionMenuItem& item, const QSize& contentsSize,
                        const QWidget* widget) const;
    void paintPopupItem(QPainter& painter, const QStyleOptionMenuItem& item,
                        const QWidget* widget, Qt::Alignment alignment = Qt::AlignLeft) const;
    static void setSemanticProperty(QWidget* widget, const char* name,
                                    const QVariant& value);
    static void paintFieldFrame(QPainter& painter, const QWidget* widget);
    int tableResizeGripWidth(const QWidget* widget) const;
    TableHandleLayout tableHandleLayout(const QRect& rect, const QWidget* widget,
                                       bool hasHandle = true) const;
    void paintDragHandle(QPainter& painter, const QRect& rect, bool enabled = true) const;
    void paintTableCellSelection(QPainter& painter, const QRect& rect) const;

    static QString colorSwatchStyle(const QColor& color);
    QString collapsibleIndicatorStyle(const QColor& color, int alpha,
                                      bool rail) const;
    QString collapsibleHeaderStyle(const QColor& indicator, bool custom, int level,
                                   bool topSeparator) const;
    static QString rgba(const QColor& color, int alpha);

private:
    const ThemeColors& colors() const { return snapshot_->ui.colors; }
    const ThemeMetrics& metrics() const { return snapshot_->ui.metrics; }

    QString accent() const { return colors().accent.name(QColor::HexArgb); }
    QString accentHover() const { return colors().accentHover.name(QColor::HexArgb); }
    QString bgPrimary() const { return colors().bgPrimary.name(QColor::HexArgb); }
    QString bgSecondary() const { return colors().bgSecondary.name(QColor::HexArgb); }
    QString bgTertiary() const { return colors().bgTertiary.name(QColor::HexArgb); }
    QString bgHover() const { return colors().bgHover.name(QColor::HexArgb); }
    QString borderPrimary() const { return colors().borderPrimary.name(QColor::HexArgb); }
    QString borderSecondary() const { return colors().borderSecondary.name(QColor::HexArgb); }
    QString viewBorder() const { return colors().viewBorder.name(QColor::HexArgb); }
    QString textPrimary() const { return colors().textPrimary.name(QColor::HexArgb); }
    QString textSecondary() const { return colors().textSecondary.name(QColor::HexArgb); }
    QString textMuted() const { return colors().textMuted.name(QColor::HexArgb); }
    QString textDisabled() const { return colors().textDisabled.name(QColor::HexArgb); }

    ThemeSnapshotPtr snapshot_;
    ThemeAssetResolver* assetResolver_ = nullptr;
};

}
