
#pragma once

#include <QString>

namespace VNX {

struct BlueprintConnectResult {
    bool    success = false;
    QString error;
    QString effectiveType;
    QString feedback;
    bool operationFailed = false;
};

}
