#pragma once

#include <QString>

namespace VNX {

struct BuildVariant
{
    QString platform;
    QString language;

    bool operator==(const BuildVariant&) const = default;
};

}
