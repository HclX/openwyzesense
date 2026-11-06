#!/usr/bin/env python3
"""Automated 2-pin cJTAG probe loop for identifying TMS and TCK pads."""

import os
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
DSLITE = ROOT / '.toolchain/uniflash/deskdb/content/TICloudAgent/linux/ccs_base/DebugServer/bin/DSLite'
CONFIG = ROOT / 'firmware/boards/wyze_contact_v1/xds110_2pin.ccxml'
OUT_DIR = ROOT / 'backups/leak_sensor_probe'
OUT_DIR.mkdir(parents=True, exist_ok=True)


def play_chime():
    sound = '/usr/share/sounds/freedesktop/stereo/complete.oga'
    for _ in range(3):
        if os.path.exists(sound):
            subprocess.run(['paplay', sound], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print('\a', end='', flush=True)
        time.sleep(0.15)


def run_probe(max_attempts=300):
    print(f'Starting cJTAG probe loop (up to {max_attempts} attempts, ~10 minutes)...', flush=True)
    temp_ccfg = OUT_DIR / 'ccfg.bin'

    for attempt in range(1, max_attempts + 1):
        # Read CCFG range (0x1FFA8, 22 32-bit words) with a short 2s timeout
        cmd = [
            str(DSLITE),
            'memory',
            '--config',
            str(CONFIG),
            '--range',
            '0x1FFA8,22',
            '--output',
            str(temp_ccfg),
            '--timeout',
            '2',
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        out = res.stdout + res.stderr

        success = res.returncode == 0 and 'fatal' not in out and 'Failed' not in out
        if success and temp_ccfg.exists() and temp_ccfg.stat().st_size >= 88:
            print('\n' + '=' * 60, flush=True)
            print('>>> TARGET CONNECTED! TMS AND TCK FOUND! <<<', flush=True)
            print('=' * 60, flush=True)
            play_chime()

            data = temp_ccfg.read_bytes()
            # On CC1310 Wyze sensors:
            # 0x1FFA8 is base of read
            # 0x1FFCC is MAC (offset 0x24 = 36 bytes)
            mac_bytes = data[36:44]
            try:
                mac_str = mac_bytes.decode('ascii')
            except Exception:
                mac_str = mac_bytes.hex()

            print(f'Chip CCFG read successfully! Size: {len(data)} bytes', flush=True)
            print(f'Sensor MAC at 0x1FFCC: {mac_str}', flush=True)

            # Clean reset so debug domain is released (trap 6)
            reset_cmd = [
                str(DSLITE),
                'flash',
                '--config',
                str(CONFIG),
                '-s',
                'KeepDebugPoweredAtDisconnect=false',
                '-r',
                '2',
            ]
            subprocess.run(reset_cmd, capture_output=True)
            print('Clean reset issued. Target saved.', flush=True)
            return 0

        print(f'[{attempt:02d}/{max_attempts}] Probing... (no connection yet)', flush=True)
        time.sleep(0.3)

    print('\nProbe loop timed out after 60 attempts without detecting target.', flush=True)
    return 1


if __name__ == '__main__':
    sys.exit(run_probe())
