//! The host protocol between a host and the dongle / our gateway (docs/FACTS.md §9, `reference/wyzesense_ref/host.py`):
//!
//! ```text
//! packet: magic(2) type(1) b2(1) cmd(1) payload(b2 - 3) checksum(2, BE, 16-bit sum of all preceding bytes)
//! magic : AA 55 host -> gateway, 55 AA gateway -> host;  type: 0x43 sync, 0x53 async
//! ACK   : cmd 0xFF, b2 = low byte of the acknowledged command, no payload (7 bytes)
//! ```
//!
//! Replies use cmd + 1. Every async packet but an ACK is ACKed by the receiver. Our extensions go under 0x53F0
//! (host -> gateway) / 0x53F1 (gateway -> host), the first payload byte a sub-ID (`OWS_SUB_*`), at most 56 bytes.
//!
//! [`Link`] is a blocking host over a [`ByteLink`] (a serial port, the dongle's hidraw): it ACKs, answers time
//! requests, and keeps what it was not waiting for. Programs with their own read loop (wyzesense2mqtt-rs) use
//! [`build`] / [`Framer`] / [`Packet`] and feed packets to the [`crate::ota::Ota`] state machine themselves.

use std::collections::VecDeque;
use std::time::{Duration, Instant};

use crate::Error;

pub const MAGIC_TO_GATEWAY: [u8; 2] = [0xAA, 0x55];
pub const MAGIC_TO_HOST: [u8; 2] = [0x55, 0xAA];
pub const SYNC: u8 = 0x43;
pub const ASYNC: u8 = 0x53;

pub const INQUIRY: u16 = 0x4327;
pub const GET_SENSOR_COUNT: u16 = 0x532E;
pub const GET_SENSOR_LIST: u16 = 0x5330;
pub const SENSOR_LIST_ITEM: u16 = 0x5331;
pub const TIME_SYNC_REQ: u16 = 0x5332;
pub const TIME_SYNC: u16 = 0x5333;
pub const ALARM: u16 = 0x5319;
pub const ACK: u16 = 0x53FF;
pub const OWS_CMD: u16 = 0x53F0;
pub const OWS_RSP: u16 = 0x53F1;

pub const OWS_SUB_FLAGS: u8 = 0x01;
pub const OWS_SUB_BUILD_INFO: u8 = 0x02;
pub const OWS_SUB_NET_KEY: u8 = 0x07;
pub const OWS_SUB_OTA_OFFER: u8 = 0x10;
pub const OWS_SUB_OTA_NEED: u8 = 0x11;
pub const OWS_SUB_OTA_DATA: u8 = 0x12;
pub const OWS_SUB_OTA_DONE: u8 = 0x13;

pub const FLAG_DEDUP: u32 = 1 << 0;
// 1 << 1: reserved (was FIX_ZERO_MAC, removed in #31: the gateway drops a zero-MAC report, as stock)
pub const FLAG_OTA: u32 = 1 << 2;
pub const FLAG_ANNOUNCE: u32 = 1 << 3;
pub const FLAG_ROAM: u32 = 1 << 4;

/// One packet.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Packet {
    pub cmd: u16,
    pub payload: Vec<u8>,
    pub to_host: bool,
    /// For an ACK: the command it acknowledges.
    pub acked: Option<u16>,
}

impl Packet {
    pub fn is_async(&self) -> bool {
        self.cmd >> 8 == ASYNC as u16
    }

    /// An `OWS_RSP` with this sub-ID: the payload after it.
    pub fn ows(&self, sub: u8) -> Option<&[u8]> {
        (self.cmd == OWS_RSP && self.payload.first() == Some(&sub)).then(|| &self.payload[1..])
    }
}

fn sum16(b: &[u8]) -> u16 {
    b.iter().fold(0u16, |a, &x| a.wrapping_add(x as u16))
}

/// A packet host -> gateway (`to_host` false) or gateway -> host.
pub fn build(cmd: u16, payload: &[u8], to_host: bool) -> Vec<u8> {
    let mut b = if to_host { MAGIC_TO_HOST } else { MAGIC_TO_GATEWAY }.to_vec();
    b.extend_from_slice(&[(cmd >> 8) as u8, (payload.len() + 3) as u8, cmd as u8]);
    b.extend_from_slice(payload);
    let s = sum16(&b);
    b.extend_from_slice(&s.to_be_bytes());
    b
}

pub fn build_ack(cmd: u16, to_host: bool) -> Vec<u8> {
    let mut b = if to_host { MAGIC_TO_HOST } else { MAGIC_TO_GATEWAY }.to_vec();
    b.extend_from_slice(&[(cmd >> 8) as u8, cmd as u8, 0xFF]);
    let s = sum16(&b);
    b.extend_from_slice(&s.to_be_bytes());
    b
}

