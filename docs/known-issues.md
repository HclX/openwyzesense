# Known bugs in the stock firmware

See [`docs/stock-re/`](stock-re/README.md) for the full reconstructed C source code of the stock sensor and gateway
firmwares (`stock_sensor.c`, `stock_gateway.c`, `stock_common.h`) with flash and SRAM addresses.

## 1. v1 contact sensor sends an all-zero MAC (low battery)

Known to the user from the field: once the battery runs low, a v1 contact sensor's reports carry
`00 00 00 00 00 00 00 00` instead of its MAC, and the bridge cannot attribute them. The damage is permanent across
battery changes: the MAC copy in NVOCTP flash page 30 (`0x1E000`, item 8) is lost while the factory MAC in CCFG
(`0x1FFCC`, flash page 31) remains intact.

Captured 2026-10-22 08:12:49, `ground_truth/rf/2026-10-22_0730_906.7M_250k_g35/snip_081249_205.cu8`
(test vector `ZERO_MAC`, `test_zero_mac_v1_contact`):

| Item | Value |
|---|---|
| MAC frame | data, PAN 0x845B, short address 0x0010 → dongle 0x3366, CRC OK |
| Plaintext | `a1 00 00 00 00 00 00 00 00 01 11 5a 00 01 00 00 0d 00…` |
| Type / sensor type | 0xA1 status / 0x01 (v1 contact) |
| Byte 10 / byte 11 | 0x11 (17 °C) / 0x5A (90 / 32 = 2.8125 V) |
| Counter | 0x000D |
| Dongle reaction | ACKed it and answered the next data request with a 0xB0 time sync; the bridge dropped it |

**A dead unit, dumped and heard (2026-10-24):** the v1 contact `777B42DF`, one of the user's dead units, still has
its MAC in its CCFG (`777B42DF` at `0x1FFCC`), yet pairs and reports with the MAC all zero, its 802.15.4 extended
address included (`firmware/contact_v1_777B42DF_2026-10-24`, `rf/2026-10-24_1147_906.7M_250k_g10_contact_v1_zero_mac_pairs`).
What it lacks is in its NV page (flash page 30, `0x1E000`): no network record (`item 1`) and no `item 8`
(the 25-byte `{xxteaKey[16], mac[8], validFlag}` record); only `item 6` (counting 1, 2, 3) and `item 4` (`0`) are left.

**Root cause in the stock firmware binary (verified in `contact_v1/flash.bin` and `motion_v1/dump.bin`; see
[`docs/stock-re/stock_sensor.c`](stock-re/stock_sensor.c)):**

1. **CCFG `0x0001FFCC` is write-only (`FactoryTest_processUartCmd` at `0x0cd1c` / `0x0cc14`)**:
   - The only reference to `0x0001FFCC` in the entire sensor binary is inside Factory Test UART command `0x08`
     (entered only if the `W` pad `DIO10` is held low at boot). During factory provisioning, command `0x08` writes the
     8-byte ASCII MAC both to CCFG `0x0001FFCC` and to NVOCTP item `(sys=6, item=8, sub=0)`.
   - At normal boot, `Ssf_readMacAndKey()` (`contact_v1` `0x11c50`, `motion_v1` `0x11ec8`) **only reads NVOCTP item
     `(6, 8, 0)`** and never reads `0x0001FFCC`. When NV item 8 is absent, the RAM MAC buffer (`0x20000aac` /
     `0x20000f08`) stays zero-initialized (`00..00`).
2. **Unconditional NVOCTP flash writes on every boot *before* the battery check (`Sensor_init` at `0x07c18` / `0x080ac`)**:
   - At the very start of `Sensor_init()`, **before** `Ssf_checkBootBattery()` (`0x11bc0` / `0x11e3c`) checks whether
     `bat_v32 <= 0x53` (`2.59 V`), the firmware unconditionally executes two NVOCTP writes on **every single boot**:
     1. `Ssf_incrementBootCounter()` (`0x1216c` / `0x123ec`): reads 2-byte NV item `(6, 6, 0)`, increments it by `+1`,
        and writes it back to page 30.
     2. `Ssf_initSeqCounter()` (`0x10278` / `0x10558`): reads 4-byte NV item `(6, 4, 0)`, adds `+25`, and writes it
        back to page 30.
