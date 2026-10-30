#pragma once

#include "ThemeSnapshot.h"

#include <QColor>
#include <QMap>
#include <QString>
#include <QVector>

#include <optional>

namespace VNX {

using ThemeColorOverrides = QMap<QString, QColor>;
using ColorPaletteCustomizations = QMap<QString, ThemeColorOverrides>;

struct ThemeColorBinding
{
    QString target;
    std::optional<int> alpha;
    bool operator==(const ThemeColorBinding&) const = default;
};

struct ThemeColorRole
{
    QString key;
    QString group;
    QString label;
    QString description;
    QVector<ThemeColorBinding> bindings;
    bool operator==(const ThemeColorRole&) const = default;
};

const QVector<ThemeColorRole>& studioDefaultColorRoles();
const ThemeColorRole* findThemeColorRole(const QVector<ThemeColorRole>& roles,
                                       const QString& key);

}
