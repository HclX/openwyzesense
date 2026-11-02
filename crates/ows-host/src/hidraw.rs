//! A [`HidTransport`] over Linux /dev/hidrawN, and finding the dongle (USB 1a86:e024) in sysfs, as
//! `tools/usb_dev.py` does.

use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::time::Duration;

use crate::HidTransport;

pub const WYZE_DONGLE: (u16, u16) = (0x1a86, 0xe024);

pub struct Hidraw {
    f: File,
}

impl Hidraw {
    pub fn open(path: &Path) -> std::io::Result<Hidraw> {
        Ok(Hidraw { f: OpenOptions::new().read(true).write(true).open(path)? })
    }
}

impl HidTransport for Hidraw {
    fn write(&mut self, report: &[u8]) -> std::io::Result<()> {
        self.f.write_all(report)
    }

    fn read(&mut self, buf: &mut [u8], timeout: Duration) -> std::io::Result<usize> {
        let mut p = libc::pollfd { fd: self.f.as_raw_fd(), events: libc::POLLIN, revents: 0 };
        let ms = timeout.as_millis().min(i32::MAX as u128) as i32;
        // SAFETY: one valid pollfd for the duration of the call
        let n = unsafe { libc::poll(&mut p, 1, ms) };
        if n < 0 {
            return Err(std::io::Error::last_os_error());
        }
        if n == 0 {
            return Err(std::io::Error::new(std::io::ErrorKind::TimedOut, "HID read timeout"));
        }
        self.f.read(buf)
    }
}

fn read_trimmed(p: &Path) -> Option<String> {
    std::fs::read_to_string(p).ok().map(|s| s.trim().to_string())
}

fn matches(dev_dir: &Path, vid: u16, pid: u16) -> bool {
    let hx = |s: &str| u32::from_str_radix(s, 16).ok();
    for line in read_trimmed(&dev_dir.join("uevent")).unwrap_or_default().lines() {
        if let Some(v) = line.strip_prefix("HID_ID=") {
            let p: Vec<_> = v.split(':').collect();
            if p.len() == 3 && hx(p[1]) == Some(vid.into()) && hx(p[2]) == Some(pid.into()) {
                return true;
            }
        }
    }
    let resolved = dev_dir.canonicalize().unwrap_or_else(|_| dev_dir.to_path_buf());
    for anc in resolved.ancestors() {
        if let (Some(v), Some(p)) = (read_trimmed(&anc.join("idVendor")), read_trimmed(&anc.join("idProduct"))) {
            return hx(&v) == Some(vid.into()) && hx(&p) == Some(pid.into());
        }
    }
    false
}

/// The first /dev/hidrawN of this USB VID:PID (natural order), or None.
pub fn find(vid: u16, pid: u16, sysfs: &Path, dev_root: &Path) -> Option<PathBuf> {
    let mut names: Vec<String> = std::fs::read_dir(sysfs)
        .ok()?
        .filter_map(|e| e.ok()?.file_name().into_string().ok())
        .filter(|n| n.starts_with("hidraw"))
        .collect();
    names.sort_by_key(|n| n[6..].parse::<u32>().unwrap_or(u32::MAX));
    names.into_iter().find(|n| matches(&sysfs.join(n).join("device"), vid, pid)).map(|n| dev_root.join(n))
}

/// The Wyze dongle's /dev/hidrawN.
pub fn find_dongle() -> Option<PathBuf> {
    find(WYZE_DONGLE.0, WYZE_DONGLE.1, Path::new("/sys/class/hidraw"), Path::new("/dev"))
}
