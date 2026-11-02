// backup / flash / restore against a simulated dongle (as reference/tests/test_sbl_common.py's FakeDongle): the CH554's
// HID framing and TI's ROM bootloader over a 128 KB flash (erase -> FF, programming only clears bits). The point: pages
// 30-31 are never touched but by the NV write (page 30), the chip is reset only after everything checked out, and the
// commands the crate does not use are never sent.
use std::collections::VecDeque;
use std::io;
use std::path::PathBuf;
use std::time::Duration;

use ows_host::dongle::{self, Backup, FlashOptions, Phase};
use ows_host::nv::{self, NvChoice};
use ows_host::{sbl, Error, HidTransport};

struct Fake {
    flash: Vec<u8>,
    out: VecDeque<Vec<u8>>,
    update_mode: bool,
    status: u8,
    download: (usize, usize),
    erased: Vec<usize>,
    resets: usize,
    log: Vec<u8>,
    corrupt_at: Option<usize>,
    chip_id: [u8; 4],
}

impl Fake {
    fn new(flash: &[u8]) -> Fake {
        Fake {
            flash: flash.to_vec(),
            out: VecDeque::new(),
            update_mode: false,
            status: 0x40,
            download: (0, 0),
            erased: vec![],
            resets: 0,
            log: vec![],
            corrupt_at: None,
            chip_id: [0x20, 0x01, 0x80, 0x00],
        }
    }

    fn report(&mut self, data: &[u8]) {
        for c in data.chunks(62) {
            let mut r = vec![c.len() as u8];
            r.extend_from_slice(c);
            r.resize(0x3F, 0);
            self.out.push_back(r);
        }
    }

    fn command(&mut self, cmd: u8, p: &[u8]) -> Option<Vec<u8>> {
        let u32be = |b: &[u8]| u32::from_be_bytes(b[..4].try_into().unwrap()) as usize;
        match cmd {
            sbl::CMD_AUTO_BAUD | sbl::CMD_PING => Some(vec![]),
            sbl::CMD_GET_CHIP_ID => Some(self.chip_id.to_vec()),
            sbl::CMD_GET_STATUS => Some(vec![self.status]),
            sbl::CMD_RESET => {
                self.resets += 1;
                Some(vec![])
            }
            sbl::CMD_CRC32 => {
                let (a, n) = (u32be(p), u32be(&p[4..]));
                // outside the flash (CCFG, FCFG1 at 0x5000xxxx): no CRC here, as some chips do not give one
                self.flash.get(a..a + n).map(|d| crc32fast::hash(d).to_be_bytes().to_vec())
            }
            sbl::CMD_MEMORY_READ => {
                let (a, n) = (u32be(p), 4 * p[5] as usize);
                Some(if a + n <= self.flash.len() { self.flash[a..a + n].to_vec() } else { vec![0; n] })
            }
            sbl::CMD_SECTOR_ERASE => {
                let a = u32be(p);
                self.erased.push(a / 0x1000);
                self.flash[a..a + 0x1000].fill(0xFF);
                Some(vec![])
            }
            sbl::CMD_DOWNLOAD => {
                self.download = (u32be(p), u32be(&p[4..]));
                Some(vec![])
            }
            sbl::CMD_SEND_DATA => {
                let (a, left) = self.download;
                for (i, &b) in p.iter().enumerate() {
                    let b = if self.corrupt_at == Some(a + i) { b ^ 0x10 } else { b };
                    self.flash[a + i] &= b;
                }
                self.download = (a + p.len(), left - p.len());
                Some(vec![])
            }
            c => panic!("the crate sent command {c:#x}"),
        }
    }
}

impl HidTransport for Fake {
    fn write(&mut self, data: &[u8]) -> io::Result<()> {
        if !self.update_mode {
            assert_eq!(data, sbl::UPDATE_MODE_REQUEST);
            self.update_mode = true;
            self.report(&[0x55, 0xAA, 0x43, 0x03, 0x13, 0x01, 0x58]);
            return Ok(());
        }
        if data.len() == 2 && data[0] == 0 {
            return Ok(()); // the host's ACK of a response
        }
        let (size, cks, cmd) = (data[0] as usize, data[1], data[2]);
        let payload = &data[3..size];
        assert_eq!(size, data.len());
        assert_eq!(cks, payload.iter().fold(cmd, |a, &b| a.wrapping_add(b)));
        self.log.push(cmd);
        match self.command(cmd, payload) {
            None => self.report(&[0x00, 0x33]),
            Some(r) if r.is_empty() => self.report(&[0x00, 0xCC]),
            Some(r) => {
                let mut v = vec![0x00, 0xCC, r.len() as u8 + 2, r.iter().fold(0u8, |a, &b| a.wrapping_add(b))];
                v.extend_from_slice(&r);
                self.report(&v);
            }
        }
        Ok(())
    }

    fn read(&mut self, buf: &mut [u8], _: Duration) -> io::Result<usize> {
        let r = self.out.pop_front().ok_or_else(|| io::Error::new(io::ErrorKind::TimedOut, "nothing to read"))?;
        buf[..r.len()].copy_from_slice(&r);
        Ok(r.len())
    }
}

