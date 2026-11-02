//! Firmware image checks: what the Python `dongle_sbl.py` (replaced by this, #44) check_image() does (fits below page 30, a vector table), plus
//! the build info block every image of ours carries (`firmware/common/build_info.h`, #76): which board and app it
//! was built for.

use crate::Error;

/// Flash pages 0-29: the only ones an image is written to.
pub const APP_END: usize = 0x1E000;
pub const BUILD_INFO_MAGIC: u32 = 0x4253_574F; // "OWSB"
const BUILD_INFO_SIZE: usize = 116;

/// The build info block (`build_info_t`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BuildInfo {
    /// Where the block is in the image.
    pub offset: usize,
    pub format: u8,
    pub flags: u8,
    /// The image ID (tools/image_id.py): the first 8 bytes of a SHA-256 over the code.
    pub id: [u8; 8],
    pub product: String,
    pub board: String,
    pub app: String,
    pub version: String,
}

impl BuildInfo {
    pub const FLAG_TX: u8 = 0x01;
    pub const FLAG_DEBUG: u8 = 0x02;

    pub fn id_hex(&self) -> String {
        hex::encode(self.id)
    }
}

fn cstr(b: &[u8]) -> String {
    let n = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(&b[..n]).into_owned()
}

/// The build info block: found by its magic, 4-aligned, in the image's first 1 KB. None for images without one
/// (stock firmware, boot selectors).
pub fn build_info(image: &[u8]) -> Option<BuildInfo> {
    let end = image.len().min(1024);
    (0..end).step_by(4).find_map(|at| {
        let b = image.get(at..at + BUILD_INFO_SIZE)?;
        if u32::from_le_bytes([b[0], b[1], b[2], b[3]]) != BUILD_INFO_MAGIC {
            return None;
        }
        Some(BuildInfo {
            offset: at,
            format: b[4],
            flags: b[5],
            id: b[8..16].try_into().unwrap(),
            product: cstr(&b[16..40]),
            board: cstr(&b[40..64]),
            app: cstr(&b[64..76]),
            version: cstr(&b[76..116]),
        })
    })
}

/// An application image for pages 0-29: not empty, fits below page 30, starts with a vector table (initial SP in
/// RAM, a Thumb reset handler inside the image). Returns it padded to whole words, as it is written.
pub fn check_app(image: &[u8]) -> Result<Vec<u8>, Error> {
    if image.is_empty() || image.len() > APP_END {
        return Err(Error::Image(format!(
            "image is {} bytes: must fit below {APP_END:#x} (the build's <product>-app.bin, without the CCFG; \
             not <product>.bin)",
            image.len()
        )));
    }
    if image.len() < 8 {
        return Err(Error::Image("no vector table: image shorter than 8 bytes".into()));
    }
    let sp = u32::from_le_bytes(image[0..4].try_into().unwrap());
    let reset = u32::from_le_bytes(image[4..8].try_into().unwrap());
    if !(0x2000_0001..=0x2000_5000).contains(&sp) || reset & 1 == 0 || (reset & !1) as usize >= image.len() {
        return Err(Error::Image(format!("no vector table: SP {sp:#x}, reset {reset:#x}")));
    }
    let mut out = image.to_vec();
    out.resize(image.len().div_ceil(4) * 4, 0xFF);
    Ok(out)
}

/// An image of ours for this product (e.g. "gateway-dongle", firmware/products.cmake): check_app() and its build
/// info naming that product.
pub fn check_product(image: &[u8], product: &str) -> Result<(Vec<u8>, BuildInfo), Error> {
    let padded = check_app(image)?;
    let info = build_info(image)
        .ok_or_else(|| Error::Image("no build info block: not an OpenWyzeSense image (or a boot selector)".into()))?;
    if info.product != product {
        return Err(Error::Image(format!(
            "image is {} ({}, {}): this needs {product}",
            info.product, info.app, info.board
        )));
    }
    Ok((padded, info))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_built_dongle_image_names_its_product() {
        let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../build/fw/firmware/gateway-dongle-app.bin");
        let Ok(img) = std::fs::read(p) else { return }; // no firmware build here
        let (_, bi) = check_product(&img, "gateway-dongle").unwrap();
        assert_eq!((bi.app.as_str(), bi.board.as_str(), bi.format), ("gateway", "Wyze Sense dongle", 1));
        assert!(check_product(&img, "gateway-contact-v1").is_err());
    }
}
