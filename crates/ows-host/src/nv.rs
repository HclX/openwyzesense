//! The NV page (flash page 30) in JSON (#61, docs/nv-format.md): a stock dongle's page or one of ours exported to
//! the validated JSON model, and that model imported into our format. A port of `reference/wyzesense_ref/nvows.py`
//! and `nvpage.py` (and `tools/nv_json.py`'s choices); the golden vectors in `reference/tests/vectors/nv/golden`
//! hold both to the same output, byte for byte.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

use crate::Error;

pub const PAGE_SIZE: usize = 0x1000;
pub const MAGIC: &[u8; 4] = b"OWNV";
pub const VERSION: u8 = 1;
pub const HEADER: [u8; 8] = [b'O', b'W', b'N', b'V', VERSION, 0xFF, 0xFF, 0xFF];
const MARKER: u8 = 0xA5;
const LIVE: u8 = 0xFF;
const DELETED: u8 = 0x00;
const REC: usize = 12;
pub const MAX_DATA: usize = 64;
pub const ITEM_NETWORK: u16 = 1;
pub const ITEM_COUNT: u16 = 4;
pub const ITEM_SENSOR: u16 = 5;
pub const ITEM_JOIN: u16 = 0x20;
pub const ITEM_CFG: u16 = 0x21;
/// The user's house network: a sensor is never made to join it.
pub const HOUSE_PAN: u16 = 0x845B;
pub const JSON_FORMAT: &str = "openwyzesense-nv-json";
pub const MAX_DEBOUNCE_MS: u32 = 5000;

/// One record: (item, sub) -> data.
pub type Items = BTreeMap<(u16, u16), Vec<u8>>;

fn u16le(b: &[u8]) -> u16 {
    u16::from_le_bytes([b[0], b[1]])
}

// --- the stock page (TI NV-on-chip): data, then an 8-byte header, parsed from the last header backwards ---

/// A stock page's records `(offset, item, sub, valid, data)`, oldest first.
pub fn stock_records(page: &[u8]) -> Vec<(usize, u16, u16, bool, Vec<u8>)> {
    let mut end = page.len() - page.iter().rev().take_while(|&&b| b == 0xFF).count();
    end += (4 - end % 4) % 4;
    let mut out = Vec::new();
    while end >= 12 && end <= page.len() {
        let h = &page[end - 8..end];
        if h[0] != 0x06 {
            break;
        }
        let (item, sub, n) = (u16le(&h[2..]), u16le(&h[4..]), u16le(&h[6..]) as usize);
        if end < 8 + n || end - 8 - n < 4 {
            break;
        }
        let start = end - 8 - n;
        out.push((start, item, sub, h[1] & 0x40 != 0, page[start..end - 8].to_vec()));
        end = start;
    }
    out.reverse();
    out
}

/// A stock page's valid records, the last one written winning.
pub fn stock_latest(page: &[u8]) -> Items {
    let mut out = Items::new();
    for (_, item, sub, valid, data) in stock_records(page) {
        if valid {
            out.insert((item, sub), data);
        } else {
            out.remove(&(item, sub));
        }
    }
    out
}

// --- our page ---

fn crc(item: u16, sub: u16, data: &[u8]) -> u32 {
    let mut h = crc32fast::Hasher::new();
    h.update(&item.to_le_bytes());
    h.update(&sub.to_le_bytes());
    h.update(&(data.len() as u16).to_le_bytes());
    h.update(data);
    h.finalize()
}

/// One record in our format.
pub fn record(item: u16, sub: u16, data: &[u8], live: bool) -> Result<Vec<u8>, Error> {
    if data.len() % 4 != 0 || data.len() > MAX_DATA {
        return Err(Error::Nv(format!("item {item}/{sub}: {} bytes: a multiple of 4, at most {MAX_DATA}", data.len())));
    }
    let mut r = vec![MARKER, if live { LIVE } else { DELETED }];
    r.extend_from_slice(&item.to_le_bytes());
    r.extend_from_slice(&sub.to_le_bytes());
    r.extend_from_slice(&(data.len() as u16).to_le_bytes());
    r.extend_from_slice(&crc(item, sub, data).to_le_bytes());
    r.extend_from_slice(data);
    Ok(r)
}

pub fn is_ows(page: &[u8]) -> bool {
    page.len() >= 8 && page[..8] == HEADER
}

