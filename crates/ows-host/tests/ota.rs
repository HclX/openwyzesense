// OTA updates against a simulated gateway (as reference/tests/test_host.py's FakeLink, and the gateway's side of
// common/ota.h): it stages offers only with GW_FLAG_OTA on, asks for a slot image block by block for each sensor it
// has an offer for, checks the bytes, and reports. ows_host::ota::run over a host::Link on it, and the state machine.
use std::collections::VecDeque;
use std::io;
use std::time::{Duration, Instant};

use ows_host::host::{self, ByteLink, Link, Packet};
use ows_host::ota::{self, Event, Ota, Outcome, Targets};

fn slot_image(slot: usize, version: u32, len: usize) -> Vec<u8> {
    let mut code = vec![0u8; len];
    code[0..4].copy_from_slice(&0x2000_4000u32.to_le_bytes());
    let reset = ota::SLOT_BASES[slot] + ota::OFFSET as u32 + 0x41;
    code[4..8].copy_from_slice(&reset.to_le_bytes());
    for (i, b) in code.iter_mut().enumerate().skip(8) {
        *b = (i * 13 + slot) as u8;
    }
    let mut img = vec![0xFF; ota::OFFSET];
    for (k, v) in [
        ota::MAGIC,
        crc32fast::hash(b"Wyze Sense v1 contact"),
        version,
        len as u32,
        crc32fast::hash(&code),
        slot as u32,
    ]
    .into_iter()
    .enumerate()
    {
        img[4 * k..4 * k + 4].copy_from_slice(&v.to_le_bytes());
    }
    img.extend_from_slice(&code);
    img
}

fn builds(version: u32) -> [Vec<u8>; 2] {
    [slot_image(0, version, 3000), slot_image(1, version, 3100)]
}

/// A sensor taking an update: the slot it asks for, what it has got.
struct Sensor {
    mac: [u8; 8],
    slot: u8,
    got: Vec<u8>,
    want: usize,
    crc: u32,
    asked: bool,
}

struct FakeGateway {
    flags: u32,
    out: VecDeque<u8>,
    sensors: Vec<Sensor>,
    paired: Vec<[u8; 8]>,
    offers: Vec<Vec<u8>>,
    corrupt: bool,
    time_reqs: usize,
}

impl FakeGateway {
    fn new(paired: &[&[u8; 8]]) -> FakeGateway {
        FakeGateway {
            flags: host::FLAG_DEDUP,
            out: VecDeque::new(),
            sensors: vec![],
            paired: paired.iter().map(|m| **m).collect(),
            offers: vec![],
            corrupt: false,
            time_reqs: 0,
        }
    }

    fn reply(&mut self, cmd: u16, payload: &[u8]) {
        self.out.extend(host::build(cmd, payload, true));
    }

    fn ows(&mut self, sub: u8, payload: &[u8]) {
        let mut p = vec![sub];
        p.extend_from_slice(payload);
        self.reply(host::OWS_RSP, &p);
    }

    fn need(&mut self, i: usize) {
        let s = &mut self.sensors[i];
        let off = s.got.len();
        let n = (s.want - off).min(64) as u8;
        let mut p = s.mac.to_vec();
        p.push(s.slot);
        p.extend_from_slice(&(off as u32).to_be_bytes());
        p.push(n);
        s.asked = true;
        self.ows(host::OWS_SUB_OTA_NEED, &p);
    }

    fn command(&mut self, p: &Packet) {
        match p.cmd {
            host::INQUIRY => self.reply(host::INQUIRY + 1, &[1]),
            host::GET_SENSOR_COUNT => {
                let n = self.paired.len() as u8;
                self.reply(host::GET_SENSOR_COUNT + 1, &[n]);
            }
            host::GET_SENSOR_LIST => {
                for m in self.paired.clone() {
                    let mut item = m.to_vec();
                    item.push(1);
                    self.reply(host::SENSOR_LIST_ITEM, &item);
                }
            }
            host::OWS_CMD => self.ows_command(&p.payload),
            _ => {}
        }
    }

