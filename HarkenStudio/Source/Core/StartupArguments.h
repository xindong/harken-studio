#pragma once

#include <QString>
#include <QStringList>

namespace harken::studio {

struct StartupArguments
{
    QString projectPath;
    bool createProject = false;
    bool shell = false;
    bool shellJson = false;
    QString projectName;
    QString projectLocation;
    QString projectTemplateId;

    static StartupArguments parse(const QStringList& args);
};

}