3. **NVOCTP single-page compaction brownout (`NVOCTP_compactPage`)**:
   - As a coin cell nears depletion, brownout resets repeatedly append NV item `6` and `4` records to flash page 30
     (`0x1E000`).
   - Once the 4 KB page fills (~250 brownout resets), the next boot's `updateItem()` triggers `NVOCTP_compactPage()`,
     which erases page 30 (`HapiSectorErase(0x1E000)` draws ~8–10 mA for ~10 ms).
   - The weak coin cell collapses mid-erase, leaving page 30 with a corrupted NVOCTP header.
   - When a fresh battery is later inserted, `NVOCTP_initNvApi()` formats a fresh empty page 30 and `Sensor_init()`
     writes only `item 6` (`1, 2, 3`) and `item 4` (`0`), permanently losing `item 8` (MAC + XXTEA key) and `item 1`
     (network info) — matching the dump of `777B42DF` byte for byte!

Related, for sensors that hang at end of battery without wiping flash: TI's CC1310 errata (SWRZ062F) Advisory 12: when
the supply hovers at the brown-out threshold (~1.78 V, a dying coin cell) a brown-out during boot can lock the chip up
(drawing ~2.25 mA) until a pin reset or a power-down below ~1 V; a fresh battery alone does not end it.

**In our gateway**: as in the stock dongle (stock-unknowns.md D8), a report goes to the host only when its short
address and its MAC both match a sensor in the table, so a zero-MAC report is dropped, never "recovered"; and a sensor
associating with an all-zero address is never paired (#31). A zero-MAC unit is cured only by reflashing it with our
sensor firmware (guide level 2). Why both must match: on the house network (2026-11-03) the dead contact above still reports from 0x0010, an address
the migrated table gives 77C681ED.
Tests: `test_only_paired_sensors`, `test_zero_mac_never_pairs` (`test_gateway.c`), on the captured frame.

**In our firmware** (R2): on a Wyze board that keeps each unit's MAC in its CCFG (`BOARD_MAC_IN_CCFG`, e.g.
`sensor-contact-v1` and `sensor-motion-v1`) the unit's own CCFG stays: the image is written below it
(`<product>-app.bin` or slot images; the CCFG page is not erased), and our firmware reads the MAC directly from CCFG
`0x1FFCC`, refusing anything but 8 ASCII hex digits (an erased or zeroed field is not taken for a MAC). Furthermore,
our sensor firmware keeps the sequence counter in RAM only (#38) and never writes flash on boot (`nvstore.c` only
writes page 30 when joining). See [`docs/design/v1-contact.md`](design/v1-contact.md) for the v1 contact bring-up guide.

## 2. Two radios on one short address

In the 07:30 session, short address `0x0010` is used by two different radios: the zero-MAC v1 contact sensor
(carrier offset +6.4 kHz) and climate sensor `77C681ED` (−0.3 to −0.4 kHz). The dongle ACKs both.
Evidence: `ground_truth/rf/2026-10-22_0730_906.7M_250k_g35/decoded_frames.jsonl` (src 0010, `carrier_khz`).

The stock dongle does hand out addresses already in use: the v1.4 dump's sensor table (flash page 30, FACTS.md §5)
has `777C1FCA` and `7788CCF6` both on `0x0004`, and `7788CD06` and an all-zero-MAC sensor both on `0x0005`.

**Root cause in the stock gateway binary (verified in `gateway_v1.8/FLASH.bin` at `0x0e60c`; see
[`docs/stock-re/stock_gateway.c`](stock-re/stock_gateway.c)):**
- In `Cllc_assocIndCb()` (`0x0e60c..0x0e6a2`), when a sensor sends an 802.15.4 Association Request:
  1. The gateway calls `Csf_getDeviceShortByMac(extAddress)` (`0x14acc -> 0x0e56c`) to check if the 8-byte MAC is
     already in NV item 5. If found, it reuses that `shortAddress`.
  2. If the MAC is **not** in NV item 5 (`0xFFFF`), lines `0x0e638..0x0e640` compute:
     ```c
     uint16_t newShortAddr = Csf_getNumDeviceListEntries() + 1; // [0x20000ac8] + 1
     ```
  3. Because `Csf_getNumDeviceListEntries()` returns the *current* number of active sensors (`NV item 4`) rather than
     a monotonically increasing ID or free-address scan, deleting any sensor other than the last one decrements the
     count and causes the next newly paired sensor to receive `count + 1` — colliding with an existing sensor that
     already holds that short address!

