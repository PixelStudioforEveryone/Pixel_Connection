"""Export an audited source snapshot without local Git history or runtime state.

Usage: python tools/export_source.py --output artifacts/open-source/Pixel_Connection
The output must be an empty directory inside artifacts. No credentials are printed.
"""
from pathlib import Path
import argparse
import json
import re
import shutil
import subprocess
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
PUBLIC_ROOTS = {'apps', 'cmake', 'include', 'server', 'src', 'tests', 'tools',
                'deploy', 'docs', 'Pixel_Connection_HOS'}
PUBLIC_FILES = {'.gitignore', '.gitattributes', 'CMakeLists.txt', 'README.md', 'README_EN.md', 'LICENSE',
                'THIRD_PARTY_NOTICES.md', 'SECURITY.md', 'architecture.html'}
ALLOWED = {'.cpp', '.h', '.hpp', '.c', '.cmake', '.md', '.txt', '.json', '.json5',
           '.ets', '.ts', '.d.ts', '.xml', '.svg', '.png', '.ico', '.qrc', '.rc',
           '.sh', '.py', '.ps1', '.html', '.properties', '.pem', '.example', '.nsi', '.desktop'}
CA_BUNDLE = 'Pixel_Connection_HOS/pixel_connection/src/main/resources/rawfile/pxc-ca-bundle.pem'
PATTERNS = {
    'private key': r'-----BEGIN (?:OPENSSH |RSA |EC |ENCRYPTED )?PRIVATE KEY-----',
    'SSH public key': r'ssh-(?:ed25519|rsa)\s+[A-Za-z0-9+/]{60,}',
    'access credential': r'(?:gh[pousr]_|github_pat_|AKIA)[A-Za-z0-9_]{15,}',
    'credential in URL': r'(?:https?|wss?)://[^\s/@:]+:[^\s/@]+@',
    'personal absolute path': r'C:[\\/]+Users[\\/]+[A-Za-z0-9_.-]+|/home/(?!pixelconnection(?:/|$))[A-Za-z0-9_.-]+',
    'signing password': r'"(?:keyPassword|storePassword)"\s*:\s*"[^"\s]+"',
}

def local_secret_values():
    """Compare against isolated local credentials, returning no values in reports."""
    values = set()
    for directory in ('artifacts/private-gateway', 'artifacts/private-release-backup'):
        base = ROOT / directory
        if not base.exists():
            continue
        for path in base.glob('*.json*'):
            content = path.read_text('utf-8', errors='replace')
            for match in re.finditer(r'"(?:turn_password|keyPassword|storePassword|password|token)"\s*:\s*"([^"]+)"', content):
                if len(match[1]) >= 8:
                    values.add(match[1])
            for match in re.finditer(r'"(?:account_api|signaling_url|turn_url)"\s*:\s*"([^"]+)"', content):
                hostname = urlsplit(match[1]).hostname
                if hostname and hostname not in {'localhost', '127.0.0.1', '::1'}:
                    values.add(hostname)
    return values

def audit_files(root, names):
    findings = []
    secrets = local_secret_values()
    for name in names:
        path = root / name
        data = path.read_bytes()
        if path.suffix.lower() in {'.png', '.ico'}:
            for value in secrets:
                if value.encode() in data:
                    findings.append({'file': name, 'kind': 'known credential in binary'})
            continue
        text = data.decode('utf-8', errors='replace')
        for label, pattern in PATTERNS.items():
            for match in re.finditer(pattern, text, re.IGNORECASE):
                findings.append({'file': name, 'line': text[:match.start()].count('\n') + 1,
                                 'kind': label})
        if name != CA_BUNDLE and re.search(r'^-----BEGIN CERTIFICATE-----$', text, re.MULTILINE):
            findings.append({'file': name, 'kind': 'unexpected certificate'})
        for value in secrets:
            if value in text:
                findings.append({'file': name, 'kind': 'known local credential'})
    return findings

def source_files():
    result = subprocess.run(['git', 'ls-files', '-co', '--exclude-standard', '-z'],
                            cwd=ROOT, check=True, capture_output=True)
    names = sorted(set(result.stdout.decode('utf-8').split('\0')) - {''})
    selected = []
    for name in names:
        path = Path(name)
        if name not in PUBLIC_FILES and path.parts[0] not in PUBLIC_ROOTS:
            continue
        if name not in PUBLIC_FILES and path.name not in {'CMakeLists.txt', '.gitignore', '.gitattributes'} and path.suffix.lower() not in ALLOWED:
            continue
        # Tracked files can remain ignored; exclude them explicitly too.
        ignored = subprocess.run(['git', 'check-ignore', '--no-index', '-q', name], cwd=ROOT)
        if ignored.returncode == 0:
            continue
        if (ROOT / name).is_file():
            selected.append(name)
    return selected

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--refresh', action='store_true', help='Refresh a previously exported source snapshot')
    args = parser.parse_args()
    output = (ROOT / args.output).resolve()
    if not output.is_relative_to((ROOT / 'artifacts').resolve()):
        raise SystemExit('Output must be inside artifacts/')
    if output.exists() and any(output.iterdir()) and not args.refresh:
        raise SystemExit('Output must be empty; existing files are never overwritten')
    names = source_files()
    for required in PUBLIC_FILES:
        if not (ROOT / required).is_file():
            raise SystemExit('Required public file missing: ' + required)
    findings = audit_files(ROOT, names)
    report = ROOT / 'artifacts' / 'open-source-audit.json'
    report.write_text(json.dumps({'files': len(names), 'names': names, 'findings': findings}, indent=2), encoding='utf-8')
    if findings:
        print(json.dumps(findings, indent=2))
        raise SystemExit('Publication blocked: sensitive findings')
    output.mkdir(parents=True, exist_ok=True)
    if args.refresh:
        # Remove obsolete exported source files, preserving snapshot Git metadata.
        # The destination is already constrained to artifacts/ above.
        for path in output.rglob('*'):
            if not path.is_file():
                continue
            name = path.relative_to(output).as_posix()
            if name not in names and (name in PUBLIC_FILES or Path(name).parts[0] in PUBLIC_ROOTS):
                path.unlink()
    for name in names:
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, destination)
    print(f'PASS: {len(names)} source files exported; no local history or credential findings')

if __name__ == '__main__':
    main()
