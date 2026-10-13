
#include "UIScaleManager.h"
#include "AppSettings.h"
#include <QApplication>
#include <QScreen>
#include <QDebug>

namespace VNX {

UIScaleManager& UIScaleManager::instance()
{
    static UIScaleManager instance;
    return instance;
}

UIScaleManager::UIScaleManager(QObject* parent)
    : QObject(parent)
    , scaleFactor_(1.0)
{
    basePointSize_ = QApplication::font().pointSizeF();
    if (basePointSize_ <= 0) {
        basePointSize_ = 10.0;
    }
    
    connect(&AppSettings::instance(), &AppSettings::globalUIScaleChanged,
            this, &UIScaleManager::setScalePercent);
    
    setScalePercent(AppSettings::instance().globalUIScale());
}

void UIScaleManager::setScaleFactor(qreal factor)
{
    factor = qBound(1.0, factor, 2.0);
    
    if (!qFuzzyCompare(scaleFactor_, factor)) {
        scaleFactor_ = factor;
        
        QFont appFont = QApplication::font();
        appFont.setPointSizeF(basePointSize_ * scaleFactor_);
        QApplication::setFont(appFont);
        
        notifyWidgets();
        
        emit scaleFactorChanged(scaleFactor_);
        emit scalePercentChanged(static_cast<int>(scaleFactor_ * 100));
        
        qDebug() << "[UIScaleManager] Scale factor changed to" << factor 
                 << "(" << static_cast<int>(factor * 100) << "%)";
    }
}

void UIScaleManager::setScalePercent(int percent)
{
    setScaleFactor(percent / 100.0);
}

QFont UIScaleManager::scaledFont(const QFont& baseFont) const
{
    QFont scaledFont = baseFont;
    
    if (baseFont.pointSizeF() > 0) {
        scaledFont.setPointSizeF(baseFont.pointSizeF() * scaleFactor_);
    } else if (baseFont.pixelSize() > 0) {
        scaledFont.setPixelSize(scaled(baseFont.pixelSize()));
    }
    
    return scaledFont;
}

void UIScaleManager::registerWidget(QWidget* widget, std::function<void(qreal)> scaleCallback)
{
    if (!widget) return;
    
    unregisterWidget(widget);
    
    registeredWidgets_.append({widget, scaleCallback});
    
    connect(widget, &QWidget::destroyed, this, [this, widget]() {
        unregisterWidget(widget);
    });
    
    if (scaleCallback) {
        scaleCallback(scaleFactor_);
    }
}

void UIScaleManager::unregisterWidget(QWidget* widget)
{
    for (int i = registeredWidgets_.size() - 1; i >= 0; --i) {
        if (registeredWidgets_[i].widget == widget) {
            registeredWidgets_.removeAt(i);
            break;
        }
    }
}

void UIScaleManager::notifyWidgets()
{
    for (const auto& entry : registeredWidgets_) {
        if (entry.widget && entry.callback) {
            entry.callback(scaleFactor_);
        }
    }
}

void UIScaleManager::applyScaleToWidget(QWidget* widget)
{
    if (!widget) return;
    
    QFont font = widget->font();
    widget->setFont(scaledFont(font));
    
    for (QWidget* child : widget->findChildren<QWidget*>()) {
        QFont childFont = child->font();
        child->setFont(scaledFont(childFont));
    }
}

}
