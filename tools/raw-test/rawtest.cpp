// rawtest: the RAW library (raw/, photos_raw.h) from a shell, linked the way
// Photos links it. Tests the interface and measures it.
//
//   rawtest version
//   rawtest info FILE
//   rawtest preview FILE [MIN_EDGE] [OUT.jpg]      the embedded JPEG as stored
//   rawtest previewimage FILE [MAX_EDGE] [OUT.ppm] the embedded preview, decoded
//   rawtest develop FILE [MAX_EDGE] [OUT.ppm]      LightCraft's default rendering
//   rawtest heifinfo FILE
//   rawtest heif FILE [MAX_EDGE] [OUT.ppm]
//   rawtest selftest                               panics and errors at the boundary
//   rawtest threads FILE [MAX_EDGE] [N]            N developments at once
//
// MAX_EDGE 0 is full size. Times are wall-clock; the peak memory is the
// process's (getrusage).

#include "photos_raw.h"

#include <sys/resource.h>
#include <sys/time.h>

#ifdef __HAIKU__
#include <OS.h>
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" int photos_raw_selftest(int what, char* error, size_t error_size);


static double
Now()
{
	timeval tv;
	gettimeofday(&tv, nullptr);
	return tv.tv_sec + tv.tv_usec / 1e6;
}


#ifdef __HAIKU__
// Haiku's getrusage() has no peak memory: a thread adds up the memory behind
// the team's areas forty times a second and keeps the largest sum.
static std::atomic<long> sPeakKiB(0);
static std::atomic<bool> sSampling(true);


static void
SampleMemory()
{
	while (sSampling) {
		size_t total = 0;
		ssize_t cookie = 0;
		area_info info;
		while (get_next_area_info(B_CURRENT_TEAM, &cookie, &info) == B_OK)
			total += info.ram_size;
		long now = (long)(total / 1024);
		if (now > sPeakKiB)
			sPeakKiB = now;
		snooze(25000);
	}
}


static long
PeakKiB()
{
	return sPeakKiB;
}
#else
static long
PeakKiB()
{
	rusage usage;
	return getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : 0;
}
#endif


static bool
WritePpm(const char* path, const photos_raw_image& image)
{
	FILE* file = fopen(path, "wb");
	if (file == nullptr)
		return false;
	fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
	std::vector<uint8_t> row(image.width * 3);
	for (uint32_t y = 0; y < image.height; y++) {
		const uint8_t* in = image.pixels + (size_t)y * image.stride;
		for (uint32_t x = 0; x < image.width; x++) {
			row[x * 3] = in[x * 4 + 2];
			row[x * 3 + 1] = in[x * 4 + 1];
			row[x * 3 + 2] = in[x * 4];
		}
		fwrite(row.data(), 1, row.size(), file);
	}
	return fclose(file) == 0;
}


// Mean of the three channels and how many pixels are not black: a picture
// that decoded to nothing shows here without looking at it.
static void
Describe(const photos_raw_image& image)
{
	double sum[3] = {0, 0, 0};
	size_t lit = 0, opaque = 0;
	for (uint32_t y = 0; y < image.height; y++) {
		const uint8_t* in = image.pixels + (size_t)y * image.stride;
		for (uint32_t x = 0; x < image.width; x++) {
			sum[0] += in[x * 4 + 2];
			sum[1] += in[x * 4 + 1];
			sum[2] += in[x * 4];
			if (in[x * 4] > 8 || in[x * 4 + 1] > 8 || in[x * 4 + 2] > 8)
				lit++;
			if (in[x * 4 + 3] == 255)
				opaque++;
		}
	}
	double count = (double)image.width * image.height;
	printf("  mean rgb %.1f %.1f %.1f, %.1f%% not black, alpha %s\n",
		sum[0] / count, sum[1] / count, sum[2] / count, 100.0 * lit / count,
		opaque == (size_t)count ? "255" : "NOT 255");
}


