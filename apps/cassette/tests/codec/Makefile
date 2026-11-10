# SPDX-License-Identifier: MIT
#
# Host build of the Cassette codec tests (no LVGL, no ALSA, no codec header or library needed to build):
#   make -C tests/codec BUILD=<fresh dir> -j3 test
# Run it through tests/codec/run.sh (a WSL native-filesystem copy, plus the ABI check and the real-library runs).
# SAN=address or SAN=undefined adds a sanitizer.
APP   ?= ../..
BUILD ?= build
SRCS  := $(wildcard $(APP)/src/core/codec/*.cpp) \
         $(APP)/src/core/alsa/alsa_api.cpp $(APP)/src/core/alsa/card.cpp $(APP)/src/core/alsa/open_policy.cpp \
         $(APP)/src/core/fileops.cpp $(APP)/src/core/wav.cpp $(APP)/src/core/journal.cpp \
         $(wildcard $(APP)/src/core/takes/*.cpp)
TESTS := $(filter-out %/measure_main.cpp %/check_codec_abi.cpp,$(wildcard $(APP)/tests/codec/*.cpp))
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS) $(TESTS))
MOBJS := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS) $(APP)/tests/codec/measure_main.cpp)
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
ifneq ($(SAN),)
CXXFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
LDFLAGS  += -fsanitize=$(SAN)
endif
CXXFLAGS += -I$(APP)/src/core -I$(APP)/src/core/codec -I$(APP)/tests/codec
LIBS := -ldl -lpthread

all: $(BUILD)/test-codec $(BUILD)/cassette-codec-measure

$(BUILD)/test-codec: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/cassette-codec-measure: $(MOBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

test: all
	$(BUILD)/test-codec

-include $(OBJS:.o=.d) $(MOBJS:.o=.d)
.PHONY: all test
