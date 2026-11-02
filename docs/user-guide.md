# OpenWyzeSense user guide

This guide is for people who want to **use** OpenWyzeSense: flash a sensor or a gateway, pair, and live with it. How
it works inside, and how it is tested, is in the other files under `docs/` (start with `docs/FACTS.md` and
`docs/v1-contact.md`).

## 1. What it is

Open firmware for Wyze Sense devices, which Wyze no longer sells or fixes:

- **Sensor firmware** for the **v1 contact sensor** (door / window). Many v1 contacts died from a stock bug: on a low
  battery they lose their address and send all zeros (`docs/known-issues.md` §1). With our firmware they work again,
  under their original MAC, which is still in the chip.
- **Gateway firmware** for the **Wyze Sense USB dongle**, for a **v1 contact board** turned into a gateway, and for a TI
  LaunchPad. It speaks the stock dongle's USB protocol, so [wyzesense2mqtt-rs](https://github.com/HclX/wyzesense2mqtt-rs)
  uses it as if it were a stock dongle.

It works with stock devices **both ways**: stock sensors pair with our gateway, and our sensors pair with a stock
dongle. Extras that change what goes over the air (over-the-air updates, for example) are **off unless the host turns
them on**.

What works today:

| | Status |
|---|---|
| Our gateway on the Wyze dongle, with stock sensors | works (all six stock sensor types pair and report, including `0xAD` offline logs) |
| Our sensor on a v1 contact, with a stock dongle or our gateway | works (reed switch, button, battery standby) |
| Over-the-air updates of our sensors, through our gateway | works (~3 minutes per sensor; multi-sensor & fleet staging supported) |
| A v1 contact board as a gateway | works; its range is shorter than the dongle's (see §5) |
| An ESP32 / ESP8266 + v1 contact as a Wi-Fi (IP) gateway | built & simulated (`esp32/`); mDNS, web UI, and zero-ladder `nv.json` migration included |
| v1 motion sensor (`sensor-motion-v1`) | built & simulated (PIR, `120 s` clear + `0xAB` summary, dual-slot OTA); hardware bench check pending |
| Repurposed v1 contact DIY sensors (leak `0x03`, dry-contact `0x01`, PIR `0x02`) | works on the standard `sensor-contact-v1` image via runtime `NV_CFG` (`0x21`) profiles (§8); survives OTA |
| Other v2 sensor models (climate, leak, ...) | run them on stock firmware with our gateway |

## 2. What you need

- A Linux PC (x86-64). Everything else is fetched by the repository's scripts.
- A **TI LAUNCHXL-CC1310** LaunchPad. Its on-board debugger (XDS110) flashes the v1 contacts. Not needed to flash a
  dongle.
- For each v1 contact: a way to touch its test pads (§3). Either solder thin wires, or hold pogo connectors steady on
  them while provisioning.
