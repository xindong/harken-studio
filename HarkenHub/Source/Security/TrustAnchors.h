#pragma once

#include <QByteArray>

namespace harken::hub {

class TrustAnchors
{
public:
    static QByteArray manifestPublicKey();
};

}
