# Our NV page format (issue #61)

Flash page 30 (0x1E000-0x1EFFF, 4096 bytes) holds a device's persistent data: a gateway's network and sensor table,
a sensor's join (`0x20`) and optional DIY pin/type profile (`0x21`, `NV_CFG`). From #61 on it is in **our own format**, not the stock TI NV-on-chip one. Stock pages are carried
over by the flashing tools, never by the firmware: `tools/nv_json.py export` reads a page (stock or ours), validates
it and writes JSON; `tools/nv_json.py import` writes a page in this format from JSON. The firmware reads only this
format; a page it does not recognise is formatted (data lost, the start never fails: the user's rule, #38).

Implementations: `reference/wyzesense_ref/nvows.py` (Python, the reference), `firmware/common/nvstore.c` (C).

## Layout

All integers little-endian. Everything starts on a 4-byte boundary.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `OWNV` (`4F 57 4E 56`) |
| 4 | 1 | format version: 1 |
| 5 | 3 | `FF FF FF` (reserved) |
| 8 | ... | records, one after the other; the rest of the page `FF` |

A record:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | marker `A5` |
| 1 | 1 | state: `FF` live, `00` deleted (flash clears bits without an erase) |
| 2 | 2 | item |
| 4 | 2 | sub-ID |
| 6 | 2 | data length *n*: 0-64, a multiple of 4 |
| 8 | 4 | CRC-32 (IEEE, as `zlib.crc32`) over bytes 2-7 of this header and the data; the state is not covered |
| 12 | *n* | data |

## Rules

- **Reading** goes forward from offset 8. A marker that is not `A5`, or a length that is not 0-64 and a multiple
  of 4, ends the records. A record whose CRC is wrong (its data cut by a power loss, a flipped bit) is invalid and
  skipped; the records after it still count. Of the good records, the last live one of each (item, sub-ID) is its
  value.
- **Writing** appends the record right after the last one (header, then data: cut half-way, its CRC fails and it is
  skipped; if `nv_page_write()` fails mid-append, `nv->end` still advances past the dirty bytes so the next write
  compacts first), then clears the state of the older live copies of that (item, sub-ID).
- **On opening**, if anything but `FF` follows the records (a write cut inside its header), the page is compacted at
  once, so that new records never land behind bytes that end the reading.
- **Deleting** clears the state of the live copies.
- **A full page** is compacted: `compact()` stages all live records into a temporary stack buffer (occupying zero
  permanent `.bss` SRAM). If the live records do not all fit in the staging buffer, `compact()` aborts and returns
  `false` **before** erasing page 30, never silently dropping live records. Otherwise page 30 is erased and formatted,
  and the staged live records are written back oldest first.
- **A page with another magic or version** (stock, erased, garbage) is erased and formatted on the next start.

## Items

The data of each item is what it was in the stock format, so the apps did not change with the container.

| Item | Sub-ID | Size | Who | Data |
|---|---|---|---|---|
| 1 | 0 | 16 | gateway | network: PAN (2), coordinator short address (2), IEEE address (8), `17 00 BE BE` (as on the stock dongles; meaning open) |
| 4 | 0 | 4 | gateway | number of sensors (1), `00 00 00` |
| 5 | *n* | 24 | gateway | sensor *n*: PAN (2), short address (2), ASCII MAC (8), 12 bytes: byte 17 `01`, the rest `00` (as stock) |
| 0x20 | 0 | 12 | sensor | its join: type, flags (bit 0: the gateway is ours), PAN (2), coordinator (2), short address (2), `00 00` (the message counter was here; RAM only since #38), the gateway's features (2) |
| 0x21 | 0 | 8 | sensor | DIY pin/type profile (`NV_CFG`): `sensor_type` (`00` = board default, `01` v1 contact, `02` v1 motion, `03` v2 leak, `07` climate, `0E` v2 contact, `0F` v2 motion), `flags` (bit 0: `invert` `INPUT_EVENT` polarity), `pull` (`00` up, `01` none, `02` down, `FF` board default), `00`, `debounce_ms` (LE16, `0` = default 40 ms, `1..5000`), `00 00` |

## JSON (`tools/nv_json.py`)

```json
{
  "format": "openwyzesense-nv-json", "version": 1,
  "source": {"layout": "stock" | "ows" | "blank"},
  "network": {"pan": "65FE", "short": "D425", "ext": "00124B0012345678"},
  "sensors": [{"sub": 1, "mac": "77A8C793", "short": "0005"}],
  "join": {"type": "01", "gw_ours": true, "pan": "65FE", "coord": "D425", "short": "0006", "gw_features": "0001"},
  "cfg": {"type": "03", "invert": true, "pull": "up", "debounce_ms": 100},
  "dropped": [{"what": "sensor 5", "why": "shares short address 0004 with 777C1FCA: re-pair 7788CCF6 after the migration", "mac": "7788CCF6"}],
  "unrecognised": [{"item": 6, "sub": 0, "data": "01000000"}]
}
```

`network`, `sensors`, `join` and `cfg` are imported; `dropped` lists what validation refused and why; `unrecognised` lists
records of a stock page whose meaning is not known (a stock sensor's: its join's layout is not verified yet), kept
for the record, not imported. A stock v1 motion's page (header `00 00 00 00`) is laid out differently and reads as
unrecognised: its units are wiped when flashed (the user's decision 4 on #61). Validation on export:

- network: PAN not 0000 / FFFF, coordinator short address not FFFE / FFFF;
- sensors: an ASCII MAC of 8 hex digits (not all zeros), a short address 0001-FFFD, the network's PAN; one per MAC
  and one per short address (the lowest sub-ID kept; a stock table can hold two on one address, #12, and the other
  is listed under `dropped` with its `mac`, to be re-paired after the migration: the user's decision, #90);
- join: PAN not 0000 / FFFF, addresses as above (any other PAN, 845B included: #70).