- Optional: [wyzesense2mqtt-rs](https://github.com/HclX/wyzesense2mqtt-rs) and an MQTT broker, to get sensor events
  into Home Assistant or similar.

Set up once:

```
git clone https://github.com/HclX/openwyzesense && cd openwyzesense
tools/toolchain.sh                   # compiler + OpenOCD into .toolchain/ (pinned, checksummed)
tools/toolchain.sh uniflash          # TI UniFlash, ~380 MB: only needed for v1 contacts
sudo cp tools/udev/*.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger
cmake --preset fw && cmake --build --preset fw
```

`tools/udev/` grants non-root access to the Wyze USB dongle (`99-wyze-dongle.rules`, `1a86:e024`), the TI LaunchPad
(`71-ti-xds110.rules`, `0451:bef3`), and ESP32/ESP8266 USB-serial boards (`72-esp-usb-serial.rules`).
`fw` builds every sensor, gateway, boot selector, and OTA `.img` image into `build/fw/firmware/`.

Instead of building, you can take the images from a **GitHub Release** (Releases page of the repository): each
release has every device's image with the version in its name, `SHA256SUMS`, and a README table saying which file goes
on which device with which tool (`tools/release_bundle.py`, made by CI; a `v*` tag publishes it). Use the tools from
the same tag: `git checkout v<version>`.

## 3. Flashing a v1 contact sensor

Open the case and take the battery out. The board's test pads are on the side away from the components:

| Pad | Connect to (LaunchPad, debugger side of the middle jumper block) |
|---|---|
| `+` | 3V3 |
| `-` | GND |
| `M` | TMS |
| `C` | TCK |
| `S` | RST (between TXD and TMS): **required**, see below |

On the LaunchPad, **pull the TMS, TCK, TDO, TDI and RST jumpers**, so its own chip is off these lines. The pad map with
a photo is in `docs/v1-contact.md`.

### Single-command provisioning (recommended)

**All five wires, `S` included.** Every step ends with a board reset through `S`. Without it (only `+`, `-`, `M`,
`C`), the chip stops answering the debugger after its first write, and on its stock firmware sometimes after a read,
until it is reset: `provision` then stops half-way. If that happens, touch the `S` pad to `-` (GND) for a moment, or
take all wires off for a few seconds, and run the command again; it carries on from the backup it already took.

Connect the pads (either soldered thin wires or pogo pins held steady for ~25 seconds) and run:

```
uv run python tools/uniflash.py provision --sensor
```

`provision --sensor` runs every safety and install step in one command:

1. Reads the chip's CCFG and verifies it still holds its 8-hex-digit factory MAC (refuses to touch flash if missing).
2. Saves a verified read-only full dump (`flash.bin`, `ccfg.bin`, `fcfg1.bin`, `backup.json`) into
   `backups/contact_v1/<MAC>/` (if a backup already exists for `<MAC>`, it verifies it instead of overwriting your
   original stock dump).
3. Installs the Page 0 boot selector (`boot-wyze_contact_v1.bin`) and Slot A (`sensor-contact-v1-slot-a.img`) with
   read-back verification, never touching Page 31.
4. Enables the ROM serial bootloader backdoor on the `W` pad (`DIO10`, active low; pass `--bootloader button` for
   `DIO11` or `--bootloader keep` to leave `BL_CONFIG` unchanged).
5. Releases the debug power domain and resets the board cleanly.

### Step-by-step manual commands

If you prefer running each step explicitly, `flash` and `slots` always verify `--backup` against the connected chip's
CCFG MAC before erasing anything:

```
uv run python tools/uniflash.py backup backups/contact_v1/manual-1
uv run python tools/uniflash.py slots build/fw/firmware/boot-wyze_contact_v1.bin \
                                      build/fw/firmware/sensor-contact-v1-slot-a.img \
                                      --backup backups/contact_v1/manual-1
uv run python tools/uniflash.py enable-bootloader w --yes
```

Wiring `RX` / `TX` too (to the LaunchPad's RXD / TXD) gives you the sensor's console on `/dev/ttyACM0` at 115200
(sending any character within 10 s of boot keeps the console awake with a sliding 5-minute idle timer; `sleep` returns
to standby immediately).

Then put the battery back in (a fresh CR2032) and pair it (§4).

## 4. Using a sensor

| What you do | What happens |
|---|---|
| Insert the battery | the LED blinks once (`250 ms`); the sensor starts (`5 × 200 ms` rapid blinks if battery is low or MAC is missing) |
| Hold the button **3 seconds** | pairing: at `3.04 s` **while still held**, the LED blinks `3 × 400 ms` and pairing starts immediately (`3 × 400 ms` when joined, `3 × 100 ms` if no gateway answered) |
| **Short press** | the LED blinks once (`100 ms`); the sensor reports in and opens a **5-minute update window** (§6) |
| Move the magnet away / back | it reports open / closed (LED stays off) |

The LED is off the rest of the time, to save the battery. Between events the sensor sleeps; it wakes for the magnet,
the button and its regular check-ins.

To pair it, turn scanning on at your gateway first: in wyzesense2mqtt-rs, from its web page, `POST /api/scan`, or MQTT.
It shows up with its original MAC, as a contact sensor.

## 5. The gateway

### On a Wyze Sense USB dongle

Back up the dongle first; the backup is the only way back to stock (`dongle_sbl.py` auto-detects the Wyze USB dongle by
VID:PID `1a86:e024`, or pass `--device /dev/hidrawN` if you have more than one):

```
uv run python tools/dongle_sbl.py backup backups/dongle-1
uv run python tools/dongle_sbl.py flash build/fw/firmware/gateway-dongle-app.bin --backup backups/dongle-1 --yes --fast
```

When `dongle_sbl.py` finishes, unplug the dongle and plug it back in right away as prompted (pass `--wait` if you want
the script to wait until it sees the replugged dongle answer `Inquiry`). It keeps its MAC, and its sensor list is
carried over into our firmware's format (`--nv migrate`, the default): sensors already paired with it stay paired. The
tool lists what it carried over and what it dropped as invalid (a sensor without a proper MAC, two sensors on one
address), and saves it all as `nv.json` in the backup directory. `--nv wipe` starts with an empty list instead;
`--nv my.json` imports a list you edited (`tools/nv_json.py export` / `import`).
To put stock firmware back: `tools/dongle_sbl.py restore --backup backups/dongle-1 --yes`. Its sensor list comes back
as it was when you made the backup: sensors paired with our firmware since then need pairing again.

### With wyzesense2mqtt-rs

A dongle with our firmware is used like a stock one: plug it in and run wyzesense2mqtt-rs as usual (`host_link.py` also
auto-detects either the TI LaunchPad `0451:bef3` or the Wyze USB dongle `1a86:e024` when `PORT` is omitted).

A gateway on a serial port (a LaunchPad, a v1 contact board) reaches wyzesense2mqtt-rs over its WebSocket bridge. Set
`bridge.enabled: true` in its config (and `usb.dongle: "none"` if it should not look for USB dongles), then:

```
uv run python tools/ws_bridge.py --device /dev/ttyACM0 --gateway ws://<host>:8080/ws/bridge [--token SECRET]
```

### On a v1 contact board

A spare v1 contact board can be a gateway (build `gateway-contact-v1`). It runs from 3.3 V mains power, not a
battery, because its receiver is always on. Its gateway MAC is the board's own MAC. Provision it in one command:

```
uv run python tools/uniflash.py provision --gateway
```

(This backs up into `backups/contact_v1/<MAC>/`, flashes `gateway-contact-v1-app.bin`, enables the `W` bootloader
backdoor for future UART updates, and resets.) Its host link is on the `RX` / `TX` pads (115200), so connect it either
to `tools/ws_bridge.py` as above or to an ESP32 / ESP8266 relay below.

Its range is shorter than the dongle's: place it centrally, or use more than one.

### Over Wi-Fi: an ESP32 or ESP8266 next to the contact board

An ESP32 (ESP32-S3, classic ESP32) or ESP8266 wired to the contact board (`+`, `-`, `RX`, `TX`, plus optional `S` and
`W`) relays the gateway to wyzesense2mqtt-rs over Wi-Fi (`esp32/README.md`):

- **mDNS discovery**: reach its setup & diagnostics web UI at `http://ows-relay-<mac4>.local/`.
- **Web UI maintenance**: reset the CC1310 radio, pause startup auto-sync (`skip_gwsync`), flash a custom radio image,
  view per-sensor RSSI / battery / last-seen diagnostics, and update the ESP itself over HTTP.
- **Zero-ladder dongle-to-ESP migration**: export `nv.json` from your existing USB dongle (`dongle_sbl.py backup` or
  `host_link.py --nv-export nv.json`) and upload it via **Import `nv.json`** in the ESP web UI (`POST /contact/nv` or
  `host_link.py --nv-import nv.json`) — all paired sensors move over in seconds without re-pairing on ladders.

## 6. Updating sensors over the air

Sensors flashed with our firmware can be updated without opening them, through **our** gateway (not a stock dongle):

```
uv run python tools/host_link.py /dev/ttyACM0 --ota <MAC> build/fw/firmware/sensor-contact-v1-slot-a.img \
                                                          build/fw/firmware/sensor-contact-v1-slot-b.img
```

To stage an update for several sensors at once (comma-separated MACs) or for every paired sensor of that board type
(`--ota-all`), so you can arm the update once and walk around short-pressing each sensor's button:

```
uv run python tools/host_link.py /dev/ttyACM0 --ota-all build/fw/firmware/sensor-contact-v1-slot-a.img \
                                                        build/fw/firmware/sensor-contact-v1-slot-b.img
```

Use `/dev/hidrawN` for a dongle. The tool preserves existing gateway flags (`GW_FLAG_DEDUP`, `GW_FLAG_FIX_ZERO_MAC`,
etc.), turns the gateway's update feature on for the session, and defaults to a 360-second idle window that
automatically extends as blocks advance. When it says `staged`, **give each target sensor's button a short press**:
its 5-minute update window opens, and the gateway updates them one at a time (~3 minutes per sensor), restarting each
into the new slot while keeping the old slot as a fallback until the new one confirms itself. A sensor only accepts an
update within 5 minutes of a press of its own button, only with enough battery (2.5 V), and only a newer version for
its board (a build's version is the repository's commit count, printed by the sensor at start-up as `slot A v70`). If
`wyzesense2mqtt-rs` is using the same serial/HID port directly, either trigger OTA through `wyzesense2mqtt-rs` or stop
it briefly while `host_link.py` owns the port.

## 7. The serial bootloader (optional)

`uniflash.py provision` (or `uniflash.py enable-bootloader w --yes`) enables the CC1310's built-in ROM serial
bootloader on the `W` pad (`DIO10`, or `button` on `DIO11`), touching only `BL_CONFIG` in the CCFG (the MAC stays).
Once enabled, `tools/uart_sbl.py` (and the ESP32 / ESP8266 gateway's `gwsync` / `/contact/flash`) can reflash the chip
over its `RX` / `TX` pads with any 3.3 V USB-serial adapter instead of the LaunchPad. If our firmware is already
running on the UART, `uart_sbl.py` enters the ROM bootloader in software automatically (with a 15-second hardware
watchdog fallback); otherwise hold `W` to GND while resetting (power or `S`), then release:

```
uv run python tools/uart_sbl.py --port /dev/ttyUSB0 flash build/fw/firmware/boot-wyze_contact_v1.bin \
                                                    build/fw/firmware/sensor-contact-v1-slot-a.img --yes
```

then reset it: `S` to GND for a moment, or take the power away. It takes about 20 seconds; the MAC and settings stay.

## 8. Repurposing a v1 contact sensor (DIY leak, dry-contact, motion)

Every `wyze_contact_v1` board runs the same standard `sensor-contact-v1` firmware and dual-slot OTA images, while
keeping an optional 8-byte profile (`NV_CFG`, item `0x21`) in Flash Page 30 (`OWNV`) that overrides its reported sensor
type, `DIO7` polarity (`invert`), internal pull resistor (`up`, `none`, `down`), and debounce window (`1..5000 ms`).
Because Page 30 is untouched by OTA slot updates, your custom profile survives every future OTA update automatically.

### Hardware wiring on `wyze_contact_v1`

On the component side of the `wyze_contact_v1` PCB, the glass reed switch bridges **`DIO7`** (signal pad) to **`GND`**
(`-` battery negative). For DIY builds where a stray magnet should never trigger the input, desolder or snip one leg of
the glass reed switch and wire your external probe/module to the **`DIO7`** reed pad and **`-` (`GND`)** (plus **`+`**
for 3.3 V micropower modules):

| DIY sensor | Wiring (`wyze_contact_v1`) | Console preset | Profile settings (`NV_CFG` `0x21`) | How it behaves |
|---|---|---|---|---|
| **Water-leak detector** (`0x03`) | Two stainless-steel probes wired across the **`DIO7` reed pad** and **`-` (`GND`)** (desolder glass reed) | `profile leak` | `type 03`, `invert 1`, `pull up`, `debounce 100` | Dry = internal pull-up holds `DIO7` high (`state = 0`); water bridges `DIO7` to `GND` for `>= 100 ms` -> reports `0xA2` wet (`state = 1`), 2 h heartbeat |
| **Normally-open dry contact** (`0x01`) | Pushbutton, float switch, pressure mat, or relay NO contact across **`DIO7`** and **`-` (`GND`)** (desolder glass reed) | `profile contact-no` | `type 01`, `invert 1`, `pull up`, `debounce 40` | Open switch = `0`; contact closure to `GND` = `1` (`open` / active) |
| **Normally-closed dry contact** (`0x01`) | External wired door/window reed or NC tamper loop across **`DIO7`** and **`-` (`GND`)** | `profile contact` | `type 01`, `invert 0`, `pull up`, `debounce 40` | Loop closed to `GND` = `0`; loop opened = `1` |
| **External 3.3 V PIR motion** (`0x02`) | 3.3 V micropower push-pull PIR output (e.g. AM312 / EKMC) to **`DIO7`**, supply to **`+`** / **`-`** (remove glass reed) | `profile motion` | `type 02`, `invert 0`, `pull none`, `debounce 40` | Rising `DIO7` pulse sends `0xA2` (`1`), counts 12 × 10 s PIR buckets (`0xAB`), and clears (`0`) 120 s after last pulse |

### Configuring over the UART console (or `nv_json.py`)

Connect `RX`, `TX` and `-` (`115200 8N1`) during the 10-second boot window (before pairing, or run `leave` first if
already paired as another sensor type):

```
profile leak        # or: profile contact-no | profile contact | profile motion | profile default
cfg                 # prints: cfg type 03 invert 1 pull up debounce 100
join                # or hold the button 3 seconds to pair with your gateway
sleep               # immediately return to coin-cell standby
```

You can also tune individual fields (`type <contact|motion|leak|climate|contact2|motion2|hex>`, `invert <0|1>`,
`pull <up|none|down|default>`, `debounce <0..5000>`), or pre-build a Page 30 image via `tools/nv_json.py import` with
`"cfg": {"type": "03", "invert": true, "pull": "up", "debounce_ms": 100}` (`docs/nv-format.md`).

## 9. Troubleshooting

| Problem | What to try |
|---|---|
| `uniflash.py` fails to connect | Check the pads: `M` / `C` swapped? Power on `+` / `-`? The LaunchPad's TMS / TCK / RST jumpers pulled? Hold pogo pins straight and firm. |
| `PermissionError` on `/dev/hidraw*` or `/dev/ttyACM*` | Install `tools/udev/*.rules` into `/etc/udev/rules.d/`, reload udev (`§2`), and unplug/replug the USB device. |
| "no MAC" for a sensor | Its chip lost its MAC. It needs one written (a build with `-DOWS_SENSOR_MAC=77xxxxxx`); ask in an issue. |
| A soldered sensor stopped answering after flashing | `uv run python tools/uniflash.py reset` (needs `S` wired). Taking the power away also works, but only if `RX` / `TX` are disconnected too, since they keep the chip half-powered. |
| The sensor does not pair | Gateway scanning on? Hold the button until the LED blinks 3 times (`3 seconds`, while still holding). Try again close to the gateway. |
| No reports from far away | Check the RSSI in wyzesense2mqtt-rs; below about -85 dBm reports get lost. Move the gateway or add one. |
| The update does not start | Short press the sensor's button after the tool offers the update; the battery must be above 2.5 V; it must be our gateway. |
| I want stock firmware back on a dongle | `tools/dongle_sbl.py restore --backup <its backup> --yes`. |
| I want stock firmware back on a v1 contact | Not supported yet: the backup has it, but the tools never write the part of the chip that holds the MAC. The stock firmware also has the bug that killed these sensors. |

Bugs and questions: open an issue at https://github.com/HclX/openwyzesense/issues.
