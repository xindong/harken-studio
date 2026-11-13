import ctypes
import json
import subprocess
import sys
import time
from pathlib import Path

EDITIONS = {"release": "HarkenEngine", "development": "HarkenEngineDev"}
SCALARS = {
    "u8": ctypes.c_uint8, "u16": ctypes.c_uint16, "u32": ctypes.c_uint32, "u64": ctypes.c_uint64,
    "i32": ctypes.c_int32, "f32": ctypes.c_float, "f64": ctypes.c_double,
}
C_NAMES = {
    "uint8_t": ctypes.c_uint8, "uint16_t": ctypes.c_uint16, "uint32_t": ctypes.c_uint32,
    "uint64_t": ctypes.c_uint64, "int32_t": ctypes.c_int32, "int64_t": ctypes.c_int64,
    "float": ctypes.c_float, "double": ctypes.c_double, "char": ctypes.c_char, "void": None,
}


def library_name(edition: str) -> str:
    stem = EDITIONS[edition]
    if sys.platform == "win32":
        return f"bin/{stem}.dll"
    if sys.platform == "darwin":
        return f"lib/lib{stem}.dylib"
    return f"lib/lib{stem}.so"


def manifest_path(install: Path) -> Path:
    return install / "share" / "HarkenEngine" / "harken_abi_manifest.json"


class Abi:
    def __init__(self, install: Path, edition: str):
        manifest = json.loads(manifest_path(install).read_text(encoding="utf-8"))
        self.native = manifest["contract"]["nativeAbi"]
        self.edition = edition
        self.library = ctypes.CDLL(str(install / library_name(edition)))
        self.constants = {}
        for name, value in self.native["defines"].items():
            self.constants[name] = value["value"]
        for enum in self.native["enums"].values():
            self.constants.update(enum["values"])
        self.constants.update(self.native["results"]["values"])
        self.results = {value: name for name, value in self.native["results"]["values"].items()}
        self._types = {}
        self._functions = {entry["name"]: entry for entry in self.native["functions"]}
        self._callbacks = {}

    def type(self, name: str):
        if name in self._types:
            return self._types[name]
        if name in SCALARS:
            return SCALARS[name]
        if name in C_NAMES:
            return C_NAMES[name]
        if name in self.native["scalarTypedefs"]:
            resolved = SCALARS[self.native["scalarTypedefs"][name]]
        elif name in self.native["structTypedefs"]:
            resolved = self.type(self.native["structTypedefs"][name])
        elif name in self.native["callbacks"]:
            resolved = self.callback(name)
        elif name in self.native["types"]:
            resolved = self._struct(name, self.native["types"][name])
        else:
            raise KeyError(f"{name} is not described by the published ABI manifest")
        self._types[name] = resolved
        return resolved

    def _shape(self, shape):
        kind = shape["kind"]
        if kind == "scalar":
            return SCALARS[shape["type"]]
        if kind == "struct":
            return self.type(shape["type"])
        if kind == "pointer":
            return ctypes.c_void_p
        if kind == "array":
            return self.type(shape["element"]) * shape["count"]
        raise KeyError(f"unsupported ABI field shape {kind}")

    def _struct(self, name, description):
        fields = []
        cursor = 0
        for index, field in enumerate(description["fields"]):
            if field["offset"] > cursor:
                fields.append((f"_pad{index}", ctypes.c_uint8 * (field["offset"] - cursor)))
            fields.append((field["name"], self._shape(field["shape"])))
            cursor = field["offset"] + field["size"]
        if description["size"] > cursor:
            fields.append(("_tail", ctypes.c_uint8 * (description["size"] - cursor)))
        structure = type(name, (ctypes.Structure,), {"_pack_": 1, "_fields_": fields})
        if ctypes.sizeof(structure) != description["size"]:
            raise RuntimeError(f"{name}: binding size {ctypes.sizeof(structure)} != manifest size {description['size']}")
        for field in description["fields"]:
            if getattr(structure, field["name"]).offset != field["offset"]:
                raise RuntimeError(f"{name}.{field['name']}: binding offset differs from manifest")
        return structure

    def ctype(self, text: str):
        text = text.replace("const ", "").strip()
        depth = text.count("*")
        base = text.replace("*", "").strip()
        if depth == 0:
            return self.type(base)
        if base in ("void", "char") or depth > 1:
            return ctypes.c_void_p if base == "void" or depth > 1 else ctypes.c_char_p
        return ctypes.POINTER(self.type(base))

    def callback(self, name: str):
        if name not in self._callbacks:
            entry = self.native["callbacks"][name]
            self._callbacks[name] = ctypes.CFUNCTYPE(
                self.ctype(entry["returnType"]), *(self.ctype(p["ctype"]) for p in entry["params"]))
        return self._callbacks[name]

    def function(self, name: str):
        entry = self._functions.get(name)
        if entry is None:
            raise KeyError(f"{name} is not described by the published ABI manifest")
        if entry["edition"] == "development" and self.edition != "development":
            raise KeyError(f"{name} belongs to the development edition")
        native = getattr(self.library, name)
        native.argtypes = [self.ctype(p["ctype"]) for p in entry["parameters"]]
        native.restype = self.ctype(entry["returnType"])
        return native

    def new(self, name: str, **values):
        value = self.type(name)()
        if hasattr(value, "structSize"):
            value.structSize = ctypes.sizeof(value)
        for key, item in values.items():
            setattr(value, key, item)
        return value

    def call(self, name: str, *arguments) -> int:
        return self.function(name)(*arguments)

    def check(self, name: str, *arguments) -> None:
        result = self.call(name, *arguments)
        if result != self.constants["VN_OK"]:
            raise EngineError(name, result, self.results.get(result, str(result)))

    def object_id(self, text: str):
        digits = text.replace("-", "").strip("{}")
        if len(digits) != 32:
            raise ValueError(f"not an object id: {text}")
        return self.new("VnObjectId", high=int(digits[:16], 16), low=int(digits[16:], 16))


