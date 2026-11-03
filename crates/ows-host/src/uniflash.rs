//! A CC1310 on 2-pin cJTAG (TMS, TCK and RESET_N from a LaunchPad's XDS110: a Wyze v1 contact or motion sensor,
//! docs/design/v1-contact.md) through TI UniFlash's DSLite, which OpenOCD cannot replace there. A port of
//! `tools/uniflash.py` (#50) with its guards:
//!
//! - flashing needs the unit's backup, checked against the chip: the same CCFG MAC, the CCFG the same but BL_CONFIG;
//! - page 30 (NV) only as `--nv` says (keep by default); page 31 (code and the CCFG with the unit's MAC) never, but
//!   BL_CONFIG in [`Probe::enable_bootloader`] (page 31 read twice, written back with only that word changed, read
//!   back whole);
//! - every write is read back; every operation ends with a board reset with the debug power domain released.
//!
//! TI UniFlash is not ours to redistribute: [`fetch`] downloads TI's installer from TI (the version and SHA-256 pinned
//! as in tools/toolchain.lock) and installs it unattended; [`find_dslite`] finds an install.

use std::path::{Path, PathBuf};
use std::process::Command;

use serde_json::json;

use crate::manifest::sha256_hex;
use crate::nv::{self, NvChoice, NvDoc};
use crate::Error;

pub const UNIFLASH_VERSION: &str = "9.7.0.5873";
pub const UNIFLASH_URL: &str =
    "https://dr-download.ti.com/software-development/software-programming-tool/MD-QeJBJLj8gq/9.7.0/uniflash_sl.9.7.0.5873.run";
pub const UNIFLASH_SHA256: &str = "3d1788cd1ce413acf2b3927834288213bf9751100ff256687440e3a32c97b1a1";
/// DSLite inside an install.
pub const DSLITE_IN: &str = "deskdb/content/TICloudAgent/linux/ccs_base/DebugServer/bin/DSLite";
/// The XDS110 2-pin cJTAG target configuration (firmware/boards/wyze_contact_v1/xds110_2pin.ccxml).
pub const CCXML: &str = include_str!("../../../firmware/boards/wyze_contact_v1/xds110_2pin.ccxml");

pub const APP_END: u32 = 0x1E000;
pub const NV_PAGE: u32 = 0x1E000;
pub const PAGE31: u32 = 0x1F000;
pub const CCFG_ADDR: u32 = 0x1FFA8;
pub const BL_CONFIG: usize = 0x30;
pub const SLOT_A: usize = 0x01000;
pub const SLOT_B: usize = 0x0F000;
const MAX_BL_PIN: u8 = 14;
const CLEAN_RESET: [&str; 4] = ["-s", "KeepDebugPoweredAtDisconnect=false", "-r", "2"];
const WRITE: [&str; 4] =
    ["-s", "FlashEraseSetting=Necessary Sectors Only", "-s", "VerifyAfterProgramLoad=Full verification"];

/// The ROM bootloader's backdoor pins (both pulled up on the board): the W pad, the button.
pub fn backdoor_pin(name: &str) -> Option<u8> {
    match name {
        "w" => Some(10),
        "button" => Some(11),
        _ => None,
    }
}

/// Where DSLite runs: a real one, or a fake chip in tests.
pub trait Runner {
    /// DSLite with these arguments: its output, or an error (exit status, "Failed", "fatal").
    fn run(&mut self, args: &[String]) -> Result<String, Error>;
}

pub struct Dslite {
    pub exe: PathBuf,
}

impl Runner for Dslite {
    fn run(&mut self, args: &[String]) -> Result<String, Error> {
        let o = Command::new(&self.exe).args(args).output()?;
        let out = String::from_utf8_lossy(&o.stdout).into_owned() + &String::from_utf8_lossy(&o.stderr);
        if !o.status.success() || out.contains("Failed") || out.contains("fatal") {
            let tail: String = out.chars().rev().take(1500).collect::<Vec<_>>().into_iter().rev().collect();
            return Err(Error::Uniflash(format!("DSLite {} failed:\n{tail}", args.first().map_or("", |s| s))));
        }
        Ok(out)
    }
}

/// The default install directory: $OWS_UNIFLASH, else $XDG_DATA_HOME (or ~/.local/share)/openwyzesense/uniflash.
pub fn default_dir() -> PathBuf {
    if let Some(d) = std::env::var_os("OWS_UNIFLASH") {
        return d.into();
    }
    let data = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".local/share"));
    data.join("openwyzesense/uniflash")
}