fn vectors() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../reference/tests/vectors/nv")
}

/// A stand-in for a stock dongle (reference/tests/stock_like.py): code with a vector table, the spare dongle's stock
/// sensor table on page 30, page 31 with the stock BL_CONFIG.
fn stock_flash() -> Vec<u8> {
    let mut f: Vec<u8> = (0..0x20000u32).map(|i| (i.wrapping_mul(2_654_435_761) >> 24) as u8).collect();
    f[0..4].copy_from_slice(&0x2000_5000u32.to_le_bytes());
    f[4..8].copy_from_slice(&0x101u32.to_le_bytes());
    let nv = std::fs::read(vectors().join("dongle_77A85A36_2026-10-23_before_ours.page30.bin")).unwrap();
    f[0x1E000..0x1F000].copy_from_slice(&nv);
    f[0x1FFD8..0x1FFDC].copy_from_slice(&dongle::STOCK_BL_CONFIG);
    f
}

fn take_backup(f: &mut Fake) -> Backup {
    let b = dongle::backup(f, &mut |_| {}).unwrap();
    f.update_mode = false;
    b
}

/// An image of ours: a vector table, then the build info block (common/build_info.h).
fn image(product: &str) -> Vec<u8> {
    let mut img: Vec<u8> = (0..20000u32).map(|i| (i * 7 + 3) as u8).collect();
    img[0..4].copy_from_slice(&0x2000_5000u32.to_le_bytes());
    img[4..8].copy_from_slice(&0x0101u32.to_le_bytes());
    let mut bi = vec![0u8; 116];
    bi[0..4].copy_from_slice(b"OWSB");
    bi[4] = 1;
    bi[5] = 1;
    let put = |bi: &mut Vec<u8>, at: usize, s: &str| bi[at..at + s.len()].copy_from_slice(s.as_bytes());
    put(&mut bi, 16, product);
    put(&mut bi, 40, "Wyze Sense dongle");
    put(&mut bi, 64, "gateway");
    put(&mut bi, 76, "v0.1.0-test");
    img[0xC0..0xC0 + 116].copy_from_slice(&bi);
    img[1000..1100].fill(0xFF);
    img.extend(std::iter::repeat(0xFF).take(2000)); // an FF tail, skipped by fast
    img.extend_from_slice(&[1, 2, 3]); // not whole words
    img
}

fn our_image() -> Vec<u8> {
    let built = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../build/fw/firmware/gateway-dongle-app.bin");
    std::fs::read(built).unwrap_or_else(|_| image("gateway-dongle"))
}

