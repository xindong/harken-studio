#!/usr/bin/env python3
"""Named-pipe client for the Harken remote-control channel.

Table-driven: everything about the protocol — opcodes, per-function
signatures, desc struct layouts — comes from the generated harken_wire
module (which embeds harken_wire_table.json verbatim), so this client can
never drift from the C++ side without the generator noticing.

Layout model mirrors MSVC x64 (the generated C++ static-asserts pin the
same numbers against the real headers): scalars align to their size,
structs align to their widest member and pad to it, unions size to their
widest member.
"""

import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harken_wire as wire

SCALAR_FMT = {"u16": "<H", "u32": "<I", "u64": "<Q", "i32": "<i", "f32": "<f"}
SCALAR_SIZE = {"u16": 2, "u32": 4, "u64": 8, "i32": 4, "f32": 4}

VN_OK = 0


class WireProtocolError(RuntimeError):
    pass


# --- layout engine over the embedded table ---

_layout_cache = {}


def layout_of(type_name):
    """Returns (size, align, {field: (offset, spec)}) for a table type."""
    if type_name in _layout_cache:
        return _layout_cache[type_name]
    desc = wire.TYPES[type_name]
    offset = 0
    align = 1
    fields = {}

    def size_align(spec):
        if isinstance(spec, str):
            return SCALAR_SIZE[spec], SCALAR_SIZE[spec]
        if "array" in spec:
            elem, count = spec["array"]
            if isinstance(elem, str) and elem in SCALAR_SIZE:
                es = ea = SCALAR_SIZE[elem]
            else:
                sub = layout_of(elem)
                es, ea = sub[0], sub[1]
            return es * count, ea
        if "struct" in spec:
            sub = layout_of(spec["struct"])
            return sub[0], sub[1]
        if "union" in spec:
            size = 0
            ualign = 1
            for member in spec["union"]:
                sub = layout_of(member)
                size = max(size, sub[0])
                ualign = max(ualign, sub[1])
            size = (size + ualign - 1) // ualign * ualign
            return size, ualign
        if "ptr" in spec:
            return 8, 8
        raise WireProtocolError("unknown field spec: %r" % (spec,))

    for fname, spec in desc["fields"]:
        fsize, falign = size_align(spec)
        offset = (offset + falign - 1) // falign * falign
        fields[fname] = (offset, spec)
        offset += fsize
        align = max(align, falign)
    size = (offset + align - 1) // align * align
    _layout_cache[type_name] = (size, align, fields)
    return _layout_cache[type_name]


def pack_struct(type_name, values):
    """Packs a table struct from a field dict into bytes.

    Scalars take numbers; nested structs take dicts; arrays take lists,
    zero-padded — scalar elements as numbers, struct elements as dicts
    (each packed recursively at its element stride); union fields take
    (member_type, member_dict) or raw bytes; pointer fields are always
    zero in the image (their content travels in sections). Omitted fields
    stay zero. structSize, when the type has one and the caller omits it,
    is stamped with the layout size.
    """
    size, _, fields = layout_of(type_name)
    image = bytearray(size)
    if "structSize" in fields and "structSize" not in values:
        values = dict(values)
        values["structSize"] = size

    for fname, value in values.items():
        if fname not in fields:
            raise WireProtocolError("%s has no field %s" % (type_name, fname))
        offset, spec = fields[fname]
        if isinstance(spec, str):
            struct.pack_into(SCALAR_FMT[spec], image, offset, value)
        elif isinstance(spec, dict) and "struct" in spec:
            sub = pack_struct(spec["struct"], value)
            image[offset:offset + len(sub)] = sub
        elif isinstance(spec, dict) and "array" in spec:
            elem, count = spec["array"]
            if isinstance(elem, str) and elem in SCALAR_FMT:
                fmt = SCALAR_FMT[elem]
                for i, item in enumerate(value):
                    if i >= count:
                        raise WireProtocolError("array overflow in %s" % fname)
                    struct.pack_into(fmt, image,
                                     offset + i * SCALAR_SIZE[elem], item)
            else:
                elem_size = layout_of(elem)[0]
                for i, item in enumerate(value):
                    if i >= count:
                        raise WireProtocolError("array overflow in %s" % fname)
                    sub = pack_struct(elem, item)
                    at = offset + i * elem_size
                    image[at:at + elem_size] = sub
        elif isinstance(spec, dict) and "union" in spec:
            if isinstance(value, (bytes, bytearray)):
                raw = bytes(value)
            else:
                member_type, member_values = value
                raw = pack_struct(member_type, member_values)
            # The layout engine sized the union to its widest member, so a
            # packed member can never exceed the union's slot.
            image[offset:offset + len(raw)] = raw
        elif isinstance(spec, dict) and "ptr" in spec:
            raise WireProtocolError(
                "pointer field %s is section-borne, not image-borne" % fname)
        else:
            raise WireProtocolError("cannot pack field %s" % fname)
    return bytes(image)


