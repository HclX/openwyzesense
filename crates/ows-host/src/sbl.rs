//! The dongle's update mode and TI's ROM serial bootloader (SBL) over its USB HID, as `tools/dongle_sbl.py` (and
//! WyzeSenseUpdater before it) does: host command 0x4312 puts the dongle in update mode; from then on each HID report
//! is one SBL packet (size, checksum, command, data), which the dongle's CH554 relays to the CC1310's UART.
//!
//! Only reading commands are public. Erase and download exist only inside the crate, and every one is checked here,
//! at the last step before the chip: pages 0-29, and page 30 only while [`Sbl::nv_write`] runs; never page 31 (code
//! and the CCFG, whose BL_CONFIG keeps the bootloader reachable). Bank erase (0x2C), memory write (0x2B) and set CCFG
//! (0x2D) are not in this crate.

use std::time::Duration;

use crate::{Error, HidTransport};

pub const CMD_PING: u8 = 0x20;
pub const CMD_DOWNLOAD: u8 = 0x21;
pub const CMD_GET_STATUS: u8 = 0x23;
pub const CMD_SEND_DATA: u8 = 0x24;
pub const CMD_RESET: u8 = 0x25;
pub const CMD_SECTOR_ERASE: u8 = 0x26;
pub const CMD_CRC32: u8 = 0x27;
pub const CMD_GET_CHIP_ID: u8 = 0x28;
pub const CMD_MEMORY_READ: u8 = 0x2A;
pub const CMD_AUTO_BAUD: u8 = 0x55;
const READ_ONLY: [u8; 7] =
    [CMD_PING, CMD_GET_STATUS, CMD_RESET, CMD_CRC32, CMD_GET_CHIP_ID, CMD_MEMORY_READ, CMD_AUTO_BAUD];

/// Host command 0x4312 (update mode), HID length byte first.
pub const UPDATE_MODE_REQUEST: [u8; 8] = [0x07, 0xAA, 0x55, 0x43, 0x03, 0x12, 0x01, 0x57];
/// Host command 0x4327 (inquiry), HID length byte first: the dongle answers 0x4328 in normal mode.
pub const INQUIRY: [u8; 8] = [0x07, 0xAA, 0x55, 0x43, 0x03, 0x27, 0x01, 0x6C];
pub const MAX_PACK_SIZE: usize = 0x3A;
pub const MAX_MEMREAD: usize = 63 * 4;
pub const PAGE: u32 = 0x1000;
/// Pages 0-29: what an image may be written to.
pub const APP_END: u32 = 0x1E000;
/// Page 30: the NV page, written only by [`Sbl::nv_write`].
pub const NV_PAGE: u32 = 0x1E000;
pub const FLASH_SIZE: u32 = 0x20000;
pub const COMMAND_RET_SUCCESS: u8 = 0x40;
const RSP_ACK: u16 = 0x00CC;

/// How long one HID read may take (dongle_sbl.py: 5 s).
pub const READ_TIMEOUT: Duration = Duration::from_secs(5);

fn checksum(cmd: u8, data: &[u8]) -> u8 {
    data.iter().fold(cmd, |a, &b| a.wrapping_add(b))
}

/// A response: refused (NAK), acknowledged without data, or data.
#[derive(Debug, PartialEq, Eq)]
enum Rsp {
    Nak,
    Ack,
    Data(Vec<u8>),
}

/// The bootloader, entered through the dongle's update mode.
pub struct Sbl<'a, T: HidTransport + ?Sized> {
    t: &'a mut T,
    nv_writing: bool,
    /// The dongle's reply to 0x4312.
    pub update_mode_reply: Vec<u8>,
    /// GET_CHIP_ID.
    pub chip_id: Vec<u8>,
}

