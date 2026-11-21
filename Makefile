# Photos: native air/OS (Haiku) build.
#   make              builds $(BUILD)/Photos, photos_agent and photos_tool
#   make check        runs the portable tests (here, on Haiku)
#   make check-host   runs the portable tests on Linux (ASan/UBSan)
# tools/build-cross.sh runs this Makefile with an air/OS cross SDK from Linux
# and builds the RAW library (raw/, Rust) first.
.DEFAULT_GOAL := all
CXX ?= g++
BUILD ?= build-haiku
# '=' is relative to the (cross) sysroot.
PRIVATE_HEADERS ?= =/boot/system/develop/headers/private
CPPFLAGS += -Isrc -Ivendor -Iraw/include $(APP_CPPFLAGS)
HAIKU_CPPFLAGS = -I$(PRIVATE_HEADERS)/shared -I$(PRIVATE_HEADERS)/interface
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-multichar -Wno-unused-parameter \
	-Wno-missing-field-initializers -Wno-sign-compare
RC ?= rc
XRES ?= xres
MIMESET ?= mimeset

# core, sync, ptp: portable C++ (no Be API), tested on the build host.
# net: libcurl transport. platform: Be API without windows (decoding, Neural
# Kit, USB, node monitoring). ui: the application. agent: the background
# program that launches Photos when a camera arrives. tool: photos_tool, the
# library from a shell (tests, scripting).
CORE_SRC = $(wildcard src/core/*.cpp) $(wildcard src/sync/*.cpp) \
	$(wildcard src/ptp/*.cpp)
NET_SRC = $(wildcard src/net/*.cpp)
PLATFORM_SRC = $(wildcard src/platform/*.cpp)
UI_SRC = $(wildcard src/ui/*.cpp)
AGENT_SRC = $(wildcard src/agent/*.cpp)
TOOL_SRC = $(wildcard src/tool/*.cpp)
TEST_SRC = $(wildcard tests/*.cpp)

obj = $(patsubst %.cpp,$(BUILD)/%.o,$(1))
CORE_OBJ = $(call obj,$(CORE_SRC))
NET_OBJ = $(call obj,$(NET_SRC))
PLATFORM_OBJ = $(call obj,$(PLATFORM_SRC))
UI_OBJ = $(call obj,$(UI_SRC))
AGENT_OBJ = $(call obj,$(AGENT_SRC))
TOOL_OBJ = $(call obj,$(TOOL_SRC))
TEST_OBJ = $(call obj,$(TEST_SRC))

# The RAW decoder (LightCraft's crates behind a C interface) is a static
# library made by tools/build-raw.sh; without it RAW files show their
# embedded previews only.
RAW_LIB ?= $(BUILD)/libphotos_raw.a
ifneq ($(wildcard $(RAW_LIB)),)
CPPFLAGS += -DPHOTOS_HAVE_RAW=1
RAW_LINK = $(RAW_LIB) $(RAW_NATIVE_LIBS)
endif

SQLITE_LIBS ?= -lsqlite3
CORE_LIBS = $(SQLITE_LIBS) -lpthread
LIBS = -lbe -ltracker -ltranslation -ldevice -lneural -llocalestub -lcurl \
	-lsqlite3 -ljpeg -lwebp -lnetwork

.PHONY: all check check-host integration-host clean

all: $(BUILD)/Photos $(BUILD)/photos_agent $(BUILD)/photos_tool

$(BUILD)/Photos.rsrc: resources/Photos.rdef $(wildcard resources/*.hvif)
	mkdir -p $(BUILD)
	$(RC) -I resources -o $@ resources/Photos.rdef

$(BUILD)/photos_agent.rsrc: resources/photos_agent.rdef
	mkdir -p $(BUILD)
	$(RC) -I resources -o $@ resources/photos_agent.rdef

$(BUILD)/Photos: $(UI_OBJ) $(PLATFORM_OBJ) $(CORE_OBJ) $(NET_OBJ) \
		$(BUILD)/Photos.rsrc $(wildcard $(RAW_LIB))
	$(CXX) $(APP_LDFLAGS) -o $@.new $(UI_OBJ) $(PLATFORM_OBJ) $(CORE_OBJ) \
		$(NET_OBJ) $(RAW_LINK) $(LIBS)
	$(XRES) -o $@.new $(BUILD)/Photos.rsrc
	$(MIMESET) -f $@.new
	mv $@.new $@

# The camera layer by itself (PTP/MTP over USB, volumes with a DCIM folder):
# all the agent and tools/ptp-test/ptpls need of the application.
CAMERA_OBJ = $(BUILD)/src/platform/CameraSource.o \
	$(BUILD)/src/platform/UsbPtpTransport.o $(BUILD)/src/ptp/Ptp.o

$(BUILD)/photos_agent: $(AGENT_OBJ) $(CAMERA_OBJ) $(BUILD)/photos_agent.rsrc
	$(CXX) $(APP_LDFLAGS) -o $@.new $(AGENT_OBJ) $(CAMERA_OBJ) -lbe -ldevice
	$(XRES) -o $@.new $(BUILD)/photos_agent.rsrc
	$(MIMESET) -f $@.new
	mv $@.new $@

$(BUILD)/photos_tool: $(TOOL_OBJ) $(PLATFORM_OBJ) $(CORE_OBJ) $(NET_OBJ) \
		$(wildcard $(RAW_LIB))
	$(CXX) $(APP_LDFLAGS) -o $@ $(TOOL_OBJ) $(PLATFORM_OBJ) $(CORE_OBJ) \
		$(NET_OBJ) $(RAW_LINK) $(LIBS)

# ptpls: cameras from a shell, the test tool of the camera layer
# (docs/DEVICES.md). Not part of "all": tools/build-cross.sh ARCH build-ARCH/ptpls
$(BUILD)/ptpls: $(BUILD)/tools/ptp-test/ptpls.o $(CAMERA_OBJ)
	$(CXX) $(APP_LDFLAGS) -o $@ $^ -lbe -ldevice

# A stand-in for Photos (its signature, a log of the camera messages it
# gets) for testing the agent by itself.
$(BUILD)/fake_photos: $(BUILD)/tools/ptp-test/fake-photos.o $(BUILD)/Photos.rsrc
	$(CXX) $(APP_LDFLAGS) -o $@ $(BUILD)/tools/ptp-test/fake-photos.o -lbe
	$(XRES) -o $@ $(BUILD)/Photos.rsrc

HAIKU_DIRS = src/ui src/platform src/agent src/tool tools/ptp-test
define haiku_rule
$(BUILD)/$(1)/%.o: $(1)/%.cpp
	mkdir -p $$(dir $$@)
	$$(CXX) $$(CPPFLAGS) $$(HAIKU_CPPFLAGS) $$(CXXFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach d,$(HAIKU_DIRS),$(eval $(call haiku_rule,$(d))))

$(BUILD)/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/photos_tests: $(TEST_OBJ) $(CORE_OBJ)
	$(CXX) -o $@ $(TEST_OBJ) $(CORE_OBJ) $(CORE_LIBS)

check: $(BUILD)/photos_tests
	$(BUILD)/photos_tests $(FILTER)

# The build host has SQLite's library but not its header: tools/host-deps.sh
# makes build-host/.deps with the header of the air/OS dependency prefix.
HOST_FLAGS = -O1 -g -fsanitize=address,undefined
check-host:
	bash tools/host-deps.sh
	$(MAKE) BUILD=build-host CXX=g++ CXXFLAGS="$(HOST_FLAGS)" \
		APP_CPPFLAGS="-Ibuild-host/.deps/include" \
		SQLITE_LIBS="build-host/.deps/lib/libsqlite3.so" \
		CORE_LIBS="build-host/.deps/lib/libsqlite3.so -lpthread -fsanitize=address,undefined" check

# End-to-end Immich sync against tools/fake-immich.py (running).
INTEGRATION_SRC = $(wildcard tests/integration/*.cpp)
$(BUILD)/photos_integration: $(call obj,$(INTEGRATION_SRC)) $(CORE_OBJ)
	$(CXX) -o $@ $^ $(CORE_LIBS)

integration-host:
	bash tools/host-deps.sh
	$(MAKE) BUILD=build-host CXX=g++ CXXFLAGS="$(HOST_FLAGS)" \
		APP_CPPFLAGS="-Ibuild-host/.deps/include" \
		CORE_LIBS="build-host/.deps/lib/libsqlite3.so -lpthread -fsanitize=address,undefined" \
		build-host/photos_integration
	build-host/photos_integration $(ONLY)

clean:
	rm -rf build-haiku build-host build-x86_64 build-arm64

-include $(CORE_OBJ:.o=.d) $(NET_OBJ:.o=.d) $(PLATFORM_OBJ:.o=.d) \
	$(UI_OBJ:.o=.d) $(AGENT_OBJ:.o=.d) $(TOOL_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