def encode_string(value):
    if value is None:
        return struct.pack("<I", wire.NULL_LENGTH)
    raw = value.encode("utf-8")
    return struct.pack("<I", len(raw)) + raw


def encode_desc(type_name, values, sections):
    """Encodes one desc param: declared size + image + sections in
    pointer-field order. `sections` maps pointer field name to either a
    string/None (string sections) or bytes (array sections)."""
    size, _, fields = layout_of(type_name)
    out = struct.pack("<I", size) + pack_struct(type_name, values)
    for fname, (offset, spec) in sorted(fields.items(),
                                        key=lambda kv: kv[1][0]):
        if not (isinstance(spec, dict) and "ptr" in spec):
            continue
        ptr = spec["ptr"]
        content = sections.get(fname)
        if ptr == "string" or (isinstance(ptr, str) and ptr == "string"):
            out += encode_string(content)
        else:
            if content is None:
                out += struct.pack("<I", wire.NULL_LENGTH)
            else:
                out += struct.pack("<I", len(content)) + bytes(content)
    return out


def encode_command(name, *args):
    """Builds one Command frame payload (opcode header + params in table
    order) for a mirrored C API call. Returns (payload, out_count,
    has_rows). Pure encoding — no transport — so the byte stream the
    client sends is directly testable against the table-pinned layouts.
    Out params (out / outu32 / outrows) are omitted from the arguments and
    returned by call() instead; an outrows param additionally marks the
    reply as carrying a trailing row section."""
    fn = wire.FUNCTIONS[name]
    payload = struct.pack("<HH", fn["opcode"], 0)
    arg_iter = iter(args)
    out_count = 0
    has_rows = False
    for param in fn.get("params", []):
        kind = param["wire"]
        if kind in ("out", "outu32"):
            out_count += 1
            continue
        if kind == "outrows":
            has_rows = True
            continue
        value = next(arg_iter)
        if kind == "u64":
            payload += struct.pack("<Q", value)
        elif kind == "u32":
            payload += struct.pack("<I", value)
        elif kind == "f32":
            payload += struct.pack("<f", value)
        elif kind == "vec3":
            payload += struct.pack("<fff", *value)
        elif kind == "string":
            payload += encode_string(value)
        elif kind == "desc":
            values, sections = value
            payload += encode_desc(param["type"], values, sections)
        elif kind == "polydesc":
            raw = bytes(value)
            payload += struct.pack("<I", len(raw)) + raw
        else:
            raise WireProtocolError("unknown wire kind %s" % kind)
    return payload, out_count, has_rows


def unpack_rows(type_name, blob):
    """Decodes an outrows reply section into a list of field dicts using
    the table-pinned layout (stride = the struct's pinned size; the server
    writes rows at its own vintage, which equals the embedded table's)."""
    stride, _, fields = layout_of(type_name)
    if len(blob) % stride != 0:
        raise WireProtocolError(
            "row section length %d is not a multiple of %s stride %d"
            % (len(blob), type_name, stride))
    scalar_fmt = {"u16": "<H", "u32": "<I", "u64": "<Q",
                  "i32": "<i", "f32": "<f"}
    rows = []
    for base in range(0, len(blob), stride):
        row = {}
        for fname, (offset, spec) in fields.items():
            if isinstance(spec, str) and spec in scalar_fmt:
                row[fname] = struct.unpack_from(scalar_fmt[spec], blob,
                                                base + offset)[0]
        rows.append(row)
    return rows


# --- pipe transport ---