static void
PrintInfo(const photos_raw_info& info)
{
	printf("size %u x %u, orientation %d\n", info.width, info.height, info.orientation);
	printf("taken %lld", (long long)info.taken);
	if (info.taken != 0) {
		time_t when = (time_t)info.taken;
		tm parts;
		gmtime_r(&when, &parts);
		char text[64];
		strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &parts);
		printf(" (%s camera time)", text);
	}
	printf("\ncamera \"%s\" \"%s\", lens \"%s\"\n", info.make, info.model, info.lens);
	printf("iso %g, exposure %g s, f/%g, %g mm\n", info.iso, info.exposure_time,
		info.f_number, info.focal_length);
	if (info.has_gps)
		printf("gps %.6f %.6f\n", info.latitude, info.longitude);
	else
		printf("gps none (%s)\n", std::isnan(info.latitude) ? "NaN" : "not NaN!");
}


static int
Fail(const char* what, int code, const char* error)
{
	fprintf(stderr, "%s: error %d: %s\n", what, code, error);
	return 1;
}


typedef int (*image_function)(const char*, uint32_t, photos_raw_image*, char*, size_t);


static int
Picture(const char* what, image_function function, int argc, char** argv)
{
	uint32_t maxEdge = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 10) : 0;
	photos_raw_image image;
	char error[256];
	double start = Now();
	int code = function(argv[2], maxEdge, &image, error, sizeof(error));
	double seconds = Now() - start;
	if (code != PHOTOS_RAW_OK)
		return Fail(what, code, error);
	printf("%s %s: %u x %u (stride %u) in %.3f s, peak %ld KiB\n", what, argv[2],
		image.width, image.height, image.stride, seconds, PeakKiB());
	Describe(image);
	if (argc > 4 && !WritePpm(argv[4], image))
		fprintf(stderr, "cannot write %s\n", argv[4]);
	photos_raw_free_image(&image);
	if (image.pixels != nullptr)
		return Fail(what, 0, "free_image left the pointer");
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: rawtest version|info|preview|previewimage|develop|"
			"heifinfo|heif|selftest|threads [FILE] [EDGE] [OUT]\n");
		return 2;
	}
	std::string command = argv[1];
	char error[256];
#ifdef __HAIKU__
	std::thread(SampleMemory).detach();
