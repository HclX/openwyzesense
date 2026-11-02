# OpenWyzeSense

Open-source firmware for Wyze Sense sensors (v1 and v2) and the Wyze Sense USB dongle, now discontinued.
- **To use it** (flash a sensor or a gateway, pair, update): [docs/user-guide.md](docs/user-guide.md)
- **System architecture & design**: [docs/DESIGN.md](docs/DESIGN.md)

Goals: keep the hardware working, fix stock bugs (first: v1 contact sensors losing their MAC on low
battery), and add features, while staying compatible with stock devices.

## Layout

| Path | What |
|---|---|
| `docs/DESIGN.md` | **authoritative system design document**: architecture, product matrix, stock vs. OWS comparison, RF/UART protocols, flash/NV/OTA layout, ESP32 relay, and cleanup roadmap |
| `ground_truth/` | raw evidence: RF captures (IQ), the bridge's decoded-state logs, firmware dumps. Never edited. |
| `docs/FACTS.md` | what is known about the protocol, each item marked verified / hypothesis / open, with evidence |
| `docs/requirements.md` | the project's requirements and decisions |
| `docs/known-issues.md` | stock bugs to fix, with evidence |
| `docs/stock-unknowns.md` | stock behaviour items resolved from binary disassembly and remaining open questions |
| `docs/stock-re/` | reconstructed C source code of the stock sensor (`contact_v1`, `motion_v1`) and gateway (`gateway_v1.8`) firmware with flash and SRAM address annotations |
| `docs/project-review-issues.md` | comprehensive project review findings: firmware/host-link/tool/test issues and product UX/DX recommendations |
| `firmware/` | the firmware (C, CMake). `products.cmake` lists what gets built: an app (`apps/`: gateway, sensor, sniffer) on a board (`boards/`: LaunchPad, Wyze dongle, Wyze v1 motion; placeholders for the boards we have no firmware for), e.g. `gateway-launchpad`, `gateway-dongle`, `sensor-motion-v1`. `common/`: the portable protocol code (gateway and sensor state machines); `hal/`: the hardware interface the apps use; `soc/cc13x0/`: it on the CC1310 (radio, UART, AES, flash, clock); `boards/host/`: it simulated, so native tests run the apps unchanged (`tests/test_apps.c`: gateway, sensor and sniffer on one simulated air) |
| `third_party/ti_coresdk/` | the subset of TI's SimpleLink core SDK the firmware builds from (BSD-3-Clause), vendored by `tools/vendor_coresdk.sh` |
| `crates/` | Rust host tools (#42): `ows-host`, the library wyzesense2mqtt-rs uses to back up, flash and restore a Wyze dongle and carry its sensor table into our NV format (a port of `dongle_sbl.py` and `nv_json.py`, same checks, held to the same golden vectors); `ows-flash`, its command line (in each release) |
| `tools/` | `toolchain.sh` (fetches the pinned compiler and OpenOCD into `.toolchain/`), `host_link.py` (a host for the dongle protocol over serial or hidraw), `ws_bridge.py` (our gateway's UART to wyzesense2mqtt-rs over its WebSocket dongle bridge), `sbl_common.py` (the ROM bootloader's framing and guarded write path, for `uart_sbl.py`; the dongle itself: `ows-flash`), `decode_stream.py`, `image_id.py` (an image's build info block and its image ID, #76), udev rules for the LaunchPad and the ESP boards' USB serial chips |
| `reference/` | Python reference implementation (`wyzesense_ref`: IQ front end, PHY, framing, payload, host protocol, and a gateway model that replays the captures byte for byte) and the tests that tie FACTS.md to `ground_truth/` |

## Building and Testing

### 1. Standard Headless Build & Test Suite (No Hardware Required)

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

### 2. Hardware-in-the-Loop (HIL) Tests (Opt-In Only)

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

## Using the gateway with wyzesense2mqtt-rs

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

## History

The project was developed in a private repository before this one was published, so this repository starts with a
single commit. Issue and PR numbers (`#90`) and commit hashes (`fdcaad9`) cited in the docs and in `ground_truth/` refer
to that earlier history, not to this repository's issues.

## License

MIT, see [LICENSE](LICENSE).
