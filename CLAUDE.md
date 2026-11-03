# Notes for Claude sessions

Other agents may work here too: `AGENTS.md` (workflow, hard rules, tests). Work goes on a branch per issue
(`issue-<n>-<slug>`) with a PR to `main` (`gh` on HclX/openwyzesense only); `main` is never committed to directly.

## What to trust
1. `ground_truth/`: raw captures, bridge logs, firmware dumps. Ground truth. Never edit it; add new
   captures as new session folders and update `ground_truth/SHA256SUMS` and its README.
   Some of it is not published (stock firmware dumps, long house-traffic recordings: `ground_truth/README.md`); tests
   needing it skip unless `OWS_GROUND_TRUTH` points at a full copy. Never add such files to this public repository.
2. `reference/tests/`: executable checks against ground truth. Run `uv run pytest` first; all must pass.
3. `docs/FACTS.md`: only rows marked **verified** are facts. **Hypothesis** rows need confirming before
   firmware depends on them. Keep this file the single place for protocol knowledge, with evidence for
   every verified row (and a test where possible).
4. Anything else (forum posts, other projects' docs, memory) is not evidence until checked against 1.

## Task list
`docs/stock-unknowns.md`: stock behaviour still to find out (buttons, reports, joins, dongle, boards), with why and
how, for helpers too; answers go into FACTS.md with evidence and the row is closed in the same commit.
`docs/TODO.md`: the one task list (next, bench checklist, hardware research, small fixes, ideas, back burner, protocol). Keep it current: add what
comes up, remove what gets done, in the same commit as the work.

## Requirements
`docs/design/requirements.md`: R1 compatibility with stock devices both ways, R2 fix the v1 zero MAC, R3 runtime
feature flags (protocol-affecting features off by default; compatible fixes always on), and the user's
decisions so far.

## Hardware and safety
- Dev board: TI LAUNCHXL-CC1310. The dongle and the v1 motion sensor are CC1310s; other sensor MCUs unknown.
- RTL-SDR (RTL2838 / R820T) with rtl_433 23.11 for captures; see FACTS.md §1 for the recipe.
- The user's house runs a stock dongle on PAN 0x845B (coordinator 0x3366, ~17 sensors), reported by a bridge
  whose URL stays out of the repository (`OWS_BRIDGE`). Use it read-only (`GET /api/dongles`, SSE `/api/events`); ask before
  any POST (scan/pair, raw commands).
- Do not transmit on 906.8 MHz without the user's OK, and never on the house PAN 0x845B. Receiving is fine.
  The user's second dongle (PAN 0x65FE, coordinator 0xD425) is the intended test network; a second test PAN, 0x65FD,
  is for our gateway on a v1 contact (the user's OK, 2026-10-24).
- Before flashing any stock device, dump and back up its whole flash, including CCFG. Never erase flash page
  31 (code + CCFG) except as part of a full reflash.
- No SDR recording overnight.
- Long recordings: run them as background tasks; a process started with `&` inside a short-lived shell call
  is killed when that call returns.

## Firmware build and test
- Layout: a product (`firmware/products.cmake`) is an app (`firmware/apps/`) on a board (`firmware/boards/<board>/`:
  `board_config.h` pins, `ccfg.c`, `board.cmake`). Apps use only the hardware interface (`firmware/hal/`, see its
  README), implemented for the CC13x0 in `firmware/soc/cc13x0/` and simulated by the host board
  (`firmware/boards/host/`), on which `tests/test_apps.c` runs the apps natively (pairing, events, resets,
  heartbeats over virtual hours, receiver on-time). New hardware access goes into `hal/` with both implementations.
  The same apps also build as host programs (`ows-host-gateway` / `-sensor` / `-sniffer`, `boards/host/host_main.c`):
  real time, the air over local UDP, the gateway's console on a pty. `test_host_e2e.py` runs wyzesense2mqtt-rs
  (`OWS_W2M=<binary>`) + mosquitto + `tools/ws_bridge.py` against them: no hardware, no radio, safe to run anytime. Boards we have no firmware dump for are README placeholders
  without build rules (user's decision). On `wyze_dongle` and `wyze_motion_v1` the built-in CCFG equals the stock one
  (`0xC5FE01C5`, `test_wyze_board_ccfg_is_the_stock_one`), whereas `wyze_contact_v1` enables the ROM bootloader on the
  `W` pad `DIO10` (`0xC5FE0AC5`) so `gateway-contact-v1` and provisioned contact boards keep UART/ESP SBL access. Boards
  that keep each unit's MAC in the CCFG (`BOARD_MAC_IN_CCFG`) build without one and are flashed with `<product>-app.bin`
  (or dual-slot `.img`) only, so the unit keeps its CCFG and MAC; `-DOWS_SENSOR_MAC=...` builds one in, for provisioning
  a unit that lost it.
- The Wyze dongle: `ows-flash` (`crates/ows-flash` on `crates/ows-host`, #42/#44; backup / flash / restore / nv) through its ROM bootloader, entered as
  WyzeSenseUpdater does (host command 0x4312, then the CH554 relays bootloader packets). It writes flash pages
  0-29, and page 30 (the NV page) only through the NV write as `--nv` says (#61: `migrate`, the default, carries
  the stock table into our format via JSON, saved as `nv.json` in the backup; `wipe`; `keep`; or an NV.json;
  `restore` writes the backup's page 30 back), never page 31 (code + CCFG with the bootloader config): do not
  erase or write page 31 on a dongle, ever (the user: a wrong bootloader pin there makes it unflashable).
  `uniflash.py` and `uart_sbl.py` take the same `--nv` (default `keep`). Back up first; the spare
  dongle's backup is `ground_truth/firmware/dongle_77A85A36_2026-10-23_before_ours`. Our gateway answers 0x4312
  as stock does, and can put its built-in CCFG back (`0x53F0` sub-ID `0x03` `OWS_SUB_CCFG`, `test_hw_ccfg.py` on the LaunchPad).
  `--fast` (GET_STATUS once per stretch, all-FF stretches skipped, CRC-only verify): 9.4 s instead of 18.9 s for
  our 38 KB image on the dongle (restore: 28 s / 59 s); the floor is ~12 ms per 58-byte packet (one HID round trip through the CH554).
  Since 2026-10-23 the spare dongle 77A85A36 runs our gateway (`fw` gateway-dongle-app.bin on pages 0-7; its
  stock page 30 table and page 31 kept); `ows-flash restore` puts stock back (from a copy of that backup: `flash` writes `nv.json` into the directory). `test_hw_sensor.py` therefore now
  checks our sensor against our gateway on the dongle (all six types pass); the stock-dongle runs are in ground truth. The CH554 answers the sync
  host commands (Inquiry, Get ENR, Get MAC) itself, so the dongle keeps its stock identity with our firmware.
- **v1 contact sensors** (the user's many zero-MAC units, the DIY version's main hardware): `docs/design/v1-contact.md`
  (pads, wiring to the LaunchPad with RESET_N, UniFlash over 2-pin cJTAG since OpenOCD cannot, the bring-up steps, and
  the traps: cold joints, pin counting, a 3V3-only power-cycle that is none, the hang after a UniFlash run). Back up
  every unit read-only first; never write its CCFG (its MAC) except enabling `BL_CONFIG` on `W` during provisioning.
  `tools/uniflash.py` (`backup` / `flash` / `slots` / `enable-bootloader` / `provision` / `reset` / `read`; `flash` and
  `slots` require `--backup <dir>` verifying the chip's CCFG MAC, and `flash-*` CMake targets pass `OWS_BACKUP_DIR`)
  always ends with a board reset with the debug domain released: no hands needed, a stuck unit comes back.
  `test_hw_contact_v1.py` (`OWS_HW=1 OWS_HW_TX=1 OWS_HW_CONTACT=1`, preset `fw`). While a sensor is wired to
  the LaunchPad's XDS110 (its jumpers pulled), the LaunchPad's own tests cannot run.
- **ESP32 / ESP8266 IP relay** (#22, `esp32/README.md`): `esp32/relay` (WebSocket relay core, strict FIFO outage buffer,
  `gwnv`), `esp32/contact` (`sbl.c` with `sbl_stream_*` pages 0-29, page 30 on request, page 31 never, plus `gwsync.c`),
  and `esp32/web` (`multipart.c`, `http_auth.c`, `mdns.c`) are portable C, built on the host (`esp32/host`: `ows-relay`,
  `ows-sbl`, `ows-gwsync`, and CTest suites `test_relay`, `test_buffer` / `relay_buffer`, `test_gwnv`,
  `test_multipart`, `test_http_auth`, `test_mdns`; tested by `test_host_e2e.py`, `test_ows_sbl.py`, `test_gwsync.py`),
  for **ESP32-S3** and **classic ESP32 (LOLIN32)** (`esp32/idf`, `tools/idf.sh build` with ESP-IDF pinned in
  `tools/toolchain.lock`; CI job `esp32`), and for **ESP8266 (D1 mini / NodeMCU)** (`esp32/esp8266`, `tools/pio.sh run`;
  CI job `esp8266`). The relay itself never transmits on 906.8 MHz; its contact board's gateway image does (the TX rules
  apply to that image). On hardware: `test_hw_esp32.py`.
- The stock dongle firmware does not start on the LaunchPad (7x7 package; the dongle's CC1310 is 5x5): RTOS error
  4 at start-up. Not pursued further.
- Everything builds from the repo: `tools/toolchain.sh` (pinned in `tools/toolchain.lock`; `tools/toolchain.sh uniflash`
  adds TI UniFlash, only for the v1 contact bench), CMake presets `host` (native tests), `fw` (CC1310 firmware), and
  `coverage` (`tools/coverage.sh` for `clang` + `llvm-cov` host coverage reports).
- Standard headless test sequence (run in order so `test_ows_sbl.py`, `test_gwsync.py`, and `test_firmware.py` do not
  skip for missing builds):
  1. `cmake --preset host && cmake --build --preset host && ctest --preset host` (12 C unit suites + host binaries)
  2. `cmake --preset fw && cmake --build --preset fw` (CC1310 firmware images)
  3. `uv run pytest` (reference, ground-truth replay, host-tool PTY tests, and `test_firmware.py` ELF checks)
  3a. `cargo test` (the Rust host crate `crates/ows-host`, #42: a simulated dongle, and the golden NV vectors that
     `tools/nv_golden.py` writes from the Python reference; change one, regenerate, both must agree)
  4. `tools/lint.sh` (mandatory before every commit, CI fails otherwise; `--fix` for the formatters)
  5. Optional full-stack host E2E: `OWS_W2M=<wyzesense2mqtt-rs> uv run pytest reference/tests/test_host_e2e.py`
     (requires `mosquitto` and `mosquitto-clients`).
- Flashing firmware that can transmit needs the user's permission (see the TX rule above).
- Hardware tests (`reference/tests/test_hw*.py`, skipped unless `OWS_HW=1`) flash and reset through
  `reference/tests/hw.py` (retries the XDS110's flaky JTAG). If every OpenOCD scan reads "all zeroes", the XDS110's JTAG
  side has hung: `hw.usb_reset()` (USBDEVFS_RESET on the probe) brings it back without a replug; the target was fine
  every time. See `docs/development.md` and `docs/design/DESIGN.md` §10.1 for the full HIL opt-in matrix (`OWS_HW=1`, `OWS_HW_TX=1`,
  `OWS_HW_CONTACT=1`, `OWS_HW_ESP32=<ip>`).
- Receive-only LaunchPad tests (`OWS_HW=1`):
  - `test_hw.py`: flashes `sniffer-launchpad.elf` (`fw`) and compares what the LaunchPad receives with an RTL-SDR
    recording (`OWS_HW_SECONDS`, default 180s).
  - `test_hw_boot.py` (`fw`, unjoined sensor): the OTA two-slot boot on the LaunchPad (selector in page 0, sensor app
    in slots A and B; `docs/design/ota.md`). Leaves the LaunchPad with the selector in page 0: any full flash replaces it.
  - `test_hw_ccfg.py` (`fw`): clears `BL_CONFIG` over JTAG and verifies `OWS_SUB_CCFG` (`0x53F0` sub-ID `0x03`) restores
    page 31 byte-for-byte.
  - `test_hw_gateway.py`: passive house-mirror gateway (requires `cmake --preset fw -B build/fw-mirror -DOWS_PASSIVE=ON
    -DOWS_PAN=0x845B -DOWS_SHORT=0x3366 && cmake --build build/fw-mirror`, and `OWS_BRIDGE=<the bridge's base URL>`).
- Transmitting LaunchPad tests (opt in with `OWS_HW=1 OWS_HW_TX=1`, only with the user's permission for the session):
  - `test_hw_inputs.py` (`fw`): BTN-1 (DIO13, reed switch / PIR) and BTN-2 (DIO14, pairing button, transmits beacon
    request on 3.04 s hold) "pressed" from the debugger (`hw.press(dio)`: output driven low while firmware runs).
    The UART is found by `/dev/serial/by-id` (`hw.uart()`): a USB reset of the probe renumbers `ttyACM*`.
  - `test_hw_sensor.py` (`fw`): pairs the sensor app with the spare dongle 77A85A36 on `/dev/hidraw6` (`OWS_HW_DONGLE`)
    as all six types, the last by its pairing button, then BTN-1 as its switch (~2 min; deletes itself afterwards).
  - `test_hw_ota.py` (`fw`, test PAN): an OTA update over the air, the LaunchPad sensor updated by our gateway on the
    spare dongle (`GW_FLAG_OTA`; flash with `ows-flash flash ... --fast`). ~5 minutes.
  - `test_hw_tx.py`: gateway built with `cmake --preset fw -B build/fw-tx-test -DOWS_DEBUG=ON -DOWS_PAN=0x65FE
    -DOWS_SHORT=0x0F0F && cmake --build build/fw-tx-test`, made to answer frames injected over debug host command
    `0x43F0` (checked with the SDR at gain 0) and its flash table across a reset. Two layers keep TX off the house PAN:
    compile-time checks on `OWS_PAN`, and `radio_tx()` refusing frames on `0x845B`.
- External v1 contact tests on the LaunchPad's 2-pin cJTAG (jumpers pulled; mutually exclusive with onboard LaunchPad
  tests): `OWS_HW=1 OWS_HW_CONTACT=1` for passive `test_hw_contact_gateway.py`; add `OWS_HW_TX=1` for
  `test_hw_contact_v1.py`, `test_hw_contact_battery.py`, and `test_hw_contact_ota.py`.
- On the LaunchPad a software system reset can leave the ROM waiting (FACTS.md §5): `chip_reset` restarts through
  the selector instead. If the board prints nothing at all but runs, the XDS110's serial side may have hung:
  `hw.usb_reset()`.
- Flash page 30 holds the gateway's table and the sensor app's join (item `0x20`) and optional DIY pin/type profile
  (item `0x21`, `NV_CFG`), in **our own format** since #61
  (`docs/design/nv-format.md`; `wyzesense_ref.nvows`): the firmware formats any other page (stock, garbage) on start, so a
  stock dongle's table is carried over by the tools (`tools/nv_json.py export` -> JSON -> `import`), never by the
  firmware. A power cut may lose NV data but never stops a start (#38). `wyzesense_ref.nvpage` decodes stock pages.
- OpenOCD: `-c "transport select jtag"` before `target/ti_cc13x0.cfg`. Its reset is VECTRESET (CPU only):
  the RF core and the HF crystal keep their state, which `board.c` undoes at boot. Its JTAG now and then fails
  ("scan chain interrogation failed"), leaving the CPU halted in OpenOCD's RAM helper: the flash targets reset in
  a second session, and the tests retry until the boot banner shows. The `arm-none-eabi-gdb`
  in the toolchain needs libncursesw5, which this machine lacks; use OpenOCD's own commands (halt, reg, mdw).
- The LaunchPad's UART output starts at reset; open `/dev/ttyACM0` before resetting or the banner is lost.
