#pragma once

#include <QString>
#include <QByteArray>

namespace VNH {

bool renameIntegrationDirectory(const QString& source, const QString& target, QString* error = nullptr);
bool writeIntegrationFile(const QString& path, const QByteArray& contents, QString* error);

}
