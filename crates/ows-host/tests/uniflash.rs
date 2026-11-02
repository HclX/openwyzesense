// ows_host::uniflash against a fake DSLite over a simulated 128 KB CC1310 (as reference/tests/test_uniflash.py's
// FakeDSLiteChip): the CCFG of the v1 contact 777B42DF as it came (reference/tests/vectors/ccfg) at the end of page 31.
use std::path::{Path, PathBuf};

use ows_host::nv::NvChoice;
use ows_host::uniflash::{self, Probe, Provision, Runner, BL_CONFIG, CCFG_ADDR};
use ows_host::Error;

fn vectors() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../reference/tests/vectors")
}

struct Chip {
    flash: Vec<u8>,
    fcfg1: Vec<u8>,
    resets: usize,
    writes: Vec<(usize, usize)>,
}

fn arg<'a>(a: &'a [String], k: &str) -> &'a str {
    &a[a.iter().position(|x| x == k).unwrap() + 1]
}

impl Runner for &mut Chip {
    fn run(&mut self, a: &[String]) -> Result<String, Error> {
        assert_eq!(a[1], "--config");
        assert!(std::fs::read_to_string(&a[2]).unwrap().contains("cJTAG (1149.7) 2-pin"));
        match a[0].as_str() {
            "memory" => {
                let (addr, words) = arg(a, "--range").split_once(',').unwrap();
                let addr = usize::from_str_radix(addr.trim_start_matches("0x"), 16).unwrap();
                let n = words.parse::<usize>().unwrap() * 4;
                let d = if addr >= 0x5000_1000 {
                    &self.fcfg1[addr - 0x5000_1000..][..n]
                } else {
                    &self.flash[addr..addr + n]
                };
                std::fs::write(arg(a, "--output"), d).unwrap();
                Ok("ok".into())
            }
            "flash" if a.contains(&"-r".to_string()) => {
                self.resets += 1;
                Ok("reset".into())
            }
            "flash" => {
                let (path, addr) = a.last().unwrap().rsplit_once(',').unwrap();
                let addr = usize::from_str_radix(addr.trim_start_matches("0x"), 16).unwrap();
                let data = std::fs::read(path).unwrap();
                let (first, last) = (addr / 0x1000 * 0x1000, (addr + data.len() - 1) / 0x1000 * 0x1000);
                self.flash[first..last + 0x1000].fill(0xFF); // necessary sectors only
                self.flash[addr..addr + data.len()].copy_from_slice(&data);
                self.writes.push((addr, data.len()));
                Ok("flash ok".into())
            }
            c => panic!("unexpected DSLite {c}"),
        }
    }
}

fn contact() -> Chip {
    let ccfg = std::fs::read(vectors().join("ccfg/contact_v1_777B42DF_2026-10-24.ccfg.bin")).unwrap();
    let mut f: Vec<u8> = (0..0x20000u32).map(|i| (i.wrapping_mul(2_246_822_519) >> 24) as u8).collect();
    f[0..8].copy_from_slice(&[0x00, 0x50, 0x00, 0x20, 0x01, 0x01, 0, 0]);
    f[0x1E000..0x1F000].fill(0xFF);
    f[0x20000 - ccfg.len()..].copy_from_slice(&ccfg);
    Chip { flash: f, fcfg1: vec![0; 1024], resets: 0, writes: vec![] }
}

fn tmp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("ows-uf-test-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    d
}

fn slot_a() -> Vec<u8> {
    let mut img = vec![0xFF; 0x100];
    img[..4].copy_from_slice(b"OWSI");
    img.extend_from_slice(&[0x00, 0x40, 0x00, 0x20, 0x01, 0x11, 0x00, 0x00]);
    img.extend(std::iter::repeat(0x34).take(512));
    img
}

fn boot() -> Vec<u8> {
    let mut b = vec![0x00, 0x40, 0x00, 0x20, 0x01, 0x01, 0x00, 0x00];
    b.extend(std::iter::repeat(0x12).take(512));
    b
}

#[test]
fn backup_is_read_only_and_complete() {
    let mut c = contact();
    let before = c.flash.clone();
    let d = tmp("backup");
    let info = Probe::new(&mut c).unwrap().backup(&d).unwrap();
    assert_eq!(info.mac, "777B42DF");
    assert_eq!(std::fs::read(d.join("flash.bin")).unwrap(), before);
    assert_eq!(std::fs::read(d.join("ccfg.bin")).unwrap(), &before[CCFG_ADDR as usize..]);
    assert!(d.join("fcfg1.bin").exists() && d.join("backup.json").exists());
    assert!(c.writes.is_empty() && c.resets == 1);
    assert!(Probe::new(&mut c).unwrap().backup(&d).is_err(), "never over a backup");
    std::fs::remove_dir_all(d).unwrap();
}

