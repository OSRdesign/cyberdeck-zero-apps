# SPDX-License-Identifier: MIT
#
# Host build of the Cassette binding tests (task T6c): the view model mapping, the action bridge and the worker on a fake
# controller port, then end to end on the real Controller with the demo mode's scripted sound system. No LVGL, no
# libasound, no sound hardware, nothing played or recorded.
#   make -C tests/bind BUILD=<fresh dir> -j3 test        (ARGS=--quick leaves out the real-time end-to-end runs)
# Run it through tests/bind/run.sh (a WSL native-filesystem copy). SAN=thread or SAN=address,undefined adds sanitizers.
APP   ?= ../..
BUILD ?= build
SRCS  := $(wildcard $(APP)/src/core/*.cpp) $(wildcard $(APP)/src/core/dsp/*.cpp) $(wildcard $(APP)/src/core/takes/*.cpp) \
         $(wildcard $(APP)/src/core/edit/*.cpp) $(wildcard $(APP)/src/core/playback/*.cpp) \
         $(wildcard $(APP)/src/core/app/*.cpp) \
         $(APP)/src/core/alsa/alsa_api.cpp $(APP)/src/core/alsa/card.cpp $(APP)/src/core/alsa/capture.cpp \
         $(APP)/src/core/alsa/mixer.cpp $(APP)/src/core/alsa/open_policy.cpp \
         $(APP)/src/bind/binder.cpp $(APP)/src/bind/view_map.cpp $(APP)/src/bind/worker.cpp \
         $(APP)/src/bind/controller_port.cpp $(APP)/src/demo/demo_alsa.cpp $(APP)/src/ui/ui_format.cpp
TESTS := $(wildcard $(APP)/tests/bind/*.cpp)
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS) $(TESTS))
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
ifneq ($(SAN),)
CXXFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
LDFLAGS  += -fsanitize=$(SAN)
endif
CXXFLAGS += -I$(APP)/src -I$(APP)/src/core -I$(APP)/tests/bind
LIBS := -lpthread

all: $(BUILD)/test-cassette-bind

$(BUILD)/test-cassette-bind: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

test: all
	$(BUILD)/test-cassette-bind $(ARGS)

-include $(OBJS:.o=.d)
.PHONY: all test
