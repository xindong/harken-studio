
#pragma once

#include "ContextInfo.h"

#include <QHash>
#include <QString>

namespace VNX {

struct HelpEntry
{
    QString title;
    QString body;
    QHash<QString, QString> byType;
};

namespace HelpCatalog {

QString keyFor(const ContextTopic& topic);
const HelpEntry* entryFor(const ContextTopic& topic);
QString bodyFor(const ContextTopic& topic);

}

}
