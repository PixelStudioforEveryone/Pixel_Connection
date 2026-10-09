"""Smoke-test a built server on loopback with a disposable database and random credentials.

python tools/check_server.py path/to/pxc-server
python tools/check_server.py path/to/pxc-client --integrated
No existing service, account database or client configuration is used.
"""
import argparse
import base64
import json
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def free_port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--integrated', action='store_true')
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    api_port, ws_port = free_port(), free_port()
    while api_port == ws_port:
        ws_port = free_port()
    base = f'http://127.0.0.1:{api_port}'
    signaling = f'ws://127.0.0.1:{ws_port}'
    # Ignore machine proxy settings for the isolated loopback test.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(path, body=None, token=None, expected=200):
        headers = {'Content-Type': 'application/json'}
        if token:
            headers['Authorization'] = 'Bearer ' + token
        req = urllib.request.Request(base + path, headers=headers,
            data=json.dumps(body).encode() if body is not None else None)
        try:
            response = opener.open(req, timeout=3)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            assert response.status == expected, f'{path}: unexpected HTTP status {response.status}'
            return json.load(response)

    with tempfile.TemporaryDirectory(prefix='pixelconnection-server-check-') as directory:
        log = Path(directory) / 'server.log'
        database = Path(directory) / 'accounts.db'
        command = [str(executable)] + (['--server'] if args.integrated else [])
        command += ['--db', str(database), '--v4', '127.0.0.1', '--no-v6',
            '--api-port', str(api_port), '--port', str(ws_port),
            '--advertise-api', base, '--advertise-ws', signaling]
        username = 'check_' + secrets.token_hex(6)
        password = secrets.token_urlsafe(32)
        server_id = None
        for cycle in range(2):
            with log.open('ab') as output:
                process = subprocess.Popen(command, cwd=directory, stdout=output, stderr=output)
                try:
                    deadline = time.monotonic() + 15
                    health = None
                    while time.monotonic() < deadline:
                        if process.poll() is not None:
                            raise RuntimeError(f'Server exited during startup ({process.returncode})')
                        try:
                            candidate = request('/api/v1/health')
                            if candidate['server']['ready']:
                                health = candidate
                                break
                        except (OSError, urllib.error.URLError):
                            pass
                        time.sleep(0.1)
                    assert health is not None, 'Server readiness timed out'
                    server = health['server']
                    assert health['status'] == 'ok' and server['product'] == 'PixelConnection'
                    assert server['api_url'] == base and server['signaling_url'] == signaling
                    assert server['listen_v4'] == '127.0.0.1' and not server['listen_v6']
                    if cycle == 0:
                        server_id = server['id']
                        assert server_id, 'Missing stable server identity'
                        request('/api/v1/auth/register', {'username': username,
                            'email': username + '@example.com', 'password': password})
                    else:
                        assert server['id'] == server_id, 'Server identity changed across restart'
                    login = request('/api/v1/auth/login', {'identifier': username, 'password': password})
                    token = login['access_token']
                    assert token and login['token_type'] == 'Bearer'
                    request('/api/v1/devices', expected=401)
                    request('/api/v1/devices', token=token)
                    with socket.create_connection(('127.0.0.1', ws_port), timeout=3) as ws:
                        key = base64.b64encode(secrets.token_bytes(16)).decode()
                        ws.sendall((f'GET / HTTP/1.1\r\nHost: 127.0.0.1:{ws_port}\r\n'
                            'Upgrade: websocket\r\nConnection: Upgrade\r\n'
                            f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
                        assert b' 101 ' in ws.recv(4096).split(b'\r\n', 1)[0], 'WebSocket handshake failed'
                    request('/api/v1/auth/logout', {}, token=token)
                    request('/api/v1/devices', token=token, expected=401)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
    print('PASS: API readiness, advertised addresses, register/login, authorization, logout, '
          'WebSocket handshake and account/server identity persistence across restart')


if __name__ == '__main__':
    main()
