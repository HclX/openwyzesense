# Sensor low-battery detection & zero-hardware ESP32 bench verification: proposal

Design and verification plan (`TASK-25`, future work) for under-load coin-cell low-battery detection on CC1310 sensors
(`wyze_contact_v1`, `wyze_motion_v1`) and a zero-extra-hardware Hardware-in-the-Loop (HIL) test harness using only an
ESP32 board (`LOLIN32` or `ESP32-S3`) wired to a `wyze_contact_v1` board (`CR2450` removed).

---

## 1. Motivation & What Stock Firmware Got Right (and Wrong)

A cold or 80%-depleted `CR2450` / `CR2032` lithium coin cell recovers to **`2.85–2.95 V` open-circuit** while a CC1310
sleeps in `STANDBY` at `~1 µA`, and only reveals its age (`30–80 Ω` internal ESR) when a `10–20 mA` radio burst pulls
`VDDS` down by `300–600 mV`.

From stock binary disassembly (`docs/FACTS.md` §4.1 & §5, `docs/stock-unknowns.md` `R5` & `Z1`,
`docs/stock-re/stock_sensor.c`):

- **What stock v1 firmware got right (`Ssf_sampleBatteryPostTx`, `0x11c14` / `0x11e90`)**:
  - Samples `AONBatMonBatteryVoltageGet() >> 3` (`1/32 V` units, `pt[11]`) **immediately after every radio TX** so
    reports carry voltage **under load**, not open-circuit recovery voltage.
  - Requires **3 consecutive post-TX samples `< 0x54` (`2.625 V`)** before setting the `pt[12] = 0x01` low-battery flag
    in `0xA1` / `0xA2` reports, filtering out single cold-morning transients.
  - Blinks `5 × 200 ms` at boot when `bat_v32 <= 0x53` (`2.59375 V`, `Ssf_checkBootBattery` at `0x11bc0` / `0x11e3c`).
- **What stock v1 firmware got fatally wrong**:
  1. **Zero-MAC brownout wipe (`Z1`)**: `Sensor_init()` unconditionally writes NVOCTP items `(6, 6, 0)` and `(6, 4, 0)`
     on Flash Page 30 *before* checking battery voltage, triggering a `FlashSectorErase(0x1E000)` compaction mid-brownout
     that wipes the sensor's MAC in NV item `8`.
  2. **Irreversible NV latch**: Stock latches `nv11LowBatLatch = 1` in NVOCTP item `(6, 11, 0)` and never clears it on
     battery replacement unless factory reset.

### Current state in OpenWyzeSense (`firmware/apps/sensor/sensor_app.c`)

- **Already implemented**:
  - MAC is read directly from read-only CCFG (`0x1FFCC`) with zero boot-time NV writes; `OWNV` (`nvstore.c`) stages
    compactions safely.
  - Boot-time warning blink (`5 × 200 ms`) when `chip_battery_mv() <= BOOT_LOW_BAT_MV` (`2590 mV`, `sensor_app.c:735`).
  - Pre-flight (`ota_offer`) and per-write (`ota_write`) OTA guards at `OTA_MIN_MV` (`2500 mV`, `sensor_app.c:268, 285`).
