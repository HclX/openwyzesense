# Wyze Sense Climate Sensor (`wyze_climate`)

Stock firmware dumped and reverse-engineered: `backups/sensor_77C68193_2026-11-05/flash.bin` (see [`docs/stock-re/stock_climate.c`](../../docs/stock-re/stock_climate.c)).

## Hardware

- **MCU**: Texas Instruments **CC1310F128** in 5×5 mm RHB (32-pin QFN) package (`FCFG1 USER_ID = 0x20018000`).
- **Sensor**: Sensirion **SHTC3** Temperature & Relative Humidity sensor over I2C at address `0x70`.
  - Formulas: $T = -45 + 175 \times (S_T / 65536)$, $RH = 100 \times (S_{RH} / 65536)$.
- **Status LED**: `DIO8` (active-high, low at boot).
- **Pairing Button**: `DIO12` (active-low, internal pull-up, IRQ on edges). Hold 3 s for 3 slow blinks and join reset.
- **I2C SDA**: `DIO9` (to SHTC3).
- **I2C SCL**: `DIO10` (to SHTC3).
- **Factory Test Strap ('W' pad)**: `DIO6` (internal pull-up; grounded at boot for Factory UART mode).
- **UART TX Test Pad**: `DIO0`.
- **UART RX Test Pad**: `DIO1` (also the CCFG ROM Serial Bootloader backdoor pin: `BL_CONFIG = 0xC5FE01C5`).
- **JTAG Test Pads**:
  - `M` pad = Pin 13 (`TMSC`)
  - `C` pad = Pin 14 (`TCKC`)
  - `S` pad = Pin 19 (`RESET_N`)

## Stock Behavior

- **Sensor Type**: `0x07` (`APP_TIME_SYNC 0xB0` downlink filter enabled).
- **Firmware Version**: `0x0F` (15).
- **Heartbeat (`0xA1`)**: Sent every 120 minutes (2 hours).
- **Climate Report (`0xE8`)**: Sent when reading moves $\ge 0.5\ ^\circ\text{C}$ or $\ge 3\ \%$ RH from the last report (sampled every 25.86 s).
