#pragma once

#include <QString>
#include <functional>

class QWidget;

namespace harken::studio {

using FramelessChromeInstaller = std::function<void(QWidget* window, const QString& title)>;

void setFramelessChromeInstaller(FramelessChromeInstaller fn);

void applyFramelessChrome(QWidget* window, const QString& title);

}
