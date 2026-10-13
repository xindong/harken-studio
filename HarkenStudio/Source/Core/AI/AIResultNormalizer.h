#pragma once

#include <QJsonObject>
#include <QString>

namespace VNX {

class AIResultNormalizer
{
public:
    static QJsonObject normalize(const QJsonObject& envelope,
                                 const QString& requestId = {},
                                 const QString& toolCallId = {},
                                 int maxBytes = 16384);
};

}
