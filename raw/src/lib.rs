//! `photos-raw`: LightCraft's RAW decoding and default rendering (and its pure-Rust HEIF decoder)
//! behind the C interface of `include/photos_raw.h`, for the Photos application.
//!
//! The safe functions here ([`develop`], [`preview_jpeg`], [`info`], …) do the work; `ffi` wraps
//! them: C strings in, error codes and messages out, panics caught at the boundary.
//!
//! The rendering is the one LightCraft itself shows for a file nobody has edited:
//! `lightcraft_engine::files::load_vec` (decode, the camera look fitted from the file's own JPEG or
//! the DNG profile, highlight reconstruction, as-shot white balance, binning for small sizes),
//! then `lightcraft_pipeline::render` (the CPU pipeline) with the settings import gives a photo
//! (`lightcraft_catalog::Photo::camera_defaults`, repeated in [`default_settings`]).

mod ffi;

use std::fmt;
use std::sync::Once;

use lightcraft_develop::DevelopSettings;
use lightcraft_engine::files;
use lightcraft_geom::Orientation;
use lightcraft_meta::Metadata;
use lightcraft_pipeline::{RenderRequest, SourceInfo};
use lightcraft_raster::Rgba8;

/// What went wrong, as the C interface's negative codes tell it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    Io(String),
    Unsupported(String),
    Damaged(String),
    NotRaw(String),
    Internal(String),
    Argument(String),
    NoPreview(String),
    NotBuilt(String),
}

impl Error {
    pub fn code(&self) -> i32 {
        match self {
            Error::Io(_) => -1,
            Error::Unsupported(_) => -2,
            Error::Damaged(_) => -3,
            Error::NotRaw(_) => -4,
            Error::Internal(_) => -5,
            Error::Argument(_) => -6,
            Error::NoPreview(_) => -7,
            Error::NotBuilt(_) => -8,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(s)
            | Error::Unsupported(s)
            | Error::Damaged(s)
            | Error::NotRaw(s)
            | Error::Internal(s)
            | Error::Argument(s)
            | Error::NoPreview(s)
            | Error::NotBuilt(s) => f.write_str(s),
        }
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// Size and EXIF facts of a file (`photos_raw_info`).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Info {
    /// Developed size, upright.
    pub width: u32,
    pub height: u32,
    /// EXIF orientation 1..=8 of the file.
    pub orientation: u16,
    /// Capture time as seconds since the epoch of the camera's wall clock (no zone); 0 unknown.
    pub taken: i64,
    pub make: String,
    pub model: String,
    pub lens: String,
    pub iso: f32,
    pub exposure_time: f32,
    pub f_number: f32,
    pub focal_length: f32,
    pub gps: Option<(f64, f64)>,
}

/// An embedded preview as a JPEG file and the EXIF orientation that turns it upright.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PreviewJpeg {
    pub jpeg: Vec<u8>,
    pub orientation: u16,
}

/// The crate's and LightCraft's versions (`photos_raw_version`).
pub fn version() -> &'static str {
    if cfg!(feature = "heif") {
        concat!("photos-raw ", env!("CARGO_PKG_VERSION"), " (lightcraft ", env!("PHOTOS_RAW_LIGHTCRAFT_REV"), ", heif)")
    } else {
        concat!("photos-raw ", env!("CARGO_PKG_VERSION"), " (lightcraft ", env!("PHOTOS_RAW_LIGHTCRAFT_REV"), ")")
    }
}

/// The work runs on rayon's global pool; `PHOTOS_RAW_THREADS=n` sizes it (before anything else
/// in the process has started it).
fn init_pool() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let threads = std::env::var("PHOTOS_RAW_THREADS").ok().and_then(|v| v.trim().parse::<usize>().ok()).filter(|&n| n > 0);
        if let Some(n) = threads {
            // (an error means the pool exists already: its size stands)
            let _ = rayon::ThreadPoolBuilder::new().num_threads(n.min(256)).thread_name(|i| format!("photos raw {i}")).build_global();
        }
    });
}

fn read(path: &std::path::Path) -> Result<Vec<u8>> {
    std::fs::read(path).map_err(|e| Error::Io(format!("{}: {e}", path.display())))
}

/// Is this the beginning (or all) of a RAW file LightCraft knows?
pub fn is_raw(head: &[u8]) -> bool {
    lightcraft_raw::probe(head).is_some()
}

