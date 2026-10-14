#pragma once

#include <QString>
#include <QByteArray>

namespace VNH {

bool renameIntegrationDirectory(const QString& source, const QString& target);
bool writeIntegrationFile(const QString& path, const QByteArray& contents, QString* error);

}
