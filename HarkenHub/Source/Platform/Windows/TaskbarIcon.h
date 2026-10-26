#pragma once

#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <windows.h>

class QWindow;

namespace VNH {

class TaskbarIcon final : public QObject, public QAbstractNativeEventFilter
{
public:
    explicit TaskbarIcon(QObject* parent = nullptr);
    ~TaskbarIcon() override;

    void setIcon(const QIcon& icon);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void apply(QWindow* window);
    HICON iconHandle(int size);

    QIcon icon_;
    QHash<int, HICON> handles_;
};

}
