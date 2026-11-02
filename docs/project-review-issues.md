# Project Review: Code, Architecture, and Usability / UX Findings

This document records the technical, architectural, testing, documentation, and user-experience (UX/DX) issues found
during a full review of the **OpenWyzeSense** repository, paired with relevant findings from the reverse-engineering
of the stock firmware ([`docs/stock-re/`](stock-re/README.md)).

---

## Part I — Firmware, SoC, Relay, Tooling & Test Suite Issues

### 1. Critical & High-Severity Issues

#### 1.1 Host Build Failure on GCC 15 (`-Werror=unterminated-string-initialization`)
- **Locations**: [`firmware/tests/test_sensor.c`](../firmware/tests/test_sensor.c#L134) (lines 134, 290, 344) and
  [`firmware/common/sensor.h`](../firmware/common/sensor.h#L50)
- **Details**: `test_sensor.c` initializes the 8-byte `uint8_t mac[8]` field of `sensor_config_t` using 8-character
  string literals (`"77BD514D"`, `"77FFFF01"`, `"77FFFF02"`). Because `firmware/CMakeLists.txt` compiles with
  `-Wall -Wextra -Werror`, GCC 15 fails to build `test_sensor` with:
  ```text
  error: initializer-string for array of 'unsigned char' truncates NUL terminator but destination lacks 'nonstring' attribute (9 chars into 8 available) [-Werror=unterminated-string-initialization]
  ```
- **Fix**: Add `__attribute__((nonstring))` to `uint8_t mac[8]` in `firmware/common/sensor.h` (or initialize with an
  explicit 8-byte array macro).

#### 1.2 Stack Buffer Overflow in Sniffer App (`print_frame`)
- **Location**: [`firmware/apps/sniffer/sniffer_app.c`](../firmware/apps/sniffer/sniffer_app.c#L26-L38)
- **Details**:
  ```c
  char line[16 + 2 * RADIO_MAX_FRAME + 2];
  int n = snprintf(line, 32, "RX %d %lu ", f->rssi, (unsigned long)f->timestamp);
  ```
  `line` is 272 bytes (`16 + 2 * 127 + 2`), sized before the 32-bit `timestamp` was added to the prefix. However,
  `"RX %d %lu "` can be up to **19 bytes** (e.g. `"RX -100 4294967295 "`). When a maximum-length frame (`f->len == 127`)
  is received, the hex loop writes `254` bytes plus `"\n\0"` (`19 + 254 + 2 = 275` bytes), overflowing `line` by **3
  bytes on the stack** (and up to 18 bytes if `snprintf` ever returns up to 31).
- **Fix**: Size `line` as `char line[32 + 2 * RADIO_MAX_FRAME + 2];`.

#### 1.3 Uninitialized Stack Reads & Flash Writes on Short Encrypted RF Frames
- **Locations**:
  - [`firmware/common/sensor.c`](../firmware/common/sensor.c#L488-L555) (`on_message`, `sensor_rx`)
  - [`firmware/common/gateway.c`](../firmware/common/gateway.c#L632-L634) (`gateway_rx`, `case APP_JOIN_RESPONSE`)
- **Details**:
  1. **Sensor (`sensor.c`)**: `sensor_rx()` declares an uninitialized `uint8_t pt[APP_MAX_PAYLOAD]` (112 bytes) on the
     stack, decrypts `m.payload_len` bytes (which can be as small as 16 bytes), and calls `on_message(s, pt, now)`
     **without passing `m.payload_len`**:
     - `APP_JOIN_TOKEN` (`0xA4`, 16 bytes) reads `pt[16]` (uninitialized stack byte) for the `A6_OURS_MAGIC` (`0x4F`)
       check and `pt[17..18]` into `s->gw_features`.
     - `APP_OTA_OFFER` (`0xC0`, requires 25 bytes, i.e. 32 encrypted bytes) reads `pt[1..24]` without checking
       `m.payload_len >= 32`.
     - `APP_OTA_BLOCK` (`0xC2`) trusts `uint8_t n = pt[5]` (`<= 96`) without checking `6 + n <= m.payload_len`; a
       16-byte `0xC2` frame with `pt[5] = 96` causes `ota_block()` (`sensor.c:387-392`) to **write 86 bytes of
       uninitialized stack into the OTA flash slot**.
  2. **Gateway (`gateway.c:632-634`)**: `case APP_JOIN_RESPONSE:` (`0xA5`) is the only encrypted message handler in
     `gateway_rx()` that omits a length check (`if (n < 27) break;`). A 16-byte `0xA5` frame causes
     `to_host(gw, HOST_GET_R1 + 1, pt + 1, 26)` to send 11 bytes of uninitialized gateway stack (`pt[16..26]`) to the
     host.

#### 1.4 Sensor Permanently Wedges in `SENSOR_HANDSHAKE` (With Radio RX Locked On) if `0xB0` Arrives During Handshake
- **Locations**: [`firmware/common/sensor.c`](../firmware/common/sensor.c#L475-L486) and
  [`firmware/apps/sensor/sensor_app.c`](../firmware/apps/sensor/sensor_app.c#L262)
- **Details**:
  - In `on_message()` (`sensor.c:475-486`), `case APP_TIME_SYNC:` (`0xB0`) does not check
    `if (s->state != SENSOR_JOINED)` before zeroing `s->polls_left = 0` and calling `next_step(s, now)`.
  - If a `0xB0` arrives while `s->state == SENSOR_HANDSHAKE` (e.g. in response to a handshake `A1` status report poll),
    `s->polls_left` is cleared and `next_step()` sets `s->step = ACT_NONE` (`s->step_armed = false`), **leaving
    `s->state == SENSOR_HANDSHAKE` with no armed timer**:
    1. `sensor_listening()` (`sensor.c:583-586`) permanently returns `true`, keeping the CC1310 radio receiver on
       continuously (~5.5 mA) until the coin cell dies.
    2. `sensor_tick()` (`sensor.c:557-576`) never transitions out of `SENSOR_HANDSHAKE` (`s->step_armed` is `false`).
    3. `join()` in `sensor_app.c:262` checks
       `if (a->sensor.state == SENSOR_JOINING || a->sensor.state == SENSOR_HANDSHAKE) return;`, so **even holding the
       pairing button for 5 seconds cannot recover the sensor**.

#### 1.5 Gateway `gw->joining[]` Table Leaks Slots Permanently on Incomplete Joins
- **Location**: [`firmware/common/gateway.c`](../firmware/common/gateway.c#L616-L630)
- **Details**:
  - `gw->joining[]` (`GW_MAX_JOINING = 4`) marks an entry `used = true` when `APP_JOIN_ANNOUNCE` (`0xA3`) arrives
    (lines 620–628) and **only** clears `used = false` when the host sends `HOST_VERIFY_SENSOR` (`0x5323`, lines
    791–796).
  - It is never cleared by `associate()`, `HOST_SET_SCAN`, `HOST_DELETE_SENSOR`, `HOST_DELETE_ALL`, or a timeout.
  - If 4 join attempts ever fail or time out before `HOST_VERIFY_SENSOR`:
    1. All 4 `gw->joining[]` slots stay `used = true` until the gateway reboots.
    2. Those 4 short addresses are permanently treated as `joining == true`, so the gateway **never** sends them `0xB0`
       time syncs or OTA offers (`ota_for()` requires `!joining`).
    3. `short_of()` searches `gw->joining[]` *before* `gw->sensors[]`, returning stale short addresses if a sensor
       later re-associates on a different short address.
    4. Any 5th joining sensor cannot be recorded in `gw->joining[]`, so `joining` is `false` during its handshake and
       the gateway sends it `0xB0` in reply to its handshake `A1`/`E8` poll — triggering **Issue 1.4** above!

#### 1.6 `gateway-contact-v1` Built-In CCFG Fails `ccfg_is_safe()`, Disables ROM SBL, and Leaves `BOARD_LOW_POWER=1` Active
- **Locations**:
  - [`firmware/products.cmake`](../firmware/products.cmake#L10) (`gateway-contact-v1`)
  - [`firmware/boards/wyze_contact_v1/ccfg.c`](../firmware/boards/wyze_contact_v1/ccfg.c#L5-L7)
  - [`firmware/boards/wyze_contact_v1/board_config.h`](../firmware/boards/wyze_contact_v1/board_config.h#L16)
  - [`firmware/soc/cc13x0/board.c`](../firmware/soc/cc13x0/board.c#L96-L152)
- **Details**:
  1. `gateway-contact-v1` compiles the `gateway` app on board `wyze_contact_v1`. That board's `ccfg.c` sets
     `SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE = 0x00` (and `BL_ENABLE = 0x00`), which gets compiled into
     `ows_ccfg_builtin`:
     - Sending `HOST_OWS_CCFG` (`0x53F2`) to `gateway-contact-v1` always fails with `GW_CCFG_NOT_SAFE` because
       `ccfg_is_safe()` rejects `BL_CONFIG` with the ROM bootloader disabled.
     - Flashing `gateway-contact-v1.hex`/`.bin` (full image) onto a contact board where `tools/uniflash.py
       enable-bootloader` was previously run overwrites page 31 and **disables the ROM bootloader**, breaking future
       ESP32 UART reflashing (`#22`).
     - `test_the_built_in_ccfg_copy_is_the_image_ccfg()` (`reference/tests/test_firmware.py:183-189`) only tests
       `('gateway-dongle', 'gateway-launchpad')`, missing `gateway-contact-v1`.
  2. `wyze_contact_v1/board_config.h` defines `#define BOARD_LOW_POWER 1`. When `gateway-contact-v1` is built with
     preset `fw`, `BOARD_LOW_POWER` is `1` and `gateway_app.c` never calls `board_keep_awake()`. After `DEBUG_WINDOW_MS`
     (60 s), `window_over()` in `board.c:96-102` clears `PRCM_DOMAIN_SERIAL` and releases `PowerCC26XX_SB_DISALLOW`!

#### 1.7 `queue_report()` Evicts the In-Flight `s->queued[0]` Report Before Its ACK Arrives
- **Location**: [`firmware/common/sensor.c`](../firmware/common/sensor.c#L284-L293) and
  [`on_ack()`](../firmware/common/sensor.c#L436-L445)
- **Details**:
  - `s->queued[0]` remains in `s->queued[]` while it is in flight (`s->step == ACT_TX_REPORT`) and is only popped
    inside `on_ack()` when the MAC ACK arrives.
  - However, if `s->n_queued == SENSOR_MAX_QUEUED` (4) when another report is queued via `queue_report()`,
    `memmove(s->queued, s->queued + 1, --s->n_queued)` shifts `s->queued[0]` out immediately — overwriting the
    currently transmitting report (e.g. `APP_STATUS` / `0xA1`) with `s->queued[1]` (`APP_EVENT` / `0xA2`).
  - When the MAC ACK for the in-flight `0xA1` arrives milliseconds later, `on_ack()` pops `s->queued[0]` *again*,
    **silently dropping an unsent `0xA2` event** and misidentifying `done` as `APP_EVENT` instead of `APP_STATUS`
    (skipping the post-`A1` `DATA_REQ` polls for `0xB0` / `0xC0`!).
  - Relatedly, `s->state_byte` is a single scalar overwritten by `sensor_event()` (`sensor.c:317`), so rapid
    open-then-close transitions before the first `0xA2` goes on air send two `0xA2` frames with the same final state
    instead of the transition sequence.

---

### 2. Medium-Severity Firmware, SoC & Host-Link Issues

#### 2.1 `sensor_join()` Does Not Reset Active OTA or Check-In State
- **Location**: [`firmware/common/sensor.c`](../firmware/common/sensor.c#L295-L308)
- **Details**: If a user holds the button to re-pair (`sensor_join()`) while an OTA download is in progress
  (`s->ota.active == true`) or `s->check_in == true`, `sensor_join()` does not clear `s->ota`, `s->awaiting_frame`, or
  `s->check_in`. During the new join's handshake, `poll_interval()` uses `OTA_POLL_MS` (50 ms) instead of
  `POLL_INTERVAL_MS` (300 ms), and as soon as `0xA4` completes the join, `next_step()` immediately resumes sending
  `0xC1` (`APP_OTA_REQUEST`) for the old OTA session.

#### 2.2 Sensor Console `watch` Command Sleeps Until Next Heartbeat Instead of Polling Every 20 ms
- **Location**: [`firmware/apps/sensor/sensor_app.c`](../firmware/apps/sensor/sensor_app.c#L499-L552)
- **Details**: In `sensor_app_step()`, the `a->watch_until` check (lines 499–513) and the
  `if (a->watch_until && input_wait > 20) input_wait = 20;` clamp (lines 518–520) execute **before** `uart_getc()` /
  `command()` (lines 529–535), and the sleep calculation at lines 537–550 ignores `a->watch_until` when
  `input_wait == 0`. Typing `watch 10` on an idle sensor prints the initial `pins ...` line and then sleeps for up to
  4 hours instead of polling every 20 ms.

#### 2.3 OTA Image Confirmation Can Be Delayed Until Next Heartbeat If First ACK Arrives After 60 s
- **Location**: [`firmware/apps/sensor/sensor_app.c`](../firmware/apps/sensor/sensor_app.c#L475-L498)
- **Details**: `sensor_app_step()` checks `if (!a->confirmed && a->now_ms >= CONFIRM_MS && ...)` at line 475 **before**
  `radio_poll(a)` (line 495) processes incoming MAC ACKs and increments `a->sensor.acks`. If the first ACK arrives
  after `t = 60 s`, the sensor sleeps until the next heartbeat (up to 4 hours) before confirming the slot image.

#### 2.4 `restore()` in `sensor_app.c` Bypasses `set_type()` and Ignores Build-Time `OWS_PAN`
- **Location**: [`firmware/apps/sensor/sensor_app.c`](../firmware/apps/sensor/sensor_app.c#L101-L108)
- **Details**:
  1. `restore()` assigns `a->sensor.cfg.type = r[0];` directly instead of calling `set_type(&a->sensor, r[0])`, leaving
     `cfg.fw_version` and `cfg.a3_extra` at the compile-time defaults.
  2. `restore()` does not check `a->sensor.cfg.only_pan` (`OWS_PAN`), so a flash NV page holding a join on PAN `X` will
     be restored and used even by a build compiled with `-DOWS_PAN=Y`.

#### 2.5 `sensor_rx()` Does Not Disarm `awaiting_ack` When Expected Downlink Frame Arrives During a Retry
- **Location**: [`firmware/common/sensor.c`](../firmware/common/sensor.c#L522-L555)
- **Details**: `ACK_TIMEOUT_MS` is 250 ms, while a `DATA_REQ` (~40 ms) + gateway ACK (~32 ms) + 127-byte `C2` OTA
  block (~210 ms) takes ~282 ms. If a `DATA_REQ` retry fires at 250 ms just before the gateway's downlink frame
  finishes arriving, `sensor_rx()` accepts the downlink frame and advances `s->step`, but leaves
  `s->awaiting_ack = true`, causing a spurious `on_ack()` or `on_ack_timeout()` on the new step.

#### 2.6 `mac_parse()` Accepts Reserved IEEE 802.15.4 Addressing Mode `1` (`0b01`)
- **Location**: [`firmware/common/mac154.c`](../firmware/common/mac154.c#L54-L88)
- **Details**: `read_addr()` handles `mode == MAC_ADDR_NONE (0)`, `MAC_ADDR_SHORT (2)`, and `MAC_ADDR_EXT (3)`, but
  does not reject reserved mode `1` (`0b01`), consuming 2 PAN ID bytes and 0 address bytes.

#### 2.7 Host Protocol Framing & Queueing Edge Cases (`host_proto.c`, `gateway.c`)
- **Locations**: [`firmware/common/host_proto.c`](../firmware/common/host_proto.c#L26-L98) and
  [`firmware/common/gateway.c`](../firmware/common/gateway.c#L823-L828)
- **Details**:
  1. `host_parse()` does not validate `p[2] == HOST_SYNC (0x43) || p[2] == HOST_ASYNC (0x53)`.
  2. `host_build()` does not assert `n <= HOST_MAX_PACKET - 7` (`252`).
  3. `HOST_GET_SENSOR_LIST` (`0x5330`, `gateway.c:823-828`) sends up to 64 async `0x5331` packets in a single loop
     while `gw->unacked[]` only holds `GW_MAX_UNACKED = 4` packets.

#### 2.8 Short Address Allocation & Network PAN Validation in Gateway
- **Locations**: [`firmware/common/gateway.c`](../firmware/common/gateway.c#L418-L429) and
  [`firmware/apps/gateway/gateway_app.c`](../firmware/apps/gateway/gateway_app.c#L70-L74)
- **Details**: `associate()` does not check if `max + 1 == gw->short_addr` or `max >= 0xFFFD`, and `gateway_app.c`
  does not exclude `pan == 0x0000` or `addr in (0x0000, 0xFFFE, 0xFFFF)` (which `nvows.py` rejects).

#### 2.9 `nvstore.c`: Partial Flash Program Failure Leaves `nv->end` Stale
- **Location**: [`firmware/common/nvstore.c`](../firmware/common/nvstore.c#L161-L185)
- **Details**: If `nv->program()` succeeds for the 12-byte header `h` at `nv->end` and fails on `data`, `append()`
  returns `false` without advancing `nv->end`, causing subsequent writes to program over non-`0xFF` bytes.

#### 2.10 CC13x0 SoC Driver & Linker Script Findings
- **Locations**:
  - [`firmware/soc/cc13x0/radio.c`](../firmware/soc/cc13x0/radio.c#L149-L175): `radio_tx()` advances `last_start`
    relative to the target start time rather than actual TX time on `gap_us > 0`, and does not call `open_rf()` if
    `radio_rx_start()` was not called first.
  - [`firmware/soc/cc13x0/clock.c`](../firmware/soc/cc13x0/clock.c#L19-L27): `clock_now_ms()` mutates static state
    without an interrupt guard.
  - [`firmware/chips/cc1310f128/cc1310f128_boot.ld`](../firmware/chips/cc1310f128/cc1310f128_boot.ld): `selector_reset`
    does not initialize `.data` or `.bss` and lacks a linker `ASSERT(SIZEOF(.data) + SIZEOF(.bss) == 0, ...)`.

---

### 3. ESP32 Relay & Host Simulation Issues (`esp32/`)

1. **`esp32/relay/relay.c:168-185`**: `strstr((char *)r->rx, " 101 ")` searches the entire HTTP response header block
   rather than the first status line (`HTTP/1.1 101 ...`).
2. **`esp32/relay/relay.c:194-216`**: Accepts WebSocket `OP_PING` frames with payload `n > 125` and echoes invalid
   pongs (`> 125` bytes), violating RFC 6455 §5.5.
3. **`esp32/host/ows_relay.c:30-49`**: `tcp_connect()` calls blocking `connect()` before setting `O_NONBLOCK`, stalling
   the UART/TCP poll loop for the OS SYN timeout when the server is unreachable.

---

### 4. Python Tools & Reference Library Issues (`tools/`, `reference/wyzesense_ref/`)

1. **`tools/decode_stream.py` & `reference/wyzesense_ref/{iq.py,frame.py}`**: `iq.decode_iq()` calls
   `frame.parse_mac()` before checking `crc_ok`; corrupted short bursts raise `IndexError` in `u16()` or `KeyError: 1`
   in `decode_stream.py:46-48` when `dst_mode` or `src_mode` is `1`.
2. **`reference/wyzesense_ref/nvows.py`**: `latest()` ignores `live=False` (`DELETED`) records instead of popping the
   deleted key; `import_()` does not validate address ranges, duplicate MACs/shorts, or the 111-sensor page limit.
3. **`reference/wyzesense_ref/gateway.py` & `host.py`**: `_short_of()` calls `mac.decode()` without `errors='replace'`;
   `_associate()` does not clear `self.types.pop(short, None)` or check for shared short addresses; `host.py` is
   missing `OWS_CCFG = 0x53F2` and `OWS_BUILD_INFO = 0x53FA`.
4. **`tools/host_link.py:264, 287`**: `--pair` hardcodes `b'\xff\x04'` in `VERIFY_SENSOR`, and combining `--ota` with
   `--pair` returns early at line 264 before `--pair` runs.

---

### 5. Test Suite Bugs & Coverage Gaps (`firmware/tests/`, `reference/tests/`)

1. **Outdated NV Corruption Tests (`test_apps.c:1130`, `test_nvstore.c:274`)**: Still craft corrupt NV pages using the
   pre-#61 TI NVOCTP format (`0x00, 0x00, 0x10, 0x00` header) instead of `OWNV\x01\xff\xff\xff`, so `nv_open()`
   immediately formats the page at byte 0 without testing `next()` on torn records.
2. **Outdated Counter Reset Assertion (`reference/tests/test_hw_sensor.py:214-237`)**: `survives_a_reset()` asserts
   that the message counter persists across a reset, which was removed in #38 (`counter` resets to `0` in RAM).
3. **Weak Frame-Count Check (`firmware/tests/test_gateway.c:114-120`)**: `feed_rf()` checks `n_expected > n_tx` instead
   of `n_tx != n_expected`.
4. **One-Way Checksum Check (`reference/tests/test_ground_truth.py:29-33`)**: Checks that entries in `SHA256SUMS` match
   disk, but not that all files in `ground_truth/` are listed in `SHA256SUMS`.

---

## Part II — Usability, Product Design & User/Developer Experience (UX/DX) Issues

### 1. Physical Device UX (Button Presses & LED Feedback)

#### 1.1 No Visual Feedback While Holding the Button to Pair (Contrast with Stock Firmware)
- **Location**: [`firmware/apps/sensor/sensor_app.c`](../firmware/apps/sensor/sensor_app.c#L397-L448)
- **Issue**: `sensor_app_step()` only checks the elapsed button hold duration **when the button is released**
  (`!button && a->button_down`). Nothing blinks when the hold threshold (`5,000 ms`) is reached. If a user releases at
  `4.8 s`, it silently triggers a short press instead of pairing.
- **Stock Firmware Comparison ([`docs/stock-re/stock_sensor.c`](stock_sensor.c))**: Stock `Ssf_buttonClockCb()` counts
  10 ms ticks *while the button is held* and **immediately blinks the LED 3 times (`400 ms`) at `3.04 s`** (`300` ticks
  after `40 ms` debounce) so the user knows they can release the button.

#### 1.2 Indistinguishable Single 50 ms LED Blink for Every Event
- **Location**: [`firmware/apps/sensor/sensor_app.c`](../firmware/apps/sensor/sensor_app.c#L17-L18) (`BLINK_MS = 50`)
- **Issue**: Boot, button press, and join completion all emit the exact same single 50 ms pulse, and join failure, low
  battery, and OTA events emit no blink at all.
- **Stock Firmware Comparison ([`docs/stock-re/stock_sensor.c`](stock_sensor.c))**: Stock uses distinct patterns via
  `Ssf_ledBlink(half_period_ms, count)`:
  - Normal boot: `250 ms × 1`
  - Low-battery / unprovisioned error: `200 ms × 5`
  - Button held `3.04 s` / Join succeeded: `400 ms × 3`
  - Join failed (all scan cycles exhausted): `100 ms × 3`

#### 1.3 Gateway LED Is Permanently Solid On With No Scan/Activity Indication
- **Location**: [`firmware/apps/gateway/gateway_app.c`](../firmware/apps/gateway/gateway_app.c#L165)
- **Issue**: `gateway_app_init()` turns LED 0 on once and never updates it during scan/inclusion mode, OTA transfers,
  or host disconnection.

---

### 2. Flashing & Bring-Up Workflow UX

1. **Missing `tools/contact_station.py` Referenced in [`docs/user-guide.md`](../docs/user-guide.md#L70-L88)**:
   Section 3 of the user guide instructs users to run `uv run python tools/contact_station.py`, which does not exist in
   the repository.
2. **Inconsistent Safety Guards Across Flashing Tools (`uniflash.py`, `uart_sbl.py`, `dongle_sbl.py`)**:
   - `dongle_sbl.py` requires `--backup DIR` and `--yes`, and defaults to `--nv migrate`.
   - `uniflash.py` defaults to `--nv keep` and overwrites flash on `flash` / `slots` without requiring `--backup` or
     `--yes`.
3. **2-Minute Unconditional Wait in `dongle_sbl.py` (`finish()` at lines 424–430)**:
   - Waits 120 seconds polling for the dongle to exit update mode on its own before telling the user to unplug and
     replug it. Prompting the user immediately (*"Unplug and replug the dongle now"*) while polling allows instant
     completion as soon as the user replugs it.
4. **Hardcoded `/dev/hidraw6` and `/dev/ttyACM0` Defaults & Missing Dongle udev Rule**:
   - `dongle_sbl.py` and `host_link.py` hardcode `/dev/hidraw6` and `/dev/ttyACM0` instead of reusing the USB VID:PID
     auto-detection (`1a86:e024` and `0451:bef3`) already in `reference/tests/hw.py`.
   - `tools/udev/` has a rule for the TI LaunchPad (`0451:bef3`), but no rule for the Wyze USB dongle (`1a86:e024`).

---

### 3. OTA Update & Feature Flag UX

1. **`slot_image.py` / `boot_slot.c` Do Not Verify Target Slot Address Before Booting**:
   - `slot_header_t` (`OWSI`) does not record which slot (`0` = `0x01000` or `1` = `0x0F000`) the image was linked for,
     and `boot_written_ok()` (`firmware/common/boot_slot.c:114-125`) does not check that the image's reset vector
     (`base + 0x100 + 4`) points within `[base, base + SLOT_SIZE)`. Passing `slot-b.img` before `slot-a.img` to
     `host_link.py --ota` writes a slot-B-linked binary into slot A with a valid CRC.
2. **`host_link.py --ota` Usability Traps**:
   - Requires stopping `wyzesense2mqtt-rs` to claim the serial/HID port.
   - Defaults `--seconds` to `60` (line 235), while an OTA update takes ~195 seconds — causing `host_link.py --ota` to
     time out unless `--seconds 600` is explicitly passed.
   - Calls `link.flags(0)` on exit (line 263), wiping any other active gateway feature flags (`DEDUP`, `FIX_ZERO_MAC`,
     `ANNOUNCE`).
3. **Gateway Feature Flags (`0x53F0`) Cleared on Every `HOST_INQUIRY` (`0x4327`)**: *(no longer true, checked
   2026-11-01: the `0x4327` handler only answers; flags and the address range reset only when the gateway restarts,
   and on a dongle the CH554 answers `0x4327` itself.)*
   - Because `gateway.c:752` resets `gw->flags = 0` on `HOST_INQUIRY`, any flags set via CLI are wiped whenever
     `wyzesense2mqtt-rs` starts or reconnects.

---

### 4. CI Artifact Naming & Completeness (`.github/workflows/ci.yml`)

- In [`.github/workflows/ci.yml`](../.github/workflows/ci.yml#L49-L56), the uploaded artifact is named
  `firmware-receive-only` even though it uploads `build/fw/firmware/` (the transmitting preset), and its file globs
  (`*.elf`, `*.hex`, `*.bin`) omit `*.img` (`sensor-*-slot-a.img`, `sensor-*-slot-b.img`), which are required for
  flashing and OTA-updating sensors.
