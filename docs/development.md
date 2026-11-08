# Development

Setting up a development environment: building the firmware, the host tools and the ESP relays from source, running
the tests, and the repository's own tools. To **use** OpenWyzeSense from a release instead, see the
[user guide](guide/README.md). The system design is in [DESIGN.md](design/DESIGN.md); the rules for contributors (and agents)
are in [AGENTS.md](../AGENTS.md).

## Set-up

A Linux PC (x86-64). Everything else is fetched and pinned by the repository's scripts (`tools/toolchain.lock`):

```
git clone https://github.com/HclX/openwyzesense && cd openwyzesense
tools/toolchain.sh                   # compiler + OpenOCD into .toolchain/ (pinned, checksummed)
tools/toolchain.sh uniflash          # TI UniFlash, ~380 MB: only for the v1 contact bench
tools/toolchain.sh esp-idf           # ESP-IDF for the ESP32 / ESP32-S3 relay (~6 GB): only for esp32/idf
sudo cp tools/udev/*.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger
```

`tools/udev/` grants non-root access to the Wyze USB dongle (`99-wyze-dongle.rules`, `1a86:e024`), the TI LaunchPad
(`71-ti-xds110.rules`, `0451:bef3`), and ESP32/ESP8266 USB-serial boards (`72-esp-usb-serial.rules`). The Python tools
run with `uv run`; the Rust host crates (`crates/`) with `cargo`.

## Building and testing

### Without hardware

Run these steps in order so that Python tests depending on built host tools (`test_ows_sbl.py`, `test_gwsync.py`) and firmware ELF images (`test_firmware.py`) execute instead of skipping:

```bash
# 1. One-time toolchain setup (fetches pinned arm-none-eabi-gcc + OpenOCD into .toolchain/)
tools/toolchain.sh

# 2. Build host targets & run native C unit tests (12 CTest suites, including simulated RF apps)
cmake --preset host && cmake --build --preset host && ctest --preset host

# 3. Build CC1310 firmware images (build/fw/firmware/<product>.{elf,hex,bin,img})
cmake --preset fw && cmake --build --preset fw

# 4. Run Python reference, ground-truth replay, host-tool PTY, and firmware ELF safety tests
uv run pytest

# 4a. The Rust host crates (ows-host, ows-flash): a simulated dongle, golden NV vectors shared with the Python
#     reference (tools/nv_golden.py writes them; change one, regenerate, both must agree)
cargo test

# 5. Run formatting and static analysis checks (clang-format, clang-tidy, ruff, shfmt, shellcheck)
tools/lint.sh          # use `tools/lint.sh --fix` to auto-format
```

Additional headless checks:
- **Host code coverage** (`clang` + `llvm-cov`): `tools/coverage.sh`
- **Full-stack IP E2E test** (requires `mosquitto`, `mosquitto-clients`, and a `wyzesense2mqtt-rs` binary):
  ```bash
  OWS_W2M=/path/to/wyzesense2mqtt-rs uv run pytest reference/tests/test_host_e2e.py
  ```
- **ESP32 / ESP8266 IP relay firmware builds** (requires `build/fw/firmware/gateway-contact-v1-app.bin` built first):
  ```bash
  tools/idf.sh build     # ESP32-S3 and classic ESP32 (LOLIN32)
  tools/pio.sh run       # ESP8266 (Wemos D1 mini / NodeMCU)
  ```

### Hardware-in-the-loop tests (opt-in)

Hardware tests (`reference/tests/test_hw*.py`) are skipped by default and require physical bench hardware. Build `fw` first (`cmake --preset fw && cmake --build --preset fw`), then opt in via environment variables:

