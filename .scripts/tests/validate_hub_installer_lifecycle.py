import argparse
import ctypes
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import uuid
import winreg
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from release import build_hub_installer, hub_msi
from release.windows_installer import Database, file_metadata

parser = argparse.ArgumentParser()
for name in ('package-dir', 'baseline', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
assert os.environ.get('GITHUB_ACTIONS') == 'true'
assert os.environ.get('RUNNER_ENVIRONMENT') == 'github-hosted'
assert ctypes.windll.shell32.IsUserAnAdmin()
args.output.mkdir(parents=True, exist_ok=True)
report = []
msi = ctypes.WinDLL('msi')
msi.MsiEnumRelatedProductsW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_wchar_p]
msi.MsiGetProductInfoW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint)]
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.CreateEventW.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_bool, ctypes.c_wchar_p]
kernel.CreateEventW.restype = ctypes.c_void_p
kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint]
kernel.CloseHandle.argtypes = [ctypes.c_void_p]


def products():
    result = []
    for index in range(100):
        code = ctypes.create_unicode_buffer(39)
        status = msi.MsiEnumRelatedProductsW(hub_msi.UPGRADE_CODE, 0, index, code)
        if status == 259:
            return result
        assert status == 0, status
        result.append(code.value)
    raise AssertionError('too many registered products')


def info(product, key):
    buffer = ctypes.create_unicode_buffer(4096)
    size = ctypes.c_uint(4096)
    assert msi.MsiGetProductInfoW(product, key, buffer, ctypes.byref(size)) == 0
    return buffer.value


def mark(name, **details):
    record = {'scenario': name, 'passed': True, **details}
    report.append(record)
    (args.output / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(record), flush=True)


def run(name, command, expected=(0, 3010)):
    started = time.monotonic()
    with (args.output / (name + '.stdout.log')).open('wb') as output:
        result = subprocess.run(command if isinstance(command, str) else [str(value) for value in command], stdout=output, stderr=subprocess.STDOUT, timeout=240)
    assert result.returncode in expected, (name, result.returncode, expected)
    mark(name, exitCode=result.returncode, seconds=round(time.monotonic() - started, 2))
    return result.returncode


def install(name, package, language=None, properties=(), expected=(0, 3010)):
    command = [package]
    if language:
        command.append('--harken-setup-' + language)
    command += ['/qn', '/norestart', '/l*v', args.output.resolve() / (name + '.msi.log')]
    arguments = subprocess.list2cmdline([str(value) for value in command])
    for property in properties:
        key, value = property.split('=', 1)
        assert '"' not in value
        arguments += f' {key}="{value}"'
    return run(name, arguments, expected)


def install_dir():
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'Software\HarkenSound\HarkenHub') as key:
        return Path(winreg.QueryValueEx(key, 'InstallDir')[0])


def verify(language, version='0.2.1', retained=None):
    registered = products()
    assert len(registered) == 1, registered
    assert info(registered[0], 'Language') == language
    assert info(registered[0], 'VersionString') == version
    destination = install_dir()
    preserved = []
    for source in build_hub_installer.collect_payload(args.payload.resolve()):
        relative = source.relative_to(args.payload.resolve())
        target = destination / relative
        assert target.is_file(), str(target)
        actual = hashlib.sha256(target.read_bytes()).hexdigest()
        if actual != hashlib.sha256(source.read_bytes()).hexdigest():
            previous = (retained or {}).get(relative.as_posix())
            assert previous and source.suffix.lower() == '.dll', str(target)
            target_version = file_metadata(target)[0]
            source_version = file_metadata(source)[0]
            assert target_version and source_version, str(target)
            assert actual == previous['sha256'] and target_version == previous['version'], str(target)
            assert tuple(map(int, target_version.split('.'))) >= tuple(map(int, source_version.split('.'))), str(target)
            preserved.append({'file': relative.as_posix(), 'version': target_version, 'sha256': actual})
    bridge = subprocess.check_output(['pwsh', '-NoProfile', '-Command', "(Get-ScheduledTask -TaskName HarkenSoundHubUpdateBridge).Actions.Execute"], text=True).strip()
    assert Path(bridge) == destination / 'Updater/HarkenHubUpdateBridge.exe', bridge
    assert Path(bridge).is_file(), bridge
    assert Path(bridge).name == 'HarkenHubUpdateBridge.exe'
    mark('verify-installed-' + language, product=registered[0], directory=str(destination), bridge=bridge, retainedDependencies=preserved)
    return registered[0]


