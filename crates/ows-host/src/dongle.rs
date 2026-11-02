//! The Wyze USB dongle: backup, flash and restore through its ROM bootloader, as the Python `dongle_sbl.py` (replaced by this, #44) does, with
//! all of its guards:
//!
//! - page 31 (code and the CCFG with BL_CONFIG) is never erased or written; page 30 (NV) only by the NV write;
//! - flashing needs the backup of this chip: same chip ID, and page 31 on the chip equal to the backup's;
//! - what was written is verified (the chip's CRC32, and a read-back unless `fast`), pages 30-31 are checked
//!   unchanged (page 31 again after the NV write), and only then is the chip reset;
//! - the backup reads everything twice and checks the chip's CRC32.
//!
//! The dongle stays in update mode if anything fails (no reset): try again, or unplug it, which starts whatever is in
//! flash; a failed write leaves pages 0-29 incomplete, and the dongle then only starts its bootloader (BL_CONFIG is
//! untouched), so flash or restore again.

use std::path::Path;
use std::time::Instant;

use serde_json::{json, Value};

use crate::image::{self, BuildInfo};
use crate::manifest::sha256_hex;
use crate::nv::{self, NvChoice, NvDoc};
use crate::sbl::{self, Sbl, APP_END, FLASH_SIZE, NV_PAGE, PAGE};
use crate::{Error, HidTransport};

/// The dongle's product (firmware/products.cmake), as its images' build info names it.
pub const PRODUCT: &str = "gateway-dongle";
/// Its board directory (firmware/boards/), as the release manifest names it.
pub const BOARD: &str = "wyze_dongle";
/// A stock dongle's BL_CONFIG (CCFG at 0x1FFD8): the bootloader on, as the backup must show.
pub const STOCK_BL_CONFIG: [u8; 4] = [0xC5, 0x01, 0xFE, 0xC5];
const BL_CONFIG_AT: usize = 0x1FFD8;
const PAGE31: usize = 0x1F000;
const RESETCTL: u32 = 0x4009_0004;
const REGIONS: [(&str, u32, usize); 3] =
    [("flash.bin", 0x0000_0000, 0x20000), ("ccfg.bin", 0x5000_3000, 0x1000), ("fcfg1.bin", 0x5000_1000, 0x3F0)];
const RESET_SRC: [&str; 8] = [
    "power on",
    "pin reset",
    "VDDS loss",
    "VDD loss",
    "VDDR loss",
    "clock loss",
    "system reset (software)",
    "warm reset",
];

/// What is going on, for a progress bar: `done` of `total` (bytes, or pages for Erase).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Progress {
    pub phase: Phase,
    pub done: usize,
    pub total: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    /// Update mode and the bootloader.
    Enter,
    /// Reading a backup (each region twice).
    Backup,
    /// Checking the chip against the backup.
    Check,
    Erase,
    Write,
    /// CRC32 and read-back.
    Verify,
    /// Reading, converting and writing page 30.
    Nv,
    Reset,
    Done,
}

/// A whole-chip backup: what the Python `dongle_sbl.py backup` wrote to its directory (info.json, flash.bin, ccfg.bin,
/// fcfg1.bin); either reads the other's.
#[derive(Debug, Clone)]
pub struct Backup {
    /// info.json: chip ID, reset source, and each file's address, size, SHA-256 and chip CRC32.
    pub info: Value,
    /// The 128 KB flash: pages 0-29 the stock firmware, 30 its NV table, 31 code and the CCFG.
    pub flash: Vec<u8>,
    pub ccfg: Option<Vec<u8>>,
    pub fcfg1: Option<Vec<u8>>,
}

impl Backup {
    pub fn chip_id(&self) -> &str {
        self.info["chip_id"].as_str().unwrap_or("")
    }

    /// Page 30 as backed up (the stock sensor table).
    pub fn nv_page(&self) -> &[u8] {
        &self.flash[NV_PAGE as usize..(NV_PAGE + PAGE) as usize]
    }

