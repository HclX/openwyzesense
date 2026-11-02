// The golden NV vectors (reference/tests/vectors/nv/golden, made by tools/nv_golden.py from the Python reference):
// the crate's export and import give the same bytes.
use std::path::PathBuf;

use ows_host::nv;

fn vectors() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../reference/tests/vectors/nv")
}

#[test]
fn golden_vectors_match_the_python_reference() {
    let golden = vectors().join("golden");
    let mut n = 0;
    for e in std::fs::read_dir(&golden).unwrap() {
        let name = e.unwrap().file_name().into_string().unwrap();
        let Some(stem) = name.strip_suffix(".json") else { continue };
        let made = golden.join(format!("{stem}.page.bin"));
        let input = if made.exists() { made } else { vectors().join(format!("{stem}.page30.bin")) };
        let page = std::fs::read(&input).unwrap();
        let doc = nv::export(&page);
        let want = std::fs::read_to_string(golden.join(&name)).unwrap();
        assert_eq!(doc.to_json(), want, "{stem}: export differs");
        assert_eq!(nv::NvDoc::from_json(&want).unwrap(), doc, "{stem}: JSON round trip");
        let ours = golden.join(format!("{stem}.ows.bin"));
        if ours.exists() {
            let page = nv::import(&doc).unwrap();
            assert_eq!(page, std::fs::read(ours).unwrap(), "{stem}: import differs");
            // and ours exports to the same model (but for its layout)
            let again = nv::export(&page);
            assert_eq!(
                (again.network, again.sensors, again.join, again.cfg),
                (doc.network, doc.sensors, doc.join, doc.cfg)
            );
        } else {
            assert!(nv::import(&doc).is_err(), "{stem}: import should refuse");
        }
        n += 1;
    }
    assert!(n >= 10, "only {n} golden vectors");
}

fn doc(json: &str) -> nv::NvDoc {
    nv::NvDoc::from_json(json).unwrap()
}

#[test]
fn import_refuses_what_the_reference_refuses() {
    let base = r#"{"format":"openwyzesense-nv-json","version":1"#;
    let net = r#","network":{"pan":"65FE","short":"D425","ext":"00124B0012345678"}"#;
    let refused = [
        r#"{"format":"other","version":1}"#.to_string(),
        format!(r#"{base},"network":{{"pan":"FFFF","short":"D425","ext":"0"}}}}"#),
        format!(r#"{base},"sensors":[{{"sub":1,"mac":"77A8C793","short":"0003"}}]}}"#),
        format!(r#"{base}{net},"sensors":[{{"sub":1,"mac":"77A8C793","short":"D425"}}]}}"#),
        format!(
            r#"{base}{net},"sensors":[{{"sub":1,"mac":"77A8C793","short":"0003"}},{{"sub":2,"mac":"77A8C793","short":"0004"}}]}}"#
        ),
        format!(
            r#"{base},"join":{{"type":"01","gw_ours":false,"pan":"845B","coord":"3366","short":"0005","gw_features":"0000"}}}}"#
        ),
        format!(r#"{base},"cfg":{{"type":"0E","pull":"sideways"}}}}"#),
        format!(r#"{base},"cfg":{{"debounce_ms":5001}}}}"#),
    ];
    for r in refused {
        assert!(nv::import(&doc(&r)).is_err(), "{r}");
    }
    let ok = format!(r#"{base}{net},"sensors":[{{"sub":3,"mac":"77A8C793","short":"0x0003"}}],"cfg":{{}}}}"#);
    let page = nv::import(&doc(&ok)).unwrap();
    let back = nv::export(&page);
    assert_eq!(back.sensors[0].short, "0003");
    assert!(back.cfg.is_none());
}

#[test]
fn page_for_choices() {
    let stock = std::fs::read(vectors().join("dongle_77A85A36_2026-10-23_before_ours.page30.bin")).unwrap();
    assert_eq!(nv::page_for(&nv::NvChoice::Keep, &stock).unwrap(), (None, None));
    assert_eq!(nv::page_for(&nv::NvChoice::Wipe, &stock).unwrap().0.unwrap(), vec![0xFF; 4096]);
    let (page, d) = nv::page_for(&nv::NvChoice::Migrate, &stock).unwrap();
    assert!(nv::is_ows(&page.unwrap()));
    assert_eq!(d.unwrap().source.unwrap().layout, "stock");
}
