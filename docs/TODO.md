# Task list

The project's one task list: what is still open. Discussion happens in GitHub issues (`#n` below refers to this
repository's issues unless it says "archive"); done items go to the commit log, facts to [FACTS.md](FACTS.md), stock
behaviour still to find out to [stock-unknowns.md](stock-unknowns.md).

On 2026-11-03 (#67) this file took over what was open in `tasks.md` (the TASK-01…25 backlog), `docs/project-review-issues.md`
(the code review those tasks came from) and DESIGN.md §10.4 / §11.2; those were removed, and are in git history.
`TASK-nn` names below refer to that backlog.

## Next

- **Roaming, host side** (#41): ownership, cross-gateway dedup and handover in wyzesense2mqtt-rs. The gateway side is
  done and verified on hardware ([roaming.md](design/roaming.md)). Only after it: moving the house network onto our
  gateways, and only on the user's explicit go.
- **Battery** (`TASK-25`, [low-battery.md](design/low-battery.md), #40): reports under load and the low-battery flag
  are done (archive #75). Left: a run on the battery rig with a simulated weak cell, the `< 2.50 V` guard on `OWNV`
  writes, an RF-silent `load <ms>` console command, and a battery-life estimate per sensor (#40).
- **For discussion with the user, nothing to build yet**: #63 (a sensor's LED on trigger), #64 (recovering a dongle
  left in update mode, see `TASK-20` below).
- **`ows-flash` on the bench**: `ows-flash ota` (LaunchPad sensor through the spare dongle) and `ows-flash v1
  provision` (a v1 contact on the LaunchPad) have only run against simulated hardware.
- **The user guide on hardware** ([guide/](guide/README.md), #65): walk it through with release files only. Check the
  LaunchPad jumper-block order in `wiring-contact-launchpad.svg` (GND 5V 3V3 RXD TXD RST TMS TCK TDO TDI SWO, drawn
  from memory of the silkscreen), the second `S` / `V−` pads in `contact_v1.jpg` (read from the photo's colours), an
  ESP's first flash with plain `esptool`, and sensor OTA from the wyzesense2mqtt-rs dashboard.

## Bench checklist

Hardware checks of changes so far tested only headless (DESIGN.md §10.4 as of 2026-10-29). Tick with the date and
the evidence.

- **#81 relay wiring**: done on the LOLIN32 B15C with contact 777B42DF, 2026-11-06 (the user wired it; console logs
  of each run). `+` on GPIO27: starts and runs. Updates through `M`/`C`: ~20 s (four runs; the first build overflowed
  the relay task's stack, fixed). Unchanged code is left alone. Fallback with `M`/`C` off: bootloader by a power cycle
  with `R`/`W` low, ~10 s, with `S` and with `S` floating; the debug-port update with `S` floating too, so `S` is
  optional; `W` only for the fallback on units with that backdoor. Trap: an ESP32's GPIO4 pulls down at reset, so `S` wired to it but unconfigured holds the
  contact in reset. Station role on the same harness (fixed pins, 2026-11-06): a cJTAG dump of 777B42DF (powered from
  GPIO27, M/C polarity found, 32 pages, ~27 s, equal to the carried gateway image) and an SBL dump with the pad search
  (R/T/backdoor W found on GPIO17/18/23, chip ID 0x20018000), the two dumps byte-identical. 2026-11-07, Identify then
  Dump/Flash as separate steps: dump refused before Identify; per transport Identify, Dump (equal) and Flash of the
  running gateway image (10 pages verified, page 31 intact). `sbl2` (two hand probes on TX/RX, + and − clipped,
  2026-11-07): touch seen on the soldered 777B42DF, one round, then waiting for a lift (its backdoor is W, not on the
  probes). The leak sensor 77C685AE was dumped this way by the user (2026-11-07, `backups/`), so touch, lift and the
  DIO1 backdoor work on a real sensor; dumped again at 10:55 after a run with + and - swapped by mistake, so it
  survived that. Since: the station on four wires (two probes on GPIO17/18 for every mode) and the
  W/S check (probe 1 on T or R as the witness, probe 2's pad held low across a power-up: S keeps the chip in reset):
  on the leak sensor it called both remaining pads W (2026-11-07). Hypothesis to check: the board pulls its T line up,
  so the witness reads high even with the chip held in reset; a fix could compare the witness against the chip
  unpowered, or watch T for real UART activity instead of a level. Check on sensors whose pads are known; cJTAG
  on the two probes; a slip during an SBL dump.
  Left: a sensor that is not a v1 contact (the leak sensor), the user guide's level 4 and its wiring SVGs (`tools/wiring_svg.py`) on the new wiring; the
  ESP32-S3 (GPIO7, 15/16); a stock-backdoor unit (`R`) on the bootloader path; transmit bursts on GPIO power.

- `TASK-01` relays: the fallback access point closes as soon as Wi-Fi is back (watch it from a phone); with a
  password set, every changing endpoint answers 401 without it, on ESP-IDF and ESP8266, and `GET /setup` shows no
  secrets.
- `TASK-02` outage buffer: 5 magnet events with wyzesense2mqtt-rs stopped, 2 more while it replays: all 7 arrive in
  order. A cold-booted relay's buffered alarm (1970 clock) gets the current time after `0x5333`.
- `TASK-03`, `TASK-05`: `test_hw.py` and `test_hw_tx.py` on the LaunchPad.
- `TASK-04`: `test_hw_sensor.py`; on a coin cell, fast magnet toggles while the gateway is out of reach: the settled
  state is never dropped or inverted.
- `TASK-06`: a contact gateway keeps `BL_CONFIG` `0xC5FE0AC5`; `/contact/flash` refuses an image with a bad
  `BL_CONFIG`.
- `TASK-07`: `test_hw_tx.py::test_gateway_keeps_its_table_in_flash` and a 35-cycle pair / delete loop (real page 30
  compactions).
- `TASK-08`: `test_hw_boot.py`, `test_hw_ota.py`, `test_hw_contact_ota.py`; a sensor OTA through an ESP32 and an
  ESP8266 relay with no poll timeouts; OTA refused at 2.45 V (5 blinks); a relay whose server vanishes without a FIN
  goes down within ~40 s and buffers; `gwsync` and `/contact/flash` on all three ESP boards.
- `TASK-09`: `test_hw_contact_battery.py`; standby current on a coin cell with no wires (~1-2 µA expected).
- `TASK-10`: every `0x53F0` sub-ID through the dongle's CH554 (no truncation of the 63-byte reports); `test_hw_ccfg.py`.
- `TASK-11` / `TASK-21`: a 4-wire relay (no `W`, no `S`) enters the bootloader in software, flashes and comes back
  through the ~15 s watchdog; `ENTER_SBL` with no bootloader traffic recovers by the watchdog; `sbl` on a sensor's
  console, then `uart_sbl.py info` without touching `W` or `S`.
- `TASK-12`: the LED and button patterns by eye (guide level 3 table, plus 2 × 100 ms / 3 × 250 ms around an OTA); a
  gateway's scan blink and RX pulse; stock sensors pair and report.
- [x] `TASK-14`: a stock dongle's sensors moved to a relay by `nv.json` without re-pairing (2026-11-01, archive #90
  phase 1).

**Manual, for the user at the bench**: a second LaunchPad (so the LaunchPad tests and the v1 contact bench run side
by side); the battery image's button and magnet by hand; a unit kept on stock firmware for remaining stock checks,
with the SDR running.

## Stock compatibility (R1)

- Forward what the stock gateway v1.8 forwards and ours does not yet (FACTS.md §4.0, #79): `0xD1` / `0xD3` / `0xE1` /
  `0xE3` → `0x5319` (18 / 15 / 21 / 19 plaintext bytes, MAC in the table), `0xB2` → `0x5359`, `0xB4` → `0x535B`
  (repeated sequence number → `0x535E`), `0xB6` → `0x535D`, `0xAE` → `0x5338`, `0xAF` → `0x533B`, and the downlink
  `0xB3` / `0xB5` (unencrypted), `0xC0`, `0xD0`, `0xD2`, `0xE0`, `0xE2`. For devices we do not have (keypad, chime?):
  what they are is unknown, so test only against the binary's layouts.

## Hardware research

- `TASK-20`: the dongle's CH554 with frames over 63 bytes, and leaving update mode (`0x4312`) without a replug (#64).
- `TASK-22`: the A6 mark (`GW_FLAG_ANNOUNCE`) with the remaining stock v2 sensors: v2 leak and v2 motion (v2 contact
  and climate took it, 2026-10-24).
- v1 motion (`sensor-motion-v1`, `TASK-13`): built and simulated, never run on hardware.
- #84 follow-up (archive): two of our gateways pairing at once can still collide; carrier sense before a beacon, as
  stock seems to do, would fix it.
- Who left the LaunchPad UART at divisor 0 with a full FIFO (FACTS.md §5): `uart_init` copes; the cause is open (seen
  once, after an OTA run and a JTAG flash).
- `test_hw_boot.py` fails about one run in four on the XDS110's flakiness; a harness reading only over JTAG would be
  steadier.
- A long soak test (days) of our gateway with sensors on the test network.
- A battery's charge per OTA update.

## Small fixes (left from the code review)

- `tools/host_link.py` `--pair` sends `VERIFY_SENSOR` with a hard-coded type `ff 04`.
- `firmware/tests/test_gateway.c` `window()`: the frame-count check only catches more expected frames than sent.

## Ideas

- Shared-identity gateways as one network with several radios: done as roaming; an RF mesh backhaul without Wi-Fi
  only if needed (much harder).
- Full TX power on a v1 contact gateway (#29: its CCFG does not force VDDR HH).

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
- **Get Key (0x4306)**: answered on a stock dongle (2019), not with our CC1310 firmware, so probably the CH554's or
  stock CC1310's. Confirm on a stock dongle; implement if anything needs it (Wyze's cloud auth; wyzesense2mqtt skips it).
- **Stock dongle firmware on the LaunchPad** stops at start-up with RTOS error 4 (7x7 vs the dongle's 5x5 package
  is a suspect). Not pursued.
- **Bootloader entry after a CCFG repair, as behaviour** (not just bytes): the UART-break test on the LaunchPad was
  inconclusive (the chip did not come up after a JTAG-triggered system reset).

## Protocol, still open

Stock behaviour still open: [stock-unknowns.md](stock-unknowns.md) (resolved items: [stock-re/](stock-re/README.md)).

- Play Chime (0x5370): its RF side is unknown (absent from `gateway_v1.4` and `gateway_v1.8`); our gateway ACKs it
  with `HOST_ACK` (`0x53FF`) as a no-op.
- The A5 XXTEA key: stock reads it from page 30 (`NVOCTP` item `(6, 8, 0)` bytes `0..15`, written at the factory by
  command `0x0A`); how the factory derives it per unit stays open.
- Stock types not yet joined to our gateway: v2 motion, leak, v1 contact.
