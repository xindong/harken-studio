#include "Integrations/IntegrationFileSystem.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>

namespace VNH {

bool renameIntegrationDirectory(const QString& source, const QString& target)
{
    constexpr int retries = 25;
    constexpr unsigned long retryDelayMs = 20;
    for (int attempt = 0; attempt <= retries; ++attempt) {
        if (QDir().rename(source, target)) return true;
        if (attempt < retries) QThread::msleep(retryDelayMs);
    }
    return false;
}

bool writeIntegrationFile(const QString& path, const QByteArray& contents, QString* error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create directory for %1").arg(path);
        return false;
    }
    constexpr int retries = 25;
    constexpr unsigned long retryDelayMs = 20;
    for (int attempt = 0; attempt <= retries; ++attempt) {
        {
            QSaveFile file(path);
            file.setDirectWriteFallback(false);
            if (file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit())
                return true;
            if (attempt == retries && error)
                *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        }
        if (attempt < retries) QThread::msleep(retryDelayMs);
    }
    return false;
}

}
