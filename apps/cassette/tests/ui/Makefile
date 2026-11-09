# SPDX-License-Identifier: MIT
#
# Host build of the Cassette UI helper tests (the pure parts of src/ui: texts, waveform geometry, board-to-screen fit;
# no LVGL, no ALSA): make -C tests/ui BUILD=<fresh dir> -j3 test
# SAN=1 builds with AddressSanitizer and UndefinedBehaviorSanitizer. Run it on a WSL native-filesystem copy
# (tests/ui/run.sh does that). The LVGL screens are checked by the headless scenes (build.sh host).
APP   ?= ../..
BUILD ?= build
SRCS  := $(APP)/src/ui/ui_format.cpp $(APP)/src/ui/wave_geom.cpp $(APP)/src/ui/fit.cpp $(APP)/src/ui/load_anim.cpp
TESTS := $(wildcard $(APP)/tests/ui/*.cpp)
OBJS  := $(patsubst $(APP)/%.cpp,$(BUILD)/%.o,$(SRCS) $(TESTS))
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror
CXXFLAGS += -I$(APP)/src/ui -I$(APP)/tests/ui
ifeq ($(SAN),1)
CXXFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
LDFLAGS  += -fsanitize=address,undefined
endif

$(BUILD)/test-ui: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: $(APP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

test: $(BUILD)/test-ui
	$(BUILD)/test-ui

-include $(OBJS:.o=.d)
.PHONY: test
