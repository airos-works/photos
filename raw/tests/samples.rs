//! The library on real files: the CC0 samples `tools/raw-test/fetch-samples.sh` downloads
//! (raw.pixls.us, the files LightCraft's own corpus pins). `PHOTOS_RAW_SAMPLES` names the folder
//! (default `target/samples` of this crate); without it these tests say so and pass.

use std::path::PathBuf;

use photos_raw::{Error, develop, develop_file, info_file, is_raw, is_raw_file, preview_image_file, preview_jpeg_file};

fn samples() -> Option<PathBuf> {
    let dir = std::env::var_os("PHOTOS_RAW_SAMPLES").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target/samples"));
    if dir.join("nikon-d70.nef").is_file() {
        Some(dir)
    } else {
        eprintln!("skip: no samples in {} (tools/raw-test/fetch-samples.sh)", dir.display());
        None
    }
}

/// Share of pixels that are neither black nor blown out, and the mean of every channel.
fn stats(img: &lightcraft_raster::Rgba8) -> (f64, [f64; 3]) {
    let mut mid = 0usize;
    let mut sum = [0f64; 3];
    for p in &img.data {
        if p[..3].iter().any(|&v| v > 8) && p[..3].iter().any(|&v| v < 247) {
            mid += 1;
        }
        for c in 0..3 {
            sum[c] += f64::from(p[c]);
        }
        assert_eq!(p[3], 255, "opaque");
    }
    let n = img.data.len() as f64;
    (mid as f64 / n, sum.map(|v| v / n))
}

#[test]
fn facts_of_the_samples() {
    let Some(dir) = samples() else { return };
    let i = info_file(&dir.join("nikon-d70.nef")).unwrap();
    assert_eq!((i.width, i.height, i.orientation), (3008, 2000, 1));
    assert_eq!((i.make.as_str(), i.model.as_str()), ("NIKON CORPORATION", "NIKON D70"));
    // 2017-09-02 16:55:01 on the camera's clock
    assert_eq!(i.taken, 1_504_371_301);
    assert!((i.f_number - 3.2).abs() < 0.01 && (i.focal_length - 50.0).abs() < 0.01 && i.gps.is_none());

    let i = info_file(&dir.join("iphone6s.dng")).unwrap();
    assert_eq!((i.width, i.height), (4032, 3024));
    assert_eq!(i.model, "iPhone 6s Plus");
    let (lat, lon) = i.gps.unwrap();
    assert!((lat - 28.133).abs() < 0.01 && (lon + 15.435).abs() < 0.01, "{lat} {lon}");
    assert!(i.iso == 25.0 && i.exposure_time > 0.0);

    let i = info_file(&dir.join("fuji-xt20.raf")).unwrap();
    assert_eq!((i.width, i.height, i.model.as_str()), (6000, 4000, "X-T20"));
    assert_eq!(i.lens, "XF18-55mmF2.8-4 R LM OIS");
}

#[test]
fn every_sample_develops() {
    let Some(dir) = samples() else { return };
    for name in ["nikon-d70.nef", "canon-40d.cr2", "iphone6s.dng", "fuji-xt20.raf", "fuji-xpro1.raf", "sony-rx100.arw", "panasonic-gx80.rw2"] {
        let path = dir.join(name);
        if !path.is_file() {
            continue;
        }
        assert!(is_raw_file(&path).unwrap(), "{name}");
        let facts = info_file(&path).unwrap();
        let img = develop_file(&path, 512).unwrap_or_else(|e| panic!("{name}: {e}"));
        let long = img.width.max(img.height);
        // (binning keeps at least three quarters of what was asked for)
        assert!((384..=512).contains(&long), "{name}: {} x {}", img.width, img.height);
        // upright and the picture's shape
        let (a, b) = (f64::from(facts.width) / f64::from(facts.height), img.width as f64 / img.height as f64);
        assert!((a - b).abs() < 0.02, "{name}: {a} against {b}");
        let (mid, mean) = stats(&img);
        assert!(mid > 0.5, "{name}: only {:.0}% of the picture is neither black nor white", mid * 100.0);
        assert!(mean.iter().all(|&v| (20.0..235.0).contains(&v)), "{name}: mean {mean:?}");
    }
}

