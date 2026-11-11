# SPDX-License-Identifier: MIT
#
# Host build of the Cassette playback tests (task T5: player, scan, output choice, countdown, deck). No LVGL, no libasound
# headers, no sound hardware; the ALSA side runs on the fake from tests/alsa.
#   make -C tests/playback BUILD=<fresh dir> -j3 test
# Run it through tests/playback/run.sh (a WSL native-filesystem copy). SAN=thread or SAN=address,undefined adds sanitizers.
APP   ?= ../..
BUILD ?= build
SRCS  := $(APP)/src/core/wav.cpp $(APP)/src/core/ring.cpp $(APP)/src/core/fileops.cpp $(APP)/src/core/alsa/alsa_api.cpp \
         $(APP)/src/core/alsa/card.cpp $(wildcard $(APP)/src/core/playback/*.cpp) $(APP)/tests/alsa/fake_alsa.cpp \
         $(wildcard $(APP)/src/core/pulse/*.cpp) $(APP)/tests/pulse/fake_pulse.cpp $(APP)/src/core/alsa/capture.cpp \
         $(APP)/src/core/alsa/open_policy.cpp \
         $(patsubst %,$(APP)/src/core/codec/%.cpp,codec_libs stream_scan ogg_opus decoder flac_decoder opus_decoder \
                                                   decoded_source encoder byte_sink)
TESTS := $(wildcard $(APP)/tests/playback/*.cpp)
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS) $(TESTS))
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
ifneq ($(SAN),)
CXXFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
LDFLAGS  += -fsanitize=$(SAN)
endif
CXXFLAGS += -I$(APP)/src/core -I$(APP)/src/core/alsa -I$(APP)/src/core/playback -I$(APP)/src/core/pulse -I$(APP)/tests/pulse -I$(APP)/tests/alsa -I$(APP)/tests/playback \
            -I$(APP)/tests
LIBS := -ldl -lpthread

all: $(BUILD)/test-cassette-playback

$(BUILD)/test-cassette-playback: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

test: all
	$(BUILD)/test-cassette-playback $(ARGS)

-include $(OBJS:.o=.d)
.PHONY: all test
