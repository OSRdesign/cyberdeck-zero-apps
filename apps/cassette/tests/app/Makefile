# SPDX-License-Identifier: MIT
#
# Host build of the Cassette app tests (task T6a: the controller end to end on fakes). No LVGL, no libasound headers, no sound
# hardware; the sound system is the fake from tests/alsa, the playback sink the fake from tests/playback.
#   make -C tests/app BUILD=<fresh dir> -j3 test
# Run it through tests/app/run.sh (a WSL native-filesystem copy). SAN=thread or SAN=address,undefined adds sanitizers.
APP   ?= ../..
BUILD ?= build
SRCS  := $(wildcard $(APP)/src/core/*.cpp) $(wildcard $(APP)/src/core/dsp/*.cpp) $(wildcard $(APP)/src/core/takes/*.cpp) \
         $(wildcard $(APP)/src/core/edit/*.cpp) $(wildcard $(APP)/src/core/playback/*.cpp) \
         $(wildcard $(APP)/src/core/app/*.cpp) $(wildcard $(APP)/src/core/codec/*.cpp) \
         $(APP)/src/core/alsa/alsa_api.cpp $(APP)/src/core/alsa/card.cpp $(APP)/src/core/alsa/capture.cpp \
         $(APP)/src/core/alsa/mixer.cpp $(APP)/src/core/alsa/open_policy.cpp $(APP)/tests/alsa/fake_alsa.cpp
TESTS := $(wildcard $(APP)/tests/app/*.cpp)
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS) $(TESTS))
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
ifneq ($(SAN),)
CXXFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
LDFLAGS  += -fsanitize=$(SAN)
endif
CXXFLAGS += -I$(APP)/src/core -I$(APP)/src/core/alsa -I$(APP)/src/core/dsp -I$(APP)/src/core/takes -I$(APP)/src/core/edit \
            -I$(APP)/src/core/playback -I$(APP)/src/core/app -I$(APP)/tests/alsa -I$(APP)/tests/playback -I$(APP)/tests/app
LIBS := -ldl -lpthread

all: $(BUILD)/test-cassette-app

$(BUILD)/test-cassette-app: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

test: all
	$(BUILD)/test-cassette-app $(ARGS)

-include $(OBJS:.o=.d)
.PHONY: all test
