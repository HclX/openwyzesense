//! Sensor OTA updates through our gateway (docs/design/ota.md, `firmware/common/ota.h`), the host's side: what
//! `tools/host_link.py --ota` does (`ota_batch`), as a state machine without I/O, so a program with its own read loop
//! (wyzesense2mqtt-rs) drives it, and [`run`] for a blocking [`Link`].
//!
//! The host stages an offer for each sensor (`OWS_SUB_OTA_OFFER`: its MAC, or `********` for any sensor with these
//! builds' board, and the board ID, version, and each slot image's size and CRC); a sensor takes it within 5 minutes
//! of a short press of its button (GW_FLAG_OTA on); then the gateway asks for blocks (`OWS_SUB_OTA_NEED`), the host
//! sends them (`OWS_SUB_OTA_DATA`, 32 bytes each), and the sensor reports (`OWS_SUB_OTA_DONE`). At most 8 sensors
//! have an offer staged at a time; the next one is offered as one finishes.
//!
//! ```ignore
//! let mut ota = Ota::new(builds, Targets::Macs(vec![*b"77A8C793"]), Duration::from_secs(360), Instant::now())?;
//! for pkt in ota.start() { send(pkt) }
//! loop {  // each packet from the gateway (to_host):
//!     let step = ota.handle(&packet, Instant::now());
//!     for pkt in step.send { send(pkt) }
//!     for ev in step.events { show(ev) }
//!     if ota.finished(Instant::now()) { break }
//! }
//! send(ota.cancel());
//! ```

use std::collections::{BTreeMap, VecDeque};
use std::time::{Duration, Instant};

use crate::host::{
    self, ByteLink, Link, Packet, OWS_SUB_OTA_DATA, OWS_SUB_OTA_DONE, OWS_SUB_OTA_NEED, OWS_SUB_OTA_OFFER,
};
use crate::Error;

/// A slot image (tools/slot_image.py): a 256-byte header, then the code linked for slot A or B.
pub const MAGIC: u32 = 0x4953_574F; // "OWSI"
pub const OFFSET: usize = 0x100;
pub const SLOT_BASES: [u32; 2] = [0x01000, 0x0F000];
pub const SLOT_SIZE: usize = 0xE000;
const CHUNK: usize = 32;
const IN_FLIGHT: usize = 8;
pub const ANY: [u8; 8] = *b"********";

/// What a sensor reported (OTA_DONE_STATUS).
pub fn status_text(st: u8) -> &'static str {
    match st {
        0 => "ok",
        1 => "bad CRC",
        2 => "refused",
        3 => "gave up (no blocks)",
        4 => "flash failed / battery low",
        _ => "unknown status",
    }
}

fn le32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}

/// The slot an image's reset vector is linked for.
fn infer_slot(reset: u32, image_len: usize) -> Option<usize> {
    if reset & 1 == 0 {
        return None;
    }
    let entry = reset & !1;
    SLOT_BASES.iter().position(|&base| {
        let start = base + OFFSET as u32;
        entry >= start && (entry as u64) < (base as u64 + SLOT_SIZE as u64).min(start as u64 + image_len as u64)
    })
}

/// The header fields: (board_id, version, size, crc).
pub fn header(img: &[u8]) -> (u32, u32, u32, u32) {
    (le32(img, 4), le32(img, 8), le32(img, 12), le32(img, 16))
}

/// slot_image.validate_builds(): two images, for slot A and slot B, of one build (board ID, version), each whole
/// (size, CRC32) with a vector table linked for its slot.
pub fn validate_builds(builds: &[Vec<u8>; 2]) -> Result<(), Error> {
    let bad = |m: String| Err(Error::Ota(m));
    let mut meta = Vec::new();
    for (idx, img) in builds.iter().enumerate() {
        let label = ['A', 'B'][idx];
        if img.len() < OFFSET + 8 {
            return bad(format!("slot {label} image is too short ({} bytes)", img.len()));
        }
        let (board_id, version, size, crc) = header(img);
        let hdr_slot = le32(img, 20);
        if le32(img, 0) != MAGIC {
            return bad(format!("slot {label} image lacks the OWSI header"));
        }
        if size < 8 || size as usize > SLOT_SIZE - OFFSET || img.len() != OFFSET + size as usize {
            return bad(format!("slot {label} image size mismatch (header {size}, actual {})", img.len() - OFFSET));
        }
        if crc32fast::hash(&img[OFFSET..]) != crc {
            return bad(format!("slot {label} image CRC-32 mismatch"));
        }
        if hdr_slot != idx as u32 && hdr_slot != 0xFFFF_FFFF {
            return bad(format!("slot {label} argument holds an image stamped for slot {hdr_slot}"));
        }
        let (sp, reset) = (le32(img, OFFSET), le32(img, OFFSET + 4));
        if !(0x2000_0001..=0x2000_5000).contains(&sp) || sp & 3 != 0 {
            return bad(format!("slot {label} image has an invalid initial SP {sp:#010x}"));
        }
        if infer_slot(reset, size as usize) != Some(idx) {
            return bad(format!("slot {label} image's reset vector {reset:#010x} is not linked for slot {label}"));
        }
        meta.push((board_id, version));
    }
    if meta[0] != meta[1] {
        return bad(format!("slot A and B images disagree on (board ID, version): {:?} vs {:?}", meta[0], meta[1]));
    }
    Ok(())
}

