//! OpenWyzeSense host tools in Rust (#42): what `tools/dongle_sbl.py` and `tools/nv_json.py` do, for programs such as
//! wyzesense2mqtt-rs that install, update and restore OpenWyzeSense firmware on a Wyze USB dongle.
//!
//! - [`HidTransport`]: the caller owns the device; the crate sends and receives HID reports through it
//!   ([`hidraw::Hidraw`] on Linux, feature `hidraw`).
//! - [`dongle`]: [`dongle::backup`], [`dongle::flash`], [`dongle::restore`], with progress callbacks.
//! - [`nv`]: page 30 (stock or ours) to the JSON model and back (#61).
//! - [`image`]: image checks and the build info block. [`manifest`]: the release manifest.
//! - [`sbl`]: the dongle's update mode and TI's ROM bootloader (reading commands public; writing only through
//!   [`dongle`]).
//!
//! Safety rules, as in the Python tools: page 31 (code + CCFG) is never erased or written; page 30 only by the NV
//! write; flashing needs this chip's backup; everything is verified before the chip is reset.

pub mod dongle;
#[cfg(all(feature = "hidraw", target_os = "linux"))]
pub mod hidraw;
pub mod image;
pub mod manifest;
pub mod nv;
pub mod sbl;
#[cfg(feature = "sim")]
pub mod sim;

use std::time::Duration;

/// The dongle's USB HID, owned by the caller (e.g. an opened /dev/hidrawN).
pub trait HidTransport {
    /// Writes one report, these bytes exactly (no report ID: the dongle has none; the first byte is a length).
    fn write(&mut self, report: &[u8]) -> std::io::Result<()>;
    /// Reads one report (up to 63 bytes, its first byte the payload length) into `buf`, returning its size.
    /// Nothing within `timeout`: an error of kind [`std::io::ErrorKind::TimedOut`].
    fn read(&mut self, buf: &mut [u8], timeout: Duration) -> std::io::Result<usize>;
}

impl<T: HidTransport + ?Sized> HidTransport for &mut T {
    fn write(&mut self, report: &[u8]) -> std::io::Result<()> {
        (**self).write(report)
    }
    fn read(&mut self, buf: &mut [u8], timeout: Duration) -> std::io::Result<usize> {
        (**self).read(buf, timeout)
    }
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("I/O: {0}")]
    Io(#[from] std::io::Error),
    /// The bootloader did not answer as expected.
    #[error("bootloader: {0}")]
    Sbl(String),
    /// A safety rule refused it (page 31, a wrong chip, a command not used).
    #[error("refused: {0}")]
    Refused(String),
    /// What was written or read does not check out. The chip was not reset.
    #[error("verify: {0}")]
    Verify(String),
    #[error("backup: {0}")]
    Backup(String),
    #[error("image: {0}")]
    Image(String),
    #[error("NV: {0}")]
    Nv(String),
    #[error("manifest: {0}")]
    Manifest(String),
}

pub const VERSION: &str = env!("CARGO_PKG_VERSION");