impl<'a, T: HidTransport + ?Sized> Sbl<'a, T> {
    /// Update mode, then the bootloader (auto baud, ping, chip ID). The dongle stays in update mode until it is
    /// reset by [`Sbl::reset`] or unplugged.
    pub fn enter(t: &'a mut T) -> Result<Self, Error> {
        let mut s = Sbl { t, nv_writing: false, update_mode_reply: vec![], chip_id: vec![] };
        s.t.write(&UPDATE_MODE_REQUEST)?;
        s.update_mode_reply = s.read_report()?;
        if s.cmd(CMD_AUTO_BAUD, &[])? == Rsp::Nak {
            return Err(Error::Sbl("no bootloader (auto baud)".into()));
        }
        if s.cmd(CMD_PING, &[])? == Rsp::Nak {
            return Err(Error::Sbl("no bootloader (ping)".into()));
        }
        s.chip_id = match s.cmd(CMD_GET_CHIP_ID, &[])? {
            Rsp::Data(d) => d,
            _ => return Err(Error::Sbl("no chip ID".into())),
        };
        Ok(s)
    }

    pub fn chip_id_hex(&self) -> String {
        hex::encode(&self.chip_id)
    }

    /// One report's payload (its first byte is the length).
    fn read_report(&mut self) -> Result<Vec<u8>, Error> {
        let mut buf = [0u8; 64];
        let n = self.t.read(&mut buf, READ_TIMEOUT)?;
        if n == 0 {
            return Err(Error::Sbl("empty HID report".into()));
        }
        let len = (buf[0] as usize).min(n - 1);
        Ok(buf[1..1 + len].to_vec())
    }

    /// Erase and download only below page 30 (page 30 inside nv_write()); never page 31.
    fn writable(&self, addr: u32, size: u32) -> Result<(), Error> {
        let end = addr as u64 + size as u64;
        if size > 0 && end <= APP_END as u64 {
            return Ok(());
        }
        if self.nv_writing && addr >= NV_PAGE && size > 0 && end <= (NV_PAGE + PAGE) as u64 {
            return Ok(());
        }
        Err(Error::Refused(format!(
            "{addr:#x}+{size:#x}: only flash below {APP_END:#x} (pages 0-29) is ever written, and page 30 by \
             the NV write"
        )))
    }

    fn cmd(&mut self, cmd: u8, payload: &[u8]) -> Result<Rsp, Error> {
        let be = |b: &[u8]| u32::from_be_bytes(b[..4].try_into().unwrap());
        match cmd {
            CMD_SECTOR_ERASE => self.writable(be(payload), PAGE)?,
            CMD_DOWNLOAD => self.writable(be(payload), be(&payload[4..]))?,
            CMD_SEND_DATA => {}
            c if READ_ONLY.contains(&c) => {}
            c => return Err(Error::Refused(format!("command {c:#x} is not used by this crate"))),
        }
        let mut pkt = vec![(payload.len() + 3) as u8, checksum(cmd, payload), cmd];
        pkt.extend_from_slice(payload);
        self.t.write(&pkt)?;
        let rsp = self.read_report()?;
        if rsp.len() < 2 {
            return Err(Error::Sbl("invalid response".into()));
        }
        if u16::from_be_bytes([rsp[0], rsp[1]]) != RSP_ACK {
            return Ok(Rsp::Nak);
        }
        if rsp.len() == 2 {
            return Ok(Rsp::Ack);
        }
        if rsp.len() < 4 {
            return Err(Error::Sbl("invalid payload".into()));
        }
        let mut n = (rsp[2] as usize).saturating_sub(2);
        let want = rsp[3];
        let mut data = rsp[4..].to_vec();
        if n < data.len() {
            return Err(Error::Sbl("invalid payload length".into()));
        }
        n -= data.len();
        while n > 0 {
            let more = self.read_report()?;
            if more.is_empty() {
                return Err(Error::Sbl("empty continuation report".into()));
            }
            let k = more.len().min(n);
            data.extend_from_slice(&more[..k]);
            n -= k;
        }
        if checksum(0, &data) != want {
            self.t.write(&[0x00, 0x33])?;
            return Err(Error::Sbl(format!("response checksum differs (command {cmd:#x})")));
        }
        self.t.write(&[0x00, 0xCC])?;
        Ok(Rsp::Data(data))
    }

