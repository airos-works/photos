//! `develop` against LightCraft itself: the same file imported into a LightCraft session and
//! exported with no edits (what `lightcraft-cli render` does) must be the picture `develop` returns.
//! This is what "LightCraft's default rendering" means; the test keeps `default_settings` in step
//! with LightCraft's import defaults when the pinned revision moves.

use std::path::{Path, PathBuf};

use lightcraft_engine::Session;
use lightcraft_engine::export::{ExportOptions, export_photo};
use lightcraft_raster::Rgba8;
use serde_json::json;

fn samples() -> Option<PathBuf> {
    let dir = std::env::var_os("PHOTOS_RAW_SAMPLES").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target/samples"));
    if dir.join("nikon-d70.nef").is_file() {
        Some(dir)
    } else {
        eprintln!("skip: no samples in {} (tools/raw-test/fetch-samples.sh)", dir.display());
        None
    }
}

/// The file as a LightCraft session exports it untouched, full size, 8-bit sRGB.
fn lightcraft_export(path: &Path) -> Rgba8 {
    let mut s = Session::new().with_fs().with_system_clock();
    let r = s.execute("library.import", &json!({"paths": [path.to_string_lossy()]})).unwrap();
    let id = r["imported"][0].as_u64().unwrap_or_else(|| panic!("{}: not imported: {r}", path.display()));
    s.execute("library.select", &json!({"ids": [id], "active": id})).unwrap();
    let o = ExportOptions::from_params(&json!({"format": "png", "colorSpace": "srgb"})).unwrap();
    let e = export_photo(&mut s, lightcraft_engine::catalog::PhotoId(id), &o, 1).unwrap();
    lightcraft_codecs::decode(&e.bytes, lightcraft_codecs::DecodeOptions::default()).unwrap().to_srgb8()
}

/// Mean and largest absolute difference of the colour channels.
fn difference(a: &Rgba8, b: &Rgba8) -> (f64, u8) {
    assert_eq!((a.width, a.height), (b.width, b.height), "same size");
    let (mut sum, mut worst) = (0u64, 0u8);
    for (p, q) in a.data.iter().zip(&b.data) {
        for c in 0..3 {
            let d = p[c].abs_diff(q[c]);
            sum += u64::from(d);
            worst = worst.max(d);
        }
    }
    (sum as f64 / (a.data.len() * 3) as f64, worst)
}

#[test]
fn develop_is_lightcraft_s_unedited_export() {
    let Some(dir) = samples() else { return };
    // a fitted look (NEF), a file with its own colour model (DNG), X-Trans (RAF)
    for name in ["nikon-d70.nef", "iphone6s.dng", "canon-40d.cr2", "fuji-xpro1.raf"] {
        let path = dir.join(name);
        if !path.is_file() {
            continue;
        }
        let ours = photos_raw::develop_file(&path, 0).unwrap();
        let theirs = lightcraft_export(&path);
        let (mean, worst) = difference(&ours, &theirs);
        eprintln!("{name}: {} x {}, mean difference {mean:.4}, largest {worst}", ours.width, ours.height);
        // (the export goes through a PNG and its colour management: a level of rounding at most)
        assert!(mean < 0.02 && worst <= 2, "{name}: mean {mean}, largest {worst}");
    }
}
