from __future__ import annotations

import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
SDK_RUNTIME = REPO_ROOT / "HarkenSDK" / "Unity" / "Harken" / "Runtime" / "Sdk"
JSON_PARSER = SDK_RUNTIME / "JsonParser.cs"
PROJECT = """<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net9.0</TargetFramework>
    <LangVersion>9.0</LangVersion>
    <LanguageTargets>$(MSBuildToolsPath)/Microsoft.CSharp.targets</LanguageTargets>
    <ImplicitUsings>disable</ImplicitUsings>
    <Nullable>disable</Nullable>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
    <UseSharedCompilation>false</UseSharedCompilation>
    <NuGetAudit>false</NuGetAudit>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="PackManifest.cs" />
    <Compile Include="JsonParser.cs" />
    <Compile Include="Program.cs" />
  </ItemGroup>
</Project>
"""
HARNESS = r"""using System;
using System.Globalization;
using System.IO;
using System.Text.Json;
using Harken;

internal static class Program
{
    private static int Main(string[] args)
    {
        string root = Path.GetFullPath(args[0]);
        int schemaVersion = int.Parse(args[1], CultureInfo.InvariantCulture);
        string expectedDirectory = Path.Combine(root, "Compatibility");
        bool indexParsed = PackManifest.TryParseIndex(
            File.ReadAllText(Path.Combine(root, "HarkenPackIndex.json")), out PackIndexEntry[] index);
        bool platformResolved = PackManifest.TryResolvePlatformDirectory(
            root, "Compatibility", out string directory);
        bool manifestParsed = PackManifest.TryParseManifest(
            File.ReadAllText(Path.Combine(expectedDirectory, "HarkenPackManifest.json")), out PackManifestEntry[] packs);
        string[] paths = new string[2];
        ulong[] ids = { 11, 22 };
        string[] names = { "Init", "Content" };
        bool packsResolved = manifestParsed && packs.Length == 2;
        for (int i = 0; i < ids.Length; ++i)
        {
            bool resolved = PackManifest.TryResolvePackPath(expectedDirectory, ids[i], out string path);
            paths[i] = resolved && path != null
                ? Path.GetRelativePath(root, path).Replace('\\', '/') : string.Empty;
            packsResolved &= resolved && File.Exists(path) &&
                string.Equals(Path.GetFullPath(path), Path.Combine(expectedDirectory, names[i] + ".pack"),
                    StringComparison.Ordinal);
            if (packs.Length == 2)
            {
                packsResolved &= packs[i].Name == names[i] && packs[i].PackStableId == ids[i] &&
                    packs[i].Path == names[i] + ".pack";
            }
        }
        bool ok = indexParsed && index.Length == 1 && index[0].Name == "Compatibility" &&
            index[0].Path == "Compatibility" && platformResolved && directory != null &&
            Path.GetFullPath(directory) == expectedDirectory && manifestParsed && packsResolved;
        Console.WriteLine(JsonSerializer.Serialize(new
        {
            schemaVersion,
            ok,
            indexEntries = Array.ConvertAll(index, item => new { id = item.Id, name = item.Name, basePlatform = item.BasePlatform, path = item.Path }),
            manifestEntries = Array.ConvertAll(packs, item => new
            {
                name = item.Name,
                packStableId = item.PackStableId.ToString(CultureInfo.InvariantCulture),
                path = item.Path,
                platformId = item.PlatformId,
                contentHash = item.ContentHash.ToString(CultureInfo.InvariantCulture),
                dependencies = item.Dependencies
            }),
            packPaths = paths
        }));
        return ok ? 0 : 1;
    }
}
"""