/// [`is_raw`] for a file: its first 64 KiB decide for nearly every file; a TIFF whose directory
/// lies further in (some DNG writers put it at the end) is read whole.
pub fn is_raw_file(path: &std::path::Path) -> Result<bool> {
    use std::io::Read;
    const HEAD: u64 = 64 * 1024;
    let mut file = std::fs::File::open(path).map_err(|e| Error::Io(format!("{}: {e}", path.display())))?;
    let mut head = Vec::new();
    file.by_ref().take(HEAD).read_to_end(&mut head).map_err(|e| Error::Io(format!("{}: {e}", path.display())))?;
    if is_raw(&head) {
        return Ok(true);
    }
    let tiff = head.starts_with(b"II*\0") || head.starts_with(b"MM\0*");
    if !tiff || (head.len() as u64) < HEAD {
        return Ok(false);
    }
    file.read_to_end(&mut head).map_err(|e| Error::Io(format!("{}: {e}", path.display())))?;
    Ok(is_raw(&head))
}

fn wall_seconds(t: &lightcraft_meta::DateTime) -> i64 {
    // the camera's clock as it read: the zone, when one is recorded, is not applied
    lightcraft_meta::DateTime { offset_minutes: None, ..*t }.unix_seconds()
}

fn info_of(m: &Metadata, width: u32, height: u32, orientation: Orientation) -> Info {
    let text = |s: &Option<String>| s.as_deref().map(str::trim).unwrap_or("").to_string();
    Info {
        width,
        height,
        orientation: orientation.to_exif(),
        taken: m.capture_time.as_ref().map(wall_seconds).unwrap_or(0),
        make: text(&m.make),
        model: text(&m.model),
        lens: m.lens_model.as_deref().map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).or_else(|| m.lens_spec.and_then(lightcraft_meta::lens_spec_name)).unwrap_or_default(),
        iso: m.iso.map(|v| v as f32).unwrap_or(0.0),
        exposure_time: m.exposure_time.filter(|v| v.is_finite() && *v > 0.0).map(|v| v as f32).unwrap_or(0.0),
        f_number: m.f_number.filter(|v| v.is_finite() && *v > 0.0).map(|v| v as f32).unwrap_or(0.0),
        focal_length: m.focal_length.filter(|v| v.is_finite() && *v > 0.0).map(|v| v as f32).unwrap_or(0.0),
        gps: m.gps.as_ref().filter(|g| g.latitude.is_finite() && g.longitude.is_finite()).map(|g| (g.latitude, g.longitude)),
    }
}

fn upright(w: usize, h: usize, o: Orientation) -> (u32, u32) {
    let (w, h) = (w.clamp(1, u32::MAX as usize) as u32, h.clamp(1, u32::MAX as usize) as u32);
    if o.swaps_axes() { (h, w) } else { (w, h) }
}

/// Turns a decode error into ours; `Ok(reason)` when the file is a RAW the decoder does not handle.
fn raw_error(e: lightcraft_raw::RawError) -> Error {
    use lightcraft_raw::RawError;
    match e {
        RawError::NotRaw => Error::NotRaw("not a RAW file".into()),
        RawError::Unsupported(why) => Error::Unsupported(format!("unsupported RAW variant: {why}")),
        e @ RawError::Corrupt(_) => Error::Damaged(e.to_string()),
        e => Error::Damaged(e.to_string()),
    }
}

/// Size, orientation and EXIF facts of a RAW file, from its headers.
pub fn info(bytes: &[u8]) -> Result<Info> {
    if !is_raw(bytes) {
        return Err(Error::NotRaw("not a RAW file".into()));
    }
    let mut m = lightcraft_meta::extract(bytes);
    match lightcraft_raw::probe_info(bytes) {
        Ok(raw) => {
            // what the Exif reader can't see, the decoder may have read from the raw's own records (CRW)
            m.fill_missing(&raw.metadata);
            let (w, h) = raw.developed_size();
            let (w, h) = upright(w, h, raw.orientation);
            Ok(info_of(&m, w, h, raw.orientation))
        }
        Err(e) => {
            // a variant the decoder can't read: described by its embedded preview, as LightCraft shows it
            let preview = lightcraft_raw::embedded_preview(bytes).ok_or_else(|| raw_error(e))?;
            let header = lightcraft_codecs::read_header(&preview).map_err(|e| Error::Damaged(format!("embedded preview: {e}")))?;
            let o = preview_orientation(&m, header.orientation);
            let (w, h) = upright(header.width as usize, header.height as usize, o);
            Ok(info_of(&m, w, h, o))
        }
    }
}