class EngineError(RuntimeError):
    def __init__(self, entry: str, code: int, name: str):
        super().__init__(f"{entry} returned {name} ({code})")
        self.entry, self.code, self.name = entry, code, name


class Packs:
    def __init__(self, abi: Abi):
        self.abi = abi
        self.buffers = {}
        span = abi.type("VnPackByteSpan")

        def provide(_, stable_id, _locator, _variant, _hash, out):
            buffer = self.buffers.get(stable_id)
            if buffer is None:
                return abi.constants["VN_CONTENT_NOT_LOADED"]
            out.contents.structSize = ctypes.sizeof(span)
            out.contents.bytes = ctypes.cast(buffer, ctypes.c_void_p)
            out.contents.byteCount = len(buffer)
            return abi.constants["VN_OK"]

        self.provide = abi.callback("VnPackByteProviderFn")(provide)
        self.release = abi.callback("VnPackByteReleaseFn")(lambda *_: None)

    def span(self, buffer):
        return self.abi.new("VnPackByteSpan", bytes=ctypes.cast(buffer, ctypes.c_void_p), byteCount=len(buffer))

    def inspect(self, path: Path, init_buffer, root_action: str | None):
        data = path.read_bytes()
        buffer = ctypes.create_string_buffer(data, len(data))
        desc = self.abi.new("VnPackInspectDescV1", pack=self.span(buffer),
                            maxInputBytes=len(data) + (len(init_buffer) if init_buffer else 0) + 1024)
        if init_buffer is not None:
            desc.init = self.span(init_buffer)
        if root_action:
            desc.rootActionId = self.abi.object_id(root_action)
        report = self.abi.new("VnPackInspectReportV1")
        self.abi.check("Vn_InspectCanonicalPackV1", ctypes.byref(desc), ctypes.byref(report))
        self.buffers[report.packStableId] = buffer
        return buffer, report

    def activate(self, init_report, content_reports) -> None:
        reports = [init_report, *sorted(content_reports, key=lambda report: report.packStableId)]
        count = len(reports)
        members = (self.abi.type("VnCanonicalPackSetMemberV2") * count)()
        results = (self.abi.type("VnCanonicalPackSetMemberResultV1") * count)()
        for member, result, report in zip(members, results, reports):
            member.structSize = ctypes.sizeof(member)
            member.operation = self.abi.constants["VN_PACK_MEMBER_CONTENT"]
            member.source.structSize = ctypes.sizeof(member.source)
            member.source.flags = self.abi.constants["VN_CANONICAL_PACK_SET_MEMBER_PROVIDER"]
            member.source.packStableId = report.packStableId
            member.source.packKind = report.packKind
            member.source.provide = ctypes.cast(self.provide, ctypes.c_void_p)
            member.source.release = ctypes.cast(self.release, ctypes.c_void_p)
            result.structSize = ctypes.sizeof(result)
        desc = self.abi.new("VnCanonicalPackSetActivationDescV2", setRevision=1, memberCount=count,
                            memberStructSize=ctypes.sizeof(members[0]),
                            members=ctypes.cast(members, ctypes.c_void_p), outputMemberCapacity=count,
                            outputMemberStructSize=ctypes.sizeof(results[0]),
                            outputMembers=ctypes.cast(results, ctypes.c_void_p))
        report = self.abi.new("VnCanonicalPackSetActivationReportV1")
        self.abi.check("Vn_ActivateCanonicalPackSetV2", ctypes.byref(desc), ctypes.byref(report))