- **Done (#75, 2026-11-01)**:
  1. **Sampled during the frame, not after it**: `radio_tx()` (`soc/cc13x0/radio.c`) starts the frame with
     `RF_postCmd()`, switches BATMON to continuous measurement, and keeps its lowest reading while the RF command is
     ACTIVE (the PA on; a report is ~86 ms on the air): `radio_tx_min_mv()` (HAL; the host board models a sag,
     `tx_sag_mv`). Stock samples *after* the frame (`Ssf_sampleBatteryPostTx`), when the cell and the board's
     capacitors may already have recovered: a likely reason stock sensors report ~3 V and then die (the user).
  2. **Reports carry the loaded reading**: byte 11 is the lowest of the current exchange (frames within 10 s); the idle
     reading only until the first frame. Byte 12 is the low-battery flag: set after 3 loaded readings in a row below
     2625 mV (stock), cleared by one at 2800 mV or above (a new cell; stock latches it in NV forever). The OTA gate
     uses the lower of idle and the last frame's loaded reading. Console: `bat` (`bat idle <mV> loaded <mV> low <0|1>`).
     Test: `test_battery_under_load` (`test_apps.c`).
  3. **Our gateway dropped low-battery reports**: it (and `host_alarm_payload`, `app_parse_report`, the Python
     reference) read bytes 12-13 as one 16-bit length, so a report with the flag set looked 256 bytes long and was
     dropped: stock sensors on our gateway lost their reports exactly when their battery got low. Byte 13 alone is the
     length (FACTS.md, verified in both v1 binaries).
  4. **Flash operations too** (the user): `chip_flash_begin()` / `chip_flash_end()` around every NV-page and OTA-slot
     erase and write (`nv_flash.c`, `boot_hal.c`): BATMON continuous, read the moment the ROM routine returns (it runs
     with interrupts off and cannot be sampled while it runs). `chip_flash_mv()` / `chip_flash_min_mv()`, on `bat`.
  5. **On hardware** (2026-11-01, the battery rig B15C + 777B42DF with this firmware, paired with 9A78 on 0x65FE):
     - steady 3.3 V supply: idle 3273 mV, under load 2960-3007 mV (the ESP32 pin's own ~20 Ω at ~13 mA);
     - a simulated weak cell (rig window 2.70-2.75 V, no hold above 2.0 V, 10 µs pulses): **idle 2738-2781 mV, under
       load 2113-2246 mV**, reported as 0x47 / 0x46 / 0x43 (2.22 / 2.19 / 2.09 V) where an idle sample says ~2.75 V;
       the flag after the third low frame; a flash operation (the join restored after a failed re-pair) read 2378 mV;
     - the flagged report was **dropped by 9A78 and the dongle on the old gateway (40f8584)** and forwarded with
       `low 0x01` once 9A78 ran this build.
     Two bugs found there: the TX status still held the last frame's DONE when the loop started (a frame right after
     another, e.g. the ACK after an A1, measured nothing: now reset to IDLE before posting), and BATUPD flags only a
     changed reading (BAT is now read all along).
- **Gaps to close (`TASK-25`)**:
  1. How stock's after-the-frame sample compares on the same weak cell (777B42DF's stock image is in ground_truth).
  2. Non-essential `OWNV` writes (`save()`, `save_cfg()`) do not yet check the battery (`NV_MIN_MV`, `2500 mV`).

---

## 2. Proposed Firmware Changes

1. **Fast Under-Load `AON_BATMON` Sampling (`firmware/soc/cc13x0/chip.c`, `firmware/apps/sensor/sensor_app.c`)**:
   - Add a `chip_battery_mv_fresh()` helper (or set `AON_BATMON_O_MEASCFG = AON_BATMON_MEASCFG_PER_CONT` around active
     TX / load measurement and restore `PER_32CYC` afterward) so `AON_BATMON_O_BAT` reflects `VDDS` during the tail of
     each transmission (`AONBatMonNewBatteryMeasureReady()`).
   - Call `read_batmon_post_tx()` immediately inside `radio_send()` (`sensor_app.c:198`) right after `radio_tx()` returns
     (matching stock `Ssf_sampleBatteryPostTx`), while keeping periodic idle updates only when `VDDS` rises (e.g. battery
     replacement) so idle recovery never overwrites a lower post-TX reading within the same report window.
2. **Hysteretic `pt[12]` Low-Battery Flag with Auto-Clear (`firmware/common/sensor.h`, `firmware/common/sensor.c`)**:
   - Track `uint8_t low_bat_streak` and `bool low_bat` on `sensor_t`:
     - Increment `low_bat_streak` (up to `3`) whenever a post-TX sample is `< LOW_BAT_WARN_MV` (`2625 mV`, `0x54`); set
       `s->low_bat = true` at `3` consecutive sags and emit `pt[12] = 0x01` in `0xA1` / `0xA2` / `0xE8` reports.
     - Reset `low_bat_streak = 0` on any post-TX sample `>= LOW_BAT_WARN_MV`, and **auto-clear `s->low_bat = false`** on
       boot or post-TX when `mv >= LOW_BAT_CLEAR_MV` (`2800 mV`, fresh battery installed) — never permanently bricking a
       unit in NV like stock NV item 11.
     - Blink `5 × 200 ms` instead of `3 × 400 ms` at the `3.04 s` button-hold mark when `s->low_bat` or boot low-battery
       is active (matching stock `Ssf_buttonClockCb`, `docs/stock-unknowns.md` `B2`/`B4`).
3. **Brownout NV Write Guard (`sensor_app.c`)**:
   - Refuse `nv_write()` / page-30 compaction in `save()` and `save_cfg()` when `chip_battery_mv() < NV_MIN_MV`
     (`2500 mV`), keeping runtime state in SRAM without risking a flash sector erase on a collapsing cell.
