/*
 * photos_raw.h: LightCraft's RAW decoder and default rendering (and its
 * pure-Rust HEIF decoder) behind a C interface. Implemented by the Rust
 * crate in raw/ (libphotos_raw.a, tools/build-raw.sh); docs/RAW.md.
 *
 * Every function may be called from several threads at once. Functions that
 * return an int return PHOTOS_RAW_OK (0) or a negative PHOTOS_RAW_* code and,
 * when `error` is not NULL, put a message for people into it (UTF-8, always
 * NUL-terminated, cut to `error_size`). Nothing here aborts the program on a
 * damaged file: a panic inside the decoder comes back as PHOTOS_RAW_INTERNAL.
 *
 * The work runs on a thread pool shared by all calls (one thread per CPU;
 * PHOTOS_RAW_THREADS=n in the environment, read at the first call, sets its
 * size).
 */
#ifndef PHOTOS_RAW_H
#define PHOTOS_RAW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	PHOTOS_RAW_OK = 0,
	PHOTOS_RAW_IO = -1,				/* the file could not be read */
	PHOTOS_RAW_UNSUPPORTED = -2,	/* a RAW file, but a camera or variant the decoder does not handle
									   (its embedded preview may still be there) */
	PHOTOS_RAW_DAMAGED = -3,		/* recognised, but the data is corrupt or cut short */
	PHOTOS_RAW_NOT_RAW = -4,		/* not a RAW (or HEIF) file at all */
	PHOTOS_RAW_INTERNAL = -5,		/* the decoder failed in a way it should not (a panic); the file is suspect */
	PHOTOS_RAW_ARGUMENT = -6,		/* a NULL or otherwise unusable argument */
	PHOTOS_RAW_NO_PREVIEW = -7,		/* the file has no embedded preview */
	PHOTOS_RAW_NOT_BUILT = -8		/* this build has no decoder for the format (HEIF left out) */
};

typedef struct photos_raw_info {
	uint32_t width, height;		/* developed size, after orientation */
	int32_t orientation;		/* EXIF orientation (1..8) of the file; develop has applied it: informational */
	int64_t taken;				/* capture time, seconds since the epoch as local wall time (no zone), 0 unknown */
	char make[64], model[64], lens[96];
	float iso, exposure_time, f_number, focal_length;	/* 0 unknown; seconds, mm */
	double latitude, longitude;	/* NaN unknown */
	int32_t has_gps;
} photos_raw_info;

typedef struct photos_raw_image {
	uint8_t* pixels;			/* BGRA, 8 bits, alpha 255: a Haiku B_RGB32 bitmap's layout; sRGB */
	uint32_t width, height, stride;	/* stride in bytes (width * 4) */
} photos_raw_image;

/* 1 if the bytes begin a RAW container LightCraft knows (DNG, CR2, CR3, CRW,
   NEF, NRW, ARW, RAF, RW2, PEF, ORF, ...), else 0. Give it the first 64 KiB
   or the whole file: a TIFF-based file whose directory lies beyond what was
   given is not recognised (photos_raw_is_raw_file reads what it needs). */
int photos_raw_is_raw(const uint8_t* head, size_t len);
int photos_raw_is_raw_file(const char* path);

/* Size, orientation and EXIF facts, without decoding the picture. Works for
   files whose pixels the decoder cannot read as well (the size is then the
   embedded preview's). */
int photos_raw_info_file(const char* path, photos_raw_info* out, char* error, size_t error_size);

/* The largest embedded preview as a JPEG file; *jpeg is freed with
   photos_raw_free(*jpeg, *jpeg_len). The camera's JPEG is returned as it is
   stored: use photos_raw_preview_file_ex to learn how to turn it upright. */
int photos_raw_preview_file(const char* path, uint8_t** jpeg, size_t* jpeg_len,
	char* error, size_t error_size);

/* As above, with two refinements. min_edge > 0 asks for the smallest
   embedded JPEG whose longest edge is at least min_edge (a camera stores
   several; a small one decodes much faster), else the largest.
   *orientation (may be NULL) is the EXIF orientation (1..8) the decoded JPEG
   must be given to stand upright: the JPEG's own when it has one, else the
   RAW file's. A preview that is not stored as a JPEG (JPEG XL in DNG 1.7,
   uncompressed RGB) is converted to one, upright (*orientation is then 1). */
int photos_raw_preview_file_ex(const char* path, uint32_t min_edge, uint8_t** jpeg,
	size_t* jpeg_len, int32_t* orientation, char* error, size_t error_size);

/* The embedded preview decoded: upright, sRGB, the longest edge at most
   max_edge (0: as stored). For thumbnails this is the fast path: no RAW data
   is touched. Free with photos_raw_free_image. */
int photos_raw_preview_image_file(const char* path, uint32_t max_edge, photos_raw_image* out,
	char* error, size_t error_size);

/* Develops the RAW data with LightCraft's default rendering (what LightCraft
   shows for the file with no edits: the camera's look fitted from its own
   JPEG or the DNG profile, as-shot white balance, highlight reconstruction,
   default sharpening and colour noise reduction, embedded lens corrections),
   sRGB, upright. max_edge 0 = full size, else the longest edge is at most
   max_edge (smaller sizes bin the sensor data and are much faster; the
   result may be up to a quarter smaller than max_edge, see docs/RAW.md).
   Free with photos_raw_free_image. */
int photos_raw_develop_file(const char* path, uint32_t max_edge, photos_raw_image* out,
	char* error, size_t error_size);

void photos_raw_free_image(photos_raw_image* image);	/* NULL and empty images are fine; zeroes *image */
void photos_raw_free(uint8_t* data, size_t len);		/* NULL is fine */

/* HEIF / HEIC (iPhone pictures), decoded by LightCraft's pure-Rust decoder.
   photos_heif_available() is 0 when the library was built without it; the
   other functions then return PHOTOS_RAW_NOT_BUILT. */
int photos_heif_available(void);
int photos_heif_is(const uint8_t* head, size_t len);	/* 1 if a HEIF/HEIC file (first 64 bytes are enough) */
int photos_heif_info_file(const char* path, photos_raw_info* out, char* error, size_t error_size);
/* Upright, sRGB; max_edge as for photos_raw_develop_file (0 = full size). */
int photos_heif_decode_file(const char* path, uint32_t max_edge, photos_raw_image* out,
	char* error, size_t error_size);

/* "photos-raw 0.1.0 (lightcraft 8fb1f41f, heif)": a static string. */
const char* photos_raw_version(void);

#ifdef __cplusplus
}
#endif

#endif	/* PHOTOS_RAW_H */
