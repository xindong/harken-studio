#pragma once

#include <QString>

namespace harken::studio {

struct BuildVariant
{
    QString platform;
    QString language;

    bool operator==(const BuildVariant&) const = default;
};

}
