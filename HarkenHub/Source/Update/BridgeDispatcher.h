#pragma once

#include <QString>

namespace harken::hub {

class BridgeDispatcher
{
public:
    static bool available();
    static bool dispatch(QString* error);
};

}
