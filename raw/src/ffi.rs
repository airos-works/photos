//! The C interface (`include/photos_raw.h`): pointers checked, panics caught, errors as codes
//! plus a message in the caller's buffer.

use std::ffi::{CStr, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::Path;

use lightcraft_raster::Rgba8;

use crate::{Error, Info, Result};

#[repr(C)]
pub struct PhotosRawInfo {
    pub width: u32,
    pub height: u32,
    pub orientation: i32,
    pub taken: i64,
    pub make: [c_char; 64],
    pub model: [c_char; 64],
    pub lens: [c_char; 96],
    pub iso: f32,
    pub exposure_time: f32,
    pub f_number: f32,
    pub focal_length: f32,
    pub latitude: f64,
    pub longitude: f64,
    pub has_gps: i32,
}

#[repr(C)]
pub struct PhotosRawImage {
    pub pixels: *mut u8,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
}

/// Copies `text` into a C buffer of `size` bytes: cut at a character boundary, NUL-terminated.
///
/// # Safety
/// `buffer` is null or points to `size` writable bytes.
unsafe fn put_text(buffer: *mut c_char, size: usize, text: &str) {
    if buffer.is_null() || size == 0 {
        return;
    }
    let mut n = text.len().min(size - 1);
    while n > 0 && !text.is_char_boundary(n) {
        n -= 1;
    }
    // SAFETY: n < size, and the caller vouches for `size` bytes at `buffer`.
    unsafe {
        std::ptr::copy_nonoverlapping(text.as_ptr(), buffer.cast::<u8>(), n);
        *buffer.add(n) = 0;
    }
}

fn fixed<const N: usize>(text: &str) -> [c_char; N] {
    let mut out = [0 as c_char; N];
    // SAFETY: `out` is N bytes.
    unsafe { put_text(out.as_mut_ptr(), N, text) };
    out
}

fn panic_text(payload: Box<dyn std::any::Any + Send>) -> String {
    let what = payload.downcast_ref::<&str>().map(|s| s.to_string()).or_else(|| payload.downcast_ref::<String>().cloned());
    match what {
        Some(s) => format!("the decoder failed unexpectedly: {s}"),
        None => "the decoder failed unexpectedly".to_string(),
    }
}

/// Runs `work`, turning a panic into [`Error::Internal`], and reports the outcome the C way.
///
/// # Safety
/// `error` is null or points to `error_size` writable bytes.
unsafe fn guarded(error: *mut c_char, error_size: usize, work: impl FnOnce() -> Result<()>) -> i32 {
    // SAFETY: as the caller's.
    unsafe { put_text(error, error_size, "") };
    let outcome = catch_unwind(AssertUnwindSafe(work)).unwrap_or_else(|payload| Err(Error::Internal(panic_text(payload))));
    match outcome {
        Ok(()) => 0,
        Err(e) => {
            // SAFETY: as the caller's.
            unsafe { put_text(error, error_size, &e.to_string()) };
            e.code()
        }
    }
}

/// # Safety
/// `path` is null or a NUL-terminated string.
unsafe fn path_of<'a>(path: *const c_char) -> Result<&'a Path> {
    if path.is_null() {
        return Err(Error::Argument("no path given".into()));
    }
    // SAFETY: the caller vouches for a NUL-terminated string.
    let bytes = unsafe { CStr::from_ptr(path) }.to_bytes();
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        Ok(Path::new(std::ffi::OsStr::from_bytes(bytes)))
    }
    #[cfg(not(unix))]
    {
        std::str::from_utf8(bytes).map(Path::new).map_err(|_| Error::Argument("the path is not UTF-8".into()))
    }
}

/// # Safety
/// `data` is null or points to `len` readable bytes.
unsafe fn slice_of<'a>(data: *const u8, len: usize) -> &'a [u8] {
    if data.is_null() || len == 0 {
        return &[];
    }
    // SAFETY: the caller vouches for `len` bytes at `data`.
    unsafe { std::slice::from_raw_parts(data, len) }
}

fn c_info(i: &Info) -> PhotosRawInfo {
    PhotosRawInfo {
        width: i.width,
        height: i.height,
        orientation: i32::from(i.orientation),
        taken: i.taken,
        make: fixed(&i.make),
        model: fixed(&i.model),
        lens: fixed(&i.lens),
        iso: i.iso,
        exposure_time: i.exposure_time,
        f_number: i.f_number,
        focal_length: i.focal_length,
        latitude: i.gps.map(|g| g.0).unwrap_or(f64::NAN),
        longitude: i.gps.map(|g| g.1).unwrap_or(f64::NAN),
        has_gps: i32::from(i.gps.is_some()),
    }
}

