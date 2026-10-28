// The three time coordinates, as three types that cannot be mixed up.
//
// A frame count on its own does not say which clock it counts. Logic decides at
// one position, the mix graph renders at another, and the device presents at a
// third; the distances between them are real and change with configuration.
// Every bug in this area has the same shape — a number from one system used as
// if it belonged to another — so the systems are distinct types and the
// compiler refuses the mistake instead of a reviewer having to catch it.
//
// Conversion is explicit and takes the distance as an argument, because the
// distance is a property of the session rather than of the number. A conversion
// is only meaningful within one observation of the three: mixing a graph
// position from this block with a lead measured in the last one produces a
// plausible number that is wrong, which is exactly the failure the types are
// there to prevent, so the observation is what carries them together.
//
// Header-only by design: these are value types on the path between the logic
// thread and the audio thread, and neither wants a call to compare two frames.
#ifndef HARKEN_CORE_TIME_COORDINATES_H
#define HARKEN_CORE_TIME_COORDINATES_H

#include <cstdint>

namespace harken {

// The session's time-geometry generation. It is raised whenever an operation
// invalidates every existing frame mapping at once -- a sample-rate change, or an
// offline/realtime transport rebuild -- so a frame coordinate from before the
// change can be told apart from a current one by comparing the epoch it was
// stamped with. Distinct from a per-instance schedule epoch, which cancels one
// instance's future work rather than the whole timeline. A plain integer like
// LogicCommitSequence, not a coordinate struct, because it is a generation
// counter that is only ever compared for equality. Zero is reserved as "no epoch"
// / invalid, so an unstamped record cannot be mistaken for a current one; the
// session owner hands out 1 and up. Wrap-around is a hard-failure condition for
// the session owner (a full runtime rebuild), never a silent reset to 1 -- but
// that decision belongs to the owner, not to any structure that merely carries
// the value.
using TransportEpoch = std::uint64_t;

// The reserved invalid transport epoch. Consumers reject a record carrying it.
inline constexpr TransportEpoch kInvalidTransportEpoch = 0;

// Whether a Segment measures positions in musical ticks (converted to frames
// through a tempo map) or absolute nanoseconds (converted through the sample rate
// alone). Shared by every structure that has to treat the two domains
// differently -- the marker index that maps positions to frames, and the
// same-position resolver that keeps a musical handoff in ticks but falls back to
// nanoseconds the moment either side is absolute.
enum class SegmentTimeType : std::uint8_t {
    Musical = 0,
    Absolute = 1
};

// Where the logic thread is deciding. Advances in whole quanta as blocks are
// serviced; this is the coordinate an authored decision and a Variable write
// take effect in.
struct ExecuteFrame {
    std::uint64_t value = 0;
};

// Where the mix graph is rendering. Ahead of the execute position by the publish
// lead, because work has to be filed before the graph reaches it.
struct GraphFrame {
    std::uint64_t value = 0;
};

// Where the device is presenting. Behind the graph position by the output
// latency, which is what a listener actually hears.
struct DeviceFrame {
    std::uint64_t value = 0;
};

inline bool operator==(ExecuteFrame a, ExecuteFrame b) {
    return a.value == b.value;
}
inline bool operator!=(ExecuteFrame a, ExecuteFrame b) { return !(a == b); }
inline bool operator<(ExecuteFrame a, ExecuteFrame b) {
    return a.value < b.value;
}
inline bool operator==(GraphFrame a, GraphFrame b) {
    return a.value == b.value;
}
inline bool operator!=(GraphFrame a, GraphFrame b) { return !(a == b); }
inline bool operator<(GraphFrame a, GraphFrame b) { return a.value < b.value; }
inline bool operator==(DeviceFrame a, DeviceFrame b) {
    return a.value == b.value;
}
inline bool operator!=(DeviceFrame a, DeviceFrame b) { return !(a == b); }
inline bool operator<(DeviceFrame a, DeviceFrame b) {
    return a.value < b.value;
}

// The three positions as they stood at one moment, together with the distances
// that relate them. Held as one value so a conversion cannot silently pair a
// position from one block with a distance measured in another.
struct TimeCoordinateObservation {
    ExecuteFrame execute{};
    // Frames the graph runs ahead of the execute position.
    std::uint64_t publishLeadFrames = 0;
    // Frames the device presents behind the graph position.
    std::uint64_t presentationLatencyFrames = 0;

    GraphFrame Graph() const {
        return GraphFrame{execute.value + publishLeadFrames};
    }

    DeviceFrame Device() const {
        const std::uint64_t graph = execute.value + publishLeadFrames;
        // Presentation sits behind the graph, and the graph has not necessarily
        // run long enough for that to be a positive position yet. Clamping at 0
        // reports "nothing has been presented" rather than wrapping into an
        // enormous frame number.
        return DeviceFrame{graph >= presentationLatencyFrames
                               ? graph - presentationLatencyFrames
                               : 0};
    }

    // Advances all three by one quantum. The distances are unchanged, which is
    // the whole reason they are stored beside the position rather than baked
    // into it.
    void AdvanceQuanta(std::uint64_t quantumFrames, std::uint64_t quanta) {
        execute.value += quantumFrames * quanta;
    }
};

} // namespace harken

#endif // HARKEN_CORE_TIME_COORDINATES_H
