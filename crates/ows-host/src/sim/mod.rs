//! A simulated Wyze USB dongle, for tests of programs that use this crate (feature `sim`; wyzesense2mqtt-rs enables it
//! in its dev-dependencies). It is the one `tests/dongle.rs` checks the crate against: the CH554's HID framing, the
//! update mode (host command 0x4312), TI's ROM bootloader over a 128 KB flash (erase -> FF, programming only clears
//! bits), and Inquiry (0x4327 -> 0x4328) in normal mode, which it is in again after the bootloader's reset, unless
//! told to need a replug. It panics on a command the crate must never send (bank erase, memory write, set CCFG).
//!
//! ```ignore
//! let mut d = SimDongle::stock();                       // stock firmware, a stock sensor table on page 30
//! let b = dongle::backup(&mut d, &mut |_| {})?;
//! dongle::flash(&mut d, &sim::gateway_image("v0.1.0"), &b, &FlashOptions::default(), &mut |_| {})?;
//! assert!(dongle::inquiry(&mut d)?);                     // back in normal mode
//! assert!(d.runs_ows());
//! ```

use std::collections::VecDeque;
use std::io;
use std::time::Duration;

use crate::dongle::STOCK_BL_CONFIG;
use crate::{sbl, HidTransport};

/// The spare dongle 77A85A36's stock page 30 (reference/tests/vectors/nv): a network and three sensors, two of them on
/// one short address (the migration drops one).
pub const STOCK_PAGE30: &[u8; 4096] = include_bytes!("stock_page30.bin");
pub const CHIP_ID: [u8; 4] = [0x20, 0x01, 0x80, 0x00];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// Running its firmware: answers Inquiry; 0x4312 enters the update mode.
    Normal,
    /// The CH554 relays SBL packets.
    Update,
    /// After the bootloader's reset with `needs_replug`: answers nothing until [`SimDongle::replug`].
    Stuck,
}

pub struct SimDongle {
    pub flash: Vec<u8>,
    pub chip_id: [u8; 4],
    pub mode: Mode,
    /// Pages erased, in order.
    pub erased: Vec<usize>,
    /// Bootloader resets.
    pub resets: usize,
    /// Every SBL command received.
    pub log: Vec<u8>,
    /// Flip a bit of the byte programmed at this address (a write that does not verify).
    pub corrupt_at: Option<usize>,
    /// After the bootloader's reset, answer nothing until replugged (as a dongle that does not leave update mode).
    pub needs_replug: bool,
    /// The CC1310 answers no async command (a firmware that does not run): the CH554 still answers Inquiry (#70).
    pub radio_silent: bool,
    /// Fail read() with this I/O error kind (e.g. the device gone).
    pub fail_io: Option<io::ErrorKind>,
    out: VecDeque<Vec<u8>>,
    status: u8,
    download: (usize, usize),
}

/// A stock-like dongle flash: code with a vector table in pages 0-29 (no build info: stock), [`STOCK_PAGE30`], page 31
/// with the stock BL_CONFIG.
pub fn stock_flash() -> Vec<u8> {
    let mut f: Vec<u8> = (0..0x20000u32).map(|i| (i.wrapping_mul(2_654_435_761) >> 24) as u8).collect();
    f[0..4].copy_from_slice(&0x2000_5000u32.to_le_bytes());
    f[4..8].copy_from_slice(&0x101u32.to_le_bytes());
    f[0x1E000..0x1F000].copy_from_slice(STOCK_PAGE30);
    f[0x1FFD8..0x1FFDC].copy_from_slice(&STOCK_BL_CONFIG);
    f
}

/// An image that passes `image::check_product(_, "gateway-dongle")`: a vector table, the build info block with this
/// version, some code, an FF stretch, a tail that is not whole words. Not a firmware that runs.
pub fn gateway_image(version: &str) -> Vec<u8> {
    product_image("gateway-dongle", version)
}

/// The same for another product (e.g. "gateway-contact-v1": an image the dongle must refuse).
pub fn product_image(product: &str, version: &str) -> Vec<u8> {
    let mut img: Vec<u8> = (0..20000u32).map(|i| (i * 7 + 3) as u8).collect();
    img[0..4].copy_from_slice(&0x2000_5000u32.to_le_bytes());
    img[4..8].copy_from_slice(&0x0101u32.to_le_bytes());
    let mut bi = [0u8; 116];
    bi[0..4].copy_from_slice(b"OWSB");
    bi[4] = 1;
    bi[5] = 1;
    let mut put = |at: usize, len: usize, s: &str| {
        let b = s.as_bytes();
        let n = b.len().min(len - 1);
        bi[at..at + n].copy_from_slice(&b[..n]);
    };
    put(16, 24, product);
    put(40, 24, "Wyze Sense dongle");
    put(64, 12, "gateway");
    put(76, 40, version);
    img[0xC0..0xC0 + 116].copy_from_slice(&bi);
    img[1000..1100].fill(0xFF);
    img.extend(std::iter::repeat(0xFF).take(2000));
    img.extend_from_slice(&[1, 2, 3]);
    img
}

impl SimDongle {
    pub fn with_flash(flash: Vec<u8>) -> SimDongle {
        assert_eq!(flash.len(), 0x20000, "a 128 KB flash");
        SimDongle {
            flash,
            chip_id: CHIP_ID,
            mode: Mode::Normal,
            erased: vec![],
            resets: 0,
            log: vec![],
            corrupt_at: None,
            needs_replug: false,
            radio_silent: false,
            fail_io: None,
            out: VecDeque::new(),
            status: 0x40,
            download: (0, 0),
        }
    }