#[test]
fn full_size_is_the_size_the_facts_promise() {
    let Some(dir) = samples() else { return };
    let path = dir.join("nikon-d70.nef");
    let facts = info_file(&path).unwrap();
    let img = develop_file(&path, 0).unwrap();
    assert_eq!((img.width as u32, img.height as u32), (facts.width, facts.height));
    // the same bytes develop the same picture
    let again = develop(std::fs::read(&path).unwrap(), 0).unwrap();
    assert!(img == again, "a development is repeatable");
}

#[test]
fn the_development_looks_like_the_camera_jpeg() {
    // A look fitted to the file's own JPEG: mean colour within a few levels of the camera's.
    let Some(dir) = samples() else { return };
    for name in ["fuji-xt20.raf", "nikon-d70.nef"] {
        let path = dir.join(name);
        let developed = develop_file(&path, 512).unwrap();
        let camera = preview_image_file(&path, 512).unwrap();
        let (a, b) = (stats(&developed).1, stats(&camera).1);
        for c in 0..3 {
            assert!((a[c] - b[c]).abs() < 12.0, "{name}: developed {a:?}, camera {b:?}");
        }
    }
}

#[test]
fn previews() {
    let Some(dir) = samples() else { return };
    let path = dir.join("fuji-xt20.raf");
    let largest = preview_jpeg_file(&path, 0).unwrap();
    assert!(largest.jpeg.starts_with(&[0xff, 0xd8, 0xff]) && largest.jpeg.len() > 100_000);
    assert_eq!(largest.orientation, 1);
    let header = lightcraft_codecs::read_header(&largest.jpeg).unwrap();
    assert!(header.width >= 1920, "the largest preview: {} x {}", header.width, header.height);
    let img = preview_image_file(&path, 256).unwrap();
    assert_eq!(img.width.max(img.height), 256);
    assert!(stats(&img).0 > 0.5);
    // the smallest that covers: never larger than the largest
    let covering = preview_jpeg_file(&path, 300).unwrap();
    assert!(covering.jpeg.len() <= largest.jpeg.len());
}

#[test]
fn what_is_not_a_raw_file_is_refused() {
    let noise: Vec<u8> = (0..200_000u32).map(|i| (i.wrapping_mul(2_654_435_761) >> 13) as u8).collect();
    assert!(!is_raw(&noise) && !is_raw(&[]));
    assert!(matches!(develop(noise.clone(), 512), Err(Error::NotRaw(_))));
    assert!(matches!(photos_raw::info(&noise), Err(Error::NotRaw(_))));
    assert!(matches!(photos_raw::preview_jpeg(&noise, 0), Err(Error::NotRaw(_))));
    assert!(matches!(develop_file(std::path::Path::new("/nonexistent/x.raf"), 0), Err(Error::Io(_))));
    // a JPEG is not a RAW file either
    assert!(!is_raw(&[0xff, 0xd8, 0xff, 0xe0, 0, 16, b'J', b'F', b'I', b'F', 0]));
}

#[test]
fn damaged_files_fail_without_a_panic() {
    let Some(dir) = samples() else { return };
    for name in ["nikon-d70.nef", "fuji-xt20.raf", "iphone6s.dng"] {
        let bytes = std::fs::read(dir.join(name)).unwrap();
        // cut short: the header is there, the sensor data is not
        for keep in [64usize, 4096, bytes.len() / 3, bytes.len() - 100_000] {
            let cut = bytes[..keep].to_vec();
            match develop(cut, 256) {
                // (a file cut inside its last strip may still develop: rows of nothing)
                Ok(img) => assert!(img.width > 0 && keep > 4096, "{name} cut to {keep}"),
                Err(e) => assert!(e.code() < 0 && !e.to_string().is_empty(), "{name} cut to {keep}: {e:?}"),
            }
        }
        // noise in the middle of the sensor data
        let mut noisy = bytes.clone();
        let start = noisy.len() / 2;
        for (i, b) in noisy[start..start + 65_536].iter_mut().enumerate() {
            *b = (i as u32).wrapping_mul(2_246_822_519) as u8;
        }
        let _ = develop(noisy, 256);
        // a header of noise after the magic
        let mut head = bytes[..8192].to_vec();
        for (i, b) in head[16..].iter_mut().enumerate() {
            *b = (i as u32).wrapping_mul(3_266_489_917).rotate_left(7) as u8;
        }
        let _ = photos_raw::info(&head);
        let _ = develop(head, 256);
    }
}