4. **Passive Bench Console Hooks (`bat` and `load <ms>` in `sensor_app.c`)**:
   - `bat`: prints `bat <mv> raw <0xXX> temp <degC> low <0|1>\r\n` for closed-loop HIL readback.
   - `load <ms>` (`1..100 ms`): runs a **100% RF-silent** internal current pulse (`radio_rx_start()` `~6.8 mA` + LED on
     `~3.5 mA` + CPU spin `~3.2 mA` ≈ **`13.5 mA`** total on `VDDS` with **zero 906.8 MHz transmissions**), samples
     `chip_battery_mv_fresh()` at the end of the pulse, feeds the post-load reading into the low-battery detector, and
     prints `load idle <mv0> loaded <mv1> low <0|1>\r\n`. This allows full ESR-droop regression testing without
     requiring `OWS_HW_TX=1` or transmitting near the house network.

---

## 3. Zero-Extra-Hardware HIL Test Rig (`ESP32` + `wyze_contact_v1` Only)

Goal: test boot thresholds, under-load coin-cell ESR droop, LED warning blinks, OTA `< 2.50 V` refusal/abort, and
500-cycle brownout NV survival using **only** a `wyze_contact_v1` (`CR2450` removed) and an ESP32 dev board connected by
jumper wires (`+`, `-`, `R`, `T`, `S`, `W`) — no bench PSU, external DAC, op-amp, resistors, or capacitors.

### 3.1 Prerequisite: Eliminating 3.3 V ESD Clamp Back-Powering

Every CC1310 GPIO has an internal upper ESD protection diode to `VDDS` ($V_F \approx 0.55\text{ V}$). If `+` (`VDDS`) is
lowered to `2.20 V` while any connected ESP32 pin drives push-pull `3.3 V`, that pin forward-biases the ESD diode and
clamps `VDDS` up to $\approx 2.75\text{ V}$ (observed on 2026-10-24, `docs/FACTS.md` §5 line 193).

**Pure-software pin configuration on the ESP32 (`esp32/idf/main/contact.c` / bench mode)**:
- **`S` (`RESET_N`) and `W` (`DIO10`)**: Already `GPIO_MODE_OUTPUT_OD` with `GPIO_PULLUP_DISABLE` (`contact.c:18`); both
  pull up to the contact board's *own* lowered `VDDS`.
- **ESP32 `UART1 TX` → Contact `R` (`DIO1`)**: Switch ESP32 TX to **`GPIO_MODE_OUTPUT_OD` + `GPIO_PULLUP_DISABLE`**
  after `uart_set_pin()`. Because CC1310 `uart_init()` (`firmware/soc/cc13x0/uart.c:62`) already sets `IOC_IOPULL_UP` on
  `DIO1`, the RX line idles at CC1310's **own `VDDS`** (`1.9–3.3 V`) with zero `3.3 V` back-feeding.
- **Contact `T` (`DIO0`) → ESP32 `UART1 RX`**: Call `gpio_pullup_dis(rx)` during low-voltage tests so ESP32's `~45 kΩ`
  pull-up does not source `~15 µA` into a sleeping sensor. (ESP32's CMOS input threshold at `3.3 V` is `~1.65 V`, so
  `115200` 8N1 from a `2.0–2.6 V` CC1310 still receives cleanly.)

### 3.2 Synthesizing Programmable `VDDS` (`1.7 V – 3.3 V`) + Coin-Cell ESR

Move the contact's `+` wire from fixed `3V3` to an ESP32 GPIO, relying on the `wyze_contact_v1` PCB's **existing onboard
`VDDS`/`VDDR`/`DCOUPL` MLCC decoupling capacitors** (`~5–10 µF`) for high-frequency filtering:

