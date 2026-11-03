#pragma once

#include <QString>
#include <QStringList>

namespace VNX {

struct StartupArguments
{
    QString projectPath;
    bool createProject = false;
    QString projectName;
    QString projectLocation;
    QString projectTemplateId;
    QString projectTemplatePath;

    static StartupArguments parse(const QStringList& args);
};

}
