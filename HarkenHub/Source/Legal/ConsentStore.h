#pragma once

#include <QDateTime>
#include <QString>

namespace VNH {

class ConsentStore
{
public:
    static QString filePath();
    static bool hasConsent(const QString& documentId,
                           const QString& documentVersion,
                           const QString& documentSha256);
    static bool recordConsent(const QString& documentId,
                              const QString& documentVersion,
                              const QString& documentSha256,
                              const QString& hubVersion,
                              const QDateTime& at);
};

}
