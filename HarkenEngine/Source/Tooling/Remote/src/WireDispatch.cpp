/* Command-mirror dispatch implementation. The per-opcode cases are
 * machine-generated from the wire description table (never hand-written
 * here — drift between Harken.h and the wire layer is the failure mode the
 * generator exists to kill); this file provides the shared frame around
 * them and compiles the generated layout asserts into the library so any
 * table to header drift breaks the build. */
#include "harken_remote/WireDispatch.h"

#include "harken_remote/WireDescDecode.h"

#include "harken_wire_layout_asserts.h"

#include <harken/Harken.h>

namespace harkenremote {

void DispatchCommand(const std::uint8_t* payload, std::size_t bytes,
                     std::uint32_t sequence, WireFrame* reply) {
    WireReader reader(payload, bytes);
    std::uint16_t opcode = 0;
    std::uint16_t reserved = 0;
    if (!reader.ReadU16(&opcode) || !reader.ReadU16(&reserved)) {
        EncodeWireError(reply, sequence, kWireErrMalformedPayload, 0);
        return;
    }
    /* The reserved word is ignored, not refused: it is the additive-minor
     * growth spot, and a future client setting it must interoperate with
     * this server per the minor-compatibility rule. */
    WireArena arena;
    switch (opcode) {
#include "harken_wire_dispatch.inl"
        default:
            break;
    }
    EncodeWireError(reply, sequence, kWireErrUnknownOpcode, opcode);
}

} // namespace harkenremote
