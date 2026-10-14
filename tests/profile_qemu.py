#!/usr/bin/env python3
"""Compare modules in one KVM guest and account QEMU threads per workload.

Only the guest loads modules. QEMU guest_time is included in utime, so subtract
it to separate host execution. Host USB IRQs outside QEMU are not accounted here.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--module', action='append', required=True, help='LABEL=MODULE_PATH')
parser.add_argument('--profile-label', help='Also run static, sampled and function-profile windows for this label')
parser.add_argument('--repeat', type=int, default=2)
parser.add_argument('--duration', type=int, default=5)
parser.add_argument('--kernel', default=os.uname().release)
parser.add_argument('--qemu', default=str(ROOT / '.qemu-build/qemu-11.1.2/build/qemu-system-x86_64'))
parser.add_argument('--cpu', default='host')
parser.add_argument('--hostbus', default='2')
parser.add_argument('--hostport', default='2')
parser.add_argument('--profiler', type=Path, required=True)
parser.add_argument('--sampler', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if args.repeat < 1 or not 1 <= args.duration <= 120:
    parser.error('repeat must be positive; duration must be between 1 and 120')
modules = []
for item in args.module:
    label, path = item.split('=', 1)
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', label):
        parser.error('module labels must be alphanumeric, underscores or hyphens')
    path = Path(path)
    version = subprocess.check_output(['modinfo', '-F', 'vermagic', str(path)], text=True).split()[0]
    if version != args.kernel:
        parser.error(f'{path}: kernel version mismatch')
    modules.append((label, path))
if args.profile_label and args.profile_label not in dict(modules):
    parser.error('profile label must match one of the modules')
bridge = ROOT / 'it66121.ko'
version = subprocess.check_output(['modinfo', '-F', 'vermagic', str(bridge)], text=True).split()[0]
if version != args.kernel:
    parser.error(f'{bridge}: kernel version mismatch')
args.output.mkdir(parents=True, exist_ok=True)
archive = bytearray()
entries = [(f'fl2000-{label}.ko', path.read_bytes(), 0o100644) for label, path in modules]
entries += [('it66121.ko', bridge.read_bytes(), 0o100644),
            ('kms-profile', args.profiler.read_bytes(), 0o100755)]
if args.sampler:
    entries.append(('kernel-sample', args.sampler.read_bytes(), 0o100755))
entries.append(('TRAILER!!!', b'', 0))
for ino, (name, data, mode) in enumerate(entries, 1):
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name) + 1, 0]
    archive.extend(b'070701' + ''.join(f'{value:08x}' for value in fields).encode())
    archive.extend(name.encode() + b'\0'); archive.extend(b'\0' * (-len(archive) % 4))
    archive.extend(data); archive.extend(b'\0' * (-len(archive) % 4))
archive.extend(b'\0' * (-len(archive) % 512))
boot = args.output / 'boot.cpio'
with boot.open('wb') as output:
    with (ROOT / 'base-initramfs.cpio.gz').open('rb') as base:
        shutil.copyfileobj(base, output)
    output.write(b'\0' * (-output.tell() % 4)); output.write(archive)
console_path = args.output / 'console.sock'
if console_path.exists():
    console_path.unlink()
command = [args.qemu, '-enable-kvm', '-cpu', args.cpu, '-m', '1G', '-nic', 'none',
           '-kernel', f'/boot/vmlinuz-{args.kernel}', '-initrd', str(boot),
           '-append', 'console=ttyS0 quiet panic=1 oops=panic',
           '-device', 'qemu-xhci,id=xhci',
           '-device', f'usb-host,bus=xhci.0,hostbus={args.hostbus},hostport={args.hostport}',
           '-display', 'none', '-monitor', 'none',
           '-chardev', f'socket,id=console,path={console_path},server=on,wait=off',
           '-serial', 'chardev:console']
hz = os.sysconf('SC_CLK_TCK')

def threads(pid):
    values = {}
    for path in Path(f'/proc/{pid}/task').glob('*/stat'):
        try:
            data = path.read_text(); last = data.rindex(')')
            fields = data[last + 2:].split()
            values[int(path.parent.name)] = {
                'name': data[data.index('(') + 1:last],
                'user': int(fields[11]), 'system': int(fields[12]), 'guest': int(fields[40]),
            }
        except (OSError, ValueError):
            continue
    return values

windows = []
current_label = 'NO_DRIVER'
current_run = 'idle'
pending = None
script = ['set -e', 'mount -t debugfs none /sys/kernel/debug',
          '/kms-profile --baseline', 'insmod /it66121.ko']
for trial in range(args.repeat):
    for label, _ in (modules if trial % 2 == 0 else modules[::-1]):
        script += [f'echo BACKEND {label} trial-{trial + 1}', f'insmod /fl2000-{label}.ko',
                   f'/kms-profile --quick --duration={args.duration}', 'rmmod fl2000']
if args.profile_label:
    label = args.profile_label
    script += [f'echo BACKEND {label} static', f'insmod /fl2000-{label}.ko',
               f'/kms-profile --static-only --duration={args.duration}',
               f'echo BACKEND {label} fpu-check', '/kms-profile --full-only --duration=1 --check-fpu']
    if args.sampler:
        script += [f'echo BACKEND {label} sampled',
                   '/kernel-sample /kms-profile --full-only --duration=10']
    script += [f'echo BACKEND {label} function-profile', '/kms-profile --duration=5',
               'dmesg', 'rmmod fl2000']
script += ['rmmod it66121', 'echo PERFORMANCE_DONE', '/usr/lib/klibc/bin/poweroff -f']
with (args.output / 'qemu-stderr.log').open('w') as error, (args.output / 'guest.log').open('w') as log:
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=error)
    console = socket.socket(socket.AF_UNIX)
    try:
        deadline = time.monotonic() + 30
        while True:
            try:
                console.connect(str(console_path)); break
            except (FileNotFoundError, ConnectionRefusedError):
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('QEMU console did not start; inspect qemu-stderr.log')
                time.sleep(.1)
        console.settimeout(.5)
        buffer = ''; sent = 0; done = False
        deadline = time.monotonic() + args.repeat * len(modules) * (6 * args.duration + 40) + 480
        while process.poll() is None:
            if time.monotonic() > deadline:
                raise TimeoutError('guest workload did not finish')
            try:
                data = console.recv(65536)
            except socket.timeout:
                continue
            if not data:
                break
            value = data.decode(errors='replace').replace('\r', '')
            log.write(value); log.flush(); buffer += value
            if not sent and '# ' in buffer:
                console.sendall(b'stty -echo 2>/dev/null || true\necho PERF_CONSOLE_READY\n'); sent = 1
            while '\n' in buffer:
                line, buffer = buffer.split('\n', 1)
                line = line.lstrip('# ')
                if line == 'PERF_CONSOLE_READY' and sent == 1:
                    console.sendall(('\n'.join(script) + '\n').encode()); sent = 2
                match = re.fullmatch(r'BACKEND (\S+) (\S+)', line)
                if match:
                    current_label, current_run = match.groups()
                    print(line, flush=True)
                match = re.fullmatch(r'WORKLOAD_BEGIN (\S+) (\S+)', line)
                if match:
                    pending = (match.groups(), time.monotonic(), threads(process.pid))
                if line.startswith('CPU ') and pending:
                    (format_, phase), start, before = pending
                    wall = time.monotonic() - start; after = threads(process.pid)
                    stats = []
                    for tid, old in before.items():
                        if tid not in after:
                            continue
                        new = after[tid]
                        cpu = (new['user'] + new['system'] - old['user'] - old['system']) / hz
                        guest = (new['guest'] - old['guest']) / hz
                        stats.append({'tid': tid, 'name': new['name'], 'total_cpu_seconds': cpu,
                                      'guest_cpu_seconds': guest, 'host_cpu_seconds': cpu - guest})
                    record = {'backend': current_label, 'run': current_run, 'format': format_,
                              'phase': phase, 'guest': line, 'host_wall_seconds': wall, 'qemu_threads': stats}
                    windows.append(record); pending = None
                    host = sum(x['host_cpu_seconds'] for x in stats)
                    guest = sum(x['guest_cpu_seconds'] for x in stats)
                    print(f'{line} QEMU_host={100 * host / wall:.2f}% KVM_guest={100 * guest / wall:.2f}%', flush=True)
                    (args.output / 'windows.json').write_text(json.dumps(windows, indent=2) + '\n')
                if line.startswith(('KERNEL_SAMPLE', 'KERNEL_SAMPLES', 'FPU_STATE')):
                    print(line, flush=True)
                if line == 'PERFORMANCE_DONE':
                    done = True
        process.wait(timeout=15)
        if not done or process.returncode:
            raise RuntimeError('guest did not complete successfully; inspect guest.log')
    finally:
        console.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
metadata = {'kernel': args.kernel, 'cpu': args.cpu, 'qemu_command': command,
            'module_sha256': {label: hashlib.sha256(path.read_bytes()).hexdigest() for label, path in modules},
            'windows': windows,
            'limitations': ['QEMU host CPU excludes host IRQ/kernel work outside the QEMU process.',
                            'QEMU guest_time is coarse tick accounting; individual short-window values are noisy.',
                            'Sampled and function-profile runs are diagnostic and must not be used as untraced CPU baselines.']}
(args.output / 'results.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(f'PASS: guest workloads and clean shutdown; results: {args.output / "results.json"}', flush=True)
