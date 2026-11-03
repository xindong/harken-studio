#pragma once

#include <QByteArray>
#include <QString>

class QFile;

namespace VNX {

bool openSourceFile(QFile& file, bool attributesOnly = false);
QByteArray sourceFileState(const QFile& file, bool* reusable = nullptr);
QByteArray outputFileState(const QFile& file);
QByteArray packFileState(const QString& path);

}
