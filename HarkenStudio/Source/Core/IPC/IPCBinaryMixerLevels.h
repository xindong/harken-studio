#pragma once

#include "IPCBinaryProtocol.h"
#include "RuntimeBridge/DiagnosticsBridge.h"

#include <QByteArray>

#include <algorithm>
#include <vector>

namespace harken::studio {
namespace BinaryMixerLevels {

// Fixed-width mixer rows have no availability bits. A row is emitted only
// when every field the client will treat as a measurement is Available.
// rmsL/rmsR stay zeroed unused padding: the field contract marks rms
// Unavailable, and this command cannot represent that on the wire.

inline bool tryEncodeEntry(const harken::studio::bridge::DebugBusData& bus,
                           IPCBusLevelEntry& out)
{
    using harken::studio::bridge::isAvailable;

    if (!isAvailable(bus.busIdAvailable)
        || !isAvailable(bus.peakAvailable)
        || !isAvailable(bus.mutedAvailable)
        || !isAvailable(bus.soloedAvailable))
    {
        return false;
    }

    out = {};
    const auto id = harken::studio::types::ObjectIdBytes(bus.busId);
    std::copy(id.begin(), id.end(), out.busId);
    out.peakL = bus.peakL;
    out.peakR = bus.peakR;
    out.muted = bus.muted ? 1 : 0;
    out.soloed = bus.soloed ? 1 : 0;
    return true;
}

enum class Outcome : uint8_t
{
    Error = 0,
    Ok
};

inline Outcome encode(bool engineReady,
                      bool snapshotOk,
                      const std::vector<harken::studio::bridge::DebugBusData>& buses,
                      QByteArray& payload)
{
    payload.clear();
    if (!engineReady || !snapshotOk)
        return Outcome::Error;

    for (const auto& bus : buses)
    {
        IPCBusLevelEntry entry{};
        if (!tryEncodeEntry(bus, entry))
            continue;
        payload.append(reinterpret_cast<const char*>(&entry), sizeof(entry));
    }
    return Outcome::Ok;
}

}
}