/// DSLite: in `dir` if given, else the default directory, else a repository checkout's .toolchain/uniflash.
pub fn find_dslite(dir: Option<&Path>) -> Option<PathBuf> {
    let mut c: Vec<PathBuf> = dir.map(Path::to_path_buf).into_iter().collect();
    if dir.is_none() {
        c.push(default_dir());
        c.push(PathBuf::from(".toolchain/uniflash"));
    }
    c.into_iter().map(|d| d.join(DSLITE_IN)).find(|p| p.is_file())
}

/// TI's installer from TI (curl), checked against the pinned SHA-256, installed unattended into `dir` (~380 MB
/// download, ~1.5 GB installed). `say` gets what is going on. Already there (the stamp): nothing done.
pub fn fetch(dir: &Path, say: &mut dyn FnMut(&str)) -> Result<PathBuf, Error> {
    let stamp = dir.join(".ows-version");
    let dslite = dir.join(DSLITE_IN);
    if std::fs::read_to_string(&stamp).ok().as_deref().map(str::trim) == Some(UNIFLASH_VERSION) && dslite.is_file() {
        say(&format!("UniFlash {UNIFLASH_VERSION} already in {}", dir.display()));
        return Ok(dslite);
    }
    let dl = dir.with_extension("download");
    std::fs::create_dir_all(&dl)?;
    let inst = dl.join(UNIFLASH_URL.rsplit('/').next().unwrap());
    let ok = |p: &Path| std::fs::read(p).map(|d| sha256_hex(&d) == UNIFLASH_SHA256).unwrap_or(false);
    if !ok(&inst) {
        say(&format!("downloading TI UniFlash {UNIFLASH_VERSION} from TI (~380 MB): {UNIFLASH_URL}"));
        let part = inst.with_extension("part");
        let st = Command::new("curl").args(["-fL", "--retry", "3", "-o"]).arg(&part).arg(UNIFLASH_URL).status()?;
        if !st.success() {
            return Err(Error::Uniflash("download failed (curl)".into()));
        }
        std::fs::rename(&part, &inst)?;
        if !ok(&inst) {
            return Err(Error::Uniflash(format!("{}: not the SHA-256 pinned ({UNIFLASH_SHA256})", inst.display())));
        }
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&inst, std::fs::Permissions::from_mode(0o755))?;
    }
    say(&format!("installing into {} (TI's license applies)", dir.display()));
    let _ = std::fs::remove_dir_all(dir);
    // it warns about libgconf-2.so.4: its GUI only; DSLite does not need it
    let _ = Command::new(&inst).args(["--mode", "unattended", "--prefix"]).arg(dir).output()?;
    if !dslite.is_file() {
        return Err(Error::Uniflash(format!("install failed: no {}", dslite.display())));
    }
    #[cfg(unix)]
    {
        let link = dir.join("deskdb/content/TICloudAgent/linux/ccs_base/cloudagent");
        let _ = std::fs::remove_file(&link);
        std::os::unix::fs::symlink("../../../../../TICloudAgentHostApp", &link)?;
    }
    std::fs::write(&stamp, format!("{UNIFLASH_VERSION}\n"))?;
    let _ = std::fs::remove_dir_all(&dl);
    Ok(dslite)
}

/// A scratch directory, removed when dropped.
struct Scratch(PathBuf);

