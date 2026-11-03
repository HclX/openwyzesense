# Task list

What is open, kept up to date as work goes on (newest decisions first in each section). Done items move to the
commit log; facts go to FACTS.md.

## Now (2026-10-29): design-implementation alignment (`TASK-01`–`TASK-19`, `TASK-24`) — implemented in software

All P0–P3 software, tooling, and HIL-script tasks in [`tasks.md`](../tasks.md) (`TASK-01`–`TASK-19` and `TASK-24`) are
implemented and verified in headless CI suites (`ctest --preset host` 12/12 suites, `tools/coverage.sh`, `uv run pytest`,
`tools/lint.sh`, and cross-builds across `fw`, ESP-IDF, and ESP8266):
- [x] `TASK-01`–`TASK-08`: ESP SoftAP teardown on `GOT_IP` + `http_auth.c` Basic/Bearer auth; strict FIFO outage buffer +
      `1970` timestamp rebasing; RF/MAC/host bounds checks; sensor handshake/queue/`watch`/`restore` fixes; gateway
      `joining[]` expiry, short-addr collision guard & ACK-paced `0x5330`; `wyze_contact_v1` CCFG `0xC5FE0AC5` (`W` pad
      SBL); `OWNV` stack-staged compaction overflow guard; OTA slot-range & per-page `< 2.50 V` battery checks +
      `WIFI_PS_NONE`/`TCP_NODELAY`/WS `OP_PING` + streaming `sbl_stream_*` on both ESP-IDF and ESP8266.
- [x] `TASK-09`–`TASK-13`: CMake preset cleanup (`fw`, `host`, `coverage`; legacy `fw-*` presets & `OWS_ENABLE_TX`
      removed) + `10 s` → `5 min` sliding battery UART awake window; multiplexed `0x53F0`/`0x53F1` sub-ID envelope
      (`<= 56 B` CH554-safe); pinless software SBL entry (`chip_enter_sbl()`, `OWS_SUB_ENTER_SBL`, 4-wire `gwsync`);
      stock-faithful `3.04 s` while-held button trigger & LED blink patterns; complete `sensor-motion-v1` bring-up
      (`DIO9/10`, `DIO13`, `DIO12`, `DIO2`, `120 s` clear + `0xAB` buckets, `SLOTS` dual-slot OTA).
- [x] `TASK-14`–`TASK-19`, `TASK-24`: runtime `OWS_SUB_NETWORK` + zero-ladder `OWS_SUB_NV_IMPORT` (`/contact/nv`);
      stock `0xAD` history-log forwarding; 8-sensor batch + `"********"` wildcard OTA staging (`--seconds 360` default,
      flag-preserving); ESP Web UI controls, mDNS (`ows-relay-xxxx.local`), `gwsync` safe-mode (`/contact/gwsync?skip=1`),
      and sensor telemetry; `uniflash.py` `--backup` guard + one-step `provision` + VID:PID auto-detect; `OWNV` item
      `0x21` (`NV_CFG`) DIY sensor profiles; and updated `test_hw_*.py` HIL scripts (`-DOWS_PASSIVE=ON`).
