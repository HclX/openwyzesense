# The hardware interface

What the apps (`apps/`) may use of the hardware, and nothing else: a board implements all of it.

| Header | What |
|---|---|
| `board.h` | power-on init (clocks, console), dynamic console sleep/wake control (`board_keep_awake`, standby gating), LEDs, raw GPIO snapshot (`board_pin_levels`), the MAC a board keeps (`board_mac`, `board_expects_mac`; stock sensors: in CCFG `0x1FFCC`), `board_config.h` |
| `clock.h` | milliseconds since boot; sleeping until a deadline or until something happens |
| `chip.h` | the chip's factory IEEE address, temperature, battery voltage, reset (`chip_reset`), and pinless software entry into the ROM Serial Bootloader (`chip_can_enter_sbl`, `chip_enter_sbl`) |
| `radio.h` | 906.8 MHz receive and transmit |
| `uart.h` | the console / host link, including releasing the UART power/sleep constraint (`uart_sleep`) when the battery console window closes |
| `nv_page.h` | raw read/write/erase access to Flash Page 30 (`0x1E000–0x1EFFF`), backing our `OWNV` log-structured store (`common/nvstore.h`, `docs/nv-format.md`) |
| `ccfg.h` | the chip's CCFG, the one built in, and putting the built-in one back (`common/ccfg_check.h`: what makes a CCFG safe) |
| `boot.h` | dual-slot OTA flash operations (`boot_running_slot`, `boot_running_take_attempt`, `boot_running_confirm`, and inactive-slot erase/write/validation via `common/boot_slot.h`) |
| `input.h` | the sensor's switch (reed / PIR, with configurable pull) and button, as logical levels; edges wake `clock_wait()` (the app debounces) |

Implementations:

- `soc/cc13x0/`: the CC13x0 (TI driverlib, the RF and Power drivers, NoRTOS), with the pins from the board's
  `boards/<board>/board_config.h`.
- `boards/host/`: a simulation, for native tests that run the apps unchanged: devices on one virtual clock,
  sharing an air, each with its own console, flash page, battery and LEDs (`boards/host/host_board.h`).

An app is `<app>_app_init()` and `<app>_app_step()` on a state struct (so a test can run several devices in one
process); `apps/<app>/main.c` runs it on a chip: `for (;;) clock_wait(<app>_app_step(&app));`.
