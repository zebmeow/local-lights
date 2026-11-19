"""Capture board logs or provision existing Wi-Fi settings over USB."""
import argparse
import os
from pathlib import Path
import time
import tomllib
import serial

ROOT = Path(__file__).resolve().parent
p = argparse.ArgumentParser()
p.add_argument('--reset', action='store_true')
p.add_argument('--wifi', action='store_true')
p.add_argument('--seconds', type=float, default=12)
p.add_argument('--log', default='boot.log')
args = p.parse_args()
config = tomllib.loads((Path.home()/'.config/esp32share/config.toml').read_text())
port = serial.Serial(port=None, baudrate=115200, timeout=0.2)
port.dtr = False
port.rts = False
port.port = '/dev/ttyUSB1'
port.open()
if args.reset:
    port.rts = True
    time.sleep(0.15)
    port.rts = False

log_path = ROOT/args.log
log_path.touch(mode=0o600, exist_ok=True)
log_path.chmod(0o600)
live_log = log_path.open('wb')
chunks = []
start = time.monotonic()
sent = False
while time.monotonic() - start < args.seconds:
    chunk = port.read(8192)
    chunks.append(chunk)
    live_log.write(chunk)
    live_log.flush()
    if args.wifi and not sent and time.monotonic() - start > 4:
        def quote(value):
            if any(c in value for c in '\r\n\x00'):
                raise ValueError('Invalid Wi-Fi credentials')
            return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'
        command = 'matter esp wifi connect ' + quote(config['ssid']) + ' ' + quote(config['password']) + '\n'
        port.write(command.encode())
        sent = True
port.close()
live_log.close()
text = b''.join(chunks).decode(errors='replace')
for key in ('password', 'ssid', 'http_password'):
    if config.get(key):
        text = text.replace(config[key], '<redacted>')
path = ROOT/args.log
path.write_text(text)
path.chmod(0o600)
print(text[-16000:])
