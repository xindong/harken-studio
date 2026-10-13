
#pragma once

#include <QString>
#include <QJsonArray>
#include "../ProjectTypes.h"

namespace VNX {

struct ResolvedSelector
{
    bool    ok = true;
    QString name;
    QString errorCode;
    QString errorMessage;
    ProjectItem* item = nullptr;
    QJsonArray candidates;
};

ResolvedSelector resolveSelector(const QString& selector);
ResolvedSelector resolveProjectSelector(ProjectData* project, const QString& selector,
                                        std::optional<ProjectItem::Type> type = std::nullopt);

}
