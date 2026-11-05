
#include "BlueprintImportUtils.h"
#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace VNX {
namespace Blueprint {

QString commonPrefix(const QStringList& filePaths)
{
    if (filePaths.size() < 2) {
        return QString();
    }

    QStringList basenames;
    basenames.reserve(filePaths.size());
    for (const QString& p : filePaths) {
        if (p.isEmpty()) {
            return QString();
        }
        basenames.push_back(QFileInfo(p).baseName());
    }

    for (const QString& b : basenames) {
        if (b.isEmpty()) {
            return QString();
        }
    }

    qsizetype minLen = basenames.front().size();
    for (const QString& b : basenames) {
        minLen = std::min(minLen, b.size());
    }

    qsizetype prefixLen = 0;
    for (qsizetype i = 0; i < minLen; ++i) {
        const QChar c = basenames.front().at(i);
        bool allMatch = true;
        for (qsizetype k = 1; k < basenames.size(); ++k) {
            if (basenames[k].at(i) != c) {
                allMatch = false;
                break;
            }
        }
        if (!allMatch) break;
        ++prefixLen;
    }

    if (prefixLen <= 0) {
        return QString();
    }

    QString shared = basenames.front().left(prefixLen);

    while (!shared.isEmpty() && shared.back().isDigit()) {
        shared.chop(1);
    }
    while (!shared.isEmpty() && !shared.back().isLetterOrNumber()) {
        shared.chop(1);
    }

    if (shared.size() < 2) {
        return QString();
    }
    return shared;
}

QPointF snapToGrid(QPointF scenePoint, double cellPx)
{
    if (cellPx <= 0.0) {
        return scenePoint;
    }
    const double sx = std::round(scenePoint.x() / cellPx) * cellPx;
    const double sy = std::round(scenePoint.y() / cellPx) * cellPx;
    return QPointF(sx, sy);
}

}
}