/// Which sensors.
#[derive(Debug, Clone)]
pub enum Targets {
    /// These MACs (8 ASCII bytes each).
    Macs(Vec<[u8; 8]>),
    /// Any sensor of the builds' board that comes (`********`); `known`: the paired sensors, all of which have to
    /// report ok before it ends (else it runs to the timeout).
    Any { known: Vec<[u8; 8]> },
}

/// How one sensor's update ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    /// It reported: 0 ok, else [`status_text`].
    Done(u8),
    /// The gateway did not stage the offer (GW_FLAG_OTA off, or not our gateway).
    Refused,
    TimedOut,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    /// The offer for this MAC (or `********`) is staged: press the sensor's button briefly now.
    Staged([u8; 8]),
    Refused([u8; 8]),
    /// Bytes of a slot image sent so far.
    Progress {
        mac: [u8; 8],
        slot: u8,
        done: usize,
        total: usize,
    },
    Done {
        mac: [u8; 8],
        status: u8,
    },
}

/// What to do after a step: packets to send to the gateway (complete, host -> gateway), and what happened.
#[derive(Debug, Default)]
pub struct Step {
    pub send: Vec<Vec<u8>>,
    pub events: Vec<Event>,
}

pub struct Ota {
    builds: [Vec<u8>; 2],
    offer_body: [u8; 24],
    any: bool,
    known: Vec<[u8; 8]>,
    timeout: Duration,
    end: Instant,
    queue: VecDeque<[u8; 8]>,
    in_flight: Vec<[u8; 8]>,
    awaiting_reply: VecDeque<[u8; 8]>,
    results: BTreeMap<[u8; 8], Option<Outcome>>,
    last: BTreeMap<[u8; 8], (u8, u32)>,
    refused_all: bool,
}

impl Ota {
    /// `builds`: the slot A and slot B images of one version. `timeout`: how long to wait while nothing advances.
    pub fn new(builds: [Vec<u8>; 2], targets: Targets, timeout: Duration, now: Instant) -> Result<Ota, Error> {
        validate_builds(&builds)?;
        let mut body = [0u8; 24];
        let (board, version, ..) = header(&builds[0]);
        body[0..4].copy_from_slice(&board.to_be_bytes());
        body[4..8].copy_from_slice(&version.to_be_bytes());
        for (s, img) in builds.iter().enumerate() {
            let (_, _, size, crc) = header(img);
            body[8 + 8 * s..12 + 8 * s].copy_from_slice(&size.to_be_bytes());
            body[12 + 8 * s..16 + 8 * s].copy_from_slice(&crc.to_be_bytes());
        }
        let (any, known, queue) = match targets {
            Targets::Any { known } => (true, known, VecDeque::new()),
            Targets::Macs(macs) => {
                let mut q = VecDeque::new();
                for mut m in macs {
                    m.make_ascii_uppercase();
                    if !q.contains(&m) {
                        q.push_back(m);
                    }
                }
                if q.is_empty() {
                    return Err(Error::Ota("no sensor to update".into()));
                }
                (false, vec![], q)
            }
        };
        let results = queue.iter().map(|m| (*m, None)).collect();
        Ok(Ota {
            builds,
            offer_body: body,
            any,
            known,
            timeout,
            end: now + timeout,
            queue,
            in_flight: vec![],
            awaiting_reply: VecDeque::new(),
            results,
            last: BTreeMap::new(),
            refused_all: false,
        })
    }

    /// The board ID and version offered.
    pub fn build(&self) -> (u32, u32) {
        let (b, v, ..) = header(&self.builds[0]);
        (b, v)
    }

    fn offer(&mut self, mac: [u8; 8], step: &mut Step) {
        let mut p = mac.to_vec();
        p.extend_from_slice(&self.offer_body);
        step.send.push(host::ows(OWS_SUB_OTA_OFFER, &p));
        self.awaiting_reply.push_back(mac);
        if mac != ANY {
            self.in_flight.push(mac);
        }
    }

    /// The first offers (all of them for `Any`; up to 8 MACs, the rest as these finish).
    pub fn start(&mut self) -> Vec<Vec<u8>> {
        let mut step = Step::default();
        if self.any {
            self.offer(ANY, &mut step);
        } else {
            while self.in_flight.len() < IN_FLIGHT {
                let Some(m) = self.queue.pop_front() else { break };
                self.offer(m, &mut step);
            }
        }
        step.send
    }