**In our gateway**: a sensor new to our gateway gets the next address after the highest in use (`max + 1`), and a
known sensor whose table address another sensor shares also gets a new one (`test_no_shared_address`), so our gateway
never puts two sensors on one address.

## 3. Stock AES-128-ECB wrapper bugs (16-byte fall-through out-of-bounds read & broken `> 32`-byte decrypt)

Both the sensor (`contact_v1` / `motion_v1`) and the gateway (`gateway_v1.8`) use a static 16-byte AES-128-ECB key
(`2b 7e 15 16 28 ae d2 a6 ab f7 15 88 09 cf 4f 3c` — the NIST FIPS-197 Appendix B test vector key from TI's
`CryptoCC26XX.h` example) stored in `.data` SRAM (`contact_v1` `0x20001944`, `motion_v1` `0x20001988`,
`gateway_v1.8` `0x200014E0`) and never modified at runtime.

Disassembling the four buffer-level AES wrapper functions (`contact_v1` `0x0C5F0` / `0x086BC`, `motion_v1` `0x0C7C0` /
`0x089E0`, `gateway_v1.8` `0x0D5A4` / `0x0924C`; see [`docs/stock-re/stock_common.h`](stock-re/stock_common.h)) reveals
two copy-pasted bugs across all stock firmwares:

1. **Missing `else` fall-through on all 16-byte packets (`contact_v1` `0x0C5D0` / `0x086E8`, `gateway_v1.8` `0x0D5CE` / `0x09278`)**:
   - In both `AES_encryptBuffer(buf, len)` and `AES_decryptBuffer(buf, len)`, the `if (len <= 16)` block encrypts or
     decrypts `buf[0..15]` into the output buffer (`0x2000187C` / `0x200018BC` on `contact_v1`; `0x200013D0` /
     `0x20001410` on `gateway_v1.8`) and **omits the `else` / `return` branch**, falling straight through into the
     `len > 16` (32-byte) path.
   - As a result, every 16-byte packet (`0xA3`, `0xA6`, `0xA7`, `0xB0`, `0xC0`, `0xD0`, `0xD2`, `0xE0`, `0xE2`) executes
     **3 hardware `CryptoCC26XX` transactions instead of 1**, re-processing `buf[0..15]` and reading **16 bytes
     out-of-bounds** past the caller's 16-byte stack buffer (`buf[16..31]`) into the second half of the 32-byte output
     buffer (harmless on the wire only because 16-byte callers copy `16` bytes back).
2. **Broken `> 32`-byte decryption branch (`contact_v1` `0x08770..0x0880A`, `gateway_v1.8` `0x09306..0x0939A`)**:
   - Both `AES_decryptBuffer` routines include a third branch (`if (len > 32)`) intended for 48-byte frames, but set
     `r6 = inBuf + 16` and decrypt `r6` (`inBuf + 16`) **twice** instead of advancing to `inBuf + 32`, then copy both
     `[sp + 0x10]` and `[sp + 0x20]` to the same destination pointer `r4` (`outBuf + 16`) without advancing `r4`. Any
     encrypted frame $> 32$ bytes cannot be decrypted by stock firmware.
3. **Uninitialized stack padding in `motion_v1` `0xA5` (`0x0F764`)**:
   - Unlike `contact_v1` (`0x0EA90`), which calls `memset(sp, 0, 0x20)` before building the 32-byte `0xA5` join
     challenge response, `motion_v1` (`0x0F764`) omits the `memset`, encrypting 5 bytes of uninitialized stack memory
     (`sp[27..31]`, containing `0x2000....` SRAM pointers) into the trailing padding of `0xA5`.

