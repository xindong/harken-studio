
#pragma once

#include <QString>

namespace harken::studio {

struct BlueprintConnectResult {
    bool    success = false;
    QString error;
    QString effectiveType;
    QString feedback;
    bool operationFailed = false;
};

}
