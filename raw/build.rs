//! Puts the LightCraft commit the crate is built against (as Cargo.lock records it) into
//! `PHOTOS_RAW_LIGHTCRAFT_REV`, for `photos_raw_version()`.

use std::path::PathBuf;

fn main() {
    let dir = PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").unwrap_or_default());
    let lock = dir.join("Cargo.lock");
    println!("cargo::rerun-if-changed={}", lock.display());
    let text = std::fs::read_to_string(&lock).unwrap_or_default();
    // [[package]] name = "lightcraft-raw" ... source = "git+https://…/lightcraft?rev=…#<40 hex>"
    let mut rev = String::from("unknown");
    let mut inside = false;
    for line in text.lines() {
        if line.starts_with("name = ") {
            inside = line == "name = \"lightcraft-raw\"";
        } else if inside && line.starts_with("source = ") {
            if let Some(hash) = line.trim_end_matches('"').rsplit('#').next() {
                rev = hash.chars().take(8).collect();
            }
            break;
        }
    }
    println!("cargo::rustc-env=PHOTOS_RAW_LIGHTCRAFT_REV={rev}");
}