/// `0x53F0 [sub, payload...]`, host -> gateway.
pub fn ows(sub: u8, payload: &[u8]) -> Vec<u8> {
    let mut p = vec![sub];
    p.extend_from_slice(payload);
    build(OWS_CMD, &p, false)
}

/// One complete packet; None if malformed.
pub fn parse(p: &[u8]) -> Option<Packet> {
    if p.len() < 7
        || (p[..2] != MAGIC_TO_GATEWAY && p[..2] != MAGIC_TO_HOST)
        || (p[2] != SYNC && p[2] != ASYNC)
        || sum16(&p[..p.len() - 2]) != u16::from_be_bytes([p[p.len() - 2], p[p.len() - 1]])
    {
        return None;
    }
    let to_host = p[..2] == MAGIC_TO_HOST;
    let cmd = (p[2] as u16) << 8 | p[4] as u16;
    if p[4] == 0xFF {
        return Some(Packet { cmd, payload: vec![], to_host, acked: Some((p[2] as u16) << 8 | p[3] as u16) });
    }
    if p.len() != p[3] as usize + 4 {
        return None;
    }
    Some(Packet { cmd, payload: p[5..p.len() - 2].to_vec(), to_host, acked: None })
}

/// A byte stream (with garbage between packets, as a serial port can have) -> packets.
#[derive(Default)]
pub struct Framer {
    buf: Vec<u8>,
}

impl Framer {
    pub fn new() -> Framer {
        Framer::default()
    }

    /// Adds bytes, returns the complete, checksum-valid packets they finish.
    pub fn push(&mut self, data: &[u8]) -> Vec<Packet> {
        self.buf.extend_from_slice(data);
        let mut out = Vec::new();
        let mut i = 0;
        while i + 7 <= self.buf.len() {
            if self.buf[i..i + 2] == MAGIC_TO_GATEWAY || self.buf[i..i + 2] == MAGIC_TO_HOST {
                let n = if self.buf[i + 4] == 0xFF { 7 } else { self.buf[i + 3] as usize + 4 };
                if i + n > self.buf.len() {
                    break; // not all here yet
                }
                if let Some(p) = parse(&self.buf[i..i + n]) {
                    out.push(p);
                    i += n;
                    continue;
                }
            }
            i += 1;
        }
        self.buf.drain(..i);
        out
    }
}

/// The host side of a link to the gateway, as bytes: a serial port (115200 8N1), the dongle's hidraw (each read one
/// report: a length byte and the protocol bytes; [`crate::hidraw::HidrawHost`] strips it), a WebSocket.
pub trait ByteLink {
    fn write(&mut self, data: &[u8]) -> std::io::Result<()>;
    /// What arrived within `timeout` (empty: nothing).
    fn read(&mut self, timeout: Duration) -> std::io::Result<Vec<u8>>;
}

impl<T: ByteLink + ?Sized> ByteLink for &mut T {
    fn write(&mut self, data: &[u8]) -> std::io::Result<()> {
        (**self).write(data)
    }
    fn read(&mut self, timeout: Duration) -> std::io::Result<Vec<u8>> {
        (**self).read(timeout)
    }
}

/// A blocking host: ACKs async packets, answers time requests, keeps packets it was not waiting for.
pub struct Link<L: ByteLink> {
    pub io: L,
    framer: Framer,
    queue: VecDeque<Packet>,
    backlog: Vec<Packet>,
}

impl<L: ByteLink> Link<L> {
    pub fn new(io: L) -> Link<L> {
        Link { io, framer: Framer::new(), queue: VecDeque::new(), backlog: Vec::new() }
    }

    pub fn send(&mut self, cmd: u16, payload: &[u8]) -> Result<(), Error> {
        Ok(self.io.write(&build(cmd, payload, false))?)
    }

    pub fn send_raw(&mut self, pkt: &[u8]) -> Result<(), Error> {
        Ok(self.io.write(pkt)?)
    }

    /// The next packet from the gateway, or None within `timeout`.
    pub fn read(&mut self, timeout: Duration) -> Result<Option<Packet>, Error> {
        let end = Instant::now() + timeout;
        loop {
            if let Some(p) = self.queue.pop_front() {
                if !p.to_host {
                    continue; // our own, echoed
                }
                if p.is_async() && p.cmd != ACK {
                    self.io.write(&build_ack(p.cmd, false))?;
                }
                if p.cmd == TIME_SYNC_REQ {
                    let ms = std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .map_or(0, |d| d.as_millis() as u64);
                    self.send(TIME_SYNC, &ms.to_be_bytes())?;
                }
                return Ok(Some(p));
            }
            let now = Instant::now();
            if now >= end {
                return Ok(None);
            }
            let data = self.io.read((end - now).min(Duration::from_millis(200)))?;
            let pkts = self.framer.push(&data);
            self.queue.extend(pkts);
        }
    }

