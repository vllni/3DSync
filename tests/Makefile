# Host test build.  These tests run on the machine doing the building, not on a
# 3DS: everything they cover is deliberately free of libctru, libcurl and
# libsmb2 so it can be exercised without hardware or an emulator.
#
#   make          build and run
#   make run      the same
#   make F=engine run only tests whose "suite.name" contains "engine"

CXX ?= g++
CXXFLAGS ?= -std=gnu++11 -Wall -Wextra -O1 -g
# As in the app's Makefile: the INI reader is compiled into the sealing tests.
CXXFLAGS += -DINI_MAX_LINE=2048
BUILD := build
BIN := $(BUILD)/run-tests

# Units under test, compiled from the app sources.
UNITS := \
	../source/utils/debug.cpp \
	../source/utils/json.cpp \
	../source/utils/pathutil.cpp \
	../source/utils/timeutil.cpp \
	../source/utils/urlutil.cpp \
	../source/utils/xmlutil.cpp \
	../source/utils/fsutil.cpp \
	../source/modules/manifest.cpp \
	../source/modules/remoteparse.cpp \
	../source/sync/extfilter.cpp \
	../source/sync/remoteconfig.cpp \
	../source/sync/syncengine.cpp \
	../source/ui/canvas.cpp \
	../source/ui/fontdata.cpp \
	../source/ui/logbuffer.cpp \
	../source/ui/screens.cpp \
	../source/ui/touchinput.cpp

# The screens drawn to PNG, for looking at a layout without a console.
UI_UNITS := ../source/ui/canvas.cpp ../source/ui/fontdata.cpp \
	../source/ui/logbuffer.cpp ../source/ui/screens.cpp

TEST_SRC := framework.cpp mocks.cpp main.cpp \
	test_debug.cpp test_engine.cpp test_extfilter.cpp test_json.cpp test_manifest.cpp test_nway.cpp \
	test_remoteconfig.cpp \
	test_path.cpp \
	test_remoteparse.cpp test_time.cpp test_url.cpp test_xml.cpp \
	test_ui.cpp

# The hash and sealing tests need mbedtls, which is what the app uses on the
# console too.
# Without it the rest still runs, and the runner says what was left out.
MBEDTLS_PROBE := $(shell printf '#include <mbedtls/sha256.h>\nint main(){return 0;}\n' > /tmp/3dsync-mbedtls-probe.cpp 2>/dev/null && $(CXX) /tmp/3dsync-mbedtls-probe.cpp -lmbedcrypto -o /tmp/3dsync-mbedtls-probe 2>/dev/null && echo yes)

ifeq ($(MBEDTLS_PROBE),yes)
UNITS += ../source/utils/hash.cpp ../source/utils/secrets.cpp ../source/utils/configseal.cpp \
	../source/libs/inih/INIReader/INIReader.cpp ../source/libs/inih/ini.c
TEST_SRC += test_hash.cpp test_secrets.cpp
CXXFLAGS += -DHAVE_MBEDTLS
LDLIBS += -lmbedcrypto
endif

SRC := $(UNITS) $(TEST_SRC)

.PHONY: all run clean preview
all: run

run: $(BIN)
	@$(BIN) $(F)

$(BIN): $(SRC) framework.h mocks.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDLIBS)

preview: $(BUILD)/preview-ui
	@mkdir -p $(BUILD)/preview
	@$(BUILD)/preview-ui

$(BUILD)/preview-ui: preview.cpp $(UI_UNITS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ preview.cpp $(UI_UNITS)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
