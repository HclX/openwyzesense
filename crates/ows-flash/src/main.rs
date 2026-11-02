//! ows-flash: back up, flash and restore a Wyze USB dongle, and convert NV pages, with the ows-host crate (#42): the
//! same steps and checks as the Python dongle_sbl.py (replaced, #44) and tools/nv_json.py, in one binary.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use ows_host::dongle::{self, Backup, FlashOptions, Phase, Progress};
use ows_host::hidraw::{self, Hidraw, HidrawHost, Serial};
use ows_host::host::{ByteLink, Link};
use ows_host::nv::{self, NvChoice};
use ows_host::ota::{self, Targets};
use ows_host::{image, Error};

const USAGE: &str = "ows-flash (OpenWyzeSense) -- the Wyze USB dongle through its bootloader

  ows-flash backup DIR                                 the whole chip to DIR (read only); do this first
  ows-flash flash IMAGE --backup DIR --yes [--fast] [--nv migrate|wipe|keep|NV.json]
                                                       our gateway (gateway-dongle-app.bin) to pages 0-29; page 30:
                                                       migrate (default: the stock sensor table carried over, saved
                                                       as DIR/nv.json), wipe, keep, or a JSON to import
  ows-flash restore --backup DIR --yes [--fast]        the backup's pages 0-30: stock again
  ows-flash info IMAGE                                 an image's build info
  ows-flash ota MAC[,MAC...]|all SLOT_A.img SLOT_B.img [--seconds N]
                                                       update sensors over the air through our gateway (OTA on for
                                                       it, back as it was after); press each sensor's button briefly
                                                       once staged. all: every sensor of the images' board
  ows-flash nv export PAGE_OR_FLASH.bin [-o NV.json]   a page (4 KB, or page 30 of a 128 KB dump) to JSON
  ows-flash nv import NV.json -o PAGE.bin              JSON to a page in our format

  --device PATH            the dongle's /dev/hidrawN (default: the first USB 1a86:e024); ota: also a serial port
                           running our gateway (a LaunchPad's /dev/ttyACMn, a pty)
  --fast                   status per stretch, all-FF skipped, CRC-only verify (no read-back)

Page 31 (code and CCFG) is never written. Flashing needs this chip's backup, and resets the dongle only after
everything is verified; if it is not back in normal mode afterwards, unplug and replug it.";

struct Args {
    pos: Vec<String>,
    backup: Option<PathBuf>,
    device: Option<PathBuf>,
    nv: String,
    out: Option<PathBuf>,
    yes: bool,
    fast: bool,
    seconds: u64,
}

fn parse() -> Result<Args, String> {
    let mut a = Args {
        pos: vec![],
        backup: None,
        device: None,
        nv: "migrate".into(),
        out: None,
        yes: false,
        fast: false,
        seconds: 360,
    };
    let mut it = std::env::args().skip(1);
    while let Some(x) = it.next() {
        let mut val = |name: &str| it.next().ok_or(format!("{name} needs a value"));
        match x.as_str() {
            "--backup" => a.backup = Some(val("--backup")?.into()),
            "--device" => a.device = Some(val("--device")?.into()),
            "--nv" => a.nv = val("--nv")?,
            "-o" | "--out" => a.out = Some(val("-o")?.into()),
            "--yes" => a.yes = true,
            "--fast" => a.fast = true,
            "--seconds" => a.seconds = val("--seconds")?.parse().map_err(|_| "--seconds: a number")?,
            "-h" | "--help" => return Err(String::new()),
            "-V" | "--version" => {
                println!("ows-flash {}", ows_host::VERSION);
                std::process::exit(0)
            }
            s if s.starts_with('-') => return Err(format!("unknown option {s}")),
            _ => a.pos.push(x),
        }
    }
    Ok(a)
}

fn bar(p: Progress) {
    let name = match p.phase {
        Phase::Enter => "bootloader",
        Phase::Backup => "reading",
        Phase::Check => "checking",
        Phase::Erase => "erasing",
        Phase::Write => "writing",
        Phase::Verify => "verifying",
        Phase::Nv => "page 30",
        Phase::Reset => "reset",
        Phase::Done => "done",
    };
    match (p.done * 100).checked_div(p.total) {
        Some(pc) => eprint!("\r  {name:<10} {pc:>3}% ({}/{})   ", p.done, p.total),
        None => eprint!("\r  {name:<10}                        "),
    }
    if p.phase == Phase::Done {
        eprintln!();
    }
}

fn open(device: &Option<PathBuf>) -> Result<(PathBuf, Hidraw), String> {
    let path = device.clone().or_else(hidraw::find_dongle).ok_or(
        "no Wyze USB dongle (1a86:e024) on /dev/hidraw*: plug it in, install tools/udev/99-wyze-dongle.rules, or \
         pass --device /dev/hidrawN",
    )?;
    let dev = Hidraw::open(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok((path, dev))
}

fn finish(path: &Path) {
    std::thread::sleep(std::time::Duration::from_millis(500));
    let back = Hidraw::open(path).ok().and_then(|mut d| dongle::inquiry(&mut d).ok()).unwrap_or(false);
    if back {
        println!("done: the dongle is back in normal mode (no unplug needed)");
    } else {
        println!("done: unplug and replug the dongle now to return it to normal mode");
    }
}

fn page_of(path: &Path) -> Result<Vec<u8>, String> {
    let d = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    match d.len() {
        0x20000 => Ok(d[0x1E000..0x1F000].to_vec()),
        0x1000 => Ok(d),
        n => Err(format!("{}: {n} bytes: a 4 KB page or a 128 KB flash dump expected", path.display())),
    }
}

fn run(a: Args) -> Result<(), String> {
    let e = |e: Error| e.to_string();
    let pos: Vec<&str> = a.pos.iter().map(String::as_str).collect();
    match pos.as_slice() {
        ["backup", dir] => {
            let dir = Path::new(dir);
            if dir.join("flash.bin").exists() {
                return Err(format!("{} already holds a backup", dir.display()));
            }
            let (path, mut dev) = open(&a.device)?;
            let b = dongle::backup(&mut dev, &mut bar).map_err(e)?;
            b.save(dir).map_err(e)?;
            println!("{}", serde_json::to_string_pretty(&b.info).unwrap_or_default());
            drop(dev);
            finish(&path);
        }
        [cmd @ ("flash" | "restore"), rest @ ..] if rest.len() == usize::from(*cmd == "flash") => {
            let dir = a.backup.as_ref().ok_or("--backup DIR is required")?;
            let b = Backup::load(dir).map_err(e)?;
            let restore = *cmd == "restore";
            let data = match rest.first() {
                Some(img) if !restore => Some(std::fs::read(img).map_err(|x| format!("{img}: {x}"))?),
                _ => None,
            };
            let nv_choice = match a.nv.as_str() {
                "migrate" => NvChoice::Migrate,
                "wipe" => NvChoice::Wipe,
                "keep" => NvChoice::Keep,
                f => NvChoice::Json(Box::new(
                    nv::NvDoc::from_json(&std::fs::read_to_string(f).map_err(|x| format!("{f}: {x}"))?).map_err(e)?,
                )),
            };
            if let Some(d) = &data {
                let (padded, bi) = image::check_product(d, dongle::PRODUCT).map_err(e)?;
                println!(
                    "flash: {} {} ({} bytes, image ID {}), page 30: {}",
                    bi.product,
                    bi.version,
                    padded.len(),
                    bi.id_hex(),
                    a.nv
                );
            } else {
                println!("restore: the backup's pages 0-30 (chip {})", b.chip_id());
            }
            if !a.yes {
                println!("not written: add --yes");
                return Err(String::new());
            }
            let (path, mut dev) = open(&a.device)?;
            let r = match &data {
                Some(d) => dongle::flash(&mut dev, d, &b, &FlashOptions { fast: a.fast, nv: nv_choice }, &mut bar),
                None => dongle::restore(&mut dev, &b, a.fast, &mut bar),
            }
            .map_err(e)?;
            if let Some(doc) = &r.nv_doc {
                println!("NV (page 30): {}", doc.summary());
                for d in &doc.dropped {
                    println!("  dropped {}: {}", d.what, d.why);
                }
                if matches!(a.nv.as_str(), "migrate") {
                    std::fs::write(dir.join("nv.json"), doc.to_json()).map_err(|x| x.to_string())?;
                    println!("  (JSON: {})", dir.join("nv.json").display());
                }
            }
            println!(
                "verified and reset in {:.1} s ({} bytes sent, CRC {:#010x})",
                r.seconds, r.bytes_sent, r.image_crc32
            );
            drop(dev);
            finish(&path);
        }
        ["info", img] => {
            let d = std::fs::read(img).map_err(|x| format!("{img}: {x}"))?;
            let bi =
                image::build_info(&d).ok_or("no build info block (not an OpenWyzeSense image, or a boot selector)")?;
            println!(
                "product {}\napp {}\nboard {}\nversion {}\nimage ID {}\nflags {:#04x}\nsize {}\nsha256 {}",
                bi.product,
                bi.app,
                bi.board,
                bi.version,
                bi.id_hex(),
                bi.flags,
                d.len(),
                ows_host::manifest::sha256_hex(&d)
            );
        }
        ["nv", "export", page] => {
            let doc = nv::export(&page_of(Path::new(page))?);
            match &a.out {
                Some(o) => std::fs::write(o, doc.to_json()).map_err(|x| x.to_string())?,
                None => print!("{}", doc.to_json()),
            }
            eprintln!("{}", doc.summary());
            for d in &doc.dropped {
                eprintln!("  dropped {}: {}", d.what, d.why);
            }
            for r in &doc.unrecognised {
                eprintln!("  not imported: item {}/{} {}", r.item, r.sub, r.data);
            }
        }
        ["ota", targets, slot_a, slot_b] => {
            let read = |f: &str| std::fs::read(f).map_err(|x| format!("{f}: {x}"));
            let builds = [read(slot_a)?, read(slot_b)?];
            ota::validate_builds(&builds).map_err(e)?;
            let (board, version, ..) = ota::header(&builds[0]);
            let path = a.device.clone().or_else(hidraw::find_dongle).ok_or("no gateway: pass --device PATH")?;
            let name = path.display().to_string();
            let mut io: Box<dyn ByteLink> = if name.contains("hidraw") {
                Box::new(HidrawHost(Hidraw::open(&path).map_err(|x| format!("{name}: {x}"))?))
            } else {
                Box::new(Serial::open(&path).map_err(|x| format!("{name}: {x}"))?)
            };
            let mut link = Link::new(&mut *io);
            if !link.inquiry().map_err(e)? {
                return Err(format!("{name}: no answer to Inquiry"));
            }
            let t = if targets.eq_ignore_ascii_case("all") || *targets == "*" {
                let known = link.sensors().map_err(e)?;
                println!("all sensors of board {board:#010x} ({} paired)", known.len());
                Targets::Any { known }
            } else {
                let mut macs = vec![];
                for m in targets.split(',').map(str::trim).filter(|m| !m.is_empty()) {
                    macs.push(<[u8; 8]>::try_from(m.as_bytes()).map_err(|_| format!("{m}: a MAC is 8 characters"))?);
                }
                Targets::Macs(macs)
            };
            println!("offering version {version} (board {board:#010x}) for up to {} s", a.seconds);
            let mac = |m: &[u8; 8]| String::from_utf8_lossy(m).into_owned();
            let res = ota::run(&mut link, builds, t, std::time::Duration::from_secs(a.seconds), &mut |ev| match ev {
                ota::Event::Staged(m) => println!("staged for {}: press its button briefly now", mac(m)),
                ota::Event::Refused(m) => println!("{}: refused by the gateway", mac(m)),
                ota::Event::Progress { mac: m, slot, done, total } => {
                    eprint!("\r  {} slot {}: {done}/{total}   ", mac(m), ["A", "B"][*slot as usize & 1])
                }
                ota::Event::Done { mac: m, status } => eprintln!("\n  {}: {}", mac(m), ota::status_text(*status)),
            })
            .map_err(e)?;
            let mut failed = false;
            for (m, o) in &res {
                let text = match o {
                    ota::Outcome::Done(st) => ota::status_text(*st).to_string(),
                    ota::Outcome::Refused => "refused".into(),
                    ota::Outcome::TimedOut => "timeout (its button not pressed?)".into(),
                };
                failed |= *o != ota::Outcome::Done(0);
                println!("result {}: {text}", mac(m));
            }
            if failed {
                return Err("not every sensor was updated".into());
            }
        }
        ["nv", "import", json] => {
            let out = a.out.as_ref().ok_or("-o PAGE.bin is required")?;
            let doc =
                nv::NvDoc::from_json(&std::fs::read_to_string(json).map_err(|x| format!("{json}: {x}"))?).map_err(e)?;
            let page = nv::import(&doc).map_err(e)?;
            std::fs::write(out, &page).map_err(|x| x.to_string())?;
            eprintln!("{}: {} records", out.display(), nv::records(&page).len());
        }
        _ => return Err(String::new()),
    }
    Ok(())
}

fn main() -> ExitCode {
    match parse().and_then(run) {
        Ok(()) => ExitCode::SUCCESS,
        Err(m) if m.is_empty() => {
            eprintln!("{USAGE}");
            ExitCode::from(2)
        }
        Err(m) => {
            eprintln!("\nows-flash: {m}");
            ExitCode::FAILURE
        }
    }
}
