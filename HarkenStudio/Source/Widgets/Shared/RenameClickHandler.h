#pragma once

#include <QObject>
#include <QPoint>
#include <QTimer>
#include <QVariant>

#include <functional>

class QWidget;

namespace VNX {

class RenameClickHandler final : public QObject
{
public:
    struct Target {
        QVariant key;
        std::function<void()> rename;
    };
    using TargetAt = std::function<Target(const QPoint&)>;

    RenameClickHandler(QWidget* focusScope, QWidget* mouseArea, TargetAt targetAt,
                       QObject* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool ownsFocus() const;
    void cancel();
    void resetFocus();

    QWidget* focusScope_;
    QWidget* mouseArea_;
    TargetAt targetAt_;
    QTimer timer_;
    QVariant pressedKey_;
    QVariant pendingKey_;
    QPoint pressPosition_;
    QPoint releasePosition_;
    bool ready_ = false;
};

}
