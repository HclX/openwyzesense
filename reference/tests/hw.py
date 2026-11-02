# Flashing and resetting the LaunchPad for the hardware tests, through its XDS110 with the pinned OpenOCD.
# The XDS110 now and then fails a JTAG scan ("scan chain interrogation failed"), or the reset after programming
# does not take and leaves the CPU in OpenOCD's RAM helper: everything here retries. Its JTAG side can also hang
# for good (every scan all zeroes) until the probe is reset on USB, which usb_reset() does (no replug needed).
import fcntl
import os
import pathlib
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]


def uart():
    """The LaunchPad's application UART: by its stable name, as a USB reset of the XDS110 renumbers ttyACM*."""
    found = sorted(pathlib.Path('/dev/serial/by-id').glob('usb-Texas_Instruments_XDS110*-if00'))
    return str(found[0]) if found else '/dev/ttyACM0'


def openocd(*commands):
    ocd = next((ROOT / '.toolchain').glob('xpack-openocd-*/bin/openocd'))
    args = [
        str(ocd),
        '-f',
        'interface/xds110.cfg',
        '-c',
        'transport select jtag',
        '-c',
        'adapter speed 2500',
        '-f',
        'target/ti_cc13x0.cfg',
    ]
    for c in commands:
        args += ['-c', c]
    r = subprocess.run(args, capture_output=True, text=True)
    return r.stdout + r.stderr


def usb_reset(vid_pid='0451:bef3'):
    """Resets the XDS110 on USB (USBDEVFS_RESET), as unplugging it would; the target stays powered."""
    out = subprocess.run(['lsusb', '-d', vid_pid], capture_output=True, text=True).stdout.split()
    if len(out) < 4:
        return
    fd = os.open(f'/dev/bus/usb/{out[1]}/{out[3][:3]}', os.O_WRONLY)
    try:
        fcntl.ioctl(fd, 0x5514, 0)
    finally:
        os.close(fd)
    time.sleep(3)


def reset():
    openocd('init', 'reset run', 'shutdown')


NV_PAGE = 0x1E000  # flash page 30: the paired-sensor table (soc/cc13x0/nv_flash.c)


def flash(elf, tries=3, erase_nv=False):
    """Programs elf, verified, and starts it; erase_nv: also forget what page 30 holds. Raises if every try
    fails."""
    out = ''
    erase = [f'flash erase_address {NV_PAGE:#x} 0x1000'] if erase_nv else []
    for _ in range(tries):
        out = openocd('init', 'reset halt', *erase, f'program {elf} verify reset exit')
        if 'Verified OK' in out:
            reset()  # the reset in the same session sometimes does not take
            return
        if 'all zeroes' in out:
            usb_reset()  # the probe's JTAG side hung
        time.sleep(2)
    raise RuntimeError(f'flashing {elf} failed:\n{out[-2000:]}')


GPIO_DOUTCLR = 0x400220A0
GPIO_DOE = 0x400220D0


def press(dio, pressed=True):
    """Holds an active-low input pin (a LaunchPad button) low from the debugger, while the firmware runs: the
    pin's output driver is enabled with its output low. pressed=False hands the pin back to its pull-up."""
    bit = 1 << dio
    doe = f'[expr {{$doe | {bit:#x}}}]' if pressed else f'[expr {{$doe & ~{bit:#x}}}]'
    for _ in range(3):
        out = openocd(
            'init',
            f'mww {GPIO_DOUTCLR:#x} {bit:#x}',
            f'set doe [lindex [read_memory {GPIO_DOE:#x} 32 1] 0]',
            f'mww {GPIO_DOE:#x} {doe}',
            'shutdown',
        )
        if 'rror' not in out:
            return
        if 'all zeroes' in out:
            usb_reset()
        time.sleep(1)
    raise RuntimeError(f'driving DIO{dio} failed:\n{out[-1000:]}')


def release(dio):
    press(dio, False)
