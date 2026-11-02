# Wyze Sense climate sensor (placeholder)

No build rule yet: we have no firmware for it.

Known from the air (FACTS.md §4): sensor type 0x07, firmware version 0x0F, A3 bytes 11-13 `66 00 00`; E8 when
the reading moves 0.5 degC or 3 % (sampled every 25.86 s), A1 every 120 min, one poll after A1 (answered with
B0); after joining, A1 then E8, the counter continuing. Missing: MCU, firmware dump, pins, the humidity sensor.