/// Orientation for an embedded preview: its own EXIF orientation when it has one, else the RAW file's
/// (`lightcraft_engine::files::preview_orientation`).
fn preview_orientation(raw_meta: &Metadata, jpeg_orientation: u16) -> Orientation {
    if jpeg_orientation > 1 {
        return Orientation::from_exif(jpeg_orientation);
    }
    raw_meta.orientation.unwrap_or(Orientation::Normal)
}

fn is_jpeg(b: &[u8]) -> bool {
    b.len() > 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff
}

fn encode_jpeg(img: &Rgba8) -> Result<Vec<u8>> {
    let (w, h) = (u16::try_from(img.width), u16::try_from(img.height));
    let (Ok(w), Ok(h)) = (w, h) else {
        return Err(Error::Unsupported("embedded preview too large for a JPEG".into()));
    };
    let mut rgb = Vec::with_capacity(img.data.len() * 3);
    for p in &img.data {
        rgb.extend_from_slice(&p[..3]);
    }
    let mut out = Vec::new();
    jpeg_encoder::Encoder::new(&mut out, 92)
        .encode(&rgb, w, h, jpeg_encoder::ColorType::Rgb)
        .map_err(|e| Error::Internal(format!("encoding the embedded preview: {e}")))?;
    Ok(out)
}

/// The embedded preview of a RAW file as a JPEG: the largest, or with `min_edge > 0` the smallest
/// whose long edge is at least that.
pub fn preview_jpeg(bytes: &[u8], min_edge: u32) -> Result<PreviewJpeg> {
    if !is_raw(bytes) {
        return Err(Error::NotRaw("not a RAW file".into()));
    }
    let found = if min_edge > 0 { lightcraft_raw::embedded_preview_covering(bytes, min_edge) } else { lightcraft_raw::embedded_preview(bytes) };
    let preview = found.ok_or_else(|| Error::NoPreview("the file has no embedded preview".into()))?;
    if is_jpeg(&preview) {
        let own = lightcraft_codecs::read_header(&preview).map(|h| h.orientation).unwrap_or(1);
        let o = preview_orientation(&lightcraft_meta::extract(bytes), own);
        return Ok(PreviewJpeg { jpeg: preview, orientation: o.to_exif() });
    }
    // JPEG XL (DNG 1.7) or uncompressed RGB (Hasselblad): decoded and written as a JPEG, upright
    let img = files::embedded_preview_srgb(bytes, usize::MAX).ok_or_else(|| Error::Damaged("the embedded preview does not decode".into()))?;
    Ok(PreviewJpeg { jpeg: encode_jpeg(&img)?, orientation: 1 })
}

fn edge(max_edge: u32) -> usize {
    if max_edge == 0 { usize::MAX } else { max_edge as usize }
}

/// The embedded preview decoded: upright sRGB, the long edge at most `max_edge` (0: as stored).
pub fn preview_image(bytes: &[u8], max_edge: u32) -> Result<Rgba8> {
    if !is_raw(bytes) {
        return Err(Error::NotRaw("not a RAW file".into()));
    }
    init_pool();
    match files::embedded_preview_srgb(bytes, edge(max_edge)) {
        Some(img) if img.width > 0 && img.height > 0 => Ok(img),
        _ if lightcraft_raw::embedded_preview(bytes).is_none() => Err(Error::NoPreview("the file has no embedded preview".into())),
        _ => Err(Error::Damaged("the embedded preview does not decode".into())),
    }
}

/// The develop settings LightCraft gives a freshly imported photo
/// (`lightcraft_catalog::Photo::camera_defaults`): as-shot white balance on the scale the source
/// is edited on, capture sharpening and colour noise reduction, embedded lens corrections on.
pub fn default_settings(info: &SourceInfo) -> DevelopSettings {
    if !info.raw {
        return DevelopSettings::default();
    }
    let (temp, tint) = if info.relative_wb { (6500.0, 0.0) } else { (info.as_shot_temp, info.as_shot_tint) };
    let mut s = DevelopSettings::for_raw(temp, tint);
    if !info.relative_wb {
        s.wb.scale = lightcraft_develop::WbScale::Kelvin;
    }
    if info.lens.is_some() {
        s.optics.lens_profile = true;
    }
    s
}

