//! The release manifest (`manifest.json` in every release, written by `tools/release_bundle.py`): each file with
//! its device, board, product and SHA-256, so a host program downloads the image it needs and checks it.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::Error;

pub const FORMAT: &str = "openwyzesense-release";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Manifest {
    pub format: String,
    /// The release tag, e.g. "v0.1.0".
    pub version: String,
    pub files: Vec<ManifestFile>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ManifestFile {
    /// The release asset's name.
    pub file: String,
    pub device: String,
    pub what: String,
    /// The firmware board (build info), e.g. "wyze_dongle"; None for the ESP relay images.
    #[serde(default)]
    pub board: Option<String>,
    /// The product, e.g. "gateway-dongle".
    #[serde(default)]
    pub product: Option<String>,
    pub size: u64,
    pub sha256: String,
}

impl Manifest {
    pub fn from_json(text: &str) -> Result<Manifest, Error> {
        let m: Manifest = serde_json::from_str(text).map_err(|e| Error::Manifest(e.to_string()))?;
        if m.format != FORMAT {
            return Err(Error::Manifest(format!("format {:?}, not {FORMAT}", m.format)));
        }
        Ok(m)
    }

    /// The file for a product (e.g. "gateway-dongle").
    pub fn product(&self, product: &str) -> Option<&ManifestFile> {
        self.files.iter().find(|f| f.product.as_deref() == Some(product))
    }
}

pub fn sha256_hex(data: &[u8]) -> String {
    hex::encode(Sha256::digest(data))
}

impl ManifestFile {
    /// The downloaded bytes are this file: size and SHA-256.
    pub fn verify(&self, data: &[u8]) -> Result<(), Error> {
        let got = sha256_hex(data);
        if data.len() as u64 != self.size || !got.eq_ignore_ascii_case(&self.sha256) {
            return Err(Error::Manifest(format!(
                "{}: {} bytes, SHA-256 {got}; the manifest says {} bytes, {}",
                self.file,
                data.len(),
                self.size,
                self.sha256
            )));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_and_verifies() {
        let data = b"image";
        let m = Manifest::from_json(&format!(
            r#"{{"format":"openwyzesense-release","version":"v0.1.0","files":[{{"file":"ows-gateway-dongle-v0.1.0.bin",
            "device":"Wyze USB dongle","what":"gateway","board":"wyze_dongle","product":"gateway-dongle","size":5,
            "sha256":"{}"}}]}}"#,
            sha256_hex(data)
        ))
        .unwrap();
        let f = m.product("gateway-dongle").unwrap();
        f.verify(data).unwrap();
        assert!(f.verify(b"imagf").is_err());
        assert!(m.product("sensor-contact-v1").is_none());
        assert!(Manifest::from_json(r#"{"format":"x","version":"v","files":[]}"#).is_err());
    }
}
