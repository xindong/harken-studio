#include "HubIntake.h"

#include "harken_buildinfo.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace VNX {

QString HubIntake::intakeDir()
{
    const QString overrideRoot =
        qEnvironmentVariable("HARKEN_HUB_USER_ROOT");
    const QString hubUserDir =
        !overrideRoot.isEmpty()
            ? overrideRoot
            : QStandardPaths::writableLocation(
                  QStandardPaths::GenericDataLocation) +
                  QStringLiteral("/HarkenSound/HarkenHub");
    return hubUserDir + QStringLiteral("/project-intake");
}

bool HubIntake::recordProjectOpened(const QString& projectFilePath)
{
    const QString canonicalPath =
        QFileInfo(projectFilePath).canonicalFilePath();
    if (canonicalPath.isEmpty())
        return false;

    const QString directory = intakeDir();
    if (!QDir().mkpath(directory))
        return false;

    QJsonObject record;
    record[QStringLiteral("projectPath")] = canonicalPath;
    record[QStringLiteral("buildId")] = QString::fromUtf8(
        Harken::BuildInfo::BuildId.data(),
        static_cast<int>(Harken::BuildInfo::BuildId.size()));
    record[QStringLiteral("openedAt")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    const QString token = QString::fromLatin1(
        QCryptographicHash::hash(canonicalPath.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex()
            .left(32));
    QSaveFile file(directory + QStringLiteral("/") + token +
                   QStringLiteral(".json"));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
    return file.commit();
}

}
