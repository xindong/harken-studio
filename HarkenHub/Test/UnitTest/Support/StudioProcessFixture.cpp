#include "StudioProtocolTestUtils.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QTextStream>
#include <QTimer>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    const auto argument = [&arguments](const QString& key) {
        const int index = arguments.indexOf(key);
        return index < 0 ? QString() : arguments.value(index + 1);
    };
    if (arguments.contains(QStringLiteral("--hold-lock"))) {
        QLockFile lock(argument(QStringLiteral("--hold-lock")));
        if (!lock.tryLock())
            return 2;
        QTextStream(stdout) << "locked" << Qt::endl;
        return app.exec();
    }
    if (arguments.contains(QStringLiteral("--self-test"))) {
        QTextStream(stdout) << QJsonDocument(HarkenHubTest::studioSelfTest()).toJson(QJsonDocument::Compact);
        return 0;
    }
    const bool create = arguments.contains(QStringLiteral("--create-project"));
    const QString projectName = argument(QStringLiteral("--project-name"));
    const QDir projectLocation(argument(QStringLiteral("--project-location")));
    const auto projectPath = create
        ? projectLocation.filePath(projectName + "/" + projectName + ".hsp")
        : argument(QStringLiteral("--project"));
    QFile configFile(create ? projectLocation.filePath(projectName + ".scenario.json")
                            : projectPath + QStringLiteral(".scenario.json"));
    if (!configFile.open(QIODevice::ReadOnly))
        return 2;
    if (create && !QDir().mkpath(QFileInfo(projectPath).absolutePath()))
        return 3;
    QFile launchLog(projectPath + QStringLiteral(".launch.json"));
    if (!launchLog.open(QIODevice::WriteOnly))
        return 3;
    launchLog.write(QJsonDocument(QJsonObject{
        {QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)},
        {QStringLiteral("pid"), app.applicationPid()},
    }).toJson());
    launchLog.close();
    QTimer::singleShot(10000, &app, &QCoreApplication::quit);
    return app.exec();
}
