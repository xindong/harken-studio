
#pragma once

#include <QString>

namespace VNX {

inline bool isUiHiddenBucket(const QString& folder)
{
    return folder == QLatin1String("Triggers");
}

}
