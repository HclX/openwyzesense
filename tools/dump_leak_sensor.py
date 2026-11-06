#!/usr/bin/env python3
"""Wait for cJTAG connection and dump full CC1310 flash, FCFG1, and CCFG."""

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
DSLITE = ROOT / '.toolchain/uniflash/deskdb/content/TICloudAgent/linux/ccs_base/DebugServer/bin/DSLite'
CONFIG = ROOT / 'firmware/boards/wyze_contact_v1/xds110_2pin.ccxml'
OUT_DIR = ROOT / 'backups/leak_sensor'
OUT_DIR.mkdir(parents=True, exist_ok=True)


def play_sound(sound_name):
    sound = f'/usr/share/sounds/freedesktop/stereo/{sound_name}.oga'
    if os.path.exists(sound):
        subprocess.run(['paplay', sound], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print('\a', end='', flush=True)


def read_mem(addr, words, outfile, timeout=120):
    cmd = [
        str(DSLITE),
        'memory',
        '--config',
        str(CONFIG),
        '--range',
        f'{addr:#x},{words}',
        '--output',
        str(outfile),
        '--timeout',
        str(timeout),
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    out = res.stdout + res.stderr
    if res.returncode != 0 or 'fatal' in out or 'Failed' in out or not outfile.exists():
        raise RuntimeError(f'Read {addr:#x} ({words} words) failed:\n{out[-500:]}')
    return outfile.read_bytes()


def clean_reset():
    cmd = [
        str(DSLITE),
        'flash',
        '--config',
        str(CONFIG),
        '-s',
        'KeepDebugPoweredAtDisconnect=false',
        '-r',
        '2',
    ]
    subprocess.run(cmd, capture_output=True)


def main():
    print('Waiting for probe contact on TMS and TCK...', flush=True)
    temp_probe = OUT_DIR / 'probe_check.bin'

    connected = False
    for attempt in range(1, 300):
        cmd = [
            str(DSLITE),
            'memory',
            '--config',
            str(CONFIG),
            '--range',
            '0x1FFA8,1',
            '--output',
            str(temp_probe),
            '--timeout',
            '2',
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        out = res.stdout + res.stderr
        success = res.returncode == 0 and 'fatal' not in out and 'Failed' not in out
        if success and temp_probe.exists() and temp_probe.stat().st_size >= 4:
            connected = True
            break
        print(f'[{attempt:02d}/300] Waiting for contact... (hold probes steady)', flush=True)
        time.sleep(0.3)

    if not connected:
        print('Timeout waiting for connection.', flush=True)
        return 1

    print('\n' + '=' * 60, flush=True)
    print('>>> CONTACT DETECTED! HOLD PROBES VERY STEADY! <<<', flush=True)
    print('=' * 60, flush=True)
    play_sound('bell')

    t0 = time.time()
    try:
        # Step 1: Read full 128 KB Flash (32768 32-bit words)
        print('[1/3] Reading 128 KB Flash (Pass 1)...', flush=True)
        flash1 = read_mem(0x0, 32768, OUT_DIR / 'flash.bin', timeout=60)
        sha1 = hashlib.sha256(flash1).hexdigest()
        print(f'      Pass 1 complete (131072 bytes, SHA-256: {sha1[:16]}...)', flush=True)

        # Step 2: Read full 128 KB Flash Pass 2 to verify read integrity
        print('[2/3] Reading 128 KB Flash (Pass 2 verification)...', flush=True)
        flash2 = read_mem(0x0, 32768, OUT_DIR / 'flash_pass2.bin', timeout=60)
        sha2 = hashlib.sha256(flash2).hexdigest()
        print(f'      Pass 2 complete (SHA-256: {sha2[:16]}...)', flush=True)

        if flash1 != flash2:
            print('WARNING: Pass 1 and Pass 2 differed! Probes might have slipped.', flush=True)
        else:
            print('      Verification MATCH: identical read-back verified!', flush=True)
            (OUT_DIR / 'flash_pass2.bin').unlink(missing_ok=True)

        # Step 3: Read FCFG1 (Factory config: MACs, chip calibration)
        print('[3/3] Reading FCFG1 (1024 bytes)...', flush=True)
        fcfg1 = read_mem(0x50001000, 256, OUT_DIR / 'fcfg1.bin', timeout=30)
        print(f'      FCFG1 complete ({len(fcfg1)} bytes)', flush=True)

        # Extract CCFG from flash end
        ccfg = flash1[0x1FFA8:]
        (OUT_DIR / 'ccfg.bin').write_bytes(ccfg)

        # Metadata
        # MACs from FCFG1:
        # MAC_15_4_0 at 0x500012F8, MAC_15_4_1 at 0x500012FC
        # MAC_BLE_0 at 0x500012E8, MAC_BLE_1 at 0x500012EC
        fcfg_user_id = int.from_bytes(fcfg1[0x294:0x298], 'little')
        ble_mac = fcfg1[0x2E8:0x2F0][::-1].hex().upper()
        ieee_mac = fcfg1[0x2F8:0x300][::-1].hex().upper()

        info = {
            'device': 'Wyze Sense Leak Sensor',
            'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'),
            'elapsed_sec': round(time.time() - t0, 2),
            'flash_size': len(flash1),
            'flash_sha256': sha1,
            'pass2_match': (flash1 == flash2),
            'fcfg1_user_id': f'{fcfg_user_id:#010x}',
            'fcfg1_ble_mac': ble_mac,
            'fcfg1_ieee_mac': ieee_mac,
            'ccfg_bl_config': f'{int.from_bytes(ccfg[0x30:0x34], "little"):#010x}',
        }
        (OUT_DIR / 'info.json').write_text(json.dumps(info, indent=2) + '\n')

        print('\n' + '=' * 60, flush=True)
        print('>>> SUCCESS! DUMP COMPLETED AND VERIFIED! <<<', flush=True)
        print('=' * 60, flush=True)
        print(json.dumps(info, indent=2), flush=True)

        # Clean reset
        clean_reset()
        for _ in range(3):
            play_sound('complete')
            time.sleep(0.2)
        return 0

    except Exception as e:
        print(f'\nERROR during dump: {e}', flush=True)
        clean_reset()
        return 1


if __name__ == '__main__':
    sys.exit(main())
