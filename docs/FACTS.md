# Wyze Sense RF protocol: facts, hypotheses, open questions

This is the single source of protocol knowledge for this project. Every statement has a status:

- **Verified**: reproducible from `ground_truth/` (raw RF captures, bridge logs, firmware dumps). The
  *Check* column names the test in `reference/tests/` that re-derives it (`uv run pytest`), or the exact
  file and offset to look at.
- **Hypothesis**: consistent with all data so far but not proven. Do not build on it without confirming.
- **Open**: not understood.

Rule for adding to this file: a new **verified** row needs evidence in `ground_truth/` and, where possible,
a test. Anything else goes in the hypothesis or open tables.

Times are local (PDT, UTC−7). "The bridge" is the user's home bridge, a host program that talks to a stock
Wyze dongle over USB and reports each sensor's decoded state; its logs in `ground_truth/rf/*/bridge_events.jsonl`
are an independent ground truth for what the stock system understood.

## 1. Physical layer

| Fact | Status | Evidence / check |
|---|---|---|
| All Wyze traffic (v1 and v2 sensors, dongle) is on **906.8 MHz**, one channel | verified | every frame in every session; band survey 902–928 MHz, 13 slices × 150 s (`rf/2026-10-21_2155_band_survey_902-928M`): bridge events matched only at 906.8 MHz |
| Modem: TI SimpleLink Long Range, "Mode 2" (TI app note SWRA642) | verified | the radio setup below; the frame structure below decodes every burst |
| 2-GFSK, 20 ksym/s, deviation ±5 kHz | verified | gateway RAM 0x2648 `CMD_PROP_RADIO_DIV_SETUP` (0x3807): GFSK, deviation 20 × 250 Hz, preScale 15 / rateWord 0x3333 (24 MHz × 0x3333 / (15 × 2²⁰) = 20,000 sym/s); `test_gateway_radio_setup`. Measured deviation ±5 kHz. |
| DSSS = 2: each coded bit is sent as 2 equal symbols, so 1 coded bit = 1 chip of 100 µs (10 kchip/s) | verified | override `0x0100505C` (RAM 0x2774), TI's documented DSSS = 2 setting; measured run lengths cluster at 100 / 200 µs |
| The radio core runs an MCE patch from RAM (override `0x00001107`, RAM 0x2728); the same 1024-byte patch block is at flash 0x2EC in all four images (3 gateway dumps, v1 motion) | verified | `test_same_radio_patch_in_every_image` |
| That patch is TI's `rf_patch_mce_sl_longrange` (all 256 words); flash 0x6EC holds TI's `rf_patch_rfe_sl_longrange` (identical to TI's genfsk RFE patch). Neither SDK CPE patch image (`rf_patch_cpe_*`, 247 words) is in any stock image: the stock firmware uses another CPE patch or none | verified against `third_party/ti_coresdk` | `test_rf_patches_are_the_stock_ones` (on our built image) |
| The stock radio commands: CMD_PROP_RADIO_DIV_SETUP (RAM 0x2648), CMD_FS 906 MHz + 0xCCCC/65536 (0x25D8), CMD_PROP_RX_ADV (0x2670: CRC check on, CRC-error frames auto-flushed, PHR header 16 bits / 11-bit length, lenOffset −4, header + RSSI + timestamp stored), CMD_PROP_TX_ADV (0x2628: 16 header bits, radio appends the CRC), sync word parameter 0x5555904E, 24 overrides (0x2728). Our firmware uses exactly these | verified | `test_radio_settings_are_the_stock_ones`, `test_tx_build_has_the_stock_tx_command` |
| The stock gateway sends behind a **carrier sense chained to its transmit command**: CMD_PROP_CS (RAM 0x25F0, pNextOp = the CMD_PROP_TX_ADV at 0x2628, condition STOP_ON_TRUE: a busy channel ends the chain without the frame), RSSI only, threshold −83 dBm, ends on the first idle/busy decision, 2 ms timeout (counted busy); last status 0x3407 (idle) with the transmit after it DONE_OK. A second, slotted one (0x260C, ABSTIME start) is chained to it. Field for field TI's 15.4-Stack `mac_settings.c` `RF_cmdPropCs` / `RF_cmdPropCsSlotted`, as are the other stock commands (the stock firmware is TI's 15.4 MAC). Whether stock senses before an ACK too is not known. Our firmware chains the same command before every frame but ACKs (#85) | verified (structures; ACK case unknown) | `test_radio_settings_are_the_stock_ones` (stock `RAM.bin`); TI `simplelink-lowpower-f2-sdk` `source/ti/ti154stack/low_level/cc13xx/subg/mac_settings.c` |
| With those settings (formatConf.bMsbFirst = 1, which the MSB-first PHR needs) the CC1310 stores every received PSDU byte bit-reversed, so the stock firmware must reverse each byte in software (and before sending) | verified on hardware: LaunchPad vs SDR, 3 data frames + 3 ACKs | `test_launchpad_with_stock_settings_stores_bytes_bit_reversed` (`rf/2026-10-22_2236_*_launchpad`) |
| With those settings the radio sends the 16-bit header (the PHR) low byte first: the TX buffer holds the PHR little-endian | verified on hardware (sent big-endian, the PHR came out byte-swapped) | `test_our_gateway_transmits_like_the_stock_dongle` (`rf/2026-10-23_0824_*_launchpad_tx`) |
| With those settings a LaunchPad hears the house traffic as the house dongle does: in 30 min of morning traffic it received the report behind every one of the bridge's 14 state changes (9 sensors), the contents agreeing; its RSSI was within ~2 dB of what the dongle reported | verified | `test_launchpad_hears_every_report_the_house_dongle_reported` (`rf/2026-10-23_0720_launchpad_bridge`) |
| Sync: 64-symbol word `0xCCC3C3CCC33C3333` (LSB first) sent 3 times, the first 2 copies inverted. After DSSS collapse: 96 chips `aa695995 aa695995 5596a66a` (one FM polarity) | verified | `phy.HEADER`; `test_phy_decode_real_bursts` |
| FEC: convolutional, rate ½, K = 7, generators 171 / 133 octal (171 output first), encoder starts in the zero state, ~10 zero tail bits; the coded stream is inverted relative to the sync polarity | verified bit-exact | `test_phy_encode_matches_air_bit_exact` re-encodes captured beacons chip for chip. (Equivalent view: invert input, start state all-ones.) |
| `formatConf` 0xE0A0: 32-bit sync, MSB first, whitenMode 7 (802.15.4g whitening + dynamic CRC); `preamConf.nPreamBytes` = 2 | verified (binary) | `test_gateway_radio_setup` |

Capture recipe that works: `rtl_433 -f 906.7M -s 250k -g 35 -w stream.cu8` (raw stream tuned 100 kHz below
the channel to avoid the DC spike), decoded with `wyzesense_ref.iq.decode_file(path, 250e3, 906.7e6)`.
Auto gain saturates when a device is close.

## 2. Framing

| Fact | Status | Evidence / check |
|---|---|---|
| PHR: IEEE 802.15.4g, 16 bits MSB first: MS (1), reserved (2), FCS type (1; 0 = 4-byte), DW (1; whitening), length (11). Every frame uses `0x08LL`: 4-byte FCS, whitening on, LL = PSDU length incl. FCS | verified | every decoded frame; `frame.parse` |
| Whitening: PSDU only, PN9 x⁹+x⁵+1, seed 0x1FF, the whitening bit is the feedback bit | verified | every data/command/beacon frame passes CRC only with this; `frame.pn9_bits` |
| PSDU bytes are sent LSB first | verified | as above |
| FCS: CRC-32 (IEEE/zlib) over the MAC frame, little-endian | verified for data, command and beacon frames | `test_crc32_on_captured_frames`; identified first with a whitening-independent test (FCS₁ ⊕ FCS₂ = CRC(data₁ ⊕ data₂)) |
| ACK frames (PHR 0x0807): the 4-byte FCS is CRC-32 over FC, seq **and one zero byte** (`crc32(ack + 00)`, little-endian), not over the 3 bytes the PHR counts. Same rule in the dongles' and the sensors' ACKs. It is the CC1310's own CRC engine: with the stock settings it accepts such ACKs (it drops CRC failures, yet delivered the house ACKs) and generates them (given the 3-byte ACK, it sent this FCS) | verified, 289 of 289 captured ACKs (130 distinct); on hardware: `test_launchpad_with_stock_settings_stores_bytes_bit_reversed`, `test_our_gateway_transmits_like_the_stock_dongle` | `test_ack_fcs`; `frame.ack_fcs`. Found by showing the trailer is affine in FC + seq, then matching its linear part to CRC-32 over a 4-byte message. Likely the stock TX buffer is padded to 4 bytes. (`decoded_frames.jsonl` checks the standard FCS, so it marks ACKs `crc_ok: false`.) |

## 3. MAC layer: IEEE 802.15.4, non-beacon star network

| Fact | Status | Evidence / check |
|---|---|---|
| MAC security off (FC bit 3 = 0); the payload is encrypted at the application layer instead | verified | all frames |
| Dongle = PAN coordinator; sensors use short addresses with PAN-ID compression | verified | all frames |
| Sensor → dongle data: FC 0x8861 (data, ack request, PAN-ID compression, short addresses) | verified | `test_mac_parse_and_build` |
| ACK: FC 0x0002, or 0x0012 (frame pending); seq echoes the acknowledged frame | verified | |
| Polling: sensor sends a data request (cmd 0x04, FC 0x8863); the dongle ACKs with frame-pending, then sends a data frame; the sensor ACKs it | verified | e.g. `rf/2026-10-21_2227_*` |
| A data request from a sensor of type 0x01, 0x02, 0x03 or 0x07 (v1 contact/motion, leak, climate) is always answered with pending + a B0, unless join frames are queued; a data request from type 0x0E / 0x0F (v2 contact / motion) never gets one (they poll ~10 times at 0.7 s and get plain ACKs). Data frames are always ACKed without pending | verified (5 stream sessions, every data request) | `test_replay_matches_stock_dongle`; `gateway.NO_TIME_SYNC` |
| The dongle has one sequence number (DSN) for every data and command frame it sends (B0, association response, A4, A6), +1 per frame, and a separate one for beacons | verified | `test_replay_matches_stock_dongle` (one DSN gap in 2303, a frame the SDR missed) |
| Model: `wyzesense_ref.gateway` reproduces, byte for byte, every frame the house dongle sent in the five stream sessions (ACKs, beacons, B0, association responses, A4, A6) from the frames it received, given the dongle clock | verified | `test_replay_matches_stock_dongle` |
| Dongle ACK starts ≈ 89 ms after the start of a 45-byte sensor data frame (the frame itself lasts ≈ 87 ms) | verified (measured) | sample positions in `decoded_frames.jsonl` |
| Frame duration ≈ 15.4 ms + 1.6 ms per PSDU byte (5 kbit/s after FEC and DSSS). The dongle's ACK starts ~2 ms after the end of the frame it acknowledges: 39.4 ms (38.9–39.8, n = 77) after the start of a data request, 89.0 ms after a 45-byte report, 63.5 ms after a 29-byte one | verified (measured, the 250 kS/s stream sessions) | |
| The data frame announced by a pending ACK starts 33.5 ms (29–38, n = 34) after the start of that ACK (which lasts ~26.7 ms): the sensor gets ~7 ms to turn round. Our gateway waits the same (`POLL_REPLY_GAP_US`); sent at once, the frame follows the ACK within ~1 ms | stock: verified (measured); ours: `test_our_gateway_transmits_like_the_stock_dongle` (30.8–33.4 ms) | |
| Beacons are sent only in reply to beacon requests (cmd 0x07 to 0xFFFF). Superframe spec 0x4FFF normally: non-beacon, PAN coordinator, association permit **off** | verified | `rf/2026-10-21_2114_*`, `_2118_*` |
| While the user has pairing on, the dongle's beacons carry 0xCFFF (association permit **on**) | verified | `rf/2026-10-22_1821_*`, `_1823_*` |
| A stock gateway's beacon starts **38–156 ms** after the start of the beacon request (the request is ~30 ms on the air), varying per request: ~38–44 ms when the air is free, else about one or two beacon lengths (~40–45 ms each) later, as if it listened first. Our gateway answered after a fixed delay, so two of ours collided on every request and a sensor never saw the farther one's permit (#84); it now waits 5–20 ms after the request when pairing, 75–125 ms when not (`BEACON_*`, `gateway.c`). Whether a stock gateway's pairing state changes its delay is not known | verified (stock timing) | n = 81: 845B/3366, D712/A5FB, 65FE/D425 before our firmware, all `rf/*/decoded_frames.jsonl`; `test_stock_gateways_answer_beacon_requests_after_a_random_delay`; ours at a fixed ~30 ms and the collision: SDR 2026-10-31 (#84) |
| Several Wyze networks are in range and all use the same payload key (see §4) | verified | coordinators 0x3366 / PAN 0x845B (house), 0xD425 / PAN 0x65FE (user's second dongle), 0xA5FB / PAN 0xD712, 0xE261 / PAN 0x80B3 |

### 3.1 Join (pairing)

Captured three times: v2 contact 77BD514D at 18:21:41 and again at 18:26:09 (a re-pair; it was already
known to the dongle), and v2 climate 77C68193 at 18:28:15 (moved from the user's second dongle). The user
turned pairing on from the bridge and held each sensor's button. Every step was seen in all three joins.
IQ: `rf/2026-10-22_1821_*/snip_182141_059.cu8`, `rf/2026-10-22_1823_*/snip_182609_878.cu8`, `snip_182815_716.cu8`.

| # | Frame | Example | Status / check |
|---|---|---|---|
| 1 | beacon request, broadcast | `03 08 1c ff ff ff ff 07` | verified |
| 2 | beacon from each coordinator in range; the pairing one has superframe 0xCFFF | `00 80 15 5b 84 66 33 ff cf 00 00` | verified |
| 3 | association request, FC 0xC823 (cmd, ack req, dst short, src **extended**), src PAN 0xFFFF; the extended source address is the sensor's **ASCII MAC** (bytes `37 37 42 44 35 31 34 44` = "77BD514D"); capability 0x80 (allocate address) | `23 c8 1d 5b 84 66 33 ff ff 37 37 …` | verified; `test_association` |
| 4 | data request from the extended address ~1.05 s later | | verified |
| 5 | association response from the dongle's own 64-bit IEEE address (here 00124B001E9FF933, TI OUI): cmd 0x02, short address, status 0 | `… 02 02 00 00` | verified; `test_association` |
| 6 | encrypted **A3** sensor → dongle, **A4** dongle → sensor, **A5** sensor → dongle, **A6** dongle → sensor (§4.3) | | verified; `test_join_handshake`, `test_iq_join_clip_contains_the_join` |
| 7 | normal reports (A2 / A1 / E8) | | verified |
| 8 | 77BD514D then polled every ~0.7 s for > 8 s with nothing pending | | verified (one join) |

| Fact | Status | Evidence |
|---|---|---|
| A sensor already known to the dongle gets its old short address back | verified (1 sensor, 2 joins) | 77BD514D: 0x0002 both times |
| A sensor new to the dongle gets the next free address | verified once | 77C68193: 0x0012, after the highest in use (0x0011) |
| The bridge reports a join as an event with battery 100 and signal 0, then a placeholder with no reading, then the first real report | verified | `rf/2026-10-22_1823_*/bridge_events.jsonl` |

## 4. Application layer

| Fact | Status | Evidence / check |
|---|---|---|
| The MAC payload of data frames is **AES-128-ECB** with key `2b7e1516 28aed2a6 abf71588 09cf4f3c` (the FIPS-197 example key from TI's `CryptoCC26XX.h` example). Stored in `.data` SRAM (`contact_v1` `0x20001944`, `motion_v1` `0x20001988`, `gateway_v1.8` `0x200014E0`) and **never written or rotated at runtime** in either firmware | **verified** | key at gateway RAM `0x14E0` (`test_aes_key_in_gateway_ram`) and sensor `.cinit` (`0x153F8` -> `0x20001944`); `AES_encryptBlock16` (`contact_v1` `0x10994`, `motion_v1` `0x10BBC`, `gateway_v1.8` `0x127A0`) and `AES_decryptBlock16` (`0x10940`, `0x10B68`, `0x112A8`); every decoded sensor message decrypts to an ASCII MAC (`test_every_decoded_sensor_message_carries_a_mac`) |
| The same key on every network seen (4 coordinators) | verified | |
| Payloads are 16 or 32 bytes; v2 sensors and `contact_v1` (`0x0EA90`) zero-pad the plaintext; `motion_v1` `Sensor_sendChallengeRsp_A5` (`0x0F764`) omits `memset(sp, 0, 0x20)` so `0xA5` bytes `27..31` leak uninitialized stack words (`…0020` SRAM pointer pattern). In both sensor (`0x0C5F0` / `0x086BC`) and gateway (`0x0D5A4` / `0x0924C`), `AES_encryptBuffer` and `AES_decryptBuffer` omit the `else` after `if (len <= 16)` (`known-issues.md` §3) | **verified** | decrypted payloads; `motion_v1` `0x0F764`; `contact_v1` `0x0C5F0`, `0x086BC`; `gateway_v1.8` `0x0D5A4`, `0x0924C` ([`docs/stock-re/stock_common.h`](stock-re/stock_common.h)) |

### 4.1 Sensor → dongle reports: 0xA1, 0xA2, 0xE8

| Offset | Size | Field | Status / check |
|---|---|---|---|
| 0 | 1 | message type | verified |
| 1 | 8 | sensor MAC, ASCII hex (e.g. `77C68217`) | verified |
| 9 | 1 | sensor type | verified for 0x02 v1 motion, 0x03 v2 leak, 0x07 v2 climate, 0x0E v2 contact, 0x0F v2 motion (bridge types); 0x01 v1 contact (the v1 contact 777B42DF's A3, `rf/2026-10-24_1147_…`) |
| 10 | 1 | chip temperature, °C (`AONBatMonTemperatureGetDegC()`) | **verified** (`contact_v1` `0x12c88`, `motion_v1` `0x12f40`; [`docs/stock-re/stock_sensor.c`](stock-re/stock_sensor.c)) |
| 11 | 1 | battery in `1/32 V` (`AONBatMonBatteryVoltageGet() >> 3`), sampled at boot (`0x11bc0` / `0x11e3c`) and immediately after every radio TX (`0x11c14` / `0x11e90`) | **verified** (`contact_v1` `0x11bc0`, `0x11c14`; `motion_v1` `0x11e3c`, `0x11e90`; [`docs/stock-re/stock_sensor.c`](stock-re/stock_sensor.c)) |
| 12 | 2 | byte 12 = low-battery latch (`0x00` normal; latched to `0x01` in NV item 11 if post-TX `bat_v32 < 0x54` = `2.625 V` on 3 consecutive TXs), byte 13 = data length `N` (`0x01` for A1/A2, `0x03` for E8) | **verified** (`contact_v1` `0x11c14`, `0x0b738`; `motion_v1` `0x11e90`, `0x0b250`) |
| 14 | N | data (for `0xA1`, live GPIO state read via `PIN_getInputValue`: `DIO7` on `contact_v1`, `DIO9 \| DIO10` on `motion_v1`) | **verified** (`contact_v1` `0x0c2f8`, `motion_v1` `0x0be78`) |
| 14+N | 2 | message counter, big-endian. Events (A2, E8) advance it by 1; the A1 status repeats the current value (77BD52EE: A2 54012 → A1 54012; 77C68193: E8 6369 → A1 6369, A1 6380 → E8 6381). On v1 sensors, a 3s button press deletes NV item 4 and resets the counter to 0 (`0x104c4` / `0x10790`), while normal reboots add `+25` to NV item 4 (`0x10278` / `0x10558`, saved every 25 events). At a join the v2 contact sensor restarted it (first A2 = 1; 3 joins) and so did the v1 motion sensor (first A1 = 0, first A2 = 1; 1 join, 77A8C793); the climate sensor continued it (2 joins) | verified (captures + `contact_v1` `0x10278`, `0x11cf4` and `motion_v1` `0x10558`, `0x11f6c`) |

| Type | Meaning | Status / check |
|---|---|---|
| 0xA1 | periodic status | verified layout; data byte (`pt[14]`): live GPIO state on v1 (`0x00` closed/clear, `0x01` open/active; `0x0c2f8` / `0x0be78`), `0x01` from the v2 contact sensor while open |
| 0xA2 | event: data 0x01 = motion active / contact open, 0x00 = clear / closed | verified against bridge `is_active` / `is_open`; `test_decoded_state_matches_bridge` |
| 0xE8 | climate: data = °C integer, °C hundredths, humidity % | verified exact against the bridge; `test_climate_report`, `test_decoded_state_matches_bridge` |
| when | A1 heartbeat: every **240 min (`48 × 5 min = 14,400 s`)** on both `contact_v1` (`0x12794` / `0x12440`) and `motion_v1` (`0x12ae8` / `0x12708`), every 120 min (119.7–120.4) from leak, climate and v2 motion. E8: sent when the reading has moved by ≥ 0.5 °C or ≥ 3 % humidity from the last E8 sent (all 393 consecutive E8 pairs; never neither), measured every 25.86 s (E8 gaps are multiples of it) | verified (7 days of the house dongle, `host/2026-05-21_*`, plus `contact_v1` `0x12440` and `motion_v1` `0x12708`); our sensor: `sensor.c`, `test_heartbeat_and_climate` |
| 0xAB | v1 motion only (`0x0cedc`): `[0]=0xAB, [1..8]=MAC, [9]=0x02, [10..21]=pirBuckets[12], [22]=0x01`. On motion (`DIO9`), `motion_v1` sends `0xA2` (`state=1`), runs a 10 s window clock (`0x12578`) for 12 windows (`120 s`) counting PIR pulses per 10 s bucket into `pirBuckets[12]` (`0x20001728 + 0x88`), then sends `0xAB`; when its 120 s clear clock (`0x125ac`) expires without motion (`event 0x2000`), it sends `0xA2` (`state=0`) | **verified** (7 days of the house dongle, 456 ABs, `test_alarm_is_rf_plaintext_plus_clock_and_rssi`; binary at `motion_v1` `0x0cedc`, `0x12578`, `0x125ac`) |
| 0xAD | v1 offline history log record (`contact_v1` `0x08280`, `motion_v1` `0x07fa8`): `[0]=0xAD, [1..8]=MAC, [9]=type, [10..17]=8B ms timestamp, [18]=0xA2, [19]=state, [20..21]=counter_be16` (`contact_v1` `0x058b8..0x058cc` copies `A2[0, 14, 15, 16]`). In `Sensor_dataCnfCb` (`contact_v1` `0x057e0`, `motion_v1` `0x05820`), if an `0xA2` event receives no 802.15.4 MAC ACK, the sensor retries the ciphertext after **80 ms** (`0x50`) and **130 ms** (`0x82`, via event `0x0002`); if all 3 attempts fail, it pushes the 12B record into the 50-entry (`600 B = 0x258`) circular buffer (`0x20000c84` / `0x200010c4`). Later, when any `0xA2` event receives a MAC ACK (`0x0582a..0x05844`), the sensor starts a **200 ms (`0xC8`) timer** (`0x12258`) to drain `0xAD` records (adding a `+1 ms` tiebreaker at `0x0834e..0x08390` if two consecutive records share the exact same 64-bit timestamp) | **verified** (`contact_v1` `0x057e0`, `0x058b8`, `0x08280`; `motion_v1` `0x05820`, `0x07fa8`) |
| 0xAA ↔ 0xA9 | 50-packet RF Factory Link-Quality / PER Test (`contact_v1` `0x03e38` & `0x0ec74`, `motion_v1` `0x04478` & `0x0ee60`): the factory gateway sends 50 encrypted 32-byte `0xAA` packets (`[0]=0xAA, ..., [30]=txRssi, [31]=sum8(0..30)`). The sensor verifies `sum8`, counts `rxGood`/`rxBad`, accumulates RSSI, echoes packets `1..49` back as 32-byte encrypted **`0xA9`** packets (`[0]=0xA9, [1..29]=payload, [30]=rxRssi, [31]=sum8`), and on packet 50 sends **`AA 55 53 06 2C <rxGood> <rxBad> <avgRssi> <csum>`** (`0x53 0x2C`) on UART0 | **verified** (`contact_v1` `0x03e38`, `0x0ec74`; `motion_v1` `0x04478`, `0x0ee60`) |
| 0xB1 | `motion_v1` decommission ACK (`0x122a4`, called from `0x04672`): when `motion_v1` receives `0xA6` with `pt[1]==0xFF && pt[2]==0x05` (decommission), it sends a 16-byte encrypted `0xB1` packet (`[0]=0xB1, [1]=pt[3]`) before calling `Ssf_factoryResetNv()` (`0x13004`) | **verified** (`motion_v1` `0x04672`, `0x122a4`) |
| — | The stock dongle forwards a report each time it receives it: a sensor's retries (same counter, < 1.5 s apart: e.g. leak sensor 77C684C4) reach the host 2–3 times. A1 heartbeats repeat the counter too, hours apart | verified (7 days of the house dongle) |
| — | Resends are rare and concentrate in a few sensors, and they do not follow the signal at the gateway: over 7 days, 88 extra copies on 4126 reports (2.1 %), **31.7 % of 77C684C4's reports and 28.9 % of 77C6853E's** (up to 3 copies), every other sensor ≤ 0.8 %, **the weakest (778B77D9, median -87 dBm) 0 %**. A sensor resends when it misses the ACK, and the dongle did receive the original, so for those two the gateway → sensor direction is the weak link (#92: a per-sensor resend counter; #69: gateway TX power) | verified | `test_resends_concentrate_in_two_sensors_not_the_weakest` (`host/2026-05-21_house_dongle_778B5F63`) |
| — | **A stock sensor nobody ACKs sends a report 6 times** (1 + 5 retries) over ~2 s, then gives up; ACKed, once. A new event cuts the last one's retries short. Stock v2 contact 77BD514D, its gateway unplugged or powered off, the other gateway overhearing. Close by (−43…−46 dBm): every report left alone 6 copies (ctr 62, 64, 66), every one followed by the next event within a second 2 (61, 63, 65). Farther: ctr 39, 40, 46, 50 six copies each (1.4–2.4 s first to last, host arrival times), the shorter bursts either followed by the next report within 2 s of their first copy (41, 43, 45, 47, 49) or heard at −84…−105 dBm (42, 44, 48); ctr 29–38, ACKed, one copy each. So a dead gateway shows as bursts of up to 6 where a lossy ACK path shows ≤ 3 (row above) | verified (v2 contact; v1 sensors and other types not measured) | `host/2026-11-01_roaming_stock_contact_retries` (runs 1-4); `test_stock_retries.py` |

Battery byte → bridge % (exact, from the morning session):

| Byte | v1 motion, climate, leak | v2 motion |
|---|---|---|
| 0x55 | | 59 |
| 0x56 | 20 | 63 |
| 0x59 | 43 | 76 |
| 0x5D | 80 | |
| 0x5E | 83 | |
| 0x5F | 87 | |
| 0x60 | 90 | |

The v2 contact sensor sent 0x29 when the bridge showed 44 %: a different scale or mapping (open).

### 4.2 Dongle → sensor: 0xB0 time sync

| Fact | Status |
|---|---|
| Sent as the data frame that answers a sensor's data request; in `gateway_v1.8` (`0x0e848..0x0e882`), queued after any sensor report **except when `sensorType == 0x0E` or `0x0F`** | **verified** (`gateway_v1.8` `0x0e848`) |
| Bytes 1–8 = 64-bit dongle clock, Unix ms big-endian (top 2 bytes `0x00 0x00`, bytes 3–8 = 48-bit Unix ms); a dongle whose clock was never set sends zeros (user's second dongle) | verified; `test_time_sync` |
| Byte 9 = `0x28` is a **hardcoded literal constant** (`movs r3, #0x28` at `gateway_v1.8` `0x0e8fa`); stock sensors (`contact_v1` `0x064f8`, `motion_v1` `0x05ee8`) only read `B0[1..8]` and ignore `B0[9]` | **verified** (`gateway_v1.8` `0x0e8fa`, `contact_v1` `0x064f8`, `motion_v1` `0x05ee8`) |

### 4.3 Join handshake (after association)

| Type | Direction | Layout | Status |
|---|---|---|---|
| 0xA3 | sensor → dongle | `[0]=0xA3, [1..8]=MAC, [9]=sensor_type, [10]=fw_version` (`0x16` v1 contact, `0x18`/`0x19` v1 motion, `0x21` v2 contact, `0x0F` v2 climate); on v1 sensors (`0x0da30` / `0x0d968`), `[11]=bootBatV32, [12]=nv8KeyValidFlag, [13]=nv10FacTestFlag, [14]=resetCauseFlag, [15]=0x00` (sent every 3.0 s up to 8 times, polling every 300 ms) | **verified** (`contact_v1` `0x0da30`, `motion_v1` `0x0d968`) |
| 0xA4 | dongle → sensor | 16-byte token: the one the host sent in 0x5321 (wyzesense2mqtt always sends `Ok5HPNQ4lf77u754`) | verified: a pairing with token `OpenWyzeSense-01` got that token in A4 (`host/2026-10-23_spare_dongle_77A85A36_pairing`, `test_stock_dongle_pairing_replies`) |
| 0xA5 | sensor → dongle | MAC, 16-byte response, sensor type, firmware version (scheduled 100 ms after `0xA4`, retried every 5.0 s up to 8 times, polling every 300 ms) | **verified** (`contact_v1` `0x0f270`, `motion_v1` `0x0f764`) |
| 0xA5 response | | **XXTEA(token)** under a per-sensor 16-byte key stored in NV item `(6, 8, 0)` bytes `0..15` (`0x20000acc` / `0x20000f28`). On `gateway_v1.8` (`0x03512..0x03552`), the CC1310 **does not verify `0xA5`**: it forwards `A5[0..26]` to the host in `0x5322` | **verified** (`contact_v1` `0x07f98`, `motion_v1` `0x0842c`, `gateway_v1.8` `0x03512`) |
| 0xA6 | dongle → sensor | `ff 04`, dongle clock (8-byte big-endian Unix ms at `A6[3..10]`). Stock sensors (`0x03f0c` / `0x04542`) copy **only `A6[0..10]`** (ignoring `A6[11..15]`): if `A6[1] == 0xFF && A6[2] == 0x04` (join accepted), they write `1` to NV item `(6, 9, 0)`, set `heartbeat5MinTicks = 47` (`0x2F` at `0x03fcc`, so the first `0xA1` fires in 5 minutes and every 48 ticks = 4 hours thereafter), and on `contact_v1` (`0x03fe0..0x03ff6`) immediately read `DIO7`, increment `seqNum`, and send an initial `0xA2` state report; if `A6[2] == 0x05` (decommission), they write `1` to NV item `(6, 11, 0)` (when `A6[3] == 0x01`), send a 16-byte `0xB1` ACK on `motion_v1` (`0x122a4`), and call `Ssf_factoryResetNv()` | **verified** (`contact_v1` `0x03f0c`, `motion_v1` `0x04542`, `gateway_v1.8` `0x0c608`) |

After joining, the v2 contact sensor restarted its message counter at 0x0001 (three joins), the v1 motion sensor
at 0 (its first A1; one join, 2026-10-23); the v2 climate sensor continued its count (0x18EB → 0x18EC on 2026-10-22,
0x18F9 → 0x18FA on 2026-10-23). Verified observation (`test_v1_motion_and_climate_join_our_gateway`).

The v1 motion sensor 77A8C793 (A3/A5 firmware byte **0x1E**; the dumped 777A9A46 has 0x19) joined our gateway
on 2026-10-23: first reports A1 alone, then A2 active (it saw motion). With the user next to it: A2 active,
A2 **clear 76.3 s later**, A2 active 5.4 s after that, then nothing for 168 s (no AB). Observation; when a v1 motion
sensor sends a clear A2 rather than an AB is open.

## 5. Firmware dumps

See `ground_truth/README.md` for what each dump is. Offsets are file offsets (= flash addresses).

| Fact | Status / check |
|---|---|
| All dumps are CC13x0 (TI CC1310 family) images: the same CC13x0 ROM, CCFG at 0x1FFA8 | verified |
| v1 motion sensor (777A9A46): MAC stored as ASCII in CCFG IEEE_MAC_1 + IEEE_BLE_0 (0x1FFCC–0x1FFD3) | `test_ccfg` |
| Both a gateway and the motion sensor: `CCFG_TAP_DAP_0` = 0xFFC500C5 (CPU debug port enabled), `BL_CONFIG` = 0xC5FE01C5 (ROM bootloader backdoor enabled, DIO1, active low) | `test_ccfg` |
| Flash page 31 (0x1F000–0x1FFFF) holds code as well as the CCFG, so it cannot be erased on its own without losing code | verified |
| CCFG power and clocks: both dongles and the v1 motion sensor force VDDR HH (VDDR_EXT_LOAD 0, ALT_DCDC_VMIN 0xC: TI's setting for +14 dBm, the stock `txPower` 0xA73F). The dongles use the DC/DC and the 32 kHz crystal (SCLK_LF option 2); the v1 motion sensor uses neither (DC/DC off in active mode and recharge, LF from the RC oscillator, option 3) | verified: our `gateway-dongle` and `sensor-motion-v1` CCFGs equal the stock ones, `test_wyze_board_ccfg_is_the_stock_one` |
| Pins, from the TI PIN driver tables (`PIN_Config` words, ended by 0xFE): dongle (2025-02-08, flash `0x174C0`) DIO10 and DIO13 outputs, low (LEDs); v1 motion (`0x156B0`) DIO12 an output, low (LED), DIO0/DIO1 UART, DIO2 factory-test (`W`) input (checked in `main()` at `0x0A5BA`); v1 contact (`0x1532C`) DIO9 LED output, DIO0/DIO1 UART, DIO10 (`0xC000200A`) factory-test (`W`) input (checked in `main()` at `0x099CE`), while DIO7 (reed switch, `0x80000007`) and DIO11 (pairing button, `0x8005000B`) live in the LZSS-compressed `.data` table at flash `0x153F8` -> SRAM `0x20002258` (opened in `Sensor_init` at `0x02950`) | **verified** (binary PIN tables + `.cinit` LZSS decompression + `main()` at `0x0994C` / `0x0A568`; `test_pin_tables`) |
| Dongle UART0 pins: TX DIO0, RX DIO1 (UART hwattrs at flash 0x17328: `… 00 01 ff ff`: tx, rx, no CTS/RTS). The ROM bootloader backdoor is DIO1 active low (BL_CONFIG): the UART line from the CH554, so the CH554 can put the CC1310 into its bootloader (the dongle's field update) | verified (binary); the field-update use: the user's WyzeSenseUpdater |
| Gateway 2025-02-08: firmware string `0.0.0.47 V1.8 Gateway GW3U` (in RAM) | `test_gateway_dumps` |
| **A zero-MAC v1 contact on the air** (777B42DF, CCFG MAC intact, NV page without item 8): it joins and reports with the MAC all zero everywhere: its association request's extended source address is 0, its A3 is `a3` + 8 zero bytes + type 01, firmware 0x10, then `6a 00 00 00`; its A2 / A1 carry the zero MAC. So the MAC it uses does not come from its CCFG | verified (`rf/2026-10-24_1147_906.7M_250k_g10_contact_v1_zero_mac_pairs`, `host/2026-10-24_contact_v1_777B42DF_zero_mac_pairs`, `test_contact_v1_zero_mac_join`) |
| **v1 contact (door) sensor** 777B42DF: a CC1310F128 in the 5x5 package (FCFG1 USER_ID 0x20018000). JTAG reaches it over 2 pins (cJTAG: TMS, TCK on its C / M pads), which OpenOCD's XDS110 driver cannot do (4-pin JTAG only) and TI's UniFlash can. Its CCFG has the **ROM bootloader disabled** (BL_CONFIG 0x00FE0100), the CPU debug port enabled, its MAC at 0x1FFCC ("777B42DF", intact). Its code (pages 0-29, 31) is byte for byte the door image of github.com/sycophantic/wyzeback. Its NV page (30) holds only item 6 (01, 02, 03) and item 4 (0): no network (1), no item 8, which in wyzeback's image holds the sensor's MAC | verified (`firmware/contact_v1_777B42DF_2026-10-24`, `test_contact_v1_dump`) |
| **Zero-MAC root cause in the v1 sensor binaries** (`contact_v1` `0x07c18`, `0x11c50`, `0x0cd1c`; `motion_v1` `0x080ac`, `0x11ec8`, `0x0cc14`): CCFG `0x1FFCC` is write-only (written only by Factory Test UART cmd `0x08` when `DIO10`/`W` is held low at boot). At normal boot, `Ssf_readMacAndKey()` (`0x11c50` / `0x11ec8`) reads the MAC **only from NVOCTP item `(6, 8, 0)`** in page 30. Meanwhile, `Sensor_init()` (`0x07c18` / `0x080ac`) unconditionally increments and writes NV item `(6, 6, 0)` (boot counter, `0x1216c`) and NV item `(6, 4, 0)` (`seq + 25`, `0x10278`) **before** checking battery voltage (`<= 0x53` = `2.59 V` at `0x11bc0`). Repeated brownout boots on a dying coin cell fill page 30 until `NVOCTP_compactPage()` erases page 30 and browns out mid-erase, wiping NV item 8 (`known-issues.md` §1, [`docs/stock-re/stock_sensor.c`](stock-re/stock_sensor.c)) | **verified** (`contact_v1` and `motion_v1` binaries) |
| **v1 sensor button state machine & LED patterns** (`contact_v1` `0x0d218`, `0x0db2c`, `0x0fee4`; `motion_v1` `0x0d100`, `0x0da64`, `0x101d4`): 10 ms poll clock with `40 ms` debounce (`4` ticks). Short press (`< 2.04 s`): no-op on `contact_v1`, arms 1-shot PIR test blink on `motion_v1`. At **`3.04 s` held** (`300` ticks after debounce), blinks LED **3x (`400 ms`)** if provisioned (`nv8 && nv10 && !nv11 && !lowBat`) or **5x (`200 ms`)** if unprovisioned/low-battery. Releasing at `3.05 s .. 8.04 s` wipes network NV items (`9, 2, 3, 4, 5, 6`) and starts 30 join scan cycles (`100 ms × 3` blink on timeout, `400 ms × 3` on `0xA6` success). Holding `> 8.04 s` (`> 800` ticks) aborts the press | **verified** (`contact_v1` and `motion_v1` binaries; [`docs/stock-re/stock_sensor.c`](stock-re/stock_sensor.c)) |
| **CC1310 (LaunchPad), a software system reset with the XDS110 attached since power-up:** SYSRESETREQ, `SysCtrlSystemReset` or `HapiResetDevice` from a running application does not boot the flash: the ROM boot code (0x10003C58 → 0x10003A18) waits in WFI on its "wake from shutdown" path (stack 0x11002000), answers no bootloader sync, and the next reset from that state boots normally, so resets alternate (12 of 12 in pattern; the backdoor pin, BOOT_DET and the RTC ruled out). A CPU-only restart (OpenOCD's VECTRESET, or `chip_reset` jumping to the selector by hand) boots every time (20 of 20). Without a debugger (a sensor in the field): not tested | verified on the LaunchPad (2026-10-24); the JTAG power domain as the cause: hypothesis |
| **v1 contact pins** (777B42DF): UART TX DIO0 / RX DIO1 (stock UART hwattrs at 0x15280, as the dongle's; its RX/TX pads); the reed switch DIO7 (chip pin 20, closes to GND); the LED DIO9; the button DIO11 (pressed: low); the W pad DIO10; the S pad RESET_N; the M / C pads TMS / TCK. Reed switch and button were found by the user (continuity) and by our firmware's `watch` (pull-ups, the levels on every change) | verified (2026-10-24, our firmware on the unit: `boards/wyze_contact_v1/board_config.h`) |
| v1 contact 777B42DF: the button's DIO11 has an **external pull-up**: it reads high with the chip's pull-up, with none, and with its pull-down (`watch 12 800 up/none/down`), so it is a safe ROM-bootloader backdoor pin (high unless pressed) | verified (2026-10-24, issue #14) |
| v1 contact 777B42DF: the W pad's **DIO10 has an external pull-up** too: high with the chip's pull-up, none and pull-down (`watch 4 C00 up/none/down`) | verified (2026-10-24, issue #14) |
| v1 contact 777B42DF: **flashing over the ROM bootloader on its UART pads works** (`tools/uart_sbl.py flash`: selector + slot A, 64 KB with the FF stretches skipped, 19.6 s at 115200, chip CRC32 verified, pages 30-31 unchanged; the v99 marker image then ran). The bootloader's own reset (CMD_RESET) left the chip running nothing until a pin reset, twice; a pin reset (S) then started the new image | verified (2026-10-24, the user holding W to GND at the reset) |
| **v1 contact 777B42DF's CCFG changed on purpose** (the user's decision, #14): BL_CONFIG 0x00FE0100 -> **0xC5FE0BC5** (ROM bootloader on, backdoor DIO11 active low), then the same day **0xC5FE0AC5** (backdoor the W pad, DIO10, active low: the user's choice), the other 85 bytes (MAC included) unchanged, read back; page 31's stock code before the CCFG erased (unused by our firmware); since #26 `enable-bootloader` keeps page 31 whole (only BL_CONFIG changes: on 777B42DF, button then w, the page read back differing in 0x1FFD9 alone, then identical). Its stock CCFG and page 31 stay in `firmware/contact_v1_777B42DF_2026-10-24`. It boots our firmware as before | verified (`tools/uniflash.py enable-bootloader`) |
| v1 contact 777B42DF with backdoor `w`: **W held to GND at a reset (RESET_N, `tools/uniflash.py reset`) enters the ROM bootloader** on the RX/TX pads at 115200: auto-baud ACK, ping ACK, chip ID 0x20018000 (= its FCFG1 USER_ID); with W released, a reset runs our firmware again | verified (2026-10-24, the user holding W to GND; `tools/uart_sbl.py`) |
| **Our sensor firmware on a v1 contact** (777B42DF, a zero-MAC unit, its CCFG untouched): it reads the MAC from the CCFG, pairs with our gateway as **777B42DF** (type 01, short 0x0006, "gateway ours"), sends its A1, and an A2 for every open / close of the reed switch, counter 1, 2, 3 … | verified (2026-10-24, `host/2026-10-24_contact_v1_ours_pairs`) |
| Flashing a v1 contact over 2-pin cJTAG with UniFlash and letting it run (DSLite `-u`, the debug power domain kept on, its default): our firmware printed its banner, then stopped answering, until a real power-off (the LaunchPad's USB pulled; pulling its 3V3 wire alone did not power it off: the UART and JTAG lines kept the chip powered). A **board reset through RESET_N with the debug domain released** (DSLite `-r 2`, `KeepDebugPoweredAtDisconnect=false`) instead starts it normally, and brings a stuck one back | verified (2026-10-24, reproduced and recovered by `test_hw_contact_v1.py`; `tools/uniflash.py`) |
| **A CC1310 in standby still answers the debugger:** our sensor image built for a battery board (standby from start-up, no console) on the LaunchPad was reached without any reset over 2-pin cJTAG (UniFlash) and 4-pin JTAG (OpenOCD). So standby does not explain a unit that stops answering on TMS/TCK | verified on the LaunchPad (2026-10-24; builds with `-DOWS_LOW_POWER=1 -DOWS_DEBUG_WINDOW_MS=0 -DOWS_NO_CONSOLE=ON`) |
| A CPU-only reset keeps the peripherals: once the UART came back enabled with a baud divisor of 0 and a full TX FIFO (who wrote it is open), and the ROM's UARTDisable (0x10002592) then waited for BUSY forever at the next start-up; `uart_init` now resets the UART's registers itself first | verified on the LaunchPad (2026-10-24) |
| Flash page 30 (0x1E000) is TI NV-on-chip storage (`NVOCTP`, `NVINTF_SYSID_APP = 6`): records of data followed by an 8-byte header (`06`, flags: 0x40 valid / 0x00 deleted, item, sub-ID, length; little-endian), read back from the last header; 4 bytes of page header first. On the gateway (`ti154stack` Collector `csf.c`, `gateway_v1.8` `0x0e56c`, `0x13088`, `0x13358`): **item 1** = packed `Llc_netInfo_t` (7 bytes: `isFh`, `channel`, `panID`, `shortAddress`, `state`), **item 4** = `uint16_t numDeviceListEntries` (2 bytes), **item 5/n** = standard 24-byte TI `Llc_deviceListItem_t` (`panID`, `shortAddress`, 8B `extAddress` MAC, 8B `ApiMac_capabilityInfo_t` where byte 17 is `capInfo.allocAddr = 0x01`, and `uint16_t msa = 0` + padding), **item 6** = 8-byte gateway MAC (`0x4308`/`0x430A`). Decoded: v1.4 dongle PAN 0x2357 / 0xC907 with 12 sensors (count 12), v1.8 (2025-02-08) PAN 0x0A96 / 0xF4B1 with 2 | verified, `test_stock_nv_page` (`wyzesense_ref.nvpage`); struct layout verified in `gateway_v1.8` `0x0e56c` ([`docs/stock-re/stock_common.h`](stock-re/stock_common.h)) |
| … so 77A88E96 (0x1E058), once read as that dongle's own MAC, is sensor 1 of its table (short 0x0002). Where the dongle keeps its own MAC (Get MAC) is in the CH554 (§9.2), while `0x4308`/`0x430A` (`0x03276`/`0x0326e`) read/write NV item `(6, 6, 0)` on the CC1310 | verified (the table + `gateway_v1.8` `0x0326e`) |
| The v1.4 dongle's table has two pairs of sensors on one short address (777C1FCA and 7788CCF6 on 0x0004; 7788CD06 and an all-zero MAC on 0x0005): in `Cllc_assocIndCb` (`gateway_v1.8` `0x0e60c..0x0e640`), a new sensor is assigned `shortAddr = Csf_getNumDeviceListEntries() + 1`, which collides whenever a previously paired sensor was deleted from the middle of the table (known-issues §2) | **verified**, `test_stock_nv_page` and `gateway_v1.8` `0x0e638` ([`docs/stock-re/stock_gateway.c`](stock-re/stock_gateway.c)) |
| Motion firmware 0xF764 (`contact_v1` `0x0EA84`) builds A5: `0xA5`, 8-byte MAC from RAM `0x20000F08` (`0x20000AAC`), 16 bytes = `xxtea(token)` (`0x842C` / `0x7F98`), sensor type `0x02` (`0x01`), firmware version `0x19` (`0x16`) | `test_motion_firmware_join_and_mac_code` (call and constants); full listing in [`docs/stock-re/stock_sensor.c`](stock-re/stock_sensor.c) |
| `0x842C` (`contact_v1` `0x7F98`, `gateway_v1.8` `0x8B2C`) is XXTEA ("corrected block TEA"): n = 4 words, 19 rounds, delta 0x9E3779B9 (the code subtracts 0x61C88647), big-endian words, in place; on sensors, key = 16 bytes read from RAM `0x20000F28` (`0x20000ACC`), loaded at boot from NV item `(6, 8, 0)` bytes `0..15`. On `gateway_v1.8`, `0x8B2C` is called **only at `0x039BC`** (handling `0x4302` → `0x4303` `GET_ENR` with the 16B key at `0x20001374` written by `0x430A` at `0x12FA8`); `0x0C608` (`send_A6`) uses only `AES_encryptBuffer` (`0x0D5A4`), and neither binary contains `XXTEA_decrypt` | **verified**; constants in the test ([`docs/stock-re/stock_common.h`](stock-re/stock_common.h), [`docs/stock-re/stock_gateway.c`](stock-re/stock_gateway.c)) |
| Tried as the XXTEA key, none matching: 16 × `'0'`, zeros, 0xFF, MAC × 2, MAC + zeros, zeros + MAC, AES key, token, MAC + token half, token half + MAC | verified negative |
| **The `W` test pad (`DIO10` on `contact_v1`, `DIO2` on `motion_v1`) & Factory UART Task**: in `main()` (`contact_v1` `0x0994C`, `motion_v1` `0x0A568`), immediately after `PIN_init(BoardGpioInitTable)` (`0x1532C` / `0x156B0`), the firmware reads `GPIO_readDio(0)` (`TX`) and `GPIO_readDio(10)` (`W` pad on `contact_v1`; `GPIO_readDio(2)` on `motion_v1`) twice (`0x099C6–0x099E8`). On a normal boot (`W` HIGH), `sram[0x20000A74]` stays `0` and **Task 2 (`FactoryUart_taskFxn` at `0x0B044`) is never constructed and `UART0` is never opened**. When `W` is grounded at boot (`sram[0x20000A74] = 1`, `sram[0x20000A8B] = 1`): (1) `main()` constructs `FactoryUart_taskFxn` (`0x0B044`, pri 2, stack `0x3C0`), which calls `UartFactory_open` (`0x0F120`) to open `UART0` at 115200 8N1 and loops on `UART_read` (`0x0B070`) for `0xAA 0x55 0x43/0x53` factory commands (holding TI-RTOS `PowerCC26XX_SB_DISALLOW` so the CC1310 stays awake); (2) `Sensor_init` (`0x02BA2–0x02BD6`) turns the red LED solid ON (`Ssf_setLed(1, 1)` at `0x0E808`) instead of blinking; (3) `Sensor_init` (`0x02C86–0x02C94`) skips posting `SENSOR_START_EVT` (`0x0001`); and (4) `Sensor_init` (`0x027F6–0x0281E`) sets the unjoined poll interval (`0x200017C8`) to `400 ms` (`0x190`) instead of `700 ms` (`0x2BC`). In `Sensor_process` (`0x01AB0–0x01BDC` / `0x01CCA–0x01E6A`), `FactoryTest_process43` (`0x0CA44` / `0x0CC14`), and `FactoryTest_sendReply43` (`0x09124` / `0x09448`): class `0x43` handles `0x04 -> 0x05` (read 8B MAC from NV 8), `0x06 -> 0x07` (read 16B XXTEA key from NV 8), `0x08 -> 0x09` (write 8B MAC to CCFG `0x1FFCC` and NV 8), `0x0A -> 0x0B` (write 16B XXTEA key to NV 8 with `valid=1`), and `0xD0 -> 0xD1` (read NV status flags `{joined, facTest, [nv12], lowBat}`); class `0x53` handles `0x16 -> 0x17` (firmware version string at `0x0EB00` / `0x0EC6C`), `0x2A -> 0x2B` (start 50-pkt RF PER test on 16-entry `TBB` channel map `{19..25, 32..34, 38..40, 48..50}`, reporting `0x53 0x2C <rxGood> <rxBad> <avgRssi>` after 50 `0xAA` packets), `0x2D` (`0xFF` writes `1` to NV item 10 and blinks LED `3x 400 ms`), `0xD2 -> 0xD3` (`motion_v1` `0x01E02`: 20-second PIR self-test returning pulse count), and `0xD4` (`motion_v1` `0x01CFE`: `0xFF` writes `1` to NV item 12) | **verified**; `test_motion_firmware_join_and_mac_code` and [`docs/stock-re/stock_sensor.c`](stock-re/stock_sensor.c) |
| `Ssf_checkBootBattery` (`motion_v1` `0x11E3C`..`0x11E8C`, `contact_v1` `0x11BC0`) samples `AONBatMonBatteryVoltageGet() >> 3` 6 times at 200 ms intervals and compares with `0x53` (`2.59375 V`); if `<= 0x53`, skips `Ssf_readMacAndKey()` and blinks the LED 5 times (`200 ms`) | **verified** (`contact_v1` `0x11bc0`, `motion_v1` `0x11e3c`) |
| Gateway 2025-02-08: the host link is UART0 (base 0x40001000, interrupt 0x15) at 115200 baud, 8 data bits, with a 64-byte RX ring buffer at RAM 0x20003E18 (UART params/hwattrs at flash 0x1731C–0x17347) | `test_gateway_host_link_is_uart0` |
| … that ring holds host → dongle protocol bytes (§9: an ACK of 0x5332, then a 0x5333 time sync), and RAM 0x200018D5 holds a complete, checksum-valid dongle → host packet (0x532F, count 2): the CC1310 itself speaks the host protocol | `test_gateway_host_link_is_uart0` |
| Gateway flash 0x17104: an AA-55-framed record (`55 aa 53 0e 3b …`, command 0x3B) in page 23; not a valid packet as stored | observation; meaning open |

## 6. Known stock bugs (evidence)

See `docs/known-issues.md`.

## 7. Hypotheses (status after binary verification)

1. (Byte 10 of sensor reports = chip temperature in °C via `AONBatMonTemperatureGetDegC()`: **verified**, §4.1.)
2. (Byte 11 = battery in `1/32 V` via `AONBatMonBatteryVoltageGet() >> 3`; `0x53` = `2.59 V` boot threshold and `0x54` = `2.625 V` post-TX low-battery threshold: **verified**, §4.1 & §5.)
3. (Sensor type 0x01 = v1 contact: **verified**, §4.1.)
4. A report that is not delivered keeps its counter on v2 contact: 77BD567E (network 0xE261, whose coordinator never
   answers) re-sent counter 0x0173 at 23:14 and again at 09:14, each time as 4 MAC sequence numbers × 2
   identical transmissions 0.25 s apart. (On v1 sensors, un-ACKed events are pushed to a 50-entry ring buffer and sent as `0xAD`: **verified**, §4.1.)
5. (A4 token and A6 `ff 04`: **verified**, §4.3.)
6. The XXTEA check is Wyze's way of verifying genuine sensors (each sensor is provisioned at the factory via UART command `0x0A` with a 16-byte key in NV item 8; the gateway forwards `0xA5` to the host in `0x5322` without checking it on the CC1310: **verified**, §4.3 & §5).
7. (Zero-MAC root cause: `Sensor_init` writes NV items 6 and 4 on every boot before checking battery voltage, triggering an NVOCTP page-30 compaction erase during coin-cell brownout and wiping NV item 8 while `Ssf_readMacAndKey` never reads CCFG `0x1FFCC`: **verified**, §5 & `known-issues.md` §1.)
8. (Sensor table in flash page 30 = standard TI `ti154stack` `Llc_deviceListItem_t`: **verified**, §5.)
9. (Pairing on the host side: **verified**, §9.2.)
10. (The dongle queues `0xB0` after any sensor report except when `sensorType == 0x0E` or `0x0F` at `gateway_v1.8` `0x0e848`: **verified**, §4.2.)

## 8. Open questions

What remains open (mostly v2 sensor internals and CH554 firmware): `docs/stock-unknowns.md`.

1. (resolved: ACK FCS, §2.)
2. Per-sensor XXTEA key derivation on the Wyze cloud side. Deliberately left open (user's decision). On the sensor, the 16-byte key is written into NV item `(6, 8, 0)` at the factory by UART command `0x0A` (`0x0cd1c` / `0x0cc14`). On the stock dongle (`gateway_v1.8` `0x03512`), the CC1310 never checks `0xA5`.
3. (resolved for v1 in binary: `0xA3` bytes 11–15, `0xA6` `ff 04` validation, §4.3; v2 leak / v2 motion `0xA3` bytes still open.)
4. (resolved: `FactoryTest_processUartCmd` at `0x0cc14` / `0x0cd1c` is driven by `FactoryTest_taskFxn` on UART0 when `DIO10` / `W` is held low at boot, §5.)
5. (resolved for v1 and dongle: `0xAB` 12×10s PIR pulse histogram, `0xA1` data byte = live GPIO state, `0xB0` byte 9 = hardcoded literal `0x28`, §4.1–4.2; v2 leak event data and v2 contact `0x29` battery mapping still open.)
6. Sensor types 0x05 and 0x0C (seen on other networks).
7. (resolved for v1 in binary: 10 polls × 700 ms on `contact_v1`, 5 polls × 800 ms on `motion_v1`, plus 50-entry `0xAD` offline log buffer, §4.1.)
8. MCU of the v2 sensors (`contact_v1` and `motion_v1` are both CC1310F128 5x5 RHB, §5).
9. (resolved: flashing the dongle via `0x4312` and `tools/dongle_sbl.py`, §9.2.)
10. GET_ENR (0x4302 → 0x4303): the 16-byte reply is a per-dongle function of the host's token computed by the CH554.
11. (resolved in `gateway_v1.8` binary: `0x535E` = UART RX checksum error / `ApiMac_mcpsDataReq` status `0xF1` / duplicate `0xB4` seq; `0x5335` = diagnostic log dump from `0x20001867`; `0x5355` = RF opcode `0xE8` report; `0x5337` = 30-entry offline `0xA2` buffer drain; §9.2 & [`docs/stock-re/stock_gateway.c`](stock-re/stock_gateway.c).)

## 9. Host ↔ dongle protocol

The protocol the host (the user's bridge, wyzesense2mqtt) speaks to the dongle, as reverse-engineered in
https://github.com/HclX/wyzesense2mqtt-rs (`docs/protocol.md`, commit 9b804d1) and verified against the gateway binary
(`Uart_processCommands` at `gateway_v1.8` `0x02bc0`; [`docs/stock-re/stock_gateway.c`](stock-re/stock_gateway.c)). Codec:
`wyzesense_ref.host`; tests: `reference/tests/test_host.py`.

Evidence: the gateway dump (§5: UART0, packets in RAM), and host-side captures from wyzesense2mqtt-rs's
`tests/captures/`, imported as `ground_truth/host/`: 7 days of the house dongle 778B5F63 (2026-05-21…27,
~13,000 packets) and a handshake trace of dongle 77A85A36.

### 9.1 Transport and framing

| Fact | Status | Check |
|---|---|---|
| CC1310 UART0, 115200 8N1; the stock dongle's CH554 turns it into USB HID. Each HID read = 1 length byte + up to 63 protocol bytes (the rest is stale) | verified (UART from the binary, HID from the captures) | `test_gateway_host_link_is_uart0`, `test_every_capture_record_holds_valid_packets` |
| Packet: magic (`aa 55` host → dongle, `55 aa` dongle → host), type (0x43 sync, 0x53 async), b2 = payload length + 3, cmd, payload, 16-bit big-endian sum of all preceding bytes | verified (every captured record parses; so does the packet in the dump's RAM) | `test_build_matches_captured_packets`, `test_parse_and_scan` |
| ACK: cmd 0xFF, b2 = low byte of the acknowledged command, no payload. Every async packet except an ACK is ACKed by the receiver, both ways | verified | as above |
| Replies are cmd + 1 | verified | |

### 9.2 Commands

| Cmd | Direction | Payload | Status |
|---|---|---|---|
| 0x4327 / 0x5327 → 0x28 | Inquiry (`0x0325e` → `0x03922`) | → `01` | **verified** (`gateway_v1.8` `0x0325e`) |
| 0x4302 / 0x5302 → 0x03 | Get ENR (`0x032ba` → `0x039ba`) | 16-byte token (`30`×16) → 16-byte `XXTEA-19(token, g_gwEnrXxteaKey)` (`0x08b2c` with key at `0x20001374`) | **verified** (`gateway_v1.8` `0x039ba`) |
| 0x4304 / 0x5304 → 0x05 | Get MAC (`0x032b6` → `0x039ac`) | → 8-byte ASCII MAC (house dongle: `778B5F63`; CC1310 reads `0x20001348`) | verified; `test_house_dongle_identity_and_sensor_list` |
| 0x5306 → 0x5307 | Get ENR key (`0x032b2` → `0x03996`) | → 16-byte `g_gwEnrXxteaKey` (`0x20001374`, default `"WYZE098765432101"`) | **verified** (`gateway_v1.8` `0x03996`) |
| 0x5308 → 0x5309 | Write gateway MAC (`0x03276` → `0x03966`) | 8-byte MAC → writes custom NV item `(6, 0x20, 0)` & `(6, 0x11, 0) = {0x00, 0x99}` → `ff` | **verified** (`gateway_v1.8` `0x03276`) |
| 0x530A → 0x530B | Write ENR key (`0x0326e` → `0x12fa8` → `0x038c6`) | 16-byte key → writes `0x20001374` & custom NV item `(6, 0x30, 0)` → `ff` | **verified** (`gateway_v1.8` `0x12fa8`) |
| 0x5316 → 0x5317 | Get version (`0x03262` → `0x0392c`) | → starts 300 ms LED blink, returns ASCII `0.0.0.47 V1.8 Gateway GW3U` (`0x2000184c`) | **verified** (`gateway_v1.8` `0x0392c`) |
| 0x5314 → 0x5315 | Finish auth (`0x0315e` → `0x03950`) | `ff` → empty (`0x200017d4 = 1`, stops LED blink, turns LED1 ON; when host ACKs `0x5315` at `0x07380`, gateway immediately sends `0x5332` time-sync req!) | **verified** (`gateway_v1.8` `0x0315e`, `0x07380`) |
| 0x532E → 0x532F | Sensor count (`0x03220` → `0x038ee`) | → 1 byte (`Csf_getNumDeviceListEntries` at `0x13088`) | **verified** (`gateway_v1.8` `0x038ee`) |
| 0x5330 → 0x5331 × n | Sensor list (`0x0321c` → `0x038e0` → `0x0dc24`) | count → one 8-byte ASCII MAC per `0x5331` packet (10 ms apart), followed immediately by `0x5332` (`0x13e90`) | **verified** (`gateway_v1.8` `0x0dc24`) |
| 0x5332 / 0x5333 | Time sync (`0x13e90` / `0x02f4c`) | dongle asks right after host ACKs `0x5315` (`0x07380`), after `0x5331` sensor list (`0x038e6`), and every 11 minutes (`11 × 60 s` ticks at `0x02070`); host answers 8-byte Unix ms, big-endian (`0x20001340`, incremented `+10 ms` every 10 ms at `0x01e42`) | **verified** (`gateway_v1.8` `0x02f4c`, `0x01e42`, `0x02070`); `test_time_sync` |
| 0x5319 | Alarm (`0x0359e`, dongle → host) | dongle clock (8 bytes, Unix ms) + decrypted RF plaintext (`0xA1`/`0xA2`/`0xAA`/`0xE1`: 16B; `0xAB`: 22B; `0xAD`: converted to `0xA2` at `0x08e10`; `0xDF`/`0xE6`: 11B) + 1 byte signal strength (−dBm) | **verified** (`gateway_v1.8` `0x0359e`, `0x08e10`); `test_alarm_is_rf_plaintext_plus_clock_and_rssi` |
| 0x531C → 0x531D | Scan (pairing) on/off (`0x0312c` → `0x038bc`) | `01` (`g_joinPollState = 3`, 60 s association permit) / `00` (`g_joinPollState = 4`) → **empty** | **verified** (`gateway_v1.8` `0x0312c`, `0x01c02`, `0x01cae`) |
| 0x5320 | Scan result (`0x03542`, when a sensor's A3 arrives) | `A3[1..11]` + `0x00` (12B: 8B MAC, sensor type, fw version, boot battery, `0x00`; gated by `scanInclusionEnable == 0xFF && permitJoinActive == 1`) | **verified** (`gateway_v1.8` `0x03542`) |
| 0x5321 → 0x5322 | Get sensor R1 (`0x030d0` → `0x03512`) | MAC + 16-byte token (queued in `0xA4` via `g_joinPollState = 1` at `0x0f878`) → `A5[1..26]` + `0xFF` (27B) when sensor's `0xA5` arrives | **verified** (`gateway_v1.8` `0x030d0`, `0x03512`) |
| 0x5323 → 0x5324 | Verify sensor (`0x0306a` → `0x038bc`) | MAC + `ff 04` (or `00 00` to reject & call `Csf_removeDevice` at `0x1064c`) → empty; queues `0xA6` (`g_joinPollState = 2` at `0x0c608`) | **verified** (`gateway_v1.8` `0x0306a`) |
| 0x5329 | Join done (`0x03504`, when a sensor's `0xA7` arrives) | `A7[1..10]` (10B: 8B MAC, sensor type, fw version) | **verified** (`gateway_v1.8` `0x03504`) |
| 0x5325 → 0x5326 | Delete sensor (`0x02fee` → `0x034d2`) | MAC → MAC + `ff` (if deleted via `Csf_removeDevice` `0x1064c`) or `00` | **verified** (`gateway_v1.8` `0x02fee`, `0x034d2`) |
| 0x532A → 0x532B / 0x2C | RF factory test (`0x03224` → `0x03918` / `0x03904`) | 1B channel → sets `rfTestChannel` (`0x200012fc`), replies empty `0x532B` (and `0x532C` 10B factory data) | **verified** (`gateway_v1.8` `0x03224`) |
| 0x532D | Factory finish (`0x02fb8`) | `ff` writes `{0x10, 0x00, 0x88}` to custom NV item `(6, 0x10, 0)` | **verified** (`gateway_v1.8` `0x02fb8`) |
| 0x5334 → 0x5335 | Internal diagnostic log dump (`0x03218`, `0x01fc2`) | triggers 1000 ms timer (`0x14204`) to send chunks from RAM `0x20001867` | **verified** (`gateway_v1.8` `0x01fc2`) |
| 0x5336 → 0x5337 | Buffered offline `0xA2` event drain (`0x03214`, `0x01f42`) | drains 30-entry × 22-byte buffer (`0x20001b98`) of `0xA2` events saved on 1000 ms `0x5319` ACK timeout (`0x01eca`): `a2`, MAC (8), type (1), time (8), `a2`, state (1), counter (2) | **verified** (`gateway_v1.8` `0x01eca`, `0x01f42`) |
| 0x533A → 0x533B | Prepare delete all (`0x03206` → `0x0348e`) | saves 9-byte argument in `0x20001820` | **verified** (`gateway_v1.8` `0x03206`) |
| 0x533C | Set NV item `0x12` & disassociate all (`0x02f0a`) | `01` writes `{0x12, 0x00, 0x77}`, `00` writes `{0x12, 0x00, 0x00}`, calls `0x14b84` | **verified** (`gateway_v1.8` `0x02f0a`) |
| 0x533D → 0x533E | Set inclusion mode (`0x02eb2` → `0x03464`) | `ff` (enable `scanInclusionEnable = 0xFF`, LED1 ON, NV `0x13 = 1`) or `00` (disable) → `ff` / `00` (or `77` if `finishAuthDone == 0`) | **verified** (`gateway_v1.8` `0x02eb2`) |
| 0x533F → 0x5340 | Delete all / factory reset (`0x03202` → `0x038ca` → `0x11310`) | empty → `ff`, then disassociates all (`0x14b84`), wipes RAM tables, and calls `SysCtrlSystemReset()` (`0x16184`) | **verified** (`gateway_v1.8` `0x11310`) |
| 0x5341 → 0x5342 | Read GPIO factory test (`0x031fe` → `0x03456`) | empty → 1B `GPIO_readDio(0xFF)` (`0x00` or `0xFF`) | **verified** (`gateway_v1.8` `0x03456`) |
| 0x5353 → 0x5354 | Send RF `0xE9` (`0x02e80` → `0x10d9c` → `0x042d4`) | `[8B MAC][rsvd][len][payload...]` → queues 32B AES-encrypted RF `0xE9` (`g_joinPollState = 10`); replies `0x5354` (`ff`) when sensor answers `0xEA` | **verified** (`gateway_v1.8` `0x02e80`, `0x10d9c`, `0x042d4`) |
| 0x5355 | Secondary leak / `0xE8` packet (`0x04166`) | `pkt[0..pkt[10]+10]` + `-rssi` | **verified** (`gateway_v1.8` `0x04166`) |
| 0x5357 → 0x5358 | Get RF noise floor (`0x031c4` → `0x11bbc`) | empty → 4-byte big-endian `uint32_t` (`noise * 1e6`) | **verified** (`gateway_v1.8` `0x031c4`) |
| 0x535A / 0x5359 | Send RF `0xB3` / recv `0xB2` (`0x02e58` → `0x128f0` / `0x0406e`) | `[8B MAC][36B payload]` → queues 37B unencrypted RF `0xB3` (`g_joinPollState = 11`); `0xB2` forwarded as `0x5359` | **verified** (`gateway_v1.8` `0x02e58`, `0x128f0`, `0x0406e`) |
| 0x535C / 0x535B / 0x535D | Send RF `0xB5` / recv `0xB4` & `0xB6` (`0x02e26` → `0x12324`) | `[8B MAC][130B payload]` → queues 131B unencrypted RF `0xB5` (`g_joinPollState = 12`); `0xB4` forwarded as `0x535B`, `0xB6` as `0x535D` | **verified** (`gateway_v1.8` `0x02e26`, `0x12324`, `0x03fdc`, `0x0425c`) |
| 0x535E | Error notification (`0x0730a`, `0x0d35e`, `0x0401a`) | 0B on UART RX checksum error; 1B (`ApiMac_status_t`, e.g. `0xF1` `transactionOverflow`) when `ApiMac_mcpsDataReq` fails; 2B (`seq`) on duplicate `0xB4` | **verified** (`gateway_v1.8` `0x0730a`, `0x0d35e`, `0x0401a`) |
| 0x5370 → 0x5371 | Send RF `0xD0` sensor config (`0x02df0` → `0x11650`) | `[8B MAC][3B args]` → queues 16B AES-encrypted RF `0xD0` (`g_joinPollState = 6`) → empty `0x5371` | **verified** (`gateway_v1.8` `0x02df0`, `0x11650`) |
| 0x5372 → 0x5373 | Send RF `0xD2` sensor config (`0x02dba` → `0x11f1c`) | `[8B MAC][3B args]` → queues 16B AES-encrypted RF `0xD2` (`g_joinPollState = 8`) → empty `0x5373` | **verified** (`gateway_v1.8` `0x02dba`, `0x11f1c`) |
| 0x5380 → 0x5381 | Send RF `0xC0` sensor command (`0x02d8c` → `0x11f7c`) | `[8B MAC][2B rsvd][1B arg]` → queues 16B AES-encrypted RF `0xC0` (`g_joinPollState = 5`) → `0x5381` (`ff`) | **verified** (`gateway_v1.8` `0x02d8c`, `0x11f7c`) |
| 0x5390 → 0x5391 | Send RF `0xE0` sensor command (`0x02d5a` → `0x11fdc`) | `[8B MAC][1B arg]` → queues 16B AES-encrypted RF `0xE0` (`g_joinPollState = 7`) → empty `0x5391` | **verified** (`gateway_v1.8` `0x02d5a`, `0x11fdc`) |
| 0x5392 → 0x5393 | Send RF `0xE2` sensor command (`0x02d28` → `0x1203c`) | `[8B MAC][1B arg]` → queues 16B AES-encrypted RF `0xE2` (`g_joinPollState = 9`) → empty `0x5393` | **verified** (`gateway_v1.8` `0x02d28`, `0x1203c`) |
| 0x53D0 → 0x53D1 | Get provisioning flags (`0x031c0` → `0x0340e`) | empty → 3 bytes `{nv10[0]==0x99, nv10[1]==0x88, nv10[2]==0x77}` from custom NV item `(6, 0x10, 0)` | **verified** (`gateway_v1.8` `0x0340e`) |
| 0x53E9 → 0x53EA | Start 50s noise monitor (`0x031ac` → `0x146ac`) | sets `chimeActiveFlag = 1` (`0x200017c9`), starts 50,000 ms timer (`0x146ac`), replies empty `0x53EA` | **verified** (`gateway_v1.8` `0x031ac`) |
| — | The dongle resends a notification the host does not ACK (`0x0209e`): 4 sends in all (`txRetryCount` `0..3`), 150 ms (`0x96`) apart (`0x14238`), then gives up. Our gateway does the same (`gw_tick`) | **verified** (`gateway_v1.8` `0x0209e`, `0x14238`); `host/2026-10-23_spare_dongle_*` |
| The dongle's Wyze MAC (Get MAC, e.g. `778B5F63`) never goes on the air | none of the 641 good-CRC frames recorded (61 from the house dongle) holds it, raw or decrypted: on the air the coordinator is its PAN, short address and the CC1310's factory IEEE address (also in its page-30 network record). So the CC1310 needs no MAC; the CH554 keeps it for the host | verified (2026-10-24, all `rf/*/decoded_frames.jsonl`) |
| sync (0x43xx) vs async (0x53xx) | **The dongle's CH554 answers the sync commands itself; the CC1310 sees only the async ones.** With our firmware on the CC1310 (2026-10-23, spare dongle 77A85A36), Get ENR still got the stock ENR (`55ffeb67…`; ours answers zeros) and Get MAC the stock MAC `77A85A36` (ours would say `774F48FD`), one answer each; Get Version, Finish Auth, the sensor list and our 0x53F0 were answered by our firmware. (The user's hypothesis.) This is why the dongle's MAC is in neither the CC1310's flash nor its RAM | verified (on the dongle, `host_link.py` log) |
| 0x4312 → 0x4313 | Update mode (WyzeSenseUpdater): the stock dongle answers an empty 0x4313 (`55aa4303130158`); then the CC1310 is in its ROM bootloader by a **pin reset** (its reset-source register, read there), so the CH554 resets it with the backdoor line (DIO1) low, and from then on relays bootloader packets whole. Leaving it: the user's experience (and WyzeSenseUpdater's README) is that it takes an unplug; but on 2026-10-24,
after `dongle_sbl.py bench` ended with the bootloader's reset command (0x25), the dongle answered the normal host
protocol about a minute later without one (observation, once: whether the reset command or a CH554 timeout did it
is open). Our gateway answers the same; with our firmware on the dongle the path works unchanged (bootloader reached by a pin reset, our image read back through it) | verified (`firmware/dongle_77A85A36_2026-10-23_before_ours/info.json`; 2026-10-23 with our firmware) |
| 0x53F2 → 0x53F3 | **Ours, not stock**: put our firmware's built-in CCFG back (to revive a chip whose bootloader went unreachable), in two steps bound to a magic, the chip's IEEE address and a nonce, the chip's identity fields (a sensor's MAC) kept (`firmware/common/gateway.h` `HOST_OWS_CCFG`) | our extension; `test_ccfg_restore`, `test_hw_ccfg.py` |
| 0x53F4 → 0x53F5, 0x53F6, 0x53F7, 0x53F8 | **Ours, not stock**: OTA updates of our sensors through our gateway, with flag `GW_FLAG_OTA` (offer for one sensor by MAC, the gateway's block requests, the host's 32-byte pieces, the sensor's result); on the air our messages C0–C3, only between our gateway and our sensor. Layouts: `firmware/common/ota.h`; design: `docs/design/ota.md` | our extension; `test_ota_*` in `firmware/tests/test_apps.c` (host board); over the air: `test_hw_ota.py`, the LaunchPad sensor updated through our gateway on the spare dongle (36 KB in 194-196 s from the button press, three runs, 2026-10-24), captured in `host/2026-10-24_ota_launchpad_over_spare_dongle` (`test_ota_capture.py`) |
| A6 bytes 11–15 | **Ours, not stock**: with flag `GW_FLAG_ANNOUNCE` (0x08) our gateway sends `4f 57 01` ("OW", version 1) and its features for sensors (2 bytes BE, bit 0: OTA) where a stock dongle's A6 has zero padding; our sensor keeps them with its join. A stock dongle's A6: bytes 11–15 zero (every captured join) | our extension; `test_gateway_announces_itself`; over the air: our sensor on the LaunchPad paired with our gateway on the spare dongle reported "gateway ours" (`test_hw_ota.py`, 2026-10-24). Stock sensors take them: **verified** for the v2 contact 77BD514D, v1 motion 77A8C793 and climate 77C68193 (each paired with the mark on and reported as usual, the marked A6 seen on the air: `rf/2026-10-24_1042_906.7M_250k_g10_stock_sensors_a6_mark`, `test_a6_mark_capture.py`); leak and v2 motion not tried |
| Our gateway gives a sensor that associates its address from the table, unless another sensor in the table has the same one (a stock table can: known-issues.md §2), then a new one. Before this fix it handed the v1 motion 77A8C793 the address 0x0003 that 77BD514D held (2026-10-24, same capture) | ours; `test_no_shared_address` |
| 0x53F0 → 0x53F1 | **Ours, not stock**: our gateway's feature flags (R3). 4 bytes BE (flags wanted) or empty (ask) → flags on + flags supported (4 + 4 bytes BE); off after every reset | our extension (`firmware/common/gateway.h` `GW_FLAG_*`); a stock dongle ignores it (no `0x53F1`, only the `0x53FF` ACK): `gateway_v1.8` `Uart_processCommands()` default branch `0x032dc`, stock-unknowns.md D6 (static analysis, not tried live) |
| 0x53FA → 0x53FB | **Ours, not stock**: what the firmware is: its 116-byte build info block as it is in the image (`firmware/common/build_info.h`: magic `OWSB`, format, flags TX/debug, the 8-byte image ID, product, board, app, version), for a relay to compare its gateway's image ID with the image it carries (#76); empty if the build has none | our extension; `test_gateway_build_info` (host board), the block and its ID `test_image_id.py` |

Observation on the host side: the user's bridge (wyzesense2mqtt) sets a v1 motion sensor to active when its 0xAB
summary arrives (`rf/2026-10-23_0720_launchpad_bridge`, 778B77D9 at 07:39:39): a host interpretation, nothing the dongle
decides; our gateway forwards 0xAB exactly as the stock one does.

### 9.3 Stock gateway, our sensor (R1)

Our sensor firmware (`firmware/common/sensor.c`, on the LaunchPad) paired with the user's spare stock dongle
77A85A36 as each of the six sensor types and reported (A1, A2, E8), every report forwarded to the host as from
a stock sensor: `test_hw_sensor.py`, `host/2026-10-23_spare_dongle_77A85A36_pairing` (verified).

### 9.3b Our gateway, stock sensor (R1)

The house's stock v2 contact 77BD514D joined our gateway firmware (LaunchPad, test network PAN 0x65FE, the host
side played by `tools/host_link.py --pair` as wyzesense2mqtt does) at its first try, and reported: the first
reports as to the stock dongle (A2 then A1), 12 open/close events by hand, each ACKed by our gateway (~88 ms
after the frame's start, as the house dongle ACKs) and forwarded to the host once, the counter unbroken, also
across a reset of our gateway (the pairing came back from its flash page). Our A4 is byte for byte the stock
dongle's; the sensor's A3/A5 are as to the stock dongle (2026-10-22_1821). `host/2026-10-23_launchpad_gateway_stock_contact_77BD514D`,
`rf/2026-10-23_1640_906.7M_250k_g10_stock_contact_launchpad_gw`, `test_a_stock_sensor_joins_our_gateway`
(verified, one sensor type: v2 contact). The same day the v1 motion 77A8C793 and the climate 77C68193 joined it too
and reported (climate: the join message for message as to a stock dongle in 2026-10-22_1823, A5 included; B0 after
the first A1 to both, none to the v2 contact, as the stock dongle does): `test_v1_motion_and_climate_join_our_gateway`
(verified: v2 contact, v1 motion, v2 climate).

### 9.3c Our gateway on the dongle hardware

Our gateway firmware on the spare dongle 77A85A36 (5x5 CC1310, its CH554, its radio; 2026-10-23): our sensor firmware
(LaunchPad) paired with it as each of the six types and reported, `test_hw_sensor.py` unchanged (written for the stock
dongle), after one fix found on the way: our gateway sent a B0 to a sensor joining again on its old short address
while the type learned from the old sensor (climate: B0 after each poll) was still on record, which broke the join
(`test_pair_again_as_another_type`). The stock dongle passed the same sequence. Then a stock sensor: the house's v2
contact 77BD514D paired with it and reported 9 open/close events, each once, with the same A5 answer as to the stock
dongle (`test_a_stock_sensor_joins_our_firmware_on_the_dongle`). Verified.

### 9.4 Gateway behaviour (model)

`wyzesense_ref.gateway.Gateway` puts §3, §4 and §9 together. On the RF side it is verified against every frame the
house dongle sent (§3). On the host side it follows wyzesense2mqtt's pairing flow (hypothesis 9): scan on → A3
→ 0x5320 → host 0x5321 → A4 → A5 → 0x5322 → host 0x5323 → A6. Driven that way, it reproduces the three
captured joins byte for byte.
