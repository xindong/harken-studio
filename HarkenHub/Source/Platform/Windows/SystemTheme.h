#pragma once

#include <QColor>
#include <QObject>

#include <windows.h>

namespace VNH {

class SystemTheme final : public QObject
{
    Q_OBJECT

public:
    explicit SystemTheme(QObject* parent = nullptr);
    ~SystemTheme() override;

    QColor iconColor() const;

signals:
    void iconColorChanged(const QColor& color);

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    void scheduleRefresh();
    void refresh();

    HWND window_ = nullptr;
    QColor iconColor_ = Qt::white;
    bool refreshPending_ = false;
};

}
