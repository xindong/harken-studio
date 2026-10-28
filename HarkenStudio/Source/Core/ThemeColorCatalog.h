#pragma once

#include "ThemeSnapshot.h"

#include <QColor>
#include <QMap>
#include <QString>
#include <QVector>

namespace VNX {

using ThemeColorOverrides = QMap<QString, QColor>;
using ThemeColorCustomizations = QMap<QString, ThemeColorOverrides>;

struct ThemeColorRole
{
    const char* key;
    const char* group;
    const char* label;
    const char* description;
    QColor (*read)(const ThemeSnapshot&);
    void (*write)(ThemeSnapshot&, const QColor&);
};

const QVector<ThemeColorRole>& themeColorRoles();
const ThemeColorRole* findThemeColorRole(const QString& key);

}
