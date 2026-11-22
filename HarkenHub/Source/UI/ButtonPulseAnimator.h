#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantAnimation>

class QAbstractButton;

namespace harken::hub {

class ButtonPulseAnimator : public QObject
{
public:
    explicit ButtonPulseAnimator(QObject* parent = nullptr);

    void start(QAbstractButton* button);
    void stop();

private:
    QPointer<QAbstractButton> button_;
    QVariantAnimation animation_;
    QString finalStyleSheet_;
};

}
