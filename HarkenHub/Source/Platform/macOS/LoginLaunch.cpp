#include "Platform/LoginLaunch.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace VNH {

namespace {

QString agentPlistPath()
{
    return QDir::homePath() +
           QStringLiteral("/Library/LaunchAgents/"
                          "com.harkensound.harkenhub.background.plist");
}

}

bool registerBackgroundOnLogin()
{
    const QString path = agentPlistPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;

    const QString plist =
        QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "\t<key>Label</key>\n"
            "\t<string>com.harkensound.harkenhub.background</string>\n"
            "\t<key>ProgramArguments</key>\n"
            "\t<array>\n"
            "\t\t<string>%1</string>\n"
            "\t\t<string>--background</string>\n"
            "\t</array>\n"
            "\t<key>RunAtLoad</key>\n"
            "\t<true/>\n"
            "</dict>\n"
            "</plist>\n")
            .arg(QCoreApplication::applicationFilePath().toHtmlEscaped());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray data = plist.toUtf8();
    return file.write(data) == data.size();
}

bool unregisterBackgroundOnLogin()
{
    const QString path = agentPlistPath();
    if (!QFile::exists(path))
        return true;
    return QFile::remove(path);
}

bool isBackgroundOnLoginRegistered()
{
    return QFile::exists(agentPlistPath());
}

}