| Rig Variant | ESP32 Board | Wiring on `+` (`VDDS`) | How It Works |
|---|---|---|---|
| **Option A (Recommended)** | **Classic ESP32 (`LOLIN32`)** | `GPIO25` (`DAC_CHAN_0`) → `+` *(optional: jumper `GPIO26` `DAC_CHAN_1` in parallel for `2×` current, + `GPIO34` `ADC1` for rail sensing)* | Uses ESP32's true 8-bit analog R-string DAC (`dac_oneshot_output_voltage`, `~13 mV`/step). The internal DAC buffer's natural **`~50–100 Ω` output impedance** (`~25–50 Ω` with `DAC1∥DAC2`) acts as a realistic dying-`CR2450` ESR (`30–80 Ω`), holding steady `VDDS` in standby/idle while automatically drooping `300–600 mV` during `load <ms>` or RF TX bursts. |
| **Option B (Universal / S3)** | **ESP32-S3** *(or any ESP32 without DAC)* | Single `ADC1`-capable GPIO (e.g. `GPIO1`) → `+` | **"Open-Source Pulse-Skipping LDO"** on Core 1: **never drives hard `0`** (which would short the contact's bypass caps). Switches strictly between `1–5 µs` `HIGH` pulses at weakest FET drive (`GPIO_DRIVE_CAP_0`, `~130 Ω`) when `ADC1 < V_target` and `Hi-Z` when `ADC1 >= V_target`. Uses internal `~45 kΩ` pull-down (`gpio_pulldown_en`) *only* to bleed caps gently downward ($\tau \approx 0.45\text{ s}$). Capping max pulse rate synthesizes tunable ESR. |
| **Option C (Zero-Loop Smoke Check)** | Any ESP32 | `+` left floating (on `ADC1` input); `W` (`DIO10`) driven weak `HIGH` after boot | Uses CC1310's upper ESD diode drop ($3.3\text{ V} - 0.6\text{ V} \approx 2.65\text{ V}$ idle, sagging `< 2.50 V` under passive `load`) for a 2-line threshold sanity check (`< 10 mA` passive load only). |

### 3.3 Self-Calibration & Power-Line LED Blink Sensing

1. **Closed-loop `AON_BATMON` targeting (no external voltmeter needed)**:
   - CC1310's `AON_BATMON` (`3.9 mV` LSB) is factory-trimmed in `FCFG1`. The harness sends `bat\n` over UART and
     steps ESP32's DAC / pulse target until CC1310 itself reports the desired millivolts (`±25 mV`), cancelling ESP32
     DAC/ADC offset and IR drop automatically.
2. **Counting LED blinks through the `+` power wire (no wire soldered to `DIO9`)**:
   - When CC1310 toggles its red LED on `DIO9` (`~3.5 mA` load step), `VDDS` dips by `~150–250 mV` across ESP32's weak
     source impedance (`DAC` or `GPIO_DRIVE_CAP_0`). Sampling `ADC1` on `+` at `500 Hz` lets the ESP32 **count LED
     blinks and measure their `200 ms` vs. `250 ms` / `400 ms` half-periods** over the supply wire alone.

---

## 4. Planned Automated Verification (`TASK-25`)

1. **Host Unit & Virtual-Air Tests (`ctest --preset host` — `test_sensor`, `test_apps`)**:
   - Verify 3-sample `< 2625 mV` post-TX streak sets `pt[12] = 0x01`, single transient sag `< 2625 mV` followed by
     `>= 2625 mV` keeps `pt[12] = 0x00`, and `>= 2800 mV` clears `pt[12]` back to `0x00`.
   - Verify `button` `3.04 s` hold blinks `5 × 200 ms` when `low_bat` is active, `NV_MIN_MV` blocks `OWNV` writes below
     `2500 mV`, and `bat` / `load` console commands behave deterministically on the host board.
2. **Zero-Hardware ESP32 Bench HIL Suite (`reference/tests/test_hw_low_battery.py`, opt-in `OWS_HW=1 OWS_HW_LOWBAT=1`)**:
   - **Sweep & back-power check**: Command `3100`, `2700`, `2580`, `2450`, and `2100 mV`; assert CC1310 `bat` settles
     within `±60 mV` (confirming zero `3.3 V` UART clamp leakage).
   - **Boot warning blink**: Reset sensor via `S` at `2950 mV` (assert `1 × 250 ms` blink via `+` ADC dip) vs. `2500 mV`
     (assert `5 × 200 ms` blinks via `+` ADC dips).
   - **RF-silent ESR sag (`load 30`)**: Park open-circuit `VDDS` at `2850 mV`, trigger 3 `load 30` pulses so loaded
     `VDDS` sags `< 2625 mV` with zero RF TX, assert `low 1` (`pt[12] = 0x01`), then raise `VDDS` to `3050 mV` and
     assert auto-recovery (`low 0`).
   - **OTA pre-flight & mid-flight brownout**: Verify `ota_offer` refuses at `2450 mV`, and verify dropping `VDDS` from
     `2850 mV` → `2420 mV` mid-transfer aborts `ota_write` cleanly without corrupting the active slot.
   - **500-cycle anti-Zero-MAC brownout torture**: Sawtooth `+` across `1700 mV` (below CC1310 `~1.80 V` BOD reset) ↔
     `2550 mV` 500 times on a provisioned/joined sensor, restore `3100 mV`, and assert via `info` that CCFG MAC
     (`0x1FFCC`), `OWNV` `NV_SENSOR_JOIN`, and `NV_CFG` remain 100% intact.
