# SPDX-License-Identifier: MIT
#
# Host build of the Cassette ALSA-layer tests and the cassette-probe CLI (no LVGL, no libasound headers, no sound hardware;
# the probe also carries the sound-server report, src/core/pulse, and the test build the fake server for --server --fake):
#   make -C tests/alsa BUILD=<fresh dir> -j3 test
# Run it through tests/alsa/run.sh (a WSL native-filesystem copy). SAN=thread or SAN=address adds a sanitizer.
APP   ?= ../..
BUILD ?= build
SRCS  := $(wildcard $(APP)/src/core/alsa/*.cpp)
TESTS := $(wildcard $(APP)/tests/alsa/*.cpp)
PROBE := $(APP)/src/core/alsa/tools/probe_main.cpp
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS) $(TESTS))
PSRCS := $(wildcard $(APP)/src/core/pulse/*.cpp) $(APP)/src/core/ring.cpp
POBJS := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS) $(PSRCS)) $(BUILD)/probe_fake.o $(BUILD)/tests/alsa/fake_alsa.o \
         $(BUILD)/tests/pulse/fake_pulse.o
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
ifneq ($(SAN),)
CXXFLAGS += -fsanitize=$(SAN)
LDFLAGS  += -fsanitize=$(SAN)
endif
CXXFLAGS += -I$(APP)/src/core/alsa -I$(APP)/tests/alsa -I$(APP)/src/core -I$(APP)/src/core/pulse -I$(APP)/tests/pulse
LIBS := -ldl -lpthread

all: $(BUILD)/test-cassette-alsa $(BUILD)/cassette-probe

$(BUILD)/test-cassette-alsa: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

# the probe is built with the fake so it can run `--fake` without a sound card
$(BUILD)/cassette-probe: $(POBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/probe_fake.o: $(PROBE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DCASSETTE_PROBE_WITH_FAKE -MMD -c -o $@ $<

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<


test: all
	$(BUILD)/test-cassette-alsa
	$(BUILD)/cassette-probe --fake --seconds 0.3 --no-open > /dev/null
	$(BUILD)/cassette-probe --fake --seconds 0.3
	$(BUILD)/cassette-probe --fake --server
	$(BUILD)/cassette-probe --fake --card bt:70:bf:92:53:6b:8b
	! $(BUILD)/cassette-probe --fake --card bt:11:22:33:44:55:66 > /dev/null
	! $(BUILD)/cassette-probe --fake --card bt:nonsense 2> /dev/null
	! $(BUILD)/cassette-probe --fake --server --card hw:0 2> /dev/null

-include $(OBJS:.o=.d) $(POBJS:.o=.d)
.PHONY: all test