#[test]
fn backup_reads_everything_and_resets() {
    let stock = stock_flash();
    let mut f = Fake::new(&stock);
    let mut phases = vec![];
    let b = dongle::backup(&mut f, &mut |p| phases.push(p.phase)).unwrap();
    assert_eq!(b.flash, stock);
    assert_eq!(b.chip_id(), "20018000");
    assert_eq!(b.info["flash.bin"]["chip_crc32"], format!("{:#010x}", crc32fast::hash(&stock)));
    assert_eq!(f.resets, 1);
    assert!(f.erased.is_empty() && !f.log.contains(&sbl::CMD_DOWNLOAD));
    assert!(phases.contains(&Phase::Backup) && phases.last() == Some(&Phase::Done));
    b.check().unwrap();
    let dir = std::env::temp_dir().join(format!("ows-host-backup-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    b.save(&dir).unwrap();
    assert!(b.save(&dir).is_err(), "never over an existing backup");
    assert_eq!(Backup::load(&dir).unwrap().flash, stock);
    std::fs::remove_dir_all(&dir).unwrap();
}

#[test]
fn flash_writes_only_pages_below_30_verifies_then_migrates() {
    let stock = stock_flash();
    for fast in [false, true] {
        let mut f = Fake::new(&stock);
        let b = take_backup(&mut f);
        let img = our_image();
        let mut last = None;
        let r = dongle::flash(&mut f, &img, &b, &FlashOptions { fast, nv: NvChoice::Migrate }, &mut |p| last = Some(p))
            .unwrap();
        let padded = ows_host::image::check_app(&img).unwrap();
        assert_eq!(&f.flash[..padded.len()], &padded[..]);
        assert_eq!(&f.flash[0x1F000..], &stock[0x1F000..], "page 31 untouched");
        let max = *f.erased.iter().filter(|&&p| p != 30).max().unwrap();
        assert!(max < 30);
        assert_eq!(
            f.erased.iter().filter(|&&p| p >= 30).collect::<Vec<_>>(),
            vec![&30],
            "page 30 once, by the NV write"
        );
        assert_eq!(f.resets, 2); // the backup's and the flash's
        assert!(!f.log.iter().any(|c| [0x2B, 0x2C, 0x2D].contains(c)));
        assert_eq!(last.unwrap().phase, Phase::Done);
        assert_eq!(r.nv_before, &stock[0x1E000..0x1F000]);
        let doc = r.nv_doc.unwrap();
        assert_eq!(doc.source.as_ref().unwrap().layout, "stock");
        assert_eq!(nv::export(&f.flash[0x1E000..0x1F000]).sensors, doc.sensors);
        assert!(r.build_info.is_some());
        if fast {
            assert!(r.bytes_sent <= padded.len(), "fast never sends more");
        }
    }
}

#[test]
fn nv_wipe_and_keep() {
    let stock = stock_flash();
    for (choice, want) in [(NvChoice::Wipe, vec![0xFF; 4096]), (NvChoice::Keep, stock[0x1E000..0x1F000].to_vec())] {
        let mut f = Fake::new(&stock);
        let b = take_backup(&mut f);
        dongle::flash(&mut f, &our_image(), &b, &FlashOptions { fast: true, nv: choice }, &mut |_| {}).unwrap();
        assert_eq!(&f.flash[0x1E000..0x1F000], &want[..]);
    }
}

#[test]
fn restore_puts_stock_back() {
    let stock = stock_flash();
    let mut f = Fake::new(&stock);
    let b = take_backup(&mut f);
    dongle::flash(&mut f, &our_image(), &b, &FlashOptions::default(), &mut |_| {}).unwrap();
    f.update_mode = false;
    dongle::restore(&mut f, &b, false, &mut |_| {}).unwrap();
    assert_eq!(f.flash, stock);
}

#[test]
fn refuses_another_chip_or_page_31() {
    let stock = stock_flash();
    let mut f = Fake::new(&stock);
    let b = take_backup(&mut f);
    f.chip_id = [0x20, 0x01, 0x80, 0x01];
    assert!(matches!(
        dongle::flash(&mut f, &our_image(), &b, &FlashOptions::default(), &mut |_| {}),
        Err(Error::Refused(_))
    ));
    let mut f = Fake::new(&stock);
    f.flash[0x1F100] ^= 1;
    assert!(matches!(
        dongle::flash(&mut f, &our_image(), &b, &FlashOptions::default(), &mut |_| {}),
        Err(Error::Refused(_))
    ));
    assert!(f.erased.is_empty());
}

#[test]
fn a_bad_write_is_not_reset() {
    let stock = stock_flash();
    for fast in [false, true] {
        let mut f = Fake::new(&stock);
        let b = take_backup(&mut f);
        f.corrupt_at = Some(0x200);
        let e = dongle::flash(&mut f, &our_image(), &b, &FlashOptions { fast, nv: NvChoice::Migrate }, &mut |_| {});
        assert!(matches!(e, Err(Error::Verify(_))), "{e:?}");
        assert_eq!(f.resets, 1, "only the backup's reset");
        assert_eq!(&f.flash[0x1E000..], &stock[0x1E000..], "pages 30-31 untouched");
    }
}

#[test]
fn refuses_images_not_for_the_dongle() {
    let stock = stock_flash();
    let mut f = Fake::new(&stock);
    let b = take_backup(&mut f);
    let opts = FlashOptions::default();
    for img in [
        image("gateway-contact-v1"),
        image("sensor-contact-v1"),
        stock[..0x1E000].to_vec(),             // stock: no build info
        vec![0u8; 0x1E004],                    // too big
        image("gateway-dongle")[8..].to_vec(), // no vector table
    ] {
        assert!(matches!(dongle::flash(&mut f, &img, &b, &opts, &mut |_| {}), Err(Error::Image(_))));
    }
    assert!(f.log.is_empty() || f.resets == 1);
    assert!(f.erased.is_empty());
}

#[test]
fn a_backup_must_be_whole_and_stock() {
    let stock = stock_flash();
    let mut f = Fake::new(&stock);
    let mut b = take_backup(&mut f);
    b.flash[0x1FFD8] = 0xFF;
    assert!(matches!(b.check(), Err(Error::Backup(_))));
}

#[test]
fn inquiry_sees_normal_mode() {
    struct Normal(VecDeque<Vec<u8>>);
    impl HidTransport for Normal {
        fn write(&mut self, d: &[u8]) -> io::Result<()> {
            assert_eq!(d, sbl::INQUIRY);
            self.0.push_back(vec![0x0A, 0x55, 0xAA, 0x43, 0x04, 0x28, 1, 2, 3, 0]);
            Ok(())
        }
        fn read(&mut self, buf: &mut [u8], _: Duration) -> io::Result<usize> {
            let r = self.0.pop_front().ok_or_else(|| io::Error::new(io::ErrorKind::TimedOut, "-"))?;
            buf[..r.len()].copy_from_slice(&r);
            Ok(r.len())
        }
    }
    assert!(dongle::inquiry(&mut Normal(VecDeque::new())).unwrap());
}

#[test]
fn fast_skips_ff_stretches() {
    let img = ows_host::image::check_app(&image("gateway-dongle")).unwrap();
    let runs = sbl::runs(&img, true);
    let sent: usize = runs.iter().map(|r| r.1.len()).sum();
    assert!(sent < img.len() && runs.len() == 2, "{} runs, {sent} of {}", runs.len(), img.len());
    for (a, d) in runs {
        assert_eq!(d, &img[a..a + d.len()]);
    }
}
