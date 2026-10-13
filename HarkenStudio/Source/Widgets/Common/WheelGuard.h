
#pragma once

#include <QObject>

namespace VNX {

class WheelGuard : public QObject
{
    Q_OBJECT
public:
    static WheelGuard* instance();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit WheelGuard(QObject* parent = nullptr);
};

}
