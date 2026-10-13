#pragma once

#include <QString>

namespace VNH {

class FileLog
{
public:
    static void install(const QString& role);
    static QString logDir();
    static QString filePathFor(const QString& role);
    static bool isIgnorableFrameworkMessage(const QString& message);
    static bool append(const QString& filePath, const QString& level,
                       const QString& message);
    static void rotateIfOversized(const QString& filePath,
                                  qint64 maxBytes);
};

}
