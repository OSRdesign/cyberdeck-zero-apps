# SPDX-License-Identifier: MIT
#
# Host build of the Cassette DSP tests (meter, soft gain, DC blocker, high-pass, limiter, processing sink; no LVGL,
# no sound hardware, synthetic signals only):
#   make -C tests/dsp BUILD=<fresh dir> -j3 test
# Run it through tests/dsp/run.sh (a WSL native-filesystem copy). SAN=address,undefined or SAN=thread adds sanitizers.
APP   ?= ../..
BUILD ?= build
SRCS  := $(wildcard $(APP)/src/core/dsp/*.cpp)
TESTS := $(wildcard $(APP)/tests/dsp/*.cpp)
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS) $(TESTS))
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
ifneq ($(SAN),)
CXXFLAGS += -fsanitize=$(SAN) -fno-sanitize-recover=all
LDFLAGS  += -fsanitize=$(SAN)
endif
CXXFLAGS += -I$(APP)/src/core -I$(APP)/src/core/dsp -I$(APP)/tests/dsp
LIBS := -lpthread

all: $(BUILD)/test-cassette-dsp

$(BUILD)/test-cassette-dsp: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

test: all
	$(BUILD)/test-cassette-dsp

-include $(OBJS:.o=.d)
.PHONY: all test
