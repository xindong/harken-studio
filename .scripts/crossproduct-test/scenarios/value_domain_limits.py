import math

from drivers.studio_cli import StudioCommandError
from scenarios.authoring import export_all, exported_actions, inspector_fields, write_tone

PLAY_BLOCKS = 40
SCHEMAS = ("Sound", "Container")


def bounded_fields(studio, project, schema_id: str, settable: set) -> dict:
    fields = inspector_fields(studio, project, schema_id)
    return {name: (field["min"], field["max"]) for name, field in fields.items()
            if name in settable and "min" in field and "max" in field and not field.get("readOnly")}


def stored_value(studio, project, path: str, field: str):
    found = []

    def walk(node):
        if isinstance(node, dict):
            for key, value in node.items():
                if key == field and isinstance(value, (int, float)) and not isinstance(value, bool):
                    found.append(value)
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(studio.call(project, "get_item_info", {"path": path}).get("components", {}))
    if len(found) != 1:
        raise AssertionError(f"{path}.{field} is not uniquely readable from get_item_info: {found}")
    return found[0]


def run(ctx) -> None:
    ctx.require("studio-cli", "engine-release")
    studio = ctx.studio()
    project = studio.create_project(ctx.work / "Project")
    settable = set(studio.command_schema("set_item_property")["inputSchema"]["properties"]["property"]["enum"])
    tone = write_tone(ctx.work / "tone.wav", 440, 0.3)
    layer = studio.call(project, "create_item", {"name": "Limits", "type": "Layer"}, save=True)["path"]

    limits, unclamped, uncovered, contract_refused, names = {}, [], [], [], []
    for schema_id in SCHEMAS:
        fields = bounded_fields(studio, project, schema_id, settable)
        for edge, pick in (("Min", 0), ("Max", 1)):
            name = f"{schema_id}{edge}"
            path = studio.call(project, "create_item", {"parent": layer, "name": name, "type": schema_id},
                               save=True)["path"]
            sound_parent = path
            if schema_id == "Container":
                sound_parent = studio.call(project, "create_item", {"parent": path, "name": f"{name}_Child",
                                                                    "type": "Sound"}, save=True)["path"]
            studio.call(project, "import_audio", {"path": str(tone), "pack": "Limits",
                                                  "target_parent_path": sound_parent}, save=True)
            for field in list(fields):
                bound = fields[field][pick]
                outside = bound - abs(bound or 1) if pick == 0 else bound + abs(bound or 1)
                try:
                    studio.call(project, "set_item_property",
                                {"path": path, "property": field, "value": outside}, save=True)
                except StudioCommandError as error:
                    if error.code == "INVALID_ARGUMENTS":
                        contract_refused.append(f"{schema_id}.{field}={outside}")
                        studio.call(project, "set_item_property",
                                    {"path": path, "property": field, "value": bound}, save=True)
                        continue
                    if error.code != "FIELD_GROUP_COMMAND_REQUIRED":
                        raise
                    uncovered.append(f"{schema_id}.{field}")
                    del fields[field]
                    continue
                stored = stored_value(studio, project, path, field)
                if not math.isclose(stored, bound, rel_tol=1e-6, abs_tol=1e-6):
                    unclamped.append(f"{schema_id}.{field}: wrote {outside}, stored {stored}, bound {bound}")
            limits[schema_id] = fields
            names.append(name)
            studio.run(project, [
                {"cmd": "create_action", "args": {"name": f"Play{name}"}},
                {"cmd": "set_item_property", "args": {"path": f"Play{name}", "property": "steps",
                                                      "value": [{"type": "Play", "targetName": name}]}},
            ], save=True)
    studio.call(project, "create_bus", {"name": "LimitBus"}, save=True)
    bus_range = inspector_fields(studio, project, "Bus")["volume"]
    for bound, outside in ((bus_range["max"], bus_range["max"] + 100), (bus_range["min"], bus_range["min"] - 100)):
        studio.call(project, "set_bus_volume_db", {"bus_name": "LimitBus", "db": outside}, save=True)
        stored = next(bus["volume"] for bus in studio.call(project, "list_buses")["buses"] if bus["name"] == "LimitBus")
        if not math.isclose(stored, bound, abs_tol=1e-6):
            unclamped.append(f"Bus.volume: wrote {outside}, stored {stored}, bound {bound}")
    ctx.details.update(limits=limits, unclamped=unclamped, refused_by_command_contract=contract_refused,
                       not_covered_field_group_route=sorted(set(uncovered)))
    if unclamped:
        raise AssertionError(f"Studio stored values outside its published range: {unclamped}")

    directory = export_all(studio, project, ctx.work / "Export")
    actions = exported_actions(directory)
    played = [f"Play{name}" for name in names]
    plan = {
        "init_pack": str(directory / "Init.bytes"),
        "content_packs": [{"path": actions[name]["pack"], "root_action": actions[name]["id"]} for name in played],
        "actions": [{"name": name, "id": actions[name]["id"], "blocks": PLAY_BLOCKS} for name in played],
    }
    editions = ["release"] + (["development"] if ctx.capabilities["engine-development"]["available"] else [])
    for edition in editions:
        result = ctx.engine({**plan, "edition": edition, "measure": False})
        ctx.details[edition] = {a["name"]: a["max_live_playables"] for a in result["actions"]}
        idle = [a["name"] for a in result["actions"] if a["max_live_playables"] < 1]
        if idle:
            raise AssertionError(f"{edition} edition accepted the Packs but started nothing for {idle}")
