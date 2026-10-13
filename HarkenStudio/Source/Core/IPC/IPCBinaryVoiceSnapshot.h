#pragma once

#include "IPCBinaryProtocol.h"

#include <QByteArray>

namespace VNX {
namespace BinaryVoiceSnapshot {

enum class Outcome : uint8_t
{
    Error = 0,
    Ok
};

inline Outcome encode(bool engineReady, bool snapshotOk, bool hasHonestRow, QByteArray& payload)
{
    payload.clear();
    if (!engineReady || !snapshotOk || !hasHonestRow)
        return Outcome::Error;
    return Outcome::Ok;
}

}
}