/// RGBA → BGRA with alpha 255, handed out as one allocation of exactly `width * height * 4` bytes.
fn c_image(img: Rgba8) -> Result<PhotosRawImage> {
    let (w, h) = (u32::try_from(img.width), u32::try_from(img.height));
    let (Ok(width), Ok(height)) = (w, h) else {
        return Err(Error::Unsupported("the picture is too large".into()));
    };
    let Some(stride) = width.checked_mul(4) else {
        return Err(Error::Unsupported("the picture is too large".into()));
    };
    let mut bytes = Vec::with_capacity(img.data.len() * 4);
    for p in &img.data {
        bytes.extend_from_slice(&[p[2], p[1], p[0], 255]);
    }
    drop(img);
    let pixels = Box::into_raw(bytes.into_boxed_slice()).cast::<u8>();
    Ok(PhotosRawImage { pixels, width, height, stride })
}

fn c_bytes(bytes: Vec<u8>) -> (*mut u8, usize) {
    let len = bytes.len();
    (Box::into_raw(bytes.into_boxed_slice()).cast::<u8>(), len)
}

const EMPTY_IMAGE: PhotosRawImage = PhotosRawImage { pixels: std::ptr::null_mut(), width: 0, height: 0, stride: 0 };

/// # Safety
/// As `photos_raw.h` says: `head` is null or `len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_raw_is_raw(head: *const u8, len: usize) -> i32 {
    // SAFETY: the caller's contract.
    let head = unsafe { slice_of(head, len) };
    i32::from(catch_unwind(|| crate::is_raw(head)).unwrap_or(false))
}

/// # Safety
/// `path` is null or a NUL-terminated string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_raw_is_raw_file(path: *const c_char) -> i32 {
    // SAFETY: the caller's contract.
    let Ok(path) = (unsafe { path_of(path) }) else { return 0 };
    i32::from(catch_unwind(|| crate::is_raw_file(path).unwrap_or(false)).unwrap_or(false))
}

/// # Safety
/// As `photos_raw.h` says.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_raw_info_file(path: *const c_char, out: *mut PhotosRawInfo, error: *mut c_char, error_size: usize) -> i32 {
    // SAFETY: the caller's contract, pointers checked before use.
    unsafe {
        guarded(error, error_size, || {
            if out.is_null() {
                return Err(Error::Argument("no place for the result".into()));
            }
            let info = crate::info_file(path_of(path)?)?;
            out.write(c_info(&info));
            Ok(())
        })
    }
}

/// # Safety
/// As `photos_raw.h` says.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_raw_preview_file(path: *const c_char, jpeg: *mut *mut u8, jpeg_len: *mut usize, error: *mut c_char, error_size: usize) -> i32 {
    // SAFETY: the caller's contract.
    unsafe { photos_raw_preview_file_ex(path, 0, jpeg, jpeg_len, std::ptr::null_mut(), error, error_size) }
}

/// # Safety
/// As `photos_raw.h` says.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_raw_preview_file_ex(
    path: *const c_char,
    min_edge: u32,
    jpeg: *mut *mut u8,
    jpeg_len: *mut usize,
    orientation: *mut i32,
    error: *mut c_char,
    error_size: usize,
) -> i32 {
    // SAFETY: the caller's contract, pointers checked before use.
    unsafe {
        if !jpeg.is_null() {
            jpeg.write(std::ptr::null_mut());
        }
        if !jpeg_len.is_null() {
            jpeg_len.write(0);
        }
        guarded(error, error_size, || {
            if jpeg.is_null() || jpeg_len.is_null() {
                return Err(Error::Argument("no place for the result".into()));
            }
            let preview = crate::preview_jpeg_file(path_of(path)?, min_edge)?;
            let (data, len) = c_bytes(preview.jpeg);
            jpeg.write(data);
            jpeg_len.write(len);
            if !orientation.is_null() {
                orientation.write(i32::from(preview.orientation));
            }
            Ok(())
        })
    }
}

/// Shared by the functions that hand out a picture.
///
/// # Safety
/// As `photos_raw.h` says for `out`, `error`.
unsafe fn image_out(out: *mut PhotosRawImage, error: *mut c_char, error_size: usize, work: impl FnOnce() -> Result<Rgba8>) -> i32 {
    // SAFETY: the caller's contract, pointers checked before use.
    unsafe {
        if !out.is_null() {
            out.write(EMPTY_IMAGE);
        }
        guarded(error, error_size, || {
            if out.is_null() {
                return Err(Error::Argument("no place for the result".into()));
            }
            out.write(c_image(work()?)?);
            Ok(())
        })
    }
}