impl Scratch {
    fn new() -> Result<Scratch, Error> {
        use std::sync::atomic::{AtomicU32, Ordering};
        static N: AtomicU32 = AtomicU32::new(0);
        let p = std::env::temp_dir().join(format!(
            "ows-uniflash-{}-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&p)?;
        Ok(Scratch(p))
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn valid_mac(m: &[u8]) -> bool {
    m.len() == 8 && m.iter().all(|c| b"0123456789ABCDEF".contains(c)) && m != b"00000000"
}

/// The unit's CCFG (88 bytes) with only BL_CONFIG changed: ROM bootloader on, backdoor DIO`pin`, active low.
pub fn bootloader_ccfg(ccfg: &[u8], pin: u8) -> Result<Vec<u8>, Error> {
    if ccfg.len() != 88 || pin > MAX_BL_PIN {
        return Err(Error::Uniflash("an 88-byte CCFG and a backdoor pin 0-14 expected".into()));
    }
    let mut new = ccfg.to_vec();
    new[BL_CONFIG..BL_CONFIG + 4].copy_from_slice(&(0xC5FE_00C5u32 | (pin as u32) << 8).to_le_bytes());
    let w = |c: &[u8], o: usize| u32::from_le_bytes(c[o..o + 4].try_into().unwrap());
    if w(&new, 0x3C) != 0xFFC5_00C5 {
        return Err(Error::Refused("the CPU debug port (TAP_DAP_0) would not stay enabled".into()));
    }
    if w(&new, 0x44) != 0 {
        return Err(Error::Refused("IMAGE_VALID_CONF is not 0 (the flash image would not run)".into()));
    }
    Ok(new)
}

/// The two-slot layout as one image at 0x0: the boot selector (page 0), slot A, and slot B's first page blank.
pub fn slots_image(boot: &[u8], slot_a: &[u8]) -> Result<Vec<u8>, Error> {
    if boot.is_empty() || boot.len() > SLOT_A || slot_a.len() > SLOT_B - SLOT_A || slot_a.get(..4) != Some(b"OWSI") {
        return Err(Error::Image("a boot selector of at most one page and a slot image (OWSI) expected".into()));
    }
    let mut img = vec![0xFF; SLOT_B + 0x1000];
    img[..boot.len()].copy_from_slice(boot);
    img[SLOT_A..SLOT_A + slot_a.len()].copy_from_slice(slot_a);
    Ok(img)
}

/// What [`Probe::backup`] wrote.
#[derive(Debug, Clone)]
pub struct BackupInfo {
    pub mac: String,
    pub bl_config: u32,
    pub flash_sha256: String,
}

/// The chip behind the probe.
pub struct Probe<R: Runner> {
    pub runner: R,
    config: PathBuf,
    scratch: Scratch,
}

impl<R: Runner> Probe<R> {
    pub fn new(runner: R) -> Result<Probe<R>, Error> {
        let scratch = Scratch::new()?;
        let config = scratch.0.join("xds110_2pin.ccxml");
        std::fs::write(&config, CCXML)?;
        Ok(Probe { runner, config, scratch })
    }

    fn dslite(&mut self, args: &[&str]) -> Result<String, Error> {
        let mut a: Vec<String> = vec![args[0].into(), "--config".into(), self.config.display().to_string()];
        a.extend(args[1..].iter().map(|s| s.to_string()));
        self.runner.run(&a)
    }

    /// `words` 32-bit words from `addr`.
    pub fn read(&mut self, addr: u32, words: usize) -> Result<Vec<u8>, Error> {
        let out = self.scratch.0.join("read.bin");
        let _ = std::fs::remove_file(&out);
        let range = format!("{addr:#x},{words}");
        let o = out.display().to_string();
        self.dslite(&["memory", "--range", &range, "--output", &o, "--timeout", "200"])?;
        let d = std::fs::read(&out)?;
        if d.len() != words * 4 {
            return Err(Error::Uniflash(format!("read {range}: {} bytes", d.len())));
        }
        Ok(d)
    }

    /// A board reset through RESET_N, the debug domain released: the chip starts as after a power-up.
    pub fn reset(&mut self) -> Result<(), Error> {
        let mut a = vec!["flash"];
        a.extend(CLEAN_RESET);
        self.dslite(&a).map(|_| ())
    }

    /// Erases the sectors `data` covers and programs it at `addr`, then reads it back. Only callers in this file;
    /// every one checks its range first.
    fn write_at(&mut self, addr: u32, data: &[u8]) -> Result<(), Error> {
        let f = self.scratch.0.join("write.bin");
        std::fs::write(&f, data)?;
        let spec = format!("{},{addr:#x}", f.display());
        let mut a = vec!["flash"];
        a.extend(WRITE);
        a.push(&spec);
        self.dslite(&a)?;
        let back = self.read(addr, data.len().div_ceil(4))?;
        if back[..data.len()] != *data {
            let at = back.iter().zip(data).position(|(a, b)| a != b).unwrap_or(0);
            return Err(Error::Verify(format!("read-back differs at {:#x}", addr as usize + at)));
        }
        Ok(())
    }

    /// The chip's CCFG and its MAC (refuses a chip without a valid one: nothing is written then).
    pub fn chip_ccfg(&mut self) -> Result<(Vec<u8>, String), Error> {
        let ccfg = self.read(CCFG_ADDR, 22)?;
        let mac = &ccfg[0x24..0x2C];
        if !valid_mac(mac) {
            return Err(Error::Refused(format!(
                "chip CCFG has no valid MAC ({}): not touching flash",
                hex::encode(mac)
            )));
        }
        let mac = String::from_utf8_lossy(mac).into_owned();
        Ok((ccfg, mac))
    }

    /// The backup in `dir` is this chip's: 128 KB (its SHA-256 as backup.json says), the same MAC, the same CCFG but
    /// BL_CONFIG.
    pub fn verify_backup(&mut self, dir: &Path) -> Result<String, Error> {
        let fl = std::fs::read(dir.join("flash.bin"))
            .map_err(|_| Error::Backup(format!("{}: no flash.bin (back the unit up first)", dir.display())))?;
        if fl.len() != 0x20000 {
            return Err(Error::Backup(format!("{}/flash.bin: {} bytes, not 128 KB", dir.display(), fl.len())));
        }
        if let Ok(t) = std::fs::read_to_string(dir.join("backup.json")) {
            let meta: serde_json::Value = serde_json::from_str(&t).map_err(|e| Error::Backup(e.to_string()))?;
            if meta["flash_sha256"].as_str() != Some(&sha256_hex(&fl)) {
                return Err(Error::Backup("flash.bin's SHA-256 is not backup.json's".into()));
            }
        }
        let (ccfg, mac) = self.chip_ccfg()?;
        let bak = &fl[CCFG_ADDR as usize..];
        if bak[0x24..0x2C] != *mac.as_bytes() {
            return Err(Error::Refused(format!(
                "the backup's MAC {} is not the chip's {mac}",
                String::from_utf8_lossy(&bak[0x24..0x2C])
            )));
        }
        let strip = |c: &[u8]| [&c[..BL_CONFIG], &c[BL_CONFIG + 4..]].concat();
        if strip(bak) != strip(&ccfg) {
            return Err(Error::Refused("the chip's CCFG differs from the backup's (but BL_CONFIG)".into()));
        }
        Ok(mac)
    }

    /// Flash (read twice, compared), CCFG and FCFG1 into `dir`, with backup.json. Read only. Never over an existing
    /// backup.
    pub fn backup(&mut self, dir: &Path) -> Result<BackupInfo, Error> {
        if dir.join("flash.bin").exists() {
            return Err(Error::Backup(format!("{} already holds a backup", dir.display())));
        }
        let a = self.read(0, 32768)?;
        if self.read(0, 32768)? != a {
            return Err(Error::Verify("two reads of the flash differ".into()));
        }
        let ccfg = self.read(CCFG_ADDR, 22)?;
        let fcfg1 = self.read(0x5000_1000, 256)?;
        if ccfg[..] != a[CCFG_ADDR as usize..] {
            return Err(Error::Verify("the CCFG read differs from the end of the flash read".into()));
        }
        std::fs::create_dir_all(dir)?;
        std::fs::write(dir.join("flash.bin"), &a)?;
        std::fs::write(dir.join("ccfg.bin"), &ccfg)?;
        std::fs::write(dir.join("fcfg1.bin"), &fcfg1)?;
        let info = BackupInfo {
            mac: String::from_utf8_lossy(&ccfg[0x24..0x2C]).into_owned(),
            bl_config: u32::from_le_bytes(ccfg[0x30..0x34].try_into().unwrap()),
            flash_sha256: sha256_hex(&a),
        };
        let meta = json!({
            "mac_ccfg": info.mac,
            "bl_config": format!("{:08x}", info.bl_config),
            "fcfg1_user_id": format!("{:08x}", u32::from_le_bytes(fcfg1[0x294..0x298].try_into().unwrap())),
            "flash_sha256": info.flash_sha256,
            "config": "xds110_2pin.ccxml (built in)",
        });
        std::fs::write(dir.join("backup.json"), serde_json::to_string_pretty(&meta).unwrap() + "\n")?;
        self.reset()?;
        Ok(info)
    }

    /// Page 30 as `choice` says (keep: nothing). Returns the document carried over (migrate, JSON).
    pub fn nv_step(&mut self, choice: &NvChoice) -> Result<Option<NvDoc>, Error> {
        if matches!(choice, NvChoice::Keep) {
            return Ok(None);
        }
        let current = self.read(NV_PAGE, 1024)?;
        let (page, doc) = nv::page_for(choice, &current)?;
        if let Some(p) = page {
            self.write_at(NV_PAGE, &p)?;
        }
        Ok(doc)
    }

    /// An app image to pages 0-29 (read back), page 30 as `nv` says, a reset. `backup`: the unit's backup, checked
    /// against the chip first (None only when the caller knowingly skips it).
    pub fn flash(&mut self, app: &[u8], nv: &NvChoice, backup: Option<&Path>) -> Result<Option<NvDoc>, Error> {
        if app.is_empty() || app.len() > APP_END as usize {
            return Err(Error::Image(format!("{} bytes: an app image for pages 0-29 only", app.len())));
        }
        if let Some(b) = backup {
            self.verify_backup(b)?;
        }
        self.write_at(0, app)?;
        let doc = self.nv_step(nv)?;
        self.reset()?;
        Ok(doc)
    }

    /// The boot selector and slot A ([`slots_image`]), as [`Probe::flash`].
    pub fn slots(
        &mut self,
        boot: &[u8],
        slot_a: &[u8],
        nv: &NvChoice,
        backup: Option<&Path>,
    ) -> Result<Option<NvDoc>, Error> {
        let img = slots_image(boot, slot_a)?;
        self.flash(&img, nv, backup)
    }

    /// The one CCFG write allowed (the user, 2026-10-24, #14): the ROM bootloader on, its backdoor DIO`pin` active low.
    /// Page 31 is read whole twice (compared) and written back with only BL_CONFIG changed (the stock code before the
    /// CCFG and the MAC stay), then read back whole. Returns false if it was already so.
    pub fn enable_bootloader(&mut self, pin: u8) -> Result<bool, Error> {
        let words = ((0x20000 - PAGE31) / 4) as usize;
        let page = self.read(PAGE31, words)?;
        if self.read(PAGE31, words)? != page {
            return Err(Error::Verify("two reads of page 31 differ: not writing".into()));
        }
        let off = (CCFG_ADDR - PAGE31) as usize;
        let mut new = page[..off].to_vec();
        new.extend(bootloader_ccfg(&page[off..], pin)?);
        if new == page {
            self.reset()?;
            return Ok(false);
        }
        self.write_at(PAGE31, &new)?;
        self.reset()?;
        Ok(true)
    }

    /// One step: the chip's MAC; its backup (made now under `backup_root/<MAC>`, or the one there verified); our
    /// image (sensor: `boot` + `slot_a`; gateway: `app`); the bootloader's backdoor (`bootloader` Some); a reset.
    pub fn provision(&mut self, p: &Provision) -> Result<ProvisionReport, Error> {
        let img = match (&p.app, &p.boot, &p.slot_a) {
            (Some(app), None, None) => {
                if app.is_empty() || app.len() > APP_END as usize {
                    return Err(Error::Image("invalid gateway app image".into()));
                }
                app.clone()
            }
            (None, Some(boot), Some(a)) => slots_image(boot, a)?,
            _ => return Err(Error::Image("a sensor (boot selector + slot A) or a gateway (app) image".into())),
        };
        let (_, mac) = self.chip_ccfg()?;
        let dir =
            if p.backup_root.join("flash.bin").is_file() { p.backup_root.clone() } else { p.backup_root.join(&mac) };
        let backed_up_now = if dir.join("flash.bin").is_file() {
            self.verify_backup(&dir)?;
            false
        } else {
            self.backup(&dir)?;
            true
        };
        let nv_doc = self.flash(&img, &p.nv, None)?;
        if let Some(pin) = p.bootloader {
            self.enable_bootloader(pin)?;
        }
        self.reset()?;
        Ok(ProvisionReport { mac, backup_dir: dir, backed_up_now, nv_doc })
    }
}

/// What to provision.
#[derive(Debug, Clone)]
pub struct Provision {
    /// The gateway image (gateway-contact-v1-app.bin), or None for a sensor.
    pub app: Option<Vec<u8>>,
    /// The sensor's boot selector and slot A image.
    pub boot: Option<Vec<u8>>,
    pub slot_a: Option<Vec<u8>>,
    /// Backups go to `backup_root/<MAC>` (or `backup_root` itself if it holds one).
    pub backup_root: PathBuf,
    /// The backdoor pin to enable ([`backdoor_pin`]), None to leave BL_CONFIG.
    pub bootloader: Option<u8>,
    pub nv: NvChoice,
}

#[derive(Debug, Clone)]
pub struct ProvisionReport {
    pub mac: String,
    pub backup_dir: PathBuf,
    pub backed_up_now: bool,
    pub nv_doc: Option<NvDoc>,
}
