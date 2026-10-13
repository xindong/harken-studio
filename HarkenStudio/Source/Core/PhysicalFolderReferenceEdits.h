#pragma once

#include <QJsonObject>
#include <QSet>
#include <QUuid>

namespace VNX {

class PhysicalFolderReferenceEdits
{
public:
    static QJsonObject removeReferences(QJsonObject content, const QSet<QUuid>& deletedGuids);
    static bool mergeReferences(QJsonObject& current, const QJsonObject& expected,
                                const QJsonObject& replacement);
};

}
