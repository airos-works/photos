// Picture metadata: what a JPEG's EXIF block says, and the pixel size of the
// common containers. RAW and HEIF files are read by the RAW library instead.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace photos {

struct Metadata {
	int width = 0;				// stored size, before the orientation is applied
	int height = 0;
	int orientation = 1;		// EXIF 1..8
	int64_t taken = 0;			// local wall time in seconds (Util.h), 0 unknown
	bool hasZone = false;
	int zoneOffset = 0;			// seconds east of UTC of `taken`
	std::string make;
	std::string model;
	std::string lens;
	double iso = 0;
	double exposureTime = 0;	// seconds
	double fNumber = 0;
	double focalLength = 0;		// mm
	bool hasGps = false;
	double latitude = 0;
	double longitude = 0;

	// Size as shown: width and height swapped for the turned orientations.
	int DisplayWidth() const { return orientation >= 5 ? height : width; }
	int DisplayHeight() const { return orientation >= 5 ? width : height; }
};

// A TIFF structure (an EXIF block without its "Exif\0\0" prefix).
bool ParseExif(const uint8_t* data, size_t size, Metadata& metadata);
// A JPEG's frame size and EXIF. False when the file is not a readable JPEG.
bool ReadJpegMetadata(const std::string& path, Metadata& metadata);
bool ParseJpegMetadata(const uint8_t* data, size_t size, Metadata& metadata);
// The pixel size of a PNG, GIF, BMP or WebP file from its header.
bool ReadImageSize(const std::string& path, int& width, int& height);

// "1/250 s", "f/2.8", "ISO 400", "35 mm": for the info panel.
std::string FormatExposure(double seconds);
std::string FormatAperture(double fNumber);

}	// namespace photos
