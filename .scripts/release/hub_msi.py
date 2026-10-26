from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import uuid
from pathlib import Path

from release.windows_installer import Database, file_metadata

UPGRADE_CODE = "{4EA0003D-209E-45E6-8343-67529D7E7E5A}"
COMPONENT_NAMESPACE = uuid.UUID("9b1a7f66-52a4-4f6e-9e01-0f30c2a7f4b3")
HANDOFF_SDDL = "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OIIO;FA;;;CO)(A;;0x1200ab;;;BU)"
UPDATER_SDDL = "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)"

SCHEMA = {
    "Icon": ("Name CHAR(72) NOT NULL, Data OBJECT NOT NULL", "Name"),
    "Property": ("Property CHAR(72) NOT NULL, Value CHAR(0) NOT NULL", "Property"),
    "Directory": ("Directory CHAR(72) NOT NULL, Directory_Parent CHAR(72), DefaultDir CHAR(255) NOT NULL", "Directory"),
    "Component": ("Component CHAR(72) NOT NULL, ComponentId CHAR(38), Directory_ CHAR(72) NOT NULL, Attributes SHORT NOT NULL, Condition CHAR(255), KeyPath CHAR(72)", "Component"),
    "Feature": ("Feature CHAR(38) NOT NULL, Feature_Parent CHAR(38), Title CHAR(64), Description CHAR(255), Display SHORT, Level SHORT NOT NULL, Directory_ CHAR(72), Attributes SHORT NOT NULL", "Feature"),
    "FeatureComponents": ("Feature_ CHAR(38) NOT NULL, Component_ CHAR(72) NOT NULL", "Feature_, Component_"),
    "File": ("File CHAR(72) NOT NULL, Component_ CHAR(72) NOT NULL, FileName CHAR(255) NOT NULL, FileSize LONG NOT NULL, Version CHAR(72), Language CHAR(20), Attributes SHORT, Sequence LONG NOT NULL", "File"),
    "MsiFileHash": ("File_ CHAR(72) NOT NULL, Options SHORT NOT NULL, HashPart1 LONG NOT NULL, HashPart2 LONG NOT NULL, HashPart3 LONG NOT NULL, HashPart4 LONG NOT NULL", "File_"),
    "Media": ("DiskId SHORT NOT NULL, LastSequence LONG NOT NULL, DiskPrompt CHAR(64), Cabinet CHAR(255), VolumeLabel CHAR(32), Source CHAR(72)", "DiskId"),
    "Registry": ("Registry CHAR(72) NOT NULL, Root SHORT NOT NULL, Key CHAR(255) NOT NULL, Name CHAR(255), Value CHAR(0), Component_ CHAR(72) NOT NULL", "Registry"),
    "Shortcut": ("Shortcut CHAR(72) NOT NULL, Directory_ CHAR(72) NOT NULL, Name CHAR(128) NOT NULL, Component_ CHAR(72) NOT NULL, Target CHAR(72) NOT NULL, Arguments CHAR(255), Description CHAR(255), Hotkey SHORT, Icon_ CHAR(72), IconIndex SHORT, ShowCmd SHORT, WkDir CHAR(72)", "Shortcut"),
    "MsiShortcutProperty": ("MsiShortcutProperty CHAR(72) NOT NULL, Shortcut_ CHAR(72) NOT NULL, PropertyKey CHAR(255) NOT NULL, PropVariantValue CHAR(255) NOT NULL", "MsiShortcutProperty"),
    "Upgrade": ("UpgradeCode CHAR(38) NOT NULL, VersionMin CHAR(20), VersionMax CHAR(20), Language CHAR(255), Attributes LONG NOT NULL, Remove CHAR(255), ActionProperty CHAR(72) NOT NULL", "UpgradeCode, VersionMin, VersionMax, Language, Attributes"),
    "AppSearch": ("Property CHAR(72) NOT NULL, Signature_ CHAR(72) NOT NULL", "Property, Signature_"),
    "RegLocator": ("Signature_ CHAR(72) NOT NULL, Root SHORT NOT NULL, Key CHAR(255) NOT NULL, Name CHAR(255), Type SHORT", "Signature_"),
    "Signature": ("Signature CHAR(72) NOT NULL, FileName CHAR(255) NOT NULL, MinVersion CHAR(20), MaxVersion CHAR(20), MinSize LONG, MaxSize LONG, MinDate LONG, MaxDate LONG, Languages CHAR(255)", "Signature"),
    "CustomAction": ("Action CHAR(72) NOT NULL, Type SHORT NOT NULL, Source CHAR(72), Target CHAR(255)", "Action"),
    "InstallExecuteSequence": ("Action CHAR(72) NOT NULL, Condition CHAR(255), Sequence SHORT", "Action"),
    "InstallUISequence": ("Action CHAR(72) NOT NULL, Condition CHAR(255), Sequence SHORT", "Action"),
    "AdminExecuteSequence": ("Action CHAR(72) NOT NULL, Condition CHAR(255), Sequence SHORT", "Action"),
    "LaunchCondition": ("Condition CHAR(255) NOT NULL, Description CHAR(255) NOT NULL", "Condition"),
    "CreateFolder": ("Directory_ CHAR(72) NOT NULL, Component_ CHAR(72) NOT NULL", "Directory_, Component_"),
    "MsiLockPermissionsEx": ("MsiLockPermissionsEx CHAR(72) NOT NULL, LockObject CHAR(72) NOT NULL, Table CHAR(32) NOT NULL, SDDLText CHAR(0) NOT NULL, Condition CHAR(255)", "MsiLockPermissionsEx"),
    "Dialog": ("Dialog CHAR(72) NOT NULL, HCentering SHORT NOT NULL, VCentering SHORT NOT NULL, Width SHORT NOT NULL, Height SHORT NOT NULL, Attributes LONG, Title CHAR(128), Control_First CHAR(50) NOT NULL, Control_Default CHAR(50), Control_Cancel CHAR(50)", "Dialog"),
    "Control": ("Dialog_ CHAR(72) NOT NULL, Control CHAR(50) NOT NULL, Type CHAR(20) NOT NULL, X SHORT NOT NULL, Y SHORT NOT NULL, Width SHORT NOT NULL, Height SHORT NOT NULL, Attributes LONG, Property CHAR(72), Text CHAR(0), Control_Next CHAR(50), Help CHAR(50)", "Dialog_, Control"),
    "ControlEvent": ("Dialog_ CHAR(72) NOT NULL, Control_ CHAR(50) NOT NULL, Event CHAR(50) NOT NULL, Argument CHAR(255) NOT NULL, Condition CHAR(255), Ordering SHORT", "Dialog_, Control_, Event, Argument, Condition"),
    "ControlCondition": ("Dialog_ CHAR(72) NOT NULL, Control_ CHAR(50) NOT NULL, Action CHAR(50) NOT NULL, Condition CHAR(255) NOT NULL", "Dialog_, Control_, Action, Condition"),
    "CheckBox": ("Property CHAR(72) NOT NULL, Value CHAR(64)", "Property"),
    "TextStyle": ("TextStyle CHAR(72) NOT NULL, FaceName CHAR(32) NOT NULL, Size SHORT NOT NULL, Color LONG, StyleBits SHORT", "TextStyle"),
    "EventMapping": ("Dialog_ CHAR(72) NOT NULL, Control_ CHAR(50) NOT NULL, Event CHAR(50) NOT NULL, Attribute CHAR(50) NOT NULL", "Dialog_, Control_, Event"),
}


