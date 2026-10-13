#pragma once

#include <QList>
#include <QPair>
#include <QString>

namespace VNH {

class LogExporter
{
public:
    static QList<QPair<QString, QString>> defaultSources();
    static bool exportFiles(const QString& zipPath,
                            const QList<QPair<QString, QString>>& sources,
                            QString* error);
    static bool exportDiagnostics(const QString& zipPath, QString* error);
};

}