    fn ok(&mut self, cmd: u8, payload: &[u8]) -> Result<bool, Error> {
        Ok(self.cmd(cmd, payload)? != Rsp::Nak)
    }

    pub fn ping(&mut self) -> Result<bool, Error> {
        self.ok(CMD_PING, &[])
    }

    /// GET_STATUS is COMMAND_RET_SUCCESS.
    pub fn status_ok(&mut self) -> Result<bool, Error> {
        Ok(matches!(self.cmd(CMD_GET_STATUS, &[])?, Rsp::Data(d) if d == [COMMAND_RET_SUCCESS]))
    }

    /// The chip's CRC32 over a range (None if it does not answer).
    pub fn crc32(&mut self, addr: u32, size: u32) -> Result<Option<u32>, Error> {
        let mut p = addr.to_be_bytes().to_vec();
        p.extend_from_slice(&size.to_be_bytes());
        p.extend_from_slice(&[0; 4]);
        Ok(match self.cmd(CMD_CRC32, &p)? {
            Rsp::Data(d) if d.len() == 4 => Some(u32::from_be_bytes(d[..4].try_into().unwrap())),
            _ => None,
        })
    }

    /// Memory (flash, CCFG, FCFG1, registers), in 32-bit reads; `progress(bytes read)`.
    pub fn read(&mut self, mut addr: u32, size: usize, progress: &mut dyn FnMut(usize)) -> Result<Vec<u8>, Error> {
        if addr % 4 != 0 || size % 4 != 0 || size == 0 {
            return Err(Error::Sbl(format!("read {addr:#x}+{size:#x}: whole words only")));
        }
        let mut data = Vec::with_capacity(size);
        while data.len() < size {
            let n = (size - data.len()).min(MAX_MEMREAD);
            let mut p = addr.to_be_bytes().to_vec();
            p.extend_from_slice(&[1, (n / 4) as u8]);
            match self.cmd(CMD_MEMORY_READ, &p)? {
                Rsp::Data(d) if d.len() == n => data.extend_from_slice(&d),
                _ => return Err(Error::Sbl(format!("read failed at {addr:#010x}"))),
            }
            addr += n as u32;
            progress(data.len());
        }
        Ok(data)
    }

    /// The bootloader resets the chip: the dongle leaves update mode (often without a replug).
    pub fn reset(&mut self) -> Result<(), Error> {
        self.cmd(CMD_RESET, &[])?;
        Ok(())
    }

    pub(crate) fn erase_page(&mut self, addr: u32) -> Result<(), Error> {
        self.cmd(CMD_SECTOR_ERASE, &addr.to_be_bytes())?;
        if !self.status_ok()? {
            return Err(Error::Sbl(format!("erasing the page at {addr:#x} failed")));
        }
        Ok(())
    }

    /// Download `data` to `addr`: `fast` checks the status once per stretch instead of after every packet.
    pub(crate) fn download(
        &mut self,
        addr: u32,
        data: &[u8],
        fast: bool,
        progress: &mut dyn FnMut(usize),
    ) -> Result<(), Error> {
        let mut p = addr.to_be_bytes().to_vec();
        p.extend_from_slice(&(data.len() as u32).to_be_bytes());
        if !self.ok(CMD_DOWNLOAD, &p)? || !self.status_ok()? {
            return Err(Error::Sbl(format!("download at {addr:#x} refused")));
        }
        let (mut off, mut tries) = (0, 0);
        while off < data.len() {
            let chunk = &data[off..(off + MAX_PACK_SIZE).min(data.len())];
            if self.ok(CMD_SEND_DATA, chunk)? && (fast || self.status_ok()?) {
                off += chunk.len();
                tries = 0;
                progress(chunk.len());
            } else {
                tries += 1;
                if tries > 3 {
                    return Err(Error::Sbl(format!("sending data failed at {:#x}", addr as usize + off)));
                }
            }
        }
        if fast && !self.status_ok()? {
            return Err(Error::Sbl(format!("programming the stretch at {addr:#x} failed")));
        }
        Ok(())
    }

