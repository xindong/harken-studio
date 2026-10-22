#pragma once

#include <QString>
#include <QStringList>
#include <QByteArray>

namespace VNH {

bool renameIntegrationDirectory(const QString& source, const QString& target, QString* error = nullptr);
bool writeIntegrationFile(const QString& path, const QByteArray& contents, QString* error);
bool checkIntegrationFilesWritable(const QStringList& paths, QString* error);

}
