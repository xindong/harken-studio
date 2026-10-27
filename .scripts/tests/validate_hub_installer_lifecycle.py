import argparse
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import uuid
import winreg
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from release import build_hub_installer, hub_msi
from release.windows_installer import Database

parser = argparse.ArgumentParser()
for name in ('package-dir', 'payload', 'stub', 'baseline', 'output'):
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
        result = subprocess.run([str(value) for value in command], stdout=output, stderr=subprocess.STDOUT, timeout=240)
    assert result.returncode in expected, (name, result.returncode, expected)
    mark(name, exitCode=result.returncode, seconds=round(time.monotonic() - started, 2))
    return result.returncode


def install(name, package, language=None, properties=(), expected=(0, 3010)):
    command = [package]
    if language:
        command.append('--harken-setup-' + language)
    command += ['/qn', '/norestart', '/l*v', args.output.resolve() / (name + '.msi.log'), *properties]
    return run(name, command, expected)


def install_dir():
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'Software\HarkenSound\HarkenHub') as key:
        return Path(winreg.QueryValueEx(key, 'InstallDir')[0])


def verify(language, version='0.1.73'):
    registered = products()
    assert len(registered) == 1, registered
    assert info(registered[0], 'Language') == language
    assert info(registered[0], 'VersionString') == version
    destination = install_dir()
    for source in build_hub_installer.collect_payload(args.payload.resolve()):
        target = destination / source.relative_to(args.payload.resolve())
        assert target.is_file(), str(target)
        assert hashlib.sha256(target.read_bytes()).digest() == hashlib.sha256(source.read_bytes()).digest(), str(target)
    bridge = subprocess.check_output(['pwsh', '-NoProfile', '-Command', "(Get-ScheduledTask -TaskName HarkenSoundHubUpdateBridge).Actions.Execute"], text=True).strip()
    assert Path(bridge).is_file(), bridge
    assert Path(bridge).name == 'HarkenHubUpdateBridge.exe'
    mark('verify-installed-' + language, product=registered[0], directory=str(destination), bridge=bridge)
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


assert not products(), 'refuse to modify a pre-existing Hub installation'
setup = next(args.package_dir.glob('*.exe')).resolve()
english = next(path for path in args.package_dir.glob('*.msi') if not path.name.endswith('.zh-CN.msi')).resolve()
chinese = next(args.package_dir.glob('*.zh-CN.msi')).resolve()
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
    install('install-released-baseline', args.baseline.resolve(), properties=[f'INSTALLFOLDER={custom}'])
    assert len(products()) == 1 and info(products()[0], 'VersionString') == '0.1.72'
    install('upgrade-release-using-bridge-arguments', setup, 'zh', ['HARKENSELFUPDATE=1', 'HARKENBRIDGE=1', f'INSTALLFOLDER={custom}-ignored'])
    current = verify('2052')
    assert install_dir() == custom
    assert all(path.read_text(encoding='utf-8') == 'must survive installation maintenance' for path in sentinels)
    startup('startup-after-release-upgrade')
    failed_msi = args.output / 'rollback.msi'
    shutil.copyfile(english, failed_msi)
    with Database(failed_msi, 1) as db:
        db.execute("UPDATE `Property` SET `Value`=? WHERE `Property`='ProductCode'", ('{' + str(uuid.uuid4()).upper() + '}',))
        db.execute("UPDATE `Property` SET `Value`='0.1.74' WHERE `Property`='ProductVersion'")
        db.execute("UPDATE `Upgrade` SET `VersionMax`='0.1.74' WHERE `ActionProperty`='HARKEN_UPGRADE_DETECTED'")
        db.execute("UPDATE `Upgrade` SET `VersionMin`='0.1.74' WHERE `ActionProperty`='HARKEN_DOWNGRADE_DETECTED'")
        db.insert('CustomAction', [('QaForcedFailure', 19, None, 'Intentional rollback validation')])
        db.insert('InstallExecuteSequence', [('QaForcedFailure', '1', 6596)])
        db.summary({9: '{' + str(uuid.uuid4()).upper() + '}'})
        db.commit()
    failed_setup = args.output / 'rollback.exe'
    build_hub_installer.embed_payload(args.stub.resolve(), failed_msi, failed_msi, failed_setup)
    install('rollback-failed-upgrade', failed_setup.resolve(), 'en', ['HARKENSELFUPDATE=1', 'HARKENBRIDGE=1'], (1603,))
    assert verify('2052') == current
    startup('startup-after-rollback')
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