/// Our page's good records `(offset, item, sub, live, data)` in write order (a bad CRC is skipped), up to the first
/// header that is not one.
pub fn records(page: &[u8]) -> Vec<(usize, u16, u16, bool, Vec<u8>)> {
    let mut out = Vec::new();
    if !is_ows(page) {
        return out;
    }
    let limit = page.len().min(PAGE_SIZE);
    let mut at = 8;
    while at + REC <= limit {
        let h = &page[at..at + REC];
        if h[0] != MARKER {
            break;
        }
        let (item, sub, n) = (u16le(&h[2..]), u16le(&h[4..]), u16le(&h[6..]) as usize);
        if n % 4 != 0 || n > MAX_DATA || at + REC + n > limit {
            break;
        }
        let data = &page[at + REC..at + REC + n];
        if u32::from_le_bytes([h[8], h[9], h[10], h[11]]) == crc(item, sub, data) {
            out.push((at, item, sub, h[1] == LIVE, data.to_vec()));
        }
        at += REC + n;
    }
    out
}

/// Our page's last live record of each (item, sub) (a deleted record clears earlier copies).
pub fn latest(page: &[u8]) -> Items {
    let mut out = Items::new();
    for (_, item, sub, live, data) in records(page) {
        if live {
            out.insert((item, sub), data);
        } else {
            out.remove(&(item, sub));
        }
    }
    out
}

/// Items -> a page in our format (sorted by item, sub), FF-padded to 4 KB.
pub fn encode(items: &Items) -> Result<Vec<u8>, Error> {
    let mut page = HEADER.to_vec();
    for (&(item, sub), data) in items {
        page.extend(record(item, sub, data, true)?);
    }
    if page.len() > PAGE_SIZE {
        return Err(Error::Nv(format!("{} bytes: more than a page", page.len())));
    }
    page.resize(PAGE_SIZE, 0xFF);
    Ok(page)
}

