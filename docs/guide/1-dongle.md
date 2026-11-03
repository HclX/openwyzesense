# Level 1: flash a Wyze USB dongle

Replace the dongle's firmware with our gateway, over USB, without opening it. Your paired sensors stay paired, and
wyzesense2mqtt-rs keeps working as before.

**You need:** the Wyze Sense USB dongle (the one that plugs into a Wyze camera; on a PC it shows up as USB
`1a86:e024`), a Linux PC (x86-64), and from the release `ows-flash-linux-x86_64-<version>`,
`ows-gateway-dongle-<version>.bin` and `99-wyze-dongle.rules` (set up as in [Getting a release](README.md#getting-a-release)).

No wiring: the dongle's chip is reached through its own USB port.

> If wyzesense2mqtt-rs runs on this PC, its dashboard can do all of this for you (the dongle's **Actions** dialog,
> **Firmware** section; see [level 5](5-wyzesense2mqtt.md)). The steps below do the same from the command line.
> Either way, stop anything else that uses the dongle while `ows-flash` runs.

## 1. Back up

```
./ows-flash backup backups/dongle-1
```

This reads the whole chip twice, compares the reads, and saves the stock firmware and the sensor list to
`backups/dongle-1`. It writes nothing. **Keep this directory**: it is the only way back to stock, and `ows-flash`
refuses to flash a dongle without this dongle's backup.

With more than one dongle plugged in, pass `--device /dev/hidrawN` to each command (`ows-flash` otherwise takes the
first `1a86:e024`).

## 2. Flash

```
./ows-flash flash ows-gateway-dongle-<version>.bin --backup backups/dongle-1 --yes
```

It takes about 20 seconds (`--fast`: about 10, checking the chip's CRC instead of reading everything back). The
dongle keeps its MAC, and its sensor list is carried over into our firmware's format (`--nv migrate`, the default).
The tool lists what it carried over and anything it had to drop, for example a sensor with no proper MAC, or two
sensors sharing one address, and saves the list as `nv.json` in the backup directory. A dropped sensor has to be paired
again.

When it finishes, the dongle normally restarts by itself. If `ows-flash` says it is not back, unplug it and plug it back
in.

Other choices for the sensor list: `--nv wipe` starts empty, and `--nv my.json` imports a list you edited
(`./ows-flash nv export` / `nv import`).

## 3. Use it

Plug it in and run wyzesense2mqtt-rs (or any other stock-dongle software) as before. Pairing, events and
everything else behave as they do with stock firmware. To turn on the extras, see [level 5](5-wyzesense2mqtt.md).

## Updating to a new release

Flash the new image the same way, with the same backup directory:

```
./ows-flash flash ows-gateway-dongle-<new version>.bin --backup backups/dongle-1 --yes
```

The sensor list stays (`--nv migrate` reads our own format too).

## Back to stock

```
./ows-flash restore --backup backups/dongle-1 --yes
```

The dongle gets its stock firmware and the sensor list from the time of the backup. Sensors paired since then need
pairing again.

## Moving the sensors to a Wi-Fi gateway

To move this dongle's sensors to a [Wi-Fi gateway](4-wifi-gateway.md) without re-pairing them, use the `nv.json` from
the backup directory (or `./ows-flash nv export backups/dongle-1/flash.bin -o nv.json`); level 4 shows where it goes.
