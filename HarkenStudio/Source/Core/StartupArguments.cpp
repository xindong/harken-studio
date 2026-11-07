#include "StartupArguments.h"

#include <QDir>
#include <QFileInfo>

namespace VNX {

StartupArguments StartupArguments::parse(const QStringList& args)
{
    StartupArguments arguments;
    arguments.createProject = args.contains(QStringLiteral("--create-project"));
    arguments.shell = args.contains(QStringLiteral("--shell"));
    arguments.shellJson = args.contains(QStringLiteral("--json"));
    for (int i = 1; i + 1 < args.size(); ++i) {
        const QString& key = args.at(i);
        const QString& value = args.at(i + 1);
        if (key == QStringLiteral("--project"))
            arguments.projectPath = value;
        else if (key == QStringLiteral("--project-name"))
            arguments.projectName = value;
        else if (key == QStringLiteral("--project-location"))
            arguments.projectLocation = value;
        else if (key == QStringLiteral("--project-template-id"))
            arguments.projectTemplateId = value;
        else if (key == QStringLiteral("--project-template-file"))
            arguments.projectTemplatePath = value;
    }

    if (arguments.createProject && arguments.projectName.isEmpty()
        && arguments.projectLocation.isEmpty()) {
        const int createIndex = args.indexOf(QStringLiteral("--create-project"));
        if (createIndex + 1 < args.size()) {
            const QString path = QDir::cleanPath(args.at(createIndex + 1));
            if (!path.startsWith(QStringLiteral("--"))) {
                const QFileInfo projectDirectory(path);
                arguments.projectName = projectDirectory.fileName();
                arguments.projectLocation = projectDirectory.absoluteDir().absolutePath();
            }
        }
    }

    return arguments;
}

}