    /// As dongle_sbl.py's check_backup() did: 128 KB, its SHA-256 as info.json says, and the stock BL_CONFIG.
    pub fn check(&self) -> Result<(), Error> {
        let want = self.info["flash.bin"]["sha256"].as_str().unwrap_or("");
        if self.flash.len() != FLASH_SIZE as usize || sha256_hex(&self.flash) != want {
            return Err(Error::Backup("bad backup: flash.bin is not the one info.json describes".into()));
        }
        if self.flash[BL_CONFIG_AT..BL_CONFIG_AT + 4] != STOCK_BL_CONFIG {
            return Err(Error::Backup("the backup's BL_CONFIG is not the stock one".into()));
        }
        if self.chip_id().is_empty() {
            return Err(Error::Backup("info.json has no chip_id".into()));
        }
        Ok(())
    }

    /// Reads and checks a backup directory.
    pub fn load(dir: &Path) -> Result<Backup, Error> {
        let info: Value = serde_json::from_slice(&std::fs::read(dir.join("info.json"))?)
            .map_err(|e| Error::Backup(format!("info.json: {e}")))?;
        let b = Backup {
            info,
            flash: std::fs::read(dir.join("flash.bin"))?,
            ccfg: std::fs::read(dir.join("ccfg.bin")).ok(),
            fcfg1: std::fs::read(dir.join("fcfg1.bin")).ok(),
        };
        b.check()?;
        Ok(b)
    }

    /// Writes the directory (created if need be); never over an existing backup's flash.bin.
    pub fn save(&self, dir: &Path) -> Result<(), Error> {
        std::fs::create_dir_all(dir)?;
        if dir.join("flash.bin").exists() {
            return Err(Error::Backup(format!("{} already holds a backup", dir.display())));
        }
        std::fs::write(dir.join("flash.bin"), &self.flash)?;
        if let Some(c) = &self.ccfg {
            std::fs::write(dir.join("ccfg.bin"), c)?;
        }
        if let Some(f) = &self.fcfg1 {
            std::fs::write(dir.join("fcfg1.bin"), f)?;
        }
        std::fs::write(dir.join("info.json"), serde_json::to_string_pretty(&self.info).unwrap() + "\n")?;
        Ok(())
    }
}

/// Reads the whole chip (flash, CCFG, FCFG1; each twice, compared and checked against the chip's CRC32) and the
/// reset source, then resets the chip. Writes nothing.
pub fn backup<T: HidTransport + ?Sized>(t: &mut T, progress: &mut dyn FnMut(Progress)) -> Result<Backup, Error> {
    progress(Progress { phase: Phase::Enter, done: 0, total: 0 });
    let mut s = Sbl::enter(t)?;
    let mut info = serde_json::Map::new();
    info.insert("time".into(), json!(utc_now()));
    info.insert("update_mode_reply".into(), json!(hex::encode(&s.update_mode_reply)));
    info.insert("chip_id".into(), json!(s.chip_id_hex()));
    match s.read(RESETCTL, 4, &mut |_| {}) {
        Ok(r) => {
            let rc = u32::from_le_bytes(r[..4].try_into().unwrap());
            info.insert("resetctl".into(), json!(format!("{rc:#010x}")));
            info.insert("reset_source".into(), json!(RESET_SRC[((rc >> 1) & 7) as usize]));
        }
        Err(e) => {
            info.insert("resetctl".into(), json!(format!("unreadable: {e}")));
        }
    }
    let total: usize = REGIONS.iter().map(|r| 2 * r.2).sum();
    let mut done = 0;
    let mut got: Vec<Vec<u8>> = Vec::new();
    for (name, addr, size) in REGIONS {
        let mut read_twice = || -> Result<Vec<u8>, Error> {
            let base = done;
            let a = s.read(addr, size, &mut |n| progress(Progress { phase: Phase::Backup, done: base + n, total }))?;
            let b =
                s.read(addr, size, &mut |n| progress(Progress { phase: Phase::Backup, done: base + size + n, total }))?;
            if a != b {
                return Err(Error::Verify(format!("{name}: two reads differ")));
            }
            Ok(a)
        };
        let a = read_twice()?;
        done += 2 * size;
        let crc = s.crc32(addr, size as u32)?;
        if let Some(c) = crc {
            if c != crc32fast::hash(&a) {
                return Err(Error::Verify(format!("{name}: chip CRC {c:#x} != {:#x}", crc32fast::hash(&a))));
            }
        }
        info.insert(
            name.into(),
            json!({
                "address": format!("{addr:#010x}"),
                "size": size,
                "sha256": sha256_hex(&a),
                "chip_crc32": crc.map(|c| format!("{c:#010x}")),
                "reads": 2,
            }),
        );
        got.push(a);
    }
    progress(Progress { phase: Phase::Reset, done: 0, total: 0 });
    s.reset()?;
    let fcfg1 = got.pop();
    let ccfg = got.pop();
    let flash = got.pop().unwrap();
    progress(Progress { phase: Phase::Done, done: 0, total: 0 });
    Ok(Backup { info: Value::Object(info), flash, ccfg, fcfg1 })
}