    /// Packets with this command (from the backlog first); others are kept.
    pub fn wait_for(
        &mut self,
        want: impl Fn(&Packet) -> bool,
        timeout: Duration,
        count: usize,
    ) -> Result<Vec<Packet>, Error> {
        let mut got = Vec::new();
        let mut i = 0;
        while i < self.backlog.len() && got.len() < count {
            if want(&self.backlog[i]) {
                got.push(self.backlog.remove(i));
            } else {
                i += 1;
            }
        }
        let end = Instant::now() + timeout;
        while got.len() < count {
            let now = Instant::now();
            if now >= end {
                break;
            }
            match self.read(end - now)? {
                None => break,
                Some(p) if want(&p) => got.push(p),
                Some(p) if p.cmd != ACK => self.backlog.push(p),
                Some(_) => {}
            }
        }
        Ok(got)
    }

    /// A command and its reply packets (cmd + 1; the sensor list: its items).
    pub fn call(&mut self, cmd: u16, payload: &[u8], timeout: Duration, replies: usize) -> Result<Vec<Packet>, Error> {
        self.send(cmd, payload)?;
        let want = if cmd == GET_SENSOR_LIST { SENSOR_LIST_ITEM } else { cmd + 1 };
        self.wait_for(|p| p.cmd == want, timeout, replies)
    }

    /// `0x53F0 [sub, payload]` -> the matching `0x53F1` payload after the sub-ID.
    pub fn ows_call(&mut self, sub: u8, payload: &[u8], timeout: Duration) -> Result<Option<Vec<u8>>, Error> {
        self.io.write(&ows(sub, payload))?;
        let got = self.wait_for(|p| p.ows(sub).is_some(), timeout, 1)?;
        Ok(got.into_iter().next().map(|p| p.payload[1..].to_vec()))
    }

    /// Our gateway's feature flags, set to `set` if given: (on, supported). None: no answer (a stock dongle).
    pub fn flags(&mut self, set: Option<u32>) -> Result<Option<(u32, u32)>, Error> {
        let p = set.map(|v| v.to_be_bytes().to_vec()).unwrap_or_default();
        Ok(self
            .ows_call(OWS_SUB_FLAGS, &p, Duration::from_secs(1))?
            .filter(|r| r.len() == 8)
            .map(|r| (u32::from_be_bytes(r[..4].try_into().unwrap()), u32::from_be_bytes(r[4..].try_into().unwrap()))))
    }

    /// Our gateway's secured-link network key (RAM only), set to `key` if given: whether a key is set. None: no answer
    /// (a stock dongle).
    pub fn net_key(&mut self, key: Option<[u8; 16]>) -> Result<Option<bool>, Error> {
        let p = key.map(|k| k.to_vec()).unwrap_or_default();
        Ok(self.ows_call(OWS_SUB_NET_KEY, &p, Duration::from_secs(1))?.filter(|r| r.len() == 1).map(|r| r[0] != 0))
    }

    /// The paired sensors' MACs (8 ASCII bytes each).
    pub fn sensors(&mut self) -> Result<Vec<[u8; 8]>, Error> {
        let t = Duration::from_secs(2);
        let n = match self.call(GET_SENSOR_COUNT, &[], t, 1)?.first() {
            Some(p) if !p.payload.is_empty() => p.payload[0] as usize,
            _ => return Err(Error::Host("no answer to the sensor count".into())),
        };
        if n == 0 {
            return Ok(vec![]);
        }
        let items = self.call(GET_SENSOR_LIST, &[n as u8], t, n)?;
        Ok(items.iter().filter(|p| p.payload.len() >= 8).map(|p| p.payload[..8].try_into().unwrap()).collect())
    }

    /// Whether the gateway answers (Inquiry, a few tries: one just reset may not yet).
    pub fn inquiry(&mut self) -> Result<bool, Error> {
        for _ in 0..5 {
            if !self.call(INQUIRY, &[], Duration::from_secs(1), 1)?.is_empty() {
                return Ok(true);
            }
        }
        Ok(false)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn build_parse_and_frame() {
        let p = build(OWS_CMD, &[0x01], false);
        assert_eq!(p, [0xAA, 0x55, 0x53, 0x04, 0xF0, 0x01, 0x02, 0x47]);
        let ack = build_ack(ALARM, true);
        assert_eq!(parse(&ack).unwrap().acked, Some(ALARM));
        let mut f = Framer::new();
        let r = build(OWS_RSP, &[0x01, 1, 2, 3, 4, 5, 6, 7, 8], true);
        let mut stream = vec![0x00, 0x13];
        stream.extend_from_slice(&r);
        stream.extend_from_slice(&ack);
        let (a, b) = stream.split_at(6);
        assert!(f.push(a).is_empty());
        let got = f.push(b);
        assert_eq!(got.len(), 2);
        assert_eq!(got[0].ows(OWS_SUB_FLAGS).unwrap(), &[1, 2, 3, 4, 5, 6, 7, 8]);
        assert!(got[0].to_host && got[1].cmd == ACK);
    }
}
