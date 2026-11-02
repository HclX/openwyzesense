//! A [`HidTransport`] over Linux /dev/hidrawN, and finding the dongle (USB 1a86:e024) in sysfs, as
//! `tools/usb_dev.py` does.

use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::time::Duration;

use crate::host::ByteLink;
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

/// The gateway's host protocol over the dongle's hidraw: each report a length byte and the protocol bytes; written
/// as they are (no length byte).
pub struct HidrawHost(pub Hidraw);

impl ByteLink for HidrawHost {
    fn write(&mut self, data: &[u8]) -> std::io::Result<()> {
        self.0.f.write_all(data)
    }

    fn read(&mut self, timeout: Duration) -> std::io::Result<Vec<u8>> {
        let mut buf = [0u8; 64];
        match HidTransport::read(&mut self.0, &mut buf, timeout) {
            Ok(n) if n > 0 => Ok(buf[1..1 + (buf[0] as usize).min(n - 1)].to_vec()),
            Ok(_) => Ok(vec![]),
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => Ok(vec![]),
            Err(e) => Err(e),
        }
    }
}

/// The host protocol over a serial port (the LaunchPad's UART, a pty): 115200 8N1, raw.
pub struct Serial {
    f: File,
}

impl Serial {
    pub fn open(path: &Path) -> std::io::Result<Serial> {
        let f = OpenOptions::new().read(true).write(true).open(path)?;
        // SAFETY: termios on a valid descriptor, the struct zeroed then filled by tcgetattr
        unsafe {
            let mut t: libc::termios = std::mem::zeroed();
            if libc::tcgetattr(f.as_raw_fd(), &mut t) == 0 {
                libc::cfmakeraw(&mut t);
                t.c_cflag |= libc::CLOCAL | libc::CREAD;
                libc::cfsetispeed(&mut t, libc::B115200);
                libc::cfsetospeed(&mut t, libc::B115200);
                libc::tcsetattr(f.as_raw_fd(), libc::TCSANOW, &t);
                libc::tcflush(f.as_raw_fd(), libc::TCIOFLUSH);
            }
        }
        Ok(Serial { f })
    }
}

impl ByteLink for Serial {
    fn write(&mut self, data: &[u8]) -> std::io::Result<()> {
        self.f.write_all(data)
    }

    fn read(&mut self, timeout: Duration) -> std::io::Result<Vec<u8>> {
        let mut p = libc::pollfd { fd: self.f.as_raw_fd(), events: libc::POLLIN, revents: 0 };
        // SAFETY: one valid pollfd for the duration of the call
        let n = unsafe { libc::poll(&mut p, 1, timeout.as_millis().min(i32::MAX as u128) as i32) };
        if n <= 0 {
            return if n < 0 { Err(std::io::Error::last_os_error()) } else { Ok(vec![]) };
        }
        let mut buf = [0u8; 512];
        let k = self.f.read(&mut buf)?;
        Ok(buf[..k].to_vec())
    }
}