/// # Safety
/// As `photos_raw.h` says.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_raw_preview_image_file(path: *const c_char, max_edge: u32, out: *mut PhotosRawImage, error: *mut c_char, error_size: usize) -> i32 {
    // SAFETY: the caller's contract.
    unsafe { image_out(out, error, error_size, || crate::preview_image_file(path_of(path)?, max_edge)) }
}

/// # Safety
/// As `photos_raw.h` says.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_raw_develop_file(path: *const c_char, max_edge: u32, out: *mut PhotosRawImage, error: *mut c_char, error_size: usize) -> i32 {
    // SAFETY: the caller's contract.
    unsafe { image_out(out, error, error_size, || crate::develop_file(path_of(path)?, max_edge)) }
}

/// # Safety
/// `image` is null or a `photos_raw_image` this library filled (or zeroed).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_raw_free_image(image: *mut PhotosRawImage) {
    if image.is_null() {
        return;
    }
    // SAFETY: the caller's contract: pixels is the boxed slice `c_image` made, of stride × height bytes.
    unsafe {
        let img = image.read();
        image.write(EMPTY_IMAGE);
        if !img.pixels.is_null() {
            let len = img.stride as usize * img.height as usize;
            drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(img.pixels, len)));
        }
    }
}

/// # Safety
/// `data` is null or a buffer this library handed out, with its length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_raw_free(data: *mut u8, len: usize) {
    if data.is_null() {
        return;
    }
    // SAFETY: the caller's contract: the boxed slice `c_bytes` made.
    unsafe { drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(data, len))) };
}

#[unsafe(no_mangle)]
pub extern "C" fn photos_heif_available() -> i32 {
    i32::from(crate::heif_available())
}

/// # Safety
/// `head` is null or `len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_heif_is(head: *const u8, len: usize) -> i32 {
    // SAFETY: the caller's contract.
    let head = unsafe { slice_of(head, len) };
    i32::from(catch_unwind(|| crate::is_heif(head)).unwrap_or(false))
}

/// # Safety
/// As `photos_raw.h` says.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_heif_info_file(path: *const c_char, out: *mut PhotosRawInfo, error: *mut c_char, error_size: usize) -> i32 {
    // SAFETY: the caller's contract, pointers checked before use.
    unsafe {
        guarded(error, error_size, || {
            if out.is_null() {
                return Err(Error::Argument("no place for the result".into()));
            }
            let info = crate::heif_info_file(path_of(path)?)?;
            out.write(c_info(&info));
            Ok(())
        })
    }
}

/// # Safety
/// As `photos_raw.h` says.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_heif_decode_file(path: *const c_char, max_edge: u32, out: *mut PhotosRawImage, error: *mut c_char, error_size: usize) -> i32 {
    // SAFETY: the caller's contract.
    unsafe { image_out(out, error, error_size, || crate::heif_decode_file(path_of(path)?, max_edge)) }
}

/// `photos_raw_set_threads`: 0 when the pool will have `threads` threads, -6 when `threads` is
/// less than 1 or the pool has started.
#[unsafe(no_mangle)]
pub extern "C" fn photos_raw_set_threads(threads: i32) -> i32 {
    let applied = usize::try_from(threads).is_ok_and(crate::set_threads);
    if applied { 0 } else { Error::Argument(String::new()).code() }
}

#[unsafe(no_mangle)]
pub extern "C" fn photos_raw_version() -> *const c_char {
    static VERSION: std::sync::OnceLock<std::ffi::CString> = std::sync::OnceLock::new();
    VERSION.get_or_init(|| std::ffi::CString::new(crate::version()).unwrap_or_default()).as_ptr()
}

/// A hidden hook for the tests of the interface itself: panics inside the guard (code -5 comes
/// back), or reports what the guard does with an ordinary error.
///
/// # Safety
/// `error` is null or `error_size` writable bytes.
#[doc(hidden)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn photos_raw_selftest(what: i32, error: *mut c_char, error_size: usize) -> i32 {
    // SAFETY: the caller's contract.
    unsafe {
        guarded(error, error_size, || match what {
            1 => panic!("selftest panic"),
            2 => Err(Error::Unsupported("selftest: unsupported".into())),
            // a panic on a pool thread reaches the caller through rayon
            3 => {
                crate::init_pool();
                rayon::scope(|s| s.spawn(|_| panic!("selftest panic on a pool thread")));
                Ok(())
            }
            _ => Ok(()),
        })
    }
}