    fn ows_command(&mut self, p: &[u8]) {
        match p[0] {
            host::OWS_SUB_FLAGS => {
                if p.len() == 5 {
                    self.flags = u32::from_be_bytes(p[1..5].try_into().unwrap());
                }
                let mut r = self.flags.to_be_bytes().to_vec();
                r.extend_from_slice(&0x1Fu32.to_be_bytes());
                self.ows(host::OWS_SUB_FLAGS, &r);
                // now and then the gateway asks for the time: the link answers it
                self.time_reqs += 1;
                self.reply(host::TIME_SYNC_REQ, &[]);
            }
            host::OWS_SUB_OTA_OFFER => {
                self.offers.push(p[1..].to_vec());
                let mac: [u8; 8] = p[1..9].try_into().unwrap();
                if p[9..].iter().all(|&b| b == 0) {
                    self.ows(host::OWS_SUB_OTA_OFFER, &[0]); // withdrawn
                    return;
                }
                if self.flags & host::FLAG_OTA == 0 {
                    self.ows(host::OWS_SUB_OTA_OFFER, &[2]);
                    return;
                }
                self.ows(host::OWS_SUB_OTA_OFFER, &[0]);
                let size_b = u32::from_be_bytes(p[9 + 16..9 + 20].try_into().unwrap()) as usize;
                let crc_b = u32::from_be_bytes(p[9 + 20..9 + 24].try_into().unwrap());
                let macs: Vec<[u8; 8]> = if mac == ota::ANY { self.paired.clone() } else { vec![mac] };
                for m in macs {
                    // each sensor runs slot A, so it asks for the slot B build, header included
                    self.sensors.push(Sensor {
                        mac: m,
                        slot: 1,
                        got: vec![],
                        want: ota::OFFSET + size_b,
                        crc: crc_b,
                        asked: false,
                    });
                    let i = self.sensors.len() - 1;
                    self.need(i);
                }
            }
            host::OWS_SUB_OTA_DATA => {
                let off = u32::from_be_bytes(p[1..5].try_into().unwrap()) as usize;
                let corrupt = self.corrupt;
                let Some(i) = self.sensors.iter().position(|s| s.asked && s.got.len() == off) else { return };
                let s = &mut self.sensors[i];
                s.got.extend_from_slice(&p[5..]);
                if corrupt && off <= 300 && 300 < s.got.len() {
                    s.got[300] ^= 1; // once, in the code (the CRC covers what follows the header)
                }
                if s.got.len() % 64 != 0 && s.got.len() < s.want {
                    return; // the rest of this block is coming
                }
                s.asked = false;
                if s.got.len() < s.want {
                    self.need(i);
                } else {
                    let s = self.sensors.remove(i);
                    let st = if crc32fast::hash(&s.got[ota::OFFSET..]) == s.crc { 0 } else { 1 };
                    let mut r = s.mac.to_vec();
                    r.push(st);
                    r.extend_from_slice(&7u32.to_be_bytes());
                    self.ows(host::OWS_SUB_OTA_DONE, &r);
                }
            }
            _ => {}
        }
    }
}

impl ByteLink for FakeGateway {
    fn write(&mut self, data: &[u8]) -> io::Result<()> {
        let p = host::parse(data).expect("one whole packet per write");
        assert!(!p.to_host);
        if p.cmd == host::ACK || p.cmd == host::TIME_SYNC {
            return Ok(());
        }
        self.command(&p);
        Ok(())
    }

    fn read(&mut self, _: Duration) -> io::Result<Vec<u8>> {
        // a few bytes at a time, as a serial port gives them
        let n = self.out.len().min(23);
        Ok(self.out.drain(..n).collect())
    }
}

#[test]
fn run_updates_named_sensors_and_puts_the_flags_back() {
    let mut gw = FakeGateway::new(&[b"77A8C793", b"77C68193"]);
    let mut link = Link::new(&mut gw);
    let mut events = vec![];
    let res = ota::run(
        &mut link,
        builds(7),
        Targets::Macs(vec![*b"77a8c793", *b"77C68193"]),
        Duration::from_secs(5),
        &mut |e| events.push(e.clone()),
    )
    .unwrap();
    assert_eq!(res.values().copied().collect::<Vec<_>>(), vec![Outcome::Done(0), Outcome::Done(0)]);
    assert!(events.contains(&Event::Staged(*b"77A8C793")), "MACs upper-cased");
    assert!(events.iter().any(|e| matches!(e, Event::Progress { slot: 1, .. })));
    assert_eq!(gw.flags, host::FLAG_DEDUP, "flags as they were");
    assert_eq!(gw.offers.last().unwrap(), &[ota::ANY.to_vec(), vec![0; 24]].concat(), "offers withdrawn at the end");
    assert!(gw.time_reqs > 0);
}

#[test]
fn run_any_ends_when_every_paired_sensor_is_done() {
    let mut gw = FakeGateway::new(&[b"77A8C793", b"77C68193", b"77BD514D"]);
    let mut link = Link::new(&mut gw);
    let known = link.sensors().unwrap();
    assert_eq!(known.len(), 3);
    let t0 = Instant::now();
    let res = ota::run(&mut link, builds(8), Targets::Any { known }, Duration::from_secs(30), &mut |_| {}).unwrap();
    assert_eq!(res.len(), 3);
    assert!(res.values().all(|o| *o == Outcome::Done(0)));
    assert!(t0.elapsed() < Duration::from_secs(5), "ended as soon as all were ok");
}

