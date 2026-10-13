#include "FramelessChrome.h"

#include <QWidget>
#include <utility>

namespace VNX {

namespace {
FramelessChromeInstaller g_installer;
}

void setFramelessChromeInstaller(FramelessChromeInstaller fn)
{
    g_installer = std::move(fn);
}

void applyFramelessChrome(QWidget* window, const QString& title)
{
    if (g_installer && window)
        g_installer(window, title);
}

}