#[test]
fn flash_needs_this_units_backup_and_never_touches_pages_30_31() {
    let mut c = contact();
    let d = tmp("flash");
    Probe::new(&mut c).unwrap().backup(&d).unwrap();
    let stock = c.flash.clone();
    let app = [boot(), vec![0x56; 3000]].concat();
    Probe::new(&mut c).unwrap().flash(&app, &NvChoice::Keep, Some(&d)).unwrap();
    assert_eq!(&c.flash[..app.len()], &app[..]);
    assert_eq!(&c.flash[0x1E000..], &stock[0x1E000..], "pages 30-31 untouched");
    assert_eq!(c.writes, vec![(0, app.len())]);

    // another unit's backup: refused before any write
    let mut other = contact();
    let mac_at = 0x20000 - 88 + 0x24;
    other.flash[mac_at..mac_at + 8].copy_from_slice(b"777B42E0");
    let e = Probe::new(&mut other).unwrap().flash(&app, &NvChoice::Keep, Some(&d));
    assert!(matches!(e, Err(Error::Refused(_))), "{e:?}");
    assert!(other.writes.is_empty());
    // no backup there
    let e = Probe::new(&mut c).unwrap().flash(&app, &NvChoice::Keep, Some(Path::new("/nonexistent")));
    assert!(matches!(e, Err(Error::Backup(_))));
    // too big for pages 0-29
    assert!(Probe::new(&mut c).unwrap().flash(&vec![0; 0x1E004], &NvChoice::Keep, None).is_err());
    std::fs::remove_dir_all(d).unwrap();
}

#[test]
fn nv_wipe_writes_page_30_only() {
    let mut c = contact();
    c.flash[0x1E000..0x1E010].fill(0);
    Probe::new(&mut c).unwrap().flash(&boot(), &NvChoice::Wipe, None).unwrap();
    assert_eq!(&c.flash[0x1E000..0x1F000], &[0xFF; 4096][..]);
    assert_eq!(c.writes, vec![(0, boot().len()), (0x1E000, 4096)]);
}

#[test]
fn the_bootloader_backdoor_changes_bl_config_only() {
    let mut c = contact();
    let before = c.flash.clone();
    assert!(Probe::new(&mut c).unwrap().enable_bootloader(10).unwrap());
    let off = CCFG_ADDR as usize + BL_CONFIG;
    assert_eq!(&c.flash[off..off + 4], &0xC5FE_0AC5u32.to_le_bytes());
    let strip = |f: &[u8]| [&f[..off], &f[off + 4..]].concat();
    assert_eq!(strip(&c.flash), strip(&before), "the stock code in page 31 and the MAC stay");
    assert!(!Probe::new(&mut c).unwrap().enable_bootloader(10).unwrap(), "already so: no write");
    assert_eq!(c.writes.len(), 1);
    assert!(uniflash::bootloader_ccfg(&before[CCFG_ADDR as usize..], 15).is_err(), "no DIO15 on the 5x5 package");
}

#[test]
fn a_chip_without_a_mac_is_not_touched() {
    let mut c = contact();
    let mac_at = 0x20000 - 88 + 0x24;
    c.flash[mac_at..mac_at + 8].copy_from_slice(b"00000000");
    let p = Provision {
        app: Some(boot()),
        boot: None,
        slot_a: None,
        backup_root: tmp("nomac"),
        bootloader: Some(10),
        nv: NvChoice::Keep,
    };
    assert!(matches!(Probe::new(&mut c).unwrap().provision(&p), Err(Error::Refused(_))));
    assert!(c.writes.is_empty());
}

#[test]
fn provision_sensor_backs_up_flashes_slots_and_enables_the_backdoor() {
    let mut c = contact();
    let stock = c.flash.clone();
    let root = tmp("prov");
    let p = Provision {
        app: None,
        boot: Some(boot()),
        slot_a: Some(slot_a()),
        backup_root: root.clone(),
        bootloader: uniflash::backdoor_pin("w"),
        nv: NvChoice::Keep,
    };
    let r = Probe::new(&mut c).unwrap().provision(&p).unwrap();
    assert_eq!(r.mac, "777B42DF");
    assert!(r.backed_up_now);
    assert_eq!(std::fs::read(root.join("777B42DF/flash.bin")).unwrap(), stock, "the stock image backed up");
    let img = uniflash::slots_image(&boot(), &slot_a()).unwrap();
    assert_eq!(&c.flash[..img.len()], &img[..]);
    assert_eq!(&c.flash[0x1000..0x1004], b"OWSI");
    let off = CCFG_ADDR as usize + BL_CONFIG;
    assert_eq!(&c.flash[off..off + 4], &0xC5FE_0AC5u32.to_le_bytes());
    // again: the backup there is checked (BL_CONFIG may differ), not remade
    let r = Probe::new(&mut c).unwrap().provision(&p).unwrap();
    assert!(!r.backed_up_now);
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn pinned_uniflash_matches_the_toolchain_lock() {
    let lock =
        std::fs::read_to_string(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tools/toolchain.lock")).unwrap();
    for (k, v) in [
        ("UNIFLASH_VERSION", uniflash::UNIFLASH_VERSION),
        ("UNIFLASH_URL", uniflash::UNIFLASH_URL),
        ("UNIFLASH_SHA256", uniflash::UNIFLASH_SHA256),
    ] {
        assert!(lock.lines().any(|l| l == format!("{k}={v}")), "{k} differs from tools/toolchain.lock");
    }
}