def startup(name):
    event_name = 'Global\\HarkenHub.Ready.' + str(uuid.uuid4())
    event = kernel.CreateEventW(None, True, False, event_name)
    assert event
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r'Software\HarkenSound\HarkenHub') as key:
        winreg.SetValueEx(key, 'registerBackgroundOnLogin', 0, winreg.REG_SZ, 'false')
    with (args.output / (name + '.log')).open('wb') as output:
        process = subprocess.Popen([str(install_dir() / 'HarkenHub.exe'), '--show-versions', '--update-ready-event', event_name], stdout=output, stderr=subprocess.STDOUT)
        try:
            assert kernel.WaitForSingleObject(event, 30000) == 0, (name, process.poll())
            assert process.poll() is None
            mark(name, readinessConfirmed=True)
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=20)
            kernel.CloseHandle(event)


def uninstall(name):
    registered = products()
    assert len(registered) == 1
    destination = install_dir()
    run(name, ['msiexec.exe', '/x', registered[0], '/qn', '/norestart', '/l*v', args.output.resolve() / (name + '.msi.log')])
    assert not products()
    assert not (destination / 'HarkenHub.exe').exists()
    query = subprocess.run(['schtasks.exe', '/query', '/tn', 'HarkenSoundHubUpdateBridge'], capture_output=True)
    assert query.returncode != 0
    mark(name + '-cleanup', productRemoved=True, bridgeTaskRemoved=True)