| Environment Variable(s) | RF TX? | Bench Hardware Required | Test Suites & Prerequisites |
|---|---|---|---|
| `OWS_HW=1` | **No** | TI `LAUNCHXL-CC1310` (jumpers installed on `P4`), plus RTL-SDR + `rtl_433` for `test_hw.py` | `test_hw.py`, `test_hw_boot.py`, `test_hw_ccfg.py`.<br>For `test_hw_gateway.py` (passive house mirror), build `build/fw-mirror` first:<br>`cmake --preset fw -B build/fw-mirror -DOWS_PASSIVE=ON -DOWS_PAN=0x845B -DOWS_SHORT=0x3366 && cmake --build build/fw-mirror` |
| `OWS_HW=1 OWS_HW_TX=1` | **Yes** (`0x65FE`) | `LAUNCHXL-CC1310` + spare dongle (`/dev/hidraw6`) / RTL-SDR | `test_hw_inputs.py`, `test_hw_sensor.py`, `test_hw_ota.py`.<br>For `test_hw_tx.py`, build `build/fw-tx-test` first:<br>`cmake --preset fw -B build/fw-tx-test -DOWS_DEBUG=ON -DOWS_PAN=0x65FE -DOWS_SHORT=0x0F0F && cmake --build build/fw-tx-test` |
| `OWS_HW=1 OWS_HW_CONTACT=1` | **No** | `LAUNCHXL-CC1310` (jumpers pulled) wired via 2-pin cJTAG to `wyze_contact_v1` (`tools/toolchain.sh uniflash`) | `test_hw_contact_gateway.py` (requires `build/fw-mirror` built as above). |
| `OWS_HW=1 OWS_HW_TX=1 OWS_HW_CONTACT=1` | **Yes** (`0x65FE`) | `LAUNCHXL-CC1310` (jumpers pulled) wired to `wyze_contact_v1` + spare dongle | `test_hw_contact_v1.py`, `test_hw_contact_battery.py`, `test_hw_contact_ota.py`. |
| `OWS_HW=1 OWS_HW_ESP32=<ip> OWS_W2M=<bin>` | Optional (`OWS_HW_TX=1`) | Networked ESP32-S3 wired to `wyze_contact_v1` on LAN + `mosquitto` | `test_hw_esp32.py` (build `tools/idf.sh build` and `fw` first; add `OWS_HW_TX=1` to also test `/contact/flash`). |

Optional HIL overrides: `OWS_HW_UART=/dev/ttyACMx` (serial port), `OWS_HW_DONGLE=/dev/hidrawX` (spare dongle), `OWS_HW_SECONDS=<n>` (capture duration for `test_hw.py` / `test_hw_gateway.py`), `OWS_HW_SAVE=<dir>` (save host/board logs), `OWS_HW_ESP32_AUTH=<secret>`, `OWS_HW_ESP32_PORT=/dev/ttyACMx`, `OWS_HW_CONTACT_MAC=<mac>`, `OWS_BRIDGE=<base URL>` (the house bridge `test_hw_gateway.py` compares with).

To flash a LaunchPad manually through its XDS110:
```bash
cmake --build --preset fw --target flash-gateway-launchpad
```

## A serial gateway with wyzesense2mqtt-rs