#[test]
fn a_bad_byte_is_reported_by_the_sensor() {
    let mut gw = FakeGateway::new(&[b"77A8C793"]);
    gw.corrupt = true;
    let mut link = Link::new(&mut gw);
    let res =
        ota::run(&mut link, builds(9), Targets::Macs(vec![*b"77A8C793"]), Duration::from_secs(5), &mut |_| {}).unwrap();
    assert_eq!(res[b"77A8C793"], Outcome::Done(1));
    assert_eq!(ota::status_text(1), "bad CRC");
}

#[test]
fn more_than_eight_are_offered_as_others_finish() {
    // the state machine alone: 10 targets, 8 offers up front, the 9th and 10th as two report
    let macs: Vec<[u8; 8]> = (0..10).map(|i| format!("770000{i:02X}").as_bytes().try_into().unwrap()).collect();
    let now = Instant::now();
    let mut o = Ota::new(builds(3), Targets::Macs(macs.clone()), Duration::from_secs(60), now).unwrap();
    let first = o.start();
    assert_eq!(first.len(), 8);
    let rsp = |sub: u8, p: &[u8]| {
        let mut v = vec![sub];
        v.extend_from_slice(p);
        host::parse(&host::build(host::OWS_RSP, &v, true)).unwrap()
    };
    for _ in 0..8 {
        o.handle(&rsp(host::OWS_SUB_OTA_OFFER, &[0]), now);
    }
    let mut need = macs[0].to_vec();
    need.extend_from_slice(&[1, 0, 0, 0, 0x10, 96]);
    let step = o.handle(&rsp(host::OWS_SUB_OTA_NEED, &need), now);
    assert_eq!(step.send.len(), 3, "96 bytes in 32-byte pieces");
    let first_data = host::parse(&step.send[0]).unwrap();
    assert_eq!(&first_data.payload[1..5], &[0, 0, 0, 0x10]);
    assert_eq!(&first_data.payload[5..], &builds(3)[1][0x10..0x30]);
    let mut sent_offers = 0;
    for m in &macs[..8] {
        let mut d = m.to_vec();
        d.extend_from_slice(&[0, 0, 0, 0, 3]);
        let step = o.handle(&rsp(host::OWS_SUB_OTA_DONE, &d), now);
        sent_offers += step.send.len();
    }
    assert_eq!(sent_offers, 2);
    assert!(!o.finished(now));
    for m in &macs[8..] {
        o.handle(&rsp(host::OWS_SUB_OTA_OFFER, &[0]), now);
        let mut d = m.to_vec();
        d.extend_from_slice(&[0, 0, 0, 0, 3]);
        o.handle(&rsp(host::OWS_SUB_OTA_DONE, &d), now);
    }
    assert!(o.finished(now));
    assert!(o.results().values().all(|r| *r == Outcome::Done(0)));
}

#[test]
fn refused_without_the_ota_flag_and_timeouts() {
    let now = Instant::now();
    let mut o = Ota::new(builds(3), Targets::Any { known: vec![] }, Duration::from_secs(10), now).unwrap();
    o.start();
    let refuse = host::parse(&host::build(host::OWS_RSP, &[host::OWS_SUB_OTA_OFFER, 2], true)).unwrap();
    o.handle(&refuse, now);
    assert!(o.refused() && o.finished(now));

    let mut o = Ota::new(builds(3), Targets::Macs(vec![*b"77A8C793"]), Duration::from_secs(10), now).unwrap();
    o.start();
    assert!(!o.finished(now + Duration::from_secs(9)));
    assert!(o.finished(now + Duration::from_secs(10)));
    assert_eq!(o.results()[b"77A8C793"], Outcome::TimedOut);
}

#[test]
fn bad_builds_are_refused() {
    let good = builds(3);
    for bad in [
        [good[1].clone(), good[0].clone()],         // swapped
        [good[0].clone(), builds(4)[1].clone()],    // two versions
        [good[0][..300].to_vec(), good[1].clone()], // cut short
        [
            {
                let mut b = good[0].clone();
                b[400] ^= 1; // CRC
                b
            },
            good[1].clone(),
        ],
    ] {
        assert!(Ota::new(bad, Targets::Macs(vec![*b"77A8C793"]), Duration::from_secs(1), Instant::now()).is_err());
    }
}
