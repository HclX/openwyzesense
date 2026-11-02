// backup / flash / restore against a simulated dongle (as reference/tests/test_dongle_sbl.py's FakeDongle): the CH554's
// HID framing and TI's ROM bootloader over a 128 KB flash (erase -> FF, programming only clears bits). The point: pages
// 30-31 are never touched but by the NV write (page 30), the chip is reset only after everything checked out, and the
// commands the crate does not use are never sent.
use std::collections::VecDeque;
use std::io;
use std::path::PathBuf;
use std::time::Duration;

use ows_host::sim::{self, SimDongle};

use ows_host::dongle::{self, Backup, FlashOptions, Phase};
use ows_host::nv::{self, NvChoice};
use ows_host::{sbl, Error, HidTransport};

fn stock_flash() -> Vec<u8> {
    sim::stock_flash()
}

fn take_backup(f: &mut SimDongle) -> Backup {
    dongle::backup(f, &mut |_| {}).unwrap()
}

fn image(product: &str) -> Vec<u8> {
    sim::product_image(product, "v0.1.0-test")
}

fn our_image() -> Vec<u8> {
    let built = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../build/fw/firmware/gateway-dongle-app.bin");
    std::fs::read(built).unwrap_or_else(|_| image("gateway-dongle"))
}

#[test]
fn backup_reads_everything_and_resets() {
    let stock = stock_flash();
    let mut f = SimDongle::with_flash(stock.clone());
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
        let mut f = SimDongle::with_flash(stock.clone());
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
        let mut f = SimDongle::with_flash(stock.clone());
        let b = take_backup(&mut f);
        dongle::flash(&mut f, &our_image(), &b, &FlashOptions { fast: true, nv: choice }, &mut |_| {}).unwrap();
        assert_eq!(&f.flash[0x1E000..0x1F000], &want[..]);
    }
}

#[test]
fn restore_puts_stock_back() {
    let stock = stock_flash();
    let mut f = SimDongle::with_flash(stock.clone());
    let b = take_backup(&mut f);
    dongle::flash(&mut f, &our_image(), &b, &FlashOptions::default(), &mut |_| {}).unwrap();
    f.replug();
    dongle::restore(&mut f, &b, false, &mut |_| {}).unwrap();
    assert_eq!(f.flash, stock);
}

#[test]
fn refuses_another_chip_or_page_31() {
    let stock = stock_flash();
    let mut f = SimDongle::with_flash(stock.clone());
    let b = take_backup(&mut f);
    f.chip_id = [0x20, 0x01, 0x80, 0x01];
    assert!(matches!(
        dongle::flash(&mut f, &our_image(), &b, &FlashOptions::default(), &mut |_| {}),
        Err(Error::Refused(_))
    ));
    let mut f = SimDongle::with_flash(stock.clone());
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
        let mut f = SimDongle::with_flash(stock.clone());
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
    let mut f = SimDongle::with_flash(stock.clone());
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
    let mut f = SimDongle::with_flash(stock.clone());
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

#[test]
fn sim_ends_in_normal_mode_or_needs_a_replug() {
    let mut d = SimDongle::stock();
    assert!(!d.runs_ows());
    let b = take_backup(&mut d);
    assert!(dongle::inquiry(&mut d).unwrap(), "normal mode after the backup's reset");
    dongle::flash(&mut d, &sim::gateway_image("v0.1.0"), &b, &FlashOptions::default(), &mut |_| {}).unwrap();
    assert_eq!(d.ows_version().as_deref(), Some("v0.1.0"));
    assert!(nv::is_ows(d.nv_page()));
    d.needs_replug = true;
    dongle::restore(&mut d, &b, true, &mut |_| {}).unwrap();
    assert!(!dongle::inquiry(&mut d).unwrap(), "stuck until replugged");
    d.replug();
    assert!(dongle::inquiry(&mut d).unwrap() && !d.runs_ows());
    assert_eq!(d.flash, sim::stock_flash());
}

#[test]
fn sim_ows_is_what_a_flash_leaves() {
    let mut a = SimDongle::stock();
    let b = take_backup(&mut a);
    dongle::flash(&mut a, &sim::gateway_image("v9"), &b, &FlashOptions::default(), &mut |_| {}).unwrap();
    assert_eq!(&a.flash[..0x1E000], &SimDongle::ows("v9").flash[..0x1E000]);
    assert_eq!(a.nv_page(), SimDongle::ows("v9").nv_page());
    let mut gone = SimDongle::stock();
    gone.fail_io = Some(io::ErrorKind::NotFound);
    assert!(matches!(dongle::backup(&mut gone, &mut |_| {}), Err(Error::Io(_))));
}