The LaunchPad's UART speaks the stock dongle's host protocol. [wyzesense2mqtt-rs](https://github.com/HclX/wyzesense2mqtt-rs)
accepts remote dongles over a WebSocket (`bridge.enabled: true` in its config; `usb.dongle: "none"` if it should
not look for USB dongles), and `tools/ws_bridge.py` connects our gateway to it the way its `dongle_bridge` connects
a stock one:

```
uv run python tools/ws_bridge.py --device /dev/ttyACM0 --gateway ws://<host>:8080/ws/bridge [--token SECRET]
```

## Without hardware: the firmware as host programs

`cmake --preset host && cmake --build --preset host` also builds the apps for the host board
(`firmware/boards/host/`) as programs, `build/host/firmware/ows-host-{gateway,sensor,sniffer}`. They run in real
time and share a simulated air over local UDP, so a host gateway and host sensors pair and report as on the
radio. The gateway's console is a pseudo-terminal, a serial port to anything that talks to a dongle:

```
B=build/host/firmware
$B/ows-host-gateway --console pty --pty-link /tmp/gw.tty --nv /tmp/gw.nv &       # its flash page in a file
uv run python tools/ws_bridge.py --device /tmp/gw.tty --gateway ws://127.0.0.1:8080/ws/bridge &
$B/ows-host-sensor --nv /tmp/sensor.nv        # console: join, event 1, ...; !input button 1 / 0, !input event 1, !reset
```

With wyzesense2mqtt-rs listening (`bridge.enabled: true`, `usb.dongle: "none"`), the gateway shows up as a dongle;
turn scanning on (its web UI, `POST /api/scan`, or MQTT), press the sensor's button, and it pairs; its switch
then reaches MQTT. `reference/tests/test_host_e2e.py` does all of that against a private mosquitto (pairing two
sensors, restarts of both sides) and runs in CI (`e2e-host`, wyzesense2mqtt-rs built at a pinned commit); locally:
`OWS_W2M=<path to wyzesense2mqtt-rs> uv run pytest reference/tests/test_host_e2e.py`.

## Repository tools for the devices

The user guide uses only `ows-flash` (`crates/ows-flash`; `cargo build --release -p ows-flash`, then
`target/release/ows-flash`) with release files. The repository has Python counterparts that work with the build's
files directly (`build/fw/firmware/`):

| Tool | What | `ows-flash` equivalent |
|---|---|---|
| `tools/uniflash.py` | v1 units over the LaunchPad's 2-pin cJTAG: `provision --sensor` / `--gateway` (build files by default), `backup`, `flash`, `slots`, `enable-bootloader`, `reset`, `read`; `flash` and `slots` need `--backup DIR` matching the chip's CCFG MAC. The `flash-*` CMake targets pass `OWS_BACKUP_DIR`. Details: [v1-contact.md](design/v1-contact.md) | `ows-flash v1 ...` |
| `tools/uart_sbl.py` | the CC1310's ROM serial bootloader over `R` / `T` with any 3.3 V USB-serial adapter, once `BL_CONFIG` is enabled on `R`; enters it in software if our firmware runs, otherwise hold `R` low across a reset | (the ESP relay's `/contact/flash`) |
| `tools/nv_json.py` | page 30 to JSON and back (`export` / `import`), including a sensor's DIY profile (`"cfg": {...}`, [nv-format.md](design/nv-format.md)) | `ows-flash nv export` / `nv import` |
| `tools/host_link.py` | a host for the dongle protocol over serial or hidraw (auto-detects `0451:bef3` / `1a86:e024`): sensor OTA (`--ota`, `--ota-all`), `--nv-export` / `--nv-import` | `ows-flash ota` |
| `tools/ws_bridge.py` | a serial gateway (LaunchPad, contact board on a USB-serial adapter) to wyzesense2mqtt-rs's WebSocket bridge (below) | (the ESP relay) |
| `tools/contact_image.py` | a sensor's boot selector + slot A as one image, for the relay's `/contact/flash` | |
| `tools/release_bundle.py` | the release directory: per-device file names, `SHA256SUMS`, `manifest.json`, its README (CI, on a `v*` tag) | |
| `tools/wiring_svg.py` | the user guide's wiring diagrams (`docs/img/wiring-*.svg`); rerun after changing a pin assignment | |

A v1 contact step by step with the repository's tools:

```
uv run python tools/uniflash.py backup backups/contact_v1/manual-1
uv run python tools/uniflash.py slots build/fw/firmware/boot-wyze_contact_v1.bin \
                                      build/fw/firmware/sensor-contact-v1-slot-a.img \
                                      --backup backups/contact_v1/manual-1
uv run python tools/uniflash.py enable-bootloader r --yes
uv run python tools/uart_sbl.py --port /dev/ttyUSB0 flash build/fw/firmware/boot-wyze_contact_v1.bin \
                                                    build/fw/firmware/sensor-contact-v1-slot-a.img --yes
```

The ESP relays' own build, flashing and bench notes: [esp32/README.md](../esp32/README.md).

## Bench hardware

The devices the hardware tests and bench checks use (as of 2026-10-29; moved from DESIGN.md §10.2).

All hardware verification across the project uses seven specific device classes on the engineering bench:

