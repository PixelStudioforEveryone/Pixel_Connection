"""Create a per-user Windows installer from a fresh Qt runtime, never a user data directory."""
from pathlib import Path
import argparse
import json
import shutil
import subprocess
import re

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', required=True)
    parser.add_argument('--qt-bin', required=True)
    parser.add_argument('--openssl-bin', required=True)
    parser.add_argument('--zlib-dll', required=True)
    parser.add_argument('--crt-dir', required=True)
    parser.add_argument('--nsis', required=True)
    parser.add_argument('--version', default='0.1.0')
    args = parser.parse_args()
    assert re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', args.version), 'Version must have three numeric parts'
    stage = (ROOT / 'artifacts/windows-installer-stage').resolve()
    assert stage.is_relative_to((ROOT / 'artifacts').resolve())
    if stage.exists() and any(stage.iterdir()):
        raise SystemExit('Staging directory must be empty; runtime data is never copied or deleted')
    stage.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.client, stage / 'pxc-client.exe')
    subprocess.run([str(Path(args.qt_bin) / 'windeployqt.exe'), '--release', '--no-compiler-runtime',
                    '--no-translations', '--dir', str(stage), str(stage / 'pxc-client.exe')], check=True)
    for name in ('libcrypto-3-x64.dll', 'libssl-3-x64.dll'):
        shutil.copy2(Path(args.openssl_bin) / name, stage / name)
    shutil.copy2(args.zlib_dll, stage / Path(args.zlib_dll).name)
    crt_files = list(Path(args.crt_dir).glob('*.dll'))
    assert crt_files, 'MSVC runtime DLLs not found'
    for path in crt_files:
        shutil.copy2(path, stage / path.name)
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md', 'README_EN.md', 'SECURITY.md'):
        shutil.copy2(ROOT / name, stage / name)
    licenses = stage / 'licenses'
    licenses.mkdir()
    dependency_roots = list((ROOT / 'build-qt/_deps').glob('*-src'))
    dependency_roots += [ROOT / 'build-qt/_deps/libdatachannel-src/deps/libjuice',
                         ROOT / 'build-qt/_deps/libdatachannel-src/deps/usrsctp',
                         Path(args.openssl_bin).parents[1] / 'info/licenses']
    for directory in dependency_roots:
        if not directory.is_dir():
            continue
        for path in directory.iterdir():
            if path.is_file() and path.name.lower().startswith(('license', 'copying', 'copyright')):
                shutil.copy2(path, licenses / (directory.name + '-' + path.name))
    extra_licenses = ROOT / 'artifacts/release-tools/distribution-licenses'
    if extra_licenses.is_dir():
        for path in extra_licenses.iterdir():
            if path.is_file():
                shutil.copy2(path, licenses / path.name)
    source_commit = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, check=True,
                                   capture_output=True, text=True).stdout.strip()
    (stage / 'SOURCE.txt').write_text('PixelConnection ' + args.version + '\n' +
        'https://github.com/PixelStudioforEveryone/Pixel_Connection/tree/' + source_commit + '\n' +
        'Dependency sources and licenses: THIRD_PARTY_NOTICES.md and licenses/\n', encoding='utf-8')
    forbidden = {'.db', '.sqlite', '.pdb', '.key', '.pub', '.p12', '.p7b', '.cer', '.json', '.json5'}
    files = sorted(path for path in stage.rglob('*') if path.is_file())
    for path in files:
        assert path.suffix.lower() not in forbidden and not path.name.endswith(('-wal', '-shm')), 'Runtime state in installer'
    lines = ['!macro RemovePayload']
    for path in files:
        relative = str(path.relative_to(stage)).replace('/', '\\')
        assert '"' not in relative and '$' not in relative
        lines.append('  Delete "$INSTDIR\\' + relative + '"')
    for path in sorted((p for p in stage.rglob('*') if p.is_dir()), key=lambda p: len(p.parts), reverse=True):
        lines.append('  RMDir "$INSTDIR\\' + str(path.relative_to(stage)).replace('/', '\\') + '"')
    lines.append('!macroend')
    removal = ROOT / 'artifacts/windows-installer-removal.nsh'
    removal.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    output_dir = ROOT / 'dist/releases'
    output_dir.mkdir(parents=True, exist_ok=True)
    output = output_dir / ('PixelConnection-' + args.version + '-windows-x64-setup.exe')
    subprocess.run([args.nsis, '/V2', '/DVERSION=' + args.version, '/DPAYLOAD_DIR=' + str(stage),
                    '/DREMOVAL_MANIFEST=' + str(removal), '/DOUTPUT_FILE=' + str(output),
                    '/DAPP_ICON=' + str(ROOT / 'src/icon.ico'), str(ROOT / 'deploy/windows-installer.nsi')], check=True)
    assert output.is_file()
    print('Installer:', output.name, 'bytes:', output.stat().st_size, 'payload files:', len(files))

if __name__ == '__main__':
    main()
