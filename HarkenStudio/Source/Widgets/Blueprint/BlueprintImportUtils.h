
#pragma once

#include <QPointF>
#include <QString>
#include <QStringList>

namespace VNX {

namespace Blueprint {

QString commonPrefix(const QStringList& filePaths);

QPointF snapToGrid(QPointF scenePoint, double cellPx = 20.0);

}
}