    /// One packet from the gateway (others than ours are ignored: ACK and answer them as usual).
    pub fn handle(&mut self, p: &Packet, now: Instant) -> Step {
        let mut step = Step::default();
        if let Some(r) = p.ows(OWS_SUB_OTA_OFFER) {
            let Some(mac) = self.awaiting_reply.pop_front() else { return step };
            if r == [0] {
                step.events.push(Event::Staged(mac));
            } else {
                step.events.push(Event::Refused(mac));
                if mac == ANY {
                    self.refused_all = true;
                } else {
                    self.in_flight.retain(|m| *m != mac);
                    self.results.insert(mac, Some(Outcome::Refused));
                    self.next(&mut step);
                }
            }
        } else if let Some(r) = p.ows(OWS_SUB_OTA_NEED).filter(|r| r.len() == 14) {
            let mac: [u8; 8] = r[..8].try_into().unwrap();
            if !self.any && !self.in_flight.contains(&mac) {
                return step;
            }
            let (slot, off, n) = (r[8], u32::from_be_bytes(r[9..13].try_into().unwrap()), r[13] as usize);
            let Some(img) = self.builds.get(slot as usize) else { return step };
            let (off_u, end) = (off as usize, (off as usize + n).min(img.len()));
            let mut k = off_u;
            while k < end {
                let mut d = (k as u32).to_be_bytes().to_vec();
                d.extend_from_slice(&img[k..(k + CHUNK).min(end)]);
                step.send.push(host::ows(OWS_SUB_OTA_DATA, &d));
                k += CHUNK;
            }
            if self.last.get(&mac) != Some(&(slot, off)) {
                self.last.insert(mac, (slot, off));
                self.end = self.end.max(now + Duration::from_secs(60));
            }
            step.events.push(Event::Progress { mac, slot, done: off_u + n, total: img.len() });
        } else if let Some(r) = p.ows(OWS_SUB_OTA_DONE).filter(|r| r.len() == 13) {
            let mac: [u8; 8] = r[..8].try_into().unwrap();
            let st = r[8];
            if !self.any && !self.in_flight.contains(&mac) && !self.results.contains_key(&mac) {
                return step;
            }
            if !self.any || st == 0 || self.results.get(&mac) != Some(&Some(Outcome::Done(0))) {
                self.results.insert(mac, Some(Outcome::Done(st)));
            }
            step.events.push(Event::Done { mac, status: st });
            self.in_flight.retain(|m| *m != mac);
            self.next(&mut step);
            if !self.in_flight.is_empty() || (self.any && !self.all_known_ok()) {
                self.end = self.end.max(now + self.timeout.min(Duration::from_secs(120)));
            }
        }
        step
    }

    fn next(&mut self, step: &mut Step) {
        if let Some(m) = self.queue.pop_front() {
            self.offer(m, step);
        }
    }

    fn all_known_ok(&self) -> bool {
        !self.known.is_empty() && self.known.iter().all(|k| self.results.get(k) == Some(&Some(Outcome::Done(0))))
    }

    /// Whether it is over: every target reported (or was refused), every known sensor ok (`Any`), the gateway refused
    /// the `Any` offer, or nothing advanced for the timeout.
    pub fn finished(&self, now: Instant) -> bool {
        self.refused_all
            || now >= self.end
            || (!self.any && self.in_flight.is_empty() && self.queue.is_empty())
            || (self.any && self.all_known_ok())
    }

    /// Each sensor's outcome (TimedOut for those that did not report).
    pub fn results(&self) -> BTreeMap<[u8; 8], Outcome> {
        self.results.iter().map(|(m, o)| (*m, o.unwrap_or(Outcome::TimedOut))).collect()
    }

    /// Whether the gateway refused the `Any` offer.
    pub fn refused(&self) -> bool {
        self.refused_all
    }

    /// The packet that withdraws every staged offer: send it at the end, whatever happened.
    pub fn cancel() -> Vec<u8> {
        let mut p = ANY.to_vec();
        p.extend_from_slice(&[0; 24]);
        host::ows(OWS_SUB_OTA_OFFER, &p)
    }
}

/// An update over a blocking link: GW_FLAG_OTA on for it (and back as it was after), the offers, the blocks, the
/// results; the offers are withdrawn at the end. `events` sees what happens (Staged: press the buttons now).
pub fn run<L: ByteLink>(
    link: &mut Link<L>,
    builds: [Vec<u8>; 2],
    targets: Targets,
    timeout: Duration,
    events: &mut dyn FnMut(&Event),
) -> Result<BTreeMap<[u8; 8], Outcome>, Error> {
    let mut ota = Ota::new(builds, targets, timeout, Instant::now())?;
    let (on, _) = link.flags(None)?.ok_or_else(|| Error::Ota("no answer to the flags: not our gateway?".into()))?;
    link.flags(Some(on | host::FLAG_OTA))?;
    let r = (|| -> Result<_, Error> {
        for p in ota.start() {
            link.send_raw(&p)?;
        }
        while !ota.finished(Instant::now()) {
            let Some(p) = link.read(Duration::from_millis(500))? else { continue };
            let step = ota.handle(&p, Instant::now());
            for s in &step.send {
                link.send_raw(s)?;
            }
            for e in &step.events {
                events(e);
            }
        }
        Ok(())
    })();
    let _ = link.send_raw(&Ota::cancel());
    let _ = link.flags(Some(on));
    r?;
    if ota.refused() {
        return Err(Error::Ota("the gateway refused the offer".into()));
    }
    Ok(ota.results())
}
