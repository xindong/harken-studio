// Compile-time ABI pin for the C# binding. The generated header's
// static_asserts compare every enum value, result code, #define constant, and
// mirrored struct size/field offset the binding emits against the real Harken.h
// symbols — so a mis-transcription in harken_wire_table.json (from which the
// binding is generated) is a compile error here, never a silent binding bug.
// This is the C++ half of the ABI-fidelity chain; HarkenAbi.CheckLayout() on the
// managed side proves the C# layout matches the same pinned values, so the
// generated C# mirror matches the C header transitively.
#include "harken_binding_abi_pin.g.h"
