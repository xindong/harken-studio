
#include "BaseDialog.h"
#include "FramelessChrome.h"
#include "ThemeManager.h"
#include <QShowEvent>

namespace VNX {

BaseDialog::BaseDialog(QWidget* parent, Qt::WindowFlags flags)
    : QDialog(parent, flags)
{
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &BaseDialog::applyTheme);
    applyTheme();
}

void BaseDialog::applyTheme()
{
}

void BaseDialog::setupStandardButtons(QDialogButtonBox::StandardButtons buttons)
{
    if (buttonBox_) {
        delete buttonBox_;
        buttonBox_ = nullptr;
    }

    buttonBox_ = new QDialogButtonBox(buttons, this);

    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void BaseDialog::setDialogTabOrder(std::initializer_list<QWidget*> widgets)
{
    QWidget* prev = nullptr;
    for (auto* w : widgets) {
        if (w && prev) {
            QWidget::setTabOrder(prev, w);
        }
        prev = w;
    }
}

void BaseDialog::showEvent(QShowEvent* event)
{
    if (autoFramelessChrome_ && !chromeApplied_) {
        chromeApplied_ = true;
        applyFramelessChrome(this, windowTitle());
    }

    QDialog::showEvent(event);
}

}