/// How to flash.
#[derive(Debug, Clone)]
pub struct FlashOptions {
    /// GET_STATUS once per stretch, all-FF stretches skipped, verified by the chip's CRC32 alone (pages 30-31 still
    /// checked): about half the time (7 s instead of 15 s for a 31 KB image). Default false: a read-back too.
    pub fast: bool,
    /// Page 30: migrate (the default), wipe, keep, or a JSON document.
    pub nv: NvChoice,
}

impl Default for FlashOptions {
    fn default() -> Self {
        FlashOptions { fast: false, nv: NvChoice::Migrate }
    }
}

/// What a flash or restore did.
#[derive(Debug, Clone)]
pub struct FlashReport {
    /// The image's build info (None for a restore of stock).
    pub build_info: Option<BuildInfo>,
    pub image_crc32: u32,
    pub bytes_sent: usize,
    /// Page 30 as it was before (keep it with the backup: ows-flash saves its JSON as nv.json).
    pub nv_before: Vec<u8>,
    /// The NV document carried over (migrate, JSON), with what was dropped and why.
    pub nv_doc: Option<NvDoc>,
    pub seconds: f64,
}

/// Our gateway image (`gateway-dongle-app.bin`) to pages 0-29, page 30 as `opts.nv` says, then a reset. Needs this
/// chip's backup. The image must be ours, for this board (its build info).
pub fn flash<T: HidTransport + ?Sized>(
    t: &mut T,
    image: &[u8],
    backup: &Backup,
    opts: &FlashOptions,
    progress: &mut dyn FnMut(Progress),
) -> Result<FlashReport, Error> {
    backup.check()?;
    let (padded, bi) = image::check_product(image, PRODUCT)?;
    let mut r = write(t, &padded, backup, opts.fast, Nv::Choice(&opts.nv), progress)?;
    r.build_info = Some(bi);
    Ok(r)
}

/// The backup's pages 0-29 and page 30 back: stock again, as backed up (pairings since are lost).
pub fn restore<T: HidTransport + ?Sized>(
    t: &mut T,
    backup: &Backup,
    fast: bool,
    progress: &mut dyn FnMut(Progress),
) -> Result<FlashReport, Error> {
    backup.check()?;
    let padded = image::check_app(&backup.flash[..APP_END as usize])?;
    write(t, &padded, backup, fast, Nv::Page(backup.nv_page()), progress)
}