/// Develops a RAW file as LightCraft shows it unedited: upright sRGB, the long edge at most
/// `max_edge` (0: full size). Takes the file's bytes: they are freed as soon as the sensor data is
/// decoded.
pub fn develop(bytes: Vec<u8>, max_edge: u32) -> Result<Rgba8> {
    if !is_raw(&bytes) {
        return Err(Error::NotRaw("not a RAW file".into()));
    }
    init_pool();
    // Headers first: they say how large the original is (noise reduction and sharpening size
    // their radii in the original's pixels) and catch what the decoder cannot read before
    // LightCraft's loader falls back to the embedded JPEG, which is not a development.
    let native_long = match lightcraft_raw::probe_info(&bytes) {
        Ok(raw) => {
            let (w, h) = raw.developed_size();
            w.max(h)
        }
        Err(e) => return Err(raw_error(e)),
    };
    // (kept for the error path only when the decode itself refuses the file)
    let (img, mut info) = match files::load_bytes(&bytes, edge(max_edge)) {
        Ok(loaded) => loaded,
        Err(why) => return Err(Error::Damaged(why)),
    };
    if !info.raw {
        // the loader showed the embedded preview instead: say why the sensor data did not decode
        return Err(match lightcraft_raw::decode(&bytes) {
            Err(e) => match raw_error(e) {
                Error::NotRaw(why) | Error::Damaged(why) => Error::Damaged(why),
                other => other,
            },
            Ok(_) => Error::Unsupported("the RAW data could not be developed".into()),
        });
    }
    drop(bytes);
    if img.width == 0 || img.height == 0 {
        return Err(Error::Damaged("the RAW file holds no picture".into()));
    }
    let long = img.width.max(img.height);
    if native_long > long {
        info.native_long = Some(native_long);
    }
    let settings = default_settings(&info);
    let rendered = lightcraft_pipeline::render(&img, &info, &settings, &RenderRequest::fit(long, long));
    drop(img);
    Ok(rendered.image)
}

/// Is this the beginning of a HEIF / HEIC file?
pub fn is_heif(head: &[u8]) -> bool {
    lightcraft_codecs::sniff(head) == Some(lightcraft_codecs::Format::Heif)
}

/// Was the HEIF decoder built in (the `heif` feature)?
pub fn heif_available() -> bool {
    cfg!(feature = "heif")
}

fn heif_checked(bytes: &[u8]) -> Result<()> {
    if !is_heif(bytes) {
        return Err(Error::NotRaw("not a HEIF file".into()));
    }
    if !heif_available() {
        return Err(Error::NotBuilt("HEIF support is not included in this build".into()));
    }
    Ok(())
}

fn codec_error(e: lightcraft_codecs::Error) -> Error {
    use lightcraft_codecs::Error as E;
    match e {
        E::UnknownFormat => Error::NotRaw("not a HEIF file".into()),
        e @ E::Unsupported(..) => Error::Unsupported(e.to_string()),
        e => Error::Damaged(e.to_string()),
    }
}

/// Size and EXIF facts of a HEIF file.
pub fn heif_info(bytes: &[u8]) -> Result<Info> {
    heif_checked(bytes)?;
    let header = lightcraft_codecs::read_header(bytes).map_err(codec_error)?;
    let o = Orientation::from_exif(header.orientation);
    let (w, h) = upright(header.width as usize, header.height as usize, o);
    Ok(info_of(&lightcraft_meta::extract(bytes), w, h, o))
}

/// A HEIF picture, upright sRGB, the long edge at most `max_edge` (0: full size).
pub fn heif_decode(bytes: &[u8], max_edge: u32) -> Result<Rgba8> {
    heif_checked(bytes)?;
    init_pool();
    let opts = if max_edge == 0 { lightcraft_codecs::DecodeOptions::default() } else { lightcraft_codecs::DecodeOptions::fit(max_edge, max_edge) };
    let d = lightcraft_codecs::decode(bytes, opts).map_err(codec_error)?;
    let o = Orientation::from_exif(d.orientation);
    let img = d.to_srgb8();
    if img.width == 0 || img.height == 0 {
        return Err(Error::Damaged("the file holds no picture".into()));
    }
    Ok(img.into_oriented(o))
}

pub fn info_file(path: &std::path::Path) -> Result<Info> {
    info(&read(path)?)
}

pub fn preview_jpeg_file(path: &std::path::Path, min_edge: u32) -> Result<PreviewJpeg> {
    preview_jpeg(&read(path)?, min_edge)
}

pub fn preview_image_file(path: &std::path::Path, max_edge: u32) -> Result<Rgba8> {
    preview_image(&read(path)?, max_edge)
}

pub fn develop_file(path: &std::path::Path, max_edge: u32) -> Result<Rgba8> {
    develop(read(path)?, max_edge)
}

pub fn heif_info_file(path: &std::path::Path) -> Result<Info> {
    heif_info(&read(path)?)
}

pub fn heif_decode_file(path: &std::path::Path, max_edge: u32) -> Result<Rgba8> {
    heif_decode(&read(path)?, max_edge)
}