def stable_id(prefix: str, rel_path: str) -> str:
    return f"{prefix}_{uuid.uuid5(COMPONENT_NAMESPACE, rel_path).hex}"


def component_guid(rel_path: str) -> str:
    return "{" + str(uuid.uuid5(COMPONENT_NAMESPACE, f"component/{rel_path.lower()}")).upper() + "}"


def directory_id(rel_dir: str) -> str:
    return "INSTALLFOLDER" if rel_dir == "." else stable_id("PayloadDir", f"dir/{rel_dir.lower()}")


def msi_name(name: str, identity: str) -> str:
    if re.fullmatch(r"[A-Za-z0-9_]{1,8}(\.[A-Za-z0-9_]{1,3})?", name):
        return name
    return uuid.uuid5(COMPONENT_NAMESPACE, identity).hex[:8] + "|" + name


def installer_ui(tables, license_rtf):
    tables["Property"] += [("DefaultUIFont", "HubNormal")]
    tables["TextStyle"] = [("HubNormal", "Segoe UI", 9, 0, 0), ("HubTitle", "Segoe UI", 12, 0, 1)]
    tables["CheckBox"] = [("LICENSEACCEPTED", "1")]

    def dialog(name, title, first, default, cancel, modal=True):
        tables["Dialog"].append((name, 50, 50, 420, 320, 3 if modal else 1,
                                 "[ProductName] Setup", first, default, cancel))
        control(name, "Title", "Text", (18, 14, 384, 25), "{\\HubTitle}" + title, attributes=3)

    def control(dialog, name, kind, bounds, text=None, prop=None, next=None, attributes=3):
        tables["Control"].append((dialog, name, kind, *bounds, attributes, prop, text, next, None))

    def event(dialog, button, event, value, condition="1", order=1):
        tables["ControlEvent"].append((dialog, button, event, value, condition, order))

    def cancel(dialog, next):
        control(dialog, "Cancel", "PushButton", (330, 285, 72, 20), "Cancel", next=next)
        event(dialog, "Cancel", "EndDialog", "Exit")

    dialog("LicenseDialog", "License Agreement and Privacy Policy", "Accept", "Next", "Cancel")
    control("LicenseDialog", "Terms", "ScrollableText", (18, 48, 384, 196), license_rtf, attributes=7)
    control("LicenseDialog", "Accept", "CheckBox", (18, 254, 384, 20),
            "I accept the license agreement and privacy policy.", "LICENSEACCEPTED", "Next")
    control("LicenseDialog", "Next", "PushButton", (250, 285, 72, 20), "Next", next="Cancel")
    cancel("LicenseDialog", "Accept")
    event("LicenseDialog", "Next", "NewDialog", "FolderDialog", 'LICENSEACCEPTED="1"')
    tables["ControlCondition"] += [
        ("LicenseDialog", "Next", "Enable", 'LICENSEACCEPTED="1"'),
        ("LicenseDialog", "Next", "Disable", 'NOT LICENSEACCEPTED="1"'),
    ]
    dialog("FolderDialog", "Install Harken Hub", "Next", "Next", "Cancel")
    control("FolderDialog", "Description", "Text", (18, 55, 384, 42),
            "Choose the folder for a new installation. Upgrades keep the existing folder, projects, and settings.")
    control("FolderDialog", "Path", "PathEdit", (18, 112, 384, 20), prop="INSTALLFOLDER", next="Back")
    tables["ControlCondition"] += [
        ("FolderDialog", "Path", "Enable", "NOT HARKENPREVIOUSINSTALLDIR"),
        ("FolderDialog", "Path", "Disable", "HARKENPREVIOUSINSTALLDIR"),
    ]
    control("FolderDialog", "Back", "PushButton", (170, 285, 72, 20), "Back", next="Next")
    event("FolderDialog", "Back", "NewDialog", "LicenseDialog")
    control("FolderDialog", "Next", "PushButton", (250, 285, 72, 20), "Install", next="Cancel")
    cancel("FolderDialog", "Path")
    event("FolderDialog", "Next", "SetTargetPath", "INSTALLFOLDER")
    event("FolderDialog", "Next", "EndDialog", "Return", order=2)

    dialog("MaintenanceDialog", "Change the existing installation", "Repair", "Repair", "Cancel")
    control("MaintenanceDialog", "Description", "Text", (18, 55, 384, 55),
            "Repair reinstalls missing or damaged program files. Remove uninstalls Harken Hub and keeps your projects and settings.")
    control("MaintenanceDialog", "Repair", "PushButton", (18, 130, 120, 24), "Repair", next="Remove")
    control("MaintenanceDialog", "Remove", "PushButton", (152, 130, 120, 24), "Remove", next="Cancel")
    control("MaintenanceDialog", "Cancel", "PushButton", (330, 285, 72, 20), "Cancel", next="Repair")
    event("MaintenanceDialog", "Repair", "[REINSTALL]", "ALL")
    event("MaintenanceDialog", "Repair", "[REINSTALLMODE]", "amus", order=2)
    event("MaintenanceDialog", "Repair", "EndDialog", "Return", order=3)
    event("MaintenanceDialog", "Remove", "[REMOVE]", "ALL")
    event("MaintenanceDialog", "Remove", "EndDialog", "Return", order=2)
    event("MaintenanceDialog", "Cancel", "EndDialog", "Exit")

    dialog("ProgressDialog", "Applying installation changes", "Status", None, None, modal=False)
    control("ProgressDialog", "Status", "Text", (18, 70, 384, 45), "Please wait while setup updates the installation.")
    control("ProgressDialog", "Progress", "ProgressBar", (18, 135, 384, 15))
    tables["EventMapping"] += [("ProgressDialog", "Status", "ActionText", "Text"),
                               ("ProgressDialog", "Progress", "SetProgress", "Progress")]
    for name, title in [("SuccessDialog", "Setup completed"),
                        ("FailureDialog", "Setup failed"), ("CancelledDialog", "Setup was cancelled")]:
        dialog(name, title, "Close", "Close", "Close")
        control(name, "Close", "PushButton", (330, 285, 72, 20), "Close", next="Close")
        event(name, "Close", "EndDialog", "Return")