// --- the JSON model ---

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Source {
    pub layout: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Network {
    pub pan: String,
    pub short: String,
    pub ext: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Sensor {
    pub sub: u32,
    pub mac: String,
    pub short: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Join {
    #[serde(rename = "type")]
    pub kind: String,
    pub gw_ours: bool,
    pub pan: String,
    pub coord: String,
    pub short: String,
    pub gw_features: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Cfg {
    #[serde(rename = "type", default)]
    pub kind: Option<String>,
    #[serde(default)]
    pub invert: bool,
    #[serde(default)]
    pub pull: Option<String>,
    #[serde(default)]
    pub debounce_ms: u32,
}

/// What validation left out, and why. `mac` is set for a sensor the user has to pair again.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Dropped {
    pub what: String,
    pub why: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mac: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Unrecognised {
    pub item: u16,
    pub sub: u16,
    pub data: String,
}

/// The JSON document (`openwyzesense-nv-json` version 1), field for field as nvows.export() writes it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NvDoc {
    pub format: String,
    pub version: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub source: Option<Source>,
    #[serde(default)]
    pub network: Option<Network>,
    #[serde(default)]
    pub sensors: Vec<Sensor>,
    #[serde(default)]
    pub join: Option<Join>,
    #[serde(default)]
    pub cfg: Option<Cfg>,
    #[serde(default)]
    pub dropped: Vec<Dropped>,
    #[serde(default)]
    pub unrecognised: Vec<Unrecognised>,
}

impl NvDoc {
    /// The document as `json.dumps(doc, indent=2) + '\n'` writes it.
    pub fn to_json(&self) -> String {
        serde_json::to_string_pretty(self).expect("serialisable") + "\n"
    }

    pub fn from_json(text: &str) -> Result<NvDoc, Error> {
        serde_json::from_str(text).map_err(|e| Error::Nv(format!("NV JSON: {e}")))
    }

    /// One line for people: what is carried over.
    pub fn summary(&self) -> String {
        let layout = self.source.as_ref().map_or("json", |s| s.layout.as_str());
        let yes = |b: bool| if b { "yes" } else { "no" };
        format!(
            "{layout} -> ours: network {}, {} sensors, join {}{}",
            yes(self.network.is_some()),
            self.sensors.len(),
            yes(self.join.is_some()),
            if self.cfg.is_some() { ", cfg yes" } else { "" }
        )
    }
}

fn h16(v: u16) -> String {
    format!("{v:04X}")
}

/// Python's int(s, 16): an optional 0x, then hex digits.
fn parse_hex(s: &str) -> Result<u64, Error> {
    let t = s.trim();
    let t = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")).unwrap_or(t);
    u64::from_str_radix(&t.replace('_', ""), 16).map_err(|_| Error::Nv(format!("{s:?} is not a hex number")))
}

fn hex_mac(m: &[u8]) -> bool {
    m.len() == 8 && m.iter().all(|c| b"0123456789ABCDEF".contains(c)) && m != b"00000000"
}

pub fn network_data(pan: u16, short: u16, ext: u64) -> Vec<u8> {
    let mut d = Vec::with_capacity(16);
    d.extend_from_slice(&pan.to_le_bytes());
    d.extend_from_slice(&short.to_le_bytes());
    d.extend_from_slice(&ext.to_le_bytes());
    d.extend_from_slice(&[0x17, 0x00, 0xBE, 0xBE]);
    d
}

pub fn sensor_data(pan: u16, short: u16, mac: &[u8]) -> Vec<u8> {
    let mut r = vec![0u8; 24];
    r[0..2].copy_from_slice(&pan.to_le_bytes());
    r[2..4].copy_from_slice(&short.to_le_bytes());
    r[4..12].copy_from_slice(mac);
    r[17] = 0x01;
    r
}

fn pull_to_u8(p: &str) -> Option<u8> {
    match p {
        "up" => Some(0),
        "none" => Some(1),
        "down" => Some(2),
        "default" => Some(0xFF),
        _ => None,
    }
}

fn u8_to_pull(v: u8) -> Option<&'static str> {
    match v {
        0 => Some("up"),
        1 => Some("none"),
        2 => Some("down"),
        0xFF => Some("default"),
        _ => None,
    }
}

fn cfg_data(c: &Cfg) -> Result<Vec<u8>, Error> {
    let kind = match c.kind.as_deref() {
        None | Some("") | Some("00") | Some("default") => 0,
        Some(t) => parse_hex(t)?,
    };
    if kind > 0xFF {
        return Err(Error::Nv(format!("cfg type {:?} out of range", c.kind)));
    }
    let pull_name = c.pull.as_deref().filter(|p| !p.is_empty()).unwrap_or("default");
    let pull = pull_to_u8(pull_name)
        .ok_or_else(|| Error::Nv(format!("cfg pull {pull_name:?} must be one of up, none, down, default")))?;
    if c.debounce_ms > MAX_DEBOUNCE_MS {
        return Err(Error::Nv(format!("cfg debounce_ms {} out of range 0..{MAX_DEBOUNCE_MS}", c.debounce_ms)));
    }
    let db = (c.debounce_ms as u16).to_le_bytes();
    Ok(vec![kind as u8, c.invert as u8, pull, 0, db[0], db[1], 0, 0])
}

/// A page (stock, ours, or blank) -> the validated JSON model.
pub fn export(page: &[u8]) -> NvDoc {
    let (layout, items) = if is_ows(page) {
        ("ows", latest(page))
    } else if page.iter().all(|&b| b == 0xFF)
        || page.len() < 4
        || (page[..4] != [0, 0, 0x10, 0] && page[..4] != [0, 0, 0, 0])
    {
        ("blank", Items::new())
    } else {
        ("stock", stock_latest(page))
    };
    let mut out = NvDoc {
        format: JSON_FORMAT.into(),
        version: 1,
        source: Some(Source { layout: layout.into() }),
        network: None,
        sensors: vec![],
        join: None,
        cfg: None,
        dropped: vec![],
        unrecognised: vec![],
    };
    let drop = |out: &mut NvDoc, what: String, why: String| out.dropped.push(Dropped { what, why, mac: None });

    let mut pan = None;
    if let Some(net) = items.get(&(ITEM_NETWORK, 0)).filter(|n| n.len() >= 12) {
        let (p, s) = (u16le(&net[0..]), u16le(&net[2..]));
        let e = u64::from_le_bytes(net[4..12].try_into().unwrap());
        if p == 0 || p == 0xFFFF || s == 0xFFFE || s == 0xFFFF {
            drop(&mut out, "network".into(), format!("PAN {} / coordinator {} not usable", h16(p), h16(s)));
        } else {
            out.network = Some(Network { pan: h16(p), short: h16(s), ext: format!("{e:016X}") });
            pan = Some(p);
        }
    }
    let mut by_mac: BTreeMap<Vec<u8>, u16> = BTreeMap::new();
    let mut by_short: BTreeMap<u16, String> = BTreeMap::new();
    for (&(item, sub), d) in &items {
        if item != ITEM_SENSOR {
            continue;
        }
        let what = format!("sensor {sub}");
        if d.len() != 24 {
            drop(&mut out, what, format!("{} bytes, not 24", d.len()));
            continue;
        }
        let (p, s, mac) = (u16le(&d[0..]), u16le(&d[2..]), &d[4..12]);
        let mac_s = String::from_utf8_lossy(mac).into_owned();
        if !hex_mac(mac) {
            drop(&mut out, what, format!("MAC {} is not 8 hex digits", hex::encode(mac)));
        } else if !(0x0001..=0xFFFD).contains(&s) {
            drop(&mut out, what, format!("short address {} not usable", h16(s)));
        } else if pan != Some(p) {
            drop(&mut out, what, format!("PAN {} is not the network's", h16(p)));
        } else if let Some(other) = by_mac.get(mac) {
            drop(&mut out, what, format!("MAC {mac_s} already in sensor {other}"));
        } else if let Some(other) = by_short.get(&s) {
            // never two on one address (roaming owns by address): the user re-pairs this one
            let why = format!("shares short address {} with {other}: re-pair {mac_s} after the migration", h16(s));
            out.dropped.push(Dropped { what, why, mac: Some(mac_s) });
        } else {
            by_mac.insert(mac.to_vec(), sub);
            by_short.insert(s, mac_s.clone());
            out.sensors.push(Sensor { sub: sub as u32, mac: mac_s, short: h16(s) });
        }
    }
    if layout == "ows" {
        if let Some(j) = items.get(&(ITEM_JOIN, 0)) {
            let f = |k: usize| if j.len() >= k + 2 { u16le(&j[k..]) } else { 0 };
            let (p, c, s) = (f(2), f(4), f(6));
            if j.len() != 12 || p == 0 || p == 0xFFFF || p == HOUSE_PAN || !(0x0001..=0xFFFD).contains(&s) {
                drop(&mut out, "join".into(), format!("PAN {} / short address {} not usable", h16(p), h16(s)));
            } else {
                out.join = Some(Join {
                    kind: format!("{:02X}", j[0]),
                    gw_ours: j[1] & 1 != 0,
                    pan: h16(p),
                    coord: h16(c),
                    short: h16(s),
                    gw_features: h16(u16le(&j[10..])),
                });
            }
        }
        if let Some(c) = items.get(&(ITEM_CFG, 0)) {
            let db = if c.len() == 8 { u16le(&c[4..]) as i64 } else { -1 };
            if c.len() != 8 || u8_to_pull(c[2]).is_none() || db > MAX_DEBOUNCE_MS as i64 {
                drop(&mut out, "cfg".into(), format!("sensor cfg {} not usable", hex::encode(c)));
            } else {
                out.cfg = Some(Cfg {
                    kind: if c[0] != 0 { Some(format!("{:02X}", c[0])) } else { None },
                    invert: c[1] & 1 != 0,
                    pull: u8_to_pull(c[2]).map(String::from),
                    debounce_ms: db as u32,
                });
            }
        }
    }
    for (&(item, sub), d) in &items {
        let known = matches!(item, ITEM_NETWORK | ITEM_COUNT | ITEM_SENSOR)
            || (layout == "ows" && matches!(item, ITEM_JOIN | ITEM_CFG));
        if !known {
            out.unrecognised.push(Unrecognised { item, sub, data: hex::encode(d) });
        }
    }
    out
}

fn bad(msg: String) -> Error {
    Error::Nv(msg)
}

/// The JSON model -> a page in our format, every value checked (a join to the house PAN: never).
pub fn import(doc: &NvDoc) -> Result<Vec<u8>, Error> {
    if doc.format != JSON_FORMAT || doc.version != 1 {
        return Err(bad("not an openwyzesense-nv-json version 1 document".into()));
    }
    let mut items = Items::new();
    if let Some(net) = &doc.network {
        let (pan, net_short, ext) = (parse_hex(&net.pan)?, parse_hex(&net.short)?, parse_hex(&net.ext)?);
        if pan == 0 || pan >= 0xFFFF {
            return Err(bad(format!("network PAN {} not usable", net.pan)));
        }
        if net_short > 0xFFFD {
            return Err(bad(format!("network coordinator short address {} not usable", net.short)));
        }
        let (pan, net_short) = (pan as u16, net_short as u16);
        items.insert((ITEM_NETWORK, 0), network_data(pan, net_short, ext));
        if doc.sensors.len() > 255 {
            return Err(bad(format!("{} sensors: more than a page can hold", doc.sensors.len())));
        }
        let (mut subs, mut macs, mut shorts) = (Vec::new(), Vec::new(), Vec::new());
        for s in &doc.sensors {
            if s.sub > 0xFFFF || subs.contains(&s.sub) {
                return Err(bad(format!("sensor {}: invalid or duplicate sub-ID {}", s.mac, s.sub)));
            }
            let mac = s.mac.as_bytes();
            if !hex_mac(mac) || macs.contains(&mac) {
                return Err(bad(format!("sensor {}: invalid or duplicate MAC", s.mac)));
            }
            let short = parse_hex(&s.short)?;
            if !(0x0001..=0xFFFD).contains(&short) || short == net_short as u64 || shorts.contains(&short) {
                return Err(bad(format!("sensor {}: invalid or duplicate short address", s.mac)));
            }
            subs.push(s.sub);
            macs.push(mac);
            shorts.push(short);
            items.insert((ITEM_SENSOR, s.sub as u16), sensor_data(pan, short as u16, mac));
        }
        items.insert((ITEM_COUNT, 0), vec![doc.sensors.len() as u8, 0, 0, 0]);
    } else if !doc.sensors.is_empty() {
        return Err(bad("sensors without a network".into()));
    }
    if let Some(j) = &doc.join {
        let (pan, coord, short) = (parse_hex(&j.pan)?, parse_hex(&j.coord)?, parse_hex(&j.short)?);
        let (kind, feat) = (parse_hex(&j.kind)?, parse_hex(&j.gw_features)?);
        if pan == HOUSE_PAN as u64 {
            return Err(bad("a join to the house network (PAN 845B): never".into()));
        }
        if pan == 0 || pan >= 0xFFFF {
            return Err(bad(format!("join PAN {} not usable", j.pan)));
        }
        if coord > 0xFFFD || !(0x0001..=0xFFFD).contains(&short) {
            return Err(bad(format!("join addresses {}/{} not usable", j.coord, j.short)));
        }
        if kind > 0xFF || feat > 0xFFFF {
            return Err(bad("join type or gw_features out of range".into()));
        }
        let mut d = vec![kind as u8, j.gw_ours as u8];
        for v in [pan, coord, short] {
            d.extend_from_slice(&(v as u16).to_le_bytes());
        }
        d.extend_from_slice(&[0, 0]);
        d.extend_from_slice(&(feat as u16).to_le_bytes());
        items.insert((ITEM_JOIN, 0), d);
    }
    if let Some(c) = &doc.cfg {
        let raw = cfg_data(c)?;
        if raw != [0, 0, 0xFF, 0, 0, 0, 0, 0] {
            items.insert((ITEM_CFG, 0), raw);
        }
    }
    encode(&items)
}

/// What to write to page 30 when flashing (#61).
#[derive(Debug, Clone)]
pub enum NvChoice {
    /// The current page exported, validated, and imported into our format (the default).
    Migrate,
    /// All FF: the firmware formats it on start.
    Wipe,
    /// Page 30 left as it is.
    Keep,
    /// This document imported.
    Json(Box<NvDoc>),
}

/// What the choice writes over `current`: `(page, doc)`. `page` None: nothing written; `doc`: the model carried
/// over (Migrate, Json), for the caller to show and keep (ows-flash saves it as nv.json in the backup).
pub fn page_for(choice: &NvChoice, current: &[u8]) -> Result<(Option<Vec<u8>>, Option<NvDoc>), Error> {
    match choice {
        NvChoice::Keep => Ok((None, None)),
        NvChoice::Wipe => Ok((Some(vec![0xFF; PAGE_SIZE]), None)),
        NvChoice::Migrate => {
            let doc = export(current);
            Ok((Some(import(&doc)?), Some(doc)))
        }
        NvChoice::Json(doc) => Ok((Some(import(doc)?), Some((**doc).clone()))),
    }
}
