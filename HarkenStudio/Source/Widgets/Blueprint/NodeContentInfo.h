#pragma once

#include <QFlags>
#include <cstdint>
#include <optional>

namespace VNX {

enum class NodeContentField {
    Volume = 0x1,
    Pitch = 0x2,
    Waveform = 0x8,
    OutputVolume = 0x10,
    OffsetOfStart = 0x20,
    Loop = 0x40,
    SendVolume = 0x80
};
Q_DECLARE_FLAGS(NodeContentFields, NodeContentField)
Q_DECLARE_OPERATORS_FOR_FLAGS(NodeContentFields)

inline constexpr NodeContentFields kAllNodeContentFields = NodeContentField::Volume
    | NodeContentField::Pitch | NodeContentField::Waveform | NodeContentField::OutputVolume
    | NodeContentField::OffsetOfStart | NodeContentField::Loop | NodeContentField::SendVolume;

struct NodeContentInfo {
    std::optional<float> volume;
    std::optional<float> pitch;
    std::optional<float> outputVolume;
    std::optional<double> offsetOfStart;
    std::optional<uint32_t> loopCount;
    std::optional<float> sendVolume;

    NodeContentFields fields() const
    {
        NodeContentFields result;
        if (volume) result |= NodeContentField::Volume;
        if (pitch) result |= NodeContentField::Pitch;
        if (outputVolume) result |= NodeContentField::OutputVolume;
        if (offsetOfStart) result |= NodeContentField::OffsetOfStart;
        if (loopCount) result |= NodeContentField::Loop;
        if (sendVolume) result |= NodeContentField::SendVolume;
        return result;
    }
};

}
