#pragma once

#include <QStringList>

namespace VNX {

class AudioFormatCatalog
{
public:
    static const QStringList& extensions();
    static QStringList filePatterns();
    static bool containsExtension(const QString& extension);
};

}
