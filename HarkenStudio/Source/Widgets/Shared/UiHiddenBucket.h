
#pragma once

#include <QString>

namespace harken::studio {

inline bool isUiHiddenBucket(const QString& folder)
{
    return folder == QLatin1String("Triggers");
}

}
