#pragma once

#include <QProxyStyle>
#include <QHash>
#include <QPointer>
#include <QSet>

namespace VNX {

class ThemeManager;

class HarkenStyle : public QProxyStyle
{
public:
    explicit HarkenStyle(QStyle* baseStyle, ThemeManager* themeManager = nullptr);
    using QProxyStyle::polish;
    using QProxyStyle::unpolish;

    static void refreshWidgetAppearance(QWidget* widget, ThemeManager& manager);
    void polish(QWidget* widget) override;
    void unpolish(QWidget* widget) override;
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* returnData = nullptr) const override;
    int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                    const QWidget* widget = nullptr) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption* option,
                           const QSize& contentsSize,
                           const QWidget* widget = nullptr) const override;
    QRect subElementRect(SubElement element, const QStyleOption* option,
                          const QWidget* widget = nullptr) const override;
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex* option,
                          SubControl subControl,
                          const QWidget* widget = nullptr) const override;
    SubControl hitTestComplexControl(ComplexControl control,
                                     const QStyleOptionComplex* option, const QPoint& point,
                                     const QWidget* widget = nullptr) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter,
                       const QWidget* widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption* option,
                     QPainter* painter, const QWidget* widget = nullptr) const override;
    void drawComplexControl(ComplexControl control,
                            const QStyleOptionComplex* option, QPainter* painter,
                            const QWidget* widget = nullptr) const override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ThemeManager& theme() const;
    void scheduleAppearanceRefresh(QWidget* widget);
    ThemeManager* themeManager_;
    QSet<QWidget*> observedWidgets_;
    QHash<QWidget*, QPointer<QWidget>> pendingRefreshes_;
};

}
