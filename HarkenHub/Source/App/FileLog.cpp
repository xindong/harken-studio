#include "App/FileLog.h"

#include "Platform/PlatformPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#include <cstdio>

namespace VNH {

namespace {

constexpr qint64 kMaxLogBytes = 2ll * 1024 * 1024;

QMutex g_mutex;
QString g_filePath;
QtMessageHandler g_previousHandler = nullptr;

QString levelFor(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("DEBUG");
    case QtInfoMsg:
        return QStringLiteral("INFO");
    case QtWarningMsg:
        return QStringLiteral("WARN");
    case QtCriticalMsg:
        return QStringLiteral("ERROR");
    case QtFatalMsg:
        return QStringLiteral("FATAL");
    }
    return QStringLiteral("INFO");
}

void messageHandler(QtMsgType type, const QMessageLogContext& context,
                    const QString& message)
{
    if (FileLog::isIgnorableFrameworkMessage(message))
        return;

    {
        QMutexLocker locker(&g_mutex);
        if (!g_filePath.isEmpty()) {
            FileLog::rotateIfOversized(g_filePath, kMaxLogBytes);
            FileLog::append(g_filePath, levelFor(type), message);
        }
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
    else
        fprintf(stderr, "%s\n", qPrintable(message));
}

}

QString FileLog::logDir()
{
    return userDataDir() + QStringLiteral("/logs");
}

QString FileLog::filePathFor(const QString& role)
{
    return logDir() + QStringLiteral("/hub-") + role + QStringLiteral(".log");
}

bool FileLog::isIgnorableFrameworkMessage(const QString& message)
{
    return message.contains(QStringLiteral(
               "QWindowsWindow::setGeometry: Unable to set geometry")) ||
           message.contains(QStringLiteral(
               "QWindowsPipeWriter: write failed."));
}

bool FileLog::append(const QString& filePath, const QString& level,
                     const QString& message)
{
    if (!QDir().mkpath(QFileInfo(filePath).dir().path()))
        return false;
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return false;
    const QByteArray line =
        (QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) +
         QStringLiteral(" [") + level + QStringLiteral("] ") + message +
         QStringLiteral("\n"))
            .toUtf8();
    return file.write(line) == line.size();
}

void FileLog::rotateIfOversized(const QString& filePath, qint64 maxBytes)
{
    const QFileInfo info(filePath);
    if (!info.exists() || info.size() < maxBytes)
        return;
    const QString rotated = filePath + QStringLiteral(".1");
    QFile::remove(rotated);
    QFile::rename(filePath, rotated);
}

void FileLog::install(const QString& role)
{
    QMutexLocker locker(&g_mutex);
    g_filePath = filePathFor(role);
    rotateIfOversized(g_filePath, kMaxLogBytes);
    if (!g_previousHandler)
        g_previousHandler = qInstallMessageHandler(messageHandler);
}

}