- [ ] Pending physical HIL & manual bench regression checklist: [`docs/DESIGN.md` §10.4](DESIGN.md#104-pending-hardware--manual-regression-checklist-for-p0--p1-changes-task-01task-12-task-14)
- [ ] Open hardware research & future feature tasks (`tasks.md`): `TASK-20` (CH554 `> 63 B` & `0x4312` software exit),
      `TASK-21` (standalone 4-wire `chip_enter_sbl()` & `WDT` reboot), `TASK-22` (remaining stock v2 sensors with
      `A6_OURS`), `TASK-23` (shared-PAN single-ACK-owner roaming: gateway side done, see below), and `TASK-25` (under-load sensor low-battery
      detection & zero-extra-hardware ESP32 coin-cell simulator: [`docs/low-battery.md`](low-battery.md)).

## Earlier (2026-10-26): the ESP32 relay (#22)

Built and tested without hardware: the relay core and the outage buffer (`esp32/relay`, ctest + the e2e test with
wyzesense2mqtt-rs), the contact's bootloader in C (`esp32/contact`, `ows-sbl` on the simulated ROM), the S3
firmware (`esp32/idf`: Wi-Fi + setup access point, relay task, web pages, contact reset / reflash, OTA with
rollback; CI job `esp32`), and the ESP8266 version (`esp32/esp8266`: streaming `sbl_stream_*` contact reflash, CI job `esp8266`).
- [x] on hardware: the ESP8266 (NodeMCU, D1 mini): verified (2026-10-26)
- [x] on hardware: the classic ESP32 (LOLIN32): verified (2026-10-26)

## Plan (2026-10-24 evening): v1 contact as the DIY sensor

Worked through in order; each item ticked in the commit that does it. Issues on GitHub (HclX/openwyzesense,
labels P1/P2/P3, manual, v1-contact, tooling, agent-ok) carry the discussion; `gh` only on this repository. The bench unit 777B42DF stays wired to the
LaunchPad (free to use, hands-free through `tools/uniflash.py`); transmitting on the test PAN 0x65FE only.

**P1: OTA on the v1 contact** (updates without JTAG from then on)
- [x] #1 `sensor-contact-v1` as slot images (+ the boot selector for its board); `tools/uniflash.py slots`
      installs selector + slot A (pages 0-15 only, never 30-31): 777B42DF runs `slot A v66`
- [x] #2 console command `ota` (bench: the console is physical access, like the button): opens the OTA window and checks in
- [x] #3 `test_hw_contact_ota.py`: install version N, update to N+1 over the air through our gateway on the spare
      dongle, check the restart into slot B and the confirmation; no hands: passes (37 KB in 196 s, 2026-10-24)

**P2: a deployable battery image**
- [x] #4 LED: battery boards no longer keep it on; stock-faithful blink patterns implemented (`TASK-12`, `DESIGN.md` §9.1)
- [x] #5 the battery build (`fw`, standby with `10 s` → `5 min` console window): `test_hw_contact_battery.py`; the magnet and the button wake it (by hand)
- [x] #6 pin UniFlash in `tools/toolchain.sh` (download, SHA-256, unattended install, the cloud-agent link for scripting)

**P3: tooling**
- [x] #9 `AGENTS.md` and issue templates (other agents may work here); work on branches with PRs from now on
- [ ] #7 `test_hw_boot.py` flakiness: a harness reading only over JTAG

**Later (resolved)**: a v1 contact as an IP gateway. (Resolved from stock binary disassembly: the `W` pad `DIO10`
is the stock Factory UART Test & Provisioning Mode strap pin in `main()` at `0x0994C`; stock button/LED behaviour
`B1-B5` and the zero-MAC root cause are documented in `docs/stock-unknowns.md`, `docs/known-issues.md`,
`docs/DESIGN.md` §3.4, and `docs/stock-re/`.)

**Manual, for the user, in one go** (when at the bench; #8 & `docs/DESIGN.md` §10.4):
1. Other TI boards: a second LaunchPad lets the LaunchPad tests and the v1 contact bench run side by side.
2. The battery image on 777B42DF: the magnet a few times and a short (`< 1 s`) / long (`>= 3.04 s` while held) button
   press (events and OTA window from the button, while it sleeps).
3. Standby current: a meter in series with its 3.3 V (or a CR2450/CR2032 run), for the battery-life estimate.
4. More dead units: solder M, C, S, RX, TX, W, +, - (`docs/v1-contact.md`), then run `tools/uniflash.py provision`.
5. A unit kept on stock firmware for remaining stock checks, with the SDR running.

## Next (Bench & Research)

- **Rust host crate (#42)**: `crates/ows-host` + `ows-flash` (backup / flash / restore, NV JSON, manifest), tested
  against a simulated dongle and the golden NV vectors. Left: the first hardware run on the spare dongle 77A85A36
  (stock -> ours -> stock -> ours, from wyzesense2mqtt-rs or `ows-flash`), then the v0.1.0 tag (the user acks).
  OTA (#47): `ota` module + `ows-flash ota`, tested on the host programs (test_ows_flash_ota.py); left: a bench
  run on the LaunchPad sensor and the spare dongle. `ows-flash v1 provision` / `fetch-uniflash` (#50): done, tested
  against a fake DSLite chip; left: a bench run on a v1 contact wired to the LaunchPad.

- **Under-load coin-cell low-battery detection & zero-extra-hardware ESP32 HIL rig (`TASK-25`, [`docs/low-battery.md`](low-battery.md))**:
  done (#75): BATMON sampled during each frame (its lowest reading), reports carry it, the low-battery flag with
  hysteresis (3 below 2.625 V sets, 2.80 V clears), `bat` console command; our gateway no longer drops low-battery
  reports (byte 12 is the flag, not a length). Left: verify on the battery rig (PR #99) with a simulated weak cell,
  stock's after-the-frame sample alongside; the `< 2.50 V` `OWNV` write guard; an RF-silent `load <ms>`.

- **ESP32-S3 / ESP32 / ESP8266 relay on hardware (`DESIGN.md` §10.4)**: run `test_hw_esp32.py` (now aligned with
  `OWS_HW_ESP32_AUTH`, `gwsync`, and `/contact/nv` in `TASK-24`), verify fallback SoftAP teardown on `GOT_IP`, verify
  streaming `/contact/flash` via `sbl_stream_*` on all three MCUs, and verify 4-wire `chip_enter_sbl()` (`TASK-21`).
- **Runtime network PAN & zero-ladder NV migration (`#77`, `TASK-14`)**: implemented via `0x53F0` sub-IDs `0x04`
  (`OWS_SUB_NETWORK`) and `0x06` (`OWS_SUB_NV_IMPORT`) + `/contact/nv`; pending live bench check (`DESIGN.md` §10.4).
- **OTA updates for our sensors (`docs/ota.md`, `TASK-08`, `TASK-10`, `TASK-16`)**: verified over the air on the
  LaunchPad (`test_hw_ota.py`) and v1 contact (`test_hw_contact_ota.py`). Multi-sensor batch (up to 8 MACs) +
  `"********"` wildcard staging and `tools/host_link.py --ota` (`--seconds 360`, flag-preserving) are implemented over
  `0x53F0`/`0x53F1` sub-IDs `0x10..0x13`; left: measure battery charge of an update on hardware, and teach
  `wyzesense2mqtt-rs` the `0x53F0`/`0x53F1` sub-IDs (`0x01`, `0x02`, `0x04`, `0x06`, `0x10..0x13`).
- **`test_hw_boot.py` still fails about one run in four** on the XDS110's flakiness (a reset that does not take, a
  banner lost, a JTAG session failing): a RAM marker, `info` and a USB reset of the probe catch most of it. The
  firmware side checked separately: 10 of 10 probation boots of slot B wrote their attempt and printed their
  banner (2026-10-24). A harness that reads only over JTAG (attempt words, the app's state) would be steadier.
- **A6 mark (`GW_FLAG_ANNOUNCE`, `TASK-22`)**: stock v1 contact/motion disassembly proved bytes `11..15` are ignored, and
  stock v2 contact and climate took it on the bench (2026-10-24). Left: verify v2 leak and v2 motion when at hand.
- **Who left the LaunchPad UART at divisor 0 with a full FIFO** (FACTS.md §5): `uart_init` copes now; the cause is
  open (seen once, after an OTA run and a JTAG flash).
- **v1 contact sensors (R2)** (bring-up guide: `docs/v1-contact.md`): **our firmware runs on the zero-MAC unit 777B42DF**
  and pairs and reports as 777B42DF (2026-10-24; FACTS §5). Stock-faithful `3.04 s` while-held button trigger and LED
  patterns (`TASK-12`), pinned UniFlash + `--backup` guard + `provision` (`TASK-18`), dual-slot OTA, and `OWNV` `0x21`
  DIY profiles (`TASK-19`) are implemented; left: standalone coin-cell standby current measurement (`DESIGN.md` §10.4)
  and provisioning remaining dead units.
- **Zero-MAC mechanism (resolved in `docs/known-issues.md` §1, `docs/DESIGN.md` §3.4, and `docs/stock-re/stock_sensor.c`)**:
  disassembly of `contact_v1` and `motion_v1` proved that stock firmware stores runtime NV in TI `NVOCTP` on **Flash Page
  30 (`0x1E000–0x1EFFF`)**, while the duplicate 8-byte ASCII MAC in **CCFG (`0x1FFCC`, Page 31)** is written once during
  factory test (`0x08`; note `0x1FFD8` in CCFG is `CCFG_O_BL_CONFIG`, not a key) and **never read back** at normal boot
  (`Ssf_readMacAndKey()` at `0x11C50` / `0x11EC8` reads only Page 30 NV item `(6, 8, 0)`). Worse, `Sensor_init()`
  unconditionally writes Page 30 NV items `(6, 6, 0)` (boot counter) and `(6, 4, 0)` (`seq + 25`) *before*
  `Ssf_checkBootBattery()` checks `bat_v32 <= 0x53` (`2.59 V`). Across ~250 dying-CR2032 brownout boot loops, Page 30
  fills until `NVOCTP_compactPage()` (`0x11258`) runs `FlashSectorErase(0x1E000)` and browns out mid-erase—permanently
  wiping NV item `8` (and network item `1`) so the sensor transmits `00:00:00:00:00:00:00:00` forever even with a fresh
  battery, while its original ASCII MAC at `0x1FFCC` stays intact.
- **Stock firmware reverse-engineering (`docs/stock-re/`, `docs/stock-unknowns.md`)**: reconstructed C business logic
  for `contact_v1`, `motion_v1`, and `gateway_v1.8` on top of the TI SimpleLink `ti154stack` SDK, resolving `B1–B5`,
  `R1`, `R3–R8`, `J1–J3`, `J5`, `D1–D3`, `D6–D8`, and `Z1–Z2`.
- **Project review & UX issues (`docs/project-review-issues.md`, `tasks.md`)**: all software/tooling items
  (`TASK-01`–`TASK-19`, `TASK-24`) implemented; hardware bench checks tracked in `DESIGN.md` §10.4.
- **v1 motion (`sensor-motion-v1`, `TASK-13`)**: implemented in firmware (`PIR` on `DIO9`/`DIO10`, button `DIO13`, LED
  `DIO12`, `W` `DIO2`, UART `DIO0`/`DIO1`, `120 s` retriggerable clear timer + `0xAB` 12-bucket PIR summary, and
  `SLOTS` dual-slot OTA, preserving stock CCFG `0xC5FE01C5`); pending physical bench verification (`DESIGN.md` §10.4).

## Ideas (the user, 2026-10-24)

Both need the v1 contact's MCU to be a CC1310: a forum board photo says it is ("Unbricking Wyze Contact Sensor PCB
Reset Pin"); our dump will confirm, with the package.
- **Dead v1 contact boards as IP gateways** (#15; receiving works: `test_hw_contact_gateway.py`): our gateway app on a
  `wyze_contact_v1` board, host link on its RX/TX test points, an ESP32 relaying UART <-> WebSocket to wyzesense2mqtt-rs `/ws/bridge` (as tools/ws_bridge.py does);
  several for coverage (w2m takes many bridges). Our firmware answers the sync commands itself (no CH554): pick its
  MAC (e.g. the board label). Mains power (always receiving). +14 dBm needs VDDR HH: check the board's CCFG.
- **Roaming (#90, [`roaming.md`](roaming.md))**: the gateway side is done, tested on the host (`GW_FLAG_ROAM`: owner-only
  ACK, `OWS_SUB_OVERHEARD`, `OWS_SUB_ADDR_RANGE`; `test_roam.c`, `test_host_roam.py`) and on hardware (B15C + the spare
  dongle, 2026-11-01). Left:
  - wyzesense2mqtt-rs: ownership, cross-gateway dedup, handover policy (#96)
  - only after that: the house network's migration (the user, 2026-11-01). RF mesh backhaul without Wi-Fi: only if
    needed (much harder).

## Safety

- `ccfg_is_safe()`: the bootloader backdoor pin must be below the package's GPIO count - 1 (TI errata SWRZ062F
  Advisory 13: DIO14 max on the 5x5 dongle/v1 chip, DIO9 on 4x4, DIO30 on 7x7). It now accepts up to 31; read the
  package from FCFG1 USER_ID and refuse more. (The CCFGs we ship use pins 1 (`wyze_dongle`, `wyze_motion_v1`), 10
  (`wyze_contact_v1`), and 13 (`launchxl_cc1310`): all fine.)

## Back burner

- **CH554** (the dongle's USB chip): where it keeps the dongle MAC (likely its data flash, provisioned at the
  factory; no EEPROM on the board, per the user's 2019 blog), and what commands it supports beyond the ones Wyze
  uses. Leads: `set_ch554_upgrade` = sync 0x430E with payload `ff` (the user's notes, WyzeCamStudy/Indoor), probably
  its WCH bootloader; `dongle_get_ch554_hard_ver_req` in `dongle_app`; unknown async 0x5333 / 0x5334 / 0x5336.
  Static analysis of `dongle_app` / `iCamera` first. Live probing only on a dongle we can lose, never 77A85A36.
- **Get Key (0x4306)**: answered on a stock dongle (2019), not with our CC1310 firmware, so probably the CH554's or stock CC1310's.
  Confirm on a stock dongle; implement if anything needs it (Wyze's cloud auth; wyzesense2mqtt skips it).
- **Stock dongle firmware on the LaunchPad** stops at start-up with RTOS error 4 (7x7 vs the dongle's 5x5 package
  is a suspect). Not pursued.
- **Bootloader entry after a CCFG repair, as behaviour** (not just bytes): the UART-break test on the LaunchPad was
  inconclusive (the chip did not come up after a JTAG-triggered system reset).

## Protocol, still open

Stock behaviour resolved from `contact_v1`, `motion_v1` and `gateway_v1.8` disassembly and remaining v2/CH554 open items:
`docs/stock-unknowns.md` and `docs/stock-re/`.

- Play Chime (0x5370): its RF side is unknown (absent from `gateway_v1.4` and `gateway_v1.8`); our gateway ACKs it with `HOST_ACK` (`0x53FF`) as a no-op.
- The A5 XXTEA key: `Ssf_readMacAndKey` (`0x11C50` in `contact_v1`, `0x11EC8` in `motion_v1`) reads the 16-byte key from
  Page 30 (`0x1E000`) `NVOCTP` item `(6, 8, 0)` bytes `0..15` (written during factory provisioning via command `0x0A`);
  the factory per-unit key derivation on live stock units remains deliberately open.
- Stock types not yet joined to our gateway: v2 motion, leak, v1 contact.

## Verification

- #84 follow-up: two of our gateways both in pairing mode can still collide (carrier sense before a beacon, as stock
  seems to do, would fix it). The beacon delay itself passed on hardware 2026-10-31 (PR #86).
- `e2e-host` CI job (wyzesense2mqtt-rs end to end): first result not seen (private repo, no `gh` here).
- User guide (`docs/guide/`, #65), not yet walked through on hardware with release files only: the LaunchPad
  jumper-block order in `wiring-contact-launchpad.svg` (GND 5V 3V3 RXD TXD RST TMS TCK TDO TDI SWO, from the board's
  silkscreen as remembered), the second S / V− pads in `contact_v1.jpg` (read from the photo's colours), an ESP's
  first flash with plain `esptool` and the `-full` image, and the dashboard's sensor OTA in wyzesense2mqtt-rs.
- A long soak test (days) of our gateway with sensors on the test network.
- Current on a battery board (standby, receive windows).

## Housekeeping (state as of 2026-10-23)

- First GitHub Release (#89): push a `v0.1.0` tag once the user decides; CI publishes the bundle
  (`tools/release_bundle.py`). Every CI run already uploads the same bundle as the `release-bundle` artifact.

- House sensors off the house network since the tests: 77BD514D (on the spare dongle), 77A8C793 and 77C68193 (no
  gateway). The user: fine to leave them unpaired for now; no need to remind.
- The LaunchPad runs our sensor firmware (`fw` `sensor-launchpad`).
- The spare dongle 77A85A36 runs our gateway; `ows-flash restore` gives stock back.
- wyzesense2mqtt-rs (the user's): could learn the multiplexed `0x53F0`/`0x53F1` sub-IDs (feature flags, build info, network, NV import, OTA).
