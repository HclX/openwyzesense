# Requirements and decisions (from the user)

## Requirements

- **R1 Compatibility.** The open-source sensor and gateway firmware must be backward compatible with the
  stock devices and cross-support them: open-source gateway with stock sensors, and stock gateway with
  open-source sensors.
- **R2 Fix the v1 empty MAC.** v1 contact sensors start sending an all-zero MAC when the battery runs low
  (docs/known-issues.md §1). This is the main bug to fix, and the reason Wyze abandoned the v1 sensors.
- **R3 Feature flags.** Extra features are locked behind runtime flags, enabled by host → gateway or
  gateway → sensor messages.
  - Clarification: **everything that affects the communication protocols is off by default.** Fixes that do
    not change what a stock peer sees (no new message, field value or timing) need no flag and are always on.
    This lets bugs be fixed without breaking compatibility.
  - Status: gateway flags are set by the host with `0x53F0` sub-ID `0x01` (`OWS_SUB_FLAGS`, with legacy 0/4-byte `0x53F0`
    retained for compatibility; FACTS.md §9, `GW_FLAG_*` in `firmware/common/gateway.h`): `DEDUP` (a sensor's retried
    report goes to the host once), `FIX_ZERO_MAC` (known-issues.md §1) and `OTA` (updates of our sensors, docs/ota.md:
    the gateway takes the host's offers; our sensors join exactly as stock ones, so a stock dongle sees nothing new from
    them).
  - Gateway → sensor: with `ANNOUNCE` on, the A6 of a pairing carries "OW", a version and the gateway's features
    for sensors in its padding (bytes 11-15, zero from a stock dongle; `wyze_app.h` A6_OURS_*): our sensor keeps
    them with its join, so it knows whether its gateway is ours. Stock v1 disassembly (`contact_v1`, `motion_v1`)
    confirmed bytes 11-15 are ignored, and stock v2 contact and climate accepted it on the bench; remaining v2 types are
    tracked in `TASK-22` (`docs/stock-unknowns.md` J6) before enabling the flag by default.

## Decisions

| Decision | Detail |
|---|---|
| Zero-MAC damage is permanent on stock firmware | A v1 contact sensor whose Page 30 `NVOCTP` item `(6, 8, 0)` was wiped during a low-battery compaction erase ([`docs/DESIGN.md` §3.4](DESIGN.md#34-definitive-root-cause-of-the-stock-v1-zero-mac-bug), [`docs/known-issues.md` §1](known-issues.md)) does not recover with a fresh battery because stock firmware never falls back to the intact CCFG MAC at `0x1FFCC`. Reflashing our firmware restores its real MAC from CCFG (`GW_FLAG_FIX_ZERO_MAC` also maps un-reflashed stock units by short address). |
| A second test PAN, 0x65FD, for our gateway on a v1 contact (#15) | The user, 2026-10-24: "pan加一个新的就是了" (just add a new PAN). Originally preset `fw-test2` (removed in `TASK-09`); now configured at runtime via `0x53F0` sub-ID `0x04` (`OWS_SUB_NETWORK`, `host_link.py --network`); 0x65FE stays the spare dongle's. |
| v1 contact: the ROM bootloader may be enabled in its CCFG, once, over JTAG | The user, 2026-10-24 (#14): BL_CONFIG with the W pad (DIO10, `0xC5FE0AC5`) as backdoor (first the button's DIO11, which has no pad; the user chose W the same day), so a unit can be flashed over its UART pads (and by an ESP32/ESP8266). Done on the bench with a verified dual-read backup first (`tools/uniflash.py provision --sensor|--gateway --backup <dir>` or `enable-bootloader w --backup <dir> --yes`); the MAC and the rest of the CCFG are kept; not a run-time sensor write. |
| Power loss may lose NV data; it must never stop a boot | The user, 2026-10-24 (#38): no second NV page or backup copy. Whatever an interrupted write leaves in the NV page (a torn record, garbage), the firmware starts: at worst the data is treated as lost (a sensor unpaired, a gateway with an empty table) and the page is formatted again. Tested by simulated power cuts at every byte of a write and by random pages. |
| The sensor's message counter lives in RAM only | The user, 2026-10-24 (#38): it starts from 0 at every start, and an event writes no flash; the NV join record (item `0x20`) is written only when the sensor joins (and optional DIY profile item `0x21` `NV_CFG` only when explicitly configured). Nothing depends on the counter surviving a reset (the stock dongle, our gateway's dedup and wyzesense2mqtt-rs do not check it). |
| Our sensor firmware never writes the CCFG while it runs | The user, 2026-10-24: a Wyze sensor keeps its MAC in its CCFG (`0x1FFCC`); our firmware reads it there and leaves page 31 alone (`test_sensor_images_never_write_the_ccfg`). Only the gateway's host-armed CCFG restore (`0x53F0` sub-ID `0x03`, `OWS_SUB_CCFG`) writes Page 31. The root cause of the stock Zero-MAC bug is documented in [`docs/DESIGN.md` §3.4](DESIGN.md#34-definitive-root-cause-of-the-stock-v1-zero-mac-bug) and [`docs/known-issues.md` §1](known-issues.md). |
| Gateway flags are not persistent | A power cycle resets them; the host re-applies them. |
| A5 XXTEA key: left open | Probably Wyze's check that a sensor is genuine; not needed between our own devices. Revisit only if a stock dongle rejects a sensor with a wrong A5. |
| License | MIT. Repository private for now. |
| Development hardware | TI LAUNCHXL-CC1310 (the dongle and the v1 motion sensor are CC1310s). |
