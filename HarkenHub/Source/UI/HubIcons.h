#pragma once

#include <QIcon>

class QApplication;

namespace harken::hub::HubIcons {

QIcon applicationIcon();
void installApplicationIcon(QApplication& app);
QIcon settingsIcon(qreal devicePixelRatio);
QIcon moreIcon(qreal devicePixelRatio);

}