def play_session(plan: dict) -> dict:
    abi = Abi(Path(plan["install"]), plan["edition"])
    outcome = {"edition": plan["edition"], "actions": []}
    version = abi.new("VnAbiVersionInfo")
    abi.check("Vn_QueryAbiVersion", ctypes.byref(version))
    outcome["abi"] = [version.major, version.minor]
    master = abi.constants["VN_BUS_MASTER"]
    outcome["master_bus"] = f"{master['high']:016x}{master['low']:016x}"
    if version.major != abi.constants["VN_ABI_VERSION_MAJOR"]:
        raise RuntimeError(f"library ABI major {version.major} differs from manifest {abi.constants['VN_ABI_VERSION_MAJOR']}")
    config = abi.new("VnConfig", sampleRate=plan["sample_rate"], blockFrames=plan["block_frames"],
                     deviceMode=abi.constants["VN_DEVICE_NULL"])
    abi.check("Vn_Init", ctypes.byref(config))
    try:
        packs = Packs(abi)
        init_buffer, init_report = packs.inspect(Path(plan["init_pack"]), None, None)
        contents = [packs.inspect(Path(content["path"]), init_buffer, content["root_action"])[1]
                    for content in plan["content_packs"]]
        packs.activate(init_report, contents)
        meter = Meter(abi) if plan["measure"] else None
        if "serve" in plan:
            outcome["serve"] = serve(abi, meter, plan)
        for action in plan["actions"]:
            outcome["actions"].append(play_action(abi, meter, action, plan))
    finally:
        abi.call("Vn_Term")
    return outcome


class Meter:
    def __init__(self, abi: Abi):
        self.abi = abi
        abi.check("VnPreview_EnableChannelPeakMetering")
        self.pump = abi.function("VnPreview_PumpNullMixer")
        self.peaks = abi.new("VnPreviewChannelPeaksV1")

    def block(self) -> dict:
        self.pump(1)
        self.abi.check("VnPreview_TakeChannelPeaks", ctypes.byref(self.peaks))
        stride = self.abi.constants["VN_MAX_CHANNELS"]
        buses = {}
        for row in range(self.peaks.busCount):
            bus = self.peaks.busIds[row]
            channels = self.peaks.channelPeaks[row * stride:row * stride + self.peaks.mixChannelCount]
            buses[f"{bus.high:016x}{bus.low:016x}"] = max(channels, default=0.0)
        return buses


