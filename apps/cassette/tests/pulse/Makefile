# SPDX-License-Identifier: MIT
#
# Host build of the Cassette sound-server tests (src/core/pulse on the fake server; no LVGL, no pulseaudio header or
# library needed to build), the real-server smoke test and the probe CLI:
#   make -C tests/pulse BUILD=<fresh dir> -j2 test
#   make -C tests/pulse BUILD=<fresh dir> CXX=aarch64-linux-gnu-g++ cli     the CLI alone (arm64 for a board)
# Run it through tests/pulse/run.sh (a WSL native-filesystem copy, plus the ABI check and the real-library runs).
# SAN=thread or SAN=address,undefined adds sanitizers.
APP   ?= ../..
BUILD ?= build
SRCS  := $(wildcard $(APP)/src/core/pulse/*.cpp) $(wildcard $(APP)/src/core/alsa/*.cpp) $(APP)/src/core/ring.cpp
TESTS := $(filter-out %/real_pulse_test.cpp %/check_pulse_abi.cpp,$(wildcard $(APP)/tests/pulse/*.cpp))
LIBOBJS := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS))
OBJS  := $(LIBOBJS) $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(TESTS))
SOBJS := $(LIBOBJS) $(BUILD)/tests/pulse/real_pulse_test.o
COBJS := $(LIBOBJS) $(BUILD)/tools/pulse_capture_test.o
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
ifneq ($(SAN),)
CXXFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
LDFLAGS  += -fsanitize=$(SAN)
endif
# kept apart from CXXFLAGS so a CXXFLAGS given on the command line (the arm64 CLI) keeps them
INCS := -I$(APP)/src/core -I$(APP)/src/core/pulse -I$(APP)/src/core/alsa -I$(APP)/tests/pulse
LIBS := -ldl -lpthread

all: $(BUILD)/test-cassette-pulse $(BUILD)/real-pulse-test $(BUILD)/pulse-capture-test

cli: $(BUILD)/pulse-capture-test

$(BUILD)/test-cassette-pulse: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/real-pulse-test: $(SOBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/pulse-capture-test: $(COBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCS) -MMD -c -o $@ $<

test: all
	$(BUILD)/test-cassette-pulse

-include $(OBJS:.o=.d) $(SOBJS:.o=.d) $(COBJS:.o=.d)
.PHONY: all cli test
