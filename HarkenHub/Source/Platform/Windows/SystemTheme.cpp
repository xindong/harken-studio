#include "Platform/Windows/SystemTheme.h"

#include <QDebug>
#include <QTimer>

namespace VNH {

namespace {

constexpr wchar_t kWindowClass[] = L"HarkenHubSystemTheme";

}

SystemTheme::SystemTheme(QObject* parent)
    : QObject(parent)
{
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        qWarning() << "Cannot register the system theme window:" << GetLastError();
    } else {
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"", WS_POPUP,
                                  0, 0, 0, 0, nullptr, nullptr,
                                  windowClass.hInstance, this);
        if (!window_)
            qWarning() << "Cannot create the system theme window:" << GetLastError();
    }
    refresh();
}

SystemTheme::~SystemTheme()
{
    if (window_)
        DestroyWindow(window_);
    UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
}

QColor SystemTheme::iconColor() const
{
    return iconColor_;
}

LRESULT CALLBACK SystemTheme::windowProc(HWND window, UINT message,
                                        WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    auto* self = reinterpret_cast<SystemTheme*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self && (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED ||
                 message == WM_SYSCOLORCHANGE))
        self->scheduleRefresh();
    return DefWindowProcW(window, message, wParam, lParam);
}

void SystemTheme::scheduleRefresh()
{
    if (refreshPending_)
        return;
    refreshPending_ = true;
    QTimer::singleShot(0, this, [this] {
        refreshPending_ = false;
        refresh();
    });
}

void SystemTheme::refresh()
{
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    QColor color;
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        const COLORREF background = GetSysColor(COLOR_WINDOW);
        const int brightness = 299 * GetRValue(background) +
                               587 * GetGValue(background) + 114 * GetBValue(background);
        color = brightness >= 128000 ? Qt::black : Qt::white;
    } else {
        DWORD light = 0;
        DWORD size = sizeof(light);
        if (RegGetValueW(HKEY_CURRENT_USER,
                         L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"SystemUsesLightTheme", RRF_RT_REG_DWORD,
                         nullptr, &light, &size) != ERROR_SUCCESS)
            return;
        color = light ? Qt::black : Qt::white;
    }
    if (color == iconColor_)
        return;
    iconColor_ = color;
    emit iconColorChanged(iconColor_);
}

}