def play_action(abi: Abi, meter, action: dict, plan: dict) -> dict:
    desc = abi.new("VnPostActionDesc", actionId=abi.object_id(action["id"]), callerSeed=action.get("seed", 1))
    firing = abi.type("VnActionFiringHandle")()
    abi.check("Vn_PostAction", ctypes.byref(desc), ctypes.byref(firing))
    info = abi.new("VnActionFiringInfo")
    bus_peaks = {}
    max_live = 0
    for _ in range(action["blocks"]):
        abi.check("Vn_Update")
        if meter is not None:
            for bus, peak in meter.block().items():
                bus_peaks[bus] = max(bus_peaks.get(bus, 0.0), peak)
        abi.check("Vn_QueryActionFiring", firing, ctypes.byref(info))
        max_live = max(max_live, info.livePlayableCount)
    abi.call("Vn_StopActionFiring", firing, 0)
    for _ in range(4):
        abi.call("Vn_Update")
    abi.call("Vn_ReleaseActionFiring", firing)
    return {"name": action["name"], "outcome": info.outcome, "state": info.state,
            "max_live_playables": max_live, "bus_peaks": bus_peaks,
            "peak": max(bus_peaks.values(), default=0.0)}


def serve(abi: Abi, meter, plan: dict) -> dict:
    serve_plan = plan["serve"]
    application = ctypes.create_string_buffer(b"crossproduct-test")
    config = abi.new("VnRemoteConfig", applicationName=ctypes.cast(application, ctypes.c_void_p))
    abi.check("Vn_RemoteStart", ctypes.byref(config))
    status = abi.new("VnRemoteStatus")
    abi.check("Vn_RemoteGetStatus", ctypes.byref(status))
    Path(serve_plan["ready_file"]).write_text(json.dumps({"port": status.tcpPort}), encoding="utf-8")
    action = abi.new("VnPostActionDesc", actionId=abi.object_id(serve_plan["action_id"]), callerSeed=1)
    firing = abi.type("VnActionFiringHandle")()
    blocks_per_post = max(1, int(serve_plan["post_interval"] * plan["sample_rate"] / plan["block_frames"]))
    posts = 0
    started = time.monotonic()
    block = 0
    while time.monotonic() - started < serve_plan["seconds"] and not Path(serve_plan["stop_file"]).exists():
        if block % blocks_per_post == 0 and abi.call("Vn_PostAction", ctypes.byref(action), ctypes.byref(firing)) == 0:
            abi.call("Vn_ReleaseActionFiring", firing)
            posts += 1
        abi.call("Vn_Update")
        if meter is not None:
            meter.block()
        block += 1
        time.sleep(plan["block_frames"] / plan["sample_rate"])
    abi.check("Vn_RemoteStop")
    return {"port": status.tcpPort, "posts": posts}


def spawn_session(python: str, plan: dict, work: Path):
    work.mkdir(parents=True, exist_ok=True)
    stem = f"engine-{plan['edition']}-serve"
    plan_file, result_file = work / f"{stem}-plan.json", work / f"{stem}-result.json"
    plan_file.write_text(json.dumps(plan, indent=2), encoding="utf-8")
    log = (work / f"{stem}.log").open("wb")
    from drivers.process import hidden_process_options
    process = subprocess.Popen([python, "-I", str(Path(__file__).resolve()), str(plan_file), str(result_file)],
                               stdout=log, stderr=subprocess.STDOUT, **hidden_process_options())
    return process, result_file


def run_session(python: str, plan: dict, work: Path, timeout: float) -> dict:
    from drivers.process import run_bounded

    work.mkdir(parents=True, exist_ok=True)
    stem = f"engine-{plan['edition']}"
    plan_file, result_file, log = work / f"{stem}-plan.json", work / f"{stem}-result.json", work / f"{stem}.log"
    plan_file.write_text(json.dumps(plan, indent=2), encoding="utf-8")
    completed = run_bounded([python, "-I", str(Path(__file__).resolve()), str(plan_file), str(result_file)],
                            timeout=timeout, log=log)
    if completed != 0 or not result_file.exists():
        raise RuntimeError(f"engine session exited with {completed}; see {log}")
    result = json.loads(result_file.read_text(encoding="utf-8"))
    if "error" in result:
        raise RuntimeError(result["error"])
    return result


def main() -> int:
    plan_file, result_file = Path(sys.argv[1]), Path(sys.argv[2])
    try:
        result = play_session(json.loads(plan_file.read_text(encoding="utf-8")))
    except Exception as error:
        result = {"error": f"{type(error).__name__}: {error}"}
    result_file.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