enum Nv<'a> {
    Choice(&'a NvChoice),
    Page(&'a [u8]),
}

fn write<T: HidTransport + ?Sized>(
    t: &mut T,
    image: &[u8],
    backup: &Backup,
    fast: bool,
    nv_plan: Nv,
    progress: &mut dyn FnMut(Progress),
) -> Result<FlashReport, Error> {
    let t0 = Instant::now();
    let p31 = crc32fast::hash(&backup.flash[PAGE31..]);
    progress(Progress { phase: Phase::Enter, done: 0, total: 0 });
    let mut s = Sbl::enter(t)?;
    progress(Progress { phase: Phase::Check, done: 0, total: 0 });
    if s.chip_id_hex() != backup.chip_id() {
        return Err(Error::Refused(format!(
            "chip ID {} is not the backup's {}: not this dongle's backup",
            s.chip_id_hex(),
            backup.chip_id()
        )));
    }
    if s.crc32(PAGE31 as u32, PAGE)? != Some(p31) {
        return Err(Error::Refused("page 31 on the chip is not the backup's".into()));
    }
    let keep = s.crc32(APP_END, FLASH_SIZE - APP_END)?;
    if keep.is_none() {
        return Err(Error::Sbl("no CRC32 of pages 30-31".into()));
    }

    let pages = image.len().div_ceil(PAGE as usize);
    for p in 0..pages {
        s.erase_page(p as u32 * PAGE)?;
        progress(Progress { phase: Phase::Erase, done: p + 1, total: pages });
    }
    let mut sent = 0;
    let stretches = sbl::runs(image, fast);
    let total: usize = stretches.iter().map(|r| r.1.len()).sum();
    for (start, data) in stretches {
        s.download(start as u32, data, fast, &mut |n| {
            sent += n;
            progress(Progress { phase: Phase::Write, done: sent, total });
        })?;
    }

    progress(Progress { phase: Phase::Verify, done: 0, total: image.len() });
    let crc = crc32fast::hash(image);
    let got = s.crc32(0, image.len() as u32)?;
    if got != Some(crc) {
        return Err(Error::Verify(format!("chip CRC {got:x?} != image CRC {crc:#x}")));
    }
    if !fast {
        let back =
            s.read(0, image.len(), &mut |n| progress(Progress { phase: Phase::Verify, done: n, total: image.len() }))?;
        if back != image {
            return Err(Error::Verify("read-back differs".into()));
        }
    }
    if s.crc32(APP_END, FLASH_SIZE - APP_END)? != keep {
        return Err(Error::Verify("pages 30-31 changed!".into()));
    }

    progress(Progress { phase: Phase::Nv, done: 0, total: 0 });
    let nv_before = s.read(NV_PAGE, PAGE as usize, &mut |_| {})?;
    let (page, nv_doc) = match nv_plan {
        Nv::Choice(c) => nv::page_for(c, &nv_before)?,
        Nv::Page(p) => (Some(p.to_vec()), None),
    };
    if let Some(page) = page {
        s.nv_write(&page)?;
    }
    if s.crc32(PAGE31 as u32, PAGE)? != Some(p31) {
        return Err(Error::Verify("page 31 changed!".into()));
    }

    progress(Progress { phase: Phase::Reset, done: 0, total: 0 });
    s.reset()?;
    progress(Progress { phase: Phase::Done, done: 0, total: 0 });
    Ok(FlashReport {
        build_info: None,
        image_crc32: crc,
        bytes_sent: sent,
        nv_before,
        nv_doc,
        seconds: t0.elapsed().as_secs_f64(),
    })
}

/// Whether the dongle answers the normal host protocol (Inquiry 0x4327 -> 0x4328): after a flash or restore, on a
/// freshly opened device, it is back in normal mode; if not, it needs a replug.
pub fn inquiry<T: HidTransport + ?Sized>(t: &mut T) -> Result<bool, Error> {
    t.write(&sbl::INQUIRY)?;
    let mut buf = [0u8; 64];
    for _ in 0..3 {
        let n = match t.read(&mut buf, std::time::Duration::from_millis(500)) {
            Ok(n) => n,
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => return Ok(false),
            Err(e) => return Err(e.into()),
        };
        if buf[..n].windows(5).any(|w| w == [0x55, 0xAA, 0x43, 0x04, 0x28]) {
            return Ok(true);
        }
    }
    Ok(false)
}

/// "YYYY-MM-DD HH:MM:SS +0000" without a date crate.
fn utc_now() -> String {
    let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs()) as i64;
    let (days, rem) = (secs.div_euclid(86400), secs.rem_euclid(86400));
    // days since 1970-01-01 -> civil date (Howard Hinnant's algorithm)
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!("{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02} +0000", rem / 3600, rem / 60 % 60, rem % 60)
}
