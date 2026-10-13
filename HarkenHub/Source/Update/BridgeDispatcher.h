#pragma once

#include <QString>

namespace VNH {

class BridgeDispatcher
{
public:
    static bool available();
    static bool dispatch(QString* error);
};

}
