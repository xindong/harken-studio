/* Command-mirror dispatch: decodes one Command payload, executes the
 * mirrored public C API call on the calling thread, and fills exactly one
 * reply frame (Reply on execution, Error on a wire-level fault or a
 * declared policy refusal). Pure over the engine's public API — no
 * transport, no session state — so the mirror is unit-testable without a
 * pipe. The caller honors the API's external synchronization contract by
 * invoking this only from the thread that owns the engine (the serving
 * host's logic thread, between Vn_Update pumps). */
#ifndef HARKEN_REMOTE_WIRE_DISPATCH_H
#define HARKEN_REMOTE_WIRE_DISPATCH_H

#include "harken_remote/WireFormat.h"

#include <cstddef>
#include <cstdint>

namespace harkenremote {

void DispatchCommand(const std::uint8_t* payload, std::size_t bytes,
                     std::uint32_t sequence, WireFrame* reply);

} // namespace harkenremote

#endif // HARKEN_REMOTE_WIRE_DISPATCH_H