def extract_msi(setup, name, destination):
    kernel.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint]
    kernel.LoadLibraryExW.restype = ctypes.c_void_p
    kernel.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = ctypes.c_void_p
    kernel.LoadResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.LoadResource.restype = ctypes.c_void_p
    kernel.LockResource.argtypes = [ctypes.c_void_p]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.SizeofResource.restype = ctypes.c_uint
    kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
    module = kernel.LoadLibraryExW(str(setup), None, 2)
    assert module, ctypes.get_last_error()
    try:
        def resource(key):
            found = kernel.FindResourceW(module, key, ctypes.c_void_p(10))
            assert found, key
            size = kernel.SizeofResource(module, found)
            loaded = kernel.LoadResource(module, found)
            pointer = kernel.LockResource(loaded)
            assert pointer and size, key
            return ctypes.string_at(pointer, size)
        manifest = resource(name)
        shared = resource(build_hub_installer.SHARED_PAYLOAD_RESOURCE_NAME)
        magic, size, digest = struct.unpack('<8sI32s', manifest[:44])
        assert magic == b'HKMSI001' and size > 0
        count = (size + 4095) // 4096
        assert len(manifest) == 44 + count * 4 and len(shared) % 4096 == 0
        indices = struct.unpack(f'<{count}I', manifest[44:])
        assert all(index < len(shared) // 4096 for index in indices)
        payload = b''.join(shared[index*4096:(index+1)*4096] for index in indices)[:size]
        assert hashlib.sha256(payload).digest() == digest
        destination.write_bytes(payload)
        mark('extract-' + name, sha256=digest.hex(), size=size)
    finally:
        kernel.FreeLibrary(module)


assert not products(), 'refuse to modify a pre-existing Hub installation'
setup, = args.package_dir.glob('*.exe')
setup = setup.resolve()
args.stub = setup
receipt_path, = args.package_dir.glob('*-evidence/receipt.json')
receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
assert receipt['version'] == '0.2.1'
assert receipt['identity']['gitCommit'] == 'c4398fbe7525ae932a8442aed77362140fe103a0'
assert receipt['identity']['channel'] == 'stable'
assert not receipt['identity']['gitDirty']
setup_digest = hashlib.sha256(setup.read_bytes()).hexdigest()
assert setup_digest == '292ecb7545d756cf46b7184d560ad8c517f10e5f95171ca4d6842f6f32781e60'
assert setup_digest == receipt['artifacts'][0]['sha256']
baseline_digest = hashlib.sha256(args.baseline.read_bytes()).hexdigest()
assert baseline_digest == '792c5c33658ffd00620fc2dc2ede4d54a2d5017540a38a22445d5c32593c8e7d'
mark('verify-final-package-provenance', source=receipt['identity']['gitCommit'], setupSha256=setup_digest, baselineSha256=baseline_digest)
english = args.output.resolve() / 'current.msi'
chinese = args.output.resolve() / 'current.zh-CN.msi'
extract_msi(setup, build_hub_installer.PAYLOAD_RESOURCE_NAME, english)
extract_msi(setup, build_hub_installer.CHINESE_PAYLOAD_RESOURCE_NAME, chinese)
extracted = args.output.resolve().parent / 'extracted'
run('administrative-extraction', f'msiexec.exe /a "{english}" /qn /norestart TARGETDIR="{extracted}" /l*v "{args.output.resolve() / "extraction.msi.log"}"')
executable, = extracted.rglob('HarkenHub.exe')
args.payload = executable.parent
for entry in receipt['payload']:
    file = args.payload / entry['path']
    assert file.is_file() and file.stat().st_size == entry['size'], entry['path']
    assert hashlib.sha256(file.read_bytes()).hexdigest() == entry['sha256'], entry['path']
mark('verify-extracted-payload', files=len(receipt['payload']))
custom = Path(os.environ['ProgramFiles']) / 'Hub QA Install With Spaces'
sentinels = [Path(os.environ['LOCALAPPDATA']) / 'HarkenSound/HarkenHub/qa-preserve.txt',
             Path(os.environ['ProgramData']) / 'HarkenSound/HarkenHub/qa-preserve.txt',
             args.output.resolve() / 'Example Project' / 'untouched.hsp']

try:
    install('fresh-english', setup, 'en', [f'INSTALLFOLDER={custom}'])
    first = verify('1033')
    assert install_dir() == custom
    for path in sentinels:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('must survive installation maintenance', encoding='utf-8')
    startup('english-gui-startup')
    run('english-health-handshake', [custom / 'HarkenHub.exe', '--await-health-handshake'])
    damaged = custom / 'Qt6Svg.dll'
    damaged.write_bytes(b'corrupt native library')
    install('repair-corrupt-library', setup, 'en', ['REINSTALL=ALL', 'REINSTALLMODE=amus'])
    assert verify('1033') == first
    damaged.unlink()
    install('repair-missing-library', setup, 'en', ['REINSTALL=ALL', 'REINSTALLMODE=amus'])
    assert verify('1033') == first
    install('same-version-reinstall', setup, 'en')
    assert verify('1033') == first
    install('english-to-chinese', setup, 'zh')
    assert verify('2052') != first
    assert install_dir() == custom
    startup('chinese-gui-startup')
    install('chinese-to-english', setup, 'en')
    current = verify('1033')
    install('reject-released-downgrade', args.baseline.resolve(), expected=(1603,))
    assert verify('1033') == current
    uninstall('uninstall-new-version')
    assert all(path.read_text(encoding='utf-8') == 'must survive installation maintenance' for path in sentinels)
    mark('uninstall-preserves-user-data')
    for baseline_language, current_language, expected_language in [('en', 'en', '1033'), ('zh', 'zh', '2052')]:
        label = 'ordinary-upgrade-' + baseline_language
        install(label + '-baseline', args.baseline.resolve(), baseline_language, [f'INSTALLFOLDER={custom}'])
        assert len(products()) == 1 and info(products()[0], 'VersionString') == '0.2.0'
        startup(label + '-baseline-startup')
        previous_files = {
            path.relative_to(custom).as_posix(): {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'version': file_metadata(path)[0]}
            for path in custom.rglob('*') if path.is_file()
        }
        install(label, setup, current_language, [f'INSTALLFOLDER={custom}-ignored'])
        verify(expected_language, retained=previous_files)
        assert install_dir() == custom
        assert all(path.read_text(encoding='utf-8') == 'must survive installation maintenance' for path in sentinels)
        startup(label + '-startup')
        mark(label + '-verified', originalInstallPathPreserved=True, userDataPreserved=True)
        uninstall(label + '-uninstall')
    install('install-released-baseline', args.baseline.resolve(), properties=[f'INSTALLFOLDER={custom}'])
    assert len(products()) == 1 and info(products()[0], 'VersionString') == '0.2.0'
    baseline_product = products()[0]
    baseline_files = {
        path.relative_to(custom).as_posix(): {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'version': file_metadata(path)[0]}
        for path in custom.rglob('*') if path.is_file()
    }
    failed_msi = args.output / 'rollback.msi'
    shutil.copyfile(english, failed_msi)
    with Database(failed_msi, 1) as db:
        db.insert('CustomAction', [('QaForcedFailure', 3106, 'INSTALLFOLDER', '"[SystemFolder]cmd.exe" /d /c exit 1')])
        db.insert('InstallExecuteSequence', [('QaForcedFailure', '1', 6596)])
        db.summary({9: '{' + str(uuid.uuid4()).upper() + '}'})
        db.commit()
    failed_setup = args.output / 'rollback.exe'
    build_hub_installer.embed_payload(args.stub.resolve(), failed_msi, failed_msi, failed_setup)
    install('rollback-failed-upgrade', failed_setup.resolve(), 'en', ['HARKENSELFUPDATE=1', 'HARKENBRIDGE=1'], (1603,))
    assert products() == [baseline_product]
    assert info(baseline_product, 'VersionString') == '0.2.0'
    for relative, previous in baseline_files.items():
        assert hashlib.sha256((custom / relative).read_bytes()).hexdigest() == previous['sha256'], relative
    assert install_dir() == custom
    bridge = subprocess.check_output(['pwsh', '-NoProfile', '-Command', "(Get-ScheduledTask -TaskName HarkenSoundHubUpdateBridge).Actions.Execute"], text=True).strip()
    assert Path(bridge) == custom / 'Updater/HarkenHubUpdateBridge.exe'
    mark('rollback-restores-released-files', filesVerified=len(baseline_files), product=baseline_product)
    startup('startup-after-rollback')
    install('upgrade-release-using-bridge-arguments', setup, 'zh', ['HARKENSELFUPDATE=1', 'HARKENBRIDGE=1', f'INSTALLFOLDER={custom}-ignored'])
    current = verify('2052', retained=baseline_files)
    assert install_dir() == custom
    assert all(path.read_text(encoding='utf-8') == 'must survive installation maintenance' for path in sentinels)
    startup('startup-after-release-upgrade')
    mark('bridge-argument-upgrade-verified', originalInstallPathPreserved=True, userDataPreserved=True)
    uninstall('uninstall-upgraded-version')
    install('fresh-chinese', setup, 'zh', [f'INSTALLFOLDER={custom}'])
    verify('2052')
    uninstall('uninstall-chinese')
    original_resources = build_hub_installer.payload_resources
    resources = original_resources((english, chinese))
    shared = bytearray(resources[build_hub_installer.SHARED_PAYLOAD_RESOURCE_NAME])
    shared[0] ^= 0x80
    resources[build_hub_installer.SHARED_PAYLOAD_RESOURCE_NAME] = bytes(shared)
    build_hub_installer.payload_resources = lambda packages: resources
    corrupt_setup = args.output / 'corrupt.exe'
    build_hub_installer.embed_payload(args.stub.resolve(), english, chinese, corrupt_setup)
    build_hub_installer.payload_resources = original_resources
    install('reject-corrupt-embedded-data', corrupt_setup.resolve(), 'en', expected=(1603,))
    assert not products()
    assert all(path.read_text(encoding='utf-8') == 'must survive installation maintenance' for path in sentinels)
    mark('final-cleanup', productsRemaining=products(), userDataPreserved=True)
except BaseException as error:
    report.append({'passed': False, 'error': repr(error)})
    (args.output / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    raise
finally:
    for package in args.output.glob('*.exe'):
        package.unlink()
    for package in args.output.glob('*.msi'):
        package.unlink()