def fnv1a64(data: bytes) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def verify_pack_reader(reader_source: Path, pack_root: Path, schema_version: int,
                       work_root: Path) -> dict[str, Any]:
    if type(schema_version) is not int or schema_version < 1:
        raise ValueError("Pack reader verification needs a positive schema version")
    if not reader_source.is_file() or reader_source.is_symlink():
        raise ValueError("Pack reader source must be a regular PackManifest.cs file")
    if not JSON_PARSER.is_file() or JSON_PARSER.is_symlink():
        raise ValueError("Pack reader source must include a regular JsonParser.cs file")
    work_root = work_root.resolve()
    if not work_root.is_relative_to((REPO_ROOT / ".temp").resolve()):
        raise ValueError("Pack reader work directory must be under repository .temp")
    fixtures: dict[str, bytes] = {}
    for name in ("Init.pack", "Content.pack"):
        path = pack_root / name
        if not path.is_file() or path.is_symlink() or path.stat().st_size == 0:
            raise ValueError(f"Pack reader fixture is missing a regular nonempty {name}")
        fixtures[name] = path.read_bytes()
    source = reader_source.read_bytes()
    platform_id = "11111111-2222-3333-4444-555555555555"
    index = {"schema_version": schema_version, "platforms": [{"id": platform_id, "name": "Compatibility", "basePlatform": "Windows", "path": "Compatibility"}]}
    manifest = {
        "schema_version": schema_version,
        "platformId": platform_id,
        "platformName": "Compatibility",
        "basePlatform": "Windows",
        "initPackStableId": "11",
        "packs": [
            {"name": "Init", "packStableId": "11", "path": "Init.pack"},
            {"name": "Content", "packStableId": "22", "path": "Content.pack"},
        ],
    }
    for pack in manifest["packs"]:
        pack["platformId"] = platform_id
        pack["contentHash"] = str(fnv1a64(fixtures[pack["path"]]))
        pack["dependencies"] = [] if pack["name"] == "Init" else ["11"]
    expected = {
        "schemaVersion": schema_version,
        "ok": True,
        "indexEntries": index["platforms"],
        "manifestEntries": manifest["packs"],
        "packPaths": ["Compatibility/Init.pack", "Compatibility/Content.pack"],
    }
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pack-reader-", dir=work_root) as directory:
        work = Path(directory)
        project = work / "PackReader.proj"
        project.write_text(PROJECT, encoding="utf-8")
        (work / "Program.cs").write_text(HARNESS, encoding="utf-8")
        (work / "PackManifest.cs").write_bytes(source)
        (work / "JsonParser.cs").write_bytes(JSON_PARSER.read_bytes())
        config = work / "NuGet.Config"
        config.write_text("<configuration><packageSources><clear /></packageSources></configuration>\n", encoding="utf-8")
        export = work / "export"
        platform = export / "Compatibility"
        platform.mkdir(parents=True)
        (export / "HarkenPackIndex.json").write_text(json.dumps(index, separators=(",", ":")), encoding="utf-8")
        (platform / "HarkenPackManifest.json").write_text(json.dumps(manifest, separators=(",", ":")), encoding="utf-8")
        for name, data in fixtures.items():
            (platform / name).write_bytes(data)
        environment = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1",
                           DOTNET_NOLOGO="1", DOTNET_CLI_HOME=str(work / "dotnet-home"),
                           NUGET_PACKAGES=str(work / "nuget-packages"),
                           MSBUILDDISABLENODEREUSE="1", DOTNET_CLI_USE_MSBUILD_SERVER="0",
                           DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE="true")
        try:
            built = subprocess.run(
                ["dotnet", "build", str(project), "--configuration", "Release", "--nologo", "--verbosity", "quiet",
                 "--disable-build-servers", "-nr:false",
                 f"-p:RestoreConfigFile={config}", "-p:ImportDirectoryBuildProps=false", "-p:ImportDirectoryBuildTargets=false"],
                cwd=REPO_ROOT, env=environment, capture_output=True, text=True, encoding="utf-8", timeout=120, check=False,
            )
            if built.returncode != 0:
                raise ValueError(f"Published Pack reader did not compile: {built.stdout} {built.stderr}")
            executed = subprocess.run(
                ["dotnet", str(work / "bin" / "Release" / "net9.0" / "PackReader.dll"), str(export), str(schema_version)],
                cwd=REPO_ROOT, env=environment, capture_output=True, text=True, encoding="utf-8", timeout=30, check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise ValueError(f"Cannot execute the controlled published Pack reader: {error}") from error
        try:
            report = json.loads(executed.stdout)
        except json.JSONDecodeError as error:
            raise ValueError("Published Pack reader did not return the controlled JSON report") from error
        if (
            executed.returncode != 0
            or not isinstance(report, dict)
            or type(report.get("schemaVersion")) is not int
            or report.get("ok") is not True
            or report != expected
        ):
            raise ValueError(f"Published Pack reader rejected or changed current sidecar data: {executed.stdout} {executed.stderr}")
    return {
        **report,
        "readerSha256": hashlib.sha256(source).hexdigest(),
        "fixtureSha256": {name: hashlib.sha256(data).hexdigest() for name, data in fixtures.items()},
    }