### 1. Production House Wyze USB Dongle (`MAC 778B5F63`) — *Strictly Read-Only Oracle*
- **Hardware & Firmware**: Unmodified stock Wyze Sense USB Dongle (`CH554` USB-HID `1a86:e024` + `CC1310F128` 5×5 QFN) running stock `0.0.0.47 V1.8 Gateway GW3U`.
- **Network & Setup**: Operates the live home network on **`PAN 0x845B`** (coordinator short address `0x3366`) with ~17 active household sensors, connected to the production `wyzesense2mqtt-rs` host.
- **Hard Safety Boundary**: **Never unplugged, never flashed, never sent `POST` requests, and no bench device may ever transmit on `PAN 0x845B`**. Used solely as a passive RF / `GET /api/dongles` ground-truth reference.

### 2. Spare Bench Wyze USB Dongle (`MAC 77A85F36` / `77A85A36` on `/dev/hidraw6`)
- **Hardware & Backup**: Stock Wyze Sense USB Dongle (`CH554` USB-HID + `CC1310F128` 5×5, dual LEDs on `DIO10` and `DIO13`). Full verified stock flash + CCFG backup preserved in `ground_truth/firmware/dongle_77A85A36_2026-10-23_before_ours/`.
- **Setup**: Plugged into host USB (`/dev/hidraw6`). Flashed via `ows-flash` (`0x4312` → CH554 SBL pass-through, Pages 0–29; Page 31 `CCFG` never written). Runs OWS `gateway-dongle-app.bin` (or restored stock firmware when testing OWS sensors against a stock coordinator) on isolated **Test `PAN 0x65FE`** (coordinator `0xD425`).

### 3. TI `LAUNCHXL-CC1310` LaunchPad (`MAC 77FED830` / IEEE-derived `7707D830`)
- **Hardware**: `CC1310F128RGZ` (7×7, 30 DIOs) + integrated **XDS110** JTAG/cJTAG debug probe and USB-UART (`0451:bef3`, `/dev/serial/by-id/usb-Texas_Instruments_XDS110*-if00`, `/dev/ttyACM0`). Onboard `BTN-1` (`DIO13`), `BTN-2` (`DIO14`), Red LED (`DIO6`), Green LED (`DIO7`).
- **Two Mutually Exclusive Bench Configurations** (cannot run simultaneously without physically moving the 7 isolation jumpers on header `P4`):
  - **Config A — Onboard Target Mode (Jumpers Installed, `Env 4A`)**: `GND`, `3V3`, `RXD` (`DIO2`), `TXD` (`DIO3`), `RST`, `TMS`, `TCK`, `TDO`, `TDI` bridged across `P4`. Programmed via pinned `xpack-openocd` (`reference/tests/hw.py`). Runs `sniffer-launchpad`, `gateway-launchpad`, or `boot-launchxl_cc1310` + `sensor-launchpad`.
  - **Config B — External 2-Pin cJTAG Probe Mode (Jumpers Pulled, `Env 4B`)**: Isolation jumpers removed so the onboard CC1310 is unpowered; female-to-male leads connect the **XDS110 (top) side** of `P4` (`TMS`, `TCK`, `RST`, `TXD`, `RXD`, `3V3`, `GND`) to soldered pads on an external Wyze v1 board, driven via TI UniFlash `DSLite` (`tools/uniflash.py`, `xds110_2pin.ccxml`).