    /// Stock firmware and a stock sensor table ([`stock_flash`]).
    pub fn stock() -> SimDongle {
        SimDongle::with_flash(stock_flash())
    }

    /// Our gateway ([`gateway_image`] `version`) in pages 0-29 (the pages after it as stock left them), page 30 the stock table migrated to our format, page
    /// 31 stock: a dongle flashed by this crate.
    pub fn ows(version: &str) -> SimDongle {
        let mut f = stock_flash();
        let img = gateway_image(version);
        f[..img.len().div_ceil(0x1000) * 0x1000].fill(0xFF); // the pages the image covers, as a flash erases them
        f[..img.len()].copy_from_slice(&img);
        let page = crate::nv::import(&crate::nv::export(STOCK_PAGE30)).expect("the stock table migrates");
        f[0x1E000..0x1F000].copy_from_slice(&page);
        SimDongle::with_flash(f)
    }

    /// Whether pages 0-29 hold one of our images (build info), and its version.
    pub fn ows_version(&self) -> Option<String> {
        crate::image::build_info(&self.flash[..crate::image::APP_END]).map(|b| b.version)
    }

    pub fn runs_ows(&self) -> bool {
        self.ows_version().is_some()
    }

    /// Page 30 now.
    pub fn nv_page(&self) -> &[u8] {
        &self.flash[0x1E000..0x1F000]
    }

    /// Unplugged and plugged in: normal mode, nothing pending.
    pub fn replug(&mut self) {
        self.mode = Mode::Normal;
        self.out.clear();
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
        let be = |b: &[u8]| u32::from_be_bytes(b[..4].try_into().unwrap()) as usize;
        match cmd {
            sbl::CMD_AUTO_BAUD | sbl::CMD_PING => Some(vec![]),
            sbl::CMD_GET_CHIP_ID => Some(self.chip_id.to_vec()),
            sbl::CMD_GET_STATUS => Some(vec![self.status]),
            sbl::CMD_RESET => {
                self.resets += 1;
                self.mode = if self.needs_replug { Mode::Stuck } else { Mode::Normal };
                Some(vec![])
            }
            sbl::CMD_CRC32 => {
                let (a, n) = (be(p), be(&p[4..]));
                // outside the flash (CCFG, FCFG1 at 0x5000xxxx): no CRC, as a chip may not give one
                self.flash.get(a..a + n).map(|d| crc32fast::hash(d).to_be_bytes().to_vec())
            }
            sbl::CMD_MEMORY_READ => {
                let (a, n) = (be(p), 4 * p[5] as usize);
                Some(self.flash.get(a..a + n).map_or(vec![0; n], <[u8]>::to_vec))
            }
            sbl::CMD_SECTOR_ERASE => {
                let a = be(p);
                self.erased.push(a / 0x1000);
                self.flash[a..a + 0x1000].fill(0xFF);
                Some(vec![])
            }
            sbl::CMD_DOWNLOAD => {
                self.download = (be(p), be(&p[4..]));
                Some(vec![])
            }
            sbl::CMD_SEND_DATA => {
                let (a, left) = self.download;
                for (i, &b) in p.iter().enumerate() {
                    let b = if self.corrupt_at == Some(a + i) { b ^ 0x10 } else { b };
                    self.flash[a + i] &= b;
                }
                self.download = (a + p.len(), left.saturating_sub(p.len()));
                Some(vec![])
            }
            c => panic!("the simulated dongle got SBL command {c:#x}, which must never be sent"),
        }
    }
}

impl HidTransport for SimDongle {
    fn write(&mut self, data: &[u8]) -> io::Result<()> {
        match self.mode {
            Mode::Stuck => return Ok(()),
            Mode::Normal => {
                if data == sbl::UPDATE_MODE_REQUEST {
                    self.mode = Mode::Update;
                    self.report(&[0x55, 0xAA, 0x43, 0x03, 0x13, 0x01, 0x58]);
                } else if data == sbl::INQUIRY {
                    self.report(&[0x55, 0xAA, 0x43, 0x04, 0x28, 0x01, 0x01, 0x6A]);
                } else if data == crate::dongle::GET_VERSION && !self.radio_silent {
                    let v = b"0.0.0.47 V1.8 Gateway GW3U";
                    let mut r = vec![0x55, 0xAA, 0x53, (v.len() + 3) as u8, 0x17];
                    r.extend_from_slice(v);
                    let sum = r.iter().fold(0u16, |a, &b| a.wrapping_add(b as u16));
                    r.extend_from_slice(&sum.to_be_bytes());
                    self.report(&r);
                }
                return Ok(());
            }
            Mode::Update => {}
        }
        if data.len() == 2 && data[0] == 0 {
            return Ok(()); // the host's ACK of a response
        }
        let (size, cks, cmd) = (data[0] as usize, data[1], data[2]);
        assert_eq!(size, data.len(), "SBL packet size");
        let payload = &data[3..size];
        assert_eq!(cks, payload.iter().fold(cmd, |a, &b| a.wrapping_add(b)), "SBL checksum");
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
        if let Some(k) = self.fail_io {
            return Err(io::Error::new(k, "simulated I/O failure"));
        }
        let r = self.out.pop_front().ok_or_else(|| io::Error::new(io::ErrorKind::TimedOut, "nothing to read"))?;
        buf[..r.len()].copy_from_slice(&r);
        Ok(r.len())
    }
}