    /// Page 30 <- `page` (4096 bytes, our NV format or all FF), verified by the chip's CRC32.
    pub(crate) fn nv_write(&mut self, page: &[u8]) -> Result<(), Error> {
        if page.len() != PAGE as usize {
            return Err(Error::Sbl(format!("NV page is {} bytes, not 4096", page.len())));
        }
        self.nv_writing = true;
        let r = (|| -> Result<(), Error> {
            self.erase_page(NV_PAGE)?;
            for (start, data) in runs(page, true) {
                self.download(NV_PAGE + start as u32, data, true, &mut |_| {})?;
            }
            Ok(())
        })();
        self.nv_writing = false;
        r?;
        if self.crc32(NV_PAGE, PAGE)? != Some(crc32fast::hash(page)) {
            return Err(Error::Verify("page 30 CRC differs from what was written".into()));
        }
        Ok(())
    }
}

/// The stretches to send `(offset, bytes)`: the whole image, or (`skip_ff`) only what is not all FF (erased flash
/// reads FF already), in words, gaps under 256 bytes merged (a new download costs a round trip).
pub fn runs(image: &[u8], skip_ff: bool) -> Vec<(usize, &[u8])> {
    if !skip_ff {
        return vec![(0, image)];
    }
    let mut out: Vec<(usize, usize)> = Vec::new();
    for k in (0..image.len()).step_by(4) {
        if image[k..(k + 4).min(image.len())].iter().all(|&b| b == 0xFF) {
            continue;
        }
        match out.last_mut() {
            Some((a, n)) if k - (*a + *n) < 256 => *n = k + 4 - *a,
            _ => out.push((k, 4)),
        }
    }
    out.into_iter().map(|(a, n)| (a, &image[a..(a + n).min(image.len())])).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Never;
    impl HidTransport for Never {
        fn write(&mut self, _: &[u8]) -> std::io::Result<()> {
            panic!("a refused command reached the device")
        }
        fn read(&mut self, _: &mut [u8], _: Duration) -> std::io::Result<usize> {
            panic!("read")
        }
    }

    #[test]
    fn writes_only_below_page_30_and_page_30_only_in_the_nv_write() {
        let mut t = Never;
        let mut s = Sbl { t: &mut t, nv_writing: false, update_mode_reply: vec![], chip_id: vec![] };
        for (cmd, p) in [
            (CMD_SECTOR_ERASE, 0x1F000u32.to_be_bytes().to_vec()), // page 31
            (CMD_SECTOR_ERASE, NV_PAGE.to_be_bytes().to_vec()),    // page 30, outside nv_write
            (CMD_DOWNLOAD, [0x1DFFCu32.to_be_bytes(), 8u32.to_be_bytes()].concat()), // across into page 30
            (CMD_DOWNLOAD, [0u32.to_be_bytes(), 0u32.to_be_bytes()].concat()),
            (0x2B, vec![]), // memory write
            (0x2C, vec![]), // bank erase
            (0x2D, vec![]), // set CCFG
        ] {
            assert!(matches!(s.cmd(cmd, &p), Err(Error::Refused(_))), "{cmd:#x} {p:02x?}");
        }
        s.nv_writing = true;
        for (cmd, p) in [
            (CMD_SECTOR_ERASE, 0x1F000u32.to_be_bytes().to_vec()),
            (CMD_DOWNLOAD, [0x1EFFCu32.to_be_bytes(), 8u32.to_be_bytes()].concat()), // into page 31
        ] {
            assert!(matches!(s.cmd(cmd, &p), Err(Error::Refused(_))), "{cmd:#x} {p:02x?}");
        }
        assert!(s.writable(NV_PAGE, PAGE).is_ok());
        s.nv_writing = false;
        assert!(s.writable(0, APP_END).is_ok());
    }
}