def package_tables(files, input_dir, version, license_rtf):
    tables = {name: [] for name in SCHEMA}
    tables["Property"] = [
        ("ALLUSERS", "1"), ("Manufacturer", "XD"), ("ProductName", "Harken Hub"),
        ("ProductCode", "{" + str(uuid.uuid4()).upper() + "}"), ("ProductLanguage", "1033"),
        ("ProductVersion", version), ("UpgradeCode", UPGRADE_CODE), ("ARPNOMODIFY", "1"),
        ("ARPPRODUCTICON", "HubIcon.ico"),
        ("SecureCustomProperties", "HARKENBRIDGE;HARKENSELFUPDATE;INSTALLFOLDER;HARKENPREVIOUSINSTALLDIR;HARKEN_UPGRADE_DETECTED;HARKEN_DOWNGRADE_DETECTED"),
    ]
    tables["Icon"] = [("HubIcon.ico", Path(__file__).resolve().parents[2] / "HarkenHub/Resources/icons/HubIcon.ico")]
    tables["Directory"] = [
        ("TARGETDIR", None, "SourceDir"), ("ProgramFiles64Folder", "TARGETDIR", "PFiles64"),
        ("INSTALLFOLDER", "ProgramFiles64Folder", "Hub|HarkenHub"),
        ("ProgramMenuFolder", "TARGETDIR", "PMenu"), ("DesktopFolder", "TARGETDIR", "Desktop"),
        ("CommonAppDataFolder", "TARGETDIR", "CommApp"),
        ("HarkenSoundDataDir", "CommonAppDataFolder", "Harken|HarkenSound"),
        ("HubDataDir", "HarkenSoundDataDir", "Hub|HarkenHub"),
        ("HandoffDir", "HubDataDir", "handoff"), ("PinsDir", "HubDataDir", "pins"),
        ("UpdaterDir", "HubDataDir", "Updater"),
    ]
    parents = set()
    for file in files:
        parents.update(file.relative_to(input_dir).parents)
    for directory in sorted(parents):
        if directory == Path("."):
            continue
        name = directory.as_posix()
        tables["Directory"].append((directory_id(name), directory_id(directory.parent.as_posix()),
                                     msi_name(directory.name, "dir/" + name.lower())))
    cabinet_files = []
    for sequence, file in enumerate(files, 1):
        rel = file.relative_to(input_dir)
        name = rel.as_posix().lower()
        component = stable_id("PayloadCmp", "cmp/" + name)
        file_id = "HarkenHubExe" if name == "harkenhub.exe" else stable_id("PayloadFile", "file/" + name)
        tables["Component"].append((component, component_guid(name), directory_id(rel.parent.as_posix()), 256, None, file_id))
        file_version, language, file_hash = file_metadata(file)
        tables["File"].append((file_id, component, msi_name(rel.name, "file/" + name),
                               file.stat().st_size, file_version, language, 512, sequence))
        if file_hash:
            tables["MsiFileHash"].append((file_id, 0, *file_hash))
        cabinet_files.append((file, file_id))
    for component, guid, directory, sddl in [
        ("HandoffAreaAcl", "B595227B-AFB0-4402-8840-BDB70535CDA2", "HandoffDir", HANDOFF_SDDL),
        ("PinsAreaAcl", "7D9C41B4-06AF-4E0B-A54B-2FBDBBF9A315", "PinsDir", HANDOFF_SDDL),
        ("UpdaterAreaAcl", "0E6C1F09-4A46-4C87-8B7C-1F5B4B6C2D31", "UpdaterDir", UPDATER_SDDL),
    ]:
        tables["Component"].append((component, "{" + guid + "}", directory, 256, None, None))
        tables["CreateFolder"].append((directory, component))
        tables["MsiLockPermissionsEx"].append((component, directory, "CreateFolder", sddl, None))
    registry_key = r"Software\HarkenSound\HarkenHub"
    for component, guid, directory, keypath, name, value in [
        ("HarkenProjectAssociation", "53FC615E-9B49-4762-B13C-B81820BDD7DB", "INSTALLFOLDER", "reg9Iw6ZFMWLFHU5l3K2mYjrhyy3bc", "InstallDir", "[INSTALLFOLDER]"),
        ("StartMenuShortcut", "3455F7BE-53E0-4D87-8C57-3DC57710C8F9", "ProgramMenuFolder", "regWwtQzH42OPt5oFFgOX3TTEsC1yI", "StartMenuShortcut", "#1"),
        ("DesktopShortcut", "4682FB52-D840-44F7-9356-E524137D4CE3", "DesktopFolder", "reg8H8qAwVwDvO1Nha1A7RQApczAdg", "DesktopShortcut", "#1"),
    ]:
        tables["Component"].append((component, "{" + guid + "}", directory, 260, None, keypath))
        tables["Registry"].append((keypath, 2, registry_key, name, value, component))
    for key, name, value in [
        (r"HarkenSound.HarkenProject\shell\open", None, "Open"),
        (r"HarkenSound.HarkenProject\shell\open\command", None, '"[#HarkenHubExe]" "%1"'),
        (".hsp", None, "HarkenSound.HarkenProject"),
        (".hsp", "Content Type", "application/x-harken-project"),
        ("HarkenSound.HarkenProject", None, "Harken Project"),
        ("HarkenSound.HarkenProject", "AppUserModelID", "HarkenSound.HarkenHub"),
    ]:
        tables["Registry"].append((stable_id("Association", key + str(name)), 0, key, name, value, "HarkenProjectAssociation"))
    for identifier, directory, component in [
        ("HarkenHubShortcut", "ProgramMenuFolder", "StartMenuShortcut"),
        ("HarkenHubDesktopShortcut", "DesktopFolder", "DesktopShortcut"),
    ]:
        tables["Shortcut"].append((identifier, directory, "Hub|Harken Hub", component,
                                   "[INSTALLFOLDER]HarkenHub.exe", None, None, None, None, None, None, "INSTALLFOLDER"))
        tables["MsiShortcutProperty"].append((identifier + "AppId", identifier,
                                             "System.AppUserModel.ID", "HarkenSound.HarkenHub"))
    tables["Feature"] = [("Hub", None, "Harken Hub", None, 2, 1, None, 0)]
    tables["FeatureComponents"] = [("Hub", row[0]) for row in tables["Component"]]
    tables["Upgrade"] = [(UPGRADE_CODE, None, version, "1033", 513, None, "HARKEN_UPGRADE_DETECTED"),
                          (UPGRADE_CODE, version, None, "1033", 2, None, "HARKEN_DOWNGRADE_DETECTED")]
    tables["LaunchCondition"] = [
        ("NOT HARKEN_DOWNGRADE_DETECTED", "A newer version of Harken Hub is already installed."),
        ("VersionNT64", "Harken Hub requires 64-bit Windows."),
    ]
    tables["AppSearch"] = [("HARKENPREVIOUSINSTALLDIR", "PreviousInstallFolder")]
    tables["RegLocator"] = [("PreviousInstallFolder", 2, registry_key, "InstallDir", 18)]
    tables["CustomAction"] = [
        ("HarkenPreserveInstallFolder", 51, "INSTALLFOLDER", "[HARKENPREVIOUSINSTALLDIR]"),
        ("HarkenHealthHandshake", 1058, "INSTALLFOLDER", '"[INSTALLFOLDER]HarkenHub.exe" --await-health-handshake'),
        ("HarkenRestartHub", 1762, "INSTALLFOLDER", '"[INSTALLFOLDER]HarkenHub.exe"'),
        ("HarkenRegisterUpdateBridge", 3106, "INSTALLFOLDER", '"[INSTALLFOLDER]HarkenHub.exe" --register-update-bridge'),
        ("HarkenMigrateShellShortcuts", 3682, "INSTALLFOLDER", '"[INSTALLFOLDER]HarkenHub.exe" --migrate-shell-shortcuts'),
        ("HarkenUnregisterUpdateBridge", 3170, "INSTALLFOLDER", '"[INSTALLFOLDER]HarkenHub.exe" --unregister-update-bridge'),
    ]
    prepare = [("FindRelatedProducts", None, 25), ("AppSearch", None, 50),
               ("HarkenPreserveInstallFolder", "HARKENPREVIOUSINSTALLDIR", 51),
               ("LaunchConditions", None, 100), ("ValidateProductID", None, 700),
               ("CostInitialize", None, 800), ("FileCost", None, 900),
               ("CostFinalize", None, 1000), ("MigrateFeatureStates", None, 1200)]
    tables["InstallExecuteSequence"] = prepare + [
        ("InstallValidate", None, 1400), ("InstallInitialize", None, 1500),
        ("HarkenUnregisterUpdateBridge", 'REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE', 1501),
        ("ProcessComponents", None, 1600), ("UnpublishFeatures", None, 1800),
        ("RemoveRegistryValues", None, 2600), ("RemoveShortcuts", None, 3200),
        ("RemoveFiles", None, 3500), ("RemoveFolders", None, 3600),
        ("CreateFolders", None, 3700), ("InstallFiles", None, 4000),
        ("HarkenRegisterUpdateBridge", "NOT REMOVE", 4001),
        ("CreateShortcuts", "NOT HARKENSELFUPDATE=1 AND NOT HARKEN_UPGRADE_DETECTED", 4500),
        ("WriteRegistryValues", None, 5000), ("RegisterUser", None, 6000),
        ("RegisterProduct", None, 6100), ("PublishFeatures", None, 6300),
        ("PublishProduct", None, 6400),
        ("HarkenMigrateShellShortcuts", "NOT REMOVE", 6597),
        ("HarkenHealthHandshake", "HARKENSELFUPDATE=1 AND NOT REMOVE", 6598),
        ("HarkenRestartHub", "HARKENSELFUPDATE=1 AND NOT HARKENBRIDGE=1 AND NOT REMOVE", 6599),
        ("InstallFinalize", None, 6600), ("RemoveExistingProducts", None, 6601),
    ]
    tables["InstallUISequence"] = prepare + [
        ("LicenseDialog", "NOT Installed", 1297), ("MaintenanceDialog", "Installed AND NOT REMOVE", 1298),
        ("ProgressDialog", None, 1299), ("ExecuteAction", None, 1300),
        ("SuccessDialog", None, -1), ("CancelledDialog", None, -2), ("FailureDialog", None, -3),
    ]
    tables["AdminExecuteSequence"] = [
        ("CostInitialize", None, 800), ("FileCost", None, 900), ("CostFinalize", None, 1000),
        ("InstallValidate", None, 1400), ("InstallInitialize", None, 1500),
        ("InstallAdminPackage", None, 3900), ("InstallFiles", None, 4000), ("InstallFinalize", None, 6600),
    ]
    tables["Media"] = [(1, len(files), None, "#hub.cab", None, None)]
    installer_ui(tables, license_rtf)
    return tables, cabinet_files


