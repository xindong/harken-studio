
#pragma once

#include <QObject>
#include <QWidget>
#include <QFont>
#include <QList>
#include <functional>

namespace VNX {

class UIScaleManager : public QObject
{
    Q_OBJECT

public:
    static UIScaleManager& instance();
    
    UIScaleManager(const UIScaleManager&) = delete;
    UIScaleManager& operator=(const UIScaleManager&) = delete;
    
    
    qreal scaleFactor() const { return scaleFactor_; }
    
    int scalePercent() const { return static_cast<int>(scaleFactor_ * 100); }
    
    
    int scaled(int value) const { 
        return static_cast<int>(value * scaleFactor_); 
    }
    
    qreal scaled(qreal value) const { 
        return value * scaleFactor_; 
    }
    
    QSize scaled(const QSize& size) const {
        return QSize(scaled(size.width()), scaled(size.height()));
    }
    
    QMargins scaled(const QMargins& margins) const {
        return QMargins(
            scaled(margins.left()),
            scaled(margins.top()),
            scaled(margins.right()),
            scaled(margins.bottom())
        );
    }
    
    QFont scaledFont(const QFont& baseFont) const;
    
    
    void registerWidget(QWidget* widget, std::function<void(qreal)> scaleCallback);
    
    void unregisterWidget(QWidget* widget);
    
    
    void setScaleFactor(qreal factor);
    void setScalePercent(int percent);
    
    void applyScaleToWidget(QWidget* widget);

signals:
    void scaleFactorChanged(qreal newFactor);
    void scalePercentChanged(int newPercent);

private:
    explicit UIScaleManager(QObject* parent = nullptr);
    ~UIScaleManager() override = default;
    
    void notifyWidgets();

private:
    qreal scaleFactor_ = 1.0;
    qreal basePointSize_ = 0.0;
    
    struct WidgetCallback {
        QWidget* widget;
        std::function<void(qreal)> callback;
    };
    QList<WidgetCallback> registeredWidgets_;
};

class ScalableWidget {
public:
    virtual ~ScalableWidget() = default;
    
    virtual void onScaleChanged(qreal scaleFactor) = 0;
    
protected:
    void registerForScaleUpdates(QWidget* self) {
        UIScaleManager::instance().registerWidget(self, 
            [this](qreal factor) { onScaleChanged(factor); });
    }
    
    void unregisterFromScaleUpdates(QWidget* self) {
        UIScaleManager::instance().unregisterWidget(self);
    }
};

}
