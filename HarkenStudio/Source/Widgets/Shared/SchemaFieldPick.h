
#pragma once

#include "Core/PropertyDescriptor.h"

#include <QLatin1String>

namespace VNX {

inline const PropertyDescriptor* pickSchemaField(const PropertySchema& src, QLatin1String field)
{
    for (const auto& d : src)
        if (d.field == field) return &d;
    return nullptr;
}

}
