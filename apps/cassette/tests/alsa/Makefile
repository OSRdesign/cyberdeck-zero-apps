# SPDX-License-Identifier: MIT
#
# Host build of the Cassette ALSA-layer tests and the cassette-probe CLI (no LVGL, no libasound headers, no sound hardware):
#   make -C tests/alsa BUILD=<fresh dir> -j3 test
# Run it through tests/alsa/run.sh (a WSL native-filesystem copy). SAN=thread or SAN=address adds a sanitizer.
APP   ?= ../..
BUILD ?= build
SRCS  := $(wildcard $(APP)/src/core/alsa/*.cpp)
TESTS := $(wildcard $(APP)/tests/alsa/*.cpp)
PROBE := $(APP)/src/core/alsa/tools/probe_main.cpp
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS) $(TESTS))
POBJS := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS)) $(BUILD)/probe_fake.o $(BUILD)/tests/alsa/fake_alsa.o
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
ifneq ($(SAN),)
CXXFLAGS += -fsanitize=$(SAN)
LDFLAGS  += -fsanitize=$(SAN)
endif
CXXFLAGS += -I$(APP)/src/core/alsa -I$(APP)/tests/alsa
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

-include $(OBJS:.o=.d)
.PHONY: all test
