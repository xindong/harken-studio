#pragma once

#include <QStringList>

namespace harken::studio {

class AudioFormatCatalog
{
public:
    static const QStringList& extensions();
    static QStringList filePatterns();
    static bool containsExtension(const QString& extension);
};

}
