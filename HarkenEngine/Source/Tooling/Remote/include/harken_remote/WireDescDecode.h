/* Typed desc decoding for the command mirror: the generic vintage-aware
 * image decode plus the generated per-type section traits. Split from
 * WireFormat.h so the primitive layer stays independent of the generated
 * headers; only the dispatcher needs this. */
#ifndef HARKEN_REMOTE_WIRE_DESC_DECODE_H
#define HARKEN_REMOTE_WIRE_DESC_DECODE_H

#include "harken_remote/WireFormat.h"

#include "harken_wire_desc_meta.h"

namespace harkenremote {

/* Decodes one desc param: u32 declared structSize (kWireNullLength encodes
 * a NULL desc pointer) + that many raw struct bytes + one out-of-line
 * section per pointer field lying inside the declared vintage.
 *
 * Vintage handling:
 *  - declared <= sizeof(T): the image is copied into zeroed storage of
 *    sizeof(T), so appended fields the client predates read as zero and a
 *    pointer field beyond the declared vintage stays null. Every pointer
 *    field inside the vintage is then overwritten from its section —
 *    raw image bytes never survive into a pointer, so a client can never
 *    smuggle a pointer value into the server process.
 *  - declared > sizeof(T): the image passes through opaquely (no section
 *    parsing, pointers left as the zeroed image the encoder produced) so
 *    the engine's own unknown-vintage refusal answers it.
 *
 * The struct's leading structSize field must equal the declared byte
 * count; a disagreement is an internally inconsistent image and fails the
 * decode (MalformedPayload at the dispatch layer). */
template <typename T>
bool DecodeWireDesc(WireReader& reader, WireArena& arena, const T** out) {
    *out = nullptr;
    std::uint32_t declared = 0;
    if (!reader.ReadU32(&declared)) {
        return false;
    }
    if (declared == kWireNullLength) {
        return true; // NULL desc; the engine refuses it per its own contract
    }
    if (declared < sizeof(std::uint32_t) || declared > kMaxFrameBytes) {
        return false; // structSize itself must exist and be sane
    }
    const std::uint8_t* raw = reader.ReadBytes(declared);
    if (raw == nullptr) {
        return false;
    }
    std::uint32_t stamped = 0;
    std::memcpy(&stamped, raw, sizeof(stamped));
    if (stamped != declared) {
        return false;
    }
    const std::size_t storage = declared > sizeof(T) ? declared : sizeof(T);
    void* buf = arena.Alloc(storage); // zero-filled
    std::memcpy(buf, raw, declared);
    T* desc = static_cast<T*>(buf);
    if (declared > sizeof(T)) {
        *out = desc;
        return true;
    }
    if (!WireDescMeta<T>::DecodeSections(reader, arena, desc, declared)) {
        return false;
    }
    *out = desc;
    return true;
}

} // namespace harkenremote

#endif // HARKEN_REMOTE_WIRE_DESC_DECODE_H