class RemoteClient:
    def __init__(self):
        self._pipe = None
        self._sequence = 0
        self.advisories = []  # (msgType, flags, sequence, payload)
        self.welcome = None

    def connect(self, pipe_name, timeout_s=15.0):
        deadline = time.monotonic() + timeout_s
        last_error = None
        while time.monotonic() < deadline:
            try:
                self._pipe = open(pipe_name, "r+b", buffering=0)
                return
            except OSError as error:
                last_error = error
                time.sleep(0.05)
        raise WireProtocolError(
            "cannot connect to %s: %s" % (pipe_name, last_error))

    def close(self):
        if self._pipe is not None:
            try:
                self._pipe.close()
            finally:
                self._pipe = None

    def _write_frame(self, msg_type, payload, flags=0, sequence=None):
        if sequence is None:
            self._sequence += 1
            sequence = self._sequence
        header = struct.pack("<IHHI", len(payload), msg_type, flags, sequence)
        self._pipe.write(header + payload)
        return sequence

    def _read_exact(self, count):
        data = b""
        while len(data) < count:
            chunk = self._pipe.read(count - len(data))
            if not chunk:
                raise WireProtocolError("pipe closed by server")
            data += chunk
        return data

    def read_frame(self):
        header = self._read_exact(wire.FRAME_HEADER_BYTES)
        payload_bytes, msg_type, flags, sequence = struct.unpack("<IHHI",
                                                                 header)
        if payload_bytes > wire.MAX_FRAME_BYTES:
            raise WireProtocolError("server sent oversized frame")
        payload = self._read_exact(payload_bytes) if payload_bytes else b""
        return msg_type, flags, sequence, payload

    def hello(self):
        payload = struct.pack("<HH", wire.PROTOCOL_MAJOR, wire.PROTOCOL_MINOR)
        self._write_frame(wire.MSG["Hello"], payload)
        msg_type, _, _, payload = self.read_frame()
        if msg_type == wire.MSG["HelloReject"]:
            reason = struct.unpack_from("<H", payload)[0]
            raise WireProtocolError("handshake rejected, reason %d" % reason)
        if msg_type != wire.MSG["Welcome"]:
            raise WireProtocolError("expected Welcome, got %d" % msg_type)
        major, minor, rate, block, config_size, version_len = \
            struct.unpack_from("<HHIIII", payload)
        version = payload[20:20 + version_len].decode("utf-8")
        self.welcome = {
            "major": major, "minor": minor, "sampleRate": rate,
            "blockFrames": block, "configStructSize": config_size,
            "version": version,
        }
        return self.welcome

    def _await_reply(self, sequence):
        while True:
            msg_type, flags, seq, payload = self.read_frame()
            if flags & wire.FLAGS["Advisory"]:
                self.advisories.append((msg_type, flags, seq, payload))
                continue
            if seq != sequence:
                continue  # a stale non-advisory frame; keep scanning
            return msg_type, payload

    def call(self, name, *args):
        """Mirrors one C API call. Positional args follow the table
        signature; wire kinds map to python values:
          u64/u32 -> int, f32 -> float, vec3 -> (x, y, z),
          string -> str or None,
          desc -> (values_dict, sections_dict),
          polydesc -> raw struct bytes,
          out params (out / outu32 / outrows) are omitted (returned
          instead).
        Returns (VnResult, [out values]); a function with an outrows param
        appends the decoded row section (raw bytes) as the last out value
        — unpack_rows() turns it into field dicts. Raises on a wire error
        frame.
        """
        fn = wire.FUNCTIONS[name]
        payload, out_count, has_rows = encode_command(name, *args)
        sequence = self._write_frame(wire.MSG["Command"], payload)
        msg_type, reply = self._await_reply(sequence)
        if msg_type == wire.MSG["Error"]:
            code, opcode = struct.unpack_from("<HH", reply)
            raise WireProtocolError(
                "wire error %d for %s (opcode %d)" % (code, name, opcode))
        opcode, _, result = struct.unpack_from("<HHi", reply)
        if opcode != fn["opcode"]:
            raise WireProtocolError("reply opcode mismatch for %s" % name)
        outs = []
        offset = 8
        for _ in range(out_count):
            outs.append(struct.unpack_from("<Q", reply, offset)[0])
            offset += 8
        if has_rows:
            (row_bytes,) = struct.unpack_from("<I", reply, offset)
            offset += 4
            outs.append(bytes(reply[offset:offset + row_bytes]))
        return result, outs

    def check(self, name, *args):
        """call() that raises unless the engine answered VN_OK."""
        result, outs = self.call(name, *args)
        if result != VN_OK:
            raise WireProtocolError("%s returned %d" % (name, result))
        return outs

    def subscribe(self, period_ms):
        payload = struct.pack("<I", period_ms)
        sequence = self._write_frame(wire.MSG["Subscribe"], payload)
        msg_type, reply = self._await_reply(sequence)
        if msg_type != wire.MSG["SubscribeAck"]:
            raise WireProtocolError("expected SubscribeAck, got %d" % msg_type)
        return struct.unpack_from("<I", reply)[0]

    def drain_advisories(self, duration_s):
        """Reads frames for a wall-clock window, accumulating advisories.
        Non-advisory frames arriving here are unexpected and raise."""
        deadline = time.monotonic() + duration_s
        while time.monotonic() < deadline:
            msg_type, flags, seq, payload = self.read_frame()
            if flags & wire.FLAGS["Advisory"]:
                self.advisories.append((msg_type, flags, seq, payload))
            else:
                raise WireProtocolError(
                    "unexpected non-advisory frame %d" % msg_type)

    def snapshots(self):
        return [p for (t, _, _, p) in self.advisories
                if t == wire.MSG["Snapshot"]]

    def notifications(self):
        return [p for (t, _, _, p) in self.advisories
                if t == wire.MSG["Notification"]]


def snapshot_field_offset(field_name):
    """Byte offset of a field inside the VnSnapshot wire image."""
    _, _, fields = layout_of("VnSnapshot")
    return fields[field_name][0]
