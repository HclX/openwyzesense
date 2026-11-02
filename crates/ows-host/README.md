# ows-host

The OpenWyzeSense host tools in Rust (#42), for programs that install, update and restore our firmware on a Wyze USB
dongle (wyzesense2mqtt-rs's dashboard). A port of `tools/dongle_sbl.py` and `tools/nv_json.py`, with the same steps and
safety checks; `ows-flash` (crates/ows-flash) is its command line.

```toml
ows-host = { git = "https://github.com/HclX/openwyzesense", tag = "v0.1.0" }
```

The tag `vX.Y.Z` is the crate's version, and its GitHub release holds the firmware and `manifest.json`.

## API

- `HidTransport`: the caller owns the device. `write(report)` sends these bytes exactly (no report ID); `read(buf,
  timeout)` returns one report (up to 63 bytes, the first one the payload length), or an error of kind `TimedOut`.
  `hidraw::Hidraw` and `hidraw::find_dongle()` (feature `hidraw`, default, Linux) do this for `/dev/hidrawN`.
- `dongle::backup(&mut t, &mut progress) -> Backup`: the whole chip (flash, CCFG, FCFG1), each read twice and checked
  with the chip's CRC32; then a reset. Nothing written. `Backup::save(dir)` / `Backup::load(dir)`: the directory
  `dongle_sbl.py backup` writes (`info.json`, `flash.bin`, `ccfg.bin`, `fcfg1.bin`); either reads the other's.
- `dongle::flash(&mut t, image, &backup, &FlashOptions { fast, nv }, &mut progress) -> FlashReport`: our image
  (`gateway-dongle-app.bin`, checked by its build info) to pages 0-29, then page 30 as `nv` says:
  `NvChoice::Migrate` (the default: the current page exported, validated, imported in our format), `Wipe`, `Keep`,
  `Json(doc)`. `FlashReport` has the page as it was (`nv_before`) and the document carried over (`nv_doc`, with
  `dropped` and why: show it, keep it with the backup).
- `dongle::restore(&mut t, &backup, fast, &mut progress)`: the backup's pages 0-30 back (stock again).
- `dongle::inquiry(&mut t) -> bool`: on a freshly opened device after the reset, whether the dongle answers in normal
  mode; if not, it needs a replug.
- `Progress { phase, done, total }`: `Enter`, `Backup`, `Check`, `Erase` (pages), `Write`, `Verify`, `Nv` (bytes),
  `Reset`, `Done`.
- `nv::export(page) -> NvDoc`, `nv::import(&doc) -> page`, `NvDoc::{to_json, from_json, summary}`: page 30 (stock or
  ours) and the JSON model (`docs/nv-format.md`).
- `image::check_product(image, "gateway-dongle")`, `image::build_info(image)`: the build info block (product, app,
  board, version, image ID).
- `manifest::Manifest::from_json(text)`, `.product("gateway-dongle")`, `ManifestFile::verify(bytes)`: a release's
  `manifest.json` and the SHA-256 of a downloaded file.
- `Error`: `Io`, `Sbl`, `Refused` (a safety rule), `Verify` (the chip was not reset), `Backup`, `Image`, `Nv`,
  `Manifest`.

## Tests of programs that use it: feature `sim`

```toml
[dev-dependencies]
ows-host = { git = "https://github.com/HclX/openwyzesense", tag = "...", features = ["sim"] }
```

- `sim::SimDongle`: a `HidTransport` that behaves as a Wyze dongle: update mode (0x4312), TI's ROM bootloader over a
  128 KB flash, Inquiry (0x4327 -> 0x4328) in normal mode, back in normal mode after the bootloader's reset.
  `SimDongle::stock()` (stock code, the spare dongle's stock table on page 30), `SimDongle::ows(version)` (as a flash
  leaves it), `SimDongle::with_flash(bytes)`. Failures on demand: `needs_replug` (silent after the reset until
  `replug()`), `corrupt_at` (a write that does not verify), `fail_io` (the device gone), `chip_id` (another chip).
  Inspect `flash`, `erased`, `resets`, `log`, `nv_page()`, `runs_ows()`, `ows_version()`. It panics on a command the
  crate must never send.
- `sim::gateway_image(version)`: an image `image::check_product(_, "gateway-dongle")` accepts (not a firmware that
  runs); `sim::product_image(product, version)` for others; `sim::stock_flash()`.

## Safety

- Page 31 (code and the CCFG, whose `BL_CONFIG` keeps the bootloader reachable) is never erased or written. Page 30 is
  written only by the NV write. Every erase and download address is checked at the last step before the chip.
  Bank erase, memory write and set-CCFG are not in the crate.
- Flashing and restoring need the backup of this chip: the same chip ID, page 31 on the chip equal to the backup's, and
  the backup's own stock `BL_CONFIG`.
- The chip is reset only after the chip's CRC32 matches (and a read-back unless `fast`), pages 30-31 are unchanged,
  and page 31 is unchanged after the NV write. On an error the dongle stays in update mode: flash or restore again,
  or replug it.

## Tests

`cargo test`: `tests/dongle.rs` runs backup, flash and restore against a simulated dongle (as
`reference/tests/test_dongle_sbl.py` does). `tests/golden.rs` holds export and import to
`reference/tests/vectors/nv/golden`, which `tools/nv_golden.py` writes from the Python reference.