#endif
	// RAWTEST_THREADS=n: the pool's size through the interface (the library
	// reads PHOTOS_RAW_THREADS itself)
	if (getenv("RAWTEST_THREADS") != nullptr) {
		int code = photos_raw_set_threads(atoi(getenv("RAWTEST_THREADS")));
		printf("set_threads(%s): %d\n", getenv("RAWTEST_THREADS"), code);
	}
	if (command == "version") {
		printf("%s; heif %s\n", photos_raw_version(),
			photos_heif_available() ? "built in" : "not built");
		return 0;
	}
	if (command == "selftest") {
		int failures = 0;
		struct { int what; int expected; } cases[] = {
			{0, PHOTOS_RAW_OK}, {1, PHOTOS_RAW_INTERNAL},
			{2, PHOTOS_RAW_UNSUPPORTED}, {3, PHOTOS_RAW_INTERNAL}, {0, PHOTOS_RAW_OK},
		};
		for (auto& c : cases) {
			int code = photos_raw_selftest(c.what, error, sizeof(error));
			printf("selftest %d: %d \"%s\"%s\n", c.what, code, error,
				code == c.expected ? "" : "  <-- WRONG");
			failures += code != c.expected;
		}
		// arguments that are not there
		photos_raw_image image;
		int code = photos_raw_develop_file(nullptr, 0, &image, error, sizeof(error));
		printf("null path: %d \"%s\"\n", code, error);
		failures += code != PHOTOS_RAW_ARGUMENT;
		code = photos_raw_develop_file("/nonexistent/file.raf", 0, &image, error, sizeof(error));
		printf("missing file: %d \"%s\"\n", code, error);
		failures += code != PHOTOS_RAW_IO || image.pixels != nullptr;
		code = photos_raw_develop_file("/nonexistent/file.raf", 0, nullptr, nullptr, 0);
		failures += code != PHOTOS_RAW_ARGUMENT;
		char tiny[8];
		code = photos_raw_selftest(2, tiny, sizeof(tiny));
		printf("short buffer: \"%s\"\n", tiny);
		failures += strlen(tiny) != sizeof(tiny) - 1;
		photos_raw_free_image(nullptr);
		photos_raw_free(nullptr, 0);
		// the pool has started (the panic on a pool thread): too late to size it
		failures += photos_raw_set_threads(0) != PHOTOS_RAW_ARGUMENT;
		failures += photos_raw_set_threads(4) != PHOTOS_RAW_ARGUMENT;
		uint8_t garbage[4096];
		for (size_t i = 0; i < sizeof(garbage); i++)
			garbage[i] = (uint8_t)(i * 2654435761u >> 13);
		failures += photos_raw_is_raw(garbage, sizeof(garbage)) != 0;
		failures += photos_raw_is_raw(nullptr, 0) != 0;
		failures += photos_heif_is(garbage, sizeof(garbage)) != 0;
		// C++ exceptions still work beside Rust's unwinding
		try {
			throw std::string("thrown");
		} catch (const std::string& text) {
			printf("c++ exception: %s\n", text.c_str());
		}
		printf("%s\n", failures == 0 ? "selftest OK" : "selftest FAILED");
		return failures == 0 ? 0 : 1;
	}
	if (argc < 3) {
		fprintf(stderr, "%s: no file\n", argv[1]);
		return 2;
	}
	if (command == "info" || command == "heifinfo") {
		photos_raw_info info;
		double start = Now();
		int code = command == "info"
			? photos_raw_info_file(argv[2], &info, error, sizeof(error))
			: photos_heif_info_file(argv[2], &info, error, sizeof(error));
		if (code != PHOTOS_RAW_OK)
			return Fail(argv[1], code, error);
		printf("%s: is_raw_file %d, %.3f s\n", argv[2], photos_raw_is_raw_file(argv[2]),
			Now() - start);
		PrintInfo(info);
		return 0;
	}
	if (command == "preview") {
		uint32_t minEdge = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 10) : 0;
		uint8_t* jpeg = nullptr;
		size_t length = 0;
		int32_t orientation = 0;
		double start = Now();
		int code = photos_raw_preview_file_ex(argv[2], minEdge, &jpeg, &length, &orientation,
			error, sizeof(error));
		if (code != PHOTOS_RAW_OK)
			return Fail("preview", code, error);
		printf("preview %s: %zu bytes, orientation %d, %.3f s\n", argv[2], length, orientation,
			Now() - start);
		if (argc > 4) {
			FILE* file = fopen(argv[4], "wb");
			if (file != nullptr) {
				fwrite(jpeg, 1, length, file);
				fclose(file);
			}
		}
		photos_raw_free(jpeg, length);
		return 0;
	}
	if (command == "previewimage")
		return Picture("previewimage", photos_raw_preview_image_file, argc, argv);
	if (command == "develop")
		return Picture("develop", photos_raw_develop_file, argc, argv);
	if (command == "heif")
		return Picture("heif", photos_heif_decode_file, argc, argv);
	if (command == "threads") {
		uint32_t maxEdge = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 10) : 1024;
		int count = argc > 4 ? atoi(argv[4]) : 4;
		std::vector<std::thread> threads;
		std::vector<int> codes(count, 1);
		std::vector<uint64_t> sums(count, 0);
		double start = Now();
		for (int i = 0; i < count; i++) {
			threads.emplace_back([&, i] {
				photos_raw_image image;
				char text[256];
				codes[i] = photos_raw_develop_file(argv[2], maxEdge, &image, text, sizeof(text));
				if (codes[i] == PHOTOS_RAW_OK) {
					for (size_t b = 0; b < (size_t)image.stride * image.height; b++)
						sums[i] = sums[i] * 1099511628211ull + image.pixels[b];
				}
				photos_raw_free_image(&image);
			});
		}
		for (auto& thread : threads)
			thread.join();
		bool same = true;
		for (int i = 0; i < count; i++)
			same = same && codes[i] == PHOTOS_RAW_OK && sums[i] == sums[0];
		printf("threads %s: %d at once in %.3f s, results %s, peak %ld KiB\n", argv[2], count,
			Now() - start, same ? "identical" : "DIFFER OR FAILED", PeakKiB());
		return same ? 0 : 1;
	}
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