### 4. Bench Wyze Sense v1 Contact Sensor (`MAC 777B42DF`, plus Zero-MAC Spares)
- **Hardware**: `CC1310F128RHB` (5×5, 15 DIOs), reed switch on `DIO7`, tactile button on `DIO11`, red LED on `DIO9`. Exposes PCB test pads `+` (`VDDS`), `-` (`GND`), `T`/`TX` (`DIO0`), `R`/`RX` (`DIO1`), `S` (`RESET_N`), `W` (`DIO10`), `M` (`TMS`), and `C` (`TCK`) (`docs/design/v1-contact.md`).
- **Backup & CCFG**: Full 128 KB stock dump archived prior to modification; CCFG `BL_CONFIG` (`0x1FFD8`) enabled once via `tools/uniflash.py enable-bootloader r` (`0xC5FE01C5`, active-low `R` `DIO1` backdoor, preserving factory ASCII MAC `777B42DF` at `0x1FFCC`).
- **Three Physical Sub-Setups**:
  1. **cJTAG Bench Sub-Setup (`Env 4B`)**: `CR2450` coin cell **removed**; wired to LaunchPad XDS110 Config B (`M←TMS`, `C←TCK`, `S←RST`, `R←TXD`, `T→RXD`, `+←3V3`, `-←GND`).
  2. **Standalone Coin-Cell / Bench-PSU Sensor Sub-Setup (`Env 5`)**: **All debug/UART wires detached** to prevent parasitic ESD-diode back-powering or `JTAG_PD` lockup; powered by `CR2450` (`3.0 V`) or programmable bench PSU (`2.35–3.10 V` via inline ammeter) running `boot-wyze_contact_v1` + `sensor-contact-v1-slot-{a,b}`.
  3. **ESP-Attached Gateway Radio Sub-Setup (`Env 4C` / `Env 5`)**: `CR2450` **removed**; powered from ESP (`+`/`-`) and wired `T/R/S` (5-wire, plus optional `M/C` debug port) or `T/R` only (4-wire software-SBL test), running `gateway-contact-v1-app.bin` on an isolated test PAN (`0x65FD` / `OWS_SUB_NETWORK`).

### 5. Unmodified Stock Reference Sensors (Sealed Factory Units, `Env 5`)
- **Stock v2 Contact (`MAC 77BD514D`, type `0x0E`)**: AAA battery, physical door magnet + side pairing button.
- **Stock v1 Motion (`MAC 77A8C793`, type `0x02`)**: `CR2450` cell, PIR Fresnel lens (`120 s` clear window + `0xAB` 12-bucket histogram) + side pinhole button. *(Supplemented by dead dumped unit `777A9A46` used for static Ghidra/disassembly ground truth).*
- **Stock v2 Climate (`MAC 77C68193`, type `0x07`)**: `CR2032` cell, I²C temperature/humidity sensor (`0xE8` reports) + side pairing button.

### 6. RTL-SDR 906.8 MHz Capture Receiver (`RTL2838` + `R820T` Tuner)
- **Setup**: USB SDR (`0bda:2838`) driven by `rtl_433` (`23.11`) recording raw `250 ksps` unsigned 8-bit IQ (`.cu8`) centered at **`906.7 MHz`** (`100 kHz` below the `906.8 MHz` carrier so the zero-IF DC spike stays out of the 5 kbps 2-GFSK passband), decoded by `reference/wyzesense_ref/iq.py`.
- **Gain Staging**: `-g 35` for room/house captures (`test_hw.py`), `-g 10` for same-bench stock sensor captures, and **`-g 0`** when capturing a LaunchPad or dongle transmitting within `< 50 cm` (`test_hw_tx.py`) to avoid front-end saturation.

### 7. Wi-Fi IP Relay Microcontrollers (`esp32/`, `Env 4C` & `Env 5`)
- **ESP32-S3 DevKit** (`esp32/idf/`, `IDF_TARGET=esp32s3`): `GPIO7` (or `3V3`)`→+`, `GND→-`, `GPIO17→R`, `GPIO18←T` (internal pull-up), `GPIO15/16↔M/C`, `GPIO4→S` (open-drain).
- **Classic ESP32 LOLIN32** (`esp32/idf/`, `IDF_TARGET=esp32`): `GPIO27` (or `3V3`)`→+`, `GND→-`, `GPIO17→R`, `GPIO18←T` (internal pull-up), `GPIO25/26↔M/C`, `GPIO4→S` (open-drain; avoids `GPIO5` boot-strap/LED and `GPIO6–11` SPI flash pins).
- **ESP8266 Wemos D1 mini / NodeMCU** (`esp32/esp8266/`, PlatformIO `d1_mini`): Split-UART wiring (`3V3→+`, `GND→-`, `D7`/`GPIO13` swapped `UART0_RX` `←T`, `D4`/`GPIO2` `UART1_TX` `→R`, `D8`/`GPIO15` **unconnected** to preserve SPI flash boot, `D2`/`GPIO4` `→S` open-drain, optional `D5/D6` `↔M/C`).

---
