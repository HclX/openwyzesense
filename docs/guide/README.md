# OpenWyzeSense user guide

OpenWyzeSense (OWS) is open firmware for Wyze Sense hardware, which Wyze no longer sells or fixes. This guide covers
using it, from flashing a dongle to building your own Wi-Fi gateway. Everything here uses only the files of a
[release](https://github.com/HclX/openwyzesense/releases) and the hardware listed for each level. You don't need
to clone the repository or build anything. (To build or change the firmware, see
[docs/development.md](../development.md).)

## What it does

- **Gateway firmware for the Wyze Sense USB dongle.** It speaks the stock dongle's USB protocol, so
  [wyzesense2mqtt-rs](https://github.com/HclX/wyzesense2mqtt-rs) uses it as if it were a stock dongle, and adds
  options on top of that (on-gateway deduplication, sensor updates over the air, several gateways on one network).
- **Sensor firmware for the v1 contact sensor** (door / window). Many v1 contacts died from a stock bug: once the
  battery runs low, they lose their address and send all zeros, and a new battery does not fix it. With our firmware
  they work again under their original MAC, which is still in the chip.
- **A Wi-Fi gateway** built from a v1 contact board and an ESP32, ESP32-S3 or ESP8266 board (the ESP side:
  [wyzesense_relay](https://github.com/HclX/wyzesense_relay)).

It works with stock devices **both ways**: stock sensors (all six types, v1 and v2) pair with our gateway, and our
sensors pair with a stock dongle. Extras that change what goes over the air are **off unless the host turns them on**.

## Levels

Each level stands on its own; pick the one you need. Later levels refer back to earlier ones for shared steps.

| Level | What you get | Hardware | Opens a device? |
|---|---|---|---|
| [1. Flash a Wyze USB dongle](1-dongle.md) | Our gateway on your existing dongle; your paired sensors stay paired | the dongle, a Linux PC | no |
| [2. Revive a dead v1 contact](2-revive-contact.md) | A "zero-MAC" contact sensor working again | the sensor, a TI LaunchPad, 5 wires | yes, the sensor |
| [3. Flash a working v1 contact](3-contact-sensor.md) | Immunity to the zero-MAC bug, updates over the air, DIY sensor types (leak, dry contact, PIR) | as level 2 | yes, the sensor |
| [4. Build a Wi-Fi gateway](4-wifi-gateway.md) | A gateway anywhere you have Wi-Fi and USB power; several of them for a large house | a v1 contact board, an ESP board, a LaunchPad (once), 6 wires | yes, the sensor |
| [5. wyzesense2mqtt-rs with OWS features](5-wyzesense2mqtt.md) | Deduplication, roaming between gateways, firmware updates from the dashboard | levels 1 or 4 | no |

Reference: [pads of the contact and the dongle](pads.md), [troubleshooting](troubleshooting.md).

## What works today

| | Status |
|---|---|
| Our gateway on the Wyze dongle, with stock sensors | works: all six stock sensor types pair and report, including the offline log (`0xAD`) |
| Our sensor on a v1 contact, with a stock dongle or our gateway | works: reed switch, button, battery reports |
| Updating our sensors over the air, through our gateway | works, about 3 minutes per sensor |
| A v1 contact board + ESP32 / ESP32-S3 / ESP8266 as a Wi-Fi gateway | works |
| DIY sensors on a v1 contact (leak, dry contact, PIR) | works on the standard contact image, set up from its console |
| v1 motion sensor with our firmware | built and simulated; not yet checked on hardware |
| v2 sensors | not flashable; run them on stock firmware with our gateway |

## Getting a release

All files of a release are on its [GitHub release page](https://github.com/HclX/openwyzesense/releases): firmware
images with the version in their names, the `ows-flash` tool (Linux x86-64), udev rules, `SHA256SUMS`, and a
README with a table of which file goes on which device. Download what your level needs into one directory, then:

```
sha256sum -c --ignore-missing SHA256SUMS      # every downloaded file must say OK
chmod +x ows-flash-linux-x86_64-*
ln -s ows-flash-linux-x86_64-* ows-flash      # so the commands below read ./ows-flash
```

The commands in this guide run from that directory. Backups land in `backups/` beneath it: **keep that
directory**. A dongle's backup is your only way back to stock firmware.

So that `ows-flash` can reach the hardware without root, install the udev rule for each device you use, once:

| Device | Rule |
|---|---|
| Wyze USB dongle | `99-wyze-dongle.rules` |
| TI LaunchPad (levels 2-4) | `71-ti-xds110.rules` |

```
sudo cp 99-wyze-dongle.rules 71-ti-xds110.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Then unplug and replug the device. (Running `ows-flash` with `sudo` also works, but then the backups belong to root.)

Bugs and questions: [open an issue](https://github.com/HclX/openwyzesense/issues).