def build(files, input_dir, version, license_rtf, msi_path, evidence_dir, run=subprocess.run):
    tables, cabinet_files = package_tables(files, input_dir, version, license_rtf.read_text(encoding="ascii"))
    evidence_dir.mkdir(parents=True, exist_ok=True)
    for name, source in tables["Icon"]:
        shutil.copyfile(source, evidence_dir / name)
    evidence_tables = dict(tables)
    evidence_tables["Icon"] = [(name, name) for name, source in tables["Icon"]]
    (evidence_dir / "msi-tables.json").write_text(json.dumps(evidence_tables, indent=2) + "\n", encoding="utf-8")
    makecab = Path(os.environ["SystemRoot"]) / "System32" / "makecab.exe"
    with tempfile.TemporaryDirectory(prefix="harken-cab-") as temporary:
        work = Path(temporary)
        sources = work / "files"
        sources.mkdir()
        ddf = [".OPTION EXPLICIT", ".Set CabinetNameTemplate=hub.cab", ".Set DiskDirectoryTemplate=.",
               ".Set Cabinet=ON", ".Set Compress=ON", ".Set CompressionType=MSZIP",
               ".Set MaxDiskSize=0", ".Set MaxCabinetSize=0", ".Set MaxDiskFileCount=0"]
        for file, identifier in cabinet_files:
            shutil.copyfile(file, sources / identifier)
            ddf.append(f'"files\\{identifier}" "{identifier}"')
        (work / "payload.ddf").write_text("\n".join(ddf) + "\n", encoding="ascii")
        result = run([str(makecab), "/F", "payload.ddf"], cwd=work, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"makecab failed: {result.stdout}\n{result.stderr}")
        with Database(msi_path) as database:
            for name, (columns, keys) in SCHEMA.items():
                definitions = ", ".join("`" + column.strip().replace(" ", "` ", 1) for column in columns.split(","))
                primary = ", ".join("`" + key.strip() + "`" for key in keys.split(","))
                database.execute(f"CREATE TABLE `{name}` ({definitions} PRIMARY KEY {primary})")
                database.insert(name, tables[name])
            database.execute("INSERT INTO `_Streams` (`Name`, `Data`) VALUES (?, ?)", ("hub.cab", work / "hub.cab"))
            database.summary({1: 1252, 2: "Installation Database", 3: "Harken Hub", 4: "XD",
                              7: "x64;1033", 9: "{" + str(uuid.uuid4()).upper() + "}",
                              14: 500, 15: 2, 18: "Harken Windows Installer Builder", 19: 2})
            database.commit()
