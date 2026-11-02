# Wyze Sense v2 contact sensor (placeholder)

No build rule yet: we have no firmware for it.

Known from the air (FACTS.md §4): sensor type 0x0E, firmware version 0x21, A3 bytes 11-13 `2c 01 00`; after
joining it sends its state (A2) then A1, restarts its counter at 1, and polls ~10 times 0.7 s apart after each
A1 (no B0 for it). The battery byte reads on another scale (0x29 at 44 %). Missing: MCU, firmware dump, pins.
