#include "FloatingTitleBar.h"
#include "Core/ThemeLayout.h"

namespace VNX {

FloatingTitleBar::FloatingTitleBar(QWidget* parent)
    : TitleBarBase(Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint, parent) {
    ThemeLayout::margins(layout_, ThemeLayout::Space::Panel, ThemeLayout::Space::None, ThemeLayout::Space::None, ThemeLayout::Space::None);
}

}
